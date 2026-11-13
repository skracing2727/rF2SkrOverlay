#include "PerfStats.h"
#include "Log.h"
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include <cmath>
#include <cstdint>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")

namespace
{
    IDXGIAdapter3* g_adapter3 = nullptr;

    // --- CPU frame time (render thread's own compute time, per frame) ---
    //
    // GetThreadTimes/GetProcessTimes (FILETIME) only update at the OS
    // scheduler's own clock-tick granularity -- commonly 15.625ms on
    // Windows without timeBeginPeriod tricks. At 96-190fps (5-10ms/frame)
    // that's coarser than the thing being measured: most frames see no
    // change at all, then one frame absorbs a full stale tick, reading as a
    // near-constant ~15.6ms unrelated to real per-frame cost. So this uses
    // QueryThreadCycleTime instead -- a hardware cycle counter, immune to
    // the scheduler tick -- converted to ms via a one-time calibration
    // against the CPU's nominal frequency.
    bool g_haveCycleCalibration = false;
    double g_cyclesPerMs = 0.0;
    bool g_haveLastThreadCycles = false;
    ULONGLONG g_lastThreadCycles = 0;
    ULONGLONG g_beforePresentCycles = 0;
    bool g_haveBeforePresent = false;
    double g_cpuFrameMs = 0.0;
    bool g_haveCpuFrameMs = false;

    // QueryThreadCycleTime counts cycles against the CPU's invariant TSC
    // rate, which tracks its nominal/base frequency (not the turbo-boosted
    // instantaneous one) -- so the registry's reported base MHz converts
    // cycles to ms correctly regardless of turbo state at measurement time.
    // This is the standard technique profilers use for this conversion.
    void CalibrateCyclesPerMs()
    {
        HKEY key;
        DWORD mhz = 0, size = sizeof(mhz);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &key) == ERROR_SUCCESS)
        {
            RegQueryValueExA(key, "~MHz", nullptr, nullptr, reinterpret_cast<LPBYTE>(&mhz), &size);
            RegCloseKey(key);
        }

        if (mhz > 0)
        {
            g_cyclesPerMs = static_cast<double>(mhz) * 1000.0; // MHz -> cycles/ms
            g_haveCycleCalibration = true;
        }
        else
        {
            // Fallback if the registry key is ever missing: measure cycles
            // burned over a short sleep against wall time. Noisier (this
            // thread may be scheduled out for part of it), but better than
            // nothing.
            LARGE_INTEGER freq, t0, t1;
            QueryPerformanceFrequency(&freq);
            ULONGLONG c0 = 0, c1 = 0;
            QueryThreadCycleTime(GetCurrentThread(), &c0);
            QueryPerformanceCounter(&t0);
            Sleep(30);
            QueryThreadCycleTime(GetCurrentThread(), &c1);
            QueryPerformanceCounter(&t1);

            const double elapsedMs = static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart) * 1000.0;
            if (elapsedMs > 1.0)
            {
                g_cyclesPerMs = static_cast<double>(c1 - c0) / elapsedMs;
                g_haveCycleCalibration = g_cyclesPerMs > 1.0;
            }
        }
    }

    // --- Published CPU frame time, for SkrSensorFeeder -> RTSS ---
    //
    // Under an RTSS frame cap, PresentMon's "CPU busy" counts the limiter's
    // wait as CPU time; g_cpuFrameMs above already excludes it. A small
    // shared-memory block exposes it to SkrSensorFeeder, which republishes
    // it to RTSS. The layout is a fixed interface shared with the feeder --
    // change it only together with SkrSensorFeeder. "Local\" on purpose: the
    // elevated feeder in the same session can open it, Global\ would need
    // SeCreateGlobalPrivilege. Created once, kept for the process lifetime
    // (the OS releases it on exit); the feeder treats data over 1 s old as
    // "game not running", so nothing is cleared.
#pragma pack(push, 8)
    struct SkrPerfShare
    {
        uint32_t magic;          // 0x50524B53 ('SKRP')
        uint32_t version;        // 1
        volatile LONG seq;       // seqlock: odd while writing, even when stable
        uint32_t reserved;       // 0
        double cpuFrameMs;       // = g_cpuFrameMs (the smoothed value)
        int64_t qpcLastWrite;    // QueryPerformanceCounter at write time
        int64_t qpcFrequency;    // QueryPerformanceFrequency
    };
#pragma pack(pop)
    static_assert(sizeof(SkrPerfShare) == 40, "SkrPerfShare layout is shared with SkrSensorFeeder");

    SkrPerfShare* g_perfShare = nullptr;
    bool g_perfShareTried = false;

    void PublishCpuFrameMs()
    {
        if (!g_perfShareTried)
        {
            g_perfShareTried = true;
            HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 64,
                                                L"Local\\SkrOverlay_PerfShare");
            void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 64) : nullptr;
            if (!view)
            {
                Log::Write("PerfStats: could not create SkrOverlay_PerfShare (error %lu) -- CPU frame time not published",
                           GetLastError());
                if (mapping) CloseHandle(mapping);
                return;
            }
            // The mapping handle is deliberately never closed: the view keeps
            // the section alive, but the named handle is what lets the feeder
            // open it by name.
            g_perfShare = static_cast<SkrPerfShare*>(view);
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            g_perfShare->magic = 0x50524B53u;
            g_perfShare->version = 1;
            g_perfShare->reserved = 0;
            g_perfShare->qpcFrequency = freq.QuadPart;
            Log::Write("PerfStats: publishing CPU frame time to Local\\SkrOverlay_PerfShare");
        }
        if (!g_perfShare)
            return;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        InterlockedIncrement(&g_perfShare->seq); // odd: writing
        g_perfShare->cpuFrameMs = g_cpuFrameMs;
        g_perfShare->qpcLastWrite = now.QuadPart;
        InterlockedIncrement(&g_perfShare->seq); // even: stable
    }

    // --- Per-frame raw CPU time, for rF2SkrTelemetry ---
    //
    // The block above carries the SMOOTHED value (what RTSS shows). The
    // telemetry plugin records every frame instead, so each frame's raw
    // render-thread CPU time also goes into a ring with its QPC timestamp.
    // Writer: this thread only. Reader: rF2SkrTelemetry, polling ~10 times a
    // second; it reads [lastRead, writeCount) and drops anything the writer
    // may have lapped (writeCount re-checked after copying). Interface shared
    // with rF2SkrTelemetry's Frames.cpp -- change both together.
#pragma pack(push, 8)
    struct SkrFrameEntry
    {
        int64_t qpc;             // QueryPerformanceCounter right after the real Present returned
        float   cpuMs;           // render-thread CPU time of this frame, excluding time inside Present
        float   reserved;
    };
    struct SkrFrameRing
    {
        uint32_t magic;          // 0x46524B53 ('SKRF')
        uint32_t version;        // 1
        uint32_t capacity;       // entries (power of two)
        uint32_t reserved;
        int64_t  qpcFrequency;
        volatile LONG64 writeCount;  // entries ever written; entry i is at i % capacity
        SkrFrameEntry entries[1];
    };
#pragma pack(pop)
    const uint32_t kRingCapacity = 4096;     // ~28 s at 144 fps
    SkrFrameRing* g_ring = nullptr;
    bool g_ringTried = false;

    void PushFrame(double cpuMs)
    {
        if (!g_ringTried)
        {
            g_ringTried = true;
            const DWORD bytes = static_cast<DWORD>(sizeof(SkrFrameRing) + (kRingCapacity - 1) * sizeof(SkrFrameEntry));
            HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes,
                                                L"Local\\SkrOverlay_FrameRing");
            void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, bytes) : nullptr;
            if (!view)
            {
                Log::Write("PerfStats: could not create SkrOverlay_FrameRing (error %lu) -- per-frame CPU time not published",
                           GetLastError());
                if (mapping) CloseHandle(mapping);
                return;
            }
            g_ring = static_cast<SkrFrameRing*>(view);
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            g_ring->capacity = kRingCapacity;
            g_ring->qpcFrequency = freq.QuadPart;
            g_ring->version = 1;
            MemoryBarrier();
            g_ring->magic = 0x46524B53u;
            Log::Write("PerfStats: publishing per-frame CPU time to Local\\SkrOverlay_FrameRing");
        }
        if (!g_ring)
            return;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const LONG64 n = g_ring->writeCount;
        SkrFrameEntry& e = g_ring->entries[static_cast<uint32_t>(n) & (kRingCapacity - 1)];
        e.qpc = now.QuadPart;
        e.cpuMs = static_cast<float>(cpuMs);
        InterlockedExchange64(&g_ring->writeCount, n + 1);   // full barrier: entry before count
    }

    double g_ramMB = 0.0;
    bool g_haveRam = false;

    double g_vramUsedMB = 0.0;
    double g_vramBudgetMB = 0.0;
    bool g_haveVram = false;
}

void PerfStats::Init(IDXGISwapChain* swapChain)
{
    if (g_adapter3) { g_adapter3->Release(); g_adapter3 = nullptr; }
    g_haveVram = false;
    g_haveLastThreadCycles = false;
    g_haveCpuFrameMs = false;
    if (!g_haveCycleCalibration)
        CalibrateCyclesPerMs();

    if (!swapChain)
        return;

    IDXGIDevice* dxgiDevice = nullptr;
    ID3D11Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || !device)
        return;

    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) && dxgiDevice)
    {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter)
        {
            if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&g_adapter3))))
            {
                Log::Write("PerfStats::Init: IDXGIAdapter3 available, VRAM readout enabled");
            }
            else
            {
                Log::Write("PerfStats::Init: IDXGIAdapter3 not available (old driver?), VRAM readout disabled");
            }
            adapter->Release();
        }
        dxgiDevice->Release();
    }

    device->Release();
}

void PerfStats::OnBeforePresent()
{
    if (!g_haveCycleCalibration)
        return;
    ULONGLONG cycles = 0;
    if (QueryThreadCycleTime(GetCurrentThread(), &cycles))
    {
        g_beforePresentCycles = cycles;
        g_haveBeforePresent = true;
    }
}

void PerfStats::OnFrame()
{
    // CPU frame time: hardware cycles THIS thread burned since the last
    // frame boundary, converted via the calibrated cycles/ms above. Called
    // from the Present hook, so this is whichever thread the game calls
    // Present on -- its main/render thread.
    if (!g_haveCycleCalibration)
        return;

    ULONGLONG cycles = 0;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles))
        return;

    if (g_haveLastThreadCycles)
    {
        // Cycles burned inside Present() (limiter busy-wait, driver work)
        // are not the game's own compute -- exclude them.
        ULONGLONG busyCycles = cycles - g_lastThreadCycles;
        if (g_haveBeforePresent && cycles >= g_beforePresentCycles)
        {
            const ULONGLONG inPresent = cycles - g_beforePresentCycles;
            busyCycles = (busyCycles > inPresent) ? busyCycles - inPresent : 0;
        }
        const double deltaMs = static_cast<double>(busyCycles) / g_cyclesPerMs;
        if (deltaMs >= 0.0 && deltaMs < 2000.0)
        {
            PushFrame(deltaMs);
            if (!g_haveCpuFrameMs)
            {
                g_cpuFrameMs = deltaMs;
                g_haveCpuFrameMs = true;
            }
            else
            {
                constexpr double kTauMs = 500.0;
                const double alpha = 1.0 - exp(-deltaMs / kTauMs);
                g_cpuFrameMs = g_cpuFrameMs * (1.0 - alpha) + deltaMs * alpha;
            }
            PublishCpuFrameMs();
        }
    }
    g_lastThreadCycles = cycles;
    g_haveLastThreadCycles = true;
    g_haveBeforePresent = false;
}

void PerfStats::Poll()
{
    // --- Process RAM (working set) ---
    PROCESS_MEMORY_COUNTERS pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
    {
        g_ramMB = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        g_haveRam = true;
    }

    // --- GPU VRAM ---
    if (g_adapter3)
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        if (SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        {
            g_vramUsedMB = static_cast<double>(info.CurrentUsage) / (1024.0 * 1024.0);
            g_vramBudgetMB = static_cast<double>(info.Budget) / (1024.0 * 1024.0);
            g_haveVram = true;
        }
        else
        {
            g_haveVram = false;
        }
    }
}

bool PerfStats::GetCpuFrameTimeMs(double& outMs)
{
    if (!g_haveCpuFrameMs)
        return false;
    outMs = g_cpuFrameMs;
    return true;
}

bool PerfStats::GetProcessRamMB(double& outMB)
{
    if (!g_haveRam)
        return false;
    outMB = g_ramMB;
    return true;
}

bool PerfStats::GetVramMB(double& outUsedMB, double& outBudgetMB)
{
    if (!g_haveVram)
        return false;
    outUsedMB = g_vramUsedMB;
    outBudgetMB = g_vramBudgetMB;
    return true;
}
