#pragma once

struct TelemInfoV01;

// Rough total-pipeline ("click-to-photon") latency estimate: how long from a
// control input being read by the sim to it appearing on screen.
//
// There is no way to hook the actual hardware device's own poll timestamp
// from a game plugin, so this is an ESTIMATE built from pieces the game and
// OS/driver do expose (review F180):
//   1. Half the 400 Hz physics step -- the average wait before an input is
//      picked up by the sim. (TelemInfoV01::mDeltaTime is the telemetry
//      interval, not the physics step, so it is not used.)
//   2. The render thread's CPU time per frame (PerfStats) plus half a frame
//      period (FrameRate) -- the average wait for, and work on, the frame
//      that shows the input.
//   3. FrameLatency's Display Latency -- Present call to scanout.
// Not included: the wheel's own USB/HID polling latency and the panel's
// response time, which no software on this side can see.
namespace InputLagEstimate
{
    // Call from the telemetry callback (player car only) each tick.
    void OnTelemetry(const ::TelemInfoV01& info);

    // Estimated total input-to-photon latency in milliseconds, combining the
    // pieces above with the given (already-measured) display latency.
    // Returns false until telemetry has arrived.
    bool GetEstimateMs(double displayLatencyMs, double& outMs);
}
