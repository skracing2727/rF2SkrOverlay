#pragma once

// AMD GPU power draw via AMD's public ADL2 API (atiadlxx.dll, already
// installed by the AMD driver -- loaded dynamically here, no SDK/link-time
// dependency, so this is a silent no-op on a non-AMD system).
//
// Unlike PerfStats.cpp's CPU/GPU frame time (D3D11 timestamp queries, works
// on any GPU vendor), real power-draw numbers only exist behind each GPU
// vendor's own proprietary API (ADL for AMD, NVAPI for NVIDIA) -- there is
// no vendor-neutral D3D/DXGI call for this. AMD-only by design.
//
// PMLog sensor coverage (which of ASIC/board/graphics power is reported)
// varies by GPU generation and driver version -- this reads whichever of
// ASIC power, board power, then graphics power the driver actually reports
// as supported, in that preference order, and is otherwise unverified
// until tested on real hardware.
namespace AmdGpuPower
{
    // Loads atiadlxx.dll and creates an ADL2 context. Safe to call multiple
    // times; a second call is a no-op if already initialized. Returns false
    // (silently) if the DLL isn't present or no AMD adapter responds --
    // that's the expected, non-error outcome on non-AMD hardware.
    bool Init();

    void Shutdown();

    // Current GPU power draw in watts. False if Init() wasn't called, found
    // no usable adapter/sensor, or the query failed this time.
    bool GetPowerWatts(double& outWatts);
}
