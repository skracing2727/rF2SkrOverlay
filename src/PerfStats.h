#pragma once
#include <dxgi.h>
#include <d3d11.h>

// Per-frame CPU compute time and GPU VRAM diagnostics -- separate from
// FrameRate.cpp (Present-to-Present wall-clock timing), FrameLatency.cpp
// (DXGI-reported display latency), and GpuEngineUsage.cpp (real GPU busy%
// via OS perf counters, used for the GPU frame time readout instead of a
// D3D11 timestamp-query approach that was tried here and dropped -- see
// GpuEngineUsage.h for why).
namespace PerfStats
{
    // Call once a real device/swapchain is bound (see Overlay.cpp's BindTo).
    // Safe to call again on a rebind; re-resolves the VRAM adapter.
    void Init(IDXGISwapChain* swapChain);

    // Call immediately before the real Present() (see Hook.cpp). Records the
    // thread's cycle count so OnFrame can subtract everything spent INSIDE
    // Present -- a frame limiter (RTSS, driver, in-game) that busy-waits
    // there would otherwise make CPU time read as the capped frame time.
    void OnBeforePresent();

    // Call once per frame, same thread as Present, right after the real
    // Present() call returns. Cheap: one QueryThreadCycleTime call.
    void OnFrame();

    // Cheap; call at the perf overlay's own refresh cadence (not every
    // frame) -- GetProcessMemoryInfo/QueryVideoMemoryInfo are OS calls, not
    // something to do 60-500 times a second for a once-or-twice-a-second
    // readout.
    void Poll();

    // Smoothed CPU time this process's render/main thread (the one calling
    // Present) actually spent computing between one frame and the next --
    // via QueryThreadCycleTime (hardware cycle counter), NOT process-wide
    // CPU%, which is meaningless on a multi-core box when only a couple of
    // threads are doing real work.
    bool GetCpuFrameTimeMs(double& outMs);

    // Process working-set RAM in MB.
    bool GetProcessRamMB(double& outMB);

    // GPU VRAM currently committed to this process's budget, in MB. False
    // if IDXGIAdapter3 (Windows 10+ / DXGI 1.4) isn't available.
    bool GetVramMB(double& outUsedMB, double& outBudgetMB);
}
