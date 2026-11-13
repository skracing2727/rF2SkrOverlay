#include "GpuEngineUsage.h"
#include "Log.h"
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <vector>
#include <string>
#include <cstring>

#pragma comment(lib, "pdh.lib")

namespace
{
    PDH_HQUERY g_query = nullptr;
    std::vector<PDH_HCOUNTER> g_counters;

    LARGE_INTEGER g_freq = {};
    LARGE_INTEGER g_lastExpand = {};
    constexpr double kReExpandSeconds = 5.0;

    double g_busyPercent = 0.0;
    bool g_havePercent = false;

    void ClearCounters()
    {
        for (PDH_HCOUNTER c : g_counters)
            PdhRemoveCounter(c);
        g_counters.clear();
    }

    // Re-expands "\GPU Engine(*)\Utilization Percentage" and (re)adds one
    // counter per instance whose name contains "engtype_3D" -- the render
    // engine, same one Task Manager's default GPU graph tracks. Called
    // fresh periodically since engine instances appear/disappear as other
    // processes start GPU work.
    void ExpandAndAddCounters()
    {
        if (!g_query)
            return;

        ClearCounters();

        const char* wildcardPath = "\\GPU Engine(*)\\Utilization Percentage";
        DWORD pathListSize = 0;
        PDH_STATUS status = PdhExpandWildCardPathA(nullptr, wildcardPath, nullptr, &pathListSize, 0);
        if (status != PDH_MORE_DATA || pathListSize == 0)
        {
            Log::Write("GpuEngineUsage: PdhExpandWildCardPathA (size query) failed, status 0x%08X", status);
            return;
        }

        std::vector<char> pathList(pathListSize);
        status = PdhExpandWildCardPathA(nullptr, wildcardPath, pathList.data(), &pathListSize, 0);
        if (status != ERROR_SUCCESS)
        {
            Log::Write("GpuEngineUsage: PdhExpandWildCardPathA (fill) failed, status 0x%08X", status);
            return;
        }

        int added = 0;
        for (const char* p = pathList.data(); *p != '\0'; p += strlen(p) + 1)
        {
            if (strstr(p, "engtype_3D") == nullptr)
                continue;

            PDH_HCOUNTER counter = nullptr;
            if (PdhAddEnglishCounterA(g_query, p, 0, &counter) == ERROR_SUCCESS && counter)
            {
                g_counters.push_back(counter);
                ++added;
            }
        }

        // Fresh counters are difference-based and return no valid value until
        // a baseline sample exists -- take it now so the next Poll is valid
        // instead of the readout dropping out for one refresh.
        PdhCollectQueryData(g_query);
        Log::Write("GpuEngineUsage: expanded %d engtype_3D counter instance(s)", added);
    }
}

void GpuEngineUsage::Init()
{
    if (g_query)
        return; // already initialized

    if (g_freq.QuadPart == 0)
        QueryPerformanceFrequency(&g_freq);

    if (PdhOpenQueryA(nullptr, 0, &g_query) != ERROR_SUCCESS || !g_query)
    {
        Log::Write("GpuEngineUsage::Init: PdhOpenQuery failed");
        g_query = nullptr;
        return;
    }

    ExpandAndAddCounters();
    QueryPerformanceCounter(&g_lastExpand);

    // First PdhCollectQueryData call establishes the baseline for the
    // "Utilization Percentage" counter type (it's a difference-based
    // counter); throw its result away.
    PdhCollectQueryData(g_query);
}

void GpuEngineUsage::Shutdown()
{
    if (g_query)
    {
        PdhCloseQuery(g_query);
        g_query = nullptr;
    }
    g_counters.clear();
    g_havePercent = false;
}

void GpuEngineUsage::Poll()
{
    if (!g_query)
        return;

    if (g_freq.QuadPart != 0)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double sinceExpandSeconds = static_cast<double>(now.QuadPart - g_lastExpand.QuadPart) / static_cast<double>(g_freq.QuadPart);
        if (sinceExpandSeconds >= kReExpandSeconds)
        {
            ExpandAndAddCounters();
            g_lastExpand = now;
            // The baseline sample was just taken; sampling again microseconds
            // later would give a near-zero interval. Keep the last value.
            return;
        }
    }

    // On any transient failure below, keep the last good value rather than
    // clearing it -- a single bad sample must not make the stat vanish.
    if (g_counters.empty())
        return;

    if (PdhCollectQueryData(g_query) != ERROR_SUCCESS)
        return;

    double sum = 0.0;
    bool any = false;
    for (PDH_HCOUNTER counter : g_counters)
    {
        PDH_FMT_COUNTERVALUE value;
        DWORD type = 0;
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, &type, &value) == ERROR_SUCCESS
            && value.CStatus == ERROR_SUCCESS)
        {
            sum += value.doubleValue;
            any = true;
        }
    }

    if (any)
    {
        g_busyPercent = sum;
        g_havePercent = true;
    }
}

bool GpuEngineUsage::GetBusyPercent(double& outPercent)
{
    if (!g_havePercent)
        return false;
    outPercent = g_busyPercent;
    return true;
}
