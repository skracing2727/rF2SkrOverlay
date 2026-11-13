#include "FrameRate.h"
#include <Windows.h>
#include <cmath>
#include <algorithm>

namespace
{
    LARGE_INTEGER g_freq = {};
    LARGE_INTEGER g_lastTickTime = {};
    double g_smoothedFrameMs = 0.0;
    bool g_hasValue = false;

    // Rolling FPS history for cap detection. What actually matters for a
    // meaningful judgment is REAL elapsed time covered, not sample count --
    // PollCapDetection is fed at [Performance] UpdateHz, which is now
    // user-configurable (0.2-30Hz), so a fixed sample count that assumed a
    // fixed ~500ms cadence would cover anywhere from ~1 minute (0.2Hz) down
    // to ~2 seconds (30Hz) instead of a consistent window. kCapWindowSize is
    // just an upper bound on ring slots at the 30Hz ceiling; IsCapped()
    // filters to the real kCapWindowSeconds by timestamp.
    constexpr double kCapWindowSeconds = 30.0;
    const int kCapWindowSize = 30 * static_cast<int>(kCapWindowSeconds); // = 900
    double g_capSamples[kCapWindowSize] = {};
    LARGE_INTEGER g_capSampleTimes[kCapWindowSize] = {};
    int g_capSampleNext = 0;
    int g_capSampleCount = 0;

    // Raw (unsmoothed) per-frame FPS history for min/1%-low/max, over a much
    // shorter real-time window than the cap detector above -- stutters get
    // smoothed away by the 500ms decay in a few frames, so this needs the
    // instantaneous per-frame value, not g_smoothedFrameMs. Sized generously
    // for high refresh rates (e.g. 1000fps unlocked for a few seconds).
    constexpr double kRangeWindowSeconds = 5.0;
    constexpr int kRangeWindowSize = 6000;
    double g_rangeSamples[kRangeWindowSize] = {};
    LARGE_INTEGER g_rangeSampleTimes[kRangeWindowSize] = {};
    int g_rangeSampleNext = 0;
    int g_rangeSampleCount = 0;
}

void FrameRate::OnFrameTick()
{
    if (g_freq.QuadPart == 0)
        QueryPerformanceFrequency(&g_freq);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (g_lastTickTime.QuadPart != 0 && g_freq.QuadPart != 0)
    {
        const double frameMs = static_cast<double>(now.QuadPart - g_lastTickTime.QuadPart)
                              / static_cast<double>(g_freq.QuadPart) * 1000.0;
        if (frameMs > 0.01 && frameMs < 2000.0)
        {
            if (!g_hasValue)
            {
                g_smoothedFrameMs = frameMs;
                g_hasValue = true;
            }
            else
            {
                // Same 500ms time-based decay used by the other perf
                // readouts, for the same reason: a fixed per-tick weight's
                // window would shrink as frame rate rises.
                constexpr double kTauMs = 500.0;
                const double alpha = 1.0 - exp(-frameMs / kTauMs);
                g_smoothedFrameMs = g_smoothedFrameMs * (1.0 - alpha) + frameMs * alpha;
            }

            g_rangeSamples[g_rangeSampleNext] = 1000.0 / frameMs;
            g_rangeSampleTimes[g_rangeSampleNext] = now;
            g_rangeSampleNext = (g_rangeSampleNext + 1) % kRangeWindowSize;
            if (g_rangeSampleCount < kRangeWindowSize) ++g_rangeSampleCount;
        }
    }
    g_lastTickTime = now;
}

bool FrameRate::GetFps(double& outFps)
{
    if (!g_hasValue || g_smoothedFrameMs <= 0.0)
        return false;
    outFps = 1000.0 / g_smoothedFrameMs;
    return true;
}

void FrameRate::PollCapDetection(double currentFps)
{
    if (currentFps <= 1.0)
        return;
    if (g_freq.QuadPart == 0)
        QueryPerformanceFrequency(&g_freq);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    g_capSamples[g_capSampleNext] = currentFps;
    g_capSampleTimes[g_capSampleNext] = now;
    g_capSampleNext = (g_capSampleNext + 1) % kCapWindowSize;
    if (g_capSampleCount < kCapWindowSize) ++g_capSampleCount;
}

bool FrameRate::IsCapped(double& outCapFps)
{
    if (g_capSampleCount == 0 || g_freq.QuadPart == 0)
        return false;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int start = (g_capSampleCount < kCapWindowSize) ? 0 : g_capSampleNext;

    double ceiling = 0.0;
    int validCount = 0;
    for (int i = 0; i < g_capSampleCount; ++i)
    {
        const int idx = (start + i) % kCapWindowSize;
        const double ageSeconds = static_cast<double>(now.QuadPart - g_capSampleTimes[idx].QuadPart) / static_cast<double>(g_freq.QuadPart);
        if (ageSeconds > kCapWindowSeconds)
            continue;
        ++validCount;
        if (g_capSamples[idx] > ceiling) ceiling = g_capSamples[idx];
    }

    // Require a reasonable amount of REAL history before judging anything --
    // otherwise the first few samples right after a scene load would look
    // "capped" purely for lack of contrary evidence yet. Kept low enough
    // (rather than the old cadence-assuming 10) that this can still resolve
    // within the 30s window even at the slowest configurable [Performance]
    // UpdateHz (0.2Hz => 6 samples in 30s).
    constexpr int kMinSamples = 6;
    if (validCount < kMinSamples || ceiling <= 1.0)
        return false;

    // A real frame limiter holds FPS essentially flat at the cap; GPU/CPU-
    // bound variability wanders well below its own recent peak instead. So:
    // most of the window sitting within 1% of its own ceiling is the
    // signature of an active cap.
    constexpr double kCloseToleranceFraction = 0.01;
    constexpr double kRequiredCloseFraction = 0.5;
    const double closeThreshold = ceiling * (1.0 - kCloseToleranceFraction);

    int closeCount = 0;
    for (int i = 0; i < g_capSampleCount; ++i)
    {
        const int idx = (start + i) % kCapWindowSize;
        const double ageSeconds = static_cast<double>(now.QuadPart - g_capSampleTimes[idx].QuadPart) / static_cast<double>(g_freq.QuadPart);
        if (ageSeconds > kCapWindowSeconds)
            continue;
        if (g_capSamples[idx] >= closeThreshold) ++closeCount;
    }

    const bool capped = (static_cast<double>(closeCount) / validCount) >= kRequiredCloseFraction;
    if (capped)
        outCapFps = ceiling;
    return capped;
}

bool FrameRate::GetFpsRange(double& outMin, double& outLow1Pct, double& outMax)
{
    if (g_rangeSampleCount == 0 || g_freq.QuadPart == 0)
        return false;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int start = (g_rangeSampleCount < kRangeWindowSize) ? 0 : g_rangeSampleNext;

    static double s_sorted[kRangeWindowSize];
    int validCount = 0;
    for (int i = 0; i < g_rangeSampleCount; ++i)
    {
        const int idx = (start + i) % kRangeWindowSize;
        const double ageSeconds = static_cast<double>(now.QuadPart - g_rangeSampleTimes[idx].QuadPart) / static_cast<double>(g_freq.QuadPart);
        if (ageSeconds > kRangeWindowSeconds)
            continue;
        s_sorted[validCount++] = g_rangeSamples[idx];
    }

    constexpr int kMinSamples = 10;
    if (validCount < kMinSamples)
        return false;

    std::sort(s_sorted, s_sorted + validCount);
    outMin = s_sorted[0];
    outMax = s_sorted[validCount - 1];

    // 1%-low: average of the slowest 1% of frames (minimum 1 frame).
    int lowCount = validCount / 100;
    if (lowCount < 1) lowCount = 1;
    double lowSum = 0.0;
    for (int i = 0; i < lowCount; ++i) lowSum += s_sorted[i];
    outLow1Pct = lowSum / lowCount;

    return true;
}
