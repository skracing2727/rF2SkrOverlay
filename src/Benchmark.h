#pragma once
#include <windows.h>

class Renderer;
struct TelemInfoV01;
struct ScoringInfoV01;
struct ID3D11Device;
struct ID3D11DeviceContext;

// Benchmark box: two graphs for judging where frame time goes, lap by lap.
//
//  1. FPS through the lap. Bottom axis: seconds since the start/finish line;
//     top axis: lap distance (the average distance reached at that time).
//     Three series, each turned into FPS (1000 / ms):
//       white = present-to-present interval (what is actually shown),
//       blue  = CPU time the game's render thread spent on the frame,
//       red   = GPU frame time from D3D11 timestamp queries.
//     This lap's frames are dots (reset at the line); one line per series is
//     the average over every lap this session at each point of the lap.
//     A table by the origin gives the lap's avg and 1% / 0.1% lows.
//  2. A histogram of the lap's frame times (0.01 ms bins) or, after a click,
//     FPS (0.1 FPS bins), with earlier laps faint behind, the 1% / 0.1% lows
//     shaded, and dotted 60..240 FPS markers.
//
// The GPU series measures the GPU clock between one frame's end-of-HUD
// timestamp and the next. That includes any time the GPU sat idle waiting
// for the CPU, a frame cap or vsync, so it only equals real GPU work when
// the GPU is the bottleneck. The box says so whenever the OS's GPU busy %
// (GpuEngineUsage) is below the warning threshold.
//
// Everything except OnTelemetry/Reset runs on the Present thread, so the
// sample stores need no locks. Memory is fixed-size; nothing is allocated
// per frame and nothing is written to disk.
namespace Benchmark
{
    void OnTelemetry(const TelemInfoV01& info);   // telemetry thread, player car only
    void OnScoring(const ScoringInfoV01& info);   // scoring thread: player's lap distance, for the top axis
    void Shutdown();                              // stops the percentile worker (plugin shutdown)
    void Reset();                                 // new session (any thread)

    // Present thread, once per frame of the game's own swapchain, right
    // after the real Present returned (after PerfStats::OnFrame, so this
    // frame's CPU time is ready).
    void OnFrameEnd();

    // Present thread, after the HUD is drawn and before the real Present:
    // issues this frame's GPU timestamp on the immediate context and reads
    // back finished ones without ever waiting for the GPU.
    void OnGpuMark(ID3D11Device* device, ID3D11DeviceContext* context);
    // Frees the timestamp queries; call before the device is released.
    void ReleaseGpu();

    // hwnd: the game window, for the click that switches the histogram
    // between frame time and FPS.
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight, HWND hwnd);
}
