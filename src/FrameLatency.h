#pragma once
#include <dxgi.h>

// Measures real presentation latency using DXGI's own frame statistics API
// (IDXGISwapChain::GetFrameStatistics) -- not the same measurement method
// CapFrameX/PresentMon use (they trace lower-level ETW events), so the two
// numbers may not match exactly, but this is a genuine, driver-reported
// figure, not an approximation invented here.
//
// Method: record the wall-clock time right before each real Present() call,
// tagged with the present count DXGI assigns it. Later, GetFrameStatistics()
// reports which present count actually reached the screen and when (SyncQPCTime)
// -- matching that back to our recorded call time gives call-to-display latency.
namespace FrameLatency
{
    void Init();

    // Call immediately before the real (unhooked) Present().
    void OnBeforePresent();
    // Call immediately after the real (unhooked) Present() returns.
    void OnAfterPresent(IDXGISwapChain* swapChain);

    // Current smoothed latency estimate in milliseconds. Returns false if no
    // measurement is available yet (e.g. the first few frames).
    bool GetLatencyMs(double& outMs);

    enum class UnavailableReason
    {
        None,
        // GetFrameStatistics has been returning DXGI_ERROR_INVALID_CALL --
        // the documented result when the swapchain isn't in EXCLUSIVE
        // fullscreen (windowed and borderless both hit this). Sustained
        // rather than a one-off, so a normal transient failure around a
        // mode switch doesn't get misreported as this.
        NotFullscreen,
    };
    // Why GetLatencyMs currently returns false, if it does -- lets the
    // overlay show a specific "needs fullscreen" note instead of just
    // silently omitting the stat.
    UnavailableReason GetUnavailableReason();
}
