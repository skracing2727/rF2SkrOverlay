#pragma once
#include <dxgi.h>
#include <d3d11.h>

namespace Overlay
{
    // Called from the Present hook every frame.
    void OnPresent(IDXGISwapChain* swapChain);

    // Called from the ResizeBuffers hook so cached render targets get rebuilt.
    void OnResize();

    // The currently bound device context, or null before the first frame is
    // bound. For PerfStats' GPU timestamp queries, issued from Hook.cpp
    // around the real Present() call, outside Overlay.cpp's own draw code.
    ID3D11DeviceContext* GetContext();
}
