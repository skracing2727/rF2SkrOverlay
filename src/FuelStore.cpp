#include "FuelStore.h"
#include "InternalsPlugin.hpp"
#include "Log.h"
#include "RealtimeState.h"
#include "Persist.h"
#include "StoreFile.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

FuelStore g_fuelStore;

namespace
{
    // Same DLL-relative-path resolution used elsewhere (Config.cpp,
    // LapDelta.cpp, GearDisplay.cpp), so the saved fuel-stats files live next
    // to the INI.
    void GetFuelStatsDir(char* out, size_t outSize)
    {
        char dllPath[MAX_PATH] = {};
        HMODULE hModule = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCSTR>(&GetFuelStatsDir), &hModule);
        GetModuleFileNameA(hModule, dllPath, MAX_PATH);

        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';

        sprintf_s(out, outSize, "%sFuelStats", dllPath);
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

    // What a car+track's saved file holds, read on the telemetry thread on
    // a car/track change BEFORE OnTelemetry takes m_lock (Draw reads the
    // store through that lock several times a frame).
    struct LoadedStats
    {
        double seedNorm = 0.0;        // 0 = no usable saved rate
        double lastLapTime = 0.0;     // 0 = none
    };

    // legacyKey: the same car+track under the old per-CLASS key, read once
    // (never written) when the car has no file of its own yet.
    LoadedStats ReadFuelStats(const std::string& key, const std::string& legacyKey,
                              const char* vehicleName, const char* trackName)
    {
        LoadedStats out;
        char dir[MAX_PATH];
        GetFuelStatsDir(dir, sizeof(dir));
        char path[MAX_PATH];
        sprintf_s(path, "%s\\%s.txt", dir, key.c_str());
        if (!StoreFile::Exists(path) && !legacyKey.empty() && legacyKey != key)
        {
            char legacy[MAX_PATH];
            sprintf_s(legacy, "%s\\%s.txt", dir, legacyKey.c_str());
            if (StoreFile::Exists(legacy))
            {
                strcpy_s(path, legacy);
                Log::Write("FuelStore: no fuel stats yet for %s -- seeding from old class file %s", key.c_str(), legacyKey.c_str());
            }
        }

        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") != 0 || !f)
        {
            Log::Write("FuelStore: no saved fuel stats yet for %s / %s", vehicleName, trackName);
            return out;
        }
        char line[64];
        double savedRate = 0.0;
        if (fgets(line, sizeof(line), f))
            savedRate = atof(line);
        // Second line: last lap time (see WriteFuelStats).
        if (fgets(line, sizeof(line), f))
        {
            const double lapTime = atof(line);
            if (lapTime > 5.0 && lapTime < 100000.0)
                out.lastLapTime = lapTime;
        }
        // Third line marks the rate as normalised to a 1x fuel multiplier.
        // Files without it were saved before the multiplier was tracked, so the
        // rate in them may have been learned under any multiplier (one session
        // at 6x burn poisoned a whole car+track) -- it is ignored rather than
        // trusted. The lap time above is still used.
        bool normalised = false;
        if (fgets(line, sizeof(line), f))
            normalised = (strncmp(line, "norm", 4) == 0);
        if (normalised && savedRate > 0.01)
            out.seedNorm = savedRate;
        fclose(f);
        Log::Write("FuelStore: loaded %.3f L/lap @1x (%s), %.3fs/lap for %s / %s",
                   out.seedNorm, normalised ? "normalised" : "legacy file, rate ignored", out.lastLapTime, vehicleName, trackName);
        return out;
    }
}

void FuelStore::Init()
{
    if (m_ready) return;
    InitializeCriticalSection(&m_lock);
    m_ready = true;
}

void FuelStore::Clear()
{
    if (!m_ready) return;
    EnterCriticalSection(&m_lock);
    // Callers flush (Persist::FlushAllNow) before Clear() -- see Plugin.cpp.
    // A rate still waiting regardless (that flush timed out) is stashed with
    // its key before the state it is worked out from is wiped below, rather
    // than dropped as it used to be (when the save read the live key).
    if (m_dirty)
    {
        m_stash = SnapshotForSaveLocked();
        m_dirty = false;
    }
    m_state = State{};
    m_lastLapNumber = -1;
    m_haveLapStartFuel = false;
    m_hasLearnedValue = false;
    m_learnedFuelPerLap = 0.0;
    m_fuelHistoryCount = 0;
    m_fuelHistoryNext = 0;
    m_lastLapTimeSeconds = 0.0;
    m_savedPaceSeconds = 0.0;
    m_paceCount = 0;
    m_paceNext = 0;
    m_rejectCount = 0;
    m_leaderLastTotalLaps = -1;
    m_leaderLapHistoryCount = 0;
    m_leaderLapHistoryNext = 0;
    m_leaderAvgLapTime = -1.0;
    m_leaderLapTainted = false;
    m_leaderBestLapTime = -1.0;
    m_leaderLapUnderYellow = false;
    m_bestCleanLapSeconds = -1.0;
    m_lapUnderYellow = false;
    m_haveEndChoice = false;
    m_timeEndsFirst = false;
    // Every stint field, not just the start time (review F3): a restart
    // from on track used to keep the previous session's start fuel.
    m_wasInPits = false;
    m_stintStartLaps = -1;
    m_stintStartFuel = -1.0;
    m_stintStartET = -1.0;
    m_pitStopped = false;
    m_pitStillSeconds = 0.0;
    m_pitStopsAtEntry = 0;
    m_pitFuelAtEntry = 0.0;
    m_lastScoringET = -1.0;
    m_historySeeded = false;
    m_seedNorm = 0.0;
    m_telemLapTainted = false;
    m_scoringLapTainted = false;
    m_havePrevFuel = false;
    m_haveConsumedSample = false;
    m_haveScoringVerdict = false;
    m_lapValidityLatch = 2;
    m_scoringLastTotalLaps = -1;
    LeaveCriticalSection(&m_lock);
    // The key is forgotten too, so the next session reloads the file even
    // for the same car and track. Only OnTelemetry touches m_key (so it can
    // compare it without the lock), so it is told to rather than the key
    // being cleared from here.
    m_keysInvalidated.store(true);
}

void FuelStore::FlushIfDirty()
{
    if (!m_ready) return;
    // Save thread, any time. Work out what to write under the lock, write
    // after letting go: OnTelemetry and Draw's Read() take m_lock.
    SaveSnapshot stashed, current;
    EnterCriticalSection(&m_lock);
    if (m_stash.valid)
    {
        stashed = m_stash;
        m_stash = SaveSnapshot{};
    }
    if (m_dirty)
    {
        current = SnapshotForSaveLocked();
        m_dirty = false;
    }
    LeaveCriticalSection(&m_lock);
    const bool stashOk = WriteFuelStats(stashed);
    const bool currentOk = WriteFuelStats(current);
    // A failed write (file locked by an editor or antivirus, say) is kept
    // for the next save pass instead of being lost (review F278). Only put
    // back if nothing newer has replaced it meanwhile.
    if (!stashOk || !currentOk)
    {
        EnterCriticalSection(&m_lock);
        if (!stashOk && !m_stash.valid)
            m_stash = stashed;
        if (!currentOk)
        {
            if (current.key == m_key)
                m_dirty = true;
            else if (!m_stash.valid)
                m_stash = current;
        }
        LeaveCriticalSection(&m_lock);
    }
}

FuelStore::SaveSnapshot FuelStore::SnapshotForSaveLocked() const
{
    // Caller holds m_lock.
    SaveSnapshot s;
    s.valid = !m_key.empty();
    s.key = m_key;
    // The rate is written normalised to 1x fuel: this session's learned
    // rate divided by its multiplier. If nothing real was learned this
    // session (still only the seeded value) or the multiplier is unknown,
    // the previously saved normalised value is kept as it was.
    const int mult = g_fuelMult.load();
    s.norm = m_seedNorm;
    if (m_hasLearnedValue && !m_historySeeded && mult > 0)
        s.norm = m_learnedFuelPerLap / mult;
    // The clean-lap pace (not the raw last lap, which may be an in-lap or
    // the lap with the stop -- review F1), so next session's "Fuel Time
    // Left" seeds from a racing lap.
    s.lapTime = PaceLocked();
    return s;
}

bool FuelStore::WriteFuelStats(const SaveSnapshot& s)
{
    if (!s.valid)
        return true;
    char dir[MAX_PATH];
    GetFuelStatsDir(dir, sizeof(dir));
    CreateDirectoryA(dir, nullptr); // no-op (ERROR_ALREADY_EXISTS) if it's already there

    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s.txt", dir, s.key.c_str());

    char tmp[StoreFile::kTmpPathSize];
    FILE* f = StoreFile::Begin(path, tmp, sizeof(tmp), "w");
    if (!f)
    {
        Log::Write("FuelStore: failed to save fuel stats to %s", path);
        return false;
    }
    fprintf(f, "%.4f\n", s.norm);
    fprintf(f, "%.3f\n", s.lapTime);
    fprintf(f, "norm\n");
    if (!StoreFile::Commit(f, tmp, path))
    {
        Log::Write("FuelStore: failed to save fuel stats to %s", path);
        return false;
    }
    return true;
}

double FuelStore::MedianOfHistory() const
{
    double v[kFuelHistorySize];
    const int n = m_fuelHistoryCount;
    for (int i = 0; i < n; ++i) v[i] = m_fuelHistory[i];
    std::sort(v, v + n);
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

void FuelStore::ApplyLearnedLap(double consumed)
{
    if (m_historySeeded)
    {
        // First real lap this session: drop the persisted stand-in entirely.
        m_fuelHistoryCount = 0;
        m_fuelHistoryNext = 0;
        m_historySeeded = false;
    }
    else if (m_fuelHistoryCount >= 3)
    {
        // Outlier guard against this session's own laps: a lap far from the
        // median (cool-down / safety-car lap, heavy lift-and-coast, or a
        // fuel glitch) is not representative consumption. Band tightened
        // from 0.6-1.6x (review F5).
        const double med = MedianOfHistory();
        if (med > 0.01 && (consumed < 0.7 * med || consumed > 1.4 * med))
        {
            // Several rejections in a row on the same side that agree with
            // each other mean the history itself is the odd one out (e.g.
            // learned under a safety car) -- restart it from them (review
            // F385); otherwise it could reject every lap for the session.
            const bool high = consumed > med;
            if (m_rejectCount > 0 && high != m_rejectHigh)
                m_rejectCount = 0;
            m_rejectHigh = high;
            m_rejected[m_rejectCount++] = consumed;
            if (m_rejectCount >= kRejectRestart)
            {
                double lo = m_rejected[0], hi = m_rejected[0];
                for (int i = 1; i < m_rejectCount; ++i)
                {
                    lo = (std::min)(lo, m_rejected[i]);
                    hi = (std::max)(hi, m_rejected[i]);
                }
                if (lo > 0.01 && hi <= 1.15 * lo)
                {
                    Log::Write("FuelStore: %d laps in a row outside the history (median %.3f L) -- restarting it from them",
                               m_rejectCount, med);
                    for (int i = 0; i < m_rejectCount; ++i)
                        m_fuelHistory[i] = m_rejected[i];
                    m_fuelHistoryCount = m_rejectCount;
                    m_fuelHistoryNext = m_rejectCount % kFuelHistorySize;
                    m_rejectCount = 0;
                    RecomputeFuelPerLap();
                    m_hasLearnedValue = true;
                    m_dirty = true;
                    Persist::RequestLearned();
                    return;
                }
                // They disagree: keep only the newest and keep counting.
                m_rejected[0] = consumed;
                m_rejectCount = 1;
            }
            Log::Write("FuelStore: rejected outlier lap %.3f L (median %.3f L)", consumed, med);
            return;
        }
    }
    m_rejectCount = 0;
    m_fuelHistory[m_fuelHistoryNext] = consumed;
    m_fuelHistoryNext = (m_fuelHistoryNext + 1) % kFuelHistorySize;
    if (m_fuelHistoryCount < kFuelHistorySize) ++m_fuelHistoryCount;
    RecomputeFuelPerLap();
    m_hasLearnedValue = true;
    // Written by FlushIfDirty() on the save thread. Learned at the line (or
    // a tick after it, when the two halves of the handoff meet), so the save
    // pass the line itself asked for may already have run -- ask again.
    m_dirty = true;
    Persist::RequestLearned();
}

void FuelStore::RecomputeFuelPerLap()
{
    // Median rather than mean: one odd lap cannot drag it.
    m_learnedFuelPerLap = (m_fuelHistoryCount == 0) ? 0.0 : MedianOfHistory();
}

double FuelStore::PaceLocked() const
{
    // Caller holds m_lock. Median of this session's clean laps; before
    // there are any, the saved pace; failing that, the raw last lap.
    if (m_paceCount > 0)
    {
        double v[kPaceHistorySize];
        const int n = m_paceCount;
        for (int i = 0; i < n; ++i) v[i] = m_paceHistory[i];
        std::sort(v, v + n);
        return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    }
    if (m_savedPaceSeconds > 0.0)
        return m_savedPaceSeconds;
    return m_lastLapTimeSeconds;
}

void FuelStore::TrySeedFromSaved()
{
    // Caller holds m_lock.
    if (m_seedNorm <= 0.0 || m_hasLearnedValue)
        return;
    const int mult = g_fuelMult.load();
    if (mult <= 0)
        return; // unknown yet (or fuel usage off) -- wait rather than guess
    const double seeded = m_seedNorm * mult;
    for (int i = 0; i < kFuelHistorySize; ++i)
        m_fuelHistory[i] = seeded;
    m_fuelHistoryCount = kFuelHistorySize;
    m_learnedFuelPerLap = seeded;
    m_hasLearnedValue = true;
    m_historySeeded = true;
    Log::Write("FuelStore: seeded %.3f L/lap (%.3f @1x x %dx fuel)", seeded, m_seedNorm, mult);
}

void FuelStore::OnTelemetry(const TelemInfoV01& info)
{
    if (!m_ready) return;

    // Car/track change. The raw names are compared with last tick's copies
    // first, so an ordinary tick builds no strings; the key is only
    // sanitized, and the saved file read, on an actual change -- and the
    // read happens here, BEFORE m_lock, which Draw takes through Read()
    // several times a frame. m_key is only ever written by this thread
    // (under the lock, for FlushIfDirty), so it can be compared here safely.
    // CAR key (class kept for seeding), shared from the scoring thread --
    // see RealtimeState.h.
    char vehKey[64];
    VehicleKey::Get(vehKey, sizeof(vehKey));
    const bool rekeyAfterClear = m_keysInvalidated.exchange(false);
    bool keyChanged = false;
    std::string newKey;
    LoadedStats loaded;
    char profilePath[MAX_PATH] = "";
    char legacyProfilePath[MAX_PATH] = "";
    if (rekeyAfterClear || !m_haveRawNames
        || strncmp(vehKey, m_rawVehKey, sizeof(m_rawVehKey)) != 0
        || strncmp(info.mTrackName, m_rawTrackName, sizeof(m_rawTrackName)) != 0)
    {
        memcpy(m_rawVehKey, vehKey, sizeof(m_rawVehKey));
        memcpy(m_rawTrackName, info.mTrackName, sizeof(m_rawTrackName));
        m_haveRawNames = true;
        const std::string trackKey = Sanitize(info.mTrackName, sizeof(info.mTrackName));
        newKey = Sanitize(vehKey, sizeof(vehKey)) + "__" + trackKey;
        keyChanged = rekeyAfterClear || newKey != m_key;
        if (keyChanged)
        {
            char classKey[64];
            VehicleKey::GetClass(classKey, sizeof(classKey));
            const std::string legacyKey = Sanitize(classKey, sizeof(classKey)) + "__" + trackKey;
            loaded = ReadFuelStats(newKey, legacyKey, vehKey, info.mTrackName);
            char dir[MAX_PATH];
            GetFuelStatsDir(dir, sizeof(dir));
            sprintf_s(profilePath, "%s\\%s_profile.txt", dir, newKey.c_str());
            if (legacyKey != newKey)
                sprintf_s(legacyProfilePath, "%s\\%s_profile.txt", dir, legacyKey.c_str());
        }
    }

    EnterCriticalSection(&m_lock);

    if (keyChanged)
    {
        // A rate learned for the car/track being left and not saved yet
        // still goes to its own file.
        if (m_dirty)
        {
            m_stash = SnapshotForSaveLocked();
            m_dirty = false;
        }
        m_key = newKey;
        m_learnedFuelPerLap = 0.0;
        m_hasLearnedValue = false;
        m_fuelHistoryCount = 0;
        m_fuelHistoryNext = 0;
        m_historySeeded = false;
        m_seedNorm = loaded.seedNorm;
        m_lastLapTimeSeconds = 0.0;
        m_savedPaceSeconds = loaded.lastLapTime;
        m_paceCount = 0;
        m_paceNext = 0;
        m_bestCleanLapSeconds = -1.0;
        m_rejectCount = 0;
        strcpy_s(m_state.profilePath, profilePath);
        strcpy_s(m_state.legacyProfilePath, legacyProfilePath);
        m_lastLapNumber = info.mLapNumber;
        m_haveLapStartFuel = true;
        m_fuelAtLapStart = info.mFuel;
        m_prevElapsedTime = info.mElapsedTime;
        m_prevLapStartET = info.mLapStartET;
        // Joined partway through a lap: its start fuel is not the lap's.
        m_telemLapTainted = true;
        m_havePrevFuel = false;
    }

    // Any sudden fuel step between two telemetry samples (~100Hz; real burn
    // is hundredths of a litre per sample) is a refuel or the setup's fuel
    // load being applied -- re-baseline and exclude the lap.
    if (m_havePrevFuel && fabs(info.mFuel - m_prevFuel) > 0.4)
    {
        m_fuelAtLapStart = info.mFuel;
        m_telemLapTainted = true;
    }
    m_prevFuel = info.mFuel;
    m_havePrevFuel = true;

    // A fuel INCREASE means a refuel (pit stop) happened -- treat it as a
    // fresh baseline rather than letting it show up as negative
    // consumption, whether or not it lines up with a lap boundary.
    if (info.mFuel > m_fuelAtLapStart + 0.5)
    {
        m_fuelAtLapStart = info.mFuel;
        m_telemLapTainted = true;
    }

    if (info.mLapNumber > m_lastLapNumber && m_haveLapStartFuel)
    {
        const double consumed = m_fuelAtLapStart - info.mFuel;
        const double lapTime = m_prevElapsedTime - m_prevLapStartET;
        const bool sampleOk = consumed > 0.05 && lapTime > 5.0 && lapTime < 100000.0;
        // Raw last lap, the pace fallback before any clean lap exists -- but
        // not a lap with pit-lane/garage time or a refuel in it (2026-11-24:
        // sitting in the box made it ~660 s, so "Fuel lasts" read 13755:04
        // and "Stint target" 0.0 laps). Such a lap leaves the old value.
        if (sampleOk && !m_telemLapTainted && !m_scoringLapTainted)
            m_lastLapTimeSeconds = lapTime; // the pace is kept separately (review F1)

        // Whether this lap's consumption gets folded into the learned
        // average depends on rF2's own lap-validity flag and the lap's pace,
        // both known only on the scoring thread -- see the header. Every
        // crossing posts exactly one item, even with no usable sample, and
        // the two halves only pair when their crossing times match, so a
        // lap skipped on one side can no longer leave the pairing a lap out
        // of step (review F5).
        const double crossET = info.mElapsedTime;
        if (m_haveScoringVerdict && fabs(m_pendingVerdictET - crossET) < kPairWindowSeconds)
        {
            if (sampleOk && m_pendingVerdictUsable && !m_telemLapTainted)
                ApplyLearnedLap(consumed);
            m_haveScoringVerdict = false;
        }
        else
        {
            m_haveScoringVerdict = false; // stale: from a crossing this side never saw
            m_pendingConsumed = consumed;
            m_pendingConsumedTainted = m_telemLapTainted || !sampleOk;
            m_pendingConsumedET = crossET;
            m_haveConsumedSample = true;
        }
        m_fuelAtLapStart = info.mFuel;
        m_telemLapTainted = false;
    }
    m_lastLapNumber = info.mLapNumber;
    m_prevElapsedTime = info.mElapsedTime;
    m_prevLapStartET = info.mLapStartET;

    TrySeedFromSaved();

    m_state.valid = true;
    m_state.fuelLoad = info.mFuel;
    m_state.fuelCapacity = info.mFuelCapacity;
    m_state.fuelPerLap = m_learnedFuelPerLap;
    m_state.fuelAtLapStart = m_fuelAtLapStart;
    const double pace = PaceLocked();
    m_state.lastLapTimeSeconds = pace;
    m_state.paceLapSeconds = pace;
    m_state.rawLastLapSeconds = m_lastLapTimeSeconds;
    m_state.telemLapNumber = info.mLapNumber;
    m_state.timeLeftSeconds = (m_hasLearnedValue && m_learnedFuelPerLap > 0.01 && pace > 0.0)
        ? (info.mFuel / m_learnedFuelPerLap) * pace
        : -1.0;

    LeaveCriticalSection(&m_lock);
}

namespace
{
    // Laps completed plus progress through the lap in progress. The lap
    // distance is clamped to [0, 1) of a lap (review F9): slightly negative
    // near the line or in the pit lane used to wrap to "lap almost done".
    double LapProgress(const VehicleScoringInfoV01& v, double trackLength)
    {
        double frac = 0.0;
        if (trackLength > 0.1)
        {
            double d = v.mLapDist;
            if (d >= trackLength) d -= trackLength;
            frac = (std::min)((std::max)(d / trackLength, 0.0), 0.9999);
        }
        return static_cast<double>(v.mTotalLaps) + frac;
    }
}

void FuelStore::OnScoring(const ScoringInfoV01& info)
{
    if (!m_ready) return;

    const VehicleScoringInfoV01* player = nullptr;
    const VehicleScoringInfoV01* leader = nullptr;
    for (long i = 0; i < info.mNumVehicles; ++i)
    {
        if (info.mVehicle[i].mIsPlayer) player = &info.mVehicle[i];
        if (info.mVehicle[i].mPlace == 1) leader = &info.mVehicle[i];
    }
    if (!player)
        return;

    // Same race-session test RelativeTiming.cpp uses -- practice/qualy/
    // warmup can still report a finite mEndET (their own time limit), which
    // is not a race duration and must not be treated as one.
    const bool isRaceSession = info.mSession >= 10 && info.mSession <= 13;

    // Latch the minimum mCountLapFlag seen during the lap -- same technique
    // as LapDeltaStore (see there for why a minimum-latch rather than a
    // same-tick read: the flag is transient and resets before it can be read
    // at lap completion).
    if (player->mCountLapFlag < m_lapValidityLatch)
        m_lapValidityLatch = player->mCountLapFlag;
    // Full-course yellow / safety car at any point of a lap makes it slow
    // and light on fuel, so it must not teach the fuel per lap or the pace
    // (CrewChief review 2026-11-24; its Fuel.cs only counts laps run fully
    // under green). Game phase 6 is FCY; mYellowFlagState 0 is none and 6
    // is "resume" (green about to show), -1 invalid -- anything else is a
    // live FCY state. Latched per lap, for the player and the leader.
    const bool fullCourseYellow = info.mGamePhase == 6
        || (info.mYellowFlagState != 0 && info.mYellowFlagState != 6 && info.mYellowFlagState != -1);
    if (fullCourseYellow)
    {
        m_lapUnderYellow = true;
        m_leaderLapUnderYellow = true;
    }
    // Any pit-lane time this lap makes its fuel delta unrepresentative.
    // In the garage stall counts as in the pits (CrewChief review
    // 2026-11-24; its RF2GameStateMapper does the same), since rF2 does
    // not always set mInPits there.
    if (player->mInPits || player->mInGarageStall)
    {
        EnterCriticalSection(&m_lock);
        m_scoringLapTainted = true;
        LeaveCriticalSection(&m_lock);
    }

    if (m_scoringLastTotalLaps >= 0 && player->mTotalLaps > m_scoringLastTotalLaps)
    {
        // Flag 1 ("count lap but not time") is what many servers report for
        // ordinary laps -- see LapDelta.cpp -- so only 0 is rejected.
        const bool countsForTime = m_lapValidityLatch >= 1;
        const double lastLap = player->mLastLapTime;
        const bool lapTimeOk = lastLap > 5.0 && lastLap < 100000.0;

        EnterCriticalSection(&m_lock);
        // A clean lap: counts, no pit-lane time, and not a race's start lap
        // (grid or rolling start). Only these feed the pace (review F1).
        const bool clean = countsForTime && !m_scoringLapTainted && lapTimeOk
                        && !m_lapUnderYellow
                        && !(isRaceSession && player->mTotalLaps <= 1);

        // Pace gate against THIS session's own clean laps rather than the
        // all-time PB (review F4): a wet race or a slower car than the one
        // that set the class PB used to learn nothing. Until two clean laps
        // exist this session every clean lap passes.
        bool nearPace = true;
        if (m_paceCount >= 2)
        {
            const double pace = PaceLocked();
            const double tol = (std::max)(kPaceToleranceSeconds, kPaceToleranceFraction * pace);
            nearPace = lapTimeOk && fabs(lastLap - pace) <= tol;
        }
        // The learned L/lap only from laps you drove all the way (review
        // F167/F252); the pace above may still use AI/team-mate laps.
        const bool usable = clean && nearPace && g_localPlayerLapClean.load(std::memory_order_relaxed);

        // The pace history takes every clean lap (no pace gate), so it
        // follows a drying or worsening track within a few laps.
        if (clean)
        {
            m_paceHistory[m_paceNext] = lastLap;
            m_paceNext = (m_paceNext + 1) % kPaceHistorySize;
            if (m_paceCount < kPaceHistorySize) ++m_paceCount;
            if (m_bestCleanLapSeconds <= 0.0 || lastLap < m_bestCleanLapSeconds)
                m_bestCleanLapSeconds = lastLap;
        }

        m_scoringLapTainted = false;
        m_lapUnderYellow = fullCourseYellow; // fresh latch for the new lap
        // One item per crossing, paired with the telemetry half only when
        // the crossing times match (review F5; see OnTelemetry).
        const double crossET = info.mCurrentET;
        if (m_haveConsumedSample && fabs(m_pendingConsumedET - crossET) < kPairWindowSeconds)
        {
            if (usable && !m_pendingConsumedTainted)
                ApplyLearnedLap(m_pendingConsumed);
            m_haveConsumedSample = false;
        }
        else
        {
            m_haveConsumedSample = false; // stale: from a crossing this side never saw
            m_pendingVerdictUsable = usable;
            m_pendingVerdictET = crossET;
            m_haveScoringVerdict = true;
        }
        LeaveCriticalSection(&m_lock);

        m_lapValidityLatch = 2; // reset for next lap
    }
    m_scoringLastTotalLaps = player->mTotalLaps;

    // Track the race LEADER's completed-lap times (independent of the
    // player's own lap tracking above), used to estimate when it takes the
    // flag. Its pit laps and the start lap are left out (review F7).
    if (leader)
    {
        const bool leaderInPits = leader->mInPits || leader->mInGarageStall;
        if (leaderInPits)
            m_leaderLapTainted = true;
        if (m_leaderLastTotalLaps < 0)
        {
            m_leaderLastTotalLaps = leader->mTotalLaps;
        }
        else if (leader->mTotalLaps != m_leaderLastTotalLaps)
        {
            const double lastLap = leader->mLastLapTime;
            if (leader->mTotalLaps == m_leaderLastTotalLaps + 1 && !m_leaderLapTainted
                && !m_leaderLapUnderYellow && leader->mTotalLaps > 1 && lastLap > 5.0 && lastLap < 100000.0)
            {
                m_leaderLapHistory[m_leaderLapHistoryNext] = lastLap;
                m_leaderLapHistoryNext = (m_leaderLapHistoryNext + 1) % kLeaderLapHistorySize;
                if (m_leaderLapHistoryCount < kLeaderLapHistorySize) ++m_leaderLapHistoryCount;

                double sum = 0.0;
                for (int i = 0; i < m_leaderLapHistoryCount; ++i)
                    sum += m_leaderLapHistory[i];
                m_leaderAvgLapTime = sum / m_leaderLapHistoryCount;
                if (m_leaderBestLapTime <= 0.0 || lastLap < m_leaderBestLapTime)
                    m_leaderBestLapTime = lastLap;
            }
            m_leaderLastTotalLaps = leader->mTotalLaps;
            m_leaderLapTainted = leaderInPits;
            m_leaderLapUnderYellow = fullCourseYellow;
        }
    }

    EnterCriticalSection(&m_lock);
    const double dt = (m_lastScoringET >= 0.0)
        ? (std::min)((std::max)(info.mCurrentET - m_lastScoringET, 0.0), 1.0) : 0.0;
    m_lastScoringET = info.mCurrentET;

    // Pit / stint detection. A pit exit only starts a fresh stint after a
    // real stop (review F3): stationary for over a second, rF2's own
    // "stopped" pit state, a stop counted, or fuel added. A drive-through
    // no longer restarts the stint.
    // The garage stall counts as the pit lane (CrewChief review 2026-11-24).
    const bool inPits = player->mInPits || player->mInGarageStall;
    if (inPits && !m_wasInPits)
    {
        m_pitStopped = false;
        m_pitStillSeconds = 0.0;
        m_pitStopsAtEntry = player->mNumPitstops;
        m_pitFuelAtEntry = m_state.fuelLoad;
    }
    if (inPits)
    {
        const TelemVect3& v = player->mLocalVel;
        const double speed = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (speed < 0.5)
            m_pitStillSeconds += dt;
        if (m_pitStillSeconds > 1.0 || player->mPitState == 3
            || player->mNumPitstops > m_pitStopsAtEntry
            || m_state.fuelLoad > m_pitFuelAtEntry + 0.5)
            m_pitStopped = true;
    }
    else if (m_wasInPits && (m_pitStopped || player->mNumPitstops > m_pitStopsAtEntry))
    {
        m_stintStartLaps = player->mTotalLaps;   // left the pits after a stop: new stint
        m_stintStartFuel = -1.0;                 // captured on the next tick, once refuelling is done
        m_stintStartET = info.mCurrentET;
    }
    m_wasInPits = inPits;
    m_state.inPits = inPits;
    m_state.pitStopsDone = player->mNumPitstops;

    // Before the green (gridwalk, formation lap) the stint keeps starting
    // now, so the pre-race minutes do not count towards it (review F3).
    if (isRaceSession && info.mGamePhase < 5)
    {
        m_stintStartLaps = player->mTotalLaps;
        m_stintStartFuel = -1.0;
        m_stintStartET = info.mCurrentET;
    }

    if (m_stintStartLaps < 0)
        m_stintStartLaps = player->mTotalLaps;   // first sample of the session
    if (m_stintStartFuel < 0.0 && !inPits)
        m_stintStartFuel = m_state.fuelLoad;
    if (m_stintStartET < 0.0)
        m_stintStartET = info.mCurrentET;        // first sample of the session

    m_state.sessionLaps = player->mTotalLaps;
    m_state.stintLaps = (player->mTotalLaps > m_stintStartLaps)
                      ? (player->mTotalLaps - m_stintStartLaps) : 0;
    m_state.stintStartFuel = m_stintStartFuel;
    m_state.stintTimeSeconds = (info.mCurrentET > m_stintStartET) ? (info.mCurrentET - m_stintStartET) : 0.0;
    m_state.sessionBestLapSeconds = (player->mBestLapTime > 0.0) ? player->mBestLapTime : -1.0;
    m_state.playerFinished = player->mFinishStatus != 0;

    // ScoringInfoV01::mLapDist is the TRACK LENGTH (VehicleScoringInfoV01's
    // own mLapDist is the car's live distance around its current lap --
    // same field name, different struct, different meaning).
    const double trackLength = info.mLapDist;
    const double myProg = LapProgress(*player, trackLength);
    const double lapInto = myProg - floor(myProg);
    m_state.currentLapFractional = myProg;

    // rF2 signals "no cap" with a huge sentinel on whichever of laps/time
    // doesn't apply -- anything below a generous ceiling is a real limit.
    // A time limit stays one after the clock reaches 0:00 (review F241):
    // the race goes on to the flag, and that last lap is when the fuel
    // figure matters most.
    const bool hasLapLimit = info.mMaxLaps > 0 && info.mMaxLaps < 100000;
    const bool hasTimeLimit = info.mEndET > 0.0 && info.mEndET < 1.0e8;
    const double secondsLeft = hasTimeLimit ? (std::max)(0.0, info.mEndET - info.mCurrentET) : -1.0;

    // Who ends the race: the overall leader in a race (the flag comes out
    // when it crosses), yourself in practice/qualy. Your laps are always
    // counted at your own clean-lap pace (reviews F7, F259, F311).
    const VehicleScoringInfoV01* endCar = (isRaceSession && leader) ? leader : player;
    const bool endCarIsMe = endCar == player;
    // Laps still to run use the FASTEST clean lap, not the median (CrewChief
    // review 2026-11-24, after its Fuel.cs): a faster lap means more laps fit
    // in the time left, so the fuel estimate errs a lap high, never short.
    // Falls back to the median pace until there is a clean lap. The leader
    // likewise at its best clean lap; as a lapped car you still finish at
    // your own crossing after its flag, i.e. the slower of the two paces
    // decides your laps (CrewChief does the same).
    const double myPace = (m_bestCleanLapSeconds > 0.0) ? m_bestCleanLapSeconds : PaceLocked();
    const double leaderPace = (m_leaderBestLapTime > 0.0) ? m_leaderBestLapTime : m_leaderAvgLapTime;
    const double endPace = (!endCarIsMe && leaderPace > 0.0) ? leaderPace : myPace;
    const double endProg = LapProgress(*endCar, trackLength);
    const bool endCarFinished = !endCarIsMe && endCar->mFinishStatus == 1;

    // Both a lap and a time limit: whichever the leader reaches first, with
    // half a lap of hysteresis so it does not flip-flop (review F312).
    bool useLaps = hasLapLimit;
    bool useTime = !hasLapLimit && hasTimeLimit;
    if (hasLapLimit && hasTimeLimit)
    {
        if (endPace > 0.0)
        {
            const double lapEndSeconds = (std::max)(0.0, info.mMaxLaps - endProg) * endPace;
            const double margin = 0.5 * endPace;
            if (!m_haveEndChoice)
                m_timeEndsFirst = secondsLeft < lapEndSeconds;
            else if (m_timeEndsFirst && lapEndSeconds < secondsLeft - margin)
                m_timeEndsFirst = false;
            else if (!m_timeEndsFirst && secondsLeft < lapEndSeconds - margin)
                m_timeEndsFirst = true;
            m_haveEndChoice = true;
        }
        useTime = m_haveEndChoice && m_timeEndsFirst;
        useLaps = !useTime;
    }

    // Your own line crossings still to come, counted from where you are in
    // the current lap.
    const double kEps = 1e-6;
    double myLapsLeft = -1.0;
    if (useLaps)
    {
        // Lapped cars finish when the leader does, not at mMaxLaps of their
        // own (review F259): you cover the leader's remaining distance (same
        // speed assumed), then run on to your next crossing.
        const double endLapsLeft = endCarFinished ? 0.0 : (std::max)(0.0, info.mMaxLaps - endProg);
        myLapsLeft = (std::max)(0.0, ceil(myProg + endLapsLeft - kEps) - myProg);
    }
    else if (useTime && myPace > 0.0 && endPace > 0.0)
    {
        // The flag falls when the leader crosses the line after 0:00 (review
        // F311); you finish at your first crossing after that.
        double flagInSeconds = 0.0;
        if (!endCarFinished)
        {
            const double endInto = endProg - floor(endProg);
            const double endLaps = (std::max)(0.0, ceil(endInto + secondsLeft / endPace - kEps) - endInto);
            flagInSeconds = endLaps * endPace;
        }
        myLapsLeft = (std::max)(0.0, ceil(lapInto + flagInSeconds / myPace - kEps) - lapInto);
    }
    if (myLapsLeft >= 0.0 && player->mFinishStatus != 0)
        myLapsLeft = 0.0; // flag taken: nothing more to cover

    m_state.isRaceSession = isRaceSession;
    m_state.isLapRace = useLaps;
    m_state.isTimedRace = useTime;
    m_state.lapsLeft = useLaps ? (std::max)(0.0, static_cast<double>(info.mMaxLaps) - static_cast<double>(player->mTotalLaps)) : -1.0;
    m_state.raceMinutesLeft = useTime ? secondsLeft / 60.0 : -1.0;
    m_state.raceSecondsLeft = secondsLeft;

    m_state.fuelNeededForSession = (m_hasLearnedValue && myLapsLeft >= 0.0)
        ? myLapsLeft * m_learnedFuelPerLap
        : -1.0;
    m_state.raceLapsRemainingEstimate = myLapsLeft;

    // Total laps you will run in the race: done + this lap's part + still
    // to come, all at your own pace (review F7) -- no longer the race time
    // over the overall leader's raw average, nor a running maximum of time
    // left that a mid-race join could never recover.
    double lapsForFullRace = -1.0;
    bool lapsForFullRaceIsEstimate = false;
    if (myLapsLeft >= 0.0)
    {
        lapsForFullRace = static_cast<double>(player->mTotalLaps) + lapInto + myLapsLeft;
        lapsForFullRaceIsEstimate = true;
        if (useLaps && fabs(lapsForFullRace - info.mMaxLaps) < 0.01)
        {
            lapsForFullRace = static_cast<double>(info.mMaxLaps); // not lapped: exact
            lapsForFullRaceIsEstimate = false;
        }
    }

    m_state.raceTotalLaps = isRaceSession ? lapsForFullRace : -1.0;
    m_state.raceTotalLapsIsEstimate = isRaceSession && lapsForFullRaceIsEstimate;

    m_state.fuelRequiredForRace = (m_hasLearnedValue && lapsForFullRace >= 0.0)
        ? lapsForFullRace * m_learnedFuelPerLap
        : -1.0;

    // The WHOLE race can never need less fuel than what's left of it.
    if (m_state.fuelRequiredForRace >= 0.0 && m_state.fuelNeededForSession >= 0.0
        && m_state.fuelRequiredForRace < m_state.fuelNeededForSession)
    {
        m_state.fuelRequiredForRace = m_state.fuelNeededForSession;
    }

    LeaveCriticalSection(&m_lock);
}

FuelStore::State FuelStore::Read()
{
    State copy;
    if (!m_ready) return copy;
    EnterCriticalSection(&m_lock);
    copy = m_state;
    LeaveCriticalSection(&m_lock);
    return copy;
}
