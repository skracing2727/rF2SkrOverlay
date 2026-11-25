#include <atomic>
#include "AmdGpuPower.h"
#include "Log.h"
#include "PerfStats.h"
#include <windows.h>
#include <cstdlib>
#include <initializer_list>

namespace
{
    // --- Minimal ADL2 type/struct definitions, matching AMD's public ADL
    // SDK exactly (GPUOpen-LibrariesAndSDKs/display-library,
    // include/adl_sdk.h, adl_structures.h, adl_defines.h) -- only the
    // handful of fixed-layout pieces actually needed here, verified against
    // that header rather than reconstructed from memory, since a wrong
    // struct layout here would corrupt memory inside the game process.
    using ADL_CONTEXT_HANDLE = void*;
    using ADL_MAIN_MALLOC_CALLBACK = void*(__stdcall*)(int);

    constexpr int kAdlOk = 0;
    constexpr int kAdlPmLogMaxSensors = 256;

    // ADLSensorType ids (adl_defines.h) -- power sensors only, tried in this
    // preference order (most GPUs report at least one of these).
    constexpr unsigned short kPmLogSocPower = 17;
    constexpr unsigned short kPmLogAsicPower = 23;   // "Current ASIC power draw in Watt"
    constexpr unsigned short kPmLogGfxPower = 30;
    constexpr unsigned short kPmLogBoardPower = 73;

    #pragma pack(push, 8)
    struct ADLSingleSensorData
    {
        int supported;
        int value;
    };
    struct ADLPMLogDataOutput
    {
        int size;
        ADLSingleSensorData sensors[kAdlPmLogMaxSensors];
    };
    #pragma pack(pop)

    using Fn_ADL2_Main_Control_Create = int(__stdcall*)(ADL_MAIN_MALLOC_CALLBACK, int, ADL_CONTEXT_HANDLE*);
    using Fn_ADL2_Main_Control_Destroy = int(__stdcall*)(ADL_CONTEXT_HANDLE);
    using Fn_ADL2_Adapter_NumberOfAdapters_Get = int(__stdcall*)(ADL_CONTEXT_HANDLE, int*);
    using Fn_ADL2_Adapter_Active_Get = int(__stdcall*)(ADL_CONTEXT_HANDLE, int, int*);
    using Fn_ADL2_New_QueryPMLogData_Get = int(__stdcall*)(ADL_CONTEXT_HANDLE, int, ADLPMLogDataOutput*);

    void* __stdcall AdlAlloc(int size) { return malloc(size); }

    HMODULE g_adlDll = nullptr;
    ADL_CONTEXT_HANDLE g_context = nullptr;
    int g_activeAdapterIndex = -1;
    unsigned short g_powerSensorId = 0;
    bool g_haveSensor = false;

    Fn_ADL2_Main_Control_Create g_Create = nullptr;
    Fn_ADL2_Main_Control_Destroy g_Destroy = nullptr;
    Fn_ADL2_Adapter_NumberOfAdapters_Get g_NumAdapters = nullptr;
    Fn_ADL2_Adapter_Active_Get g_AdapterActive = nullptr;
    Fn_ADL2_New_QueryPMLogData_Get g_QueryPMLog = nullptr;
}

bool AmdGpuPower::Init()
{
    if (g_context)
        return true; // already initialized

    g_adlDll = LoadLibraryA("atiadlxx.dll");
    if (!g_adlDll)
    {
        Log::Write("AmdGpuPower::Init: atiadlxx.dll not found (not an AMD system, or driver not installed)");
        return false;
    }

    g_Create = reinterpret_cast<Fn_ADL2_Main_Control_Create>(GetProcAddress(g_adlDll, "ADL2_Main_Control_Create"));
    g_Destroy = reinterpret_cast<Fn_ADL2_Main_Control_Destroy>(GetProcAddress(g_adlDll, "ADL2_Main_Control_Destroy"));
    g_NumAdapters = reinterpret_cast<Fn_ADL2_Adapter_NumberOfAdapters_Get>(GetProcAddress(g_adlDll, "ADL2_Adapter_NumberOfAdapters_Get"));
    g_AdapterActive = reinterpret_cast<Fn_ADL2_Adapter_Active_Get>(GetProcAddress(g_adlDll, "ADL2_Adapter_Active_Get"));
    g_QueryPMLog = reinterpret_cast<Fn_ADL2_New_QueryPMLogData_Get>(GetProcAddress(g_adlDll, "ADL2_New_QueryPMLogData_Get"));

    if (!g_Create || !g_Destroy || !g_NumAdapters || !g_AdapterActive || !g_QueryPMLog)
    {
        Log::Write("AmdGpuPower::Init: atiadlxx.dll missing expected exports, giving up");
        FreeLibrary(g_adlDll);
        g_adlDll = nullptr;
        return false;
    }

    if (g_Create(&AdlAlloc, 1, &g_context) != kAdlOk || !g_context)
    {
        Log::Write("AmdGpuPower::Init: ADL2_Main_Control_Create failed");
        g_context = nullptr;
        FreeLibrary(g_adlDll);
        g_adlDll = nullptr;
        return false;
    }

    int numAdapters = 0;
    if (g_NumAdapters(g_context, &numAdapters) != kAdlOk || numAdapters <= 0)
    {
        Log::Write("AmdGpuPower::Init: no ADL adapters reported");
        Shutdown();
        return false;
    }

    // Pick the active adapter with the best power sensor (ASIC/board over
    // GFX over SOC), not just the first one found: on a Ryzen-iGPU + Radeon
    // desktop the first may be the APU (review F183). Which power sensor a
    // GPU reports varies by generation/driver.
    const unsigned short kCandidates[] = { kPmLogAsicPower, kPmLogBoardPower, kPmLogGfxPower, kPmLogSocPower };
    int bestRank = 99;
    for (int i = 0; i < numAdapters; ++i)
    {
        int status = 0;
        if (g_AdapterActive(g_context, i, &status) != kAdlOk || status != 1)
            continue;

        ADLPMLogDataOutput data = {};
        data.size = sizeof(data);
        if (g_QueryPMLog(g_context, i, &data) != kAdlOk)
            continue;

        for (int rank = 0; rank < 4 && rank < bestRank; ++rank)
        {
            const unsigned short candidate = kCandidates[rank];
            if (candidate < kAdlPmLogMaxSensors && data.sensors[candidate].supported)
            {
                bestRank = rank;
                g_activeAdapterIndex = i;
                g_powerSensorId = candidate;
                g_haveSensor = true;
                break;
            }
        }
    }
    if (g_haveSensor)
        Log::Write("AmdGpuPower::Init: adapter %d, power sensor id %u", g_activeAdapterIndex, g_powerSensorId);

    if (!g_haveSensor)
    {
        Log::Write("AmdGpuPower::Init: active AMD adapter found, but no power sensor reported by this driver/GPU");
        Shutdown();
        return false;
    }

    return true;
}

void AmdGpuPower::Shutdown()
{
    // The poller did not stop in time and may still be inside ADL: keep the
    // library loaded (review F231, see PerfStats::StopPoller).
    if (PerfStats::PollerStuck())
        return;
    if (g_context && g_Destroy)
        g_Destroy(g_context);
    g_context = nullptr;
    g_haveSensor = false;
    g_activeAdapterIndex = -1;

    if (g_adlDll)
        FreeLibrary(g_adlDll);
    g_adlDll = nullptr;

    g_Create = nullptr;
    g_Destroy = nullptr;
    g_NumAdapters = nullptr;
    g_AdapterActive = nullptr;
    g_QueryPMLog = nullptr;
}

namespace
{
    std::atomic<double> g_cachedWatts{ 0.0 };
    std::atomic<bool> g_haveCachedWatts{ false };
}
static bool QueryPowerWatts(double& outWatts);

void AmdGpuPower::Sample()
{
    double w = 0.0;
    // Only when rF2 renders on an AMD card: on a Ryzen laptop with an NVIDIA
    // dGPU the ADL sensor is the APU's, not the card's (review F183).
    const unsigned vendor = PerfStats::GetCardVendorId();   // 0 until known
    const bool ok = vendor == 0x1002 && QueryPowerWatts(w);
    if (ok) g_cachedWatts.store(w, std::memory_order_relaxed);
    g_haveCachedWatts.store(ok, std::memory_order_relaxed);
}

bool AmdGpuPower::GetPowerWatts(double& outWatts)
{
    if (!g_haveCachedWatts.load(std::memory_order_relaxed)) return false;
    outWatts = g_cachedWatts.load(std::memory_order_relaxed);
    return true;
}

static bool QueryPowerWatts(double& outWatts)
{
    if (!g_context || !g_haveSensor || !g_QueryPMLog)
        return false;

    ADLPMLogDataOutput data = {};
    data.size = sizeof(data);
    if (g_QueryPMLog(g_context, g_activeAdapterIndex, &data) != kAdlOk)
        return false;

    if (!data.sensors[g_powerSensorId].supported)
        return false;

    // PMLog power sensors report whole watts as a plain integer.
    outWatts = static_cast<double>(data.sensors[g_powerSensorId].value);
    return true;
}
