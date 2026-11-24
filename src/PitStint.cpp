#include "PitStint.h"
#include "InternalsPlugin.hpp"
#include <Windows.h>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace
{
    // Scoring thread only -- no lock needed.
    struct Car
    {
        char name[64] = {};       // vehicle (entry) name, as in VehicleScoringInfoV01::mVehicleName
        int exitLaps = -1;        // completed laps when it last left the pits; -1 = unknown
        bool inPits = false;
        bool seen = false;
    };
    std::vector<Car> s_cars;                 // every car seen this session, by name
    char s_sessionKey[160] = {};             // track|session|length
    double s_lastET = -1.0;
    // This tick's answers are built here, then swapped in under the lock;
    // the old list comes back as next tick's scratch, so no tick allocates
    // once the field has been seen.
    std::vector<std::pair<long, int>> s_build;

    // Shared with the render thread (PitStint::Get, once per drawn row): only
    // the finished list, sorted by slot ID (-1 laps = unknown). The lock is
    // held just for the swap, never while the list is being built.
    SRWLOCK s_lock = SRWLOCK_INIT;
    std::vector<std::pair<long, int>> s_lapsById;

    // mVehicleName is 64 chars and need not be terminated.
    Car& FindCar(const char* name)
    {
        for (Car& c : s_cars)
            if (strncmp(c.name, name, sizeof(c.name)) == 0)
                return c;
        s_cars.emplace_back();
        memcpy(s_cars.back().name, name, sizeof(s_cars.back().name));
        return s_cars.back();
    }
}

void PitStint::OnScoring(const ScoringInfoV01& info)
{
    // Session identity: track + session type + scheduled length. Time going
    // backwards (a restart of the session) also starts over. Nothing is kept
    // across sessions: laps since a stop only mean something within one.
    char key[160];
    _snprintf_s(key, _TRUNCATE, "%.64s|%ld|%.0f", info.mTrackName, info.mSession, info.mEndET);
    if (strcmp(key, s_sessionKey) != 0 || info.mCurrentET < s_lastET - 5.0)
    {
        strcpy_s(s_sessionKey, key);
        s_cars.clear();
    }
    s_lastET = info.mCurrentET;

    s_build.clear();
    for (long i = 0; info.mVehicle && i < info.mNumVehicles; ++i)
    {
        const VehicleScoringInfoV01& v = info.mVehicle[i];
        Car& c = FindCar(v.mVehicleName);
        const bool inPits = v.mInPits != 0;
        if (!c.seen)
        {
            c.seen = true;
            c.inPits = inPits;
            if (c.exitLaps < 0 && v.mNumPitstops == 0) c.exitLaps = 0;   // no stop yet: count from the start
        }
        if (c.inPits && !inPits)
            c.exitLaps = v.mTotalLaps;
        c.inPits = inPits;
        int laps = -1;
        if (c.exitLaps >= 0)
        {
            laps = v.mTotalLaps - c.exitLaps;
            if (laps < 0) { laps = -1; c.exitLaps = -1; }   // record does not fit this car: forget it
        }
        s_build.emplace_back(v.mID, laps);
    }
    std::sort(s_build.begin(), s_build.end());

    AcquireSRWLockExclusive(&s_lock);
    s_lapsById.swap(s_build);
    ReleaseSRWLockExclusive(&s_lock);
}

bool PitStint::Get(long carId, int& laps)
{
    bool known = false;
    AcquireSRWLockShared(&s_lock);
    const auto it = std::lower_bound(s_lapsById.begin(), s_lapsById.end(), std::make_pair(carId, INT_MIN));
    if (it != s_lapsById.end() && it->first == carId && it->second >= 0)
    {
        laps = it->second;
        known = true;
    }
    ReleaseSRWLockShared(&s_lock);
    return known;
}
