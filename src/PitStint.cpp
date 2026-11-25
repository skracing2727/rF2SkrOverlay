#include "PitStint.h"
#include "InternalsPlugin.hpp"
#include <Windows.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace
{
    // Scoring thread only -- no lock needed.
    struct Car
    {
        long id = -1;             // slot ID
        char name[64] = {};       // vehicle (entry) name, as in VehicleScoringInfoV01::mVehicleName
        int exitLaps = -1;        // completed laps when it last left the pits after a stop; -1 = unknown
        bool inPits = false;
        bool seen = false;
        // The current pit visit (review F6): only a real stop resets the count.
        double visitStartET = 0.0;
        double stillSeconds = 0.0;
        short stopsAtEntry = 0;
        bool stopped = false;
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

    // Keyed by slot ID + name (review F6): the name alone let two cars with
    // the same default livery share a record; the ID alone can be re-used
    // by someone else after a driver leaves. mVehicleName is 64 chars and
    // need not be terminated.
    Car& FindCar(long id, const char* name)
    {
        for (Car& c : s_cars)
            if (c.id == id && strncmp(c.name, name, sizeof(c.name)) == 0)
                return c;
        s_cars.emplace_back();
        s_cars.back().id = id;
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
        s_lastET = -1.0;
    }
    const double dt = (s_lastET >= 0.0) ? (std::min)((std::max)(info.mCurrentET - s_lastET, 0.0), 1.0) : 0.0;
    s_lastET = info.mCurrentET;

    s_build.clear();
    for (long i = 0; info.mVehicle && i < info.mNumVehicles; ++i)
    {
        const VehicleScoringInfoV01& v = info.mVehicle[i];
        Car& c = FindCar(v.mID, v.mVehicleName);
        // The garage stall counts as the pit lane (CrewChief review
        // 2026-11-24; its RF2GameStateMapper does the same).
        const bool inPits = v.mInPits || v.mInGarageStall;
        if (!c.seen)
        {
            c.seen = true;
            c.inPits = false;   // so a car first seen in the pits opens a visit below
            if (c.exitLaps < 0 && v.mNumPitstops == 0) c.exitLaps = 0;   // no stop yet: count from the start
        }
        if (inPits && !c.inPits)
        {
            c.visitStartET = info.mCurrentET;
            c.stillSeconds = 0.0;
            c.stopsAtEntry = v.mNumPitstops;
            c.stopped = false;
        }
        if (inPits)
        {
            // A real stop: stationary for over a second, rF2's "stopped"
            // pit state, or the stop counter going up (as TinyPedal judges).
            const double speed = sqrt(v.mLocalVel.x * v.mLocalVel.x + v.mLocalVel.y * v.mLocalVel.y
                                      + v.mLocalVel.z * v.mLocalVel.z);
            if (speed < 0.5)
                c.stillSeconds += dt;
            if (c.stillSeconds > 1.0 || v.mPitState == 3 || v.mNumPitstops > c.stopsAtEntry)
                c.stopped = true;
        }
        else if (c.inPits)
        {
            // Left the lane: a pass under 2 s is mInPits flicker, and a
            // drive-through has no stop -- neither resets the count.
            const bool longEnough = info.mCurrentET - c.visitStartET > 2.0;
            if (longEnough && (c.stopped || v.mNumPitstops > c.stopsAtEntry))
                c.exitLaps = v.mTotalLaps;
        }
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
