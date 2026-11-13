#include "FuelStore.h"
#include "InternalsPlugin.hpp"
#include "LapDelta.h"
#include "Log.h"
#include "RealtimeState.h"
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
        std::string out;
        for (size_t i = 0; i < maxLen && s[i]; ++i)
        {
            const char c = s[i];
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
            out.push_back(ok ? c : '_');
        }
        if (out.empty()) out = "unknown";
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
    // Callers are expected to FlushIfDirty() before Clear() (see
    // Plugin.cpp) -- SaveFuelStats() needs m_key, which is about to be
    // wiped below. Dropped here too, defensively, so a dirty flag left set
    // by some future call path can't later write using an already-cleared
    // key.
    m_dirty = false;
    m_state = State{};
    m_key.clear();
    m_lastLapNumber = -1;
    m_haveLapStartFuel = false;
    m_hasLearnedValue = false;
    m_learnedFuelPerLap = 0.0;
    m_fuelHistoryCount = 0;
    m_fuelHistoryNext = 0;
    m_lastLapTimeSeconds = 0.0;
    m_haveRaceTotal = false;
    m_raceTotalLaps = -1.0;
    m_raceTotalSeconds = -1.0;
    m_leaderLastTotalLaps = -1;
    m_leaderLapHistoryCount = 0;
    m_leaderLapHistoryNext = 0;
    m_leaderAvgLapTime = -1.0;
    m_stintStartET = -1.0;
    m_historySeeded = false;
    m_seedNorm = 0.0;
    m_telemLapTainted = false;
    m_scoringLapTainted = false;
    m_havePrevFuel = false;
    m_haveConsumedSample = false;
    m_haveScoringVerdict = false;
    LeaveCriticalSection(&m_lock);
}

void FuelStore::FlushIfDirty()
{
    if (!m_ready) return;
    EnterCriticalSection(&m_lock);
    if (m_dirty)
    {
        SaveFuelStats();
        m_dirty = false;
    }
    LeaveCriticalSection(&m_lock);
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
        // fuel glitch) is not representative consumption.
        const double med = MedianOfHistory();
        if (med > 0.01 && (consumed < 0.6 * med || consumed > 1.6 * med))
        {
            Log::Write("FuelStore: rejected outlier lap %.3f L (median %.3f L)", consumed, med);
            return;
        }
    }
    m_fuelHistory[m_fuelHistoryNext] = consumed;
    m_fuelHistoryNext = (m_fuelHistoryNext + 1) % kFuelHistorySize;
    if (m_fuelHistoryCount < kFuelHistorySize) ++m_fuelHistoryCount;
    RecomputeFuelPerLap();
    m_hasLearnedValue = true;
    m_dirty = true; // written later by FlushIfDirty(), off this thread's critical path
}

void FuelStore::RecomputeFuelPerLap()
{
    // Median rather than mean: one odd lap cannot drag it.
    m_learnedFuelPerLap = (m_fuelHistoryCount == 0) ? 0.0 : MedianOfHistory();
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

void FuelStore::LoadFuelStats(const char* vehicleName, const char* trackName)
{
    m_learnedFuelPerLap = 0.0;
    m_hasLearnedValue = false;
    m_fuelHistoryCount = 0;
    m_fuelHistoryNext = 0;
    m_historySeeded = false;
    m_seedNorm = 0.0;
    m_lastLapTimeSeconds = 0.0;

    char dir[MAX_PATH];
    GetFuelStatsDir(dir, sizeof(dir));
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s.txt", dir, m_key.c_str());

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f)
    {
        Log::Write("FuelStore: no saved fuel stats yet for %s / %s", vehicleName, trackName);
        return;
    }
    char line[64];
    double savedRate = 0.0;
    if (fgets(line, sizeof(line), f))
        savedRate = atof(line);
    // Second line: last lap time (see SaveFuelStats).
    if (fgets(line, sizeof(line), f))
    {
        const double lapTime = atof(line);
        if (lapTime > 5.0 && lapTime < 100000.0)
            m_lastLapTimeSeconds = lapTime;
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
        m_seedNorm = savedRate;
    fclose(f);
    Log::Write("FuelStore: loaded %.3f L/lap @1x (%s), %.3fs/lap for %s / %s",
               m_seedNorm, normalised ? "normalised" : "legacy file, rate ignored", m_lastLapTimeSeconds, vehicleName, trackName);
}

void FuelStore::SaveFuelStats() const
{
    char dir[MAX_PATH];
    GetFuelStatsDir(dir, sizeof(dir));
    CreateDirectoryA(dir, nullptr); // no-op (ERROR_ALREADY_EXISTS) if it's already there

    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s.txt", dir, m_key.c_str());

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f)
    {
        Log::Write("FuelStore: failed to save fuel stats to %s", path);
        return;
    }
    // The rate is written normalised to 1x fuel: this session's learned
    // rate divided by its multiplier. If nothing real was learned this
    // session (still only the seeded value) or the multiplier is unknown,
    // the previously saved normalised value is kept as it was.
    const int mult = g_fuelMult.load();
    double normToSave = m_seedNorm;
    if (m_hasLearnedValue && !m_historySeeded && mult > 0)
        normToSave = m_learnedFuelPerLap / mult;
    fprintf(f, "%.4f\n", normToSave);
    // Whatever the most recent real lap time seen this session was -- just
    // enough for next session's "Fuel Time Left" to have something to seed
    // from at the green flag. Not an average, and not meant to be a
    // reference lap; the very next completed lap this session overwrites it
    // in memory (see OnTelemetry), only the persisted file lags by one lap.
    fprintf(f, "%.3f\n", m_lastLapTimeSeconds);
    fprintf(f, "norm\n");
    fclose(f);
}

void FuelStore::OnTelemetry(const TelemInfoV01& info)
{
    if (!m_ready) return;

    // Vehicle CLASS, shared from the scoring thread -- see RealtimeState.h.
    char vehKey[64];
    VehicleKey::Get(vehKey, sizeof(vehKey));
    const std::string key = Sanitize(vehKey, sizeof(vehKey)) + "__" + Sanitize(info.mTrackName, sizeof(info.mTrackName));

    EnterCriticalSection(&m_lock);

    if (key != m_key)
    {
        m_key = key;
        LoadFuelStats(vehKey, info.mTrackName);
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
        if (consumed > 0.05 && lapTime > 5.0 && lapTime < 100000.0)
        {
            // Always kept regardless of the validity gate below -- this is
            // only "how long did the last lap take", used for the time-left
            // estimate, not the fuel-per-lap average itself.
            m_lastLapTimeSeconds = lapTime;

            // Whether this lap's consumption gets folded into the learned
            // average depends on rF2's own lap-validity flag AND proximity
            // to the personal best, both known only on the scoring thread --
            // see the header. Whichever side (this one, or OnScoring) notices
            // the lap boundary SECOND completes the handoff immediately;
            // whichever notices first leaves its half pending.
            if (m_haveScoringVerdict)
            {
                if (m_pendingVerdictUsable && !m_telemLapTainted)
                    ApplyLearnedLap(consumed);
                m_haveScoringVerdict = false;
            }
            else
            {
                m_pendingConsumed = consumed;
                m_pendingConsumedTainted = m_telemLapTainted;
                m_haveConsumedSample = true;
            }
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
    m_state.fuelPerLap = m_learnedFuelPerLap;
    m_state.timeLeftSeconds = (m_hasLearnedValue && m_learnedFuelPerLap > 0.01 && m_lastLapTimeSeconds > 0.0)
        ? (info.mFuel / m_learnedFuelPerLap) * m_lastLapTimeSeconds
        : -1.0;

    LeaveCriticalSection(&m_lock);
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

    // Latch the minimum mCountLapFlag seen during the lap -- same technique
    // as LapDeltaStore (see there for why a minimum-latch rather than a
    // same-tick read: the flag is transient and resets before it can be read
    // at lap completion).
    if (player->mCountLapFlag < m_lapValidityLatch)
        m_lapValidityLatch = player->mCountLapFlag;
    // Any pit-lane time this lap makes its fuel delta unrepresentative.
    if (player->mInPits)
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
        const double pb = g_lapDelta.PersonalBestSeconds();
        // No PB yet (pb <= 0) means this IS the first valid lap becoming the
        // PB, in LapDeltaStore's own same-tick ordering (LapDelta::OnScoring
        // runs before FuelStore::OnScoring -- see Plugin.cpp) -- so it is
        // never usable to bootstrap from before a PB exists at all.
        // With no PB yet the lap is accepted (outlier guard and taint checks
        // still apply); otherwise it must be near the PB.
        const double tol = (std::max)(kPbToleranceSeconds, kPbToleranceFraction * pb);
        const bool withinPbTolerance = pb <= 0.0 || fabs(player->mLastLapTime - pb) <= tol;
        const bool usable = countsForTime && withinPbTolerance && !m_scoringLapTainted;

        EnterCriticalSection(&m_lock);
        m_scoringLapTainted = false;
        if (m_haveConsumedSample)
        {
            if (usable && !m_pendingConsumedTainted)
                ApplyLearnedLap(m_pendingConsumed);
            m_haveConsumedSample = false;
        }
        else
        {
            m_pendingVerdictUsable = usable;
            m_haveScoringVerdict = true;
        }
        LeaveCriticalSection(&m_lock);

        m_lapValidityLatch = 2; // reset for next lap
    }
    m_scoringLastTotalLaps = player->mTotalLaps;

    // Same race-session test RelativeTiming.cpp uses -- practice/qualy/
    // warmup can still report a finite mEndET (their own time limit), which
    // is not a race duration and must not be treated as one.
    const bool isRaceSession = info.mSession >= 10 && info.mSession <= 13;

    // rF2 signals "no cap" with a huge sentinel (LONG_MAX-ish) on whichever
    // of laps/time doesn't apply to this session -- treat anything below a
    // generous ceiling as a real limit.
    const bool isLapRace = info.mMaxLaps > 0 && info.mMaxLaps < 100000;
    const bool isTimedRace = !isLapRace && info.mEndET > 0.0 && info.mEndET < 1.0e8 && info.mEndET > info.mCurrentET;
    const double lapsLeft = isLapRace ? (std::max)(0.0, static_cast<double>(info.mMaxLaps) - static_cast<double>(player->mTotalLaps)) : -1.0;
    const double raceMinutesLeft = isTimedRace ? (info.mEndET - info.mCurrentET) / 60.0 : -1.0;

    // Track the LEADER's own completed-lap times (independent of the
    // player's own lap tracking above) purely to estimate a timed race's
    // total lap count -- see State::raceTotalLaps.
    if (leader)
    {
        if (m_leaderLastTotalLaps < 0)
        {
            m_leaderLastTotalLaps = leader->mTotalLaps;
        }
        else if (leader->mTotalLaps > m_leaderLastTotalLaps)
        {
            const double lastLap = leader->mLastLapTime;
            if (lastLap > 5.0 && lastLap < 100000.0)
            {
                m_leaderLapHistory[m_leaderLapHistoryNext] = lastLap;
                m_leaderLapHistoryNext = (m_leaderLapHistoryNext + 1) % kLeaderLapHistorySize;
                if (m_leaderLapHistoryCount < kLeaderLapHistorySize) ++m_leaderLapHistoryCount;

                double sum = 0.0;
                for (int i = 0; i < m_leaderLapHistoryCount; ++i)
                    sum += m_leaderLapHistory[i];
                m_leaderAvgLapTime = sum / m_leaderLapHistoryCount;
            }
            m_leaderLastTotalLaps = leader->mTotalLaps;
        }
    }

    EnterCriticalSection(&m_lock);
    // Pit detection -- the first in this plugin. A pit exit starts a fresh
    // stint: the lap count restarts and the fuel load in the tank at that
    // moment becomes the stint's starting load. Without this, "stint" figures
    // simply carried on across a pit reset as if nothing had happened, which
    // is what made them look stuck.
    const bool inPits = player->mInPits != 0;
    if (inPits && !m_wasInPits)
    {
        m_inStint = false;                 // entered the pits: stint over
    }
    else if (!inPits && m_wasInPits)
    {
        m_stintStartLaps = player->mTotalLaps;   // left the pits: new stint
        m_stintStartFuel = -1.0;                 // captured on the next tick, once refuelling is done
        m_stintStartET = info.mCurrentET;
        m_inStint = true;
    }
    m_wasInPits = inPits;

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

    m_state.isRaceSession = isRaceSession;
    m_state.isLapRace = isLapRace;
    m_state.isTimedRace = isTimedRace;
    m_state.lapsLeft = lapsLeft;
    m_state.raceMinutesLeft = raceMinutesLeft;
    // ScoringInfoV01::mLapDist is the TRACK LENGTH (VehicleScoringInfoV01's
    // own mLapDist is the car's live distance around its current lap --
    // same field name, different struct, different meaning).
    const double trackLengthForLap = info.mLapDist;
    m_state.currentLapFractional = static_cast<double>(player->mTotalLaps)
        + ((trackLengthForLap > 0.1) ? (player->mLapDist / trackLengthForLap) : 0.0);

    // Fuel needed to cover the SESSION's remaining laps/time at the current
    // learned rate -- a timed race's remaining minutes are converted to an
    // equivalent lap count via the last completed lap's time.
    double lapsRemainingForFuel = -1.0;
    if (isLapRace)
        lapsRemainingForFuel = lapsLeft;
    else if (isTimedRace && m_lastLapTimeSeconds > 0.0)
        lapsRemainingForFuel = (raceMinutesLeft * 60.0) / m_lastLapTimeSeconds;

    m_state.fuelNeededForSession = (m_hasLearnedValue && lapsRemainingForFuel >= 0.0)
        ? lapsRemainingForFuel * m_learnedFuelPerLap
        : -1.0;
    m_state.raceLapsRemainingEstimate = lapsRemainingForFuel;

    // The race's TOTAL laps/duration, for estimating fuelRequiredForRace --
    // as opposed to lapsLeft/raceMinutesLeft above, which shrink as the race
    // goes on. A lap race needs no tracking at all: mMaxLaps is a stable,
    // always-correct value straight from the game every tick. A timed race
    // has no equivalent "total" field, only "remaining" -- so it's tracked
    // as a running MAXIMUM of remaining time rather than a single captured
    // sample. That self-corrects if the plugin only started partway through
    // (e.g. re-enabled mid-race, or an early tick during a session-load
    // transient read anomalously low): a later, larger reading simply
    // overwrites it, rather than that one bad tick being locked in forever.
    if (isLapRace)
    {
        m_raceTotalLaps = static_cast<double>(info.mMaxLaps);
    }
    else if (isTimedRace)
    {
        const double currentTotalSeconds = raceMinutesLeft * 60.0;
        if (!m_haveRaceTotal || currentTotalSeconds > m_raceTotalSeconds)
            m_raceTotalSeconds = currentTotalSeconds;
    }
    m_haveRaceTotal = true;

    // Total lap count for the race -- exact for a lap race, and for a timed
    // race a live estimate from the LEADER's average pace (not the player's
    // own): a timed race's actual lap count is decided by whoever finishes
    // out front when time expires, not by the player's pace, so the
    // player's own lap time would misestimate it whenever the two differ.
    double lapsForFullRace = -1.0;
    bool lapsForFullRaceIsEstimate = false;
    if (isLapRace && m_raceTotalLaps >= 0.0)
    {
        lapsForFullRace = m_raceTotalLaps;
    }
    else if (isTimedRace && m_raceTotalSeconds >= 0.0 && m_leaderAvgLapTime > 0.0)
    {
        lapsForFullRace = m_raceTotalSeconds / m_leaderAvgLapTime;
        lapsForFullRaceIsEstimate = true;
    }

    m_state.raceTotalLaps = isRaceSession ? lapsForFullRace : -1.0;
    m_state.raceTotalLapsIsEstimate = isRaceSession && lapsForFullRaceIsEstimate;

    m_state.fuelRequiredForRace = (m_hasLearnedValue && lapsForFullRace >= 0.0)
        ? lapsForFullRace * m_learnedFuelPerLap
        : -1.0;

    // The WHOLE race can never need less fuel than what's left of it -- the
    // remaining portion is a subset of the total by definition. Floor it at
    // fuelNeededForSession as a defensive guard against this exact symptom
    // (a stale/too-small "total" reading) recurring from some other edge
    // case in the future.
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
