#pragma once
#include <Windows.h>
#include <vector>
#include <atomic>

// Per-tyre degradation, reset each lap (% accrued since the CURRENT lap
// started, not a cumulative total), plus an estimate of remaining tyre life
// from the SDK's own cumulative wear fraction and the recently learned
// per-lap degradation rate. No persistence -- tyre wear behaviour is tied to
// the specific set mounted this run, not a car+track constant worth saving
// across sessions the way fuel consumption or a pit limit is.
class TyreWearStore
{
public:
    void Init();
    // Called from the game's telemetry callback (player car only).
    void OnTelemetry(const struct TelemInfoV01& info);
    // Scoring (~5Hz): only the player's pit-lane flag, which telemetry lacks.
    void OnScoring(const struct ScoringInfoV01& info);
    void Clear();

    struct WheelState
    {
        double degradationThisLapPercent = 0.0; // since the current lap started
        bool anomalyHigh = false;                // this lap's rate is a sudden spike vs recent laps
        double timeLeftSeconds = -1.0;           // -1 = not enough data yet
        double lapsRemaining = -1.0;             // same estimate, in laps rather than time -- -1 = not enough data yet
        double remainingPercent = 100.0;         // the SDK's own mWear (life remaining, 1.0=fresh) as a %, 0..100
        double lapFraction = -1.0;               // how far through the lap (by time vs the last lap), 0..1; -1 = unknown
        // This lap's wear and lap fraction, refreshed only 20 times a lap (each
        // 5% of the lap), for the wear plot's moving dot.
        double steppedDegradationPercent = 0.0;
        double steppedLapFraction = -1.0;
    };

    // One wheel's wear per lap across every counted lap this session, as a
    // box plot: the middle half of the laps (q1..q3), the median, whiskers
    // out to the furthest lap within 1.5x the box's width of it (Tukey's
    // rule), and anything beyond that as a separate outlier point. In-laps,
    // out-laps and any lap with a pit-lane visit are left out -- pit-lane
    // driving wears tyres nothing like a racing lap. Recomputed once per lap,
    // not per frame.
    struct WearDistribution
    {
        static const int kMaxOutliers = 12;
        int count = 0;               // counted laps; 0 = nothing to plot yet
        float q1 = 0.0f, median = 0.0f, q3 = 0.0f;
        float whiskerLo = 0.0f, whiskerHi = 0.0f;
        int outlierCount = 0;
        float outliers[kMaxOutliers] = {};
        float lastLap = -1.0f;       // the most recent counted lap
    };

    struct State
    {
        bool valid = false;
        WheelState wheel[4]; // FL, FR, RL, RR -- same order as TelemInfoV01::mWheel
        WearDistribution distribution[4]; // same order

        // The just-completed lap's own final numbers, held for
        // FuelTyrePanelConfig::tyreFreezeAfterLineSeconds after crossing the
        // line -- same idea as LapDelta's freezeAfterLineSeconds. Without
        // this, degradationThisLapPercent resets toward 0 for the new lap at
        // the exact instant the just-finished lap's real number becomes
        // worth reading.
        bool haveLineFreeze = false;
        WheelState frozenWheel[4];
        LARGE_INTEGER lineCrossedAt = {};
    };
    State Read();

private:
    static const int kLapHistorySize = 8;

    CRITICAL_SECTION m_lock;
    bool m_ready = false;
    State m_state;
    LARGE_INTEGER m_freq = {};

    long m_lastLapNumber = -1;
    double m_wearAtLapStart[4] = {};
    bool m_haveLapStartWear = false;
    double m_lastLapTimeSeconds = 0.0;
    double m_prevElapsedTime = 0.0;
    double m_prevLapStartET = 0.0;

    // Rolling history of completed-lap %/lap degradation, per wheel -- used
    // both for the "sudden increase" anomaly check and the remaining-life
    // estimate.
    double m_lapHistory[4][kLapHistorySize] = {};
    int m_lapHistoryCount[4] = {};
    int m_lapHistoryNext[4] = {};

    double AverageHistory(int wheel) const;

    // Every counted lap's %/lap this session, per wheel (see WearDistribution).
    static const size_t kMaxSessionLaps = 1000;
    std::vector<float> m_sessionLaps[4];
    // The lap in progress has been in the pit lane, or had a tyre change --
    // it will not be counted.
    bool m_lapTouchedPits = false;
    // Written by OnScoring, read by OnTelemetry.
    std::atomic<bool> m_playerInPits{ false };
    void RebuildDistribution(int wheel);
    int m_dotStep[4] = { -1, -1, -1, -1 };
    long m_dotLap = -1;
};

extern TyreWearStore g_tyreWear;
