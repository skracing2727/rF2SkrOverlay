#include "Profiler.h"
#include "RenderThrottle.h"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "Log.h"
#include <algorithm>
#include <vector>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace
{
    const int kMaxSlots = 64;   // more PROF/DRAWN names than 48 exist (review F347)
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
    // Render-thread ms of each recent frame, for the 99.9% figure: a ring
    // big enough for 10 s at ~400 fps.
    const int kFrameRing = 4096;
    float g_frameRing[kFrameRing] = {};
    int g_frameRingNext = 0, g_frameRingCount = 0;
    long long g_lastFrameRenderTicks = 0;
    LARGE_INTEGER g_freq = {};

    // Spike log (DllCostConfig::logSpikes): render-thread ticks per module in
    // the frame being built, and a 10 s window of per-frame figures.
    long long g_frameSlotTicks[kMaxSlots] = {};   // render thread only
    struct SpikeWindow
    {
        long long startQpc = 0;
        std::vector<float> frameMs;               // DLL render-thread ms per frame
        double sumMs[kMaxSlots] = {};
        float maxMs[kMaxSlots] = {};
        int framesOver[kMaxSlots] = {};           // frames this module alone took > 0.5 ms
        struct Worst { float ms; float slotMs[kMaxSlots]; };
        Worst worst[3] = {};                      // the three costliest frames
    } g_win;
    // Scratch copy of g_win.frameMs for the percentiles. Both vectors keep
    // their capacity from one window to the next, so the report every 10 s
    // allocates nothing on the render thread once the first window is in.
    std::vector<float> g_spikeScratch;

    // Starts the next 10 s window, keeping frameMs' storage.
    void ResetSpikeWindow(long long startQpc)
    {
        g_win.startQpc = startQpc;
        g_win.frameMs.clear();
        for (int i = 0; i < kMaxSlots; ++i)
        {
            g_win.sumMs[i] = 0.0;
            g_win.maxMs[i] = 0.0f;
            g_win.framesOver[i] = 0;
        }
        for (SpikeWindow::Worst& w : g_win.worst)
            w = SpikeWindow::Worst{};
    }

    void EndFrameForSpikes()
    {
        if (!g_dllCostConfig.logSpikes || g_freq.QuadPart == 0) return;
        const double msPerTick = 1000.0 / static_cast<double>(g_freq.QuadPart);
        const int n = g_slotCount.load(std::memory_order_acquire);
        float slotMs[kMaxSlots] = {};
        float total = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            slotMs[i] = static_cast<float>(g_frameSlotTicks[i] * msPerTick);
            g_frameSlotTicks[i] = 0;
            total += slotMs[i];
            g_win.sumMs[i] += slotMs[i];
            if (slotMs[i] > g_win.maxMs[i]) g_win.maxMs[i] = slotMs[i];
            if (slotMs[i] > 0.5f) ++g_win.framesOver[i];
        }
        // ~1400 frames per window at 140 fps; reserved once rather than grown.
        if (g_win.frameMs.capacity() == 0)
            g_win.frameMs.reserve(4096);
        g_win.frameMs.push_back(total);
        for (int w = 0; w < 3; ++w)
            if (total > g_win.worst[w].ms)
            {
                for (int k = 2; k > w; --k) g_win.worst[k] = g_win.worst[k - 1];
                g_win.worst[w].ms = total;
                memcpy(g_win.worst[w].slotMs, slotMs, sizeof(slotMs));
                break;
            }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (g_win.startQpc == 0) g_win.startQpc = now.QuadPart;
        if (now.QuadPart - g_win.startQpc < 10 * g_freq.QuadPart) return;

        // Report: frame distribution, the worst frames and what was in them,
        // and each module's average / worst / count of frames over 0.5 ms.
        // Percentiles by partial selection rather than a full sort: each
        // nth_element puts one rank in place with everything above it after
        // it, so asking for rising ranks only ever works on what is left.
        // Gives exactly the values the sorted array would have at those ranks.
        g_spikeScratch.assign(g_win.frameMs.begin(), g_win.frameMs.end());
        std::vector<float>& sorted = g_spikeScratch;
        const size_t f = sorted.size();
        size_t settledUpTo = 0;   // ranks below this are already in place
        auto rank = [&](size_t k)
        {
            if (k >= settledUpTo)
            {
                std::nth_element(sorted.begin() + settledUpTo, sorted.begin() + k, sorted.end());
                settledUpTo = k + 1;
            }
            return sorted[k];
        };
        auto pct = [&](double p) { return rank((std::min)(f - 1, static_cast<size_t>(p * (f - 1)))); };
        const float p50 = pct(0.5), p90 = pct(0.9), p99 = pct(0.99);
        const float pMax = rank(f - 1);
        Log::Write("Spikes: %zu frames in 10 s, DLL ms/frame median %.2f  p90 %.2f  p99 %.2f  max %.2f",
                   f, p50, p90, p99, pMax);
        for (int w = 0; w < 3 && g_win.worst[w].ms > 0.0f; ++w)
        {
            char line[512]; int len = sprintf_s(line, "Spikes:  worst #%d %.2f ms:", w + 1, g_win.worst[w].ms);
            int order[kMaxSlots]; for (int i = 0; i < n; ++i) order[i] = i;
            std::sort(order, order + n, [&](int a, int b) { return g_win.worst[w].slotMs[a] > g_win.worst[w].slotMs[b]; });
            if (len < 0) len = 0;
            for (int k = 0; k < n && k < 5 && len < 440; ++k)
                if (g_win.worst[w].slotMs[order[k]] >= 0.05f)
                {
                    // A failed format returns -1: skip it rather than step back (review F263).
                    const int add = sprintf_s(line + len, sizeof(line) - len, "  %s %.2f", g_names[order[k]], g_win.worst[w].slotMs[order[k]]);
                    if (add > 0) len += add;
                }
            Log::Write("%s", line);
        }
        int order[kMaxSlots]; for (int i = 0; i < n; ++i) order[i] = i;
        std::sort(order, order + n, [&](int a, int b) { return g_win.maxMs[a] > g_win.maxMs[b]; });
        for (int k = 0; k < n; ++k)
        {
            const int i = order[k];
            if (g_win.maxMs[i] < 0.1f) break;
            Log::Write("Spikes:  %-18s avg %.3f  max %.2f  frames>0.5ms %d", g_names[i],
                       g_win.sumMs[i] / f, g_win.maxMs[i], g_win.framesOver[i]);
        }
        ResetSpikeWindow(now.QuadPart);
    }

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

    // Computed once a second from the snapshots, drawn every frame.
    struct Shown
    {
        bool valid = false;
        double totalMsPerSec = 0.0;
        double renderMsPerFrame = 0.0;
        double renderP999 = 0.0;   // 99.9th percentile render ms/frame, last 10 s
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
        // 99.9%: the frames in the window, slowest 0.1% cut off.
        {
            const int n = (std::min)(g_frameRingCount, static_cast<int>((std::min)(frames, static_cast<long long>(kFrameRing))));
            if (n > 0)
            {
                static float tmp[kFrameRing];
                for (int i = 0; i < n; ++i)
                    tmp[i] = g_frameRing[(g_frameRingNext - 1 - i + kFrameRing) % kFrameRing];
                const int k = (std::min)(n - 1, static_cast<int>(n * 0.999));
                std::nth_element(tmp, tmp + k, tmp + n);
                shown.renderP999 = tmp[k];
            }
        }
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
    if (slot < 0 && n >= kMaxSlots)
    {
        static bool s_loggedFull = false;   // under g_registerLock
        if (!s_loggedFull)
        {
            s_loggedFull = true;
            Log::Write("Profiler: no free slot for '%s' (kMaxSlots %d) -- not timed", name, kMaxSlots);
        }
    }
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
    {
        g_renderTicks.fetch_add(ticks, std::memory_order_relaxed);
        g_frameSlotTicks[slot] += ticks;
    }
}

void Profiler::FrameTick()
{
    g_renderThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    ++g_frames;
    if (g_freq.QuadPart == 0)
        QueryPerformanceFrequency(&g_freq);
    {
        const long long rt = g_renderTicks.load(std::memory_order_relaxed);
        if (g_freq.QuadPart && g_lastFrameRenderTicks != 0)   // skip the first frame (startup)
        {
            g_frameRing[g_frameRingNext] = static_cast<float>((rt - g_lastFrameRenderTicks) * 1000.0 / g_freq.QuadPart);
            g_frameRingNext = (g_frameRingNext + 1) % kFrameRing;
            if (g_frameRingCount < kFrameRing) ++g_frameRingCount;
        }
        g_lastFrameRenderTicks = rt;
    }
    EndFrameForSpikes();   // the frame that just finished
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
    // Snapshots only advance while the panel is drawn: after a hidden
    // stretch, start the window afresh so it does not span the hidden time
    // (review F185).
    if (g_snapCount > 0 && now.QuadPart - last >= 2 * g_freq.QuadPart)
    {
        g_snapCount = 0;
        g_snapNext = 0;
        g_shown.valid = false;
    }
    if (g_snapCount == 0 || now.QuadPart - last >= g_freq.QuadPart)
        TakeSnapshot(now.QuadPart);

    // 2 Hz rebuild, replayed in between (2026-11-24 throttle pass); the
    // figures themselves change once a second.
    static RenderThrottle s_throttle(0.5);
    if (!Layout::Active() && s_throttle.TryReplay(renderer))
        return;
    RenderThrottle::Capture throttleCapture(s_throttle, renderer);

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
        // Core %: all the DLL's CPU time per second (every thread: render,
        // telemetry, scoring) as a share of one core's 1000 ms per second.
        sprintf_s(header, "DLL %.3f ms/frame (render, 99.9%% %.2f)   %.2f%% of a core   10 s",
                  g_shown.renderMsPerFrame, g_shown.renderP999, g_shown.totalMsPerSec / 10.0);
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
