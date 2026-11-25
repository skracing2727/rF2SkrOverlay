#pragma once
#include <Windows.h>
#include <atomic>
#include <string>

// Fuel time-left: current fuel load divided by a learned per-lap
// consumption rate, converted into a time estimate via the clean-lap pace
// (median of this session's recent laps without pit-lane time). Consumption is learned from laps completed THIS
// session (EMA-smoothed so one odd lap doesn't swing it), and persisted per
// car+track so a fresh session still starts with a sane estimate before the
// driver has completed a lap in it.
class FuelStore
{
public:
    void Init();
    // Called from the game's telemetry callback (player car only).
    void OnTelemetry(const struct TelemInfoV01& info);
    // Called from the game's scoring callback (~5Hz) purely for session-wide
    // race-remaining info (laps or time left in the RACE itself, not fuel) --
    // shown alongside the fuel estimate so the driver can compare the two.
    void OnScoring(const struct ScoringInfoV01& info);
    void Clear();
    // Writes the learned fuel rate if it's changed since the last flush.
    // Called from the save thread (Persist.cpp) at any moment: it works out
    // what to write under m_lock and writes after releasing it.
    // OnTelemetry() itself never touches disk for a save; a completed lap
    // just sets the dirty flag, so the game's telemetry thread can't be
    // stalled by a slow disk or an antivirus scan intercepting the write.
    void FlushIfDirty();

    struct State
    {
        bool valid = false;
        double fuelLoad = 0.0;          // liters remaining right now
        // The load the car STARTED this stint on, captured at pit exit (or at
        // the first sample of the session). Fixed for the stint, so pairing
        // it with fuelLoad above shows what was put in versus what is left.
        double stintStartFuel = -1.0;
        // Laps completed in the session, and in the current stint (since
        // leaving the pits). The SDK gives the first directly; the second has
        // to be counted here, since there is no "laps since pit exit" field.
        int sessionLaps = 0;
        int stintLaps = 0;
        // Elapsed since the current stint started: pit exit after a real stop
        // (not a drive-through), the green flag in a race, or session start.
        double stintTimeSeconds = -1.0;
        double fuelAtLapStart = 0.0;     // litres at the start of the current telemetry lap
        double fuelCapacity = 0.0;       // tank capacity in litres (TelemInfoV01::mFuelCapacity), 0 = unknown
        // Lap time to plan with: the clean-lap pace (paceLapSeconds below).
        // Kept under this name because FuelSave/TyreSave read it; it used to
        // be the raw last lap, in-laps and the lap with the stop included
        // (review F1). 0 = none yet.
        double lastLapTimeSeconds = 0.0;
        // Clean-lap pace: median of the last 5 laps that counted, had no
        // pit-lane time and were not a race's start lap; before there are
        // any, the pace saved last session for this car+track, else the raw
        // last lap. 0 = none yet.
        double paceLapSeconds = 0.0;
        double rawLastLapSeconds = 0.0;  // the last completed lap as driven (any lap), 0 = none
        double sessionBestLapSeconds = -1.0; // your fastest lap this session (mBestLapTime), -1 = none
        bool playerFinished = false;     // you have taken the flag (mFinishStatus != 0)
        long telemLapNumber = -1;
        bool inPits = false;
        char profilePath[MAX_PATH] = ""; // per car+track fuel-use profile file (FuelSave)
        // The same car+track under the old per-class key ("" = same file):
        // FuelSave/TyreSave read it once when profilePath does not exist yet.
        char legacyProfilePath[MAX_PATH] = "";
        // Litres/lap, learned: median of the last 10 laps that counted, had
        // no pit-lane time (so no in-laps, out-laps or stops), no refuel
        // and were near this session's pace. Seeded from the saved rate
        // until the first such lap. 0 = nothing yet.
        double fuelPerLap = 0.0;
        double timeLeftSeconds = -1.0;  // -1 = not enough data yet

        // True only for an actual race session (mSession 10-13) -- practice/
        // qualy/warmup can still report a finite mEndET (the session's own
        // time limit), which is not a race duration and shouldn't be treated
        // as one.
        bool isRaceSession = false;
        // The player's pit stops so far this session (mNumPitstops).
        int pitStopsDone = 0;

        // Race remaining, whichever limit ends this session -- rF2 signals
        // a lap limit via a real mMaxLaps and a time limit via a real mEndET.
        // With both, whichever the leader reaches first is used. A timed
        // session stays timed after 0:00 (raceMinutesLeft 0) until the flag.
        // At most one of the two flags is set.
        bool isLapRace = false;
        double lapsLeft = -1.0;          // lap limit minus your completed laps
        bool isTimedRace = false;
        double raceMinutesLeft = -1.0;
        // Session time left whenever there is a time limit (even in a lap
        // race with a time limit too), never below 0. -1 = no time limit.
        double raceSecondsLeft = -1.0;

        // Total laps YOU will run in the race, race sessions only: laps done
        // + this lap's part + raceLapsRemainingEstimate. Exact (mMaxLaps) in
        // a lap race when not lapped; otherwise an estimate. -1 = not
        // applicable/not enough data yet.
        double raceTotalLaps = -1.0;
        bool raceTotalLapsIsEstimate = false;

        // Laps completed (whole part) plus live progress through the lap in
        // progress (fractional part, mLapDist / track length) -- e.g. 1.4
        // means 1 lap completed and 40% of the way through lap 2. Paired
        // with raceTotalLaps above for a "lap X / Y" progress readout.
        double currentLapFractional = 0.0;

        // Laps (with the fraction of the current one) YOU still have to
        // drive to the flag, whichever kind of session: a lap race counts to
        // when the leader finishes (lapped cars run fewer); a timed race
        // counts to your first crossing after the leader takes the flag
        // after 0:00, at your own pace. 0 once you have finished. -1 = not
        // enough data yet.
        double raceLapsRemainingEstimate = -1.0;

        // Fuel needed to finish: raceLapsRemainingEstimate x fuelPerLap -- compare
        // directly against fuelLoad to see whether a stop is actually
        // needed, rather than just how long what's already in the tank
        // lasts. -1 = not enough data yet (no learned rate, or session type
        // undetermined).
        double fuelNeededForSession = -1.0;

        // Fuel required for the WHOLE race distance/duration (from the
        // start, not just what's left) at the current learned rate --
        // "how big a tank would this race need". -1 = not enough data yet.
        double fuelRequiredForRace = -1.0;
    };
    State Read();

private:
    // Pit / stint tracking. mInPits is edge-detected here rather than in each
    // consumer, so "the stint" means one thing across the whole plugin.
    // Scoring thread, under m_lock.
    bool m_wasInPits = false;
    short m_stintStartLaps = -1;
    double m_stintStartFuel = -1.0;
    double m_stintStartET = -1.0; // ScoringInfoV01::mCurrentET at stint start
    // This pit visit: did the car really stop (review F3)?
    bool m_pitStopped = false;
    double m_pitStillSeconds = 0.0;
    short m_pitStopsAtEntry = 0;
    double m_pitFuelAtEntry = 0.0;
    double m_lastScoringET = -1.0;

    CRITICAL_SECTION m_lock;
    bool m_ready = false;
    State m_state;

    std::string m_key; // sanitized "vehicle__track"; written by OnTelemetry only
    // Telemetry thread only: the raw names m_key was last worked out from,
    // so OnTelemetry can tell "nothing changed" without building strings.
    // Clear() asks for a re-key through m_keysInvalidated.
    char m_rawVehKey[64] = {};
    char m_rawTrackName[64] = {};
    bool m_haveRawNames = false;
    std::atomic<bool> m_keysInvalidated{ false };

    long m_lastLapNumber = -1;
    double m_fuelAtLapStart = 0.0;
    bool m_haveLapStartFuel = false;
    double m_lastLapTimeSeconds = 0.0; // raw last lap (any lap)
    // Clean-lap pace history (scoring thread, under m_lock) -- see
    // State::paceLapSeconds. Saved as the file's lap time.
    static const int kPaceHistorySize = 5;
    double m_paceHistory[kPaceHistorySize] = {};
    int m_paceCount = 0;
    int m_paceNext = 0;
    double m_savedPaceSeconds = 0.0;
    double PaceLocked() const;
    // Fastest clean lap this session (-1 = none yet). Used for the laps
    // still to run in a timed race, so the estimate errs a lap high rather
    // than low (CrewChief review 2026-11-24).
    double m_bestCleanLapSeconds = -1.0;
    // Set if any tick of the player's current lap was under full-course
    // yellow / safety car; such a lap is not clean (CrewChief review
    // 2026-11-24). Scoring thread, reset at each line crossing.
    bool m_lapUnderYellow = false;
    double m_prevElapsedTime = 0.0;
    double m_prevLapStartET = 0.0;

    // Race with both a lap and a time limit: which one ends it (with
    // hysteresis). Scoring thread, under m_lock.
    bool m_haveEndChoice = false;
    bool m_timeEndsFirst = false;

    // Rolling average of the race LEADER's completed-lap times (not the
    // player's own), without its pit laps or the start lap, used only to
    // estimate when it takes the flag in a timed race.
    bool m_leaderLapTainted = false;
    long m_leaderLastTotalLaps = -1;
    static const int kLeaderLapHistorySize = 5;
    double m_leaderLapHistory[kLeaderLapHistorySize] = {};
    int m_leaderLapHistoryCount = 0;
    int m_leaderLapHistoryNext = 0;
    double m_leaderAvgLapTime = -1.0;
    // Leader's fastest clean lap and its per-lap full-course-yellow latch
    // (CrewChief review 2026-11-24), same rules as the player's.
    double m_leaderBestLapTime = -1.0;
    bool m_leaderLapUnderYellow = false;

    // Moving average of the last kFuelHistorySize completed laps' fuel
    // consumption. Persisted (as the resulting average) per car+track, and
    // on load that average seeds the WHOLE window -- so a fresh stint/race
    // already has a sane estimate from previous sessions instead of
    // starting blank, with real laps this session then displacing the
    // seeded ones one at a time.
    static const int kFuelHistorySize = 10;
    double m_fuelHistory[kFuelHistorySize] = {};
    int m_fuelHistoryCount = 0;
    int m_fuelHistoryNext = 0;
    double m_learnedFuelPerLap = 0.0;
    bool m_hasLearnedValue = false;
    // Set instead of saving directly whenever m_learnedFuelPerLap changes;
    // cleared by FlushIfDirty() once copied out for writing.
    bool m_dirty = false;
    // What one save writes. m_stash holds one for the car/track just left,
    // when it changed with a rate still unsaved (under m_lock).
    struct SaveSnapshot
    {
        bool valid = false;
        std::string key;
        double norm = 0.0;     // L/lap at 1x fuel
        double lapTime = 0.0;
    };
    SaveSnapshot m_stash;
    SaveSnapshot SnapshotForSaveLocked() const;
    static bool WriteFuelStats(const SaveSnapshot& s); // false = failed, keep it for the next pass

    // Fuel-learning validity gate: a completed lap is only folded into the
    // learned average if it counts for time (rF2's own mCountLapFlag), had
    // no pit-lane time, and is within this many seconds of THIS session's
    // clean-lap pace (not the all-time PB -- review F4) -- an out-lap, a
    // spin, or an off-line cruise lap would otherwise drag the estimate
    // away from real racing consumption.
    //
    // Fuel is only ever measurable on the telemetry thread (TelemInfoV01::
    // mFuel has no scoring-side equivalent) while lap validity is only ever
    // known on the scoring thread (VehicleScoringInfoV01::mCountLapFlag has
    // no telemetry-side equivalent) -- see LapDeltaStore's own latch for the
    // same reasoning. Each side posts one item per line crossing, tagged
    // with the session time of the crossing; the second side to see the
    // line pairs with the pending item only if the times match within
    // kPairWindowSeconds, else replaces it (review F5). All of it is
    // guarded by m_lock like everything else shared between the two.
    static constexpr double kPaceToleranceSeconds = 5.0;
    static constexpr double kPaceToleranceFraction = 0.05; // whichever of the two is larger
    static constexpr double kPairWindowSeconds = 2.0;

    // True while the window still only holds the persisted (previous
    // session) value. The first real lap this session REPLACES it outright
    // instead of being averaged in against ten stale copies, so a bad or
    // out-of-date saved rate cannot linger for a whole session.
    bool m_historySeeded = false;

    // Saved rate normalised to a 1x fuel multiplier (0 = none saved). Turned
    // into a seeded window only once the session's multiplier is known --
    // see TrySeedFromSaved().
    double m_seedNorm = 0.0;
    void TrySeedFromSaved();

    // Lap contaminated for learning purposes: fuel jumped (refuel, or the
    // setup fuel load being applied on leaving the garage), the car went
    // through the pit lane, or the plugin joined mid-lap. Such a lap's fuel
    // delta is not a racing-lap consumption. The telemetry-thread and
    // scoring-thread halves are tracked separately (each is only knowable
    // on its own thread) and each cleared at its own lap boundary.
    bool m_telemLapTainted = false;
    bool m_scoringLapTainted = false;
    bool m_havePrevFuel = false;
    double m_prevFuel = 0.0;
    bool m_pendingConsumedTainted = false;

    short m_lapValidityLatch = 2;       // min mCountLapFlag seen this lap (scoring thread)
    short m_scoringLastTotalLaps = -1;  // detects the scoring-side lap-completed edge

    bool m_haveConsumedSample = false;  // a telemetry-side fuel sample is waiting on a verdict
    double m_pendingConsumed = 0.0;
    double m_pendingConsumedET = 0.0;   // session time of its crossing

    bool m_haveScoringVerdict = false;  // a scoring-side verdict is waiting on a sample
    bool m_pendingVerdictUsable = false;
    double m_pendingVerdictET = 0.0;    // session time of its crossing

    // Outlier rejections in a row on one side (review F385): enough of them
    // that agree restart the history from them.
    static const int kRejectRestart = 3;
    double m_rejected[kRejectRestart] = {};
    int m_rejectCount = 0;
    bool m_rejectHigh = false;

    // Folds one lap's consumption into the learned history. Caller must
    // already hold m_lock.
    void ApplyLearnedLap(double consumed);
    double MedianOfHistory() const;

    void RecomputeFuelPerLap();
};

extern FuelStore g_fuelStore;
