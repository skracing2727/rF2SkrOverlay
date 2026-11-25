#pragma once
#include <Windows.h>
#include <vector>
#include <utility>

// Relative timing table: in a race, the leader (P1) plus a window of two
// places ahead and two behind the player; in practice/qualy/warmup (where
// "place" is just a best-lap ranking and says nothing about who is near you)
// the physically closest cars instead. Deduplicated either way, so a small
// field or a player near the front/back naturally shows fewer than the usual
// 6 rows rather than padding with something meaningless.
//
// Each row's "gap" is a TIME gap derived from a lap-distance -> time trace of
// the player's own most recent complete lap: look up when that lap passed the
// player's current distance, look up when it passed the other car's, and the
// difference is how many seconds of driving separate the two track positions.
//
// The naive alternative -- (distance between the cars) / (current speed) --
// is what this replaced, and it was badly wrong: the SAME physical separation
// reads as a huge gap while you crawl through a chicane and a tiny one once
// you are on the straight, so the number lurched around constantly with no
// real change in the race situation. Dividing by a single instantaneous speed
// implicitly assumes the whole gap is covered at that speed; a trace of the
// actual lap accounts for the corners and straights genuinely lying between
// the two cars.
//
// The SDK's own mTimeBehindLeader isn't usable either -- it only refreshes
// when a car crosses the start/finish line, which left the table looking
// frozen most of the time.
class RelativeTimingStore
{
public:
    void Init();
    // Called from the game's scoring callback (~5Hz -- this is where
    // per-vehicle place/gap data lives).
    void OnScoring(const struct ScoringInfoV01& info);
    void Clear();

    // Sized for the largest the user-adjustable "cars shown" slider allows
    // (see RelativeTimingConfig::visibleCars) plus room for a few "also
    // right next to you on track" duplicates -- see Row::trackRelative --
    // and the leader's own pinned row when you're last place.
    static const int kMaxRows = 20;
    static const int kMaxTrackRelativeRows = 3;

    struct Row
    {
        bool valid = false;
        bool isPlayer = false;
        unsigned char place = 0;
        // Slot ID, carried so this car can be looked up in stores keyed by
        // ID -- the tyre compound, which lives in telemetry rather than in
        // anything this row already holds (see Lookups below).
        long carId = -1;
        char driverName[32] = {};
        char carNumber[8] = {};      // from the entry name's "#19", empty if it has none
        double lastLapTime = -1.0;   // -1 = no completed lap yet

        // This car is flagged as in the pit lane while the player is out on
        // track. (The SDK says this flag is not always accurate for remote
        // cars, so it is only used to keep such a car out of the "about to
        // lap you" rows -- never to decide a gap.)
        bool inPits = false;
        // This car is in the pit lane at all (whatever the player is doing):
        // its gap shows as "PIT" instead of a number.
        bool carInPits = false;
        // Barely moving (parked in the pits, or stopped on track). Its gap is
        // still real -- the time until one car reaches the other -- but it
        // changes at a second per second only because the player is the one
        // moving, so it carries no trend: shown in a neutral colour instead of
        // red/green.
        bool stopped = false;

        // The same car measured BOTH ways round the lap, so a nearly empty
        // track can show one opponent ahead and behind at once.
        //  - gapAheadSeconds (<= 0): how long the PLAYER needs to reach this
        //    car's position, at the player's own pace.
        //  - gapBehindSeconds (>= 0): how long THIS car needs to reach the
        //    player's position, at ITS OWN pace -- its previous lap time
        //    scaled over the player's reference lap.
        double gapAheadSeconds = 0.0;
        double gapBehindSeconds = 0.0;

        // A car on a DIFFERENT lap can matter twice over: once for the race
        // standings (the leader is two minutes up the road) and once for
        // where it physically is right now (that same leader is two seconds
        // behind you, about to lap you). A place-based window can never
        // surface the second one -- if you are last, nobody is "2 places
        // behind" you -- so such a car gets a second row with the
        // track-relative gap instead of the standings gap. True marks that
        // duplicate; lapDelta is their lap count minus yours (0 = same lap,
        // positive = they are ahead by that many laps).
        bool trackRelative = false;
        int lapDelta = 0;
        // True once a real distance-to-time reference (this session's own
        // clean lap, or a saved personal best) was available to compute
        // trackRelativeGapSeconds -- false means it fell all the way back
        // to the crude constant-speed guess, which can be flat-out wrong
        // (including sign) early in a session/race before either reference
        // exists. Extra track-relative rows are only shown when true: a
        // second, less-trustworthy number that can contradict the main
        // (SDK-accurate) standings gap is worse than not showing it yet.
        bool trackGapReliable = false;
        double trackRelativeGapSeconds = 0.0;
        double trackRelativeGapSecondsPrev = 0.0; // paired with gapPrevTime/gapCurTime below, same as gapSecondsPrev
        double trackRelativeTrendPerSecond = 0.0;

        // Signed gap-to-PLAYER: negative = that car is ahead of the player,
        // positive = behind. The player's own row is forced to exactly 0
        // (comparing yourself to yourself is meaningless) -- the leader's
        // row shows the number that would otherwise have lived there
        // (gap between the player and the leader), via this same formula.
        double gapSeconds = 0.0;
        double trendPerSecond = 0.0; // d(gapSeconds)/dt, see RelativeTiming.cpp for sign convention; always 0 for the player's own row
        LARGE_INTEGER updatedAt = {};

        // For smooth (rather than steppy) display: gapSeconds above is the
        // most recent real ~5Hz scoring sample ("cur"); gapSecondsPrev is
        // the ONE BEFORE that. Draw() interpolates between the two based on
        // how far into the current ~200ms scoring interval "now" falls,
        // the same delayed-interpolation technique CarState.cpp uses for
        // the radar -- bounded between two real samples rather than
        // guessing forward, at the cost of a ~1-tick presentation delay.
        double gapSecondsPrev = 0.0;
        LARGE_INTEGER gapPrevTime = {};
        LARGE_INTEGER gapCurTime = {};

        // Lap-time coloring vs the player's OWN last lap (non-player rows
        // only, and only once both sides have a completed lap to compare).
        bool lapFasterThanPlayer = false; // their last lap beat yours -- red
        bool lapSlowerThanPlayer = false; // yours beat theirs -- green

        // At least one full lap behind the player -- RACE sessions only,
        // where laps completed is meaningful (in practice everyone has run a
        // different number of laps, so it says nothing). Colours the place
        // and name blue so a car you are lapping is obvious at a glance.
        bool lapDown = false;

        // Places gained (+) or lost (-) over this car's last completed lap:
        // its place when it crossed the line most recently versus the time
        // before. Race sessions only; haveLastLapGain false until the car
        // has crossed the line twice while being watched.
        bool haveLastLapGain = false;
        int lastLapGain = 0;

        // This car's last lap is the fastest lap of the session so far.
        bool lastLapIsSessionFastest = false;

        // No gap to show (gap box outside a race when either car has no best
        // lap yet): drawn blank rather than a misleading number.
        bool gapUnknown = false;

        // Gap colour, judged once per sector (see UpdateSectors): +1 the
        // player was faster through the last sector both ran (green), -1
        // this car was (red), 2 this car's sector was the session best
        // (purple), 0 nothing to compare yet (plain).
        int sectorVerdict = 0;

        // Recently changed track position (this car's own mPlace, not the
        // row's place in this table) -- -1 = just gained a place, +1 = just
        // lost one, 0 = no recent change. placeChangeTime is when it was
        // confirmed (QPC); Draw() fades the highlight from that on its own
        // clock, so it still fades if the table freezes (review F364).
        int placeChangeSign = 0;
        LARGE_INTEGER placeChangeTime = {};

        // The tyre compound (TyreStore) and laps since the last stop
        // (PitStint), looked up when the row is published rather than by
        // Draw() for every row on every frame: both stores sit behind their
        // own locks, which the render thread then never has to take. The
        // defaults are what a car neither store knows about shows.
        struct Lookups
        {
            bool haveTyre = false;
            char tyre[8] = {};       // "S", or "S/M" for a split set
            float tyreR = 1.0f, tyreG = 1.0f, tyreB = 1.0f;
            float tyreRearR = 1.0f, tyreRearG = 1.0f, tyreRearB = 1.0f; // a split set's rear letter
            bool pitLapsKnown = false;
            int pitLaps = 0;
        };
        Lookups lookups;
    };

    struct State
    {
        int rowCount = 0;
        Row rows[kMaxRows];

        // Fractional lap counters -- the whole-number part is laps actually
        // COMPLETED, the fraction is live progress through the lap in
        // progress (mLapDist / track length), so the number visibly ticks
        // up continuously rather than jumping only at the line -- no
        // ambiguity about whether it counts the lap you're currently on.
        // Total is laps completed this whole session; Stint is the same but
        // zeroed at the start of the current stint (the last pit exit, or
        // session start if there hasn't been one yet).
        double stintLapsFractional = 0.0;
        double totalLapsFractional = 0.0;

        // Fastest lap of the session by anyone (from every car's best lap),
        // and who set it. fastestLapSeconds <= 0 = none yet.
        double fastestLapSeconds = -1.0;
        // Your own best lap this session (footer "PB"); <= 0 = none yet.
        double myBestLapSeconds = -1.0;
        char fastestLapName[32] = {};

        // Gap box: the two cars placed just ahead of the player, the player,
        // and the one just behind, picked from the whole field by place
        // (retired cars skipped) -- not from the standings rows above, whose
        // window can leave those places out.
        int gapRowCount = 0;
        Row gapRows[5];
    };
    State Read();

private:
    // Per-car (keyed by the game's own slot ID, NOT row index -- an overtake
    // can change which car occupies a given place between ticks, so trend
    // history must follow the car, not the row) trend bookkeeping.
    struct TrendTracker
    {
        double lastGap = 0.0;
        LARGE_INTEGER lastTime = {};
        double trendPerSecond = 0.0;
        bool has = false;

        // The DISPLAYED gap number: the latest scoring sample, shifted on
        // every ~5Hz tick (Draw() slides between it and prevDisplayedGap).
        double displayedGap = 0.0;
        LARGE_INTEGER lastDisplayTime = {};

        // The value displayedGap held BEFORE the most recent update, and
        // when that update was -- Draw() interpolates between this and
        // displayedGap rather than snapping straight to it, so the table
        // animates smoothly between real samples instead of stepping.
        double prevDisplayedGap = 0.0;
        LARGE_INTEGER prevDisplayTime = {};

        // Same again for the track-relative gap, which a car on a different
        // lap needs in parallel with its standings gap (see Row::trackRelative).
        // Driven off the same two timers above so both stay in step.
        double lastTrackGap = 0.0;
        double trackTrendPerSecond = 0.0;
        double displayedTrackGap = 0.0;
        double prevDisplayedTrackGap = 0.0;

        // Position-change highlight bookkeeping, keyed by this same car ID
        // so an overtake is attributed to the car that moved, not to
        // whichever row it happens to occupy. A raw mPlace change is only
        // confirmed once the NEW place has held steady for a debounce
        // period -- early in a race (first lap, first corner) places swap
        // back and forth within a second as cars jostle, and without this a
        // place that immediately reverts would still flash a highlight.
        int lastConfirmedPlace = -1;     // -1 = not seen yet, so the first sample just anchors
        int pendingPlace = -1;           // place currently being observed for the debounce window
        LARGE_INTEGER pendingPlaceTime = {};
        int placeChangeSign = 0;         // -1 = last confirmed change was a gain, +1 = a loss, 0 = none pending
        LARGE_INTEGER placeChangeTime = {};

        // Places gained over the car's last completed lap (see Row).
        long lapsSeen = -1;
        int placeAtLine = -1;
        bool haveLastLapGain = false;
        int lastLapGain = 0;

        // Per-sector colouring (see UpdateSectors): this car's most recent
        // time through each of the 3 sectors (-1 = not clean / none yet),
        // the session clock when it finished it, and whether it was the
        // session best at that moment.
        int lastSectorSeen = -1;         // mSector as last seen, -1 = not yet
        double sectorTime[3] = { -1.0, -1.0, -1.0 };
        double sectorDoneET[3] = { -1.0e9, -1.0e9, -1.0e9 };
        bool sectorWasBest[3] = {};
        int sectorVerdict = 0;           // see Row::sectorVerdict
    };

    CRITICAL_SECTION m_lock;
    bool m_ready = false;
    // Set once the race is over (chequered flag): the published table is
    // then left as it was, since the game's own gap figures stop making
    // sense after the leader finishes. Cleared with the session.
    bool m_raceOverFrozen = false;
    // What the freeze was set in, so a NEW session lifts it on its own.
    long m_frozenSession = -1;
    double m_frozenET = 0.0;

    // The player's recent completed lap times (ring of 7). Their median is
    // the lap time used to turn "N laps behind" into seconds in a race --
    // the single last lap swung 86-101 s in one race, which made every
    // lapped car's gap jump by that much times N at each line crossing.
    static const int kLapHistory = 7;
    double m_lapHistory[kLapHistory] = {};
    int m_lapHistoryCount = 0;
    int m_lapHistoryNext = 0;
    double SteadyLapTime() const;
    State m_state;
    LARGE_INTEGER m_freq = {};

    // Sized to the SDK's 128 cars, and trackers of cars gone from scoring
    // (or whose ID now carries another driver) are freed each tick -- with
    // 64 and no freeing, the 65th car shared a slot and showed another
    // car's gap (review F82/F95).
    static const int kMaxTrackedCars = 128;
    long m_trackerCarId[kMaxTrackedCars] = {};
    TrendTracker m_tracker[kMaxTrackedCars] = {};
    char m_trackerName[kMaxTrackedCars][32] = {};
    int m_trackerMissingTicks[kMaxTrackedCars] = {};
    int m_trackerCount = 0;
    void PruneTrackers(const struct ScoringInfoV01& info);

    // The lap being traced touched the pits or was disturbed (a long pause
    // between samples, lap distance going backwards): it may not become
    // the reference lap (review F78).
    bool m_curLapRejected = false;
    double m_lastTimeIntoLapForTrace = -1.0;

    // Stint tracking (scoring thread only): baseline lap count at the start
    // of the current stint, re-anchored on every PIT EXIT (mInPits true ->
    // false), the same edge FuelStore.cpp already uses for its own stint
    // tracking. Originally keyed off mNumPitstops incrementing instead, but
    // that only increments once rF2 registers a "completed" stop -- driving
    // into the pits and back out without a real dwell (as when just testing
    // the box, or in some practice-session pit visits) never increments it,
    // so the stint counter looked stuck. mInPits toggles on the pit-lane
    // boundary itself and needs no such completion.
    double m_stintStartTotalLaps = 0.0;
    bool m_havePitState = false; // false = not yet observed, so the very first sample just anchors rather than firing a spurious reset
    bool m_wasInPits = false;

    // Lap-distance -> time-into-lap trace of the player's own driving, used
    // to convert a track-position difference into a real time gap (see the
    // class comment). m_currentTrace accumulates as the lap is driven and is
    // promoted to m_referenceTrace when a plausible lap completes; until the
    // first one does, there is no reference and the gap falls back to the
    // cruder distance/current-speed estimate.
    std::vector<std::pair<double, double>> m_currentTrace;
    std::vector<std::pair<double, double>> m_referenceTrace;
    // Running maximum of m_referenceTrace's distances, index for index --
    // always in order even where the trace itself steps back, so
    // InterpolateTraceAt can binary-search it (see there).
    std::vector<double> m_referenceMaxDist;
    double m_referenceLapTime = -1.0;
    short m_lastTotalLapsForTrace = -1;
    double m_lastLapDistForTrace = -1.0;

    // Time into the reference lap at the given lap distance. False if there
    // is no usable reference trace yet.
    bool InterpolateTraceAt(double lapDist, double& outTime) const;

    TrendTracker& GetTracker(long carId);

    // Each car's row as already worked out this tick, by its index in the
    // scoring array, and its tracker (see computeRow in OnScoring). Scoring
    // thread only; a member rather than a local only to keep ~40 KB off
    // the callback's stack.
    static const int kMaxRowCache = 128;
    Row m_rowCache[kMaxRowCache];
    TrendTracker* m_rowCacheTracker[kMaxRowCache] = {};
    // Nearest-cars candidates (whole field + one wrap row); a member for
    // the same stack reason.
    Row m_candidates[kMaxRowCache + 1];

    // Session-best time for each sector, by anyone (-1 = none yet).
    double m_sessionBestSector[3] = { -1.0, -1.0, -1.0 };
    // Notes every car that finished a sector this tick and re-judges the
    // affected rows' colours against the player's time for that sector.
    void UpdateSectors(const struct ScoringInfoV01& info, const struct VehicleScoringInfoV01* player);
};

extern RelativeTimingStore g_relativeTiming;

class Renderer;

namespace RelativeTiming
{
    // Draws into the current frame. Returns false if there is nothing to show.
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight);

    // The gap box: the same table cut down to the two places ahead of you,
    // you, and the one place behind -- no header or footer.
    bool DrawGapBox(Renderer& renderer, int screenWidth, int screenHeight);

    // Screen Y of the standings panel's own bottom edge as it was LAST
    // DRAWN, padding included. The table's height varies with how many rows
    // are in range, so anything positioned under it has to follow this
    // rather than a fixed fraction, or it ends up hidden behind it.
    float BottomEdgeY(int screenWidth, int screenHeight);
}
