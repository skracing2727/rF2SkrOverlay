#include "MiniSector.h"
#include "InternalsPlugin.hpp"
#include "LapDelta.h"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "Log.h"
#include "RenderThrottle.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

MiniSectorStore g_miniSector;

namespace
{
    // How far back the segment index must jump before it counts as a real
    // backward move rather than resync jitter. A spin or a pit reset moves
    // you a large fraction of the lap; a resync correction is a metre or two,
    // i.e. never more than one segment.
    const int kBackwardResetSegments = 4;
}

void MiniSectorStore::Init()
{
    if (m_ready)
        return;
    InitializeCriticalSection(&m_lock);
    QueryPerformanceFrequency(&m_freq);
    m_ready = true;
}

int MiniSectorStore::SegmentCount() const
{
    int n = g_miniSectorConfig.segmentCount;
    if (n < 8) n = 8;
    if (n > kMaxSegments) n = kMaxSegments;
    return n;
}

void MiniSectorStore::SnapshotReference()
{
    // Taken once per lap rather than per frame: LapDelta's trace is built and
    // owned by the scoring thread, so it is only safe to read from here, and
    // one pass over it per lap is far cheaper than interpolating on every
    // segment crossing anyway.
    m_hasReference = false;
    if (m_trackLength <= 1.0)
        return;

    const int n = SegmentCount();
    const bool useSession = g_miniSectorConfig.useSessionReference;

    for (int i = 0; i <= n; ++i)
    {
        const double dist = (m_trackLength * i) / n;
        double t = 0.0;
        if (!g_lapDelta.GetReferenceTimeAt(dist, useSession, t))
            return; // no reference lap yet -- leave m_hasReference false
        m_refBoundaryTime[i] = t;
    }

    // A trace that does not actually advance in time is unusable (and would
    // make every segment look identically "faster"), so treat it as absent.
    if (!(m_refBoundaryTime[n] > m_refBoundaryTime[0]))
        return;

    m_refSegmentCount = n;
    m_hasReference = true;

    // Diagnostic: the per-segment coloring has been reported as noisy
    // (alternating red/green rather than coherent stretches) -- log the
    // shape of the just-snapshotted reference so a real repro can be
    // checked against Log.txt instead of guessing further. Safe to leave in
    // permanently: fires once per lap at most, not per frame.
    {
        int badSegments = 0;
        double minRefTime = 1e9, maxRefTime = -1e9;
        for (int i = 0; i < n; ++i)
        {
            const double rt = m_refBoundaryTime[i + 1] - m_refBoundaryTime[i];
            if (rt <= 0.0) { ++badSegments; continue; }
            if (rt < minRefTime) minRefTime = rt;
            if (rt > maxRefTime) maxRefTime = rt;
        }
        Log::Write("MiniSector: reference snapshot -- lapTime=%.3fs, segments=%d, badSegments=%d, "
                   "perSegmentTime min=%.4fs max=%.4fs (segmentDist=%.2fm)",
                   m_refBoundaryTime[n] - m_refBoundaryTime[0], n, badSegments,
                   minRefTime, maxRefTime, m_trackLength / n);
    }
}

void MiniSectorStore::PublishState()
{
    if (!m_ready)
        return;
    EnterCriticalSection(&m_lock);
    m_state.current = m_currentRow;
    m_state.previous = m_previousRow;
    m_state.hasReference = m_hasReference;
    m_state.referenceLapTimeSeconds = (m_hasReference && m_refSegmentCount > 0)
                                     ? m_refBoundaryTime[m_refSegmentCount] : -1.0;
    m_state.hasLastSegment = m_hasLastSegment;
    m_state.lastSegmentIndex = m_lastSegmentIndex;
    m_state.lastSegmentDeltaKmh = m_lastSegmentDeltaKmh;
    m_state.lastYourSpeedKmh = m_lastYourSpeedKmh;
    m_state.lastRefSpeedKmh = m_lastRefSpeedKmh;
    LeaveCriticalSection(&m_lock);
}

void MiniSectorStore::FinishLap()
{
    // Only promote a lap that was actually driven end to end. A lap only
    // partly seen -- joined mid-track, interrupted by a trip to the monitor,
    // or in progress when the plugin started -- would otherwise replace a
    // complete strip with a mostly-empty one, which reads as "you were
    // slower here" when really there is just no data.
    int filled = 0;
    for (int i = 0; i < m_currentRow.segmentCount && i < kMaxSegments; ++i)
        if (m_currentRow.filled[i]) ++filled;

    const int needed = static_cast<int>(m_currentRow.segmentCount * 0.9);
    if (m_currentRow.valid && filled >= needed)
        m_previousRow = m_currentRow;

    m_currentRow = Row{};
    m_currentRow.segmentCount = SegmentCount();
    m_currentSegment = -1;
}

void MiniSectorStore::OnScoring(const ScoringInfoV01& info)
{
    if (!m_ready || info.mVehicle == nullptr)
        return;

    const VehicleScoringInfoV01* player = nullptr;
    for (long i = 0; i < info.mNumVehicles; ++i)
    {
        if (info.mVehicle[i].mIsPlayer) { player = &info.mVehicle[i]; break; }
    }
    if (!player)
        return;

    m_trackLength = info.mLapDist; // session-level: total track length

    // Resync the extrapolated distance to the authoritative value. Segment
    // TIMING is untouched by this: it accumulates tick by tick (see
    // m_segmentAccumTime), so a correction here only adjusts where we think
    // we are, never how much time the current segment has banked.
    m_lastKnownDist = player->mLapDist;
    QueryPerformanceCounter(&m_lastSyncTime);

    // Re-snapshot the reference once per lap: a new best lap replaces
    // LapDelta's trace, and this is where that gets picked up.
    if (m_lastScoringLaps < 0 || player->mTotalLaps != m_lastScoringLaps || !m_hasReference)
    {
        m_lastScoringLaps = player->mTotalLaps;
        SnapshotReference();
    }
}

void MiniSectorStore::OnTelemetry(const TelemInfoV01& info)
{
    if (!m_ready || !g_miniSectorConfig.enabled)
        return;
    if (m_trackLength <= 1.0)
        return; // no scoring data yet

    // SPEED, not signed forward velocity. mLocalVel.z's sign convention is a
    // trap here: rF2's local Z points backward (which is exactly why
    // Radar.cpp has to negate it -- see 'forward = -relZ' there), so using it
    // raw made the extrapolated distance run BACKWARDS between scoring
    // resyncs. Every backward boundary crossing then hit the re-anchor path
    // below and threw away the segment's banked time, leaving a fraction of
    // the real duration and so a wildly inflated speed. The magnitude is what
    // this actually wants -- you are travelling forwards around the lap --
    // and it matches how GearDisplay, PedalTrace and
    // RelativeTiming all read speed. Reverse is not modelled; the ~5Hz resync
    // corrects it within a tick anyway.
    m_forwardSpeed = sqrt(info.mLocalVel.x * info.mLocalVel.x +
                          info.mLocalVel.y * info.mLocalVel.y +
                          info.mLocalVel.z * info.mLocalVel.z);

    // How much sim time this tick represents, for the segment accumulator.
    // Clamped the same way the extrapolation below is: a pause, a load, or a
    // session change must not dump seconds into one segment.
    double tickDt = 0.0;
    if (m_lastTelemET >= 0.0)
    {
        tickDt = info.mElapsedTime - m_lastTelemET;
        if (tickDt < 0.0) tickDt = 0.0;
        if (tickDt > 0.5) tickDt = 0.5;
    }
    m_lastTelemET = info.mElapsedTime;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double dt = 0.0;
    if (m_lastSyncTime.QuadPart != 0 && m_freq.QuadPart != 0)
    {
        dt = static_cast<double>(now.QuadPart - m_lastSyncTime.QuadPart) / static_cast<double>(m_freq.QuadPart);
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.5) dt = 0.5; // stalled scoring (pause/load) -- don't run away
    }

    double dist = m_lastKnownDist + m_forwardSpeed * dt;
    while (dist >= m_trackLength) dist -= m_trackLength;
    while (dist < 0.0) dist += m_trackLength;

    const int n = SegmentCount();

    if (m_currentRow.segmentCount != n)
    {
        // Segment count changed under us (config reload) -- start clean
        // rather than mixing two different griddings in one strip.
        m_currentRow = Row{};
        m_currentRow.segmentCount = n;
        m_previousRow = Row{};
        m_currentSegment = -1;
    }

    // Lap boundary, straight from telemetry (higher rate than scoring, so the
    // strip flips over promptly rather than up to 200ms late).
    if (m_lastLapNumber < 0)
    {
        m_lastLapNumber = info.mLapNumber;
        m_currentRow = Row{};
        m_currentRow.segmentCount = n;
        m_currentSegment = -1;
    }
    else if (info.mLapNumber != m_lastLapNumber)
    {
        m_lastLapNumber = info.mLapNumber;
        FinishLap();
        PublishState();
    }

    int seg = static_cast<int>((dist / m_trackLength) * n);
    if (seg < 0) seg = 0;
    if (seg >= n) seg = n - 1;

    // Braking flag: checked on every sample rather than only at a segment
    // crossing, since a whole braking application typically happens well
    // inside one segment. Published immediately on the FIRST sample that
    // flags a segment (not on every braking sample, which would publish at
    // physics rate) rather than waiting for the next segment-timing publish
    // below, which sits on the far side of an early return -- the line
    // should extend the instant you brake, not once you cross into the next
    // mini-sector.
    // Both pedals are tested against their own low thresholds rather than
    // one being inferred from the other -- see Row::accelerating.
    const MiniSectorConfig& msCfg = g_miniSectorConfig;
    const bool braking = info.mUnfilteredBrake > msCfg.accelLineBrakeThreshold;
    const bool accelerating = info.mUnfilteredThrottle > msCfg.accelLineThrottleThreshold;
    if ((braking && !m_currentRow.braking[seg]) || (accelerating && !m_currentRow.accelerating[seg]))
    {
        if (braking) m_currentRow.braking[seg] = true;
        if (accelerating) m_currentRow.accelerating[seg] = true;
        PublishState();
    }

    if (m_currentSegment < 0)
    {
        // First sample of the lap: just anchor, so the segment we happen to
        // be part-way through isn't timed as if it had been entered at its
        // boundary.
        m_currentSegment = seg;
        m_segmentAccumTime = 0.0;
        return;
    }

    // Bank this tick's time against the segment we are in. Done before the
    // crossing test, so the tick that carries us over a boundary counts
    // toward the segment being left -- at physics rate that attributes a
    // couple of milliseconds to the wrong side of the line, far below the
    // resolution of anything drawn from it.
    m_segmentAccumTime += tickDt;

    if (seg == m_currentSegment)
        return; // still inside the same mini-sector

    if (seg < m_currentSegment)
    {
        // A SMALL backward step is not a real backward move: it is the ~5Hz
        // resync correcting the extrapolated distance by a metre or two, and
        // if that happens to land across a boundary the segment index dips.
        // Treating that as a reset (which this used to do unconditionally)
        // wiped the banked time again and again mid-segment, which is the
        // other half of what made the strip read absurdly fast. Ignore it and
        // keep banking; the index recovers on the next tick.
        const int backward = m_currentSegment - seg;
        if (backward < kBackwardResetSegments)
            return;

        // A genuine backward move (spin, reset to pits, teleport) -- the
        // banked time no longer belongs to any segment driven cleanly.
        m_currentSegment = seg;
        m_segmentAccumTime = 0.0;
        return;
    }

    const double elapsed = m_segmentAccumTime;
    const int crossed = seg - m_currentSegment;
    if (elapsed > 0.0 && elapsed < 60.0 && m_hasReference && m_refSegmentCount == n)
    {
        // Normally exactly one segment per crossing; if several were covered
        // between samples (a high-speed straight with a small segment size
        // and a whole telemetry tick's gap), split the elapsed time across
        // them evenly -- they are equal-length, so this is exactly the same
        // "assume constant speed across the gap" approximation as the single-
        // segment case, just applied more than once.
        const double segmentDistance = m_trackLength / n;
        const double perSegment = elapsed / crossed;
        const double yourAvgSpeed = (perSegment > 0.0) ? (segmentDistance / perSegment) : 0.0;

        // Diagnostic for the inflated-speed bug: every anomalous crossing
        // (more than one segment at a time, or an implausible speed) plus a
        // periodic healthy sample for comparison, so a single lap's log shows
        // both whether the fix holds and, if it doesn't, which of 'elapsed'
        // and 'crossed' is wrong -- the two are indistinguishable from the
        // on-screen readout alone.
        const double yourKmh = yourAvgSpeed * 3.6;
        if (crossed != 1 || yourKmh > 350.0 || (m_currentSegment % 25) == 0)
        {
            Log::Write("MiniSector: seg %d->%d crossed=%d elapsed=%.4fs perSeg=%.4fs "
                       "you=%.0fkph | dist=%.1f lastKnownDist=%.1f speed=%.2fm/s tickDt=%.4fs",
                       m_currentSegment, seg, crossed, elapsed, perSegment, yourKmh,
                       dist, m_lastKnownDist, m_forwardSpeed, tickDt);
        }

        // Wall-clock "now" -- each segment's fade-in start, independent of
        // sim-time elapsed/perSegment above (which drive the speed math).
        // Setting every segment's own start to the same instant they were
        // computed means a batch of several (a fast straight crossing more
        // than one small segment in a single tick) each ease in from their
        // own moment rather than all popping in together -- see DrawRow.
        LARGE_INTEGER nowQpc;
        QueryPerformanceCounter(&nowQpc);
        const double nowSeconds = (m_freq.QuadPart > 0)
                                 ? static_cast<double>(nowQpc.QuadPart) / static_cast<double>(m_freq.QuadPart)
                                 : 0.0;

        for (int k = 0; k < crossed; ++k)
        {
            const int idx = m_currentSegment + k;
            if (idx < 0 || idx >= n) continue;
            const double refTime = m_refBoundaryTime[idx + 1] - m_refBoundaryTime[idx];
            if (refTime <= 0.0) continue; // malformed reference sample -- skip rather than divide by it

            const double refAvgSpeed = segmentDistance / refTime;
            // Reference minus yours: +ve = you were slower there (a deficit).
            m_currentRow.speedDeficitKmh[idx] = static_cast<float>((refAvgSpeed - yourAvgSpeed) * 3.6);
            m_currentRow.filled[idx] = true;
            m_currentRow.valid = true;
            m_currentRow.revealAtSeconds[idx] = nowSeconds;

            m_hasLastSegment = true;
            m_lastSegmentIndex = idx;
            m_lastSegmentDeltaKmh = m_currentRow.speedDeficitKmh[idx];
            m_lastYourSpeedKmh = static_cast<float>(yourAvgSpeed * 3.6);
            m_lastRefSpeedKmh = static_cast<float>(refAvgSpeed * 3.6);
        }
        PublishState();
    }

    m_currentSegment = seg;
    m_segmentAccumTime = 0.0;
}

void MiniSectorStore::InvalidateReference()
{
    // Only the reference is dropped, not the rows: the lap you are driving is
    // still yours, it just has nothing to be compared against until a new
    // reference exists. m_lastScoringLaps is reset too so OnScoring's
    // once-per-lap guard re-snapshots on the very next tick rather than
    // waiting for the next lap boundary.
    m_hasReference = false;
    m_refSegmentCount = 0;
    m_lastScoringLaps = -1;
    m_currentRow = Row{};
    m_previousRow = Row{};
    m_currentRow.segmentCount = SegmentCount();
    m_hasLastSegment = false;
    PublishState();
}

void MiniSectorStore::Clear()
{
    m_trackLength = 0.0;
    m_lastKnownDist = 0.0;
    m_forwardSpeed = 0.0;
    m_lastSyncTime.QuadPart = 0;

    m_hasReference = false;
    m_refSegmentCount = 0;

    m_lastLapNumber = -1;
    m_lastScoringLaps = -1;
    m_currentSegment = -1;
    m_segmentAccumTime = 0.0;
    m_lastTelemET = -1.0;
    m_hasLastSegment = false;
    m_lastSegmentIndex = -1;

    m_currentRow = Row{};
    m_previousRow = Row{};

    if (!m_ready)
        return;
    EnterCriticalSection(&m_lock);
    m_state = State{};
    LeaveCriticalSection(&m_lock);
}

MiniSectorStore::State MiniSectorStore::Read()
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
    // Plain two-state colour, no gradient and no scale: red where the
    // reference carried more speed through this segment (you were slower),
    // green where you did. deficitKmh is (reference speed - your speed).
    void SegmentColor(float deficitKmh, const MiniSectorConfig& cfg, float& r, float& g, float& b)
    {
        if (deficitKmh > 0.0f) { r = cfg.slowerR; g = cfg.slowerG; b = cfg.slowerB; }
        else                   { r = cfg.fasterR; g = cfg.fasterG; b = cfg.fasterB; }
    }

    // Draws one lap's strip, merging neighbouring segments that resolve to the
    // same colour into a single rect -- a 200-segment strip is otherwise 200
    // quads per lap per frame, and long stretches of a lap sit at the same
    // shade anyway.
    // 0 for a segment not yet filled, ramping linearly to 1 over
    // fadeDurationSeconds after the moment it was filled (revealAtSeconds),
    // then holding at 1. Individual per segment, so a whole batch filled in
    // one tick (a fast straight crossing several small segments at once)
    // still each ease in from the same instant rather than popping in
    // together -- and steady-state segments (the overwhelming majority at
    // any moment) all read exactly 1.0 and merge into runs exactly as
    // before, so this costs nothing once the strip has settled.
    inline float FadeAlpha(const MiniSectorStore::Row& row, int i, double nowSeconds, double fadeDurationSeconds)
    {
        if (!row.filled[i]) return 0.0f;
        if (fadeDurationSeconds <= 0.0) return 1.0f;
        const double age = nowSeconds - row.revealAtSeconds[i];
        if (age <= 0.0) return 0.0f;
        if (age >= fadeDurationSeconds) return 1.0f;
        return static_cast<float>(age / fadeDurationSeconds);
    }

    void DrawRow(Renderer& renderer, const MiniSectorStore::Row& row, const MiniSectorConfig& cfg,
                 float leftX, float topY, float width, float height,
                 double nowSeconds, double fadeDurationSeconds)
    {
        if (row.segmentCount <= 0)
            return;

        const float segW = width / static_cast<float>(row.segmentCount);
        const float centerY = topY + height * 0.5f;
        const float halfH = height * 0.5f;

        int i = 0;
        while (i < row.segmentCount)
        {
            const float fade = FadeAlpha(row, i, nowSeconds, fadeDurationSeconds);
            if (fade <= 0.0f) { ++i; continue; }

            float r, g, b;
            SegmentColor(row.speedDeficitKmh[i], cfg, r, g, b);

            // Extend while the next segment is filled, lands on visually the
            // same colour, AND is at the same point in its own fade -- a
            // segment still easing in must not be merged into a fully-faded
            // neighbour's rect, or it would jump straight to full alpha.
            int j = i + 1;
            while (j < row.segmentCount)
            {
                const float nextFade = FadeAlpha(row, j, nowSeconds, fadeDurationSeconds);
                if (fabsf(nextFade - fade) > 0.02f) break;
                float nr, ng, nb;
                SegmentColor(row.speedDeficitKmh[j], cfg, nr, ng, nb);
                if (fabsf(nr - r) > 0.02f || fabsf(ng - g) > 0.02f || fabsf(nb - b) > 0.02f)
                    break;
                ++j;
            }

            const float runLeft = leftX + segW * i;
            const float runWidth = segW * (j - i);

            if (fade >= 1.0f)
            {
                // Settled -- draw the full run at full alpha, same as
                // before there was any fade at all.
                renderer.AddFilledRect(runLeft + runWidth * 0.5f, centerY, runWidth * 0.5f, halfH, 0.0f,
                                       r, g, b, cfg.alpha);
            }
            else
            {
                // Still easing in: a WIPE from the run's own left edge
                // rather than a uniform alpha fade over the whole width --
                // the left side of a sliver (or a same-tick batch of them)
                // visibly fills in before the right side, rather than the
                // whole thing appearing at once, dimly, everywhere.
                const float revealWidth = runWidth * fade;
                if (revealWidth > 0.01f)
                {
                    renderer.AddFilledRect(runLeft + revealWidth * 0.5f, centerY, revealWidth * 0.5f, halfH, 0.0f,
                                           r, g, b, cfg.alpha);
                }
            }
            i = j;
        }
    }

    // A continuous line along this strip's own bottom edge: green where the
    // segment was driven under acceleration, red where the brake was used.
    // Neighbouring segments of the same state merge into one rect, so a lap
    // is a handful of quads rather than 200. Only DRIVEN segments are drawn,
    // so the line fills in with the strip above it rather than implying the
    // whole lap was on the throttle before you got there.
    void DrawAccelLine(Renderer& renderer, const MiniSectorStore::Row& row, const MiniSectorConfig& cfg,
                       float leftX, float bottomY, float width, float lineHeightPx,
                       double nowSeconds, double fadeDurationSeconds)
    {
        if (!cfg.accelLineEnabled || row.segmentCount <= 0 || lineHeightPx <= 0.0f)
            return;

        const float segW = width / static_cast<float>(row.segmentCount);
        const float centerY = bottomY + lineHeightPx * 0.5f;
        const float halfH = lineHeightPx * 0.5f;

        int i = 0;
        while (i < row.segmentCount)
        {
            const float fade = FadeAlpha(row, i, nowSeconds, fadeDurationSeconds);
            if (fade <= 0.0f) { ++i; continue; }

            const bool braking = row.braking[i];
            const bool accelerating = row.accelerating[i];
            int j = i + 1;
            while (j < row.segmentCount)
            {
                const float nextFade = FadeAlpha(row, j, nowSeconds, fadeDurationSeconds);
                if (fabsf(nextFade - fade) > 0.02f) break;
                if (row.braking[j] != braking || row.accelerating[j] != accelerating) break;
                ++j;
            }

            // Neither pedal: draw nothing. The gap IS the information --
            // it marks the coasting/turn-in phase between the two.
            // Brake wins a segment where both were used, since trail-braking
            // into a corner is the brake's story, not the throttle's.
            if (braking || accelerating)
            {
                const float runLeft = leftX + segW * i;
                const float runWidth = segW * (j - i);
                renderer.AddFilledRect(runLeft + runWidth * 0.5f, centerY, runWidth * 0.5f, halfH, 0.0f,
                                       braking ? cfg.accelLineBrakeR : cfg.accelLineAccelR,
                                       braking ? cfg.accelLineBrakeG : cfg.accelLineAccelG,
                                       braking ? cfg.accelLineBrakeB : cfg.accelLineAccelB,
                                       cfg.accelLineAlpha * fade);
            }
            i = j;
        }
    }
}

bool MiniSector::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    // Medium tier (~25Hz): the delta-speed strip updates per distance-sample,
    // not per frame, so skip the rebuild on frames not due and replay the
    // last one. Bypassed in layout mode so dragging/resizing stays smooth.
    static RenderThrottle s_throttle(0.04);
    if (!Layout::Active() && s_throttle.TryReplay(renderer))
        return true;
    RenderThrottle::Capture throttleCapture(s_throttle, renderer);

    // This widget paints its own background panel, so the renderer's
    // per-string text plate would double-darken over it -- LapDelta.cpp
    // already does this for the same reason; this one never had it, which is
    // why its "no session best set yet" placeholder showed a second, darker
    // box behind the text that the delta bar's own placeholder never had.
    struct TextBgGuard
    {
        Renderer& r; bool prev;
        explicit TextBgGuard(Renderer& rr) : r(rr), prev(rr.TextBackgroundEnabled()) { r.SetTextBackground(false); }
        ~TextBgGuard() { r.SetTextBackground(prev); }
    } textBgGuard(renderer);

    const MiniSectorConfig& cfg = g_miniSectorConfig;
    if (!cfg.enabled)
        return false;

    const MiniSectorStore::State state = g_miniSector.Read();

    // Slaved to the delta bar's own scale and X position -- the
    // two bars are meant to be identical in size and position, not just
    // width-matched with independent handles. Both bars now move and resize
    // as one: dragging/resizing the delta bar (see LapDelta.cpp) carries
    // this strip with it, and this strip has no handle of its own any more
    // (see the Layout::Register removal below). g_miniSectorConfig's own
    // scale/centerXFraction/centerYFraction/widthFraction/matchDeltaBarWidth
    // fields are no longer read here; kept in Config.h only so an existing
    // INI with those keys still loads without error.
    const float uiScale = static_cast<float>(screenHeight) / 2160.0f * g_lapDeltaConfig.scale;
    const float gapAbovePx = cfg.gapAboveDeltaBarPx * uiScale;
    const float bgPaddingPx = cfg.bgPaddingPx * uiScale;
    const float bgCornerRadiusPx = cfg.bgCornerRadiusPx * uiScale;

    const float width = screenWidth * g_lapDeltaConfig.barWidthFraction * g_lapDeltaConfig.scale;
    const float centerX = screenWidth * g_lapDeltaConfig.centerXFraction;
    const float leftX = centerX - width * 0.5f;

    // Height is a fixed proportion of THIS panel's own resolved width, which
    // is now always exactly the delta bar's own width -- so the two bars'
    // heights match too. See LapDelta.cpp's BarAspectRatio() for the shared
    // constant both panels compute this from.
    const float barHeightPx = width * LapDelta::BarAspectRatio();

    // Always stacked directly above the delta bar's own top edge, by a fixed
    // gap -- no independent Y any more, so the two bars cannot drift apart.
    const float deltaBarCenterY = screenHeight * g_lapDeltaConfig.centerYFraction;
    const float deltaBarHalfH = g_lapDeltaConfig.barHeightPx * uiScale * 0.5f;
    const float stackBottomY = deltaBarCenterY - deltaBarHalfH - gapAbovePx;

    // ONE strip: the lap being driven right now. The previous lap's strip was
    // dropped -- stacking two doubled the widget's height for a comparison
    // that was rarely made, and the reference lap is already what every
    // segment is coloured against.
    const float currentTopY = stackBottomY - barHeightPx;

    if (cfg.bgEnabled)
    {
        // Both width AND height now match the delta bar's own box exactly --
        // no added padding on any side. The delta bar itself is drawn at
        // exactly barHalfW/barHalfH with none (see LapDelta.cpp); padding
        // this one made it visibly WIDER and TALLER even though the strip
        // inside it was already the same size as the bar. The accel/brake
        // line sits just below this box now rather than inside it -- it was
        // never fully covered by the old padding either once its own height
        // exceeded that padding.
        renderer.AddFilledRoundedRect(centerX, (currentTopY + stackBottomY) * 0.5f,
                                      width * 0.5f, (stackBottomY - currentTopY) * 0.5f,
                                      bgCornerRadiusPx, 0.0f,
                                      cfg.bgR, cfg.bgG, cfg.bgB, cfg.bgA);
    }

    // Same placeholder as the SB delta row, and for the same reason: this
    // strip is driven by the same session reference, so it is blank for
    // exactly as long, and a blank strip explains nothing. Same TEXT SIZE
    // too now -- this used to be sized off barHeightPx*0.62 (its own strip's
    // height), which does not equal the SB row's own textHeightPx*uiScale*0.7
    // even though the two bars are now the same size, so the two placeholder
    // messages read as different sizes. Both now come from the same
    // g_lapDeltaConfig::textHeightPx, so they match exactly.
    if (!state.hasReference)
    {
        const float msgPx = g_lapDeltaConfig.textHeightPx * uiScale * 0.7f;
        renderer.AddText(centerX, currentTopY + (barHeightPx - msgPx) * 0.5f,
                         "no session best set yet", msgPx,
                         0.7f, 0.7f, 0.7f, 1.0f, Renderer::HAlign::Center);
        return true;
    }
    if (!state.current.valid)
        return true;

    LARGE_INTEGER nowQpc, freq;
    QueryPerformanceCounter(&nowQpc);
    QueryPerformanceFrequency(&freq);
    const double nowSeconds = (freq.QuadPart > 0)
                             ? static_cast<double>(nowQpc.QuadPart) / static_cast<double>(freq.QuadPart)
                             : 0.0;

    // Each segment fades in over roughly how long it actually takes to
    // drive through: the reference lap's own time divided evenly across its
    // segments. Falls back to 60s/segmentCount (the default 200 segments
    // gives 0.3s) when there is no reference lap time to divide by yet.
    const int segCountForFade = (state.current.segmentCount > 0) ? state.current.segmentCount : 200;
    double fadeDurationSeconds = 60.0 / static_cast<double>(segCountForFade);
    if (state.hasReference && state.referenceLapTimeSeconds > 0.0)
        fadeDurationSeconds = state.referenceLapTimeSeconds / static_cast<double>(segCountForFade);

    DrawRow(renderer, state.current, cfg, leftX, currentTopY, width, barHeightPx, nowSeconds, fadeDurationSeconds);

    // The accel/brake line sits in the gap below the strip's own bottom edge.
    const float accelLineHeightPx = cfg.accelLineHeightPx * uiScale;
    DrawAccelLine(renderer, state.current, cfg, leftX, currentTopY + barHeightPx, width, accelLineHeightPx, nowSeconds, fadeDurationSeconds);

    return true;
}
