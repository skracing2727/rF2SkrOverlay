#pragma once
#include <Windows.h>

// Compact snapshot of just what the radar needs, handed from the game's scoring
// thread to the render thread.
//
// Scoring only arrives ~5 times a second, which would look like visible stepping
// at 144fps. Rather than dead-reckoning forward from a single instantaneous
// velocity/yaw-rate sample (which overshoots badly whenever that rate spikes and
// reverses within one scoring interval -- exactly what a sliding car's yaw rate
// does), every render reads a straight interpolation BETWEEN the two most
// recently received real samples for each car. That means the displayed state is
// always bounded by two points the game actually reported -- never a guess that
// can run ahead of reality -- at the cost of a constant ~1 scoring-interval
// (~0.2s) presentation delay, applied uniformly to every car (so the RELATIVE
// positions/headings the radar actually shows are unaffected).
//
// The measured interval between those two samples is CAPPED (see Read()), so an
// interrupted scoring stream can't stretch one stale step out into seconds of
// slow-motion drift.
struct CarPos
{
    long id;          // VehicleScoringInfoV01::mID -- stable per car across
                      // ticks, so a caller can track one specific car frame
                      // to frame (ProximityHue fades each car's contribution
                      // in and out individually).
    double x, z;      // world position, ground plane (interpolated)
    double heading;   // world yaw, radians (interpolated)
    bool isPlayer;
    // Which entry of the footprint table below this car's class/name matched
    // (-1: none). Worked out on the scoring thread, and only when a car's
    // class or name actually changes -- it used to be up to 28 case-
    // insensitive substring scans per car per frame in the radar's draw.
    int footprint;
    // Pit lane / garage stall flags and ground speed (m/s) from scoring, so the
    // radar and side bars can mute cars behind the pit wall (review F129).
    bool inPits;
    bool inGarage;
    float speedMps;
    // Lap distance (m) from scoring. Negative means the game has no real
    // position for the car yet, so the radar and side bars skip it
    // (CrewChief review 2026-11-24).
    double lapDist;
};

// Typical real footprint for a handful of recognisable categories, matched
// against the car's own VehicleScoringInfoV01 class and name (see CarState.cpp
// for the table and where each figure comes from). The SDK exposes no actual
// per-car dimension field, so this is not exact for any one specific car --
// but it is a genuine improvement over one flat box shared by every car on a
// mixed grid. Returns false for -1 (no category matched).
struct CarFootprint { float halfWidthM, halfLengthM, cornerRadiusM; };
bool GetCarFootprint(int footprint, CarFootprint& out);

// One frame's interpolated read of every car (see CarStateStore::Read). The
// radar and the side bars draw from the same one, so they cannot disagree by
// the few microseconds between two separate reads either.
struct CarSnapshot
{
    static const int kMaxCars = 128;
    CarPos cars[kMaxCars];
    int count = 0;
    double playerHeading = 0.0;
    bool hasPlayer = false;
};

class CarStateStore
{
public:
    void Init();
    // NOTE: must be 'struct' -- ScoringInfoV01 is declared as a struct in the SDK,
    // and class/struct mismatch changes MSVC name mangling (link error).
    void Update(const struct ScoringInfoV01& info);
    // Every car's telemetry (scope 2), ~50 Hz: a fresher position/heading/
    // speed than the ~5 Hz interpolated scoring path, which is ~0.2 s behind.
    // Read() uses it for a car while it is under 0.5 s old, else falls back
    // to scoring. Scoring still owns the car list, identity and pit flags.
    void OnTelemetry(const struct TelemInfoV01& info);
    // Copies the latest snapshot out, interpolated to 'now'.
    int Read(CarPos* out, int maxCars, double& outPlayerHeading, bool& outHasPlayer);
    void Read(CarSnapshot& out) { out.count = Read(out.cars, CarSnapshot::kMaxCars, out.playerHeading, out.hasPlayer); }
    void Clear();

private:
    static const int kMaxCars = 128;

    struct CarEntry
    {
        long id = -1;       // VehicleScoringInfoV01::mID -- stable per car, used to
                             // match a car between the previous and current sample
                             // sets even if the game reorders mVehicle[] between ticks
                             // (e.g. by running order).
        double x, z;
        double heading;
        bool isPlayer;
        // Index of the same car (by id) in the previous sample set, or -1 --
        // found once per scoring tick in Update, instead of searched for by
        // every Read.
        int prevIndex = -1;
        int footprint = -1;
        bool inPits = false;
        bool inGarage = false;
        float speedMps = 0.0f;
        double lapDist = 0.0;
        // Kept only to notice a class/name change (and so redo 'footprint').
        char vehicleClass[32];
        char vehicleName[64];
    };

    CRITICAL_SECTION m_lock;
    bool m_ready = false;

    // Three sample sets: "cur" (the most recently received real sample) and
    // "prev" (the one before it), which every Read() interpolates between,
    // and a spare one Update fills without the lock. Taking the lock then only
    // swaps the indices, so the render thread never waits on the scoring
    // thread's per-car work. Only Update writes the buffers.
    CarEntry m_cars[3][kMaxCars] = {};
    int m_cur = 0, m_prev = 1, m_spare = 2;   // m_cur/m_prev change under m_lock; m_spare is Update's

    int m_curCount = 0;
    LARGE_INTEGER m_curTime = {};
    int m_prevCount = 0;
    LARGE_INTEGER m_prevTime = {};
    // Sim time (mCurrentET) of each sample: the real spacing between them,
    // unlike QPC arrival times which bunch after a hitch (review F130).
    double m_curET = -1.0;
    double m_prevET = -1.0;

    bool m_hasPlayer = false;
    LARGE_INTEGER m_freq = {};

    // Latest telemetry per car, keyed by mID; written by OnTelemetry and
    // read by Read(), both under m_lock (copy of a few fields only).
    struct TelemEntry { long id; double x, z, heading; float speedMps; LONGLONG t; };
    TelemEntry m_telem[kMaxCars] = {};
    int m_telemCount = 0;
};

extern CarStateStore g_carState;
