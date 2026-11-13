#include "FrameLatency.h"
#include "Log.h"
#include <Windows.h>

namespace
{
    LARGE_INTEGER g_freq = {};
    LARGE_INTEGER g_pendingCallTime = {};
    bool g_hasPending = false;

    // Small ring of (presentCount -> call time) so GetFrameStatistics' report
    // (which lags by an unpredictable number of frames) can be matched back to
    // when that specific frame's Present() was actually called.
    struct Entry { UINT presentCount; LARGE_INTEGER callTime; bool valid; };
    Entry g_history[16] = {};
    int g_historyNext = 0;

    double g_smoothedMs = 0.0;
    bool g_hasValue = false;
    UINT g_lastSeenPresentCount = 0;

    FrameLatency::UnavailableReason g_reason = FrameLatency::UnavailableReason::None;
    // Required run of consecutive INVALID_CALLs before concluding it's the
    // window mode and not just a one-off around a mode switch/focus change.
    int g_consecutiveInvalidCall = 0;
    const int kInvalidCallThreshold = 30;

    // Diagnostics: log the first several calls' outcomes so a failure mode
    // (GetFrameStatistics erroring, or never finding a matching present count)
    // is visible in the log instead of just "no number appeared".
    int g_diagLogCount = 0;
    const int kMaxDiagLogs = 20;
}

void FrameLatency::Init()
{
    QueryPerformanceFrequency(&g_freq);
}

void FrameLatency::OnBeforePresent()
{
    QueryPerformanceCounter(&g_pendingCallTime);
    g_hasPending = true;
}

void FrameLatency::OnAfterPresent(IDXGISwapChain* swapChain)
{
    if (!swapChain || g_freq.QuadPart == 0)
        return;

    // Tag the frame we just submitted with the call time recorded a moment ago.
    if (g_hasPending)
    {
        UINT presentCount = 0;
        if (SUCCEEDED(swapChain->GetLastPresentCount(&presentCount)))
        {
            g_history[g_historyNext] = { presentCount, g_pendingCallTime, true };
            g_historyNext = (g_historyNext + 1) % 16;
        }
        g_hasPending = false;
    }

    // Ask DXGI which present count actually reached the screen, and when.
    DXGI_FRAME_STATISTICS stats = {};
    const HRESULT hr = swapChain->GetFrameStatistics(&stats);
    if (FAILED(hr))
    {
        if (g_diagLogCount < kMaxDiagLogs)
        {
            Log::Write("FrameLatency: GetFrameStatistics FAILED hr=0x%08X", hr);
            ++g_diagLogCount;
        }

        // DXGI_ERROR_INVALID_CALL is the DOCUMENTED result when the swapchain
        // isn't in exclusive fullscreen, but real logs from actual windowed/
        // borderless sessions show DXGI_ERROR_UNSUPPORTED (0x887A0004)
        // instead, consistently -- apparently the real behaviour for a flip-
        // model swapchain outside exclusive fullscreen differs from the
        // documented one. Treat both as the same "not fullscreen" signal. A
        // sustained run of either (not just one, which could be a transient
        // around a mode switch) means that's genuinely why, not a bug.
        if (hr == DXGI_ERROR_INVALID_CALL || hr == DXGI_ERROR_UNSUPPORTED)
        {
            if (++g_consecutiveInvalidCall >= kInvalidCallThreshold)
                g_reason = FrameLatency::UnavailableReason::NotFullscreen;
        }
        else
        {
            g_consecutiveInvalidCall = 0;
        }
        return; // e.g. DXGI_ERROR_FRAME_STATISTICS_DISJOINT after a focus change -- skip this tick
    }
    g_consecutiveInvalidCall = 0;

    if (stats.PresentCount == g_lastSeenPresentCount)
        return; // nothing new completed since we last checked
    g_lastSeenPresentCount = stats.PresentCount;

    bool matched = false;
    for (int i = 0; i < 16; ++i)
    {
        if (g_history[i].valid && g_history[i].presentCount == stats.PresentCount)
        {
            matched = true;
            const double latencyMs = static_cast<double>(stats.SyncQPCTime.QuadPart - g_history[i].callTime.QuadPart)
                                    / static_cast<double>(g_freq.QuadPart) * 1000.0;

            // Sanity bound: a genuine call-to-display latency on this system
            // should be a handful of milliseconds, not seconds -- discard
            // anything wildly out of range (e.g. right after a mode switch)
            // rather than showing a nonsense number.
            const bool inRange = (latencyMs >= 0.0 && latencyMs < 200.0);
            if (inRange)
            {
                if (!g_hasValue)
                {
                    g_smoothedMs = latencyMs;
                    g_hasValue = true;
                }
                else
                {
                    // Light exponential smoothing -- responsive but not jittery
                    // frame to frame.
                    g_smoothedMs = g_smoothedMs * 0.85 + latencyMs * 0.15;
                }
                g_reason = FrameLatency::UnavailableReason::None;
            }

            if (g_diagLogCount < kMaxDiagLogs)
            {
                Log::Write("FrameLatency: matched presentCount=%u latency=%.2fms %s",
                           stats.PresentCount, latencyMs, inRange ? "(accepted)" : "(REJECTED, out of range)");
                ++g_diagLogCount;
            }
            break;
        }
    }

    if (!matched && g_diagLogCount < kMaxDiagLogs)
    {
        Log::Write("FrameLatency: stats.PresentCount=%u had NO match in history ring", stats.PresentCount);
        ++g_diagLogCount;
    }
}

bool FrameLatency::GetLatencyMs(double& outMs)
{
    if (!g_hasValue)
        return false;
    outMs = g_smoothedMs;
    return true;
}

FrameLatency::UnavailableReason FrameLatency::GetUnavailableReason()
{
    return g_reason;
}
