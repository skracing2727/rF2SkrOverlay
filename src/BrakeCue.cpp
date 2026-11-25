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
#include <cmath>
#include <cstdio>
#include <cstring>

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

bool BrakeCueStore::PlayBeep()
{
    if (!m_ready || !m_wavBuffer)
        return false;
    // The Ctrl+Shift toggle kills the beep as well as the on-screen cue --
    // one switch for the whole feature, so hiding it does not leave the sim
    // still beeping at you from a widget you cannot see.
    // Toggled, not Visible: layout mode or Enabled=0 overrides must not
    // switch the beep on (review F199).
    if (!Shortcuts::Toggled(Shortcuts::BrakeCue))
        return false;
    // Handed to the sound thread: even SND_ASYNC PlaySound opens the wave
    // device before returning, and this is the game's telemetry callback.
    Sound::PlayMemory(m_wavBuffer);
    return true;
}

namespace
{
    // Largest forward step between two telemetry ticks still treated as
    // driving past a cue (a scoring resync, a cut chicane). Anything bigger
    // is a teleport/rejoin: nothing fires across it.
    const double kMaxCueStepM = 60.0;

    double WrapDist(double d, double len)
    {
        while (d >= len) d -= len;
        while (d < 0.0) d += len;
        return d;
    }

    // Did moving 'step' metres forward from 'prev' pass 'target'? Wrap-aware.
    bool Crossed(double prev, double step, double target, double len)
    {
        const double ahead = WrapDist(target - prev, len);
        return ahead > 0.0 && ahead <= step;
    }
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

    // Forward speed from local velocity. rF2's local +z points to the REAR,
    // so forward is -z; the raw value ran the extrapolated distance
    // backwards between scoring resyncs (review F240, as MiniSector/Radar).
    m_forwardSpeed = -info.mLocalVel.z;

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

    // Scoring re-anchored (lap count jumped) or the car is not under local
    // control: this lap's recording can no longer count.
    if (m_invalidateLap.exchange(false) || !m_recordAllowed.load())
        m_lapFullyRecorded = false;

    // --- Lap rollover, straight from telemetry (review F65/F394) ---
    // The finished lap's points go to scoring under m_lock; the
    // accept/reject decision waits there for the lap time.
    if (m_lastTelemLap < 0)
    {
        m_lastTelemLap = info.mLapNumber;
        m_lapFullyRecorded = false; // joined part way
    }
    else if (info.mLapNumber != m_lastTelemLap)
    {
        const bool plusOne = (info.mLapNumber == m_lastTelemLap + 1);
        m_lastTelemLap = info.mLapNumber;
        EnterCriticalSection(&m_lock);
        m_finished.count = m_recordingCount;
        for (int i = 0; i < m_recordingCount; ++i)
        {
            m_finished.points[i] = m_recording[i];
            m_finished.dropMs[i] = m_recordingEntrySpeed[i] - m_recordingMinSpeed[i];
        }
        m_finished.fullyRecorded = m_lapFullyRecorded && plusOne;
        m_finishedReady = true;
        LeaveCriticalSection(&m_lock);
        m_recordingCount = 0;
        m_lapFullyRecorded = plusOne && m_recordAllowed.load();
    }

    if (!g_brakeCueConfig.enabled)
    {
        m_havePrevDist = false;
        return;
    }

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
    // Every point whose target was crossed since last tick fires, in any
    // order (review F59/F394). A fired point re-arms half a lap later, so a
    // target sliding forward as the car slows cannot fire it twice.
    bool beep = false;
    if (m_havePrevDist && m_trackLength > 1.0)
    {
        const double len = m_trackLength;
        double step = dist - m_prevDist;
        if (step < -0.5 * len) step += len;
        else if (step > 0.5 * len) step -= len;
        if (step > 0.0 && step <= kMaxCueStepM)
        {
            // Current speed as the estimate of speed at the point itself --
            // re-evaluated every tick, so it keeps refining up to the trigger.
            const double leadDistance = fabs(m_forwardSpeed) * LeadTimeSeconds();
            const bool cueOk = m_cueAllowed.load();
            EnterCriticalSection(&m_lock);
            for (int i = 0; i < m_referenceCount; ++i)
            {
                if (!m_refFired[i])
                {
                    if (Crossed(m_prevDist, step, WrapDist(m_reference[i] - leadDistance, len), len))
                    {
                        m_refFired[i] = true;
                        // Small stops (a lift, a dab) pass silently; pit lane,
                        // yellow, AI or ride-along pass silently too (F257).
                        if (cueOk && m_referenceDropMs[i] * 3.6 >= g_brakeCueConfig.minSpeedDropKmh)
                            beep = true;
                    }
                }
                else if (Crossed(m_prevDist, step, WrapDist(m_reference[i] + 0.5 * len, len), len))
                    m_refFired[i] = false;
            }
            LeaveCriticalSection(&m_lock);
        }
    }
    m_prevDist = dist;
    m_havePrevDist = true;

    // Only arm a measurement if a beep was really heard and the pedal is
    // free -- a cue landing mid-braking has no pedal movement of its own.
    if (beep && PlayBeep())
    {
        m_pendingBeep = !pedalPressed;
        m_beepTime = now;
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
    {
        // Ride-along / spectating: nothing here is ours (review F62).
        m_cueAllowed = false;
        m_recordAllowed = false;
        return;
    }

    // Only learn from and beep for the player's own driving (review F62),
    // and only beep on track at green in realtime (review F257).
    const bool human = (player->mControl == 0);
    // Also not while the AI or a team-mate drives our car (review F167/F252).
    m_recordAllowed = human && g_localPlayerDriving.load(std::memory_order_relaxed);
    // Phase 5 (green), 6 (full-course yellow) and 8 (leader has finished)
    // while you are still racing: phase 5 alone went silent on your last lap
    // once the leader took the flag, and under FCY (CrewChief review
    // 2026-11-24). Your own mFinishStatus ends it once you are done.
    const bool racingPhase = info.mGamePhase == 5 || info.mGamePhase == 6
                          || info.mGamePhase == 8;
    m_cueAllowed = human && !player->mInPits && info.mInRealtime && racingPhase
                && player->mFinishStatus == 0;

    // Resync extrapolated distance to the authoritative value.
    m_lastKnownDist = player->mLapDist;
    QueryPerformanceCounter(&m_lastSyncTime);

    // Latch the minimum mCountLapFlag seen during the lap: the flag is a
    // transient property of the lap IN PROGRESS that resets before being read
    // at lap completion. Latching the minimum catches the valid moment, even
    // if it later resets.
    if (player->mCountLapFlag < m_lapValidityLatch)
        m_lapValidityLatch = player->mCountLapFlag;

    // A different car in the same session (LapDelta's car+track key): the
    // old car's braking points and best time must not cue this one (F359).
    {
        const std::string& key = g_lapDelta.CurrentKey();
        if (!key.empty() && key != m_carKey)
        {
            if (!m_carKey.empty())
                ResetReference();
            m_carKey = key;
        }
    }

    if (m_lastTotalLaps < 0)
    {
        m_lastTotalLaps = player->mTotalLaps;
        m_lapTimeAtStart = player->mLastLapTime;
        return;
    }

    if (player->mTotalLaps != m_lastTotalLaps)
    {
        const bool plusOne = (player->mTotalLaps == m_lastTotalLaps + 1);
        m_lastTotalLaps = player->mTotalLaps;
        if (plusOne)
        {
            // Lap just completed. The decision waits for both the telemetry
            // snapshot and a fresh lap time (review F58).
            m_pendingLap = true;
            m_pendingHaveSnap = false;
            // Captured now: the "driven by you all lap" latch is re-armed
            // after this tick, and the lap may resolve ticks later.
            m_pendingCountsForTime = (m_lapValidityLatch >= 1)
                && g_localPlayerLapClean.load(std::memory_order_relaxed);
            m_pendingOldLapTime = m_lapTimeAtStart;
            m_pendingSinceET = info.mCurrentET;
        }
        else
        {
            // Any other jump (restart, stale tick, a team-mate's stint) is
            // not one finished lap: re-anchor instead (review F61).
            m_pendingLap = false;
            m_invalidateLap = true;
            m_lapTimeAtStart = player->mLastLapTime;
            EnterCriticalSection(&m_lock);
            m_finishedReady = false;
            LeaveCriticalSection(&m_lock);
            Log::Write("BrakeCue: lap count jumped to %d -- re-anchored", static_cast<int>(player->mTotalLaps));
        }
        m_lapValidityLatch = 2; // reset for next lap
    }

    if (m_pendingLap)
        ResolvePendingLap(player->mLastLapTime, info.mCurrentET);
    // The time seen before the next line tick (review F115): a lap time that
    // arrived after a pending lap timed out must not be taken as the next
    // lap's time.
    if (!m_pendingLap)
        m_lapTimeAtStart = player->mLastLapTime;
}

void BrakeCueStore::ResolvePendingLap(double lastLapTime, double currentET)
{
    if (!m_pendingHaveSnap)
    {
        EnterCriticalSection(&m_lock);
        if (m_finishedReady)
        {
            m_pendingSnap = m_finished;
            m_finishedReady = false;
            m_pendingHaveSnap = true;
        }
        LeaveCriticalSection(&m_lock);
    }

    const bool fresh = lastLapTime > 0.0 && lastLapTime != m_pendingOldLapTime;
    if (!m_pendingHaveSnap || !fresh)
    {
        if (currentET - m_pendingSinceET > 5.0)
        {
            m_pendingLap = false;
            m_lapTimeAtStart = lastLapTime;
        }
        return;
    }
    m_pendingLap = false;
    m_lapTimeAtStart = lastLapTime;

    // Same acceptance rule as LapDelta (review F57): flag >= 1, a plausible
    // time, and a lap recorded from the line under local control.
    const bool plausible = lastLapTime > 5.0 && lastLapTime < 100000.0;
    const bool valid = m_pendingCountsForTime && plausible && m_pendingSnap.fullyRecorded;
    if (!valid)
        return;

    if (m_referenceLapTime >= 0.0 && lastLapTime >= m_referenceLapTime)
        return;
    m_referenceLapTime = lastLapTime;
    // A best lap without braking points (beep off) only moves the time.
    const int n = m_pendingSnap.count;
    if (n <= 0)
        return;

    int bigStops = 0;
    EnterCriticalSection(&m_lock);
    for (int i = 0; i < n; ++i)
    {
        m_reference[i] = m_pendingSnap.points[i];
        m_referenceDropMs[i] = m_pendingSnap.dropMs[i];
        m_refFired[i] = false;
        if (m_referenceDropMs[i] * 3.6 >= g_brakeCueConfig.minSpeedDropKmh)
            ++bigStops;
    }
    m_referenceCount = n;
    LeaveCriticalSection(&m_lock);
    Log::Write("BrakeCue: new best lap %.3fs, %d braking points recorded, %d over the %.0f km/h drop",
               m_referenceLapTime, n, bigStops, g_brakeCueConfig.minSpeedDropKmh);
    for (int i = 0; i < n; ++i)
        Log::Write("BrakeCue:   point %d at %.0fm, drop %.0f km/h", i + 1, m_pendingSnap.points[i],
                   m_pendingSnap.dropMs[i] * 3.6);
}

void BrakeCueStore::ResetReference()
{
    EnterCriticalSection(&m_lock);
    m_referenceCount = 0;
    m_finishedReady = false;
    LeaveCriticalSection(&m_lock);
    m_referenceLapTime = -1.0;
    m_pendingLap = false;
    m_invalidateLap = true;
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
    m_lastTotalLaps = -1;
    m_lastTelemLap = -1;
    m_havePrevDist = false;
    m_pendingLap = false;

    m_pendingBeep = false;
    m_wasPedalPressed = false;

    // The lap in progress is now a partial one -- see OnScoring.
    m_lapFullyRecorded = false;
    if (m_ready)
    {
        EnterCriticalSection(&m_lock);
        m_finishedReady = false;
        // Re-armed: on rejoin only targets actually crossed fire (F59).
        for (int i = 0; i < kMaxPoints; ++i) m_refFired[i] = false;
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
    if (m_ready)
    {
        EnterCriticalSection(&m_lock);
        m_referenceCount = 0;
        LeaveCriticalSection(&m_lock);
    }
    m_referenceLapTime = -1.0;

    m_carKey.clear();
    m_pendingLap = false;

    m_reactionHistoryCount = 0;
    m_reactionHistoryNext = 0;
    m_avgReactionSeconds = -1.0;

    if (!m_ready)
        return;
    EnterCriticalSection(&m_lock);
    m_state = State{};
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
