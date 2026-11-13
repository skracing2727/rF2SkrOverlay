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
    // Copied straight from VehicleScoringInfoV01 (not interpolated -- a car
    // does not change class mid-tick). Used to look up a real per-class
    // footprint for the radar's car icons instead of one guessed box shared
    // by every car on the grid; see Radar.cpp's LookupCarFootprint.
    char vehicleClass[32];
    char vehicleName[64];
};

class CarStateStore
{
public:
    void Init();
    // NOTE: must be 'struct' -- ScoringInfoV01 is declared as a struct in the SDK,
    // and class/struct mismatch changes MSVC name mangling (link error).
    void Update(const struct ScoringInfoV01& info);
    // Copies the latest snapshot out, interpolated to 'now'.
    int Read(CarPos* out, int maxCars, double& outPlayerHeading, bool& outHasPlayer);
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
        char vehicleClass[32];
        char vehicleName[64];
    };

    CRITICAL_SECTION m_lock;
    bool m_ready = false;

    // "cur" is the most recently received real sample; "prev" is the one
    // before that. Every Read() interpolates between them.
    CarEntry m_curCars[kMaxCars] = {};
    int m_curCount = 0;
    LARGE_INTEGER m_curTime = {};

    CarEntry m_prevCars[kMaxCars] = {};
    int m_prevCount = 0;
    LARGE_INTEGER m_prevTime = {};

    bool m_hasPlayer = false;
    LARGE_INTEGER m_freq = {};

    const CarEntry* FindPrev(long id) const;
};

extern CarStateStore g_carState;
