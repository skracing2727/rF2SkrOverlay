#include "LapDelta.h"
#include "InternalsPlugin.hpp"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "Log.h"
#include "DrsCue.h"
#include "Shortcuts.h"
#include "RealtimeState.h"
#include "RenderThrottle.h"
#include "Persist.h"
#include "StoreFile.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

LapDeltaStore g_lapDelta;

namespace
{
    // Same DLL-relative-path resolution as Config.cpp, so the saved best-lap
    // files live next to the INI rather than wherever the game's working
    // directory happens to be.
    void GetBestLapDir(char* out, size_t outSize)
    {
        char dllPath[MAX_PATH] = {};
        HMODULE hModule = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCSTR>(&GetBestLapDir), &hModule);
        GetModuleFileNameA(hModule, dllPath, MAX_PATH);

        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';

        sprintf_s(out, outSize, "%sBestLaps", dllPath);
    }

    // maxLen bounds the scan to the SDK's own fixed buffer size (see the call
    // sites) -- mVehicleName/mTrackName are plain char[64] fields with no
    // documented guarantee of null-termination if a name exactly fills the
    // buffer, so scanning for a '\0' without a bound could read past the end
    // of the struct.
    std::string Sanitize(const char* s, size_t maxLen)
    {
        return StoreFile::SafeName(s, maxLen); // shared rule, review F420
    }

    // Whether a trace is in non-decreasing lap distance, so InterpolateAt can
    // binary-search it. Practically always true (a point is only added once
    // the car is further round than the last tick), but a car reversing back
    // down the track and then driving on can leave a step backwards in it.
    bool SortedByDistance(const std::vector<std::pair<double, double>>& trace)
    {
        for (size_t i = 1; i < trace.size(); ++i)
            if (trace[i].first < trace[i - 1].first)
                return false;
        return true;
    }

    // A lap trace that was actually driven in one go (review F97): no
    // distance jump over 150 m between samples, time never going backwards,
    // no implied speed over 100 m/s, and the last sample close to the lap
    // time. Catches laps patched together across a garage return/teleport.
    bool TraceConsistent(const std::vector<std::pair<double, double>>& trace, double lapTime)
    {
        if (trace.empty()) return false;
        for (size_t i = 1; i < trace.size(); ++i)
        {
            const double dd = trace[i].first - trace[i - 1].first;
            const double dt = trace[i].second - trace[i - 1].second;
            if (dd > 150.0 || dt < 0.0) return false;
            if (dt > 0.0 && dd / dt > 100.0) return false;
            if (dt <= 0.0 && dd > 25.0) return false;
        }
        const double tail = lapTime - trace.back().second;
        return tail >= -0.05 && tail <= 1.0;
    }

    void BestLapPath(const std::string& key, char* path, size_t pathSize)
    {
        char dir[MAX_PATH];
        GetBestLapDir(dir, sizeof(dir));
        sprintf_s(path, pathSize, "%s\\%s.csv", dir, key.c_str());
    }

    // Save thread only (see LapDeltaStore::FlushIfDirty).
    bool WriteBestLapFile(const std::string& key, double lapTimeSeconds,
                          const std::vector<std::pair<double, double>>& trace)
    {
        char dir[MAX_PATH];
        GetBestLapDir(dir, sizeof(dir));
        CreateDirectoryA(dir, nullptr); // no-op (ERROR_ALREADY_EXISTS) if it's already there

        char path[MAX_PATH];
        BestLapPath(key, path, sizeof(path));

        char tmp[StoreFile::kTmpPathSize];
        FILE* f = StoreFile::Begin(path, tmp, sizeof(tmp), "w");
        if (!f)
        {
            Log::Write("LapDelta: failed to save best lap to %s", path);
            return false;
        }
        fprintf(f, "%.6f\n", lapTimeSeconds);
        for (const auto& sample : trace)
            fprintf(f, "%.3f,%.4f\n", sample.first, sample.second);
        if (!StoreFile::Commit(f, tmp, path))
        {
            Log::Write("LapDelta: failed to save best lap to %s", path);
            return false;
        }
        Log::Write("LapDelta: saved new best lap %.3fs to %s", lapTimeSeconds, path);
        return true;
    }
}

void LapDeltaStore::SaveDir(char* out, size_t outSize)
{
    GetBestLapDir(out, outSize);
}

bool LapDeltaStore::Reference::InterpolateAt(double lapDist, double& outTime) const
{
    if (trace.size() < 2)
        return false;

    if (lapDist <= trace.front().first) { outTime = trace.front().second; return true; }
    if (lapDist >= trace.back().first) { outTime = trace.back().second; return true; }

    // Binary search: RelativeTiming asks this for every car on every scoring
    // tick, against a trace of several hundred points. It finds the same
    // pair the linear scan below does -- the first point at or beyond
    // lapDist -- because the trace is in order of distance (checked once,
    // when it was set; see SortedByDistance).
    if (sorted)
    {
        const auto it = std::lower_bound(trace.begin(), trace.end(), lapDist,
            [](const std::pair<double, double>& p, double d) { return p.first < d; });
        const size_t i = static_cast<size_t>(it - trace.begin());
        if (i == 0 || i >= trace.size())
        {
            outTime = trace.back().second;   // only a NaN distance gets here
            return true;
        }
        const double d0 = trace[i - 1].first, t0 = trace[i - 1].second;
        const double d1 = trace[i].first, t1 = trace[i].second;
        const double span = d1 - d0;
        const double frac = (span > 0.0001) ? (lapDist - d0) / span : 0.0;
        outTime = t0 + (t1 - t0) * frac;
        return true;
    }

    // Out-of-order trace (rare, see SortedByDistance): the original linear
    // scan, so the result is exactly what it always was.
    for (size_t i = 1; i < trace.size(); ++i)
    {
        if (lapDist <= trace[i].first)
        {
            const double d0 = trace[i - 1].first, t0 = trace[i - 1].second;
            const double d1 = trace[i].first, t1 = trace[i].second;
            const double span = d1 - d0;
            const double frac = (span > 0.0001) ? (lapDist - d0) / span : 0.0;
            outTime = t0 + (t1 - t0) * frac;
            return true;
        }
    }
    outTime = trace.back().second;
    return true;
}

void LapDeltaStore::Reference::UpdateTrend(double delta, LARGE_INTEGER now, LARGE_INTEGER freq, double windowSeconds, DeltaState& outState)
{
    if (windowSeconds < 0.1) windowSeconds = 0.1;

    const double sinceTrendSec = (lastTrendTime.QuadPart == 0) ? 0.0
        : static_cast<double>(now.QuadPart - lastTrendTime.QuadPart) / static_cast<double>(freq.QuadPart);
    if (sinceTrendSec >= windowSeconds)
    {
        outState.trendPerSecond = (delta - lastDeltaForTrend) / sinceTrendSec;
        lastDeltaForTrend = delta;
        lastTrendTime = now;
    }
    else if (lastTrendTime.QuadPart == 0)
    {
        lastDeltaForTrend = delta;
        lastTrendTime = now;
    }
    outState.deltaSeconds = delta;
    outState.updatedAt = now;
    outState.valid = true;
}

void LapDeltaStore::Init()
{
    if (m_ready) return;
    InitializeCriticalSection(&m_lock);
    QueryPerformanceFrequency(&m_freq);
    m_ready = true;
}

void LapDeltaStore::Clear()
{
    if (!m_ready) return;
    // A PB reset the scoring thread never got round to: done now, while
    // m_currentKey still names the file it was meant for.
    if (m_resetPbRequested.exchange(false))
        ApplyPersonalBestReset();
    // Callers flush (Persist::FlushAllNow) before Clear() -- see Plugin.cpp.
    // A save still waiting regardless (that flush timed out) is kept rather
    // than dropped: the save slot carries its own key and trace, so it can no
    // longer end up written under a cleared key, which is why it used to be
    // dropped here.
    EnterCriticalSection(&m_lock);
    m_currentTrace.clear();
    if (!m_currentKey.empty())
        m_lastKey = m_currentKey; // m_best still belongs to it (review F101)
    m_currentKey.clear();
    m_lastTotalLaps = -1;
    m_lastLapDist = -1.0;
    m_lapValidityLatch = 2;  // a 0 from the in-lap must not carry over (F101)
    m_lapTainted = false;
    m_lastET = -1.0;
    m_session = Reference{}; // session-best forgotten; "best" (persisted) is untouched
    {
        const unsigned epoch = m_state.refEpoch + 1;
        m_state = State{};
        m_state.refEpoch = epoch;
    }
    LeaveCriticalSection(&m_lock);
}

void LapDeltaStore::FlushIfDirty()
{
    if (!m_ready) return;
    // Save thread, any time. The save slot is swapped out under m_saveLock
    // (nothing is copied while holding it), the file work done after. The
    // delete goes first: a PB reset followed by a new best before this ran
    // must end with the new best on disk.
    std::string deleteKey, saveKey;
    std::vector<std::pair<double, double>> trace;
    double lapTime = -1.0;
    bool doDelete = false, doSave = false;
    AcquireSRWLockExclusive(&m_saveLock);
    if (m_deletePending)
    {
        deleteKey.swap(m_deleteKey);
        m_deletePending = false;
        doDelete = true;
    }
    if (m_bestLapDirty)
    {
        saveKey.swap(m_saveKey);
        trace.swap(m_saveTrace);
        lapTime = m_saveLapTime;
        m_bestLapDirty = false;
        doSave = true;
    }
    ReleaseSRWLockExclusive(&m_saveLock);

    if (doDelete)
    {
        // Deleted, not just left: otherwise the next session simply loads the
        // "deleted" lap straight back off disk.
        char path[MAX_PATH];
        BestLapPath(deleteKey, path, sizeof(path));
        const bool removed = DeleteFileA(path) != 0;
        const DWORD err = removed ? 0 : GetLastError();
        if (removed || err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
        {
            Log::Write("LapDelta: personal best file %s %s", path, removed ? "deleted" : "-- not present");
            // A "cleared" marker in its place, so a car without a PB of its
            // own is not seeded again from the old per-class file -- the PB
            // the driver just threw away (per-car keys, 2026-11-24).
            char tmp[StoreFile::kTmpPathSize];
            if (FILE* mf = StoreFile::Begin(path, tmp, sizeof(tmp), "w"))
            {
                fputs("cleared\n", mf);
                StoreFile::Commit(mf, tmp, path);
            }
        }
        else
        {
            // Locked or read-only: tried again next pass, unless a newer
            // delete or save has been queued meanwhile (review F278).
            Log::Write("LapDelta: could not delete personal best file %s (error %lu)", path, err);
            static int s_deleteFailures = 0;
            AcquireSRWLockExclusive(&m_saveLock);
            if (!m_deletePending && !m_bestLapDirty && !doSave && ++s_deleteFailures <= 3)
            {
                m_deleteKey = deleteKey;
                m_deletePending = true;
            }
            ReleaseSRWLockExclusive(&m_saveLock);
        }
    }
    // A failed write is queued again (review F278), a few tries only, and
    // only if no newer best has replaced it.
    static int s_saveFailures = 0;
    if (doSave)
    {
        if (WriteBestLapFile(saveKey, lapTime, trace))
            s_saveFailures = 0;
        else if (++s_saveFailures <= 3)
        {
            AcquireSRWLockExclusive(&m_saveLock);
            if (!m_bestLapDirty)
            {
                m_saveKey = saveKey;
                m_saveTrace.swap(trace);
                m_saveLapTime = lapTime;
                m_bestLapDirty = true;
            }
            ReleaseSRWLockExclusive(&m_saveLock);
        }
    }
}

void LapDeltaStore::LoadBestLap(const char* vehicleName, const char* trackName, double trackLength)
{
    // Scoring thread, on a car/track change. m_best is this thread's own, so
    // the file is simply read into it; no lock is involved.
    std::vector<std::pair<double, double>> trace;
    double lapTime = -1.0;

    char path[MAX_PATH];
    BestLapPath(m_currentKey, path, sizeof(path));

    // A car with no file yet starts from its old per-class file (read
    // only; the old file stays). Its next PB is saved under the car key.
    if (!StoreFile::Exists(path) && !m_legacyKey.empty() && m_legacyKey != m_currentKey)
    {
        char legacy[MAX_PATH];
        BestLapPath(m_legacyKey, legacy, sizeof(legacy));
        if (StoreFile::Exists(legacy))
        {
            strcpy_s(path, legacy);
            Log::Write("LapDelta: no best lap yet for %s -- seeding from old class file %s", m_currentKey.c_str(), m_legacyKey.c_str());
        }
    }

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f)
    {
        m_best.trace.clear();
        m_best.lapTimeSeconds = -1.0;
        m_best.sorted = true;
        Log::Write("LapDelta: no saved best lap yet for %s / %s", vehicleName, trackName);
        return;
    }

    char line[128];
    if (fgets(line, sizeof(line), f))
    {
        // Left by a PB reset (see FlushIfDirty): no PB, and no seeding.
        if (strncmp(line, "cleared", 7) == 0)
        {
            fclose(f);
            m_best.trace.clear();
            m_best.lapTimeSeconds = -1.0;
            m_best.sorted = true;
            Log::Write("LapDelta: personal best for %s / %s was cleared -- none yet", vehicleName, trackName);
            return;
        }
        lapTime = atof(line);
    }

    while (fgets(line, sizeof(line), f))
    {
        double dist = 0.0, t = 0.0;
        if (sscanf_s(line, "%lf,%lf", &dist, &t) == 2)
            trace.emplace_back(dist, t);
    }
    fclose(f);

    // Sanity-check the file before trusting it (review F102): a bad header
    // used to give a 0.0 PB that no lap could ever beat.
    const char* bad = nullptr;
    if (!(lapTime > 5.0 && lapTime < 100000.0)) bad = "lap time out of range";
    else if (trace.size() < 20) bad = "too few points";
    else
    {
        for (size_t i = 1; i < trace.size() && !bad; ++i)
            if (trace[i].first < trace[i - 1].first || trace[i].second < trace[i - 1].second)
                bad = "distance/time goes backwards";
        if (!bad && fabs(trace.back().second - lapTime) > 0.5)
            bad = "end time does not match lap time";
        if (!bad && trackLength > 1.0 && fabs(trace.back().first - trackLength) > trackLength * 0.01)
            bad = "end distance does not match track length";
    }
    if (bad)
    {
        Log::Write("LapDelta: ignoring saved best lap for %s / %s (%s)", vehicleName, trackName, bad);
        m_best.trace.clear();
        m_best.lapTimeSeconds = -1.0;
        m_best.sorted = true;
        return;
    }

    m_best.sorted = SortedByDistance(trace);
    m_best.trace.swap(trace);
    m_best.lapTimeSeconds = lapTime;

    Log::Write("LapDelta: loaded best lap %.3fs (%zu points) for %s / %s",
               m_best.lapTimeSeconds, m_best.trace.size(), vehicleName, trackName);
}

void LapDeltaStore::ResetSessionBest()
{
    if (!m_ready) return;
    // Render thread (hotkey). The readout clears here; the trace itself is
    // dropped by the scoring thread at its next tick, which reads m_session
    // without a lock (same as ResetPersonalBest).
    m_resetSessionRequested.store(true);
    EnterCriticalSection(&m_lock);
    m_state.session = DeltaState{};
    m_state.haveLineFreeze = false;
    ++m_state.refEpoch;
    LeaveCriticalSection(&m_lock);
    Log::Write("LapDelta: session best cleared by hotkey");
}

void LapDeltaStore::ResetPersonalBest()
{
    if (!m_ready) return;

    // Called from the hotkeys on the render thread. What shows on screen is
    // cleared right here; the trace itself is dropped by the scoring thread
    // at its next tick (ApplyPersonalBestReset) -- it reads m_best without a
    // lock, for every car, every tick, so freeing it from this thread could
    // pull the vector out from under it. The file is deleted by the next
    // save pass, not here: no disk work on the render thread.
    m_resetPbRequested.store(true);
    EnterCriticalSection(&m_lock);
    m_state.best = DeltaState{};
    m_state.haveLineFreeze = false;
    ++m_state.refEpoch;
    LeaveCriticalSection(&m_lock);
}

void LapDeltaStore::ApplyPersonalBestReset()
{
    // Scoring thread (or Clear, between sessions).
    AcquireSRWLockExclusive(&m_saveLock);
    // A best lap set moments ago but not yet saved would otherwise survive
    // this reset -- the save recreating the just-deleted file from it.
    m_bestLapDirty = false;
    m_saveTrace.clear();
    // After Clear() the key is empty but m_best still holds the last car's
    // PB, so fall back to that key -- otherwise the file stays (F101).
    const std::string resetKey = m_currentKey.empty() ? m_lastKey : m_currentKey;
    if (!resetKey.empty())
    {
        m_deleteKey = resetKey;
        m_deletePending = true;
    }
    ReleaseSRWLockExclusive(&m_saveLock);

    EnterCriticalSection(&m_lock);
    m_best = Reference{};
    m_state.best = DeltaState{};
    m_state.haveLineFreeze = false;
    ++m_state.refEpoch;
    LeaveCriticalSection(&m_lock);

    // No Persist::RequestLearned() here: that ran a full learned save pass
    // mid-lap (review F275). The delete waits for the next normal save pass
    // (line crossing or leaving the car).
    Log::Write("LapDelta: personal best cleared by hotkey (%s)", resetKey.c_str());
}

void LapDeltaStore::DescribePersonalBest(char* out, size_t outSize) const
{
    if (!out || outSize == 0) return;
    if (m_best.lapTimeSeconds <= 0.0)
    {
        strcpy_s(out, outSize, "none set");
        return;
    }
    const int totalMs = static_cast<int>(m_best.lapTimeSeconds * 1000.0 + 0.5);
    sprintf_s(out, outSize, "%d:%02d.%03d", totalMs / 60000, (totalMs / 1000) % 60, totalMs % 1000);
}

double LapDeltaStore::PersonalBestSeconds() const
{
    return m_best.lapTimeSeconds;
}

bool LapDeltaStore::PersonalBestInterpolateAt(double lapDist, double& outTime) const
{
    return m_best.InterpolateAt(lapDist, outTime);
}

void LapDeltaStore::OnScoring(const ScoringInfoV01& info)
{
    if (!m_ready)
        return;

    // A PB reset from the hotkey, done first so it applies to the car/track
    // it was asked for even if this tick changes them.
    if (m_resetPbRequested.exchange(false))
        ApplyPersonalBestReset();
    if (m_resetSessionRequested.exchange(false))
    {
        EnterCriticalSection(&m_lock);
        m_session = Reference{};
        m_state.session = DeltaState{};
        ++m_state.refEpoch;
        LeaveCriticalSection(&m_lock);
    }

    const VehicleScoringInfoV01* player = nullptr;
    if (info.mVehicle != nullptr)
    {
        for (long i = 0; i < info.mNumVehicles; ++i)
        {
            if (info.mVehicle[i].mIsPlayer) { player = &info.mVehicle[i]; break; }
        }
    }
    if (!player)
    {
        // No player car (ride-along after a driver swap, spectating): hide
        // the rows instead of freezing the last reading on screen (F99).
        // Traces and references are left alone.
        EnterCriticalSection(&m_lock);
        m_state.best.valid = false;
        m_state.session.valid = false;
        m_state.haveLineFreeze = false;
        LeaveCriticalSection(&m_lock);
        m_lapTainted = true; // the lap in progress was not seen whole
        return;
    }

    // Keyed on the CAR model (review F261; see RealtimeState.h), not the
    // class or the per-entry name. Published here (this is the only place
    // with the scoring struct in hand) for the telemetry-driven stores to
    // reuse. The class key is kept to seed a car's first file.
    VehicleKey::SetFromScoring(player->mVehicleClass, player->mVehicleName);
    char vehKey[64], classKey[64];
    VehicleKey::Get(vehKey, sizeof(vehKey));
    VehicleKey::GetClass(classKey, sizeof(classKey));
    const std::string trackKey = Sanitize(info.mTrackName, sizeof(info.mTrackName));
    const std::string key = Sanitize(vehKey, sizeof(vehKey)) + "__" + trackKey;
    if (key != m_currentKey)
    {
        m_currentKey = key;
        m_legacyKey = Sanitize(classKey, sizeof(classKey)) + "__" + trackKey;
        LoadBestLap(vehKey, info.mTrackName, info.mLapDist);
        m_currentTrace.clear();
        m_lastTotalLaps = player->mTotalLaps;
        m_lastLapDist = player->mLapDist;
        m_lapValidityLatch = 2; // review F101
        m_lapTainted = true;    // joined mid-lap
        EnterCriticalSection(&m_lock);
        ++m_state.refEpoch;
        LeaveCriticalSection(&m_lock);
    }

    const double timeIntoLap = info.mCurrentET - player->mLapStartET;

    // Session clock went backwards (restart, rewind): the trace so far is
    // from another timeline (review F97).
    if (m_lastET >= 0.0 && info.mCurrentET < m_lastET - 0.5)
    {
        m_currentTrace.clear();
        m_lapTainted = true;
    }
    m_lastET = info.mCurrentET;

    // Lap just completed -- check both references before starting a fresh
    // trace for the lap now underway.
    if (m_lastTotalLaps >= 0 && player->mTotalLaps > m_lastTotalLaps)
    {
        // Snapshot the delta as it stood at the line, BEFORE anything below
        // recomputes it against the new lap. m_state still holds the previous
        // tick's values, which is exactly the reading wanted.
        // The held number is the lap's real result (last lap minus the
        // reference lap time, as the timing screen shows it), not the last
        // sample before the line (review F100). The references are still the
        // pre-update ones here. No reference = no hold.
        if (g_lapDeltaConfig.freezeAfterLineSeconds > 0.0f)
        {
            const double ll = player->mLastLapTime;
            const bool llOk = ll > 5.0 && ll < 100000.0;
            const bool bestOk = llOk && m_best.lapTimeSeconds > 0.0 && !m_resetPbRequested.load();
            const bool sessOk = llOk && m_session.lapTimeSeconds > 0.0;
            EnterCriticalSection(&m_lock);
            m_state.haveLineFreeze = (bestOk && m_state.best.valid) || (sessOk && m_state.session.valid);
            m_state.bestAtLine = bestOk ? ll - m_best.lapTimeSeconds : m_state.best.deltaSeconds;
            m_state.sessionAtLine = sessOk ? ll - m_session.lapTimeSeconds : m_state.session.deltaSeconds;
            QueryPerformanceCounter(&m_state.lineCrossedAt);
            LeaveCriticalSection(&m_lock);
        }

        // Reset the delta bar's auto window back to its floor for the lap
        // about to start -- it only grows again during THIS new lap if the
        // delta actually needs it (see the reactive growth in the scoring
        // update below). Applied on every completed lap, valid reference or
        // not, so even an off-line lap starts the next one fresh.
        if (g_lapDeltaConfig.autoBarWindowEnabled)
        {
            EnterCriticalSection(&m_lock);
            m_barWindowSeconds = g_lapDeltaConfig.maxBarSeconds;
            m_state.barWindowSeconds = m_barWindowSeconds;
            LeaveCriticalSection(&m_lock);
        }

        const double lastLap = player->mLastLapTime;
        const bool plausible = lastLap > 5.0 && lastLap < 100000.0;
        // Originally required the latch to hit exactly 2 ("count lap and
        // time"), per the SDK header's own wording. In practice, online,
        // this server (and others) reported flag 1 ("count lap but not
        // time") for ordinary, plausible flying laps that rF2's own overlay
        // showed as valid -- repeatedly, across several different tracks and
        // sessions, never a broken/implausible time. So flag 1 is treated as
        // countable too now; only 0 ("do not count lap or time" -- an
        // off-track cut, typically) is rejected. If a genuinely bogus lap
        // (flag 1, implausible line-up with the rest of the lap) starts
        // getting used as a reference because of this, tighten it back to
        // >= 2 and look at spansLap/plausible instead -- those two already
        // catch most of what made a flag-1 lap actually unusable.
        const bool countsForTime = m_lapValidityLatch >= 1;
        // The SAVED personal best (the file that outlives the session) is
        // stricter: only flag 2, as CrewChief does (CrewChief review
        // 2026-11-24). Flag 1 is usually a track-limits cut; letting it into
        // the session reference keeps the online flag-1 case above working,
        // but a cut lap written to disk would be the target in every later
        // session at this track.
        const bool countsForSavedPb = m_lapValidityLatch >= 2;
        // Not driven by the player in realtime the whole way, a pit / garage /
        // clock-jump moment during it, or a gap in the trace (F252, F97).
        const bool clean = !m_lapTainted && TraceConsistent(m_currentTrace, lastLap);

        // The trace must actually SPAN the lap, not merely contain points --
        // same requirement RelativeTiming.cpp's own reference trace enforces
        // (see there). A race's very first "lap" is the grid-to-line crawl,
        // which starts at a lap distance near the END of the lap (the grid
        // sits behind the start/finish line); an out-lap that joins pit-exit
        // mid-track has the same shape. Without this check, either becomes a
        // reference covering only a sliver of the track -- InterpolateAt then
        // clamps every query outside that sliver to its nearest endpoint,
        // freezing the reference time for most of the lap while the live
        // clock keeps counting, which is exactly what "the delta increases
        // and decreases wildly" looks like from the cockpit.
        const double trackLength = info.mLapDist; // session-level: total track length
        const bool spansLap = trackLength > 1.0
                             && m_currentTrace.size() >= 20
                             && m_currentTrace.front().first <= trackLength * 0.05
                             && m_currentTrace.back().first >= trackLength * 0.95;

        // Whether a completed lap becomes a reference is decided PURELY by
        // rF2's own mCountLapFlag, never by a lap number. That is what makes
        // a race behave differently from practice without any special case:
        // a practice out lap leaves the pits mid-lap and the sim flags it as
        // not counting for time, while a race's lap 1 starts ON the line and
        // does count -- so a race has a session best from the end of lap 1
        // and the delta from lap 2, where practice needs one lap more.
        // Logged so that if it ever behaves otherwise, the flag responsible
        // is visible rather than guessed at.
        Log::Write("LapDelta: lap %d complete, %.3fs, countLapFlag=%d%s -> %s",
                   static_cast<int>(player->mTotalLaps), lastLap, static_cast<int>(m_lapValidityLatch),
                   clean ? "" : " (tainted: AI/pit/garage/gap)",
                   (plausible && countsForTime && spansLap && clean) ? "recorded as reference" : "not counted");

        if (plausible && countsForTime && spansLap && clean)
        {
            // Close the trace off AT the finish line, with the lap's real
            // time. Samples only arrive at ~5Hz, so the last real one sits up
            // to a scoring interval short of the line -- at racing speed, ten
            // metres or more. InterpolateAt clamps anything past its last
            // sample to that sample's time, so without this cap the reference
            // clock effectively stops for the final stretch of every lap
            // while your own keeps running: the delta bar lurches positive
            // approaching every start/finish line, and MiniSector's last
            // segment (which asks for exactly trackLength) is timed against a
            // reference that never got there.
            if (trackLength > 1.0
                && m_currentTrace.back().first < trackLength
                && lastLap > m_currentTrace.back().second)
            {
                m_currentTrace.emplace_back(trackLength, lastLap);
            }

            // Same problem at the FRONT: the first sample lands up to a
            // scoring interval past the line, and InterpolateAt clamps
            // distance 0 to that sample's time, so the whole reference is
            // offset by that much. Measured at Zandvoort: a 136.760s lap
            // produced a 136.639s reference, the missing 0.121s sitting
            // entirely here. Anchor the start at the line itself.
            if (m_currentTrace.front().first > 0.0 && m_currentTrace.front().second > 0.0)
                m_currentTrace.insert(m_currentTrace.begin(), std::make_pair(0.0, 0.0));

            const bool traceSorted = SortedByDistance(m_currentTrace);
            // The saved PB only from a lap you drove all the way (review
            // F167/F252); the session best still takes it.
            if (countsForSavedPb
                && g_localPlayerLapClean.load(std::memory_order_relaxed)
                && (m_best.lapTimeSeconds < 0.0 || lastLap < m_best.lapTimeSeconds))
            {
                m_best.trace = m_currentTrace;
                m_best.lapTimeSeconds = lastLap;
                m_best.sorted = traceSorted;
                // Written by FlushIfDirty() on the save thread. Its own copy
                // of the trace and key, made before taking m_saveLock, so the
                // save thread only ever swaps it out.
                std::vector<std::pair<double, double>> saveTrace = m_best.trace;
                std::string saveKey = m_currentKey;
                AcquireSRWLockExclusive(&m_saveLock);
                m_saveTrace.swap(saveTrace);
                m_saveKey.swap(saveKey);
                m_saveLapTime = lastLap;
                m_bestLapDirty = true;
                ReleaseSRWLockExclusive(&m_saveLock);
                Persist::RequestLearned();
            }
            if (m_session.lapTimeSeconds < 0.0 || lastLap < m_session.lapTimeSeconds)
            {
                m_session.trace = m_currentTrace;
                m_session.lapTimeSeconds = lastLap;
                m_session.sorted = traceSorted;
            }
        }
        m_currentTrace.clear();
        m_lapValidityLatch = 2; // reset for next lap
        m_lapTainted = false;
        EnterCriticalSection(&m_lock);
        ++m_state.refEpoch;
        LeaveCriticalSection(&m_lock);
    }
    m_lastTotalLaps = player->mTotalLaps;

    // Folded in AFTER the lap-completion check: the line tick's flag and
    // state belong to the lap just starting, not the one just judged (F101).
    if (player->mCountLapFlag < m_lapValidityLatch)
        m_lapValidityLatch = player->mCountLapFlag;
    if (player->mControl != 0 || !info.mInRealtime || player->mInPits || player->mInGarageStall)
        m_lapTainted = true;

    // A trace only starts near the line, so a lap picked up mid-track (or
    // after a distance glitch) is never recorded (review F100/F97).
    const double startLimit = (std::min)(200.0, info.mLapDist > 1.0 ? info.mLapDist * 0.1 : 200.0);
    if (timeIntoLap >= 0.0 && timeIntoLap < 100000.0
        && ((m_currentTrace.empty() && player->mLapDist >= 0.0 && player->mLapDist <= startLimit)
            || (!m_currentTrace.empty() && player->mLapDist > m_lastLapDist + 0.5)))
    {
        m_currentTrace.emplace_back(player->mLapDist, timeIntoLap);
    }
    m_lastLapDist = player->mLapDist;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    double bestRefTime = 0.0, sessionRefTime = 0.0;
    const bool haveBest = m_best.InterpolateAt(player->mLapDist, bestRefTime);
    const bool haveSession = m_session.InterpolateAt(player->mLapDist, sessionRefTime);

    const double windowSeconds = g_lapDeltaConfig.trendWindowSeconds;

    EnterCriticalSection(&m_lock);
    m_state.countLapFlag = static_cast<int>(player->mCountLapFlag);
    m_state.lapValidityLatch = m_lapValidityLatch;
    m_state.pitState = static_cast<int>(player->mPitState);
    // (A PB reset requested during this tick keeps its cleared readout
    // until the next tick applies it.)
    if (haveBest && !m_resetPbRequested.load())
        m_best.UpdateTrend(timeIntoLap - bestRefTime, now, m_freq, windowSeconds, m_state.best);
    else
        m_state.best.valid = false;

    if (haveSession)
    {
        m_session.UpdateTrend(timeIntoLap - sessionRefTime, now, m_freq, windowSeconds, m_state.session);

        // Live growth: if THIS lap's delta outgrows the window it started
        // the lap with (reset to the floor at the previous lap's
        // completion, above), keep widening it in whole-second steps,
        // capped at BarWindowMaxSeconds, so the bar never clips mid-lap.
        if (g_lapDeltaConfig.autoBarWindowEnabled)
        {
            if (m_barWindowSeconds < g_lapDeltaConfig.maxBarSeconds)
                m_barWindowSeconds = g_lapDeltaConfig.maxBarSeconds;
            const double absDelta = fabs(m_state.session.deltaSeconds);
            while (absDelta > m_barWindowSeconds && m_barWindowSeconds < g_lapDeltaConfig.barWindowMaxSeconds)
                m_barWindowSeconds += 1.0;
            if (m_barWindowSeconds > g_lapDeltaConfig.barWindowMaxSeconds)
                m_barWindowSeconds = g_lapDeltaConfig.barWindowMaxSeconds;
        }
        else
        {
            m_barWindowSeconds = g_lapDeltaConfig.maxBarSeconds;
        }
        m_state.barWindowSeconds = m_barWindowSeconds;
    }
    else
        m_state.session.valid = false;
    LeaveCriticalSection(&m_lock);
}

bool LapDeltaStore::GetReferenceTimeAt(double lapDist, bool useSession, double& outTime) const
{
    const Reference& ref = useSession ? m_session : m_best;
    if (ref.lapTimeSeconds < 0.0)
        return false;
    return ref.InterpolateAt(lapDist, outTime);
}

LapDeltaStore::State LapDeltaStore::Read()
{
    State copy;
    if (!m_ready) return copy;
    EnterCriticalSection(&m_lock);
    copy = m_state;
    LeaveCriticalSection(&m_lock);
    return copy;
}

namespace
{
    // Number: plain green/red on the sign of the absolute delta -- no
    // brightness, that's the bar's job.
    void DeltaColor(double deltaSeconds, const LapDeltaConfig& cfg, float& r, float& g, float& b)
    {
        if (deltaSeconds <= 0.0) { r = cfg.numberAheadR; g = cfg.numberAheadG; b = cfg.numberAheadB; }
        else { r = cfg.numberBehindR; g = cfg.numberBehindG; b = cfg.numberBehindB; }
    }

    // Same colour as DeltaColor above, but EASED toward it rather than
    // snapping the instant the delta crosses zero -- a lap hovering right
    // at even pace would otherwise strobe green/red every time it ticked
    // past the sign boundary. One static current colour is enough: there is
    // exactly one PB row on screen.
    void FadedDeltaColor(double deltaSeconds, const LapDeltaConfig& cfg, float& r, float& g, float& b)
    {
        static float s_r = -1.0f, s_g = 0.0f, s_b = 0.0f; // s_r < 0 => not yet initialised
        static LARGE_INTEGER s_lastTime = {};

        float targetR, targetG, targetB;
        DeltaColor(deltaSeconds, cfg, targetR, targetG, targetB);

        LARGE_INTEGER freq, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);

        if (s_r < 0.0f)
        {
            s_r = targetR; s_g = targetG; s_b = targetB;
            s_lastTime = now;
        }

        double dt = 0.0;
        if (freq.QuadPart > 0 && s_lastTime.QuadPart > 0)
            dt = static_cast<double>(now.QuadPart - s_lastTime.QuadPart) / static_cast<double>(freq.QuadPart);
        if (dt < 0.0) dt = 0.0;
        s_lastTime = now;

        const double fadeSeconds = 0.2;
        float t = static_cast<float>(dt / fadeSeconds);
        if (t > 1.0f) t = 1.0f;
        if (t < 0.0f) t = 0.0f;

        s_r += (targetR - s_r) * t;
        s_g += (targetG - s_g) * t;
        s_b += (targetB - s_b) * t;

        r = s_r; g = s_g; b = s_b;
    }

    // Eases the delta bar's fill fraction toward its target rather than
    // snapping to it -- the bar otherwise visibly jumps in length every time
    // a new scoring sample lands (~5Hz), even though displayDelta itself is
    // already smoothly extrapolated between those updates: the CORRECTION
    // that arrives with each real sample still moved the fill instantly.
    // One static value is enough: there is exactly one delta bar.
    float SmoothedFillFraction(float target)
    {
        static float s_value = 0.0f;
        static bool s_initialised = false;
        static LARGE_INTEGER s_lastTime = {};

        LARGE_INTEGER freq, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);

        if (!s_initialised)
        {
            s_value = target;
            s_lastTime = now;
            s_initialised = true;
        }

        double dt = 0.0;
        if (freq.QuadPart > 0 && s_lastTime.QuadPart > 0)
            dt = static_cast<double>(now.QuadPart - s_lastTime.QuadPart) / static_cast<double>(freq.QuadPart);
        if (dt < 0.0) dt = 0.0;
        s_lastTime = now;

        const double smoothSeconds = 0.15;
        float t = static_cast<float>(dt / smoothSeconds);
        if (t > 1.0f) t = 1.0f;
        if (t < 0.0f) t = 0.0f;

        s_value += (target - s_value) * t;
        return s_value;
    }

    // Bar: hue follows the TREND, not the sign of the delta -- flat green
    // while moving more negative (gaining time), flat red while moving more
    // positive (losing time), independent of whether the delta itself is
    // still ahead or behind (bleeding back an advantage shows red
    // immediately). No brightness scale -- one colour each way.
    // Colour of the delta bar, decided from THE VALUE BEING DRAWN rather
    // than from LapDeltaStore's own trend sample.
    //
    // The old version used State::trendPerSecond, which is recomputed only
    // once per trendWindowSeconds (2s by default) as a two-point difference
    // across that whole window. So the colour could be a full two seconds
    // stale, and described a comparison against where the delta sat two
    // seconds ago rather than what the bar was visibly doing -- which is why
    // it so often disagreed with the bar in front of you.
    //
    // The rule now is simply: red while the delta is INCREASING, green while
    // it is DECREASING. On the right of centre that reads as "the bar is
    // growing, you are losing more"; on the left, a growing bar means the
    // delta is going more negative, i.e. gaining more, and correctly reads
    // green. One rule covers both sides.
    void BarTrendColor(double displayedDelta, unsigned epoch, bool frozen, const LapDeltaConfig& cfg,
                       float& r, float& g, float& b)
    {
        // Render-thread only, and there is exactly one bar, so plain statics
        // are enough -- no locking, same as TickBlink below.
        static const int kSamples = 240;      // ~2s of headroom at 120fps
        static double s_value[kSamples] = {};
        static double s_time[kSamples] = {};
        static int s_count = 0;
        static int s_next = 0;
        static bool s_lastWasLosing = false;  // held through the deadband

        // The value jumped for a non-driving reason (lap done, reference
        // changed, line hold started/ended): drop the history so the jump
        // is not read as time lost or gained (review F104).
        static unsigned s_epoch = 0;
        static bool s_frozen = false;
        if (epoch != s_epoch || frozen != s_frozen)
        {
            s_epoch = epoch;
            s_frozen = frozen;
            s_count = 0;
            s_next = 0;
        }

        LARGE_INTEGER freq, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        const double t = (freq.QuadPart > 0)
                       ? static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart)
                       : 0.0;

        double window = cfg.colorWindowSeconds;
        if (window < 0.05) window = 0.05;

        // Oldest sample still inside the window; falls back to the oldest
        // held sample while the history is shorter than the window (the
        // first moments of a lap).
        double past = displayedDelta;
        bool havePast = false;
        double bestAge = -1.0;
        for (int i = 0; i < s_count; ++i)
        {
            const double age = t - s_time[i];
            if (age < 0.0 || age > window * 3.0) continue;
            // Prefer the sample closest to exactly one window old.
            const double score = (age >= window) ? (age - window) : (window - age) * 2.0;
            if (bestAge < 0.0 || score < bestAge)
            {
                bestAge = score;
                past = s_value[i];
                havePast = true;
            }
        }

        s_value[s_next] = displayedDelta;
        s_time[s_next] = t;
        s_next = (s_next + 1) % kSamples;
        if (s_count < kSamples) ++s_count;

        if (havePast)
        {
            const double change = displayedDelta - past;
            if (change > cfg.colorDeadbandSeconds) s_lastWasLosing = true;
            else if (change < -cfg.colorDeadbandSeconds) s_lastWasLosing = false;
            // Inside the deadband the previous colour is held rather than
            // flipping on noise.
        }

        // Ease the OUTPUT colour toward whichever the trend decided above,
        // rather than snapping the instant it flips -- a hard cut reads as a
        // strobe if the trend flickers right at the deadband boundary; a
        // crossfade instead reads as one colour smoothly becoming the other.
        static float s_curR = -1.0f, s_curG = 0.0f, s_curB = 0.0f; // s_curR < 0 => not yet initialised
        static LARGE_INTEGER s_fadeLastTime = {};

        float targetR, targetG, targetB;
        if (s_lastWasLosing) { targetR = cfg.losingR; targetG = cfg.losingG; targetB = cfg.losingB; }
        else { targetR = cfg.gainingR; targetG = cfg.gainingG; targetB = cfg.gainingB; }

        if (s_curR < 0.0f)
        {
            s_curR = targetR; s_curG = targetG; s_curB = targetB;
            s_fadeLastTime = now;
        }

        double fadeDt = 0.0;
        if (freq.QuadPart > 0 && s_fadeLastTime.QuadPart > 0)
            fadeDt = static_cast<double>(now.QuadPart - s_fadeLastTime.QuadPart) / static_cast<double>(freq.QuadPart);
        if (fadeDt < 0.0) fadeDt = 0.0;
        s_fadeLastTime = now;

        const double fadeSeconds = (cfg.barColorFadeSeconds > 0.01f) ? cfg.barColorFadeSeconds : 0.01f;
        float ft = static_cast<float>(fadeDt / fadeSeconds);
        if (ft > 1.0f) ft = 1.0f;
        if (ft < 0.0f) ft = 0.0f;

        s_curR += (targetR - s_curR) * ft;
        s_curG += (targetG - s_curG) * ft;
        s_curB += (targetB - s_curB) * ft;

        r = s_curR; g = s_curG; b = s_curB;
    }

    // Wall-clock on/off blink phase, 0..1 over one period -- same technique
    // as GearDisplay.cpp's own TickBlink (kept separate since anonymous-
    // namespace helpers don't cross translation units).
    float TickBlink(double periodSeconds)
    {
        static float s_phase = 0.0f;
        static LARGE_INTEGER s_lastTime = {};
        static LARGE_INTEGER s_freq = {};
        if (s_freq.QuadPart == 0)
            QueryPerformanceFrequency(&s_freq);

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = 0.0;
        if (s_lastTime.QuadPart != 0 && s_freq.QuadPart != 0)
        {
            dt = static_cast<double>(now.QuadPart - s_lastTime.QuadPart) / static_cast<double>(s_freq.QuadPart);
            if (dt < 0.0 || dt > 1.0) dt = 0.0;
        }
        s_lastTime = now;

        if (periodSeconds < 0.05) periodSeconds = 0.05;
        s_phase = fmodf(s_phase + static_cast<float>(dt / periodSeconds), 1.0f);
        return s_phase;
    }
}

float LapDelta::BarAspectRatio()
{
    // cfg.barHeightPx is defined at the 2160p-tall reference (scaled by
    // screenHeight/2160 elsewhere), while cfg.barWidthFraction is a plain
    // fraction of screenWidth -- so at the matching 3840-wide 16:9
    // reference frame, height/width == barHeightPx / (3840 * barWidthFraction).
    const float widthFraction = g_lapDeltaConfig.barWidthFraction;
    if (widthFraction <= 0.0f) return 0.0f;
    return g_lapDeltaConfig.barHeightPx / (3840.0f * widthFraction);
}

bool LapDelta::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    // Medium tier (~25Hz): the SB/PB delta only changes meaningfully a few
    // times a second, so skip the rebuild on frames not due and replay the
    // last one. Bypassed in layout mode so dragging/resizing stays smooth.
    static RenderThrottle s_throttle(0.04);
    if (!Layout::Active() && s_throttle.TryReplay(renderer))
        return true;
    RenderThrottle::Capture throttleCapture(s_throttle, renderer);

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

    const LapDeltaConfig& cfg = g_lapDeltaConfig;
    if (!cfg.enabled)
        return false;

    const LapDeltaStore::State state = g_lapDelta.Read();
    // No early-return on "no reference lap yet" -- the DRS indicator below
    // can still be relevant (e.g. the very first lap of a session) even
    // when neither delta row has anything to show yet.

    static LARGE_INTEGER s_freq = {};
    if (s_freq.QuadPart == 0)
        QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // Distance-around-track (and so deltaSeconds) only updates at the ~5Hz
    // scoring rate; extrapolating by the already-computed trend between
    // updates moves the bar/number smoothly at full frame rate instead of
    // visibly stepping every scoring tick. Clamped short so a stalled
    // update (menu, replay) doesn't extrapolate off into nonsense.
    auto extrapolate = [&](const LapDeltaStore::DeltaState& s) -> double
    {
        if (s.updatedAt.QuadPart == 0 || s_freq.QuadPart == 0)
            return s.deltaSeconds;
        double elapsed = static_cast<double>(now.QuadPart - s.updatedAt.QuadPart) / static_cast<double>(s_freq.QuadPart);
        if (elapsed < 0.0) elapsed = 0.0;
        if (elapsed > 0.5) elapsed = 0.5;
        return s.deltaSeconds + s.trendPerSecond * elapsed;
    };

    // For the first few seconds of a new lap, keep showing the delta as it
    // stood at the line instead of the new lap's own (near-zero, and
    // meaningless this early) value. Held, not extrapolated -- a frozen
    // reading that kept drifting by its trend would not be the number that
    // was actually posted.
    bool freezeActive = false;
    if (state.haveLineFreeze && cfg.freezeAfterLineSeconds > 0.0f
        && state.lineCrossedAt.QuadPart != 0 && s_freq.QuadPart != 0)
    {
        const double sinceLine =
            static_cast<double>(now.QuadPart - state.lineCrossedAt.QuadPart) / static_cast<double>(s_freq.QuadPart);
        freezeActive = sinceLine >= 0.0 && sinceLine < cfg.freezeAfterLineSeconds;
    }

    // cfg.scale is the layout-mode corner-handle multiplier. Folding it
    // in HERE rather than applying it to the drawn box means every pixel
    // value downstream -- text height, padding, gaps, corner radii --
    // scales with it in exact proportion, because they all already
    // derive from this one value.
    const float uiScale = static_cast<float>(screenHeight) / 2160.0f * cfg.scale;
    const float barWidthPx = screenWidth * cfg.barWidthFraction * cfg.scale;
    const float barHeightPx = barWidthPx * BarAspectRatio();
    const float textHeightPx = cfg.textHeightPx * uiScale;
    const float rowGapPx = cfg.rowGapPx * uiScale;
    const float bgPaddingPx = cfg.bgPaddingPx * uiScale;
    const float bgCornerRadiusPx = cfg.bgCornerRadiusPx * uiScale;

    // Always dead centre horizontally -- the delta bar
    // (and the delta-speed strip, which takes its X straight from this one)
    // should never sit off-centre. Forced back to 0.5 every frame rather than
    // just defaulted there, so a horizontal drag in layout mode has no
    // lasting effect: the CenterXFraction below is a throwaway local, not
    // g_lapDeltaConfig's own field, so nothing ever persists a different X.
    // Vertical position is unaffected -- drag up/down still works normally.
    g_lapDeltaConfig.centerXFraction = 0.5f;
    const float centerX = screenWidth * 0.5f;
    static float s_dragOnlyCenterXFraction = 0.5f;
    s_dragOnlyCenterXFraction = 0.5f;
    const float barHalfW = screenWidth * cfg.barWidthFraction * 0.5f * cfg.scale;
    const float barHalfH = barHeightPx * 0.5f;

    // Session row: bar with the delta number drawn CENTRED ON the bar
    // itself, always plain white -- the bar's own fill colour (trend-based)
    // already carries the gaining/losing signal, so the number doesn't need
    // to repeat it via its own ahead/behind colour.
    float rowY = screenHeight * cfg.centerYFraction;
    // Registered at the anchor row rather than wherever the second row ends
    // up, so the grab box matches the value being dragged. X is the
    // throwaway local above (see comment), so the bar is still shown as
    // grabbable/resizable in layout mode but a horizontal drag snaps back
    // next frame instead of persisting.
    Layout::Register("Delta bar", "LapDelta", "CenterXFraction", "CenterYFraction",
                     &s_dragOnlyCenterXFraction, &g_lapDeltaConfig.centerYFraction,
                     centerX, rowY, barHalfW, barHalfH,
                     &g_lapDeltaConfig.scale, "Scale");
    bool drewAny = false;
    // No separate background panel for the Session row -- the bar itself IS
    // the background the text sits on (it's taller than the text by
    // config), so a second grey box behind it would just be a mismatched
    // double-background. barHalfH alone is the row's full visual height.
    float sessionBoxHalfH = 0.0f;

    const bool showSessionRow = Shortcuts::Visible(Shortcuts::LapDelta);
    const bool showBestRow = Shortcuts::Visible(Shortcuts::BestDelta);

    // No session best yet (the out lap, and the first flying lap, since the
    // first completion only lands at the END of it). Say so rather than
    // leaving a blank space with no explanation -- the absence is the thing
    // that needed explaining, not the timing. Also keeps the layout from
    // jumping when the real bar finally appears, and doubles as a diagnostic:
    // it shows exactly which lap the SB actually lands on.
    if (showSessionRow && !state.session.valid)
    {
        const char* msg = "no session best set yet";
        sessionBoxHalfH = barHalfH;
        renderer.AddFilledRoundedRect(centerX, rowY, barHalfW, barHalfH, bgCornerRadiusPx, 0.0f, 0.15f, 0.15f, 0.15f, 0.6f);
        renderer.AddText(centerX, rowY - textHeightPx * 0.5f, msg, textHeightPx * 0.7f,
                         0.7f, 0.7f, 0.7f, 1.0f, Renderer::HAlign::Center);
        rowY += barHalfH * 2.0f + rowGapPx;
        drewAny = true;
    }

    if (showSessionRow && state.session.valid)
    {
        const double displayDelta = freezeActive ? state.sessionAtLine : extrapolate(state.session);

        float barR, barG, barB;
        BarTrendColor(displayDelta, state.refEpoch, freezeActive, cfg, barR, barG, barB);

        char buf[24];
        sprintf_s(buf, "SB %+.3f", displayDelta);

        sessionBoxHalfH = barHalfH;

        renderer.AddFilledRoundedRect(centerX, rowY, barHalfW, barHalfH, bgCornerRadiusPx, 0.0f, 0.15f, 0.15f, 0.15f, 0.6f);

        // The bar's LENGTH is smoothed toward the target fraction rather
        // than snapping to it -- displayDelta already extrapolates smoothly
        // between the ~5Hz scoring updates, but the correction that lands
        // WITH each real update still moved the fill instantly, which read
        // as the bar visibly jumping in length every scoring tick.
        const double barWindowSeconds = (state.barWindowSeconds > 0.01) ? state.barWindowSeconds : cfg.maxBarSeconds;
        double fillFrac = displayDelta / barWindowSeconds;
        if (fillFrac > 1.0) fillFrac = 1.0;
        if (fillFrac < -1.0) fillFrac = -1.0;
        const float smoothedFillFrac = SmoothedFillFraction(static_cast<float>(fillFrac));
        const float fillHalfW = fabsf(smoothedFillFrac) * barHalfW * 0.5f;
        const float fillCenterX = centerX + (smoothedFillFrac > 0.0f ? 1.0f : -1.0f) * fillHalfW;
        if (fillHalfW > 0.5f)
        {
            // Rounded to match the same radius the track behind it uses, but
            // only clamped to at most the fill's own half-width -- otherwise
            // a fill that has barely started (a few px wide) would round
            // into a near-circle rather than a small rounded bar.
            const float fillCornerPx = (bgCornerRadiusPx < fillHalfW) ? bgCornerRadiusPx : fillHalfW;
            renderer.AddFilledRoundedRect(fillCenterX, rowY, fillHalfW, barHalfH, fillCornerPx, 0.0f, barR, barG, barB, 0.9f);
        }

        // Centre tick, so 0 is unambiguous even mid-fill.
        renderer.AddFilledRect(centerX, rowY, 1.5f, barHalfH, 0.0f, 1.0f, 1.0f, 1.0f, 0.8f);

        // Thin outline first (8 offset copies in a dark colour), then the
        // real text on top -- keeps it readable whether it lands over the
        // bar's plain grey background or its coloured fill, instead of
        // looking inconsistently half-and-half between the two.
        if (cfg.barNumberOutlineEnabled)
        {
            const float ot = cfg.barNumberOutlineThicknessPx * uiScale;
            const float textTopY = rowY - textHeightPx * 0.5f;
            static const float kOffsets[8][2] =
            {
                { -1, -1 }, { 0, -1 }, { 1, -1 },
                { -1,  0 },            { 1,  0 },
                { -1,  1 }, { 0,  1 }, { 1,  1 },
            };
            for (const auto& o : kOffsets)
            {
                renderer.AddText(centerX + o[0] * ot, textTopY + o[1] * ot, buf, textHeightPx,
                                 cfg.barNumberOutlineR, cfg.barNumberOutlineG, cfg.barNumberOutlineB,
                                 1.0f, Renderer::HAlign::Center);
            }
        }

        renderer.AddText(centerX, rowY - textHeightPx * 0.5f, buf, textHeightPx,
                         cfg.barNumberR, cfg.barNumberG, cfg.barNumberB, 1.0f, Renderer::HAlign::Center);

        drewAny = true;
    }

    // Best row: no bar at all -- just a plain number below the session row,
    // coloured green when ahead of the all-time best, red when behind.
    // In layout mode a placeholder is drawn with no PB yet, so the row can
    // be placed before it first appears (review F105).
    if (showBestRow && (state.best.valid || Layout::Preview()))
    {
        const double displayDelta = !state.best.valid ? 0.0
                                  : (freezeActive ? state.bestAtLine : extrapolate(state.best));

        float numR = 0.7f, numG = 0.7f, numB = 0.7f;
        char buf[24];
        if (state.best.valid)
        {
            FadedDeltaColor(displayDelta, cfg, numR, numG, numB);
            sprintf_s(buf, "PB %+.3f", displayDelta);
        }
        else
            strcpy_s(buf, "PB --.---");

        // Same text size as the SB number on the bar above -- both just use
        // uiScale (which already carries LapDeltaConfig::scale), with no
        // multiplier of the PB row's own. The PB and SB
        // text match, and the PB row's size follows the delta bar's
        // own resizing rather than having a handle of its own -- see the
        // Layout::Register call below, which no longer passes a scale.
        // LapDeltaConfig::bestScale is kept only so an existing INI with
        // that key still loads without error; it is no longer read here.
        const float bestTextPx = cfg.textHeightPx * uiScale;
        const float bestPadPx = cfg.bgPaddingPx * uiScale;
        const float bestCornerPx = cfg.bgCornerRadiusPx * uiScale;
        const float bestBoxHalfH = bestTextPx * 0.5f + bestPadPx;
        const float textW = renderer.MeasureText(buf, bestTextPx);

        // Resolve the sentinel exactly once, against the position this row
        // USED to inherit: stacked under the session row, or in its place at
        // the top if the session row did not draw. From here the PB row has
        // an anchor of its own and no longer tracks the session row at all,
        // which is the whole point -- it can be dragged anywhere.
        if (cfg.bestCenterYFraction < 0.0f || cfg.bestCenterXFraction < 0.0f)
        {
            const float legacyY = drewAny ? (rowY + sessionBoxHalfH + rowGapPx + bestBoxHalfH) : rowY;
            g_lapDeltaConfig.bestCenterXFraction = centerX / screenWidth;
            g_lapDeltaConfig.bestCenterYFraction = legacyY / screenHeight;
        }

        const float bestCenterX = screenWidth * cfg.bestCenterXFraction;
        const float bestCenterY = screenHeight * cfg.bestCenterYFraction;

        // Position only -- no scale/scaleKey, so this row has no resize
        // handle of its own in layout mode; it can still be dragged, but its
        // SIZE now only ever follows the delta bar's own scale above.
        Layout::Register("PB delta", "LapDelta", "BestCenterXFraction", "BestCenterYFraction",
                         &g_lapDeltaConfig.bestCenterXFraction, &g_lapDeltaConfig.bestCenterYFraction,
                         bestCenterX, bestCenterY, textW * 0.5f + bestPadPx, bestBoxHalfH);

        if (cfg.bgEnabled)
        {
            renderer.AddFilledRoundedRect(bestCenterX, bestCenterY, textW * 0.5f + bestPadPx, bestBoxHalfH,
                                          bestCornerPx, 0.0f, cfg.bgR, cfg.bgG, cfg.bgB, cfg.bgA);
        }

        renderer.AddText(bestCenterX, bestCenterY - bestTextPx * 0.5f, buf, bestTextPx,
                         numR, numG, numB, 1.0f, Renderer::HAlign::Center);

        // rowY is left where the session row put it. The PB row no longer
        // occupies space in that stack, so nothing below it should shift
        // depending on whether the PB drew.
        drewAny = true;
    }
    else if (drewAny)
    {
        rowY += sessionBoxHalfH;
    }

    rowY += rowGapPx;

    // Live rF2 lap-validity readout -- see LapDeltaConfig::showValidityDebug.
    // Independent of the session/best rows above: shown whenever there is
    // scoring data at all, since the whole point is to explain WHY neither
    // row above might ever populate.
    if (cfg.showValidityDebug && state.countLapFlag >= 0)
    {
        static const char* kPitStateNames[] = { "none", "request", "entering", "stopped", "exiting" };
        const char* pitName = (state.pitState >= 0 && state.pitState <= 4) ? kPitStateNames[state.pitState] : "?";

        char buf[96];
        sprintf_s(buf, "lap flag %d (latched %d)  pit: %s",
                  state.countLapFlag, state.lapValidityLatch, pitName);

        // Green once this lap has latched a countable flag (2); red while it
        // is still short of that -- same rule LapDelta.cpp's own
        // countsForTime check uses at lap completion (LapDelta.cpp), just
        // shown live instead of only in the log afterward.
        const bool countable = (state.lapValidityLatch == 2);
        const float debugTextPx = cfg.validityDebugTextHeightPx * uiScale;
        const float debugGapPx = cfg.validityDebugGapPx * uiScale;

        renderer.AddText(centerX, rowY, buf, debugTextPx,
                         countable ? 0.4f : 1.0f, countable ? 1.0f : 0.4f, 0.4f, 1.0f, Renderer::HAlign::Center);
        rowY += debugTextPx + debugGapPx;
        drewAny = true;
    }

    // The DRS sign is its own panel now: LapDelta::DrawDrs (review F51).

    return drewAny;
}

// The DRS sign as its own panel (review F51): drawn whenever DRS is relevant,
// whether or not the delta bars are on, enabled or hidden. Own position and
// own scale (DrsScale, no longer multiplied by the delta bar's Scale).
bool LapDelta::DrawDrs(Renderer& renderer, int screenWidth, int screenHeight)
{
    const LapDeltaConfig& cfg = g_lapDeltaConfig;
    if (!cfg.drsIndicatorEnabled)
        return false;
    const DrsCueStore::State drs = g_drsCue.Read();
    // Normally gated on real DRS availability -- but then the box would
    // never register on a car/track with no DRS, so it could never be
    // dragged or resized in layout mode. Forced visible (with its default
    // "OPEN DRS" text) while arranging the HUD.
    if (!drs.showIndicator && !Layout::Preview())
        return false;
    // 10 Hz rebuild, replayed in between (2026-11-24 throttle pass).
    static RenderThrottle s_drsThrottle(0.1);
    if (!Layout::Active() && s_drsThrottle.TryReplay(renderer))
        return true;
    RenderThrottle::Capture throttleCapture(s_drsThrottle, renderer);

    struct TextBgGuard
    {
        Renderer& r; bool prev;
        explicit TextBgGuard(Renderer& rr) : r(rr), prev(rr.TextBackgroundEnabled()) { r.SetTextBackground(false); }
        ~TextBgGuard() { r.SetTextBackground(prev); }
    } textBgGuard(renderer);

    const float drsScale = static_cast<float>(screenHeight) / 2160.0f * cfg.drsScale;
    const float textPx = cfg.drsIndicatorTextHeightPx * drsScale;
    const float paddingPx = cfg.drsIndicatorPaddingPx * drsScale;
    const float cornerPx = cfg.drsIndicatorBgCornerRadiusPx * drsScale;

    const char* text = drs.drsOpen ? "DRS ENGAGED" : "OPEN DRS";
    const float textW = renderer.MeasureText(text, textPx);
    const float halfW = textW * 0.5f + paddingPx;
    const float halfH = textPx * 0.5f + paddingPx;
    // Never placed yet: just under where the delta bar sits, centred.
    if (cfg.drsCenterYFraction < 0.0f || cfg.drsCenterXFraction < 0.0f)
    {
        const float barHalfH = screenWidth * cfg.barWidthFraction * cfg.scale * BarAspectRatio() * 0.5f;
        g_lapDeltaConfig.drsCenterXFraction = 0.5f;
        g_lapDeltaConfig.drsCenterYFraction = (screenHeight * cfg.centerYFraction + barHalfH
                                               + cfg.drsIndicatorGapPx * drsScale + halfH) / screenHeight;
    }
    const float boxCenterX = screenWidth * cfg.drsCenterXFraction;
    const float boxCenterY = screenHeight * cfg.drsCenterYFraction;

    Layout::Register("DRS", "LapDelta", "DrsCenterXFraction", "DrsCenterYFraction",
                     &g_lapDeltaConfig.drsCenterXFraction, &g_lapDeltaConfig.drsCenterYFraction,
                     boxCenterX, boxCenterY, halfW, halfH,
                     &g_lapDeltaConfig.drsScale, "DrsScale");

    // Flash while it's legal to open but not yet engaged, to prompt pressing
    // the button; solid once engaged, since it is confirming state.
    float fade = 1.0f;
    if (!drs.drsOpen)
    {
        const float phase = TickBlink(cfg.drsIndicatorFlashPeriodSeconds);
        fade = (phase < 0.5f) ? 1.0f : 0.15f;
    }

    renderer.AddFilledRoundedRect(boxCenterX, boxCenterY, halfW, halfH, cornerPx, 0.0f,
                                  cfg.drsIndicatorBgR, cfg.drsIndicatorBgG, cfg.drsIndicatorBgB, cfg.drsIndicatorBgA * fade);
    renderer.AddText(boxCenterX, boxCenterY - textPx * 0.5f, text, textPx,
                     cfg.drsIndicatorTextR, cfg.drsIndicatorTextG, cfg.drsIndicatorTextB, fade, Renderer::HAlign::Center);
    return true;
}
