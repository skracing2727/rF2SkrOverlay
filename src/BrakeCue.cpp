#include "BrakeCue.h"
#include "InternalsPlugin.hpp"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "RelativeTiming.h"
#include "Shortcuts.h"
#include "Log.h"
#include "LapDelta.h"
#include "Persist.h"
#include "Sound.h"
#include "StoreFile.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

BrakeCueStore g_brakeCue;

namespace
{
    // Tuning (threshold, spacing, lead time, beep tone/duration) lives in
    // g_brakeCueConfig (Config.h/.cpp) -- edit the INI and restart rather than
    // changing values here. Sample rate stays fixed: it's a WAV-synthesis
    // implementation detail, not something worth exposing.
    const int kBeepSampleRate = 44100;
}

void BrakeCueStore::Init()
{
    if (m_ready)
        return;

    InitializeCriticalSection(&m_lock);
    QueryPerformanceFrequency(&m_freq);

    // Build a short sine-wave beep as an in-memory WAV. PlaySound(SND_MEMORY)
    // needs a complete WAV image, not raw PCM, so RIFF/fmt/data headers are
    // written by hand here rather than requiring an external audio library or
    // a .wav file shipped alongside the DLL.
    // Config::Load() runs before this (see Plugin::Startup), so these reflect
    // the INI already.
    const int beepFrequencyHz = g_brakeCueConfig.beepFrequencyHz;
    // Clamped: this is user-editable in the INI, and a negative value would
    // wrap around to a huge size once cast to the unsigned byte count below
    // (new[] would then try to allocate ~4GB and throw, uncaught, straight
    // out of Startup() -- crashing the game on load rather than the overlay
    // alone). A large-but-still-positive value could also overflow the plain
    // int multiplication below before the clamp existed.
    int beepDurationMs = g_brakeCueConfig.beepDurationMs;
    if (beepDurationMs < 0) beepDurationMs = 0;
    if (beepDurationMs > 10000) beepDurationMs = 10000;
    const int sampleCount = (kBeepSampleRate * beepDurationMs) / 1000;
    const int fadeSamples = kBeepSampleRate / 200; // 5ms fade in/out, avoids a click

    const DWORD dataBytes = static_cast<DWORD>(sampleCount) * 2; // 16-bit mono
    const DWORD totalSize = 44 + dataBytes;

    m_wavBuffer = new unsigned char[totalSize];
    unsigned char* p = m_wavBuffer;

    auto writeU32 = [&](DWORD v) { memcpy(p, &v, 4); p += 4; };
    auto writeU16 = [&](WORD v) { memcpy(p, &v, 2); p += 2; };

    memcpy(p, "RIFF", 4); p += 4;
    writeU32(36 + dataBytes);
    memcpy(p, "WAVE", 4); p += 4;

    memcpy(p, "fmt ", 4); p += 4;
    writeU32(16);                      // fmt chunk size
    writeU16(1);                       // PCM
    writeU16(1);                       // mono
    writeU32(kBeepSampleRate);
    writeU32(kBeepSampleRate * 2);     // byte rate
    writeU16(2);                       // block align
    writeU16(16);                      // bits per sample

    memcpy(p, "data", 4); p += 4;
    writeU32(dataBytes);

    for (int i = 0; i < sampleCount; ++i)
    {
        double amp = 1.0;
        if (i < fadeSamples)
            amp = static_cast<double>(i) / fadeSamples;
        else if (i > sampleCount - fadeSamples)
            amp = static_cast<double>(sampleCount - i) / fadeSamples;

        const double t = static_cast<double>(i) / kBeepSampleRate;
        const double sample = amp * sin(2.0 * 3.14159265358979 * beepFrequencyHz * t);
        const short pcm = static_cast<short>(sample * 32000.0);
        memcpy(p, &pcm, 2); p += 2;
    }

    m_wavSize = totalSize;
    m_ready = true;
}

void BrakeCueStore::PlayBeep()
{
    if (!m_ready || !m_wavBuffer)
        return;
    // The Ctrl+Shift toggle kills the beep as well as the on-screen cue --
    // one switch for the whole feature, so hiding it does not leave the sim
    // still beeping at you from a widget you cannot see.
    if (!Shortcuts::Visible(Shortcuts::BrakeCue))
        return;
    // Handed to the sound thread: even SND_ASYNC PlaySound opens the wave
    // device before returning, and this is the game's telemetry callback.
    Sound::PlayMemory(m_wavBuffer);
}

double BrakeCueStore::LeadTimeSeconds() const
{
    // m_avgReactionSeconds is owned by this (the telemetry) thread, so no
    // lock is needed to read it here -- see the header.
    double lead = g_brakeCueConfig.leadTimeSec;
    if (g_brakeCueConfig.adaptiveLeadTimeEnabled && m_avgReactionSeconds > 0.0)
        lead = m_avgReactionSeconds;

    // Bounded by the same range a single measurement has to fall in to be
    // accepted at all. This matters for more than tidiness: a driver who
    // consistently brakes LATER than the reference point (rather than
    // reacting to the cue) measures a gap of "current lead + however late
    // they are" every time, so each adaptation would add that overshoot
    // again and march the cue earlier without limit. Both the per-sample
    // ceiling and this clamp stop that; the cue settles at the edge of the
    // plausible range instead of running away.
    if (lead < g_brakeCueConfig.reactionMinSeconds) lead = g_brakeCueConfig.reactionMinSeconds;
    if (lead > g_brakeCueConfig.reactionMaxSeconds) lead = g_brakeCueConfig.reactionMaxSeconds;
    return lead;
}

void BrakeCueStore::RecordReaction(double reactionSeconds)
{
    m_reactionHistory[m_reactionHistoryNext] = reactionSeconds;
    m_reactionHistoryNext = (m_reactionHistoryNext + 1) % kReactionHistorySize;
    if (m_reactionHistoryCount < kReactionHistorySize)
        ++m_reactionHistoryCount;

    double sum = 0.0;
    for (int i = 0; i < m_reactionHistoryCount; ++i)
        sum += m_reactionHistory[i];
    m_avgReactionSeconds = sum / m_reactionHistoryCount;

    EnterCriticalSection(&m_lock);
    m_state.avgReactionSeconds = m_avgReactionSeconds;
    m_state.latestReactionSeconds = reactionSeconds;
    m_state.sampleCount = m_reactionHistoryCount;
    m_state.leadTimeInUseSeconds = LeadTimeSeconds();
    LeaveCriticalSection(&m_lock);

    Log::Write("BrakeCue: reaction %.0fms (avg %.0fms over %d), beep lead time now %.0fms",
               reactionSeconds * 1000.0, m_avgReactionSeconds * 1000.0,
               m_reactionHistoryCount, LeadTimeSeconds() * 1000.0);
}

void BrakeCueStore::OnTelemetry(const TelemInfoV01& info)
{
    if (!m_ready)
        return;

    // Forward speed from local velocity (Z is the car's own forward axis).
    m_forwardSpeed = info.mLocalVel.z;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = 0.0;
    if (m_lastSyncTime.QuadPart != 0 && m_freq.QuadPart != 0)
    {
        dt = static_cast<double>(now.QuadPart - m_lastSyncTime.QuadPart) / static_cast<double>(m_freq.QuadPart);
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.5) dt = 0.5; // stalled scoring (pause/load) -- don't run away
    }

    double dist = m_lastKnownDist + m_forwardSpeed * dt;
    if (m_trackLength > 1.0)
    {
        while (dist >= m_trackLength) dist -= m_trackLength;
        while (dist < 0.0) dist += m_trackLength;
    }
    m_currentDist = dist;

    // --- Brake pedal profile of the current lap ---
    if (m_trackLength > kBrakeBinM)
    {
        const size_t bins = static_cast<size_t>(ceil(m_trackLength / kBrakeBinM));
        const size_t bin = static_cast<size_t>(dist / kBrakeBinM);
        const float brake = static_cast<float>(info.mUnfilteredBrake);
        EnterCriticalSection(&m_lock);
        if (m_recBrake.size() != bins)
            m_recBrake.assign(bins, 0.0f);
        if (bin < bins && brake > m_recBrake[bin])
            m_recBrake[bin] = brake;
        LeaveCriticalSection(&m_lock);
    }

    if (!g_brakeCueConfig.enabled)
        return;

    // --- Record braking points on the current lap ---
    const bool braking = info.mUnfilteredBrake > g_brakeCueConfig.brakeThreshold;
    if (braking && !m_wasBraking)
    {
        const bool tooClose = (m_recordingCount > 0) &&
            (fabs(dist - m_recording[m_recordingCount - 1]) < g_brakeCueConfig.minPointSpacingM);
        if (!tooClose && m_recordingCount < kMaxPoints)
        {
            m_recording[m_recordingCount] = dist;
            m_recordingEntrySpeed[m_recordingCount] = fabs(m_forwardSpeed);
            m_recordingMinSpeed[m_recordingCount] = fabs(m_forwardSpeed);
            ++m_recordingCount;
        }
    }
    m_wasBraking = braking;
    // The latest point's slowest speed so far -- runs on until the next
    // point starts, so it ends up at that corner's minimum.
    if (m_recordingCount > 0 && fabs(m_forwardSpeed) < m_recordingMinSpeed[m_recordingCount - 1])
        m_recordingMinSpeed[m_recordingCount - 1] = fabs(m_forwardSpeed);

    // --- Resolve the reaction time for the beep still waiting on a pedal ---
    // Edge-triggered on the pedal CROSSING the threshold, not merely being
    // above it: a beep is only armed while the pedal is below the threshold
    // (see below), so the first crossing after it is the reaction to it.
    const bool pedalPressed = info.mUnfilteredBrake > g_brakeCueConfig.reactionThreshold;
    if (m_pendingBeep && m_freq.QuadPart != 0)
    {
        const double sinceBeep = static_cast<double>(now.QuadPart - m_beepTime.QuadPart) / static_cast<double>(m_freq.QuadPart);
        if (pedalPressed && !m_wasPedalPressed)
        {
            m_pendingBeep = false;
            if (sinceBeep >= g_brakeCueConfig.reactionMinSeconds && sinceBeep <= g_brakeCueConfig.reactionMaxSeconds)
                RecordReaction(sinceBeep);
        }
        else if (sinceBeep > g_brakeCueConfig.reactionTimeoutSeconds)
        {
            // Cue went unanswered -- abandon it rather than eventually
            // pairing it with an unrelated braking event much later.
            m_pendingBeep = false;
        }
    }
    m_wasPedalPressed = pedalPressed;

    // --- Check against the reference (best) lap ---
    if (m_referenceCount > 0 && m_nextCueIndex < m_referenceCount)
    {
        // Current speed as the estimate of speed at the point itself -- a good
        // approximation since this is re-evaluated every telemetry update as
        // you get closer, so it keeps refining right up to the trigger.
        const double leadDistance = fabs(m_forwardSpeed) * LeadTimeSeconds();

        double target = m_reference[m_nextCueIndex] - leadDistance;
        if (target < 0.0) target += m_trackLength;

        // Fire once we reach or pass the target (handles frame-to-frame gaps
        // stepping slightly past it rather than landing exactly on it).
        if (dist >= target && dist < target + 10.0) // +10m guard against a lap-wrap false match
        {
            // Small stops (a lift, a dab) are passed over silently. Read
            // live, so the slider takes effect straight away.
            if (m_referenceDropMs[m_nextCueIndex] * 3.6 < g_brakeCueConfig.minSpeedDropKmh)
            {
                ++m_nextCueIndex;
                return;
            }
            PlayBeep();
            ++m_nextCueIndex;

            // Only arm a measurement if the pedal is currently free -- a cue
            // landing mid-braking (a second point in the same zone, a corner
            // taken on the brakes) has no pedal movement of its own to time.
            m_pendingBeep = !pedalPressed;
            m_beepTime = now;
        }
    }
}

void BrakeCueStore::OnScoring(const ScoringInfoV01& info)
{
    if (!m_ready)
        return;

    m_trackLength = info.mLapDist; // session-level: total track length

    if (info.mVehicle == nullptr)
        return;

    const VehicleScoringInfoV01* player = nullptr;
    for (int i = 0; i < info.mNumVehicles; ++i)
    {
        if (info.mVehicle[i].mIsPlayer)
        {
            player = &info.mVehicle[i];
            break;
        }
    }
    if (!player)
        return;

    // Resync extrapolated distance to the authoritative value.
    m_lastKnownDist = player->mLapDist;
    QueryPerformanceCounter(&m_lastSyncTime);

    // Latch the minimum mCountLapFlag seen during the lap: the flag is a
    // transient property of the lap IN PROGRESS that resets before being read
    // at lap completion. Latching the minimum catches the valid moment, even
    // if it later resets.
    if (player->mCountLapFlag < m_lapValidityLatch)
        m_lapValidityLatch = player->mCountLapFlag;

    // This car+track's saved brake profiles, once LapDelta has the key.
    {
        const std::string& key = g_lapDelta.CurrentKey();
        if (!key.empty() && key != m_savedKey && m_trackLength > kBrakeBinM)
            LoadSavedLaps(key, static_cast<size_t>(ceil(m_trackLength / kBrakeBinM)));
    }

    if (m_pendingSave)
    {
        if (player->mLastLapTime > 0.0 && player->mLastLapTime != m_pendingOldLapTime)
        {
            AddSavedLap(player->mLastLapTime, m_pendingBrake);
            m_lapTimeAtStart = player->mLastLapTime;
            m_pendingSave = false;
            m_pendingBrake.clear();
        }
        else if (info.mCurrentET - m_pendingSinceET > 5.0)
        {
            m_pendingSave = false;
            m_pendingBrake.clear();
        }
    }

    if (m_lastTotalLaps < 0)
    {
        m_lastTotalLaps = player->mTotalLaps;
        m_lapTimeAtStart = player->mLastLapTime;
        return;
    }

    // Lap just completed.
    if (player->mTotalLaps > m_lastTotalLaps)
    {
        m_lastTotalLaps = player->mTotalLaps;

        const bool validForTiming = (m_lapValidityLatch == 2);
        const bool isNewBest = validForTiming &&
            (m_referenceLapTime < 0.0 || player->mLastLapTime < m_referenceLapTime);

        // The brake profile goes with the best lap, then starts over. Only
        // when it covered the whole lap: a lap joined part way (left the
        // pits, came back from the monitor) leaves bins never driven at 0,
        // which is indistinguishable from "did not brake here". Without
        // braking points (beep off) the lap time is tracked here instead.
        // The profile goes into the saved best five (see the header) rather
        // than straight into the reference. The lap time can reach scoring a
        // tick or two after the lap count, so wait for it to change.
        std::vector<float> finished;
        EnterCriticalSection(&m_lock);
        if (validForTiming && m_lapFullyRecorded && !m_recBrake.empty())
            finished = m_recBrake;
        std::fill(m_recBrake.begin(), m_recBrake.end(), 0.0f);
        LeaveCriticalSection(&m_lock);
        if (!finished.empty())
        {
            if (player->mLastLapTime > 0.0 && player->mLastLapTime != m_lapTimeAtStart)
                AddSavedLap(player->mLastLapTime, finished);
            else
            {
                m_pendingSave = true;
                m_pendingOldLapTime = m_lapTimeAtStart;
                m_pendingSinceET = info.mCurrentET;
                m_pendingBrake.swap(finished);
            }
        }
        if (isNewBest && (m_recordingCount == 0 || !g_brakeCueConfig.enabled))
            m_referenceLapTime = player->mLastLapTime;
        m_lapTimeAtStart = player->mLastLapTime;
        m_lapFullyRecorded = true;

        if (isNewBest && m_recordingCount > 0)
        {
            memcpy(m_reference, m_recording, sizeof(double) * m_recordingCount);
            int bigStops = 0;
            for (int i = 0; i < m_recordingCount; ++i)
            {
                m_referenceDropMs[i] = m_recordingEntrySpeed[i] - m_recordingMinSpeed[i];
                if (m_referenceDropMs[i] * 3.6 >= g_brakeCueConfig.minSpeedDropKmh)
                    ++bigStops;
            }
            m_referenceCount = m_recordingCount;
            m_referenceLapTime = player->mLastLapTime;
            Log::Write("BrakeCue: new best lap %.3fs, %d braking points recorded, %d over the %.0f km/h drop",
                       m_referenceLapTime, m_referenceCount, bigStops, g_brakeCueConfig.minSpeedDropKmh);
            for (int i = 0; i < m_recordingCount; ++i)
                Log::Write("BrakeCue:   point %d at %.0fm, drop %.0f km/h", i + 1, m_reference[i],
                           m_referenceDropMs[i] * 3.6);
        }

        // Start recording fresh for the new lap either way.
        m_recordingCount = 0;
        m_wasBraking = false;
        m_nextCueIndex = 0;
        m_lapValidityLatch = 2; // reset for next lap
    }
}

void BrakeCueStore::OnExitRealtime()
{
    // Transient in-lap state only -- see the header for what deliberately
    // survives. m_lastTotalLaps going back to -1 makes the next scoring
    // sample re-anchor instead of reading the gap as a completed lap.
    m_lastKnownDist = 0.0;
    m_forwardSpeed = 0.0;
    m_lastSyncTime.QuadPart = 0;

    m_recordingCount = 0;
    m_wasBraking = false;
    m_nextCueIndex = 0;
    m_lastTotalLaps = -1;

    m_pendingBeep = false;
    m_wasPedalPressed = false;

    // The lap in progress is now a partial one -- see OnScoring.
    m_lapFullyRecorded = false;
    if (m_ready)
    {
        EnterCriticalSection(&m_lock);
        std::fill(m_recBrake.begin(), m_recBrake.end(), 0.0f);
        LeaveCriticalSection(&m_lock);
    }
}

void BrakeCueStore::Clear()
{
    OnExitRealtime();

    m_trackLength = 0.0;

    // The reference braking points and the reaction times measured against
    // them belong to the session/track being left, so both go here (unlike
    // OnExitRealtime above, which keeps them).
    m_referenceCount = 0;
    m_referenceLapTime = -1.0;

    // Callers flush first (see Plugin.cpp); reloaded on the next scoring tick.
    AcquireSRWLockExclusive(&m_saveLock);
    m_savedLaps.clear();
    m_savedKey.clear();
    m_savedDirty = false;
    ReleaseSRWLockExclusive(&m_saveLock);
    m_pendingSave = false;
    m_pendingBrake.clear();

    m_reactionHistoryCount = 0;
    m_reactionHistoryNext = 0;
    m_avgReactionSeconds = -1.0;

    if (!m_ready)
        return;
    EnterCriticalSection(&m_lock);
    m_state = State{};
    m_refBrake.clear();
    m_recBrake.clear();
    LeaveCriticalSection(&m_lock);
}

BrakeCueStore::State BrakeCueStore::Read()
{
    State copy;
    if (!m_ready) return copy;
    EnterCriticalSection(&m_lock);
    copy = m_state;
    LeaveCriticalSection(&m_lock);
    return copy;
}

bool BrakeCue::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    // This widget paints its own background panel, so the renderer's
    // per-string text plate would just double-darken over it. Restored on
    // every exit path by the scope guard, since Draw() returns from several
    // places.
    struct TextBgGuard
    {
        Renderer& r; bool prev;
        explicit TextBgGuard(Renderer& rr) : r(rr), prev(rr.TextBackgroundEnabled()) { r.SetTextBackground(false); }
        ~TextBgGuard() { r.SetTextBackground(prev); }
    } textBgGuard(renderer);

    const BrakeCueConfig& cfg = g_brakeCueConfig;
    if (!cfg.enabled || !cfg.showReactionStats)
        return false;

    const BrakeCueStore::State state = g_brakeCue.Read();
    // Normally nothing draws until a real reaction time has been measured --
    // an empty box would just be noise during actual driving. But that also
    // means the box never registers with Layout, so it could never be
    // dragged or resized before you had driven enough to populate it.
    // Layout mode bypasses the guard; the state's own -1/0 defaults print as
    // harmless placeholder numbers below.
    if (state.sampleCount <= 0 && !Layout::Preview())
        return false;

    // Same reference-resolution scaling as the other pixel-sized widgets.
    // cfg.scale is the layout-mode corner-handle multiplier. Folding it
    // in HERE rather than applying it to the drawn box means every pixel
    // value downstream -- text height, padding, gaps, corner radii --
    // scales with it in exact proportion, because they all already
    // derive from this one value.
    const float uiScale = static_cast<float>(screenHeight) / 2160.0f * cfg.reactionStatsScale;
    const float textHeightPx = cfg.reactionStatsTextHeightPx * uiScale;
    const float bgPaddingPx = cfg.reactionStatsBgPaddingPx * uiScale;
    const float bgCornerRadiusPx = cfg.reactionStatsBgCornerRadiusPx * uiScale;

    // Milliseconds throughout -- a reaction is a couple of hundred of them,
    // which reads far better than "0.24 s". Latest is shown alongside the
    // average since a single slow one is exactly what the driver wants to
    // notice, and the average alone hides it.
    char buf[96];
    sprintf_s(buf, "Brake React %.0fms avg | %.0fms last | cue %.0fms early",
              state.avgReactionSeconds * 1000.0,
              state.latestReactionSeconds * 1000.0,
              state.leadTimeInUseSeconds * 1000.0);

    const float centerX = screenWidth * cfg.reactionStatsCenterXFraction;
    // Below the STANDINGS table, not just below the gear display. The
    // configured fraction is only a floor: the standings table's height
    // varies with how many cars are in range, and it is drawn after this
    // widget, so a fixed fraction simply got painted over -- which is exactly
    // what was happening. Take whichever is lower.
    const float configuredY = screenHeight * cfg.reactionStatsCenterYFraction;
    const float belowStandingsY = RelativeTiming::BottomEdgeY(screenWidth, screenHeight)
                                + textHeightPx * 0.5f + bgPaddingPx * 2.0f;
    const float centerY = (belowStandingsY > configuredY) ? belowStandingsY : configuredY;
    const float textW = renderer.MeasureText(buf, textHeightPx);

    if (cfg.reactionStatsBgEnabled)
    {
        Layout::Register("Brake reaction", "BrakeCue",
                         "ReactionStatsCenterXFraction", "ReactionStatsCenterYFraction",
                         &g_brakeCueConfig.reactionStatsCenterXFraction,
                         &g_brakeCueConfig.reactionStatsCenterYFraction,
                         centerX, centerY, textW * 0.5f + bgPaddingPx, textHeightPx * 0.5f + bgPaddingPx,
                         &g_brakeCueConfig.reactionStatsScale, "ReactionStatsScale");

        renderer.AddFilledRoundedRect(centerX, centerY, textW * 0.5f + bgPaddingPx, textHeightPx * 0.5f + bgPaddingPx,
                                      bgCornerRadiusPx, 0.0f,
                                      cfg.reactionStatsBgR, cfg.reactionStatsBgG, cfg.reactionStatsBgB, cfg.reactionStatsBgA);
    }

    renderer.AddText(centerX, centerY - textHeightPx * 0.5f, buf, textHeightPx,
                     cfg.reactionStatsR, cfg.reactionStatsG, cfg.reactionStatsB, 1.0f, Renderer::HAlign::Center);
    return true;
}

// --- Saved brake profiles: the best five laps, kept across sessions ---

namespace
{
    const unsigned kBrakeFileMagic = 0x4B425253; // "SRBK"
    const unsigned kBrakeFileVersion = 1;
}

void BrakeCueStore::LoadSavedLaps(const std::string& key, size_t bins)
{
    // Read into a local list first, outside any lock; m_saveLock is only
    // taken to swap it in (FlushIfDirty may be copying the old list on the
    // save thread at the same moment).
    std::vector<SavedLap> laps;
    char dir[MAX_PATH];
    LapDeltaStore::SaveDir(dir, sizeof(dir));
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s.brake", dir, key.c_str());
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") == 0 && f)
    {
        unsigned hdr[4] = {};
        if (fread(hdr, sizeof(unsigned), 4, f) == 4 && hdr[0] == kBrakeFileMagic && hdr[1] == kBrakeFileVersion
            && hdr[2] <= static_cast<unsigned>(kKeptLaps))
        {
            if (hdr[3] == bins)
            {
                for (unsigned i = 0; i < hdr[2]; ++i)
                {
                    SavedLap lap;
                    lap.brake.resize(bins);
                    if (fread(&lap.lapTime, sizeof(double), 1, f) != 1
                        || fread(lap.brake.data(), sizeof(float), bins, f) != bins)
                        break;
                    laps.push_back(std::move(lap));
                }
            }
            else
                Log::Write("BrakeCue: saved brake profiles for %s are for a different track length -- ignored", key.c_str());
        }
        fclose(f);
    }
    std::sort(laps.begin(), laps.end(),
              [](const SavedLap& a, const SavedLap& b) { return a.lapTime < b.lapTime; });

    AcquireSRWLockExclusive(&m_saveLock);
    m_savedKey = key;
    m_savedLaps.swap(laps);
    m_savedDirty = false;
    ReleaseSRWLockExclusive(&m_saveLock);

    if (m_savedLaps.empty())
        Log::Write("BrakeCue: no saved brake profiles yet for %s", key.c_str());
    else
        Log::Write("BrakeCue: loaded %zu saved brake profiles for %s (fastest %.3fs)",
                   m_savedLaps.size(), key.c_str(), m_savedLaps.front().lapTime);
    RebuildReferenceFromSaved();
}

void BrakeCueStore::WriteSavedLaps(const std::string& key, const std::vector<SavedLap>& laps)
{
    if (key.empty() || laps.empty())
        return;
    char dir[MAX_PATH];
    LapDeltaStore::SaveDir(dir, sizeof(dir));
    CreateDirectoryA(dir, nullptr);
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s.brake", dir, key.c_str());
    char tmp[StoreFile::kTmpPathSize];
    FILE* f = StoreFile::Begin(path, tmp, sizeof(tmp), "wb");
    if (!f)
    {
        Log::Write("BrakeCue: failed to save brake profiles to %s", path);
        return;
    }
    const unsigned bins = static_cast<unsigned>(laps.front().brake.size());
    const unsigned hdr[4] = { kBrakeFileMagic, kBrakeFileVersion, static_cast<unsigned>(laps.size()), bins };
    bool ok = fwrite(hdr, sizeof(unsigned), 4, f) == 4;
    for (const SavedLap& lap : laps)
    {
        ok = ok && fwrite(&lap.lapTime, sizeof(double), 1, f) == 1;
        ok = ok && fwrite(lap.brake.data(), sizeof(float), bins, f) == bins;
    }
    if (!StoreFile::Commit(f, tmp, path, ok))
    {
        Log::Write("BrakeCue: failed to save brake profiles to %s", path);
        return;
    }
    Log::Write("BrakeCue: saved %zu brake profiles to %s", laps.size(), path);
}

void BrakeCueStore::AddSavedLap(double lapTime, const std::vector<float>& brake)
{
    if (m_savedKey.empty() || brake.empty() || lapTime <= 0.0)
        return;
    if (!m_savedLaps.empty() && m_savedLaps.front().brake.size() != brake.size())
        return; // track length changed under us -- not comparable
    if (static_cast<int>(m_savedLaps.size()) >= kKeptLaps && lapTime >= m_savedLaps.back().lapTime)
        return;
    SavedLap lap{ lapTime, brake };   // copied before taking the lock
    AcquireSRWLockExclusive(&m_saveLock);
    m_savedLaps.push_back(std::move(lap));
    std::sort(m_savedLaps.begin(), m_savedLaps.end(),
              [](const SavedLap& a, const SavedLap& b) { return a.lapTime < b.lapTime; });
    if (static_cast<int>(m_savedLaps.size()) > kKeptLaps)
        m_savedLaps.resize(kKeptLaps);
    m_savedDirty = true;
    ReleaseSRWLockExclusive(&m_saveLock);
    // Learned at (or a tick or two after) the line: the save pass the line
    // itself asked for may already have run.
    Persist::RequestLearned();
    Log::Write("BrakeCue: %.3fs joins the best %d brake profiles", lapTime, kKeptLaps);
    RebuildReferenceFromSaved();
}

void BrakeCueStore::RebuildReferenceFromSaved()
{
    std::vector<float> ref;
    if (!m_savedLaps.empty())
    {
        const size_t bins = m_savedLaps.front().brake.size();
        const size_t n = m_savedLaps.size();
        ref.resize(bins);
        std::vector<float> v(n);
        for (size_t b = 0; b < bins; ++b)
        {
            for (size_t i = 0; i < n; ++i) v[i] = m_savedLaps[i].brake[b];
            std::sort(v.begin(), v.end());
            ref[b] = (n % 2) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
        }
    }
    EnterCriticalSection(&m_lock);
    m_refBrake.swap(ref);
    LeaveCriticalSection(&m_lock);
}

void BrakeCueStore::FlushIfDirty()
{
    if (!m_ready)
        return;
    // Save thread, any time. Copy under m_saveLock (which only the scoring
    // thread's list changes also take), write after letting go.
    std::string key;
    std::vector<SavedLap> laps;
    AcquireSRWLockExclusive(&m_saveLock);
    if (m_savedDirty)
    {
        key = m_savedKey;
        laps = m_savedLaps;
        m_savedDirty = false;
    }
    ReleaseSRWLockExclusive(&m_saveLock);
    WriteSavedLaps(key, laps);
}