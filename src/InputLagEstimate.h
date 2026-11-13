#pragma once

struct TelemInfoV01;

// Rough total-pipeline ("click-to-photon") latency estimate: how long from a
// control input being read by the sim to it appearing on screen.
//
// There is no way to hook the actual hardware device's own poll timestamp
// from a game plugin -- this is a LOWER-BOUND estimate built from two pieces
// the game and OS/driver DO expose:
//   1. Half the average physics-tick period (TelemInfoV01::mDeltaTime) -- on
//      average, a device is read mid-way through waiting for the NEXT tick to
//      pick it up, so this is the expected wait before an input is even seen
//      by the sim.
//   2. FrameLatency's own Display Latency figure -- real Present-call-to-
//      scanout time, i.e. everything from a simulated frame being submitted
//      to it actually reaching the screen.
// Missing from this (and so NOT included, making it an underestimate): the
// physical controller/wheel's own USB/HID polling latency, which no software
// on this side of the device can see.
namespace InputLagEstimate
{
    // Call from the telemetry callback (player car only) each tick.
    void OnTelemetry(const ::TelemInfoV01& info);

    // Estimated total input-to-photon latency in milliseconds, combining the
    // current physics-tick estimate with the given (already-measured)
    // display latency. Returns false if no physics-tick data yet.
    bool GetEstimateMs(double displayLatencyMs, double& outMs);
}
