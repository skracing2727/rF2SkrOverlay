#pragma once
#include <Windows.h>
#include <string>

// Fuel time-left: current fuel load divided by a learned per-lap
// consumption rate, converted into a time estimate via the most recently
// completed lap's time. Consumption is learned from laps completed THIS
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
    // Writes the learned fuel rate if it's changed since the last flush --
    // call only while NOT actively driving (see Plugin.cpp). OnTelemetry()
    // itself never touches disk; a completed lap just sets the dirty flag,
    // so the game's telemetry thread can't be stalled by a slow disk or an
    // antivirus scan intercepting the write.
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
        double stintTimeSeconds = -1.0; // elapsed since the current stint started (pit exit, or session start)
        double fuelAtLapStart = 0.0;     // litres at the start of the current telemetry lap
        double lastLapTimeSeconds = 0.0; // last completed lap, 0 = none yet
        long telemLapNumber = -1;
        bool inPits = false;
        char profilePath[MAX_PATH] = ""; // per car+track fuel-use profile file (FuelSave)
        double fuelPerLap = 0.0;        // liters/lap, learned
        double timeLeftSeconds = -1.0;  // -1 = not enough data yet

        // True only for an actual race session (mSession 10-13) -- practice/
        // qualy/warmup can still report a finite mEndET (the session's own
        // time limit), which is not a race duration and shouldn't be treated
        // as one.
        bool isRaceSession = false;

        // Race remaining, whichever applies to this session -- rF2 signals
        // a lap-limited race via a real mMaxLaps, and a time-limited one via
        // a real (finite, still in the future) mEndET. At most one of these
        // is meaningful at once.
        bool isLapRace = false;
        double lapsLeft = -1.0;
        bool isTimedRace = false;
        double raceMinutesLeft = -1.0;

        // Total laps the race will run, race sessions only -- exact
        // (mMaxLaps) for a lap race; for a timed race, a live estimate from
        // the race's total duration divided by the LEADER's own average lap
        // time (not the player's -- a timed race's actual lap count is
        // decided by whoever's out front, not by your own pace), updated
        // continuously as the leader completes more laps. -1 = not
        // applicable/not enough data yet.
        double raceTotalLaps = -1.0;
        bool raceTotalLapsIsEstimate = false;

        // Laps completed (whole part) plus live progress through the lap in
        // progress (fractional part, mLapDist / track length) -- e.g. 1.4
        // means 1 lap completed and 40% of the way through lap 2. Paired
        // with raceTotalLaps above for a "lap X / Y" progress readout.
        double currentLapFractional = 0.0;

        // Race laps remaining, unified into one number regardless of
        // whether this is a lap-limited or time-limited session (a timed
        // race's remaining minutes converted via the last lap time) -- lets
        // other widgets (e.g. tyre life) compare against "laps left" without
        // caring which kind of session this is. -1 = not enough data yet.
        double raceLapsRemainingEstimate = -1.0;

        // Fuel that would be needed to cover the SESSION's remaining
        // laps/time at the current learned consumption rate -- compare
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
    bool m_wasInPits = false;
    bool m_inStint = true;
    short m_stintStartLaps = -1;
    double m_stintStartFuel = -1.0;
    double m_stintStartET = -1.0; // ScoringInfoV01::mCurrentET at stint start

    CRITICAL_SECTION m_lock;
    bool m_ready = false;
    State m_state;

    std::string m_key; // sanitized "vehicle__track"

    long m_lastLapNumber = -1;
    double m_fuelAtLapStart = 0.0;
    bool m_haveLapStartFuel = false;
    double m_lastLapTimeSeconds = 0.0;
    double m_prevElapsedTime = 0.0;
    double m_prevLapStartET = 0.0;

    // The race's TOTAL laps/duration -- exact and refreshed every tick for a
    // lap race (mMaxLaps), or a running maximum of remaining time for a timed
    // one (see the OnScoring implementation for why a max rather than a
    // single captured sample). Used for fuelRequiredForRace, as opposed to
    // lapsLeft/raceMinutesLeft above
    // which are the REMAINING distance from right now.
    bool m_haveRaceTotal = false;
    double m_raceTotalLaps = -1.0;
    double m_raceTotalSeconds = -1.0;

    // Rolling average of the race LEADER's completed-lap times (not the
    // player's own), used only to turn a timed race's total duration into an
    // estimated total lap count -- see State::raceTotalLaps.
    long m_leaderLastTotalLaps = -1;
    static const int kLeaderLapHistorySize = 5;
    double m_leaderLapHistory[kLeaderLapHistorySize] = {};
    int m_leaderLapHistoryCount = 0;
    int m_leaderLapHistoryNext = 0;
    double m_leaderAvgLapTime = -1.0;

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
    // cleared by FlushIfDirty() once actually written.
    bool m_dirty = false;

    // Fuel-learning validity gate: a completed lap is only folded into the
    // learned average if it is BOTH a lap that counts for time (rF2's own
    // mCountLapFlag) AND within this many seconds of the personal best for
    // the car+track -- an out-lap, a spin, or an off-line cruise lap would
    // otherwise drag the estimate away from real racing consumption.
    //
    // Fuel is only ever measurable on the telemetry thread (TelemInfoV01::
    // mFuel has no scoring-side equivalent) while lap validity is only ever
    // known on the scoring thread (VehicleScoringInfoV01::mCountLapFlag has
    // no telemetry-side equivalent) -- see LapDeltaStore's own latch for the
    // same reasoning. Telemetry fires far more often than scoring, so
    // whichever side notices the lap boundary first hands its half off in
    // a single pending slot for the other side to complete; all of it is
    // guarded by m_lock like everything else shared between the two.
    static constexpr double kPbToleranceSeconds = 5.0;
    static constexpr double kPbToleranceFraction = 0.05; // whichever of the two is larger

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

    bool m_haveScoringVerdict = false;  // a scoring-side verdict is waiting on a sample
    bool m_pendingVerdictUsable = false;

    // Folds one lap's consumption into the learned history. Caller must
    // already hold m_lock.
    void ApplyLearnedLap(double consumed);
    double MedianOfHistory() const;

    void RecomputeFuelPerLap();
    void LoadFuelStats(const char* vehicleName, const char* trackName);
    void SaveFuelStats() const;
};

extern FuelStore g_fuelStore;
