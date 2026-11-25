#include "PitStint.h"
#include "RenderThrottle.h"
#include "RelativeTiming.h"
#include "TyreStore.h"
#include "InternalsPlugin.hpp"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "Log.h"
#include "FuelStore.h"
#include "LapDelta.h"
#include "DriverAbbrev.h"
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <cmath>

RelativeTimingStore g_relativeTiming;

namespace
{
    // Fills each row's Row::Lookups from the tyre and pit-stint stores --
    // what Draw() used to look up itself for every row on every frame.
    void LookUpOtherStores(RelativeTimingStore::Row* rows, int count)
    {
        for (int r = 0; r < count; ++r)
        {
            const long id = rows[r].carId;
            RelativeTimingStore::Row::Lookups& out = rows[r].lookups;
            out = RelativeTimingStore::Row::Lookups{};
            out.haveTyre = TyreStore::Get(id, out.tyre, sizeof(out.tyre), out.tyreR, out.tyreG, out.tyreB);
            out.tyreRearR = out.tyreR; out.tyreRearG = out.tyreG; out.tyreRearB = out.tyreB;
            if (out.haveTyre && out.tyre[1] == '/')
                TyreStore::GetRearColour(id, out.tyreRearR, out.tyreRearG, out.tyreRearB);
            out.pitLapsKnown = PitStint::Get(id, out.pitLaps);
        }
    }
}

void RelativeTimingStore::Init()
{
    if (m_ready) return;
    InitializeCriticalSection(&m_lock);
    QueryPerformanceFrequency(&m_freq);
    m_ready = true;
}

void RelativeTimingStore::Clear()
{
    if (!m_ready) return;
    EnterCriticalSection(&m_lock);
    m_state = State{};
    m_trackerCount = 0;
    LeaveCriticalSection(&m_lock);
    m_havePitState = false;
    m_wasInPits = false;
    m_currentTrace.clear();
    m_referenceTrace.clear();
    m_referenceMaxDist.clear();
    m_referenceLapTime = -1.0;
    m_lastTotalLapsForTrace = -1;
    m_lastLapDistForTrace = -1.0;
    m_lastTimeIntoLapForTrace = -1.0;
    m_curLapRejected = false;
    m_raceOverFrozen = false;
    m_frozenSession = -1;
    m_frozenET = 0.0;
    m_lapHistoryCount = 0;
    m_lapHistoryNext = 0;
    for (int k = 0; k < 3; ++k) m_sessionBestSector[k] = -1.0;
}

double RelativeTimingStore::SteadyLapTime() const
{
    // Median of the recent laps: one slow lap (standing start, traffic, a
    // moment) cannot move it. Needs three laps to mean anything.
    if (m_lapHistoryCount < 3)
        return -1.0;
    double v[kLapHistory];
    for (int i = 0; i < m_lapHistoryCount; ++i) v[i] = m_lapHistory[i];
    std::sort(v, v + m_lapHistoryCount);
    const int n = m_lapHistoryCount;
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

bool RelativeTimingStore::InterpolateTraceAt(double lapDist, double& outTime) const
{
    if (m_referenceTrace.size() < 2)
        return false;

    if (lapDist <= m_referenceTrace.front().first) { outTime = m_referenceTrace.front().second; return true; }
    if (lapDist >= m_referenceTrace.back().first) { outTime = m_referenceTrace.back().second; return true; }

    // The first sample at or past lapDist, interpolated from the one before.
    // This runs 70-90 times every scoring tick over a few hundred points, so
    // it is a binary search -- but not over the trace itself, which is not
    // strictly in distance order: a sample is added whenever the distance
    // moves on from the PREVIOUS TICK's, so a step back mid-lap (a spin, a
    // resync) leaves a smaller distance after a larger one. The running
    // maximum (m_referenceMaxDist) is always in order, and the first point
    // where it reaches lapDist is exactly the first sample that does -- the
    // same sample a front-to-back scan of the trace finds.
    size_t i = 0;
    if (m_referenceMaxDist.size() == m_referenceTrace.size() && lapDist == lapDist) // (NaN: no sample matches)
    {
        i = static_cast<size_t>(std::lower_bound(m_referenceMaxDist.begin() + 1, m_referenceMaxDist.end(), lapDist)
                                - m_referenceMaxDist.begin());
    }
    else
    {
        for (i = 1; i < m_referenceTrace.size() && !(lapDist <= m_referenceTrace[i].first); ++i) {}
    }
    if (i < m_referenceTrace.size())
    {
        const double d0 = m_referenceTrace[i - 1].first, t0 = m_referenceTrace[i - 1].second;
        const double d1 = m_referenceTrace[i].first, t1 = m_referenceTrace[i].second;
        const double span = d1 - d0;
        const double frac = (span > 0.0001) ? (lapDist - d0) / span : 0.0;
        outTime = t0 + (t1 - t0) * frac;
        return true;
    }
    outTime = m_referenceTrace.back().second;
    return true;
}

void RelativeTimingStore::UpdateSectors(const ScoringInfoV01& info, const VehicleScoringInfoV01* player)
{
    // Pass 1: who finished a sector this tick, and in what time. mSector is
    // 1 = in sector 1, 2 = in sector 2, 0 = in sector 3; a step to the NEXT
    // sector means the one just left is done. Anything else (a jump back to
    // the garage, a session reset) only re-anchors.
    static const int kMaxEvents = 3 * kMaxTrackedCars;
    long eventCar[kMaxEvents];
    int eventSector[kMaxEvents];
    int eventCount = 0;
    for (long i = 0; i < info.mNumVehicles; ++i)
    {
        const VehicleScoringInfoV01* v = &info.mVehicle[i];
        TrendTracker& tr = GetTracker(v->mID);
        const int cur = v->mSector;
        const int prev = tr.lastSectorSeen;
        tr.lastSectorSeen = cur;
        if (prev < 0 || cur == prev)
            continue;

        int k = -1;          // 0-based sector just finished
        double t = -1.0;
        if (prev == 1 && cur == 2)      { k = 0; t = v->mCurSector1; }
        else if (prev == 2 && cur == 0) { k = 1; t = (v->mCurSector1 > 0.0 && v->mCurSector2 > 0.0) ? v->mCurSector2 - v->mCurSector1 : -1.0; }
        else if (prev == 0 && cur == 1) { k = 2; t = (v->mLastLapTime > 0.0 && v->mLastSector2 > 0.0) ? v->mLastLapTime - v->mLastSector2 : -1.0; }
        if (k < 0)
            continue;

        // A pit-lane sector is not a racing comparison.
        const bool valid = t > 1.0 && t < 1000.0 && !v->mInPits;
        tr.sectorTime[k] = valid ? t : -1.0;
        tr.sectorDoneET[k] = info.mCurrentET;
        if (valid && (m_sessionBestSector[k] < 0.0 || t < m_sessionBestSector[k]))
            m_sessionBestSector[k] = t;
        tr.sectorWasBest[k] = valid && t <= m_sessionBestSector[k] + 1.0e-4;

        if (eventCount < kMaxEvents)
        {
            eventCar[eventCount] = v->mID;
            eventSector[eventCount] = k;
            ++eventCount;
        }
    }
    if (eventCount == 0)
        return;

    // Pass 2: re-judge. A pair is compared when the SECOND of the two
    // finishes the sector -- the car ahead when the player gets there, a
    // car behind when it gets there itself -- so both times are from the
    // same pass. "Same pass" = finished within half a lap of each other.
    double lap = SteadyLapTime();
    if (lap <= 0.0) lap = m_referenceLapTime;
    if (lap <= 0.0) lap = player->mBestLapTime;
    if (lap <= 0.0) lap = 120.0;
    const double sameWindow = 0.5 * lap;

    TrendTracker& me = GetTracker(player->mID);
    auto judge = [&](TrendTracker& them, int k)
    {
        if (fabs(them.sectorDoneET[k] - me.sectorDoneET[k]) > sameWindow)
            return;
        if (them.sectorTime[k] < 0.0 || me.sectorTime[k] < 0.0)
            them.sectorVerdict = 0;
        else if (them.sectorWasBest[k])
            them.sectorVerdict = 2;
        else if (them.sectorTime[k] < me.sectorTime[k])
            them.sectorVerdict = -1;
        else if (them.sectorTime[k] > me.sectorTime[k])
            them.sectorVerdict = 1;
    };
    for (int e = 0; e < eventCount; ++e)
    {
        if (eventCar[e] == player->mID)
        {
            for (long i = 0; i < info.mNumVehicles; ++i)
                if (info.mVehicle[i].mID != player->mID)
                    judge(GetTracker(info.mVehicle[i].mID), eventSector[e]);
        }
        else
        {
            judge(GetTracker(eventCar[e]), eventSector[e]);
        }
    }
}

RelativeTimingStore::TrendTracker& RelativeTimingStore::GetTracker(long carId)
{
    for (int i = 0; i < m_trackerCount; ++i)
    {
        if (m_trackerCarId[i] == carId)
            return m_tracker[i];
    }
    if (m_trackerCount < kMaxTrackedCars)
    {
        m_trackerCarId[m_trackerCount] = carId;
        m_tracker[m_trackerCount] = TrendTracker{};
        m_trackerName[m_trackerCount][0] = 0;
        m_trackerMissingTicks[m_trackerCount] = 0;
        return m_tracker[m_trackerCount++];
    }
    // More distinct cars seen than kMaxTrackedCars (shouldn't happen at any
    // real grid size) -- reuse the last slot rather than growing; a wrong
    // trend for one overflow car is harmless.
    return m_tracker[kMaxTrackedCars - 1];
}

void RelativeTimingStore::PruneTrackers(const ScoringInfoV01& info)
{
    // Review F82/F95: free trackers whose car has left scoring for ~5 s
    // (25 ticks at 5 Hz), and reset one whose ID now belongs to a different
    // driver (IDs are reused in multiplayer). Runs before any GetTracker of
    // the tick, so no tracker reference is held across the compaction.
    for (int i = 0; i < m_trackerCount; )
    {
        const VehicleScoringInfoV01* v = nullptr;
        for (long k = 0; k < info.mNumVehicles; ++k)
            if (info.mVehicle[k].mID == m_trackerCarId[i]) { v = &info.mVehicle[k]; break; }
        if (v)
        {
            m_trackerMissingTicks[i] = 0;
            if (m_trackerName[i][0] && strncmp(m_trackerName[i], v->mDriverName, sizeof(m_trackerName[i])) != 0)
                m_tracker[i] = TrendTracker{};
            strncpy_s(m_trackerName[i], v->mDriverName, _TRUNCATE);
            ++i;
        }
        else if (++m_trackerMissingTicks[i] > 25)
        {
            const int last = m_trackerCount - 1;
            m_trackerCarId[i] = m_trackerCarId[last];
            m_tracker[i] = m_tracker[last];
            memcpy(m_trackerName[i], m_trackerName[last], sizeof(m_trackerName[i]));
            m_trackerMissingTicks[i] = m_trackerMissingTicks[last];
            m_trackerName[last][0] = 0;
            m_trackerMissingTicks[last] = 0;
            --m_trackerCount;
        }
        else
        {
            ++i;
        }
    }
}

void RelativeTimingStore::OnScoring(const ScoringInfoV01& info)
{
    if (!m_ready || info.mVehicle == nullptr || info.mNumVehicles <= 0)
        return;

    const VehicleScoringInfoV01* player = nullptr;
    for (long i = 0; i < info.mNumVehicles; ++i)
    {
        if (info.mVehicle[i].mIsPlayer) { player = &info.mVehicle[i]; break; }
    }
    if (!player)
        return;

    const int playerPlace = player->mPlace;

    PruneTrackers(info);
    UpdateSectors(info, player);

    // ScoringInfoV01::mLapDist is the TRACK LENGTH (confusingly, the exact
    // same field name on VehicleScoringInfoV01 means "this car's current
    // distance around the lap" instead) -- needed to turn each car's own
    // lap count + in-lap distance into one monotonically increasing
    // "distance since race start" value, comparable across cars regardless
    // of who's a lap up or down.
    const double trackLength = info.mLapDist;

    const double playerSpeed = sqrt(player->mLocalVel.x * player->mLocalVel.x
                                   + player->mLocalVel.y * player->mLocalVel.y
                                   + player->mLocalVel.z * player->mLocalVel.z);
    // Floored well above zero so a near-stationary moment (grid, pits) can't
    // blow the distance->time conversion up into a huge, meaningless number.
    const double paceSpeed = (playerSpeed > 5.0) ? playerSpeed : 5.0;

    const double playerLastLap = player->mLastLapTime;
    // Cumulative distance covered, used only for the boundary-stable "is this
    // car a lap down" test below -- NOT for the gap itself, which comes from
    // the lap-time trace instead.
    const double playerTotalDist = static_cast<double>(player->mTotalLaps) * trackLength + player->mLapDist;

    // Fractional lap counters: whole part = laps completed, fraction = live
    // progress through the lap in progress -- ticks up continuously instead
    // of only jumping at the line, so it's unambiguous that it's not
    // counting the in-progress lap as done. Stint re-anchors to 0 on every
    // pit EXIT (mInPits true -> false); Total never resets.
    const double totalLapsFractional = static_cast<double>(player->mTotalLaps)
        + ((trackLength > 0.1) ? (player->mLapDist / trackLength) : 0.0);

    const bool inPits = player->mInPits != 0;
    if (!m_havePitState)
    {
        // First sample seen (fresh session/key) -- anchor without treating
        // it as a real pit-exit edge.
        m_stintStartTotalLaps = static_cast<double>(player->mTotalLaps);
        m_havePitState = true;
    }
    else if (!inPits && m_wasInPits)
    {
        m_stintStartTotalLaps = static_cast<double>(player->mTotalLaps);
    }
    m_wasInPits = inPits;

    // ---- Lap-distance -> time trace of the player's own driving. Built the
    // same way LapDelta.cpp builds its reference lap: append a sample per
    // scoring tick while the distance is genuinely advancing, and promote
    // the whole lap to the reference once it completes cleanly. ----
    const double timeIntoLap = info.mCurrentET - player->mLapStartET;

    if (m_lastTotalLapsForTrace >= 0 && player->mTotalLaps > m_lastTotalLapsForTrace)
    {
        const double lastLap = player->mLastLapTime;
        if (lastLap > 5.0 && lastLap < 100000.0)
        {
            m_lapHistory[m_lapHistoryNext] = lastLap;
            m_lapHistoryNext = (m_lapHistoryNext + 1) % kLapHistory;
            if (m_lapHistoryCount < kLapHistory) ++m_lapHistoryCount;
        }

        // The trace must actually SPAN the lap, not merely contain points.
        // A race's very first "lap" is the grid-to-line crawl, which starts
        // at a lap distance near the END of the lap (the grid sits behind
        // the start/finish line) -- so it produces a trace covering only
        // that last sliver of track. Interpolating any normal distance
        // against such a trace clamps to its first entry, returning the SAME
        // time for every car and collapsing every gap to exactly 0.00. Same
        // hazard for an out lap that joins mid-track. Requiring near-full
        // coverage rejects both; the previous reference (or the fallback)
        // stays in use until a genuine complete lap is driven.
        const bool plausibleLapTime = lastLap > 5.0 && lastLap < 100000.0;
        const bool spansLap = trackLength > 1.0
                             && m_currentTrace.size() >= 20
                             && m_currentTrace.front().first <= trackLength * 0.05
                             && m_currentTrace.back().first >= trackLength * 0.95;

        // Review F78: a lap that touched the pits or was disturbed, or is
        // well off the usual pace, must not replace the reference -- an
        // in-lap made every gap wrong for the whole next lap.
        const double steadyForRef = SteadyLapTime();
        const bool cleanLap = !m_curLapRejected
                             && (steadyForRef <= 0.0 || lastLap <= steadyForRef * 1.07);
        if (plausibleLapTime && spansLap && !cleanLap)
        {
            Log::Write("RelativeTiming: rejected trace (lap %.3fs) -- pit, pause or slow lap", lastLap);
        }
        else if (plausibleLapTime && spansLap)
        {
            m_referenceTrace = m_currentTrace;
            m_referenceMaxDist.resize(m_referenceTrace.size());
            double maxDist = m_referenceTrace.front().first;
            for (size_t k = 0; k < m_referenceTrace.size(); ++k)
            {
                if (m_referenceTrace[k].first > maxDist) maxDist = m_referenceTrace[k].first;
                m_referenceMaxDist[k] = maxDist;
            }
            m_referenceLapTime = lastLap;
            Log::Write("RelativeTiming: reference lap trace accepted -- %zu points, %.0f..%.0fm, lap %.3fs",
                       m_currentTrace.size(), m_currentTrace.front().first, m_currentTrace.back().first, lastLap);
        }
        else if (!m_currentTrace.empty())
        {
            Log::Write("RelativeTiming: rejected trace (%zu points, %.0f..%.0fm, lap %.3fs) -- does not span the lap",
                       m_currentTrace.size(), m_currentTrace.front().first, m_currentTrace.back().first, lastLap);
        }
        m_currentTrace.clear();
        m_curLapRejected = false;
    }
    m_lastTotalLapsForTrace = player->mTotalLaps;

    if (inPits)
        m_curLapRejected = true;
    // Lap distance jumping back (a reset, a tow) disturbs the lap too.
    if (!m_currentTrace.empty() && player->mLapDist < m_lastLapDistForTrace - 50.0)
        m_curLapRejected = true;
    if (timeIntoLap >= 0.0 && timeIntoLap < 100000.0
        && (m_currentTrace.empty() || player->mLapDist > m_lastLapDistForTrace + 0.5))
    {
        // Over 2 s since the last sample: the car stood still, so the whole
        // pause lands in one step of the trace.
        if (!m_currentTrace.empty() && timeIntoLap - m_currentTrace.back().second > 2.0)
            m_curLapRejected = true;
        m_currentTrace.emplace_back(player->mLapDist, timeIntoLap);
    }
    m_lastLapDistForTrace = player->mLapDist;
    m_lastTimeIntoLapForTrace = timeIntoLap;

    // Reference lap time is what a full lap "costs" in the time domain --
    // needed to wrap a lap-relative gap and to convert whole-lap
    // differences in a race. Three tiers, same priority order as the trace
    // lookup below: this session's own clean lap, then the saved personal
    // best (survives restarts, so it is usually available even on lap 1 of
    // a fresh session), then finally the player's raw last lap time.
    const double pbLapTimeSeconds = g_lapDelta.PersonalBestSeconds();
    const double lapTimeForWrap = (m_referenceLapTime > 0.0) ? m_referenceLapTime
                                 : (pbLapTimeSeconds > 0.0) ? pbLapTimeSeconds
                                 : ((playerLastLap > 5.0 && playerLastLap < 100000.0) ? playerLastLap : -1.0);

    double playerTraceTime = 0.0;
    const bool haveTrace = InterpolateTraceAt(player->mLapDist, playerTraceTime) && lapTimeForWrap > 0.0;
    // Second tier: the saved personal-best trace, when this session hasn't
    // produced a clean lap of its own yet. Far steadier than the crude
    // distance/pace-speed guess below -- it is a REAL lap's distance-to-time
    // curve, just an older one, so cornering and braking zones are already
    // shaped correctly instead of assumed flat.
    double playerPbTraceTime = 0.0;
    const bool havePbTrace = !haveTrace
                            && g_lapDelta.PersonalBestInterpolateAt(player->mLapDist, playerPbTraceTime)
                            && lapTimeForWrap > 0.0;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    double windowSeconds = g_relativeTimingConfig.trendWindowSeconds;
    if (windowSeconds < 0.1) windowSeconds = 0.1;

    // mPlace (and so "leader"/"position") only means something when cars are
    // actually racing each other for track position -- in practice/qualy/
    // warmup it's typically just ranked by best lap time, which reshuffles
    // constantly and has nothing to do with who's actually near you on
    // track. So: race sessions keep the place-based leader+window; anything
    // else instead shows whichever cars are physically CLOSEST by distance,
    // which is what's actually useful when nobody's racing for position.
    const bool isRaceSession = info.mSession >= 10 && info.mSession <= 13;
    // Race, but listed in track order (RelativeTimingConfig::raceTrackOrder):
    // gaps and row choice then work as in practice, while the lap markers
    // and places gained still use the race bookkeeping.
    const bool standingsOrder = isRaceSession && !g_relativeTimingConfig.raceTrackOrder;

    // Guards the trend against the discontinuity when a car crosses the
    // half-lap wrap boundary: the gap legitimately flips by a whole lap
    // there, which is not a real pace change. A genuine change never
    // approaches this much within one trend window.
    const double wrapSpan = (haveTrace || havePbTrace) ? lapTimeForWrap
                          : ((trackLength > 1.0) ? (trackLength / paceSpeed) : 1.0e9);
    const double wrapGuardSeconds = wrapSpan * 0.4;
    // Median of the player's recent laps, for converting whole laps to
    // seconds in a race (see SteadyLapTime) -- the same for every row.
    const double steadyLap = SteadyLapTime();

    // Computes one vehicle's row: the time gap, its trend, and the lap-time
    // comparison. Negative gap = v is ahead of the player, positive = behind.
    // Retired or disqualified (mFinishStatus 2/3) cars are left out of the
    // tower entirely: they are not racing anyone any more.
    auto isRetired = [](const VehicleScoringInfoV01* v)
    {
        return !v->mIsPlayer && (v->mFinishStatus == 2 || v->mFinishStatus == 3);
    };
    // Review F91: rF2's "Transparent Trainer" ghost is not a rival, and a
    // car parked in its garage stall has no track position worth a row.
    auto isTrainer = [](const VehicleScoringInfoV01* v)
    {
        return !v->mIsPlayer && strncmp(v->mDriverName, "Transparent Trainer", 19) == 0;
    };
    auto isParked = [&](const VehicleScoringInfoV01* v)
    {
        // Also cars nobody controls (mControl -1) or replay cars (3): only
        // local player 0, AI 1 and remote 2 are really in the session
        // (CrewChief review 2026-11-24). Never the player's own row.
        const bool controlled = v->mControl >= 0 && v->mControl <= 2;
        return !v->mIsPlayer && (v->mInGarageStall != 0 || isTrainer(v) || !controlled);
    };

    // Everything in a row except the position-change highlight (see
    // computeRow below for why that one is kept apart).
    auto buildRow = [&](const VehicleScoringInfoV01* v, TrendTracker& tr) -> Row
    {
        Row row;
        const bool isPlayer = v->mIsPlayer;

        // Preferred path: convert both cars' track positions into times via
        // the player's own reference lap, so the gap is "seconds of driving
        // between these two points on track". That stays steady through
        // corners and straights alike, unlike dividing the raw distance by
        // one instantaneous speed (which made the number lurch every time
        // either car hit a chicane -- see the header comment).
        double withinLapGap = 0.0;
        bool usedTrace = false;
        if (haveTrace)
        {
            double vTraceTime = 0.0;
            if (InterpolateTraceAt(v->mLapDist, vTraceTime))
            {
                withinLapGap = playerTraceTime - vTraceTime;
                usedTrace = true;
            }
        }
        else if (havePbTrace)
        {
            // Second tier: no clean lap yet THIS session, but a saved
            // personal-best trace exists -- use it instead of assuming a
            // flat constant speed. Both cars' positions are looked up
            // against the SAME real distance-to-time curve, so cornering
            // and braking zones are already shaped correctly even though
            // the curve itself is from an earlier session.
            double vPbTraceTime = 0.0;
            if (g_lapDelta.PersonalBestInterpolateAt(v->mLapDist, vPbTraceTime))
            {
                withinLapGap = playerPbTraceTime - vPbTraceTime;
                usedTrace = true;
            }
        }
        if (!usedTrace)
        {
            // Last resort, until a personal best exists at all for this
            // car+track combo (never driven it before): a crude constant-
            // speed estimate. Jumpy through corners, but better than a
            // blank table.
            withinLapGap = (player->mLapDist - v->mLapDist) / paceSpeed;
        }

        // Track-relative gap: purely "where are they on track versus me right
        // now". Measured BOTH ways round the lap, each with the pace of
        // whichever car has to do the driving:
        //  - AHEAD of me: how long I need to reach where they are (my own
        //    reference lap).
        //  - BEHIND me: how long THEY need to reach where I am -- the
        //    distance they have left to cover, at their own pace, taken from
        //    their previous lap time scaled over my reference lap.
        // Doing both with MY pace, as this used to, misstates the behind
        // figure whenever the other car is faster or slower than me.
        // trackGap is whichever of the two is shorter, signed (negative =
        // ahead), so it still flips sign as you pass each other.
        double trackGap = withinLapGap;
        double tAheadSeconds = 0.0, tBehindSeconds = 0.0;
        const bool spanValid = wrapSpan > 0.0 && wrapSpan < 1.0e8;
        if (spanValid)
        {
            tAheadSeconds = fmod(-withinLapGap, wrapSpan);
            if (tAheadSeconds < 0.0) tAheadSeconds += wrapSpan;
            const double behindAtMyPace = wrapSpan - tAheadSeconds;

            // Their pace relative to my reference lap; 1.0 (my own pace)
            // when either lap time is unknown. Clamped so a slow in/out lap
            // cannot blow the figure up.
            double paceRatio = 1.0;
            if ((haveTrace || havePbTrace) && lapTimeForWrap > 0.0)
            {
                double theirLap = -1.0;
                if (!isPlayer)
                {
                    if (v->mLastLapTime > 5.0 && v->mLastLapTime < 100000.0) theirLap = v->mLastLapTime;
                    else if (v->mBestLapTime > 5.0 && v->mBestLapTime < 100000.0) theirLap = v->mBestLapTime;
                }
                if (theirLap > 0.0)
                {
                    paceRatio = theirLap / lapTimeForWrap;
                    if (paceRatio < 0.5) paceRatio = 0.5;
                    if (paceRatio > 2.0) paceRatio = 2.0;
                }
            }
            tBehindSeconds = behindAtMyPace * paceRatio;
            trackGap = (tAheadSeconds <= tBehindSeconds) ? -tAheadSeconds : tBehindSeconds;
        }

        double gap = 0.0;
        if (standingsOrder)
        {
            // Prefer the SDK's own behind-leader bookkeeping over the
            // reconstructed lap trace: it is exactly what the game's own
            // timing/leaderboard display uses, so it is steady from the very
            // first lap of the race (before this plugin has a reference
            // trace to interpolate against) and isn't subject to any trace-
            // interpolation noise. Subtracting two cars' own time/laps-
            // behind-leader gives their gap to EACH OTHER, since the leader
            // term cancels out regardless of which of the two is ahead.
            const double lapsDiff = static_cast<double>(v->mLapsBehindLeader) - static_cast<double>(player->mLapsBehindLeader);
            const double lapSeconds = (steadyLap > 0.0) ? steadyLap : wrapSpan;
            gap = lapsDiff * lapSeconds + (v->mTimeBehindLeader - player->mTimeBehindLeader);
        }
        else
        {
            // Practice/qualy/warmup: cars join and pit at will, so cumulative
            // laps are meaningless here -- somebody who has been out 30 laps
            // longer than you would read as thousands of seconds "behind"
            // despite sitting right next to you on track.
            gap = trackGap;
        }

        // Places gained over the last completed lap: compare the place held
        // at each crossing of the line with the one at the crossing before.
        if (tr.lapsSeen != v->mTotalLaps)
        {
            if (tr.lapsSeen >= 0 && tr.placeAtLine > 0 && v->mTotalLaps == tr.lapsSeen + 1)
            {
                tr.lastLapGain = tr.placeAtLine - static_cast<int>(v->mPlace);
                tr.haveLastLapGain = true;
            }
            else
            {
                // Missed a crossing (car was outside the shown window), or
                // first sighting: no trustworthy figure until the next lap.
                tr.haveLastLapGain = false;
            }
            tr.placeAtLine = static_cast<int>(v->mPlace);
            tr.lapsSeen = v->mTotalLaps;
        }
        row.haveLastLapGain = isRaceSession && tr.haveLastLapGain;
        row.lastLapGain = tr.lastLapGain;

        // The player's own row comparing itself to itself is meaningless --
        // force it to exactly 0, no trend. In a race, the leader's row
        // naturally shows "gap between the player and the leader" via the
        // same formula above -- that's the number that used to (uselessly)
        // live on the player's own row instead.
        double trendPerSecond = 0.0;
        double trackTrend = 0.0;
        if (isPlayer)
        {
            gap = 0.0;
            trackGap = 0.0;
        }
        else
        {
            if (!tr.has)
            {
                tr.lastGap = gap;
                tr.lastTime = now;
                tr.has = true;
                tr.prevDisplayedGap = gap;
                tr.displayedGap = gap;
                tr.prevDisplayTime = now;
                tr.lastDisplayTime = now;
                tr.lastTrackGap = trackGap;
                tr.prevDisplayedTrackGap = trackGap;
                tr.displayedTrackGap = trackGap;
            }
            else
            {
                const double sinceTrendSec = static_cast<double>(now.QuadPart - tr.lastTime.QuadPart) / static_cast<double>(m_freq.QuadPart);
                if (sinceTrendSec >= windowSeconds)
                {
                    // A jump bigger than wrapGuardSeconds means the car
                    // crossed the half-lap wrap boundary, not that it
                    // suddenly gained/lost that much -- re-baseline instead
                    // of recording a nonsense trend spike.
                    const double delta = gap - tr.lastGap;
                    if (fabs(delta) < wrapGuardSeconds)
                        tr.trendPerSecond = delta / sinceTrendSec;
                    tr.lastGap = gap;
                    tr.lastTime = now;

                    const double trackDelta = trackGap - tr.lastTrackGap;
                    if (fabs(trackDelta) < wrapGuardSeconds)
                        tr.trackTrendPerSecond = trackDelta / sinceTrendSec;
                    tr.lastTrackGap = trackGap;
                }

                // The displayed number follows every ~5Hz scoring tick (the
                // colour is judged per sector instead, see UpdateSectors).
                // Once per tick: a car's row can still be built more than
                // once in one OnScoring (computeRow's uncached path), and a
                // second shift would collapse prev onto cur and kill the slide.
                if (tr.lastDisplayTime.QuadPart != now.QuadPart)
                {
                    // Shift cur -> prev before overwriting, so Draw() can
                    // interpolate between two real samples instead of
                    // snapping. A jump bigger than wrapGuardSeconds is a
                    // genuine wrap (half-lap/whole-lap boundary), not real
                    // motion -- skip the interpolation for just this one
                    // transition (collapse prev to the new value too) so
                    // the number doesn't visibly sweep across the wrap.
                    const double gapJump = gap - tr.displayedGap;
                    tr.prevDisplayedGap = (fabs(gapJump) < wrapGuardSeconds) ? tr.displayedGap : gap;
                    const double trackJump = trackGap - tr.displayedTrackGap;
                    tr.prevDisplayedTrackGap = (fabs(trackJump) < wrapGuardSeconds) ? tr.displayedTrackGap : trackGap;
                    tr.prevDisplayTime = tr.lastDisplayTime;

                    tr.displayedGap = gap;
                    tr.displayedTrackGap = trackGap;
                    tr.lastDisplayTime = now;
                }
            }
            trendPerSecond = tr.trendPerSecond;
            trackTrend = tr.trackTrendPerSecond;
            gap = tr.displayedGap;
            trackGap = tr.displayedTrackGap;
        }

        row.valid = true;
        row.isPlayer = isPlayer;
        row.inPits = !isPlayer && v->mInPits != 0 && !inPits;
        {
            const double vSpeed = sqrt(v->mLocalVel.x * v->mLocalVel.x
                                     + v->mLocalVel.y * v->mLocalVel.y
                                     + v->mLocalVel.z * v->mLocalVel.z);
            row.stopped = !isPlayer && vSpeed < 1.5;
        }
        row.gapAheadSeconds = isPlayer ? 0.0 : -tAheadSeconds;
        row.gapBehindSeconds = isPlayer ? 0.0 : tBehindSeconds;
        row.carId = v->mID;
        row.place = v->mPlace;
        // Review F89: folded to ASCII (case kept) -- the font skips accented
        // bytes, so "Raikkonen" lost letters.
        DriverAbbrev::FoldToAscii(v->mDriverName, row.driverName, sizeof(row.driverName));
        if (!row.driverName[0]) strcpy_s(row.driverName, "?");
        {
            // Car number: rF2 has no field for it, but entry names carry it
            // as "#19" (e.g. "Baggies CZ eMotorsport 2012 #19").
            row.carNumber[0] = 0;
            const char* vn = v->mVehicleName;
            for (int k = 0; k < 63 && vn[k]; ++k)
            {
                if (vn[k] == '#' && vn[k + 1] >= '0' && vn[k + 1] <= '9')
                {
                    int n = 0;
                    row.carNumber[n++] = '#';
                    for (int j = k + 1; j < 64 && vn[j] >= '0' && vn[j] <= '9' && n < 6; ++j)
                        row.carNumber[n++] = vn[j];
                    row.carNumber[n] = 0;
                    break;
                }
            }
        }
        row.lastLapTime = v->mLastLapTime;
        row.gapSeconds = gap;
        row.trendPerSecond = trendPerSecond;
        row.sectorVerdict = isPlayer ? 0 : tr.sectorVerdict;
        row.carInPits = !isPlayer && v->mInPits != 0;
        row.updatedAt = now;
        if (isPlayer)
        {
            row.gapSecondsPrev = 0.0;
            row.gapPrevTime = now;
            row.gapCurTime = now;
        }
        else
        {
            row.gapSecondsPrev = tr.prevDisplayedGap;
            row.gapPrevTime = tr.prevDisplayTime;
            row.gapCurTime = tr.lastDisplayTime;
        }

        row.lapFasterThanPlayer = false;
        row.lapSlowerThanPlayer = false;
        if (!isPlayer && v->mLastLapTime > 0.0 && playerLastLap > 0.0)
        {
            if (v->mLastLapTime < playerLastLap - 0.001) row.lapFasterThanPlayer = true;
            else if (v->mLastLapTime > playerLastLap + 0.001) row.lapSlowerThanPlayer = true;
        }

        // "A lap down" measured from CUMULATIVE DISTANCE rounded to whole
        // laps, not from mTotalLaps directly: the two cars cross the line at
        // slightly different moments, so a raw lap-count comparison flickers
        // for a car running right alongside you. Rounding the distance
        // difference only reads >= 1 once a genuine full lap separates you.
        row.lapDown = false;
        row.lapDelta = 0;
        if (!isPlayer && isRaceSession && trackLength > 1.0)
        {
            const double vTotalDist = static_cast<double>(v->mTotalLaps) * trackLength + v->mLapDist;
            // Review F80: a lap counts from 0.9 laps apart (as TinyPedal),
            // not from half a lap, which marked cars half a lap away +1/-1.
            const double lapsRaw = (playerTotalDist - vTotalDist) / trackLength;
            double lapsBehind = (lapsRaw >= 0.0) ? floor(lapsRaw + 0.1) : ceil(lapsRaw - 0.1);
            // Review F90: NaN from a half-loaded struct made the int cast
            // INT_MIN; clamp to what the column can print.
            if (!(lapsBehind == lapsBehind)) lapsBehind = 0.0;
            if (lapsBehind > 99.0) lapsBehind = 99.0;
            if (lapsBehind < -99.0) lapsBehind = -99.0;
            row.lapDown = lapsBehind >= 1.0;
            // +ve = they are AHEAD of you by this many whole laps (they have
            // lapped you), -ve = they are BEHIND by this many (you have
            // lapped them).
            row.lapDelta = -static_cast<int>(lapsBehind);
        }

        row.trackRelative = false;
        row.trackGapReliable = usedTrace;
        row.trackRelativeGapSeconds = trackGap;
        row.trackRelativeGapSecondsPrev = isPlayer ? 0.0 : tr.prevDisplayedTrackGap;
        row.trackRelativeTrendPerSecond = trackTrend;
        return row;
    };

    // Position-change highlight: tracked for EVERY car (including the
    // player), keyed by car ID so an overtake follows the car that moved
    // rather than whichever row/place it happens to occupy right now. A
    // raw mPlace change is only confirmed once the new place has held
    // steady for positionChangeDebounceSeconds -- see TrendTracker's
    // comment for why (first-lap/first-corner jostling would otherwise
    // flash the highlight constantly).
    auto updatePlaceChange = [&](const VehicleScoringInfoV01* v, TrendTracker& tr, Row& row)
    {
        {
            const int currentPlace = static_cast<int>(v->mPlace);
            if (tr.lastConfirmedPlace < 0)
            {
                tr.lastConfirmedPlace = currentPlace;
                tr.pendingPlace = currentPlace;
                tr.pendingPlaceTime = now;
            }
            else if (currentPlace != tr.pendingPlace)
            {
                tr.pendingPlace = currentPlace;
                tr.pendingPlaceTime = now;
            }
            else if (currentPlace != tr.lastConfirmedPlace)
            {
                double debounceSeconds = g_relativeTimingConfig.positionChangeDebounceSeconds;
                if (debounceSeconds < 0.0) debounceSeconds = 0.0;
                const double sincePending = static_cast<double>(now.QuadPart - tr.pendingPlaceTime.QuadPart) / static_cast<double>(m_freq.QuadPart);
                if (sincePending >= debounceSeconds)
                {
                    tr.placeChangeSign = (currentPlace < tr.lastConfirmedPlace) ? -1 : 1;
                    tr.placeChangeTime = now;
                    tr.lastConfirmedPlace = currentPlace;
                }
            }
        }
        row.placeChangeSign = 0;
        row.placeChangeTime = {};
        if (tr.placeChangeSign != 0)
        {
            double holdSeconds = g_relativeTimingConfig.positionChangeHoldSeconds;
            if (holdSeconds < 0.1) holdSeconds = 0.1;
            const double sinceChange = (tr.placeChangeTime.QuadPart == 0) ? 1.0e9
                : static_cast<double>(now.QuadPart - tr.placeChangeTime.QuadPart) / static_cast<double>(m_freq.QuadPart);
            if (sinceChange < holdSeconds)
            {
                row.placeChangeSign = tr.placeChangeSign;
                row.placeChangeTime = tr.placeChangeTime;  // faded by Draw() (F364)
            }
            else
            {
                tr.placeChangeSign = 0;
            }
        }
    };

    // One vehicle's row. The same car is asked for up to four times a tick
    // (the standings window, the track-relative pass, the leader's pinned
    // row, the gap box), each of which used to redo the whole row, trace
    // lookups included -- so it is built once and reused. Everything in it
    // comes out the same on a repeat within a tick, except the position-
    // change step, which is re-run on every request exactly as before (with
    // a zero debounce, a repeat request is what confirms a new place). Not
    // cached once more cars have been seen than there are trackers: the
    // overflow cars then share one, and each request has to see the others'
    // changes to it, as it always did.
    const bool rowCacheUsable = m_trackerCount < kMaxTrackedCars;
    bool rowCached[kMaxRowCache] = {};
    auto computeRow = [&](const VehicleScoringInfoV01* v) -> Row
    {
        const ptrdiff_t idx = v - info.mVehicle;
        if (!rowCacheUsable || idx < 0 || idx >= kMaxRowCache)
        {
            TrendTracker& tr = GetTracker(v->mID);
            Row row = buildRow(v, tr);
            updatePlaceChange(v, tr, row);
            return row;
        }
        if (!rowCached[idx])
        {
            m_rowCacheTracker[idx] = &GetTracker(v->mID);
            m_rowCache[idx] = buildRow(v, *m_rowCacheTracker[idx]);
            rowCached[idx] = true;
        }
        Row row = m_rowCache[idx];
        updatePlaceChange(v, *m_rowCacheTracker[idx], row);
        return row;
    };

    State newState;
    newState.totalLapsFractional = totalLapsFractional;

    // Session fastest lap, from every car's own best lap.
    for (long k = 0; k < info.mNumVehicles; ++k)
    {
        const double best = info.mVehicle[k].mBestLapTime;
        if (best > 5.0 && best < 100000.0 && !isTrainer(&info.mVehicle[k])
            && (newState.fastestLapSeconds <= 0.0 || best < newState.fastestLapSeconds))
        {
            newState.fastestLapSeconds = best;
            DriverAbbrev::FoldToAscii(info.mVehicle[k].mDriverName, newState.fastestLapName, sizeof(newState.fastestLapName));
        }
        if (info.mVehicle[k].mIsPlayer && best > 5.0 && best < 100000.0)
            newState.myBestLapSeconds = best;
    }
    newState.stintLapsFractional = totalLapsFractional - m_stintStartTotalLaps;

    if (standingsOrder)
    {
        // Review F83: the window is cut from the RUNNING cars in place
        // order, not from places 1..mNumVehicles -- retired cars (whom rF2
        // classifies behind the field) used to eat rows and hide the
        // "leader behind you" row. rank = 1-based index into that list.
        const VehicleScoringInfoV01* running[256];
        int numVehicles = 0;
        for (long i = 0; i < info.mNumVehicles && numVehicles < 256; ++i)
        {
            const VehicleScoringInfoV01* v = &info.mVehicle[i];
            if (isRetired(v) || v->mPlace == 0) continue;
            running[numVehicles++] = v;
        }
        std::sort(running, running + numVehicles, [](const VehicleScoringInfoV01* a, const VehicleScoringInfoV01* b)
        {
            return a->mPlace < b->mPlace;
        });
        int playerRank = 0;
        for (int i = 0; i < numVehicles; ++i)
            if (running[i]->mIsPlayer) { playerRank = i + 1; break; }

        // Wanted ranks: a window of EXACTLY visibleCars cars centred on the
        // player (widened on whichever side still has room near the front
        // or back, capped at the field size), with the leader always one of
        // them -- swapped in for whichever end sits farthest from the
        // player rather than added on top, so the slider still has an
        // effect in a small field.
        const int visibleCars = static_cast<int>(g_relativeTimingConfig.visibleCars + 0.5f);
        const int windowSize = (std::min)((std::max)(2, visibleCars), numVehicles);
        const int halfWindow = (windowSize - 1) / 2;
        int windowLow = playerRank - halfWindow;
        int windowHigh = windowLow + windowSize - 1;
        if (windowLow < 1) { windowHigh += (1 - windowLow); windowLow = 1; }
        if (windowHigh > numVehicles) { windowLow -= (windowHigh - numVehicles); windowHigh = numVehicles; }
        if (windowLow < 1) windowLow = 1;
        if (windowHigh > numVehicles) windowHigh = numVehicles;

        bool wanted[257] = {};
        for (int p = windowLow; p <= windowHigh; ++p)
        {
            if (p >= 1 && p <= 256) wanted[p] = true;
        }
        if (!wanted[1] && windowSize < numVehicles)
        {
            if ((playerRank - windowLow) >= (windowHigh - playerRank))
            {
                wanted[windowLow] = false;
                ++windowLow;
            }
            else
            {
                wanted[windowHigh] = false;
                --windowHigh;
            }
            wanted[1] = true;
        }

        for (int p = 1; p <= numVehicles && newState.rowCount < RelativeTimingStore::kMaxRows; ++p)
        {
            if (!wanted[p]) continue;
            newState.rows[newState.rowCount++] = computeRow(running[p - 1]);
        }

        // Second pass: a car on a DIFFERENT lap that is physically close to
        // you needs a row the place-based window above can never produce --
        // if you are last, nobody is "2 places behind" you, so the leader
        // coming to lap you would otherwise only ever show as a -112s
        // standings gap with nothing saying he is 2s off your gearbox. Give
        // those cars a second row carrying the TRACK-relative gap instead.
        Row extras[RelativeTimingStore::kMaxTrackRelativeRows];
        int extraCount = 0;
        const double threshold = g_relativeTimingConfig.trackRelativeThresholdSeconds;
        for (long i = 0; i < info.mNumVehicles; ++i)
        {
            const VehicleScoringInfoV01* v = &info.mVehicle[i];
            if (v->mIsPlayer || isRetired(v)) continue;

            const Row candidate = computeRow(v);
            // A car in the pit lane has no meaningful track position.
            if (candidate.inPits) continue;
            // Same lap: the standings gap already IS the track gap, so a
            // duplicate would just repeat the row it came from.
            if (candidate.lapDelta == 0) continue;
            // Without a real reference (this session's own clean lap, or a
            // saved personal best) this number falls back to a crude
            // constant-speed guess that can be wrong in both magnitude AND
            // sign -- showing it as if it were a precise "closing in to lap
            // you" gap would be actively misleading rather than just
            // unavailable, so skip it until a real reference exists.
            if (!candidate.trackGapReliable) continue;
            if (fabs(candidate.trackRelativeGapSeconds) > threshold) continue;

            Row extra = candidate;
            extra.trackRelative = true;
            extra.gapSeconds = candidate.trackRelativeGapSeconds;
            extra.gapSecondsPrev = candidate.trackRelativeGapSecondsPrev;
            extra.trendPerSecond = candidate.trackRelativeTrendPerSecond;

            // Keep the closest few if more than kMaxTrackRelativeRows qualify.
            if (extraCount < RelativeTimingStore::kMaxTrackRelativeRows)
            {
                extras[extraCount++] = extra;
            }
            else
            {
                int worst = 0;
                for (int k = 1; k < extraCount; ++k)
                    if (fabs(extras[k].gapSeconds) > fabs(extras[worst].gapSeconds)) worst = k;
                if (fabs(extra.gapSeconds) < fabs(extras[worst].gapSeconds)) extras[worst] = extra;
            }
        }

        std::sort(extras, extras + extraCount, [](const Row& a, const Row& b)
        {
            return a.gapSeconds < b.gapSeconds;
        });
        for (int i = 0; i < extraCount && newState.rowCount < RelativeTimingStore::kMaxRows; ++i)
            newState.rows[newState.rowCount++] = extras[i];

        // Last place has nobody physically "behind" for the window above to
        // ever surface -- pin one extra row for exactly that: how far
        // behind you the leader is, going the OTHER way around the loop
        // from the standings row above. On a closed circuit the leader is
        // ALWAYS both some distance ahead of you (the short way) and some
        // distance behind you (the short way was already shown above as
        // the classification gap; wrapping trackRelativeGapSeconds to
        // whichever direction is NEAREST -- which used to be all this row
        // did -- meant it showed "ahead" every single time the leader is
        // faster than you, since the short way round is essentially always
        // the ahead direction for a faster car. That's not a second
        // opinion on the gap, it's the same fact restated -- what's
        // actually missing is the BEHIND figure, so that's what this row
        // shows now: the wrap-around complement, always positive.
        // F83: "last" is the last RUNNING car; and no second leader row
        // when the extras above already show him coming up behind.
        bool leaderInExtras = false;
        for (int i = 0; i < extraCount; ++i)
            if (numVehicles > 0 && extras[i].carId == running[0]->mID && extras[i].gapSeconds > 0.0) leaderInExtras = true;
        if (playerRank == numVehicles && !leaderInExtras && newState.rowCount < RelativeTimingStore::kMaxRows)
        {
            const VehicleScoringInfoV01* leader = (numVehicles > 0) ? running[0] : nullptr;
            if (leader && !leader->mIsPlayer)
            {
                const Row leaderRow = computeRow(leader);
                // Same reliability gate as the extras loop above -- without
                // a real reference lap yet (early in a session/race, before
                // a clean lap or a saved PB exists), this falls back to the
                // crude constant-speed guess, which can be badly wrong.
                // The classification row already shown above stays
                // trustworthy regardless.
                if (leaderRow.trackGapReliable && wrapSpan > 0.0 && wrapSpan < 1.0e8)
                {
                    // How long the LEADER needs to reach where the player is:
                    // the distance he has left to cover, at HIS OWN pace (his
                    // previous lap time over the player's reference lap), not
                    // the player's. Always positive. Shown whatever the leader
                    // is doing -- in the pits included -- since it is still
                    // the time he would need.
                    Row extra = leaderRow;
                    extra.trackRelative = true;
                    extra.gapSeconds = leaderRow.gapBehindSeconds;
                    extra.gapSecondsPrev = leaderRow.gapBehindSeconds; // live at the ~5Hz scoring rate, nothing to sweep from
                    // This figure grows whenever trackRelativeGapSeconds does
                    // (whether the leader is currently the nearer car ahead
                    // or behind), so the trend carries over unchanged:
                    // positive = growing = moving in the player's favour.
                    extra.trendPerSecond = leaderRow.trackRelativeTrendPerSecond;
                    newState.rows[newState.rowCount++] = extra;
                }
            }
        }
    }
    else
    {
        // Nearest-by-distance: compute every car's row, then keep whichever
        // ones have the smallest absolute gap to the player -- regardless of
        // "place" -- and display them ordered ahead-to-behind.
        // Review F95: the whole field (SDK max 128), not the first 64
        // array entries, so a car at the back -- or you -- is never missed.
        // F91: cars parked in their garage stall and the trainer ghost skipped.
        static const int kMaxCandidates = kMaxRowCache;
        Row* const candidates = m_candidates;
        int candidateCount = 0;
        for (long i = 0; i < info.mNumVehicles && candidateCount < kMaxCandidates; ++i)
        {
            if (isRetired(&info.mVehicle[i]) || isParked(&info.mVehicle[i])) continue;
            candidates[candidateCount++] = computeRow(&info.mVehicle[i]);
        }

        std::sort(candidates, candidates + candidateCount, [](const Row& a, const Row& b)
        {
            return fabs(a.gapSeconds) < fabs(b.gapSeconds);
        });

        // The user-adjustable "cars shown" slider (visibleCars) caps how
        // many of the nearest-by-distance cars are kept, same knob the race
        // window above uses -- the extra rows added below (race-only) don't
        // apply here.
        const int visibleCars = static_cast<int>(g_relativeTimingConfig.visibleCars + 0.5f);
        // Bounded so the wrap row below still fits in rows[] (F86 guard).
        int keep = (std::min)((std::min)(candidateCount, visibleCars), RelativeTimingStore::kMaxRows - 1);
        if (keep < 0) keep = 0;

        // With few cars on track, the nearest-by-distance pick above can
        // come out entirely on one side -- e.g. with just one opponent,
        // whichever direction happens to be the shorter way round wins, and
        // the other side of the table (ahead, or behind) is simply empty.
        // But on a circuit the SAME car is always both some distance ahead
        // of you going forward AND some distance behind you going the other
        // way round the lap -- so if a side is missing a real opponent,
        // fill it with the closest car's WRAP-AROUND complement instead of
        // leaving it blank. Only a genuinely empty track (no other cars at
        // all) should ever show one side missing.
        bool haveAhead = false, haveBehind = false;
        for (int i = 0; i < keep; ++i)
        {
            if (candidates[i].isPlayer) continue;
            if (candidates[i].gapSeconds < 0.0) haveAhead = true;
            else if (candidates[i].gapSeconds > 0.0) haveBehind = true;
        }
        if ((!haveAhead || !haveBehind) && wrapSpan > 0.0 && wrapSpan < 1.0e8 && keep < kMaxCandidates + 1)
        {
            int bestIdx = -1;
            double bestAbs = 1.0e18;
            for (int i = 0; i < candidateCount; ++i)
            {
                if (candidates[i].isPlayer) continue;
                const double complement = (candidates[i].gapSeconds < 0.0)
                    ? candidates[i].gapBehindSeconds
                    : candidates[i].gapAheadSeconds;
                const bool fillsAhead = !haveAhead && complement < 0.0;
                const bool fillsBehind = !haveBehind && complement > 0.0;
                if ((fillsAhead || fillsBehind) && fabs(complement) < bestAbs)
                {
                    bestAbs = fabs(complement);
                    bestIdx = i;
                }
            }
            if (bestIdx >= 0)
            {
                // The same car the OTHER way round: if it is ahead of you,
                // how long it needs to reach you; if it is behind, how long
                // you need to reach it. Set straight from the two directional
                // figures rather than wrapping the nearest one by my lap time.
                Row wrapped = candidates[bestIdx];
                wrapped.gapSeconds = (wrapped.gapSeconds < 0.0) ? wrapped.gapBehindSeconds
                                                                : wrapped.gapAheadSeconds;
                // Prev collapsed onto the new value so the display does not
                // sweep from the primary row's number across to this one.
                wrapped.gapSecondsPrev = wrapped.gapSeconds;
                // Trend left as it was: the other-way-round figure grows
                // exactly when the nearest one does (closing on a car ahead
                // makes the time it needs to reach you the long way round
                // grow too), so positive still means the player's favour.
                candidates[keep++] = wrapped;
            }
        }

        std::sort(candidates, candidates + keep, [](const Row& a, const Row& b)
        {
            return a.gapSeconds < b.gapSeconds;
        });

        for (int i = 0; i < keep && newState.rowCount < RelativeTimingStore::kMaxRows; ++i)
            newState.rows[newState.rowCount++] = candidates[i];
    }

    // Gap box rows: whole field ordered by place, two ahead of the player
    // and one behind.
    {
        const VehicleScoringInfoV01* order[256];
        int count = 0;
        for (long i = 0; i < info.mNumVehicles && count < 256; ++i)
        {
            const VehicleScoringInfoV01* v = &info.mVehicle[i];
            if (isRetired(v) || isTrainer(v) || v->mPlace == 0) continue;
            order[count++] = v;
        }
        std::sort(order, order + count, [](const VehicleScoringInfoV01* a, const VehicleScoringInfoV01* b)
        {
            return a->mPlace < b->mPlace;
        });
        int me = -1;
        for (int i = 0; i < count; ++i)
            if (order[i]->mIsPlayer) { me = i; break; }
        newState.gapRowCount = 0;
        if (me >= 0)
            for (int i = (std::max)(0, me - 2); i <= me + 2 && i < count; ++i)
            {
                Row gr = computeRow(order[i]);
                // Review F88: these rows are picked by PLACE, so outside the
                // standings-order race mode they need a place gap, not the
                // shorter-way-round track gap computeRow gives there: the
                // race interval in a race, the best-lap difference otherwise.
                if (!standingsOrder && !gr.isPlayer)
                {
                    const VehicleScoringInfoV01* v = order[i];
                    if (isRaceSession)
                    {
                        const double lapsDiff = static_cast<double>(v->mLapsBehindLeader) - static_cast<double>(player->mLapsBehindLeader);
                        const double lapSeconds = (steadyLap > 0.0) ? steadyLap : wrapSpan;
                        gr.gapSeconds = lapsDiff * lapSeconds + (v->mTimeBehindLeader - player->mTimeBehindLeader);
                    }
                    else
                    {
                        const double theirs = v->mBestLapTime, mine = player->mBestLapTime;
                        if (theirs > 5.0 && theirs < 100000.0 && mine > 5.0 && mine < 100000.0)
                            gr.gapSeconds = theirs - mine;
                        else
                            gr.gapUnknown = true;
                    }
                    // 5 Hz figures with no slide of their own.
                    gr.gapSecondsPrev = gr.gapSeconds;
                    gr.trendPerSecond = 0.0;
                }
                newState.gapRows[newState.gapRowCount++] = gr;
            }
    }

    // Diagnostic (off by default): does the SDK's own gap data actually
    // update continuously? If mTimeBehindNext/mTimeBehindLeader tick along
    // smoothly rather than only changing at line crossings, they are the
    // game's own numbers and a better source than the reconstructed lap
    // trace -- worth reading the log before assuming either way.
    if (g_relativeTimingConfig.logSdkGapFields)
    {
        static LARGE_INTEGER s_lastSdkLog = {};
        const double sinceLogSec = (s_lastSdkLog.QuadPart == 0) ? 1.0e9
            : static_cast<double>(now.QuadPart - s_lastSdkLog.QuadPart) / static_cast<double>(m_freq.QuadPart);
        if (sinceLogSec >= 1.0)
        {
            s_lastSdkLog = now;
            Log::Write("RelativeTiming/SDK: ET=%.3f player P%u behindNext=%.3f (%ld laps) behindLeader=%.3f (%ld laps) lapDist=%.1f",
                       info.mCurrentET, static_cast<unsigned>(player->mPlace),
                       player->mTimeBehindNext, player->mLapsBehindNext,
                       player->mTimeBehindLeader, player->mLapsBehindLeader,
                       player->mLapDist);
        }
    }

    // Race gap diagnostic, every 10 s: what each shown row displays next to
    // the SDK figures it was computed from. Added after the gaps went badly
    // wrong in a 1-hour race while the game's own HUD was fine -- the log
    // then had nothing to explain it. Cheap (buffered while driving). Off
    // unless LogRaceGaps=1.
    if (isRaceSession && g_relativeTimingConfig.logRaceGaps)
    {
        static LARGE_INTEGER s_lastGapLog = {};
        const double sinceGapLog = (s_lastGapLog.QuadPart == 0) ? 1.0e9
            : static_cast<double>(now.QuadPart - s_lastGapLog.QuadPart) / static_cast<double>(m_freq.QuadPart);
        if (sinceGapLog >= 10.0)
        {
            s_lastGapLog = now;
            Log::Write("RelativeTiming/race: ET=%.1f me P%d lap %ld behindLeader %.3f (%ld laps) wrapSpan %.3f trace=%d pbTrace=%d",
                       info.mCurrentET, playerPlace, player->mTotalLaps, player->mTimeBehindLeader,
                       player->mLapsBehindLeader, wrapSpan, haveTrace ? 1 : 0, havePbTrace ? 1 : 0);
            for (int r = 0; r < newState.rowCount; ++r)
            {
                const Row& row = newState.rows[r];
                if (row.isPlayer) continue;
                const VehicleScoringInfoV01* v = nullptr;
                for (long k = 0; k < info.mNumVehicles; ++k)
                    if (info.mVehicle[k].mID == row.carId) { v = &info.mVehicle[k]; break; }
                if (!v) continue;
                Log::Write("  P%u %-20.20s shown %+8.2f | behindLeader %8.3f (%ld laps) lap %ld dist %6.0f pits %d | ahead %.2f behind %.2f",
                           static_cast<unsigned>(row.place), row.driverName, row.gapSeconds,
                           v->mTimeBehindLeader, v->mLapsBehindLeader, v->mTotalLaps, v->mLapDist,
                           v->mInPits ? 1 : 0, row.gapAheadSeconds, row.gapBehindSeconds);
            }
        }
    }

    for (int r = 0; r < newState.rowCount; ++r)
    {
        Row& row = newState.rows[r];
        row.lastLapIsSessionFastest = newState.fastestLapSeconds > 0.0
            && row.lastLapTime > 0.0 && fabs(row.lastLapTime - newState.fastestLapSeconds) < 0.0005;
    }
    // Review F93: the gap box rows too, or they never showed purple.
    for (int r = 0; r < newState.gapRowCount; ++r)
    {
        Row& row = newState.gapRows[r];
        row.lastLapIsSessionFastest = newState.fastestLapSeconds > 0.0
            && row.lastLapTime > 0.0 && fabs(row.lastLapTime - newState.fastestLapSeconds) < 0.0005;
    }

    // Chequered flag: freeze the table as it last was. After the leader
    // finishes, the game's behind-leader figures go haywire (logged: one car
    // 56 s -> 65 s -> -22 s within 30 s, places reshuffling).
    // A freeze belongs to the session it was set in. rF2 can send a scoring
    // update AFTER EndSession (which clears everything) -- with the old race
    // still finished and the cars back in the garage -- and that froze a
    // garbage table which then stayed up for the whole of the NEXT race
    // (logged 25 Oct: P1 all race, table stuck on the previous session's
    // rows). So lift it as soon as the session type changes, the session
    // clock goes backwards, or the game phase is back before the green flag.
    if (m_raceOverFrozen
        && (info.mSession != m_frozenSession
            || info.mCurrentET < m_frozenET - 1.0
            || info.mGamePhase < 5))
    {
        m_raceOverFrozen = false;
        Log::Write("RelativeTiming: new session -- table unfrozen");
    }
    if (isRaceSession && !m_raceOverFrozen)
    {
        // Not on rF2's game phase 8 alone (CrewChief review 2026-11-24):
        // rF2 sets phase 8 as soon as the LEADER takes the flag, which froze
        // the table for the whole of your own last lap. Your own finish
        // status is the real signal. The one exception kept as a safety net:
        // phase 8 while you sit in the garage stall -- you will never cross
        // the line, so the race is over for you anyway.
        bool anyFinished = false;
        // Review F254: freeze when YOU are done (finished, DNF, DQ), not
        // when any car takes the flag -- a multiclass leader finishing
        // froze the table while you were still racing.
        for (long k = 0; k < info.mNumVehicles && !anyFinished; ++k)
            if (info.mVehicle[k].mIsPlayer
                && (info.mVehicle[k].mFinishStatus != 0
                    || (info.mGamePhase == 8 && info.mVehicle[k].mInGarageStall != 0)))
                anyFinished = true;
        if (anyFinished)
        {
            m_raceOverFrozen = true;
            m_frozenSession = info.mSession;
            m_frozenET = info.mCurrentET;
            Log::Write("RelativeTiming: race over -- table frozen");
        }
    }
    if (m_raceOverFrozen)
    {
        // The table stays as it was, but its tyre and laps-since-stop
        // columns kept following the cars back when Draw() looked them up
        // live, so refresh just those. Looked up outside m_lock (it is the
        // one Draw() takes), then applied wherever the row is still the
        // same car.
        State frozen = Read();
        LookUpOtherStores(frozen.rows, frozen.rowCount);
        LookUpOtherStores(frozen.gapRows, frozen.gapRowCount);
        EnterCriticalSection(&m_lock);
        for (int r = 0; r < m_state.rowCount && r < frozen.rowCount; ++r)
            if (m_state.rows[r].carId == frozen.rows[r].carId) m_state.rows[r].lookups = frozen.rows[r].lookups;
        for (int r = 0; r < m_state.gapRowCount && r < frozen.gapRowCount; ++r)
            if (m_state.gapRows[r].carId == frozen.gapRows[r].carId) m_state.gapRows[r].lookups = frozen.gapRows[r].lookups;
        LeaveCriticalSection(&m_lock);
        return;
    }

    LookUpOtherStores(newState.rows, newState.rowCount);
    LookUpOtherStores(newState.gapRows, newState.gapRowCount);

    EnterCriticalSection(&m_lock);
    m_state = newState;
    LeaveCriticalSection(&m_lock);
}

RelativeTimingStore::State RelativeTimingStore::Read()
{
    State copy;
    if (!m_ready) return copy;
    EnterCriticalSection(&m_lock);
    copy = m_state;
    LeaveCriticalSection(&m_lock);
    return copy;
}

namespace
{
    // Upper bound as well as lower: a partially-initialised scoring struct
    // (the moment a session loads) can hand over a wild lap time, and a huge
    // minutes value would both overflow the int cast and print more
    // characters than `out` holds -- sprintf_s aborts the process on that
    // rather than truncating.
    // Session-fastest-lap purple (the usual timing-screen convention).
    const float kPurpleR = 0.75f, kPurpleG = 0.35f, kPurpleB = 1.0f;

    void FormatLapTime(double seconds, char* out, size_t outSize)
    {
        if (!(seconds > 0.0) || seconds >= 6000.0)
        {
            sprintf_s(out, outSize, "--:--.-");
            return;
        }
        // Tenths only -- this table is for at-a-glance comparison between
        // cars, not precision timing. Rounded to tenths BEFORE splitting off
        // the minutes, or 1:59.97 printed as "1:60.0" (review F92).
        const long long tenths = llround(seconds * 10.0);
        const long long minutes = tenths / 600;
        const long long rem = tenths % 600;
        _snprintf_s(out, outSize, _TRUNCATE, "%lld:%02lld.%lld", minutes, rem / 10, rem % 10);
    }

    // "%+.1f" prints the same text for any two values with the same sign
    // and the same nearest tenth, so a sliding gap only needs printing again
    // when one of those changes. This gives the two (tenths, negative), or
    // false when v sits too near a rounding boundary (or is too big) for
    // that to be certain without printing it -- the caller then just does.
    bool GapTextKey(double v, long long& tenths, bool& negative)
    {
        const double t = v * 10.0;
        if (!(fabs(t) < 1.0e9))   // NaN and infinity too
            return false;
        const double k = floor(t + 0.5);
        if (fabs(t - k) > 0.499)
            return false;
        tenths = static_cast<long long>(k);
        negative = std::signbit(v);
        return true;
    }

    // Gap colour from the last sector both cars ran (Row::sectorVerdict):
    // green the player was faster, red they were -- the same for a car
    // ahead (pulling away) and behind (catching) -- purple for a session-
    // best sector, plain until there is a comparison.
    void SectorColor(int verdict, const RelativeTimingConfig& cfg, float& r, float& g, float& b)
    {
        if (verdict == 2)       { r = kPurpleR; g = kPurpleG; b = kPurpleB; }
        else if (verdict == 1)  { r = cfg.gainingR; g = cfg.gainingG; b = cfg.gainingB; }
        else if (verdict == -1) { r = cfg.losingR; g = cfg.losingG; b = cfg.losingB; }
        else                    { r = cfg.colorR; g = cfg.colorG; b = cfg.colorB; }
    }
}

namespace
{
    // Bottom edge of the standings table as last drawn -- see the header for
    // why this is cached rather than recomputed.
    float s_lastBottomEdgeY = -1.0f;
}

float RelativeTiming::BottomEdgeY(int screenWidth, int screenHeight)
{
    (void)screenWidth;
    if (s_lastBottomEdgeY >= 0.0f)
        return s_lastBottomEdgeY;
    return screenHeight * g_relativeTimingConfig.posYFraction;
}

static bool DrawPanel(Renderer& renderer, int screenWidth, int screenHeight, bool gapBox);

bool RelativeTiming::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    return DrawPanel(renderer, screenWidth, screenHeight, false);
}

bool RelativeTiming::DrawGapBox(Renderer& renderer, int screenWidth, int screenHeight)
{
    return DrawPanel(renderer, screenWidth, screenHeight, true);
}

static bool DrawPanel(Renderer& renderer, int screenWidth, int screenHeight, bool gapBox)
{
    // Was drawn every frame: the gap numbers slide between scoring samples,
    // which a 5Hz replay showed as visible steps. 10 Hz since 2026-11-24
    // (user: gaps show 0.1 s, so they cannot change faster); one throttle per box.
    static RenderThrottle s_throttles[2] = { RenderThrottle(0.1), RenderThrottle(0.1) };
    RenderThrottle& s_throttle = s_throttles[gapBox ? 1 : 0];
    if (!Layout::Active() && s_throttle.TryReplay(renderer))
        return true;
    RenderThrottle::Capture throttleCapture(s_throttle, renderer);

    // This widget paints its own background panel, so the renderer's
    // per-string text plate would just double-darken over it. Restored on
    // every exit path by the scope guard, since Draw() returns from several
    // places.
    struct TextBgGuard
    {
        Renderer& r; bool prev;
        explicit TextBgGuard(Renderer& rr) : r(rr), prev(rr.TextBackgroundEnabled()) { r.SetTextBackground(false); }
        ~TextBgGuard() { r.SetTextBackground(prev); }
    } textBgGuard(renderer);

    const RelativeTimingConfig& cfg = g_relativeTimingConfig;
    // Review F365: the standings' cached bottom edge is only live while
    // the table draws -- other panels (BrakeCue, the gap box) place
    // themselves under it.
    if (gapBox ? !cfg.showGapBox : !cfg.enabled)
    {
        if (!gapBox) s_lastBottomEdgeY = -1.0f;
        return false;
    }

    RelativeTimingStore::State state = g_relativeTiming.Read();
    if (gapBox)
    {
        for (int i = 0; i < state.gapRowCount; ++i) state.rows[i] = state.gapRows[i];
        state.rowCount = state.gapRowCount;
    }
    if (state.rowCount == 0)
    {
        // Normally nothing to show with no other cars on track -- but that
        // also means the panel never registers with Layout, so it could
        // never be dragged or resized outside an actual multi-car session.
        // Layout mode substitutes one placeholder row so the box exists to
        // grab; every field it needs is on this struct already.
        if (!Layout::Preview())
        {
            if (!gapBox) s_lastBottomEdgeY = -1.0f;
            return false;
        }
        state = RelativeTimingStore::State{};
        state.rowCount = 1;
        state.rows[0].valid = true;
        state.rows[0].isPlayer = true;
        state.rows[0].place = 1;
        strcpy_s(state.rows[0].driverName, "YOU");
    }

    // Layout mode: fill the panel to its full row count with placeholder
    // rows, so it is sized and placed as it will be in a full race rather
    // than as whatever few cars happen to be in the session now.
    if (Layout::Preview())
    {
        const int full = gapBox ? 5
            : (std::min)(static_cast<int>(RelativeTimingStore::kMaxRows),
                         static_cast<int>(g_relativeTimingConfig.visibleCars + 0.5f));
        int lastPlace = 0;
        for (int i = 0; i < state.rowCount; ++i)
            if (state.rows[i].place > lastPlace) lastPlace = state.rows[i].place;
        while (state.rowCount < full)
        {
            RelativeTimingStore::Row& r = state.rows[state.rowCount++];
            r = RelativeTimingStore::Row{};
            r.valid = true;
            r.place = ++lastPlace;
            strcpy_s(r.driverName, "Driver name");
        }
    }

    // Each row's text is kept between frames and printed again only when
    // the value behind it changes -- the key* fields are what it was last
    // printed from. The panel has to be drawn every frame for the sliding
    // gaps, but re-printing ~8 strings a row for two dozen rows each frame,
    // when almost none of them had changed, was most of its cost. One set
    // per panel; render thread only.
    struct RowText
    {
        char place[8];
        char carNumber[8];
        char name[28];
        char lapTime[16];
        char gap[16];
        char lapDiff[8];    // "+1" / "-1" laps vs you; empty on your lap
        bool isPlayer;
        bool differentLap;
        bool lapAhead;      // a lap or more ahead of you: its own colour
        bool inPits;        // car in the pit lane (greys the laps-since-stop column)
        char tyre[8];
        bool haveTyre;
        float tyreR, tyreG, tyreB;
        float tyreRearR, tyreRearG, tyreRearB;  // split set: the rear letter's colour
        float gapR, gapG, gapB;
        float lapR, lapG, lapB;
        int placeChangeSign;
        float placeChangeAlpha;
        char lapGain[8];
        float lapGainR, lapGainG, lapGainB;
        bool pitKnown;      // laps since the last stop known (else "L?", greyed)
        char pit[16];

        bool printed;       // false until this slot's text has been printed once
        unsigned char keyPlace;
        char keyDriverName[32];
        bool keyIsPlayer, keyTrackRelative;
        int keyLapDelta;
        double keyLapTime;
        bool keyShowGain;
        int keyGain;
        bool keyPitKnown;
        int keyPitLaps;
        int keyGapKind;     // 0 blank (your own row), 1 "PIT", 2 a number
        bool keyGapExact;   // the three below are valid (see GapTextKey)
        long long keyGapTenths;
        bool keyGapNegative;
    };
    static RowText s_rowText[2][RelativeTimingStore::kMaxRows];
    RowText* const rows = s_rowText[gapBox ? 1 : 0];

    // One clock read per frame for every row's gap slide, not one per row.
    static LARGE_INTEGER s_freq = {};
    if (s_freq.QuadPart == 0)
        QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    for (int i = 0; i < state.rowCount; ++i)
    {
        const RelativeTimingStore::Row& row = state.rows[i];
        RowText& rt = rows[i];
        const bool first = !rt.printed;
        rt.printed = true;

        if (first || rt.keyPlace != row.place)
        {
            rt.keyPlace = row.place;
            _snprintf_s(rt.place, _TRUNCATE, "P%u", static_cast<unsigned>(row.place));
        }
        strcpy_s(rt.carNumber, row.carNumber);

        // Name and the laps-ahead/behind column, from the same few fields.
        if (first || rt.keyIsPlayer != row.isPlayer || rt.keyTrackRelative != row.trackRelative
            || rt.keyLapDelta != row.lapDelta || strcmp(rt.keyDriverName, row.driverName) != 0)
        {
            rt.keyIsPlayer = row.isPlayer;
            rt.keyTrackRelative = row.trackRelative;
            rt.keyLapDelta = row.lapDelta;
            strncpy_s(rt.keyDriverName, row.driverName, _TRUNCATE);

            char nameBuf[32];
            strncpy_s(nameBuf, row.driverName, _TRUNCATE);
            if (strlen(nameBuf) > 14) nameBuf[14] = '\0';
            // Review F90: truncating prints throughout -- sprintf_s ends the
            // game on overflow.
            if (row.isPlayer)
            {
                _snprintf_s(rt.name, _TRUNCATE, "%s (you)", nameBuf);
            }
            else if (row.trackRelative)
            {
                // The duplicate "where they are on track right now" row -- the
                // lap difference is what distinguishes it from the same car's
                // standings row above, so it is spelled out rather than implied.
                _snprintf_s(rt.name, _TRUNCATE, "%s %+dL", nameBuf, row.lapDelta);
            }
            else
            {
                _snprintf_s(rt.name, _TRUNCATE, "%s", nameBuf);
            }

            // Laps ahead (+) or behind (-) you, in its own column next to the
            // on-track gap; nothing for a car on your lap.
            if (!row.isPlayer && row.lapDelta != 0)
                _snprintf_s(rt.lapDiff, _TRUNCATE, "%+d", row.lapDelta);
            else
                rt.lapDiff[0] = '\0';
        }

        if (first || !(rt.keyLapTime == row.lastLapTime))
        {
            rt.keyLapTime = row.lastLapTime;
            FormatLapTime(row.lastLapTime, rt.lapTime, sizeof(rt.lapTime));
        }

        // Smoothly interpolated between the last two real ~5Hz scoring
        // samples rather than snapping straight to the newest one, so the
        // table animates instead of stepping -- same delayed-interpolation
        // technique CarState.cpp uses for the radar (bounded between two
        // real samples, ~1 tick of presentation delay, never a guess
        // projected forward). Falls back to the raw value if the two
        // samples' timestamps aren't both populated yet (first tick a car
        // is seen).
        double displayGap = row.gapSeconds;
        if (row.gapPrevTime.QuadPart != 0 && row.gapCurTime.QuadPart != 0 && s_freq.QuadPart != 0)
        {
            const double tickPeriod = static_cast<double>(row.gapCurTime.QuadPart - row.gapPrevTime.QuadPart) / static_cast<double>(s_freq.QuadPart);
            if (tickPeriod > 0.001)
            {
                double elapsedSinceCur = static_cast<double>(now.QuadPart - row.gapCurTime.QuadPart) / static_cast<double>(s_freq.QuadPart);
                if (elapsedSinceCur < 0.0) elapsedSinceCur = 0.0;
                if (elapsedSinceCur > tickPeriod) elapsedSinceCur = tickPeriod;
                const double frac = elapsedSinceCur / tickPeriod;
                displayGap = row.gapSecondsPrev + (row.gapSeconds - row.gapSecondsPrev) * frac;
            }
        }
        // Your own row has no gap to show (it would only ever be the gap to
        // yourself) -- left blank. A sliding number is printed again only
        // when it moves into a different tenth.
        if (!(displayGap == displayGap)) displayGap = 0.0;
        if (displayGap > 999.9) displayGap = 999.9;     // F90: fits "+999.9"
        if (displayGap < -999.9) displayGap = -999.9;
        const int gapKind = (row.isPlayer || row.gapUnknown) ? 0 : (row.carInPits ? 1 : 2);
        if (gapKind != 2)
        {
            if (first || rt.keyGapKind != gapKind)
            {
                if (gapKind == 0) rt.gap[0] = '\0';
                else strcpy_s(rt.gap, "PIT");
            }
            rt.keyGapExact = false;
        }
        else
        {
            long long tenths = 0;
            bool negative = false;
            const bool exact = GapTextKey(displayGap, tenths, negative);
            if (first || rt.keyGapKind != 2 || !exact || !rt.keyGapExact
                || tenths != rt.keyGapTenths || negative != rt.keyGapNegative)
            {
                _snprintf_s(rt.gap, _TRUNCATE, "%+.1f", displayGap);
            }
            rt.keyGapExact = exact;
            rt.keyGapTenths = tenths;
            rt.keyGapNegative = negative;
        }
        rt.keyGapKind = gapKind;

        rt.isPlayer = row.isPlayer;
        rt.inPits = row.carInPits;
        // Compound, as looked up at scoring time (Row::Lookups).
        const RelativeTimingStore::Row::Lookups& found = row.lookups;
        rt.haveTyre = cfg.showTyre && found.haveTyre;
        if (rt.haveTyre)
        {
            strcpy_s(rt.tyre, found.tyre);
            rt.tyreR = found.tyreR; rt.tyreG = found.tyreG; rt.tyreB = found.tyreB;
            rt.tyreRearR = found.tyreRearR; rt.tyreRearG = found.tyreRearG; rt.tyreRearB = found.tyreRearB;
        }
        else
        {
            rt.tyre[0] = '\0';
            rt.tyreR = rt.tyreG = rt.tyreB = 1.0f;
            rt.tyreRearR = rt.tyreRearG = rt.tyreRearB = 1.0f;
        }
        // Laps since the car's last pit stop.
        if (first || rt.keyPitKnown != found.pitLapsKnown || (found.pitLapsKnown && rt.keyPitLaps != found.pitLaps))
        {
            rt.keyPitKnown = found.pitLapsKnown;
            rt.keyPitLaps = found.pitLaps;
            if (found.pitLapsKnown) _snprintf_s(rt.pit, _TRUNCATE, "L%d", found.pitLaps); else strcpy_s(rt.pit, "L?");
        }
        rt.pitKnown = found.pitLapsKnown;
        // Blue for any car not on your lap -- one you are lapping, or one
        // lapping you (the blue-flag case, where it matters most).
        // Blue specifically means "this car is a lap down" (classic blue-flag
        // semantics: a backmarker being lapped) -- NOT "this car is on any
        // different lap", which would also paint the leader blue on your own
        // screen when you're the one who fell a lap down. row.lapDown is
        // already the sign-correct "they are behind you" test; lapDelta's
        // sign is for the +/-NL display text only.
        rt.differentLap = row.lapDown;
        rt.lapAhead = row.lapDelta > 0;
        // Review F364: faded here on the render clock, so it still fades
        // out when scoring pauses or the table is frozen.
        rt.placeChangeSign = 0;
        rt.placeChangeAlpha = 0.0f;
        if (row.placeChangeSign != 0 && row.placeChangeTime.QuadPart != 0 && s_freq.QuadPart != 0)
        {
            double holdSeconds = cfg.positionChangeHoldSeconds;
            if (holdSeconds < 0.1) holdSeconds = 0.1;
            const double since = static_cast<double>(now.QuadPart - row.placeChangeTime.QuadPart) / static_cast<double>(s_freq.QuadPart);
            if (since >= 0.0 && since < holdSeconds)
            {
                rt.placeChangeSign = row.placeChangeSign;
                rt.placeChangeAlpha = static_cast<float>(1.0 - since / holdSeconds);
            }
        }

        // Places gained/lost last lap: green +N, red -N, blank when none.
        const bool showGain = row.haveLastLapGain && row.lastLapGain != 0;
        if (first || rt.keyShowGain != showGain || (showGain && rt.keyGain != row.lastLapGain))
        {
            rt.keyShowGain = showGain;
            rt.keyGain = row.lastLapGain;
            if (showGain) _snprintf_s(rt.lapGain, _TRUNCATE, "%+d", row.lastLapGain);
            else rt.lapGain[0] = '\0';
        }
        rt.lapGainR = cfg.colorR; rt.lapGainG = cfg.colorG; rt.lapGainB = cfg.colorB;
        if (showGain)
        {
            if (row.lastLapGain > 0) { rt.lapGainR = 0.25f; rt.lapGainG = 0.9f; rt.lapGainB = 0.3f; }
            else                     { rt.lapGainR = 1.0f;  rt.lapGainG = 0.25f; rt.lapGainB = 0.25f; }
        }
        // The player's own row is always exactly 0 with no trend -- color it
        // plain (the row's own text color) rather than running a trivial 0
        // through the gaining/losing rule, which would just paint it red.
        if (row.isPlayer)
        {
            rt.gapR = cfg.playerR; rt.gapG = cfg.playerG; rt.gapB = cfg.playerB;
        }
        else if (row.stopped || row.carInPits)
        {
            // No trend to show: the gap only moves because the player does.
            rt.gapR = cfg.colorR; rt.gapG = cfg.colorG; rt.gapB = cfg.colorB;
        }
        else
        {
            SectorColor(row.sectorVerdict, cfg, rt.gapR, rt.gapG, rt.gapB);
        }

        // Lap-time color vs the player's own last lap: red if theirs beat
        // yours, green if yours beat theirs, plain for the player's own row
        // (nothing to compare) or once either side lacks a completed lap.
        if (row.isPlayer)
        {
            rt.lapR = cfg.playerR; rt.lapG = cfg.playerG; rt.lapB = cfg.playerB;
        }
        else if (row.lapFasterThanPlayer)
        {
            rt.lapR = cfg.lapFasterR; rt.lapG = cfg.lapFasterG; rt.lapB = cfg.lapFasterB;
        }
        else if (row.lapSlowerThanPlayer)
        {
            rt.lapR = cfg.lapSlowerR; rt.lapG = cfg.lapSlowerG; rt.lapB = cfg.lapSlowerB;
        }
        else
        {
            rt.lapR = cfg.colorR; rt.lapG = cfg.colorG; rt.lapB = cfg.colorB;
        }
        // Session fastest lap outranks everything: purple, on any row.
        if (row.lastLapIsSessionFastest)
        {
            rt.lapR = kPurpleR; rt.lapG = kPurpleG; rt.lapB = kPurpleB;
        }
    }

    // Every *Px value in this config is a literal pixel count tuned against
    // a 3840x2160 reference -- unlike the radar/pedal-trace widgets, which
    // derive their internal sizes as fractions of their own already-scaled
    // panel, so they track resolution automatically. Scaling every pixel
    // value here by the same ratio keeps this table the same fraction of
    // the screen at any resolution instead of ballooning at lower ones.
    // cfg.scale is the layout-mode corner-handle multiplier. Folding it
    // in HERE rather than applying it to the drawn box means every pixel
    // value downstream -- text height, padding, gaps, corner radii --
    // scales with it in exact proportion, because they all already
    // derive from this one value.
    const float uiScale = static_cast<float>(screenHeight) / 2160.0f * (gapBox ? cfg.gapScale : cfg.scale);
    const float textHeightPx = cfg.textHeightPx * uiScale;
    const float rowGapPx = cfg.rowGapPx * uiScale;
    const float columnGapPx = cfg.columnGapPx * uiScale;
    const float bgPaddingPx = cfg.bgPaddingPx * uiScale;
    const float bgCornerRadiusPx = cfg.bgCornerRadiusPx * uiScale;

    // Header line: fractional lap counters (whole part = laps completed,
    // fraction = live progress through the lap in progress) so it's visibly
    // ticking up rather than jumping only at the line -- see
    // RelativeTimingStore::OnScoring for why. In a race, the total race
    // length and time/laps remaining are folded in here too, rather than
    // living in the fuel/tyre panel -- this header already carries the
    // session's own lap counters, so the race's overall length belongs
    // alongside them, not off in an unrelated panel.
    //
    // Standings only -- the gap box has no header or footer, so it skips
    // both. Kept between frames like the row text above: printed again only
    // when something it is printed from changes (5Hz at most).
    static char headerBuf[128] = "";
    float headerW = 0.0f;
    if (!gapBox)
    {
        const FuelStore::State fuel = g_fuelStore.Read();

        // Time (or laps) left in whatever session this is -- NOT race-only.
        // Practice and qualifying report their own time limit through the same
        // mEndET the race does (see FuelStore), so the clock is just as
        // available there and just as wanted; it used to be unreachable purely
        // because the whole branch below was gated on isRaceSession. No session
        // type is named: the number is what matters, and the driver knows which
        // session they are in. remainingKind: 0 none, 1 laps, 2 time.
        int remainingKind = 0, remaining = 0;
        if (fuel.isLapRace && fuel.lapsLeft >= 0.0)
        {
            remainingKind = 1;
            remaining = static_cast<int>(fuel.lapsLeft + 0.5);
        }
        else if (fuel.isTimedRace && fuel.raceMinutesLeft >= 0.0)
        {
            remainingKind = 2;
            remaining = static_cast<int>(fuel.raceMinutesLeft * 60.0 + 0.5);
        }
        // A race additionally knows its own total distance, so the lap
        // counter can read "lap X of Y" rather than just counting up.
        // totalKind: 0 none, 1 exact, 2 an estimate.
        int totalKind = 0, totalLapsInt = 0;
        if (fuel.isRaceSession && fuel.raceTotalLaps >= 0.0)
        {
            totalKind = fuel.raceTotalLapsIsEstimate ? 2 : 1;
            totalLapsInt = static_cast<int>(fuel.raceTotalLaps + 0.5);
        }

        static bool s_printed = false;
        static int s_remainingKind, s_remaining, s_totalKind, s_totalLaps;
        static double s_stint, s_total;
        if (!s_printed || remainingKind != s_remainingKind || remaining != s_remaining
            || totalKind != s_totalKind || totalLapsInt != s_totalLaps
            || !(state.stintLapsFractional == s_stint) || !(state.totalLapsFractional == s_total))
        {
            s_printed = true;
            s_remainingKind = remainingKind; s_remaining = remaining;
            s_totalKind = totalKind; s_totalLaps = totalLapsInt;
            s_stint = state.stintLapsFractional; s_total = state.totalLapsFractional;

            char remainingBuf[24] = "";
            if (remainingKind == 1)
                _snprintf_s(remainingBuf, _TRUNCATE, "%d laps left", remaining);
            else if (remainingKind == 2)
                _snprintf_s(remainingBuf, _TRUNCATE, "%d:%02d left", remaining / 60, remaining % 60);

            if (totalKind != 0)
            {
                char totalLapsBuf[16];
                if (totalKind == 2)
                    _snprintf_s(totalLapsBuf, _TRUNCATE, "~%d", totalLapsInt);
                else
                    _snprintf_s(totalLapsBuf, _TRUNCATE, "%d", totalLapsInt);

                if (remainingBuf[0])
                    _snprintf_s(headerBuf, _TRUNCATE, "Stint %.1f | Lap %.1f / %s | %s",
                              state.stintLapsFractional, state.totalLapsFractional, totalLapsBuf, remainingBuf);
                else
                    _snprintf_s(headerBuf, _TRUNCATE, "Stint %.1f | Lap %.1f / %s",
                              state.stintLapsFractional, state.totalLapsFractional, totalLapsBuf);
            }
            else if (remainingBuf[0])
            {
                _snprintf_s(headerBuf, _TRUNCATE, "Stint %.1f | Total %.1f | %s",
                          state.stintLapsFractional, state.totalLapsFractional, remainingBuf);
            }
            else
            {
                _snprintf_s(headerBuf, _TRUNCATE, "Stint %.1f | Total %.1f", state.stintLapsFractional, state.totalLapsFractional);
            }
        }
        headerW = renderer.MeasureText(headerBuf, textHeightPx);
    }

    // Column widths: measured from FIXED worst-case reference strings, not
    // from the actual rows this frame -- using live content made the whole
    // panel visibly resize frame to frame as e.g. a gap crossed from "-9.9"
    // to "-10.0" or a shorter name rotated into the window. Each reference
    // is at least as wide as anything the corresponding column can actually
    // print, so real content never overflows it.
    const float rowStep = textHeightPx + rowGapPx;
    const float placeW = renderer.MeasureText("P99", textHeightPx);
    const float nameW = renderer.MeasureText("WWWWWWWWWWWWWWWWWWWW", textHeightPx); // 14-char truncated name + " (you)"/" +99L" suffix
    const float lapW = renderer.MeasureText("99:59.9", textHeightPx);
    const float gapW = renderer.MeasureText("+999.9", textHeightPx);
    const float lapDiffW = renderer.MeasureText("+99", textHeightPx);

    const float colGap = columnGapPx;
    // "S/M" is the widest the compound column can print (a split strategy);
    // measured like every other column here rather than guessed, so the panel
    // does not resize as compounds change.
    const float tyreW = cfg.showTyre ? renderer.MeasureText("S/M", textHeightPx) : 0.0f;
    const float tyreAdvance = cfg.showTyre ? (tyreW + colGap) : 0.0f;
    const float gainW = renderer.MeasureText("+99", textHeightPx);
    // Laps since the car's last pit stop, "L99" at most wide.
    const bool showPitLaps = cfg.showPitLaps;
    const float pitW = showPitLaps ? renderer.MeasureText("L999", textHeightPx) : 0.0f;
    const float pitAdvance = showPitLaps ? (pitW + colGap) : 0.0f;
    const float numW = cfg.showCarNumber ? renderer.MeasureText("#999", textHeightPx) : 0.0f;
    const float numAdvance = cfg.showCarNumber ? (numW + colGap) : 0.0f;
    const float tableW = numAdvance + placeW + colGap + gainW + colGap + nameW + colGap + gapW + colGap + lapDiffW + colGap + lapW + tyreAdvance + pitAdvance;

    // Footer: the session's fastest lap, who set it, in purple, then your
    // own best lap this SESSION (not the all-time best) on the same line
    // after it. Standings only, and printed again only when one changes.
    static char fastestBuf[80] = "";
    static char pbBuf[40] = "";
    float fastestW = 0.0f, footerW = 0.0f;
    bool haveFooter = false;
    if (!gapBox)
    {
        static bool s_printed = false;
        static double s_fastest, s_pb;
        static char s_fastestName[32];
        if (!s_printed || !(state.fastestLapSeconds == s_fastest) || !(state.myBestLapSeconds == s_pb)
            || strcmp(state.fastestLapName, s_fastestName) != 0)
        {
            s_printed = true;
            s_fastest = state.fastestLapSeconds;
            s_pb = state.myBestLapSeconds;
            strncpy_s(s_fastestName, state.fastestLapName, _TRUNCATE);

            fastestBuf[0] = '\0';
            if (state.fastestLapSeconds > 0.0)
            {
                char t[16];
                FormatLapTime(state.fastestLapSeconds, t, sizeof(t));
                _snprintf_s(fastestBuf, _TRUNCATE, "Fastest lap  %s  %.24s", t, state.fastestLapName);
            }
            pbBuf[0] = '\0';
            const double pb = state.myBestLapSeconds;
            if (pb > 0.0)
            {
                char t[16];
                FormatLapTime(pb, t, sizeof(t));
                _snprintf_s(pbBuf, _TRUNCATE, "%sPB  %s", fastestBuf[0] ? "    " : "", t);
            }
        }
        fastestW = fastestBuf[0] ? renderer.MeasureText(fastestBuf, textHeightPx) : 0.0f;
        haveFooter = fastestBuf[0] != '\0' || pbBuf[0] != '\0';
        footerW = fastestW + (pbBuf[0] ? renderer.MeasureText(pbBuf, textHeightPx) : 0.0f);
    }

    const bool showHeader = !gapBox;
    const bool showFooter = haveFooter && !gapBox;
    const float contentW = gapBox ? tableW : (std::max)((std::max)(tableW, headerW), footerW);
    const float contentH = static_cast<float>(state.rowCount + (showHeader ? 1 : 0) + (showFooter ? 1 : 0)) * rowStep - rowGapPx;

    // (posXFraction, posYFraction) anchors the panel's own top-right outer
    // corner (padding included), matching the "top right = timing metrics"
    // layout convention.
    if (gapBox && (cfg.gapPosXFraction < 0.0f || cfg.gapPosYFraction < 0.0f))
    {
        // First draw: start just under the standings, right edges lined up.
        g_relativeTimingConfig.gapPosXFraction = cfg.posXFraction;
        const float under = s_lastBottomEdgeY >= 0.0f ? s_lastBottomEdgeY : screenHeight * (cfg.posYFraction + 0.25f);
        g_relativeTimingConfig.gapPosYFraction = (under + 12.0f * uiScale) / screenHeight;
    }
    const float panelRightX = screenWidth * (gapBox ? cfg.gapPosXFraction : cfg.posXFraction);
    const float panelTopY = screenHeight * (gapBox ? cfg.gapPosYFraction : cfg.posYFraction);
    const float halfW = contentW * 0.5f + bgPaddingPx;
    const float halfH = contentH * 0.5f + bgPaddingPx;
    const float centerX = panelRightX - halfW;
    const float centerY = panelTopY + halfH;

    if (gapBox)
        Layout::Register("Gap box", "RelativeTiming", "GapBoxPosXFraction", "GapBoxPosYFraction",
                         &g_relativeTimingConfig.gapPosXFraction, &g_relativeTimingConfig.gapPosYFraction,
                         centerX, centerY, halfW, halfH,
                         &g_relativeTimingConfig.gapScale, "GapBoxScale");
    else
    {
        s_lastBottomEdgeY = centerY + halfH;
        Layout::Register("Standings", "RelativeTiming", "PosXFraction", "PosYFraction",
                         &g_relativeTimingConfig.posXFraction, &g_relativeTimingConfig.posYFraction,
                         centerX, centerY, halfW, halfH,
                         &g_relativeTimingConfig.scale, "Scale");
    }

    if (cfg.bgEnabled)
    {
        renderer.AddFilledRoundedRect(centerX, centerY, halfW, halfH, bgCornerRadiusPx, 0.0f,
                                      cfg.bgR, cfg.bgG, cfg.bgB, cfg.bgA);
    }

    const float leftX = centerX - contentW * 0.5f;
    float y = centerY - contentH * 0.5f;

    if (showHeader)
    {
        renderer.AddText(leftX, y, headerBuf, textHeightPx, cfg.colorR, cfg.colorG, cfg.colorB, 1.0f, Renderer::HAlign::Left);
        y += rowStep;
    }

    for (int i = 0; i < state.rowCount; ++i)
    {
        // Place and name: the player's own row in its own colour, a car not
        // on your lap in blue, everything else plain.
        const RowText& rt = rows[i];
        float rowR = cfg.colorR, rowG = cfg.colorG, rowB = cfg.colorB;
        if (rt.isPlayer)          { rowR = cfg.playerR;  rowG = cfg.playerG;  rowB = cfg.playerB; }
        else if (rt.differentLap) { rowR = cfg.lapDownR; rowG = cfg.lapDownG; rowB = cfg.lapDownB; }
        else if (rt.lapAhead)     { rowR = cfg.lapUpR;   rowG = cfg.lapUpG;   rowB = cfg.lapUpB; }

        // Whole-row background highlight for a recently confirmed position
        // change -- faint green for a gain, faint red for a loss, fading out
        // as placeChangeAlpha decays toward 0 (see RelativeTimingStore).
        if (rt.placeChangeSign != 0)
        {
            const bool gained = rt.placeChangeSign < 0;
            const float hr = gained ? cfg.positionGainR : cfg.positionLossR;
            const float hg = gained ? cfg.positionGainG : cfg.positionLossG;
            const float hb = gained ? cfg.positionGainB : cfg.positionLossB;
            const float ha = (gained ? cfg.positionGainA : cfg.positionLossA) * rt.placeChangeAlpha;
            renderer.AddFilledRect(centerX, y + textHeightPx * 0.5f, contentW * 0.5f, textHeightPx * 0.5f, 0.0f,
                                   hr, hg, hb, ha);
        }

        // Your own row: a faint bar behind it (and its own text colour), so
        // it can be found at a glance. Drawn at normal weight, not bold.
        if (rt.isPlayer)
            renderer.AddFilledRect(centerX, y + textHeightPx * 0.5f, contentW * 0.5f, textHeightPx * 0.55f, 0.0f,
                                   cfg.playerR, cfg.playerG, cfg.playerB, 0.14f);
        auto rowText = [&](float tx, const char* text)
        {
            renderer.AddText(tx, y, text, textHeightPx, rowR, rowG, rowB, 1.0f, Renderer::HAlign::Left);
        };
        float x = leftX;
        rowText(x, rt.place);
        x += placeW + colGap;
        if (cfg.showCarNumber)
        {
            if (rt.carNumber[0])
                renderer.AddText(x, y, rt.carNumber, textHeightPx, rowR, rowG, rowB, 0.75f, Renderer::HAlign::Left);
            x += numW + colGap;
        }
        if (rt.lapGain[0])
            renderer.AddText(x + gainW, y, rt.lapGain, textHeightPx, rt.lapGainR, rt.lapGainG, rt.lapGainB, 1.0f, Renderer::HAlign::Right);
        x += gainW + colGap;
        rowText(x, rt.name);
        x += nameW + colGap;
        renderer.AddText(x + gapW, y, rt.gap, textHeightPx, rt.gapR, rt.gapG, rt.gapB, 1.0f, Renderer::HAlign::Right);
        x += gapW + colGap;
        if (rt.lapDiff[0])
            renderer.AddText(x + lapDiffW, y, rt.lapDiff, textHeightPx, rowR, rowG, rowB, 1.0f, Renderer::HAlign::Right);
        x += lapDiffW + colGap;
        renderer.AddText(x + lapW, y, rt.lapTime, textHeightPx, rt.lapR, rt.lapG, rt.lapB, 1.0f, Renderer::HAlign::Right);
        if (cfg.showTyre)
        {
            x += lapW + colGap;
            if (rt.haveTyre && rt.tyre[1] == '/' && rt.tyre[2])
            {
                // Split set: front letter, "/", rear letter, each in its
                // own compound colour (F1 colours: S red, M yellow, H white,
                // I green, W blue).
                const char front[2] = { rt.tyre[0], 0 };
                const char rear[2] = { rt.tyre[2], 0 };
                float tx = x;
                renderer.AddText(tx, y, front, textHeightPx, rt.tyreR, rt.tyreG, rt.tyreB, 1.0f, Renderer::HAlign::Left);
                tx += renderer.MeasureText(front, textHeightPx);
                renderer.AddText(tx, y, "/", textHeightPx, cfg.colorR, cfg.colorG, cfg.colorB, 1.0f, Renderer::HAlign::Left);
                tx += renderer.MeasureText("/", textHeightPx);
                renderer.AddText(tx, y, rear, textHeightPx, rt.tyreRearR, rt.tyreRearG, rt.tyreRearB, 1.0f, Renderer::HAlign::Left);
            }
            else if (rt.haveTyre)
            {
                renderer.AddText(x, y, rt.tyre, textHeightPx,
                                 rt.tyreR, rt.tyreG, rt.tyreB, 1.0f, Renderer::HAlign::Left);
            }
            else
            {
                // No telemetry for this car. With AllVehicleTelemetry off
                // that is every car but your own, which is the honest thing
                // to show rather than repeating your own compound.
                renderer.AddText(x, y, "-", textHeightPx,
                                 rowR, rowG, rowB, 0.5f, Renderer::HAlign::Left);
            }
        }
        if (showPitLaps)
        {
            // x is at the last drawn column's left edge: step past it.
            x += cfg.showTyre ? (tyreW + colGap) : (lapW + colGap);
            // Greyed while the car is in the pits, or when not known.
            const float a = (rt.inPits || !rt.pitKnown) ? 0.45f : 1.0f;
            renderer.AddText(x + pitW, y, rt.pit, textHeightPx, rowR, rowG, rowB, a, Renderer::HAlign::Right);
        }

        y += rowStep;
    }

    if (showFooter)
    {
        if (fastestBuf[0])
            renderer.AddText(leftX, y, fastestBuf, textHeightPx, kPurpleR, kPurpleG, kPurpleB, 1.0f, Renderer::HAlign::Left);
        if (pbBuf[0])
            renderer.AddText(leftX + fastestW, y, pbBuf, textHeightPx, cfg.colorR, cfg.colorG, cfg.colorB, 1.0f,
                             Renderer::HAlign::Left);
    }

    return true;
}
