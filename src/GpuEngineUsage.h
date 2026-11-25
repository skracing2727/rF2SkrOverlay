#pragma once

// The game's GPU 3D-engine utilization %, via the same OS "GPU Engine"
// performance counters Task Manager's own GPU graph (and tools built on
// PresentMon-adjacent data) read -- NOT the D3D11 timestamp-query approach
// PerfStats.cpp uses for CPU/GPU frame *pacing*. Only this process's 3D
// engines on rF2's own adapter are read (busiest engine, 0-100); if none can
// be told apart, every process's are.
//
// That distinction matters: a Present-to-Present GPU timestamp interval
// includes any idle/wait time the GPU spends pacing to a frame cap or
// vsync, so it reads noticeably higher than "real" busy time whenever a cap
// is active. These PDH counters report actual engine occupancy, excluding
// idle -- the same category of figure CapFrameX/Task Manager show.
namespace GpuEngineUsage
{
    // Opens the PDH query (one language-independent wildcard counter) and
    // takes the baseline sample. Safe to call multiple times.
    void Init();

    // Fresh baseline sample; call when the stat is turned back on after
    // Poll was skipped for a while.
    void Rebaseline();

    void Shutdown();

    // Cheap-ish; call at the perf overlay's own refresh cadence (not every
    // frame). Poller thread only.
    void Poll();

    // 0-100, the busiest 3D engine. False until the first successful poll.
    bool GetBusyPercent(double& outPercent);
}
