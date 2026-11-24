#include "PerfStats.h"
#include "GpuEngineUsage.h"
#include "AmdGpuPower.h"
#include "ReplayInfo.h"
#include "Config.h"
#include <atomic>
#include "Log.h"
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cwchar>
#include <vector>
#pragma comment(lib, "pdh.lib")
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <malloc.h>

// Counts the bytes this DLL's C++ code holds on the heap, for the "HUD RAM"
// stat. Replacing the global operator new/delete applies to this DLL only
// (each module links its own), so the game's memory is not counted.
//
// Each block carries its own size in a small header in front of it, so
// new/delete only add and subtract a number. This used to ask the heap for
// the size instead (_msize), once in new and again in delete -- a heap call
// on every allocation, for a figure shown twice a second.
//
// The header is 16 bytes, not 8: malloc hands back 16-byte aligned blocks on
// x64 and operator new promises the same (__STDCPP_DEFAULT_NEW_ALIGNMENT__),
// so the pointer given out after the header keeps that alignment. Over-
// aligned new (align_val_t) is not replaced: nothing here uses it, and the
// library's own versions pair with each other (_aligned_malloc/_aligned_free),
// never with these.
namespace
{
    std::atomic<long long> g_pluginHeapBytes{ 0 };
    constexpr size_t kHeapHeader = 16;
    static_assert(kHeapHeader >= sizeof(size_t) && kHeapHeader % alignof(std::max_align_t) == 0,
                  "the header must keep operator new's alignment");

    // Null on failure; the throwing forms below turn that into bad_alloc.
    void* CountedAlloc(size_t n) noexcept
    {
        if (n > static_cast<size_t>(-1) - kHeapHeader)
            return nullptr;
        unsigned char* base = static_cast<unsigned char*>(malloc(n + kHeapHeader));
        if (!base)
            return nullptr;
        *reinterpret_cast<size_t*>(base) = n + kHeapHeader;
        g_pluginHeapBytes.fetch_add(static_cast<long long>(n + kHeapHeader), std::memory_order_relaxed);
        return base + kHeapHeader;
    }

    void CountedFree(void* p) noexcept
    {
        if (!p) return;
        unsigned char* base = static_cast<unsigned char*>(p) - kHeapHeader;
        g_pluginHeapBytes.fetch_sub(static_cast<long long>(*reinterpret_cast<size_t*>(base)), std::memory_order_relaxed);
        free(base);
    }
}
void* operator new(size_t n)
{
    void* p = CountedAlloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
// The nothrow forms are replaced too, so every allocation this DLL makes has
// the header its delete expects.
void* operator new(size_t n, const std::nothrow_t&) noexcept { return CountedAlloc(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return CountedAlloc(n); }
void operator delete(void* p) noexcept { CountedFree(p); }
void operator delete[](void* p) noexcept { CountedFree(p); }
void operator delete(void* p, size_t) noexcept { CountedFree(p); }
void operator delete[](void* p, size_t) noexcept { CountedFree(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { CountedFree(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { CountedFree(p); }

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")

namespace
{
    // The VRAM adapter is replaced by Init on every rebind (render thread)
    // while the stats poller thread may be about to query it. The lock only
    // covers swapping and borrowing the pointer: the poller takes its own
    // reference under it and queries after letting go, so a rebind never
    // waits on a driver call and never frees an adapter mid-query.
    SRWLOCK g_adapterLock = SRWLOCK_INIT;
    IDXGIAdapter3* g_adapter3 = nullptr;   // guarded by g_adapterLock

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

    // Whole-card VRAM (poller thread only, apart from the published values).
    // "Dedicated Usage" is a plain byte count, so one collect per poll gives
    // a value -- no two-sample rate counter.
    PDH_HQUERY g_cardQuery = nullptr;
    PDH_HCOUNTER g_cardCounter = nullptr;
    bool g_cardQueryFailed = false;
    wchar_t g_cardLuid[40] = L"";       // "luid_0x%08X_0x%08X" of rF2's adapter
    double g_cardTotalMB = 0.0;
    std::vector<BYTE> g_cardBuf;
    std::atomic<double> g_cardUsedMBPub{0.0};
    std::atomic<double> g_cardTotalMBPub{0.0};
    std::atomic<bool> g_haveCardVram{false};

    // Sums "\GPU Adapter Memory(luid_..._phys_N)\Dedicated Usage" over the
    // instances of rF2's adapter. Returns false when PDH has no such data.
    bool SampleCardVram(double& outUsedMB)
    {
        if (g_cardQueryFailed || !g_cardLuid[0])
            return false;
        if (!g_cardQuery)
        {
            if (PdhOpenQueryW(nullptr, 0, &g_cardQuery) != ERROR_SUCCESS)
            {
                g_cardQuery = nullptr;
                g_cardQueryFailed = true;
                return false;
            }
            if (PdhAddEnglishCounterW(g_cardQuery, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &g_cardCounter) != ERROR_SUCCESS)
            {
                PdhCloseQuery(g_cardQuery);
                g_cardQuery = nullptr;
                g_cardQueryFailed = true;
                Log::Write("PerfStats: no GPU Adapter Memory counter -- whole-card VRAM not shown");
                return false;
            }
        }
        if (PdhCollectQueryData(g_cardQuery) != ERROR_SUCCESS)
            return false;
        DWORD size = 0, count = 0;
        PDH_STATUS st = PdhGetFormattedCounterArrayW(g_cardCounter, PDH_FMT_LARGE, &size, &count, nullptr);
        if (st != PDH_MORE_DATA || size == 0)
            return false;
        if (g_cardBuf.size() < size)
            g_cardBuf.resize(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(g_cardBuf.data());
        if (PdhGetFormattedCounterArrayW(g_cardCounter, PDH_FMT_LARGE, &size, &count, items) != ERROR_SUCCESS)
            return false;
        const size_t luidLen = wcslen(g_cardLuid);
        LONGLONG bytes = 0;
        bool found = false;
        for (DWORD i = 0; i < count; ++i)
        {
            if (!items[i].szName || _wcsnicmp(items[i].szName, g_cardLuid, luidLen) != 0)
                continue;
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
                continue;
            bytes += items[i].FmtValue.largeValue;
            found = true;
        }
        if (!found)
            return false;
        outUsedMB = static_cast<double>(bytes) / (1024.0 * 1024.0);
        return true;
    }
}

namespace
{
    // Puts 'adapter' (may be null) in place and releases the old one, outside
    // the lock -- the poller holds its own reference while it queries.
    void SwapAdapter(IDXGIAdapter3* adapter)
    {
        AcquireSRWLockExclusive(&g_adapterLock);
        IDXGIAdapter3* old = g_adapter3;
        g_adapter3 = adapter;
        ReleaseSRWLockExclusive(&g_adapterLock);
        if (old) old->Release();
    }
}

void PerfStats::Init(IDXGISwapChain* swapChain)
{
    SwapAdapter(nullptr);
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
            IDXGIAdapter3* adapter3 = nullptr;
            if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))) && adapter3)
            {
                SwapAdapter(adapter3);
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
    // Borrowed under the lock, queried outside it (see g_adapterLock).
    AcquireSRWLockShared(&g_adapterLock);
    IDXGIAdapter3* adapter = g_adapter3;
    if (adapter) adapter->AddRef();
    ReleaseSRWLockShared(&g_adapterLock);
    if (adapter)
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        {
            g_vramUsedMB = static_cast<double>(info.CurrentUsage) / (1024.0 * 1024.0);
            g_vramBudgetMB = static_cast<double>(info.Budget) / (1024.0 * 1024.0);
            g_haveVram = true;
        }
        else
        {
            g_haveVram = false;
        }
        // The adapter's identity and VRAM size, once (they do not change).
        if (!g_cardLuid[0])
        {
            DXGI_ADAPTER_DESC desc = {};
            if (SUCCEEDED(adapter->GetDesc(&desc)))
            {
                swprintf_s(g_cardLuid, L"luid_0x%08X_0x%08X",
                           static_cast<unsigned>(desc.AdapterLuid.HighPart), static_cast<unsigned>(desc.AdapterLuid.LowPart));
                g_cardTotalMB = static_cast<double>(desc.DedicatedVideoMemory) / (1024.0 * 1024.0);
                Log::Write("PerfStats: graphics card %ls, %.0f MB dedicated VRAM", desc.Description, g_cardTotalMB);
            }
        }
        adapter->Release();

        double cardUsedMB = 0.0;
        if (g_cardTotalMB > 0.0 && SampleCardVram(cardUsedMB))
        {
            g_cardUsedMBPub.store(cardUsedMB, std::memory_order_relaxed);
            g_cardTotalMBPub.store(g_cardTotalMB, std::memory_order_relaxed);
            g_haveCardVram.store(true, std::memory_order_release);
        }
        else
        {
            g_haveCardVram.store(false, std::memory_order_release);
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

bool PerfStats::GetCardVramMB(double& outUsedMB, double& outTotalMB)
{
    if (!g_haveCardVram.load(std::memory_order_acquire))
        return false;
    outUsedMB = g_cardUsedMBPub.load(std::memory_order_relaxed);
    outTotalMB = g_cardTotalMBPub.load(std::memory_order_relaxed);
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

void PerfStatsSamplePcRam();

namespace
{
    HANDLE g_pollThread = nullptr;
    std::atomic<bool> g_pollStop{ false };

    DWORD WINAPI PollThreadProc(LPVOID)
    {
        while (!g_pollStop.load())
        {
            PerfStats::Poll();
            PerfStatsSamplePcRam();
            GpuEngineUsage::Poll();
            AmdGpuPower::Sample();
            // File-system lookups (player.JSON, the replay folder): here, not
            // on the render thread.
            if (g_frameLatencyConfig.showReplay)
                ReplayInfo::Poll();
            const float hz = g_perfUpdateConfig.updateHz > 0.1f ? g_perfUpdateConfig.updateHz : 2.0f;
            const DWORD waitMs = static_cast<DWORD>(1000.0f / hz);
            for (DWORD slept = 0; slept < waitMs && !g_pollStop.load(); slept += 50)
                Sleep(50);
        }
        return 0;
    }
}

void PerfStats::StartPoller()
{
    if (g_pollThread) return;
    g_pollStop.store(false);
    g_pollThread = CreateThread(nullptr, 0, PollThreadProc, nullptr, 0, nullptr);
}

void PerfStats::StopPoller()
{
    if (!g_pollThread) return;
    g_pollStop.store(true);
    WaitForSingleObject(g_pollThread, 2000);
    CloseHandle(g_pollThread);
    g_pollThread = nullptr;
}

namespace
{
    std::atomic<double> g_pcRamUsedMB{ 0.0 }, g_pcRamTotalMB{ 0.0 };
    std::atomic<bool> g_havePcRam{ false };
    double g_imageMB = -1.0;
}

void PerfStatsSamplePcRam()
{
    MEMORYSTATUSEX ms = {};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
    {
        g_pcRamTotalMB.store(static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024.0), std::memory_order_relaxed);
        g_pcRamUsedMB.store(static_cast<double>(ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0), std::memory_order_relaxed);
        g_havePcRam.store(true, std::memory_order_relaxed);
    }
}

bool PerfStats::GetPcRamMB(double& outUsedMB, double& outTotalMB)
{
    if (!g_havePcRam.load(std::memory_order_relaxed)) return false;
    outUsedMB = g_pcRamUsedMB.load(std::memory_order_relaxed);
    outTotalMB = g_pcRamTotalMB.load(std::memory_order_relaxed);
    return true;
}

bool PerfStats::GetPluginRamMB(double& outMB)
{
    if (g_imageMB < 0.0)
    {
        HMODULE self = nullptr;
        MODULEINFO mi = {};
        g_imageMB = 0.0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&PerfStats::GetPluginRamMB), &self)
            && GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof(mi)))
            g_imageMB = static_cast<double>(mi.SizeOfImage) / (1024.0 * 1024.0);
    }
    outMB = g_imageMB + static_cast<double>(g_pluginHeapBytes.load(std::memory_order_relaxed)) / (1024.0 * 1024.0);
    return true;
}
