#include "Profiler.h"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include <atomic>
#include <cstdio>
#include <cstring>

namespace
{
    const int kMaxSlots = 48;
    // 1 s snapshots; 11 of them span the 10 s window.
    const int kSnapshots = 11;

    // Statically initialised, so first use from any thread is safe.
    SRWLOCK g_registerLock = SRWLOCK_INIT;
    const char* g_names[kMaxSlots] = {};
    std::atomic<int> g_slotCount{ 0 };
    std::atomic<long long> g_ticks[kMaxSlots];
    // Ticks spent on the render thread, and frames, for the per-frame figure.
    std::atomic<long long> g_renderTicks{ 0 };
    std::atomic<DWORD> g_renderThreadId{ 0 };
    long long g_frames = 0; // render thread only

    struct Snapshot
    {
        long long qpc = 0;
        long long ticks[kMaxSlots] = {};
        long long renderTicks = 0;
        long long frames = 0;
    };
    Snapshot g_snaps[kSnapshots];
    int g_snapCount = 0;
    int g_snapNext = 0;
    LARGE_INTEGER g_freq = {};

    // Computed once a second from the snapshots, drawn every frame.
    struct Shown
    {
        bool valid = false;
        double totalMsPerSec = 0.0;
        double renderMsPerFrame = 0.0;
        int count = 0;
        int slot[kMaxSlots] = {};
        double msPerSec[kMaxSlots] = {};
    } g_shown;

    // Distinct, readable on a dark plate; cycled by slot.
    const float kPalette[][3] =
    {
        { 0.30f, 0.65f, 1.00f }, { 1.00f, 0.60f, 0.20f }, { 0.40f, 0.85f, 0.40f },
        { 0.90f, 0.35f, 0.40f }, { 0.70f, 0.50f, 0.95f }, { 0.95f, 0.85f, 0.30f },
        { 0.30f, 0.85f, 0.85f }, { 0.95f, 0.50f, 0.80f }, { 0.65f, 0.65f, 0.65f },
        { 0.60f, 0.80f, 0.30f },
    };
    const int kPaletteCount = sizeof(kPalette) / sizeof(kPalette[0]);

    void TakeSnapshot(long long qpc)
    {
        Snapshot& s = g_snaps[g_snapNext];
        s.qpc = qpc;
        const int n = g_slotCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            s.ticks[i] = g_ticks[i].load(std::memory_order_relaxed);
        s.renderTicks = g_renderTicks.load(std::memory_order_relaxed);
        s.frames = g_frames;
        g_snapNext = (g_snapNext + 1) % kSnapshots;
        if (g_snapCount < kSnapshots) ++g_snapCount;

        if (g_snapCount < 2)
            return;
        const Snapshot& newest = s;
        const Snapshot& oldest = g_snaps[(g_snapNext + kSnapshots - g_snapCount) % kSnapshots];
        const double seconds = static_cast<double>(newest.qpc - oldest.qpc) / static_cast<double>(g_freq.QuadPart);
        if (seconds <= 0.0)
            return;
        const double msPerTick = 1000.0 / static_cast<double>(g_freq.QuadPart);

        Shown shown;
        shown.valid = true;
        for (int i = 0; i < n; ++i)
        {
            const double ms = static_cast<double>(newest.ticks[i] - oldest.ticks[i]) * msPerTick / seconds;
            shown.totalMsPerSec += ms;
            if (ms <= 0.0)
                continue;
            // Biggest first: insertion into the (short) sorted list.
            int at = shown.count++;
            while (at > 0 && shown.msPerSec[at - 1] < ms)
            {
                shown.msPerSec[at] = shown.msPerSec[at - 1];
                shown.slot[at] = shown.slot[at - 1];
                --at;
            }
            shown.msPerSec[at] = ms;
            shown.slot[at] = i;
        }
        const long long frames = newest.frames - oldest.frames;
        if (frames > 0)
            shown.renderMsPerFrame = static_cast<double>(newest.renderTicks - oldest.renderTicks) * msPerTick / frames;
        g_shown = shown;
    }
}

int Profiler::Slot(const char* name)
{
    AcquireSRWLockExclusive(&g_registerLock);
    const int n = g_slotCount.load(std::memory_order_relaxed);
    int slot = -1;
    for (int i = 0; i < n; ++i)
        if (strcmp(g_names[i], name) == 0) { slot = i; break; }
    if (slot < 0 && n < kMaxSlots)
    {
        g_names[n] = name; // string literals only (see PROF)
        g_ticks[n].store(0, std::memory_order_relaxed);
        slot = n;
        g_slotCount.store(n + 1, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_registerLock);
    return slot;
}

Profiler::Scope::Scope(int slot) : m_slot(slot)
{
    QueryPerformanceCounter(&m_start);
}

Profiler::Scope::~Scope()
{
    Stop();
}

void Profiler::Scope::Stop()
{
    if (m_slot < 0)
        return;
    const int slot = m_slot;
    m_slot = -1;
    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    const long long ticks = end.QuadPart - m_start.QuadPart;
    g_ticks[slot].fetch_add(ticks, std::memory_order_relaxed);
    if (GetCurrentThreadId() == g_renderThreadId.load(std::memory_order_relaxed))
        g_renderTicks.fetch_add(ticks, std::memory_order_relaxed);
}

void Profiler::FrameTick()
{
    g_renderThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    ++g_frames;
}

void Profiler::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    PROF("DLL cost panel");
    const DllCostConfig& cfg = g_dllCostConfig;
    if (g_freq.QuadPart == 0)
        QueryPerformanceFrequency(&g_freq);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const long long last = g_snapCount > 0 ? g_snaps[(g_snapNext + kSnapshots - 1) % kSnapshots].qpc : 0;
    if (g_snapCount == 0 || now.QuadPart - last >= g_freq.QuadPart)
        TakeSnapshot(now.QuadPart);

    const float uiScale = static_cast<float>(screenHeight) / 2160.0f * cfg.scale;
    const float textPx = cfg.textHeightPx * uiScale;
    const float barW = cfg.barWidthPx * uiScale;
    const float barH = cfg.barHeightPx * uiScale;
    const float pad = 10.0f * uiScale;
    const float lineGap = textPx * 0.35f;

    const float left = screenWidth * cfg.posXFraction;
    const float top = screenHeight * cfg.posYFraction;
    const float panelW = barW + pad * 2.0f;
    const float panelH = pad + textPx + lineGap + barH + pad;

    Layout::Register("DLL cost", "DllCost", "PosXFraction", "PosYFraction",
                     &g_dllCostConfig.posXFraction, &g_dllCostConfig.posYFraction,
                     left + panelW * 0.5f, top + panelH * 0.5f, panelW * 0.5f, panelH * 0.5f,
                     &g_dllCostConfig.scale, "Scale");

    const bool prevTextBg = renderer.TextBackgroundEnabled();
    renderer.SetTextBackground(false);

    renderer.AddFilledRoundedRect(left + panelW * 0.5f, top + panelH * 0.5f, panelW * 0.5f, panelH * 0.5f,
                                  6.0f * uiScale, 0.0f, 0.08f, 0.08f, 0.08f, 0.6f);

    char header[128];
    if (!g_shown.valid)
        strcpy_s(header, "DLL cost: measuring...");
    else
        sprintf_s(header, "DLL %.2f ms/s (%.2f%% of a core)   render %.3f ms/frame   10 s avg",
                  g_shown.totalMsPerSec, g_shown.totalMsPerSec / 10.0, g_shown.renderMsPerFrame);
    renderer.AddText(left + pad, top + pad, header, textPx, 0.85f, 0.85f, 0.85f, 1.0f);

    // The bar: the whole width is the DLL's total, split by module, biggest
    // on the left. Only shares above labelMinShare get a name.
    const float barTop = top + pad + textPx + lineGap;
    const float barLeft = left + pad;
    renderer.AddFilledRect(barLeft + barW * 0.5f, barTop + barH * 0.5f, barW * 0.5f, barH * 0.5f, 0.0f,
                           0.25f, 0.25f, 0.28f, 0.8f);
    if (g_shown.valid && g_shown.totalMsPerSec > 0.0)
    {
        const float labelPx = barH * 0.62f;
        float x = barLeft;
        for (int k = 0; k < g_shown.count; ++k)
        {
            const double share = g_shown.msPerSec[k] / g_shown.totalMsPerSec;
            const float w = static_cast<float>(share) * barW;
            if (w <= 0.0f)
                continue;
            const float* c = kPalette[g_shown.slot[k] % kPaletteCount];
            renderer.AddFilledRect(x + w * 0.5f, barTop + barH * 0.5f, w * 0.5f, barH * 0.5f, 0.0f,
                                   c[0] * 0.75f, c[1] * 0.75f, c[2] * 0.75f, 0.95f);
            if (share >= cfg.labelMinShare)
            {
                char label[64];
                sprintf_s(label, "%s %.0f%%", g_names[g_shown.slot[k]], share * 100.0);
                if (renderer.MeasureText(label, labelPx) > w - 6.0f * uiScale)
                    sprintf_s(label, "%s", g_names[g_shown.slot[k]]);
                if (renderer.MeasureText(label, labelPx) <= w - 6.0f * uiScale)
                    renderer.AddText(x + w * 0.5f, barTop + (barH - labelPx) * 0.5f, label, labelPx,
                                     0.05f, 0.05f, 0.05f, 1.0f, Renderer::HAlign::Center);
            }
            x += w;
        }
    }

    renderer.SetTextBackground(prevTextBg);
}
