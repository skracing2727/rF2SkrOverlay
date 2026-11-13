#pragma once

// System GPU 3D-engine utilization %, via the same OS "GPU Engine"
// performance counters Task Manager's own GPU graph (and tools built on
// PresentMon-adjacent data) read -- NOT the D3D11 timestamp-query approach
// PerfStats.cpp uses for CPU/GPU frame *pacing*.
//
// That distinction matters: a Present-to-Present GPU timestamp interval
// includes any idle/wait time the GPU spends pacing to a frame cap or
// vsync, so it reads noticeably higher than "real" busy time whenever a cap
// is active. These PDH counters report actual engine occupancy, excluding
// idle -- the same category of figure CapFrameX/Task Manager show.
namespace GpuEngineUsage
{
    // Opens the PDH query and does the first counter-path expansion. Safe
    // to call multiple times.
    void Init();

    void Shutdown();

    // Cheap-ish; call at the perf overlay's own refresh cadence (not every
    // frame). Periodically re-expands the counter list internally, since
    // GPU engine instances come and go as other processes start/stop.
    void Poll();

    // 0-100 (can exceed 100 if summed across multiple concurrent 3D
    // workloads, though that's rare on a single-GPU gaming rig). False
    // until the first successful poll.
    bool GetBusyPercent(double& outPercent);
}
