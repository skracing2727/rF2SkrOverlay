#pragma once
#include <Windows.h>

// Warnings drawn as yellow lines above the gear/speed boxes (GearDisplay.cpp
// draws them; this only works out what to say):
//
//  - A stopped car AHEAD, with the distance to it. A car counts as stopped
//    under TrackAlertConfig::stoppedKmh (40 km/h) -- in a corner your own
//    best lap took nearly that slowly (LapDelta's saved best-lap trace), only
//    under slowCornerFraction of your speed there. Stopped cars within
//    groupMeters of each other are one incident (a pile-up): one line, the
//    distance to the first of them and how many there are. It
//    has to stay like that for dwellSeconds before it is reported, so a car
//    that only runs wide does not flash up. No sound: the line appearing is
//    the warning. Cars in the pit lane, cars that have taken the flag, the start, and
//    full-course yellows are left out -- everyone is slow then.
//  - Which sector has a yellow flag (ScoringInfoV01::mSectorFlag; only set
//    when the server runs local yellows), and a full-course yellow.
//  - That a pit stop is requested, so GearDisplay can show the pit-lane
//    limit above the speed box until the car is in the pit lane.
//
// All of it is worked out on the scoring callback (~5 Hz, every car); the
// render thread only copies the result. Nothing here touches the disk.
class TrackAlertStore
{
public:
    void Init();
    // Scoring thread. Call after g_lapDelta.OnScoring: it reads the best-lap
    // trace LapDelta owns on that same thread.
    void OnScoring(const struct ScoringInfoV01& info);
    void Clear();

    struct State
    {
        bool valid = false;
        // Nearest incident ahead, within range: one stopped car, or several
        // close together.
        bool haveCar = false;
        int carCount = 0;            // cars in the incident
        bool carOffTrack = false;    // ALL of them beyond the track edge
        int carSector = 0;           // first car's sector, 1..3, 0 = not known
        double carDistM = 0.0;       // forward distance to the first, at the scoring tick
        double closingMs = 0.0;      // your speed minus its speed, for counting down between ticks
        LARGE_INTEGER tickTime = {}; // QPC time of that tick
        // Yellow flag per sector, S1..S3, and full course.
        bool sectorYellow[3] = {};
        bool fullCourseYellow = false;
        // Full-course yellow stage, ScoringInfoV01::mYellowFlagState while
        // fullCourseYellow: 1 FCY called, SC not out yet (pending), 2 pits
        // closed in the SDK's naming but treated by CrewChief as pits open
        // under standard rules, 3 pits open for the lead lap, 4 pits open,
        // 5 last lap, 7 race halt. 6 (resume) never shows here: it ends the
        // full-course yellow (CrewChief review 2026-11-24).
        int yellowStage = 0;
        // The player is being shown the blue flag (mFlag == 6).
        bool blueFlag = false;
        // Outstanding penalties (mNumPenalties). rF2 does not say which kind.
        int penalties = 0;
        // A pit stop is requested and the car is not in the pit lane yet.
        bool pitRequested = false;
    };
    State Read();

    // The distance to the reported car now, counted down from the last tick
    // by the closing speed (the scoring tick only comes 5 times a second).
    static double DistanceNow(const State& s);

private:
    SRWLOCK m_lock = SRWLOCK_INIT;
    State m_state;
    bool m_ready = false;

    // Scoring thread only.
    struct CarTrack
    {
        long id = -1;
        double slowSince = -1.0;   // sim time it went slow, -1 = not slow
        double fastSince = -1.0;   // sim time it got back up to speed while flagged
        bool flagged = false;      // has been slow for the dwell
        bool announced = false;    // logged for this episode
        double lastSeen = -1.0;    // sim time this id was last in scoring
    };
    // Looked up by the real mID, not mID % kSlots: two ids 256 apart used to
    // share a slot and reset each other every tick (review F370).
    CarTrack* TrackFor(long id, double et);
    static const int kSlots = 256;
    CarTrack m_cars[kSlots];
    int m_lastGamePhase = -1;
    double m_greenSince = -1.0;
    signed char m_lastSectorFlag[3] = { -1, -1, -1 };
};

extern TrackAlertStore g_trackAlerts;
