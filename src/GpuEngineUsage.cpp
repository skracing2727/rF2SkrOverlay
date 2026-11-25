#include "GpuEngineUsage.h"
#include "PerfStats.h"
#include "Log.h"
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <vector>
#include <string>
#include <cwchar>
#include <cstdio>

#pragma comment(lib, "pdh.lib")

namespace
{
    // One wildcard counter, "\GPU Engine(*)\Utilization Percentage", added by
    // its ENGLISH name (PdhAddEnglishCounterW works on every Windows
    // language) and read with PdhGetFormattedCounterArrayW -- the same
    // pattern as PerfStats' card VRAM. The old code expanded the English
    // path with PdhExpandWildCardPathA, which only takes localized names, so
    // the GPU ms stat never appeared on non-English Windows and the failure
    // was logged every 5 s (review F178). A wildcard counter picks up engine
    // instances as they come and go, so no periodic re-expansion is needed.
    PDH_HQUERY g_query = nullptr;
    PDH_HCOUNTER g_counter = nullptr;
    std::vector<BYTE> g_buf;

    double g_busyPercent = 0.0;
    bool g_havePercent = false;
    bool g_loggedSource = false;

    struct Engine { std::wstring key; double percent; };

    bool ContainsNoCase(const wchar_t* hay, const wchar_t* needle)
    {
        const size_t n = wcslen(needle);
        for (; *hay; ++hay)
            if (_wcsnicmp(hay, needle, n) == 0)
                return true;
        return false;
    }
}

void GpuEngineUsage::Init()
{
    if (g_query)
        return; // already initialized

    if (PdhOpenQueryW(nullptr, 0, &g_query) != ERROR_SUCCESS || !g_query)
    {
        Log::Write("GpuEngineUsage::Init: PdhOpenQuery failed");
        g_query = nullptr;
        return;
    }
    const PDH_STATUS st = PdhAddEnglishCounterW(g_query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &g_counter);
    if (st != ERROR_SUCCESS)
    {
        // Logged once; the stat just stays unavailable.
        Log::Write("GpuEngineUsage::Init: no GPU Engine counter (status 0x%08X) -- GPU ms not shown", st);
        PdhCloseQuery(g_query);
        g_query = nullptr;
        g_counter = nullptr;
        return;
    }

    // First collect is the baseline for this difference-based counter.
    PdhCollectQueryData(g_query);
}

void GpuEngineUsage::Rebaseline()
{
    // Poll is skipped while the GPU ms stat is off; a fresh baseline keeps
    // the first value after turning it on from averaging the idle stretch
    // (review F181).
    if (g_query && g_counter)
        PdhCollectQueryData(g_query);
}

void GpuEngineUsage::Shutdown()
{
    // The poller did not stop in time and may still be inside PDH: leave the
    // query alone (the module is pinned, see PerfStats::StopPoller; review F231).
    if (PerfStats::PollerStuck())
        return;
    if (g_query)
    {
        PdhCloseQuery(g_query);
        g_query = nullptr;
    }
    g_counter = nullptr;
    g_havePercent = false;
}

void GpuEngineUsage::Poll()
{
    if (!g_query || !g_counter)
        return;

    // On any transient failure below, keep the last good value rather than
    // clearing it -- a single bad sample must not make the stat vanish.
    if (PdhCollectQueryData(g_query) != ERROR_SUCCESS)
        return;

    DWORD size = 0, count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(g_counter, PDH_FMT_DOUBLE, &size, &count, nullptr);
    if (st != PDH_MORE_DATA || size == 0)
        return;
    if (g_buf.size() < size)
        g_buf.resize(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(g_buf.data());
    if (PdhGetFormattedCounterArrayW(g_counter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS)
        return;

    // Instance names look like
    //   pid_1234_luid_0x00000000_0x0000D1C2_phys_0_eng_0_engtype_3D
    // Only 3D engines of rF2's own adapter count (review F411: an iGPU's
    // engines used to be added to the dGPU's). Per engine, the processes'
    // shares are summed; the busiest engine is the figure, as Task Manager
    // does, clamped to 0-100. Own process first, every process if no
    // instance carries this process's id.
    wchar_t ownPid[32];
    swprintf_s(ownPid, L"pid_%lu_", GetCurrentProcessId());
    const size_t ownPidLen = wcslen(ownPid);
    const wchar_t* luid = PerfStats::GetCardLuid();   // empty until known

    // Filters, strictest first: own process on rF2's adapter, any process
    // on it, then the same without the adapter filter (in case the adapter
    // id is not known yet or names it differently).
    std::vector<Engine> engines;
    bool ownFound = false;
    bool luidUsed = false;
    for (int pass = 0; pass < 4 && engines.empty(); ++pass)
    {
        const bool needOwn = (pass % 2) == 0;
        const bool needLuid = pass < 2;
        if (needLuid && !luid[0])
            continue;
        for (DWORD i = 0; i < count; ++i)
        {
            const wchar_t* name = items[i].szName;
            if (!name || !ContainsNoCase(name, L"engtype_3D"))
                continue;
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
                continue;
            if (needLuid && !ContainsNoCase(name, luid))
                continue;
            if (needOwn && _wcsnicmp(name, ownPid, ownPidLen) != 0)
                continue;
            // Engine key: everything from "luid_" on (drops "pid_<n>_").
            const wchar_t* key = wcsstr(name, L"luid_");
            const std::wstring k = key ? key : name;
            bool merged = false;
            for (Engine& e : engines)
                if (e.key == k) { e.percent += items[i].FmtValue.doubleValue; merged = true; break; }
            if (!merged)
                engines.push_back({ k, items[i].FmtValue.doubleValue });
        }
        ownFound = needOwn;
        luidUsed = needLuid;
    }
    if (engines.empty())
        return;

    double busiest = 0.0;
    for (const Engine& e : engines)
        if (e.percent > busiest) busiest = e.percent;
    if (busiest < 0.0) busiest = 0.0;
    if (busiest > 100.0) busiest = 100.0;

    if (!g_loggedSource)
    {
        g_loggedSource = true;
        Log::Write("GpuEngineUsage: reading %d engtype_3D engine(s)%s%s", static_cast<int>(engines.size()),
                   ownFound ? " of this process" : " (all processes)", luidUsed ? " on rF2's adapter" : "");
    }
    g_busyPercent = busiest;
    g_havePercent = true;
}

bool GpuEngineUsage::GetBusyPercent(double& outPercent)
{
    if (!g_havePercent)
        return false;
    outPercent = g_busyPercent;
    return true;
}
