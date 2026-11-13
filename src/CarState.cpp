#include "CarState.h"
#include "InternalsPlugin.hpp"
#include <cmath>
#include <cstring>

CarStateStore g_carState;

namespace
{
    // Shortest-path interpolation between two angles, so a car crossing the
    // -pi/+pi wrap doesn't spin the long way around.
    double LerpAngle(double a, double b, double t)
    {
        double diff = b - a;
        while (diff > 3.14159265358979) diff -= 2.0 * 3.14159265358979;
        while (diff < -3.14159265358979) diff += 2.0 * 3.14159265358979;
        return a + diff * t;
    }
}

void CarStateStore::Init()
{
    if (m_ready)
        return;
    InitializeCriticalSection(&m_lock);
    QueryPerformanceFrequency(&m_freq);
    m_ready = true;
}

const CarStateStore::CarEntry* CarStateStore::FindPrev(long id) const
{
    for (int i = 0; i < m_prevCount; ++i)
    {
        if (m_prevCars[i].id == id)
            return &m_prevCars[i];
    }
    return nullptr;
}

void CarStateStore::Update(const ScoringInfoV01& info)
{
    if (!m_ready)
        return;

    EnterCriticalSection(&m_lock);

    // The previous sample set becomes whatever was "current" until now.
    for (int i = 0; i < m_curCount; ++i)
        m_prevCars[i] = m_curCars[i];
    m_prevCount = m_curCount;
    m_prevTime = m_curTime;

    m_curCount = 0;
    m_hasPlayer = false;

    if (info.mVehicle != nullptr)
    {
        const int n = info.mNumVehicles < kMaxCars ? info.mNumVehicles : kMaxCars;
        for (int i = 0; i < n; ++i)
        {
            const VehicleScoringInfoV01& v = info.mVehicle[i];
            CarEntry& e = m_curCars[m_curCount];

            e.id = v.mID;
            e.x = v.mPos.x;
            e.z = v.mPos.z;

            // mOri[2] is the row mapping local forward into world space.
            e.heading = atan2(v.mOri[2].x, v.mOri[2].z);

            e.isPlayer = v.mIsPlayer != 0;
            if (e.isPlayer)
                m_hasPlayer = true;

            strncpy_s(e.vehicleClass, v.mVehicleClass, _TRUNCATE);
            strncpy_s(e.vehicleName, v.mVehicleName, _TRUNCATE);

            ++m_curCount;
        }
    }

    QueryPerformanceCounter(&m_curTime);

    LeaveCriticalSection(&m_lock);
}

int CarStateStore::Read(CarPos* out, int maxCars, double& outPlayerHeading, bool& outHasPlayer)
{
    if (!m_ready)
    {
        outHasPlayer = false;
        return 0;
    }

    EnterCriticalSection(&m_lock);

    // How long the two most recent real samples were apart, and how far
    // into that window "now" falls. Rendering is deliberately one interval
    // BEHIND the newest sample (frac in [0,1] interpolates strictly between
    // prev and cur) rather than projecting past it -- see CarState.h.
    double frac = 1.0; // no valid prev sample yet -> just show cur as-is
    if (m_freq.QuadPart != 0 && m_prevTime.QuadPart != 0 && m_curTime.QuadPart != 0)
    {
        double tickPeriod = static_cast<double>(m_curTime.QuadPart - m_prevTime.QuadPart) / static_cast<double>(m_freq.QuadPart);

        // A sample pair spanning far longer than the real ~5Hz scoring rate
        // means the stream was INTERRUPTED (pause, load, hitch) -- not that
        // the cars genuinely took that long to travel between the two. Left
        // uncapped, frac then stretches that single stale prev->cur step
        // across the whole gap, and every car visibly slides in slow motion
        // for as long as the gap lasted: pausing the game showed the radar
        // creeping for several seconds before finally settling. Capping the
        // period makes that last step play out at roughly the normal rate
        // and then hold still.
        //
        // Chosen well above the nominal 0.2s so genuinely slower scoring
        // (down to ~2Hz) still animates smoothly rather than finishing early
        // and stuttering to a stop before the next sample lands.
        const double kMaxTickPeriodSeconds = 0.5;
        if (tickPeriod > kMaxTickPeriodSeconds)
            tickPeriod = kMaxTickPeriodSeconds;

        if (tickPeriod > 0.001)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double elapsedSinceCur = static_cast<double>(now.QuadPart - m_curTime.QuadPart) / static_cast<double>(m_freq.QuadPart);
            if (elapsedSinceCur < 0.0) elapsedSinceCur = 0.0;
            // Beyond one tick period means scoring has stalled (pause, load,
            // hitch) -- freeze at cur rather than projecting further.
            if (elapsedSinceCur > tickPeriod) elapsedSinceCur = tickPeriod;
            frac = elapsedSinceCur / tickPeriod;
        }
    }

    const int n = m_curCount < maxCars ? m_curCount : maxCars;
    outPlayerHeading = 0.0;
    outHasPlayer = m_hasPlayer;

    for (int i = 0; i < n; ++i)
    {
        const CarEntry& cur = m_curCars[i];
        const CarEntry* prev = (frac < 1.0) ? FindPrev(cur.id) : nullptr;

        if (prev)
        {
            out[i].x = prev->x + (cur.x - prev->x) * frac;
            out[i].z = prev->z + (cur.z - prev->z) * frac;
            out[i].heading = LerpAngle(prev->heading, cur.heading, frac);
        }
        else
        {
            // No matching previous sample (car just appeared -- session
            // start, a join, or the very first tick) -- nothing to
            // interpolate from, so show the real value directly.
            out[i].x = cur.x;
            out[i].z = cur.z;
            out[i].heading = cur.heading;
        }
        out[i].id = cur.id;
        out[i].isPlayer = cur.isPlayer;
        strncpy_s(out[i].vehicleClass, cur.vehicleClass, _TRUNCATE);
        strncpy_s(out[i].vehicleName, cur.vehicleName, _TRUNCATE);

        if (cur.isPlayer)
            outPlayerHeading = out[i].heading;
    }

    LeaveCriticalSection(&m_lock);
    return n;
}

void CarStateStore::Clear()
{
    if (!m_ready)
        return;
    EnterCriticalSection(&m_lock);
    m_curCount = 0;
    m_prevCount = 0;
    m_curTime = LARGE_INTEGER{};
    m_prevTime = LARGE_INTEGER{};
    m_hasPlayer = false;
    LeaveCriticalSection(&m_lock);
}
