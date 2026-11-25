#include "TrackAlerts.h"
#include "InternalsPlugin.hpp"
#include "Config.h"
#include "LapDelta.h"
#include "Log.h"
#include <cmath>
#include <cstring>
#include <algorithm>

TrackAlertStore g_trackAlerts;

namespace
{
    // Seconds after the green flag before cars are judged at all: the whole
    // field is slow off a standing start, and bunched up behind a rolling one.
    const double kAfterGreenSeconds = 10.0;

    double SpeedOf(const VehicleScoringInfoV01& v)
    {
        return sqrt(v.mLocalVel.x * v.mLocalVel.x + v.mLocalVel.y * v.mLocalVel.y + v.mLocalVel.z * v.mLocalVel.z);
    }

    // Your best lap's speed at a lap distance, from its time-at-distance
    // trace: 30 m divided by how long that lap took to cover them. 0 = no
    // best lap for this car and track yet (or the trace has a gap there).
    double ReferenceSpeedAt(double lapDist, double trackLen)
    {
        const double span = 30.0;
        if (trackLen < span * 4.0)
            return 0.0;
        double d1 = lapDist - span * 0.5;
        if (d1 < 0.0) d1 = 0.0;
        if (d1 + span > trackLen) d1 = trackLen - span;
        double t1 = 0.0, t2 = 0.0;
        if (!g_lapDelta.PersonalBestInterpolateAt(d1, t1) || !g_lapDelta.PersonalBestInterpolateAt(d1 + span, t2))
            return 0.0;
        const double dt = t2 - t1;
        return (dt > 0.001) ? span / dt : 0.0;
    }

    // rF2 numbers sectors 1, 2, 0 (= sector 3) in mSector. Returns 1..3.
    // Only for mSector: mSectorFlag[] is indexed plainly, [i] = sector i+1,
    // which is how CrewChief reads it (CrewChief review 2026-11-24).
    int SectorNumber(int rf2Index)
    {
        return rf2Index == 0 ? 3 : rf2Index;
    }
}

void TrackAlertStore::Init()
{
    if (m_ready)
        return;

    m_ready = true;
}

void TrackAlertStore::Clear()
{
    // Runs at session end, when no scoring tick is in flight -- the same
    // convention the other stores' Clear() rely on for their scoring-thread
    // state.
    for (CarTrack& t : m_cars)
        t = CarTrack{};
    m_lastGamePhase = -1;
    m_greenSince = -1.0;
    for (signed char& f : m_lastSectorFlag)
        f = -1;
    AcquireSRWLockExclusive(&m_lock);
    m_state = State{};
    ReleaseSRWLockExclusive(&m_lock);
}

TrackAlertStore::CarTrack* TrackAlertStore::TrackFor(long id, double et)
{
    CarTrack* reuse = nullptr;
    for (CarTrack& t : m_cars)
    {
        if (t.id == id)
        {
            t.lastSeen = et;
            return &t;
        }
        // Free, not seen for a few seconds, or from before a clock reset.
        if (!reuse && (t.id == -1 || et - t.lastSeen > 5.0 || et < t.lastSeen))
            reuse = &t;
    }
    if (!reuse)
        return nullptr;
    *reuse = CarTrack{};
    reuse->id = id;
    reuse->lastSeen = et;
    return reuse;
}

void TrackAlertStore::OnScoring(const ScoringInfoV01& info)
{
    if (!m_ready)
        return;
    const TrackAlertConfig& cfg = g_trackAlertConfig;
    State out;
    QueryPerformanceCounter(&out.tickTime);

    const VehicleScoringInfoV01* me = nullptr;
    for (int i = 0; i < info.mNumVehicles; ++i)
        if (info.mVehicle[i].mIsPlayer) { me = &info.mVehicle[i]; break; }

    if (!cfg.enabled || !me)
    {
        AcquireSRWLockExclusive(&m_lock);
        m_state = out;   // valid = false: nothing to show
        ReleaseSRWLockExclusive(&m_lock);
        return;
    }
    out.valid = true;

    const double et = info.mCurrentET;
    // Paused: hold everything as it is, so a STOPPED line is still there on
    // resume and the after-green hold is not re-armed (review F128).
    if (info.mGamePhase == 9)
        return;
    // FCY "resume" (mYellowFlagState 6) means the race is going green, so it
    // ends the full-course yellow whatever mGamePhase still says; CrewChief
    // treats it as green too (CrewChief review 2026-11-24). Seen as phase 5,
    // the step out of phase 6 re-arms the after-green hold just below, so the
    // bunched field off the restart is not judged as stopped cars.
    const int phase = (info.mGamePhase == 6 && info.mYellowFlagState == 6) ? 5 : info.mGamePhase;
    // Re-arm the after-green hold only on a real start or restart, not when
    // coming back from session-over/pause.
    if (phase == 5 && m_lastGamePhase != 5 && m_lastGamePhase != 8 && m_lastGamePhase != 9)
        m_greenSince = et;
    m_lastGamePhase = phase;
    // Session over but you have not taken the flag yet: still racing.
    const bool green = phase == 5 || (phase == 8 && me->mFinishStatus == 0);
    if (green && m_greenSince < 0.0)
        m_greenSince = et;
    out.fullCourseYellow = phase == 6;
    if (out.fullCourseYellow)
        out.yellowStage = info.mYellowFlagState;
    // Blue flag and pending penalties: player's own scoring entry.
    out.blueFlag = me->mFlag == 6;
    out.penalties = me->mNumPenalties > 0 ? me->mNumPenalties : 0;

    // --- Yellow flag per sector ---
    for (int i = 0; i < 3; ++i)
    {
        const signed char f = info.mSectorFlag[i];
        if (f != m_lastSectorFlag[i])
        {
            // Logged so the sector numbering can be checked against the
            // game's own flag display the first time it happens.
            if (m_lastSectorFlag[i] != -1)
                Log::Write("TrackAlerts: mSectorFlag[%d] (read as sector %d) %d -> %d", i, i + 1, m_lastSectorFlag[i], f);
            m_lastSectorFlag[i] = f;
        }
        if (cfg.showSectorYellow && green && f == 1)
            out.sectorYellow[i] = true;   // [i] = sector i+1
    }

    // --- Pit request: until the car is in the pit lane ---
    out.pitRequested = (me->mPitState == 1 || me->mPitState == 2) && !me->mInPits;

    // --- Stopped cars ahead ---
    const double trackLen = info.mLapDist;
    const bool judge = green && m_greenSince >= 0.0 && et - m_greenSince >= kAfterGreenSeconds && trackLen > 100.0;
    const double mySpeed = SpeedOf(*me);
    const double dwell = cfg.dwellSeconds > 0.0f ? cfg.dwellSeconds : 0.0;
    const double stoppedMs = cfg.stoppedKmh / 3.6;
    // Every stopped car within range, for grouping below.
    struct Ahead { double dist; const VehicleScoringInfoV01* v; CarTrack* t; bool off; };
    Ahead ahead[kSlots];
    int aheadCount = 0;
    for (int i = 0; i < info.mNumVehicles; ++i)
    {
        const VehicleScoringInfoV01& v = info.mVehicle[i];
        if (&v == me)
            continue;
        CarTrack* tp = TrackFor(v.mID, et);
        if (!tp)
            continue;
        CarTrack& t = *tp;

        // Out of the running for this: the pit lane and garage, cars that
        // have taken the flag (slowing down on the cool-down lap) or been
        // disqualified, and any moment the field as a whole is slow.
        // Also cars with no real data yet: a negative lap distance, or
        // standing exactly still at lap distance 0 -- and the first 30 s of
        // the session, while cars are still being placed (CrewChief review
        // 2026-11-24, after CrewChief's GameStateData).
        const bool noData = v.mLapDist < 0.0 || (SpeedOf(v) == 0.0 && v.mLapDist <= 0.0);
        const bool ignore = !judge || et < 30.0 || noData || v.mInPits || v.mInGarageStall || v.mFinishStatus == 1 || v.mFinishStatus == 3;
        const double speed = SpeedOf(v);
        bool slowNow = false;
        if (!ignore)
        {
            // Under the fixed speed -- and, where your own best lap was
            // nearly that slow (a hairpin), also well under your speed there.
            if (speed < stoppedMs)
            {
                const double ref = cfg.slowCornerFraction > 0.0f ? ReferenceSpeedAt(v.mLapDist, trackLen) : 0.0;
                slowNow = ref <= 0.0 || ref * cfg.slowCornerFraction >= stoppedMs || speed < ref * cfg.slowCornerFraction;
            }
        }

        if (slowNow)
        {
            t.fastSince = -1.0;
            if (t.slowSince < 0.0) t.slowSince = et;
            if (et - t.slowSince >= dwell) t.flagged = true;
        }
        else
        {
            t.slowSince = -1.0;
            if (t.flagged)
            {
                if (t.fastSince < 0.0) t.fastSince = et;
                if (ignore || et - t.fastSince >= dwell)
                {
                    t.flagged = false;
                    t.announced = false;
                    t.fastSince = -1.0;
                }
            }
        }
        if (!t.flagged)
            continue;

        double dist = fmod(v.mLapDist - me->mLapDist, trackLen);
        if (dist < 0.0) dist += trackLen;
        if (dist > 0.0 && dist <= cfg.rangeMeters && aheadCount < kSlots)
        {
            // Off track: further from the track's centre path than the edge
            // on its own side is, by more than half a metre.
            const bool off = fabs(v.mTrackEdge) > 0.5 && fabs(v.mPathLateral) > fabs(v.mTrackEdge) + 0.5;
            ahead[aheadCount++] = Ahead{ dist, &v, &t, off };
        }
    }

    if (aheadCount > 0)
    {
        // The nearest incident: the nearest stopped car plus every one that
        // follows within groupMeters of the previous, so a pile-up strung out
        // over a corner is still one line rather than one per car (which also
        // made each further car "new" while you were already passing them).
        std::sort(ahead, ahead + aheadCount, [](const Ahead& a, const Ahead& b) { return a.dist < b.dist; });
        int n = 1;
        while (n < aheadCount && ahead[n].dist - ahead[n - 1].dist <= cfg.groupMeters)
            ++n;
        bool allOff = true, anyNew = false;
        for (int k = 0; k < n; ++k)
        {
            allOff = allOff && ahead[k].off;
            anyNew = anyNew || !ahead[k].t->announced;
            ahead[k].t->announced = true;
        }
        const VehicleScoringInfoV01& v = *ahead[0].v;
        out.haveCar = true;
        out.carCount = n;
        out.carOffTrack = allOff;
        out.carSector = SectorNumber(v.mSector);
        out.carDistM = ahead[0].dist;
        out.closingMs = mySpeed - SpeedOf(v);
        if (anyNew)
            Log::Write("TrackAlerts: %d stopped car(s) %.0f m ahead, first '%s' at %.0f km/h, sector %d%s",
                       n, ahead[0].dist, v.mDriverName, SpeedOf(v) * 3.6, out.carSector,
                       allOff ? ", off track" : "");
    }

    AcquireSRWLockExclusive(&m_lock);
    m_state = out;
    ReleaseSRWLockExclusive(&m_lock);
}

TrackAlertStore::State TrackAlertStore::Read()
{
    AcquireSRWLockShared(&m_lock);
    const State s = m_state;
    ReleaseSRWLockShared(&m_lock);
    return s;
}

double TrackAlertStore::DistanceNow(const State& s)
{
    static LARGE_INTEGER s_freq = {};
    if (s_freq.QuadPart == 0)
        QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = static_cast<double>(now.QuadPart - s.tickTime.QuadPart) / static_cast<double>(s_freq.QuadPart);
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.5) dt = 0.5;   // ticks stopped (pause, menu): hold the last value
    const double d = s.carDistM - s.closingMs * dt;
    return d > 0.0 ? d : 0.0;
}
