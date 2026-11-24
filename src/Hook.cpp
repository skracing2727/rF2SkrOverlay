#include "Hook.h"
#include "Overlay.h"
#include "FrameLatency.h"
#include "FrameRate.h"
#include "PerfStats.h"
#include "Log.h"
#include <d3d11.h>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffersFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static PresentFn s_originalPresent = nullptr;
static ResizeBuffersFn s_originalResizeBuffers = nullptr;
static void** s_vtable = nullptr;

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
    // Drawing happens before the real Present so it's included in what's
    // shown. Latency is timed around the real call itself, so the number
    // drawn this frame reflects the last fully-measured present, one or two
    // frames behind -- normal for this kind of live readout.
    Overlay::OnPresent(pSwapChain);

    // Frame stats follow the game view's swapchain only (decided after
    // OnPresent, which binds it): any other swapchain presenting in between
    // would otherwise count as an extra frame -- halving the frame time and
    // the per-frame CPU time -- and its GetFrameStatistics would be mixed in.
    const bool mainSwapChain = Overlay::IsMainSwapChain(pSwapChain);

    if (mainSwapChain)
    {
        FrameRate::OnFrameTick();
        FrameLatency::OnBeforePresent();
        PerfStats::OnBeforePresent();
    }
    const HRESULT hr = s_originalPresent(pSwapChain, SyncInterval, Flags);
    if (mainSwapChain)
    {
        FrameLatency::OnAfterPresent(pSwapChain);

        // Right after Present, same as FrameLatency above -- CPU thread time
        // wants to bracket the real Present() call itself.
        PerfStats::OnFrame();
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    Overlay::OnResize();
    return s_originalResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

// Creates a throwaway device+swapchain purely to read the real vtable pointers.
// The vtable itself lives in the DXGI/D3D11 runtime, not in our dummy objects,
// so it stays valid after we tear the dummy objects down.
static bool GetRealVTable(void*** outVTable)
{
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "rF2RadarDummyWndClass";
    RegisterClassExA(&wc);

    HWND hWnd = CreateWindowExA(0, wc.lpszClassName, "dummy", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hWnd)
        return false;

    DXGI_SWAP_CHAIN_DESC scDesc = {};
    scDesc.BufferCount = 1;
    scDesc.BufferDesc.Width = 100;
    scDesc.BufferDesc.Height = 100;
    scDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferDesc.RefreshRate.Numerator = 60;
    scDesc.BufferDesc.RefreshRate.Denominator = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.OutputWindow = hWnd;
    scDesc.SampleDesc.Count = 1;
    scDesc.Windowed = TRUE;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 1, D3D11_SDK_VERSION,
        &scDesc, &swapChain, &device, nullptr, &context);

    bool ok = false;
    if (SUCCEEDED(hr) && swapChain)
    {
        void** vtable = *reinterpret_cast<void***>(swapChain);
        *outVTable = vtable;
        ok = true;
    }

    if (swapChain) swapChain->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(hWnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);

    return ok;
}

static void PatchVTableEntry(void** vtable, int index, void* newFn, void** outOriginal)
{
    DWORD oldProtect;
    VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &oldProtect);
    *outOriginal = vtable[index];
    vtable[index] = newFn;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
}

bool Hook::Install()
{
    if (s_vtable)
        return true; // already installed

    void** vtable = nullptr;
    if (!GetRealVTable(&vtable))
    {
        Log::Write("Hook::Install: could not create probe device/swapchain yet");
        return false;
    }

    Log::Write("Hook::Install: got DXGI vtable at %p", vtable);
    s_vtable = vtable;

    // IDXGISwapChain vtable layout: 0-2 IUnknown, 3-6 IDXGIObject, 7 GetDevice,
    // 8 Present, 9 GetBuffer, 10 SetFullscreenState, 11 GetFullscreenState,
    // 12 GetDesc, 13 ResizeBuffers, ...
    PatchVTableEntry(vtable, 8, reinterpret_cast<void*>(&HookedPresent), reinterpret_cast<void**>(&s_originalPresent));
    PatchVTableEntry(vtable, 13, reinterpret_cast<void*>(&HookedResizeBuffers), reinterpret_cast<void**>(&s_originalResizeBuffers));

    Log::Write("Hook::Install: Present/ResizeBuffers hooked (orig Present=%p)", s_originalPresent);
    return true;
}

void Hook::Uninstall()
{
    if (!s_vtable)
        return;

    DWORD oldProtect;
    if (s_originalPresent)
    {
        VirtualProtect(&s_vtable[8], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_vtable[8] = reinterpret_cast<void*>(s_originalPresent);
        VirtualProtect(&s_vtable[8], sizeof(void*), oldProtect, &oldProtect);
    }
    if (s_originalResizeBuffers)
    {
        VirtualProtect(&s_vtable[13], sizeof(void*), PAGE_READWRITE, &oldProtect);
        s_vtable[13] = reinterpret_cast<void*>(s_originalResizeBuffers);
        VirtualProtect(&s_vtable[13], sizeof(void*), oldProtect, &oldProtect);
    }
}
