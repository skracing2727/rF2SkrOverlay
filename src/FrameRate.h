#pragma once

// Smoothed frames-per-second, from the real Present-to-Present wall-clock
// interval. Simple and unambiguous, unlike CPU/GPU busy time -- FPS is
// "how often a frame actually got shown", which stays exactly as
// meaningful under a frame cap as without one.
namespace FrameRate
{
    // Call once per frame (same thread as Present, cheap QueryPerformanceCounter-
    // only work).
    void OnFrameTick();

    // Smoothed FPS. Returns false if no measurement yet (first frame).
    bool GetFps(double& outFps);

    // The latest single Present-to-Present interval, unsmoothed (the
    // benchmark box wants every frame). False before the second frame.
    bool GetLastFrameMs(double& outMs);

    // Feeds the frame-cap detector one fresh FPS sample -- call this at a
    // slow, steady cadence (the perf overlay's own [Performance] UpdateHz
    // refresh, not a hardcoded one), not every frame. Real-time-windowed
    // internally, so it stays meaningful at whatever that cadence is.
    void PollCapDetection(double currentFps);

    // True once FPS has spent a solid majority of its recent history sitting
    // essentially flat at its own recent ceiling -- the signature of an
    // active frame limiter (in-game cap, RTSS, driver-level), as opposed to
    // GPU/CPU-bound variability that wanders well below its own peak.
    // Deliberately NOT the live/instantaneous FPS: using that as a stand-in
    // for "the budget" is circular (heavier CPU/GPU load drops FPS, which
    // would loosen the very budget meant to catch that load). outCapFps
    // receives the detected ceiling only when this returns true.
    bool IsCapped(double& outCapFps);

    // Min / max / 1%-low FPS (average of the slowest 1% of frames) over the
    // last few seconds of real frame history. 1%-low is the stutter-
    // sensitive figure enthusiast benchmarks quote; plain min is one bad
    // frame, average hides stutter entirely. Returns false until enough
    // frame history exists.
    bool GetFpsRange(double& outMin, double& outLow1Pct, double& outMax);
}
