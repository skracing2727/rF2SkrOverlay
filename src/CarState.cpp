#include "CarState.h"
#include "InternalsPlugin.hpp"
#include <cmath>
#include <cstring>
#include <cctype>

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

    // Case-insensitive "does haystack contain needle" -- car class/name
    // strings arrive in whatever case the mod/content author used.
    bool ContainsCI(const char* haystack, const char* needle)
    {
        if (haystack[0] == '\0' || needle[0] == '\0')
            return false;
        const size_t hLen = strlen(haystack);
        const size_t nLen = strlen(needle);
        if (nLen > hLen)
            return false;
        for (size_t i = 0; i + nLen <= hLen; ++i)
        {
            size_t j = 0;
            for (; j < nLen; ++j)
            {
                if (tolower(static_cast<unsigned char>(haystack[i + j])) != tolower(static_cast<unsigned char>(needle[j])))
                    break;
            }
            if (j == nLen)
                return true;
        }
        return false;
    }

    // Typical real footprint for a handful of recognisable categories,
    // matched against the car's own VehicleScoringInfoV01 class and name.
    // The SDK exposes no actual per-car dimension field, so this is not
    // exact for any one specific car -- but it is a genuine improvement over
    // one flat box shared by every car on a mixed grid (a BTCC-style touring
    // car IS narrower and shorter than a GT3 car). Checked in order; the
    // first match wins. A car matching none of these keeps the configured
    // CarHalfWidthMeters/CarHalfLengthMeters/CarCornerRadiusMeters box,
    // exactly as with RadarConfig::useRealCarSize turned off (that part is
    // the radar's LookupCarFootprint in Overlay.cpp).
    //
    // Figures are half of a real published overall length/width for a
    // representative car in each class (converted from mm), not invented:
    //   GT3   -- Audi R8 LMS GT3 evo: 4600 x 1996mm
    //   GTE   -- Porsche 911 RSR (2017): 4557 x 2045mm (axle-width figure)
    //   LMP   -- Oreca 07 LMP2: 4745 x 1895mm (LMP1/LMP3 regs land within a
    //            few cm of this too -- Toyota TS050 4650x1900, Ligier
    //            JS P320 4605x1900 -- so one shared bucket stays accurate)
    //   NASCAR (Cup) -- Next Gen car: 4912 x 1996mm
    //   INDYCAR -- Dallara IR18, ~5080 x 2000mm (published figures for this
    //            one are inconsistent between sources; used the wider,
    //            more conservative reading)
    // GT4/TCR/TOURING/BTCC/FORMULA/PROTOTYPE/TRUCK are class-typical
    // approximations, not each individually sourced.
    struct CarCategory { const char* keyword; float halfWidthM, halfLengthM, cornerRadiusM; };
    const CarCategory kCarCategories[] =
    {
        { "GTE",       1.02f, 2.28f, 0.45f },  // ~2.05m x 4.56m
        { "GT3",       1.00f, 2.30f, 0.45f },  // ~2.0m x 4.6m
        { "GT4",       0.95f, 2.20f, 0.40f },  // ~1.9m x 4.4m
        { "TCR",       0.90f, 2.15f, 0.40f },  // ~1.8m x 4.3m
        { "TOURING",   0.90f, 2.15f, 0.40f },
        { "BTCC",      0.90f, 2.15f, 0.40f },
        { "TRUCK",     1.00f, 2.45f, 0.45f },  // NASCAR truck -- approximate, no verified source found
        { "NASCAR",    1.00f, 2.45f, 0.45f },  // ~2.0m x 4.9m
        { "DPI",       0.95f, 2.38f, 0.40f },  // ~1.9m x 4.75m
        { "LMP",       0.95f, 2.37f, 0.40f },  // ~1.9m x 4.75m
        { "PROTOTYPE", 0.95f, 2.37f, 0.40f },
        { "INDYCAR",   1.00f, 2.54f, 0.35f },  // ~2.0m x 5.08m
        { "IR18",      1.00f, 2.54f, 0.35f },
        { "FORMULA",   0.90f, 2.50f, 0.30f },  // open-wheel: narrower, longer
    };
    const int kCarCategoryCount = static_cast<int>(sizeof(kCarCategories) / sizeof(kCarCategories[0]));

    // The first category whose keyword is in the class or the name, or -1.
    int MatchFootprint(const char* vehClass, const char* vehName)
    {
        for (int i = 0; i < kCarCategoryCount; ++i)
        {
            if (ContainsCI(vehClass, kCarCategories[i].keyword) || ContainsCI(vehName, kCarCategories[i].keyword))
                return i;
        }
        return -1;
    }
}

bool GetCarFootprint(int footprint, CarFootprint& out)
{
    if (footprint < 0 || footprint >= kCarCategoryCount)
        return false;
    const CarCategory& cat = kCarCategories[footprint];
    out.halfWidthM = cat.halfWidthM;
    out.halfLengthM = cat.halfLengthM;
    out.cornerRadiusM = cat.cornerRadiusM;
    return true;
}

void CarStateStore::Init()
{
    if (m_ready)
        return;
    InitializeCriticalSection(&m_lock);
    QueryPerformanceFrequency(&m_freq);
    m_ready = true;
}

void CarStateStore::Update(const ScoringInfoV01& info)
{
    if (!m_ready)
        return;

    // The newest sample, about to become "prev". Its buffer is only ever
    // written here (this thread), so it can be read without the lock; only
    // the count needs it, since Clear() may zero it.
    EnterCriticalSection(&m_lock);
    const int lastIdx = m_cur;
    const int lastCount = m_curCount;
    LeaveCriticalSection(&m_lock);
    const CarEntry* last = m_cars[lastIdx];

    // Built in the spare buffer, which Read() never looks at.
    CarEntry* out = m_cars[m_spare];
    int count = 0;
    bool hasPlayer = false;

    if (info.mVehicle != nullptr)
    {
        const int n = info.mNumVehicles < kMaxCars ? info.mNumVehicles : kMaxCars;
        for (int i = 0; i < n; ++i)
        {
            const VehicleScoringInfoV01& v = info.mVehicle[i];
            CarEntry& e = out[count];

            e.id = v.mID;
            e.x = v.mPos.x;
            e.z = v.mPos.z;

            // mOri[2] is the row mapping local forward into world space.
            e.heading = atan2(v.mOri[2].x, v.mOri[2].z);

            e.isPlayer = v.mIsPlayer != 0;
            if (e.isPlayer)
                hasPlayer = true;

            e.inPits = v.mInPits != 0;
            e.inGarage = v.mInGarageStall != 0;
            e.lapDist = v.mLapDist;
            e.speedMps = static_cast<float>(sqrt(v.mLocalVel.x * v.mLocalVel.x + v.mLocalVel.y * v.mLocalVel.y + v.mLocalVel.z * v.mLocalVel.z));

            strncpy_s(e.vehicleClass, v.mVehicleClass, _TRUNCATE);
            strncpy_s(e.vehicleName, v.mVehicleName, _TRUNCATE);

            // The same car in the previous sample, matched by id even if the
            // game reordered mVehicle[] (first match, as the old per-Read
            // search did).
            e.prevIndex = -1;
            for (int j = 0; j < lastCount; ++j)
            {
                if (last[j].id == e.id) { e.prevIndex = j; break; }
            }

            // Footprint: carried over while the class and name stay the same
            // (nearly always), looked up again only when they change.
            const CarEntry* before = (e.prevIndex >= 0) ? &last[e.prevIndex] : nullptr;
            if (before && strcmp(before->vehicleClass, e.vehicleClass) == 0
                       && strcmp(before->vehicleName, e.vehicleName) == 0)
                e.footprint = before->footprint;
            else
                e.footprint = MatchFootprint(e.vehicleClass, e.vehicleName);

            // A car that jumped further than any car can drive in one tick
            // (ESC to garage, tow, reset to track) is shown at its new spot
            // directly instead of sliding across the radar past the player
            // (review F358). 40 m is ~150 m/s over a slow 0.25 s tick.
            if (before)
            {
                const double dx = e.x - before->x, dz = e.z - before->z;
                if (dx * dx + dz * dz > 40.0 * 40.0)
                    e.prevIndex = -1;
            }

            ++count;
        }
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    EnterCriticalSection(&m_lock);
    // The previous sample set becomes whatever was "current" until now. If
    // Clear() ran meanwhile, m_curCount/m_curTime are zero here, so the new
    // prev is empty and Read() does not interpolate from it.
    const int oldPrev = m_prev;
    m_prev = m_cur;
    m_prevCount = m_curCount;
    m_prevTime = m_curTime;
    m_prevET = m_curET;
    m_cur = m_spare;
    m_curCount = count;
    m_curTime = now;
    m_curET = info.mCurrentET;
    m_hasPlayer = hasPlayer;
    m_spare = oldPrev;
    LeaveCriticalSection(&m_lock);
}

void CarStateStore::OnTelemetry(const TelemInfoV01& info)
{
    if (!m_ready)
        return;
    // Done outside the lock: the few fields the radar uses.
    const double x = info.mPos.x, z = info.mPos.z;
    const double heading = atan2(info.mOri[2].x, info.mOri[2].z);   // same row as scoring's
    const float speed = static_cast<float>(sqrt(info.mLocalVel.x * info.mLocalVel.x
        + info.mLocalVel.y * info.mLocalVel.y + info.mLocalVel.z * info.mLocalVel.z));
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG staleTicks = m_freq.QuadPart / 2;   // 0.5 s

    EnterCriticalSection(&m_lock);
    TelemEntry* e = nullptr;
    int stale = -1;
    for (int i = 0; i < m_telemCount; ++i)
    {
        if (m_telem[i].id == info.mID) { e = &m_telem[i]; break; }
        if (stale < 0 && now.QuadPart - m_telem[i].t > staleTicks) stale = i;
    }
    if (!e)
    {
        if (m_telemCount < kMaxCars) e = &m_telem[m_telemCount++];
        else if (stale >= 0) e = &m_telem[stale];   // a car that left: reuse its slot
    }
    if (e)
    {
        e->id = info.mID;
        e->x = x; e->z = z; e->heading = heading; e->speedMps = speed;
        e->t = now.QuadPart;
    }
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
        // Prefer the sim-time spacing when both samples have it: callbacks
        // that arrive bunched after a hitch would otherwise snap then freeze
        // (review F130). Falls back to arrival times if ET went backwards
        // (session restart) or did not move.
        const double etPeriod = m_curET - m_prevET;
        if (m_prevET >= 0.0 && etPeriod > 0.001 && etPeriod < 5.0)
            tickPeriod = etPeriod;

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

    // Telemetry younger than this replaces the scoring position of a car.
    LARGE_INTEGER nowQpc;
    QueryPerformanceCounter(&nowQpc);
    const LONGLONG telemFresh = m_freq.QuadPart / 2;   // 0.5 s

    const CarEntry* curCars = m_cars[m_cur];
    const CarEntry* prevCars = m_cars[m_prev];
    for (int i = 0; i < n; ++i)
    {
        const CarEntry& cur = curCars[i];
        const CarEntry* prev = (frac < 1.0 && cur.prevIndex >= 0 && cur.prevIndex < m_prevCount)
            ? &prevCars[cur.prevIndex] : nullptr;

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
        // Fresh telemetry for this car: use it as-is (~50 Hz, no 0.2 s lag).
        float telemSpeed = -1.0f;
        for (int k = 0; k < m_telemCount; ++k)
        {
            const TelemEntry& t = m_telem[k];
            if (t.id != cur.id) continue;
            if (nowQpc.QuadPart - t.t <= telemFresh)
            {
                out[i].x = t.x;
                out[i].z = t.z;
                out[i].heading = t.heading;
                telemSpeed = t.speedMps;
            }
            break;
        }
        out[i].id = cur.id;
        out[i].isPlayer = cur.isPlayer;
        out[i].footprint = cur.footprint;
        out[i].inPits = cur.inPits;
        out[i].inGarage = cur.inGarage;
        out[i].speedMps = cur.speedMps;
        out[i].lapDist = cur.lapDist;
        if (telemSpeed >= 0.0f)
            out[i].speedMps = telemSpeed;

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
    m_curET = -1.0;
    m_prevET = -1.0;
    m_hasPlayer = false;
    m_telemCount = 0;
    LeaveCriticalSection(&m_lock);
}
