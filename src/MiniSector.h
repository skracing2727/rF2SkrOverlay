#pragma once
#include <atomic>
#include <Windows.h>

// Two horizontal strips sitting above the lap-delta bar, each representing one
// whole lap left-to-right: the previous completed lap on top, the lap being
// driven right now below it, filling in as you go.
//
// The lap is chopped into a fixed NUMBER of equal mini-sectors (so the strip
// always spans exactly one lap whatever the track length). Each mini-sector
// is coloured by AVERAGE SPEED through it versus the same reference lap
// LapDelta compares to -- carrying more speed than the reference is green,
// less is red. Same distance, so more average speed = less time there. This is a different question from the delta bar's running
// total: it shows WHERE you were quicker or slower through the corner itself,
// not where lap time accumulated (the two can disagree -- braking a little
// deep can show a slow mini-sector even on a lap that nets a faster delta,
// if it sets up a stronger exit the reference didn't have).
//
// Binned by DISTANCE, not time, deliberately: each pixel column is then the
// same stretch of track no matter how fast it was taken. Binning by time would
// smear slow corners across half the strip and squash the straights to
// nothing.
//
// Two colours only: green where you carried more speed than the reference
// through that segment, red where you carried less. (It used to be a
// red-to-green gradient on a dynamic km/h scale with a +/- label beside the
// strip; that was harder to read than a plain faster/slower.)
class MiniSectorStore
{
public:
    void Init();
    // ~5Hz: authoritative lap distance (resync), track length, and the
    // once-per-lap snapshot of the reference lap's segment times.
    void OnScoring(const struct ScoringInfoV01& info);
    // Physics rate: distance extrapolation and the actual segment timing.
    // Lap distance is not in telemetry, so it is extrapolated from speed
    // between scoring resyncs.
    void OnTelemetry(const struct TelemInfoV01& info);
    void Clear();

    // Plenty for the strip's own pixel resolution: a third of a 4K screen is
    // ~1280px, so even 256 segments is ~5px each.
    static const int kMaxSegments = 256;

    struct Row
    {
        bool valid = false;              // has at least one timed segment
        int segmentCount = 0;
        // Reference average speed minus yours, through that segment, in
        // km/h -- +ve = you were SLOWER (a deficit, coloured red), -ve =
        // faster (coloured green). Deliberately reference-minus-yours rather
        // than the more natural yours-minus-reference, so the sign matches
        // the same "positive = worse, draws red" convention the rest of the
        // HUD uses (LapDelta's deltaSeconds, RelativeTiming's gapSeconds).
        float speedDeficitKmh[kMaxSegments] = {};
        bool filled[kMaxSegments] = {};  // false = not driven yet this lap
        // True for any segment where the brake was used at all (above
        // MiniSectorConfig::accelLineBrakeThreshold). Drawn as
        // a continuous green/red line under the strip, which shows the whole
        // braking ZONE and its length; the earlier version marked only the
        // single segment an application started in, which said nothing about
        // how long it lasted.
        bool braking[kMaxSegments] = {};
        // Separate from braking, deliberately. The line used to be a single
        // bool -- "braking" or, by elimination, "accelerating" -- which drew
        // the accelerating colour the instant you came off the brakes, all
        // the way through the coasting and turn-in phase where neither pedal
        // is doing anything. A corner therefore looked like brake-then-power
        // with no gap, which is not how any corner is actually driven.
        // Tracking the two independently leaves a genuine third state, drawn
        // as a gap in the line.
        bool accelerating[kMaxSegments] = {};
        // Wall-clock time (QueryPerformanceCounter seconds) a segment was
        // FILLED -- the moment its fade-in starts. Drawn at zero alpha right
        // at this timestamp, ramping to full over MiniSectorConfig's fade
        // duration, so a segment eases onto the strip instead of popping in
        // -- which matters most when one telemetry tick resolves several
        // segments at once (a fast straight with a small segment size):
        // each still starts its OWN fade independently, so they visually
        // sweep on in sequence rather than appearing as a block. Defaults to
        // 0.0, which is always further in the past than any fade duration,
        // so segments filled before this field existed still show at full
        // alpha immediately, as before.
        double revealAtSeconds[kMaxSegments] = {};
    };

    struct State
    {
        Row previous; // last completed lap
        Row current;  // lap in progress
        bool hasReference = false;
        // Reference lap's total time, when there is one -- used to derive
        // the fade-in duration (referenceLapTimeSeconds / segment count), so
        // a longer track's segments fade in slower and a short one's faster,
        // matching how long each one actually takes to drive through. -1
        // when there is no reference lap yet.
        double referenceLapTimeSeconds = -1.0;
        // Debug readout: the most recently computed segment's raw numbers,
        // so the value behind a colour can be checked directly against what
        // is actually happening on track instead of judging by eye alone.
        bool hasLastSegment = false;
        int lastSegmentIndex = -1;
        float lastSegmentDeltaKmh = 0.0f;
        float lastYourSpeedKmh = 0.0f;
        float lastRefSpeedKmh = 0.0f;
    };
    State Read();

    // Drops the snapshotted reference, so the strips stop comparing against
    // a lap that has just been deleted. The next scoring tick re-snapshots
    // if a reference still exists.
    void InvalidateReference();

private:
    CRITICAL_SECTION m_lock;
    bool m_ready = false;

    double m_trackLength = 0.0;

    // Position tracking between the 5Hz scoring resyncs.
    double m_lastKnownDist = 0.0;
    double m_forwardSpeed = 0.0;
    LARGE_INTEGER m_lastSyncTime = {};
    LARGE_INTEGER m_freq = {};

    // Reference lap's time for each segment BOUNDARY, snapshotted once per lap
    // on the scoring thread. Held as boundary times rather than per-segment
    // durations so a segment's reference is just a subtraction of two entries.
    double m_refBoundaryTime[kMaxSegments + 1] = {};
    bool m_hasReference = false;
    int m_refSegmentCount = 0;

    // Lap in progress (telemetry thread).
    long m_lastLapNumber = -1;
    short m_lastScoringLaps = -1;
    int m_currentSegment = -1;
    // Time spent in the segment currently being driven, ACCUMULATED tick by
    // tick from the sim's own clock rather than measured as (now - entry
    // time).
    //
    // This is deliberate and load-bearing. An entry-time scheme has to be
    // re-anchored whenever the ~5Hz scoring resync moves the extrapolated
    // distance, or the correction gets misread as real travel; but
    // re-anchoring also throws away the time already spent in the segment.
    // Since a mini-sector normally takes LONGER than the 0.2s scoring
    // interval, that meant almost every segment was timed from the last
    // resync instead of from its own entry -- a fraction of its true
    // duration, so every segment read as far faster than it was. Accumulated
    // time is immune: a resync changes only which segment we believe we are
    // in, never how much time has been banked.
    double m_segmentAccumTime = 0.0;
    // Previous telemetry sample's mElapsedTime, for the per-tick delta above.
    // The sim clock is used rather than a wall clock so it stops when the sim
    // does. NOT time-into-lap, which resets at the line mid-segment.
    double m_lastTelemET = -1.0;

    Row m_currentRow;
    Row m_previousRow;

    // Staging for the debug readout above, copied into m_state by PublishState.
    bool m_hasLastSegment = false;
    int m_lastSegmentIndex = -1;
    float m_lastSegmentDeltaKmh = 0.0f;
    float m_lastYourSpeedKmh = 0.0f;
    float m_lastRefSpeedKmh = 0.0f;

    State m_state; // guarded by m_lock

    // Cross-thread flags (review F110, F109, F111). Scoring publishes
    // whether the player car exists and is in the pits/garage; the render
    // thread only requests an invalidation, scoring carries it out and bumps
    // the generation, and telemetry clears its rows on a new generation.
    std::atomic<bool> m_playerFound{ false };
    std::atomic<bool> m_inPitsOrGarage{ false };
    std::atomic<bool> m_invalidateRequested{ false };
    std::atomic<unsigned> m_refGeneration{ 0 };
    unsigned m_telemGeneration = 0;          // telemetry thread
    // Telemetry thread (review F108): the segment being driven was entered
    // part-way (anchor/reset), so its time is not a full segment's.
    bool m_segmentPartial = true;
    // Lap number changed while still in the last segments: the distance
    // wrap that follows closes the last segment and rolls the row.
    bool m_lapChangePending = false;
    // The distance wrap already rolled the row; the lap-number change that
    // follows must not roll it a second time.
    bool m_wrapFinishedLap = false;

    int SegmentCount() const;
    void FillCrossed(int from, int crossed, int n, double elapsed, double dist, double tickDt);
    void SnapshotReference();
    void PublishState();
    void FinishLap();
};

extern MiniSectorStore g_miniSector;

class Renderer;

namespace MiniSector
{
    // Draws both strips. Returns false if there is nothing to show (disabled,
    // or no reference lap to compare against yet).
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight);
}
