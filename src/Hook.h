#pragma once
#include <dxgi.h>
#include <d3d11.h>

// Hooks IDXGISwapChain::Present process-wide by patching the shared DXGI vtable,
// the same technique tools like ReShade/RTSS's OSD use. This lets us draw
// directly into the game's own back buffer -- no separate window involved,
// so it cannot conflict with DWM/Independent-Flip the way a normal overlay does.
namespace Hook
{
    // Call once, early. Safe to call from a background thread (not DllMain).
    bool Install();
    void Uninstall();
}
