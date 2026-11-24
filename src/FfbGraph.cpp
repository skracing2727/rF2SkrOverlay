#include "FfbGraph.h"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "Log.h"
#include "RenderThrottle.h"
#include "Shortcuts.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <vector>

FfbSpectrumStore g_ffbSpectrum;

void FfbSpectrumStore::Init()
{
    if (m_ready) return;
    InitializeCriticalSection(&m_lock);
    QueryPerformanceFrequency(&m_freq);
    m_ready = true;
}

void FfbSpectrumStore::OnForce(double value)
{
    if (!m_ready) return;
    // The latest value is always kept: the pedal trace reads it to learn the
    // FFB sign.
    m_latest.store(static_cast<float>(value), std::memory_order_relaxed);

    // The history only feeds the graph, so it is not recorded while the
    // graph is hidden. Coming back, it restarts empty rather than joining
    // the old samples to the new across the gap.
    const bool collect = Shortcuts::Visible(Shortcuts::FfbGraph);
    if (!collect)
    {
        m_collecting = false;
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    EnterCriticalSection(&m_lock);
    if (!m_collecting)
    {
        m_total = 0;
        m_collecting = true;
    }
    const int i = static_cast<int>(m_total % kRing);
    m_values[i] = static_cast<float>(value);
    m_times[i] = static_cast<double>(now.QuadPart) / static_cast<double>(m_freq.QuadPart);
    ++m_total;
    LeaveCriticalSection(&m_lock);
}

void FfbSpectrumStore::Clear()
{
    if (!m_ready) return;
    EnterCriticalSection(&m_lock);
    m_total = 0;
    LeaveCriticalSection(&m_lock);
}

int FfbSpectrumStore::CopyLatest(float* values, double* times, int count, long long& totalOut)
{
    totalOut = 0;
    if (!m_ready) return 0;
    EnterCriticalSection(&m_lock);
    totalOut = m_total;
    long long avail = (std::min)(static_cast<long long>(count), (std::min)(m_total, static_cast<long long>(kRing)));
    const long long start = m_total - avail;
    for (long long n = 0; n < avail; ++n)
    {
        const int i = static_cast<int>((start + n) % kRing);
        values[n] = m_values[i];
        times[n] = m_times[i];
    }
    LeaveCriticalSection(&m_lock);
    return static_cast<int>(avail);
}

long long FfbSpectrumStore::Total()
{
    if (!m_ready) return 0;
    EnterCriticalSection(&m_lock);
    const long long total = m_total;
    LeaveCriticalSection(&m_lock);
    return total;
}

namespace
{
    // Log frequency axis from kLowHz to kTopHz, labelled at the Moza Pit
    // House EQ points (0 Hz can't sit on a log axis; the left edge is 1 Hz).
    const int kPoints = 12;
    const float kPointHz[kPoints] = { 5, 10, 15, 25, 30, 40, 50, 60, 80, 100, 150, 200 };
    const float kLowHz = 1.0f;
    const double kTwoPi = 6.283185307179586;

    // One point per DFT bin up to 200 Hz (half the 400 Hz FFB rate): with a 1 s window, every
    // 1 Hz, 201 points. The Moza points end at 100 Hz; 150 and 200 extend the axis.
    const int kMaxBins = 256;
    const float kTopHz = 200.0f;

    // 1 s Hann-windowed segments, a new one every 1/30 s. Live = the newest
    // segment; averaged = running sums over windowSeconds; lap average =
    // every segment of the last completed lap. (Every 1/60 s before
    // 2026-11-23: twice the work, for a 1 s window that hardly moves in a
    // thirtieth of a second either way.)
    const double kHopSeconds = 1.0 / 30.0;
    // A lap shorter than this (an aborted out-lap, a restart) doesn't
    // replace the previous lap's average.
    const double kMinLapSeconds = 20.0;

    struct Segment
    {
        float amp2[kMaxBins]; // squared amplitude per bin, as a fraction of full force
        int freshSamples;     // samples new in this segment (for the clip %)
        int freshClipped;
    };

    // The averaging window's segments, a fixed ring rather than a deque (which
    // allocated every ~1 KB segment on its own). Room for WindowSeconds' 30 s
    // maximum at the hop rate.
    const int kMaxSegs = 1024;
    Segment g_segs[kMaxSegs];

    struct Analysis
    {
        int segLen = 0;                    // samples per segment (~1 s)
        int binCount = 0;                  // bins 0..binCount-1 are shown
        double segRateHz = 0.0;            // sample rate the bins were laid out for
        std::vector<float> window;         // Hann window, segLen long
        std::vector<float> coef;           // Goertzel 2cos(2 pi k / segLen) per bin
        std::vector<double> coefD;         // the same, unrounded, for the final magnitude
        int segHead = 0, segCount = 0;     // g_segs: next slot to write, segments held (newest at segHead - 1)
        size_t maxSegs = 1;
        double sumAmp2[kMaxBins] = {};     // running sums over the segments held
        long long sumSamples = 0, sumClipped = 0;
        long long lastSegEndTotal = 0;
        double sampleRateHz = 0.0;
        double loggedRateHz = 0.0;
        float clipAmp[kMaxBins] = {};      // spectrum (%) at the latest clip
        double clipTime = -1.0e9;          // when it was captured (QPC seconds)
        float clipPercent = 0.0f;
        double lastSampleTime = 0.0;

        // Lap average: sums for the lap in progress, and the finished one.
        long currentLap = -1;
        double lapSumAmp2[kMaxBins] = {};
        long long lapSegments = 0;
        float lastLapAmp[kMaxBins] = {};   // %
        bool haveLastLap = false;
    };
    Analysis g_an;

    double NowSeconds()
    {
        static LARGE_INTEGER s_freq = {};
        if (s_freq.QuadPart == 0) QueryPerformanceFrequency(&s_freq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart) / static_cast<double>(s_freq.QuadPart);
    }

    double BinHz(int k) { return k * g_an.segRateHz / g_an.segLen; }

    void ClearLap()
    {
        for (int k = 0; k < kMaxBins; ++k) g_an.lapSumAmp2[k] = 0.0;
        g_an.lapSegments = 0;
    }

    const Segment& NewestSegment() { return g_segs[(g_an.segHead - 1 + kMaxSegs) % kMaxSegs]; }

    void ClearHistory()
    {
        g_an.segHead = g_an.segCount = 0;
        for (int k = 0; k < kMaxBins; ++k) g_an.sumAmp2[k] = 0.0;
        g_an.sumSamples = g_an.sumClipped = 0;
        ClearLap();
        g_an.haveLastLap = false;
        g_an.currentLap = -1;
    }

    void SetSegmentLength(int n, double rate)
    {
        g_an.segLen = n;
        g_an.segRateHz = rate;
        g_an.window.resize(n);
        for (int i = 0; i < n; ++i)
            g_an.window[i] = static_cast<float>(0.5 - 0.5 * cos(kTwoPi * i / n));
        int bins = 0;
        while (bins < kMaxBins && bins <= n / 2 && BinHz(bins) <= kTopHz + 1e-6) ++bins;
        g_an.binCount = bins;
        g_an.coef.resize(bins);
        g_an.coefD.resize(bins);
        for (int k = 0; k < bins; ++k)
        {
            g_an.coefD[k] = 2.0 * cos(kTwoPi * k / n);
            g_an.coef[k] = static_cast<float>(g_an.coefD[k]);
        }
        ClearHistory();
    }

    // Amplitude per bin, as a fraction of full force, so the same strength
    // of vibration reads the same at 10 Hz as at 70 Hz:
    //  - the steady part (0 Hz) is the plain mean, taken out BEFORE the
    //    window, so it doesn't leak into the 1 Hz bin;
    //  - every other bin is |X| corrected for the Hann window's 0.5 gain
    //    and the one-sided spectrum: A = 4|X|/N -- a full-scale sine reads
    //    1.0 (100%). A sine still shows about half of that in the bins
    //    either side (the window's own width, +/-1 Hz).
    //
    // The same bins as a plain DFT, by Goertzel's recurrence: the segment is
    // windowed once, then every bin's two-term state is stepped through it
    // together, one sample at a time -- independent bins side by side, which
    // the compiler turns into SIMD. ~17 us a segment at 400 Hz against
    // ~120 us for the DFT it replaced (which re-windowed the whole segment
    // for each of the ~200 bins, in double). In float the state drifts by at
    // most ~0.005% of full force (measured, 1024-sample segments; ~0.001% at
    // 400) -- far under the plot's default 0.5% floor (MinPercent).
    void AnalyseSegment(const float* x, int n, Segment& out)
    {
        double mean = 0.0;
        for (int i = 0; i < n; ++i) mean += x[i];
        mean /= n;
        out.amp2[0] = static_cast<float>(mean * mean);

        static float s_xw[1024];             // segLen is at most 1024 (see Update)
        static float s_s1[kMaxBins], s_s2[kMaxBins], s_c[kMaxBins];
        if (n > 1024) n = 1024;
        for (int i = 0; i < n; ++i)
            s_xw[i] = static_cast<float>((x[i] - mean) * g_an.window[i]);
        const int bins = g_an.binCount;
        for (int k = 0; k < bins; ++k)
        {
            s_s1[k] = 0.0f;
            s_s2[k] = 0.0f;
            s_c[k] = g_an.coef[k];
        }
        for (int i = 0; i < n; ++i)
        {
            const float v = s_xw[i];
            for (int k = 1; k < bins; ++k)
            {
                const float s = v + s_c[k] * s_s1[k] - s_s2[k];
                s_s2[k] = s_s1[k];
                s_s1[k] = s;
            }
        }
        // |X|^2 = s1^2 + s2^2 - 2cos(w) s1 s2, and A^2 = 16 |X|^2 / N^2.
        const double scale = 16.0 / (static_cast<double>(n) * n);
        for (int k = 1; k < bins; ++k)
        {
            const double s1 = s_s1[k], s2 = s_s2[k];
            const double p = s1 * s1 + s2 * s2 - g_an.coefD[k] * s1 * s2;
            out.amp2[k] = static_cast<float>(p > 0.0 ? p * scale : 0.0);
        }
    }

    // Sample rate from the median gap between samples: unlike "samples /
    // time span", a pause (menu, ESC) is one long gap among hundreds of
    // normal ones and can't drag the estimate down.
    double MedianRate(const double* times, int count)
    {
        static double s_gaps[512];
        const int n = (std::min)(count - 1, 512);
        if (n < 16) return 0.0;
        for (int i = 0; i < n; ++i)
            s_gaps[i] = times[count - n + i] - times[count - n + i - 1];
        std::nth_element(s_gaps, s_gaps + n / 2, s_gaps + n);
        const double gap = s_gaps[n / 2];
        return (gap > 1e-5) ? 1.0 / gap : 0.0;
    }

    // Pulls in any newly completed segment, the clip figure and the lap average.
    void Update(const FfbGraphConfig& cfg)
    {
        // Nothing to do until a hop's worth of new samples is in: skip the
        // copy below (made under the FFB thread's lock) on those frames.
        if (g_an.segLen > 0 && g_an.sampleRateHz >= 20.0)
        {
            const long long have = g_ffbSpectrum.Total();
            const long long hop = (std::max)(1LL, static_cast<long long>(g_an.sampleRateHz * kHopSeconds + 0.5));
            if (have >= g_an.lastSegEndTotal && have - g_an.lastSegEndTotal < hop)
                return;
        }

        static float s_values[FfbSpectrumStore::kRing];
        static double s_times[FfbSpectrumStore::kRing];
        long long total = 0;
        // Only what one segment plus the rate estimate needs -- the copy is
        // made under the FFB thread's lock.
        const int want = (std::min)(FfbSpectrumStore::kRing, (std::max)(1024, g_an.segLen + 64));
        const int got = g_ffbSpectrum.CopyLatest(s_values, s_times, want, total);
        if (got < 64)
            return;
        if (total < g_an.lastSegEndTotal) // store was cleared (new session)
        {
            ClearHistory();
            g_an.lastSegEndTotal = 0;
        }
        g_an.lastSampleTime = s_times[got - 1];

        const double rate = MedianRate(s_times, got);
        if (rate > 0.0)
            g_an.sampleRateHz = rate;
        if (g_an.sampleRateHz < 20.0)
            return;
        if (fabs(g_an.sampleRateHz - g_an.loggedRateHz) > g_an.loggedRateHz * 0.25)
        {
            g_an.loggedRateHz = g_an.sampleRateHz;
            Log::Write("FfbGraph: FFB signal arriving at %.0f Hz -- spectrum covers up to %.0f Hz",
                       g_an.sampleRateHz, (std::min)(g_an.sampleRateHz * 0.5, static_cast<double>(kTopHz)));
        }

        const int segLen = (std::max)(32, (std::min)(1024, static_cast<int>(g_an.sampleRateHz + 0.5)));
        if (g_an.segLen == 0 || abs(segLen - g_an.segLen) > g_an.segLen / 4)
            SetSegmentLength(segLen, g_an.sampleRateHz);

        const long long hop = (std::max)(1LL, static_cast<long long>(g_an.sampleRateHz * kHopSeconds + 0.5));
        if (got < g_an.segLen || total - g_an.lastSegEndTotal < hop)
            return;
        // Only the samples new since the last segment decide whether this
        // one counts as a clip, so one clip doesn't re-fire over all the
        // overlapping segments that contain it.
        const int fresh = static_cast<int>((std::min)(static_cast<long long>(g_an.segLen), total - g_an.lastSegEndTotal));
        int freshClipped = 0;
        for (int i = got - fresh; i < got; ++i)
            if (fabsf(s_values[i]) >= cfg.clipLevel) ++freshClipped;
        g_an.lastSegEndTotal = total;

        // Oldest segments out first, so the new one has its slot: the window
        // holds at most maxSegs once it is in.
        const double segSeconds = g_an.segLen / g_an.sampleRateHz;
        g_an.maxSegs = static_cast<size_t>((std::max)(1.0, (cfg.windowSeconds - segSeconds) / kHopSeconds + 1.0));
        if (g_an.maxSegs > static_cast<size_t>(kMaxSegs)) g_an.maxSegs = kMaxSegs;
        while (g_an.segCount > 0 && static_cast<size_t>(g_an.segCount) >= g_an.maxSegs)
        {
            const Segment& old = g_segs[(g_an.segHead - g_an.segCount + kMaxSegs) % kMaxSegs];
            for (int k = 0; k < g_an.binCount; ++k) g_an.sumAmp2[k] -= old.amp2[k];
            g_an.sumSamples -= old.freshSamples;
            g_an.sumClipped -= old.freshClipped;
            --g_an.segCount;
        }

        Segment& seg = g_segs[g_an.segHead];
        AnalyseSegment(s_values + got - g_an.segLen, g_an.segLen, seg);
        seg.freshSamples = fresh;
        seg.freshClipped = freshClipped;
        g_an.segHead = (g_an.segHead + 1) % kMaxSegs;
        ++g_an.segCount;
        for (int k = 0; k < g_an.binCount; ++k) g_an.sumAmp2[k] += seg.amp2[k];
        g_an.sumSamples += seg.freshSamples;
        g_an.sumClipped += seg.freshClipped;

        // Clip %, over the averaging window -- counted per segment, since
        // the raw samples copied above only cover the latest second or two.
        g_an.clipPercent = g_an.sumSamples > 0
            ? 100.0f * static_cast<float>(g_an.sumClipped) / static_cast<float>(g_an.sumSamples) : 0.0f;
        if (freshClipped > 0)
        {
            for (int k = 0; k < g_an.binCount; ++k) g_an.clipAmp[k] = 100.0f * sqrtf(seg.amp2[k]);
            g_an.clipTime = NowSeconds();
        }

        // Lap average. A new lap number closes the previous lap (if it was
        // long enough to mean anything); time stopped or crawling in the
        // pits is left out, since it would just pull the average down.
        const long lap = g_ffbSpectrum.Lap();
        if (lap != g_an.currentLap)
        {
            if (g_an.currentLap >= 0 && lap > g_an.currentLap
                && g_an.lapSegments * kHopSeconds >= kMinLapSeconds)
            {
                for (int k = 0; k < g_an.binCount; ++k)
                    g_an.lastLapAmp[k] = 100.0f * sqrtf(static_cast<float>(g_an.lapSumAmp2[k] / g_an.lapSegments));
                g_an.haveLastLap = true;
            }
            ClearLap();
            g_an.currentLap = lap;
        }
        if (g_ffbSpectrum.SpeedMs() > 15.0f)
        {
            for (int k = 0; k < g_an.binCount; ++k) g_an.lapSumAmp2[k] += seg.amp2[k];
            ++g_an.lapSegments;
        }
    }
}

bool FfbGraph::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    const FfbGraphConfig& cfg = g_ffbGraphConfig;
    if (!cfg.enabled)
        return false;

    // 60 fps. The analysis hops at 30 Hz (kHopSeconds), so every other
    // redraw shows the same spectrum; it is the clip curve's fade that
    // wants the 60.
    static RenderThrottle s_throttle(1.0 / 60.0);
    if (!Layout::Active() && s_throttle.TryReplay(renderer))
        return true;
    RenderThrottle::Capture throttleCapture(s_throttle, renderer);

    struct TextBgGuard
    {
        Renderer& r; bool prev;
        explicit TextBgGuard(Renderer& rr) : r(rr), prev(rr.TextBackgroundEnabled()) { r.SetTextBackground(false); }
        ~TextBgGuard() { r.SetTextBackground(prev); }
    } textBgGuard(renderer);

    Update(cfg);

    const float uiScale = static_cast<float>(screenHeight) / 2160.0f * cfg.scale;
    const float boxW = cfg.widthPx * uiScale;
    const float boxH = cfg.heightPx * uiScale;
    const float pad = cfg.paddingPx * uiScale;
    const float textPx = cfg.textHeightPx * uiScale;
    const float centerX = screenWidth * cfg.posXFraction;
    const float centerY = screenHeight * cfg.posYFraction;
    const float left = centerX - boxW * 0.5f;
    const float top = centerY - boxH * 0.5f;

    Layout::Register("FFB spectrum", "FfbGraph", "PosXFraction", "PosYFraction",
                     &g_ffbGraphConfig.posXFraction, &g_ffbGraphConfig.posYFraction,
                     centerX, centerY, boxW * 0.5f, boxH * 0.5f,
                     &g_ffbGraphConfig.scale, "Scale");

    renderer.AddFilledRoundedRect(centerX, centerY, boxW * 0.5f, boxH * 0.5f, cfg.cornerRadiusPx * uiScale, 0.0f,
                                  cfg.bgR, cfg.bgG, cfg.bgB, cfg.bgA);

    // Title row: what this is, the signal's own rate, and the clip figure.
    const float titleY = top + pad;
    const double now = NowSeconds();
    const bool live = g_an.segCount > 0 && (now - g_an.lastSampleTime) < 1.0;
    const bool showLap = cfg.showLapAverage && (g_an.haveLastLap || g_an.lapSegments > 0);

    char title[160];
    sprintf_s(title, "FFB spectrum  %s%s  signal %.0f Hz",
              cfg.realtime ? "(live 1 s)" : "(average)",
              !cfg.showLapAverage ? "" : (g_an.haveLastLap ? "  dotted: last lap" : "  dotted: this lap so far"),
              g_an.sampleRateHz);
    renderer.AddText(left + pad, titleY, title, textPx, 0.75f, 0.75f, 0.75f, 1.0f);
    char clipText[48];
    sprintf_s(clipText, "Clipping %.1f%% (%.0f s)", g_an.clipPercent, cfg.windowSeconds);
    const bool clipping = g_an.clipPercent > 0.0f;
    renderer.AddText(left + boxW - pad, titleY, clipText, textPx,
                     clipping ? 1.0f : 0.6f, clipping ? 0.25f : 0.6f, clipping ? 0.2f : 0.6f, 1.0f,
                     Renderer::HAlign::Right);

    // Plot area: y labels down the left, Hz labels along the bottom.
    const float yLabelW = renderer.MeasureText("100%", textPx) + pad * 0.6f;
    const float plotL = left + pad + yLabelW;
    const float plotR = left + boxW - pad * 2.0f;
    const float plotT = titleY + textPx * 1.8f;
    const float plotB = top + boxH - pad - textPx * 1.6f;
    const float plotH = plotB - plotT;
    // Frequency -> x on the log axis.
    const float logLo = log10f(kLowHz), logSpan = log10f(kTopHz) - log10f(kLowHz);
    auto xForHz = [&](double f) {
        if (f <= kLowHz) return plotL;
        if (f >= kTopHz) return plotR;
        return plotL + (plotR - plotL) * (log10f(static_cast<float>(f)) - logLo) / logSpan;
    };

    // Log scale: road texture and vibration are often only a few percent of
    // full force, and would sit flat on the floor of a linear 0-100% axis.
    const float lo = log10f((std::max)(cfg.minPercent, 0.01f));
    const float hi = log10f((std::max)(cfg.maxPercent, cfg.minPercent * 2.0f));
    auto yFor = [&](float pct) {
        const float l = log10f((std::max)(pct, cfg.minPercent));
        const float f = (std::min)(1.0f, (std::max)(0.0f, (l - lo) / (hi - lo)));
        return plotB - f * plotH;
    };

    const float gridThick = (std::max)(1.0f, 2.0f * uiScale);
    for (float pct : { 1.0f, 10.0f, 100.0f })
    {
        if (pct < cfg.minPercent || pct > cfg.maxPercent) continue;
        const float y = yFor(pct);
        renderer.AddLine(plotL, y, plotR, y, gridThick, 0.30f, 0.30f, 0.30f, 0.6f);
        char lbl[16];
        sprintf_s(lbl, "%.0f%%", pct);
        renderer.AddText(plotL - pad * 0.6f, y - textPx * 0.5f, lbl, textPx, 0.55f, 0.55f, 0.55f, 1.0f,
                         Renderer::HAlign::Right);
    }
    // Moza EQ points: a grid line each; a label where it doesn't run into
    // the previous one (25/30 Hz and the like sit close on a log axis).
    {
        float lastLabelR = -1.0e9f;
        char lbl[16];
        sprintf_s(lbl, "%.0fHz", kLowHz);
        renderer.AddText(plotL, plotB + textPx * 0.5f, lbl, textPx, 0.55f, 0.55f, 0.55f, 1.0f, Renderer::HAlign::Center);
        lastLabelR = plotL + renderer.MeasureText(lbl, textPx) * 0.5f;
        for (int i = 0; i < kPoints; ++i)
        {
            const float x = xForHz(kPointHz[i]);
            renderer.AddLine(x, plotT, x, plotB, gridThick, 0.06f, 0.06f, 0.06f, 0.9f);
            sprintf_s(lbl, "%.0fHz", kPointHz[i]);
            const float w = renderer.MeasureText(lbl, textPx);
            if (x - w * 0.5f < lastLabelR + textPx * 0.3f) continue;
            renderer.AddText(x, plotB + textPx * 0.5f, lbl, textPx, 0.55f, 0.55f, 0.55f, 1.0f, Renderer::HAlign::Center);
            lastLabelR = x + w * 0.5f;
        }
    }
    // Minor ticks: 1-9 in each decade.
    {
        const float tickLen = textPx * 0.35f;
        for (float dec = 1.0f; dec < kTopHz; dec *= 10.0f)
            for (int j = 1; j <= 9; ++j)
            {
                const float f = dec * j;
                if (f < kLowHz || f > kTopHz) continue;
                const float x = xForHz(f);
                renderer.AddLine(x, plotB - tickLen, x, plotB, gridThick, 0.45f, 0.45f, 0.45f, 0.8f);
            }
        renderer.AddLine(plotL, plotB, plotR, plotB, gridThick, 0.45f, 0.45f, 0.45f, 0.8f);
    }

    if (!live || g_an.binCount < 2)
    {
        renderer.AddText(centerX, (plotT + plotB) * 0.5f - textPx * 0.5f, "no FFB signal", textPx,
                         0.6f, 0.6f, 0.6f, 1.0f, Renderer::HAlign::Center);
        return true;
    }

    const float lineThick = 4.0f * uiScale;

    // Per-bin % -> the points drawn: log bands (energy of every 1 Hz bin in
    // the band, at the band's centre), or one point per bin from 1 Hz.
    // DC (bin 0) is left out: it is the steady steering load, not vibration.
    struct Pts { int n = 0; float hz[kMaxBins]; float pct[kMaxBins]; };
    auto toPoints = [&](const float* pct, Pts& out) {
        out.n = 0;
        const int per = cfg.bandsPerDecade;
        if (per <= 0)
        {
            for (int k = 1; k < g_an.binCount; ++k)
            {
                if (BinHz(k) < kLowHz) continue;
                out.hz[out.n] = static_cast<float>(BinHz(k)); out.pct[out.n] = pct[k]; ++out.n;
            }
            return;
        }
        int k = 1;
        for (int band = 0; out.n < kMaxBins; ++band)
        {
            const double lo = kLowHz * pow(10.0, (band - 0.5) / per);
            const double hi = kLowHz * pow(10.0, (band + 0.5) / per);
            if (lo > kTopHz) break;
            double e = 0.0; int used = 0;
            while (k < g_an.binCount && BinHz(k) < lo) ++k;
            int kk = k;
            for (; kk < g_an.binCount && BinHz(kk) < hi; ++kk) { e += pct[kk] * pct[kk]; ++used; }
            k = kk;
            if (!used) continue;   // a band narrower than 1 Hz with no bin in it
            out.hz[out.n] = static_cast<float>((std::min)(static_cast<double>(kTopHz), kLowHz * pow(10.0, static_cast<double>(band) / per)));
            out.pct[out.n] = static_cast<float>(sqrt(e));
            ++out.n;
        }
    };
    // A solid line through one point per bin, at that bin's own frequency.
    auto drawLine = [&](const float* binPct, float r, float g, float b, float a, float thick) {
        static Pts pts; toPoints(binPct, pts);
        if (pts.n < 2) return;
        float px = xForHz(pts.hz[0]), py = yFor(pts.pct[0]);
        for (int k = 1; k < pts.n; ++k)
        {
            const float x = xForHz(pts.hz[k]), y = yFor(pts.pct[k]);
            renderer.AddLineSmooth(px, py, x, y, thick, r, g, b, a);
            px = x; py = y;
        }
    };
    // Same, dotted: short dashes laid along the polyline, the pattern
    // carried across points so the spacing stays even.
    //
    // Each step advances by a whole dash+gap cycle. The first version walked
    // to each dash/gap boundary via fmodf instead, and float rounding could
    // leave it a hair short of the boundary forever (a step smaller than one
    // ulp of the distance) -- an infinite loop on the render thread that hung
    // rF2 as soon as the lap-average line first drew (26.10.25.7).
    auto drawDotted = [&](const float* binPct, float r, float g, float b, float a, float thick) {
        static Pts pts; toPoints(binPct, pts);
        if (pts.n < 2) return;
        const float dash = (std::max)(1.0f, 7.0f * uiScale);
        const float cycle = dash + (std::max)(1.0f, 7.0f * uiScale);
        float px = xForHz(pts.hz[0]), py = yFor(pts.pct[0]);
        float phase = 0.0f; // distance already into the current cycle at this segment's start
        for (int k = 1; k < pts.n; ++k)
        {
            const float x = xForHz(pts.hz[k]), y = yFor(pts.pct[k]);
            const float dx = x - px, dy = y - py;
            const float len = sqrtf(dx * dx + dy * dy);
            if (len > 0.0f && len < 100000.0f) // also skips NaN
            {
                for (float s = -phase; s < len; s += cycle)
                {
                    const float d1 = (std::max)(s, 0.0f);
                    const float d2 = (std::min)(s + dash, len);
                    if (d2 > d1)
                        renderer.AddLine(px + dx * d1 / len, py + dy * d1 / len,
                                         px + dx * d2 / len, py + dy * d2 / len, thick, r, g, b, a);
                }
                phase = fmodf(phase + len, cycle);
            }
            px = x; py = y;
        }
    };

    float livePct[kMaxBins];
    for (int k = 0; k < g_an.binCount; ++k)
    {
        const double amp2 = cfg.realtime
            ? NewestSegment().amp2[k]
            : (std::max)(0.0, g_an.sumAmp2[k] / g_an.segCount);
        livePct[k] = 100.0f * sqrtf(static_cast<float>(amp2));
    }

    // Lap average underneath: the last completed lap, or until there is one,
    // the lap so far.
    if (showLap)
    {
        float lapPct[kMaxBins];
        if (g_an.haveLastLap)
            for (int k = 0; k < g_an.binCount; ++k) lapPct[k] = g_an.lastLapAmp[k];
        else
            for (int k = 0; k < g_an.binCount; ++k)
                lapPct[k] = 100.0f * sqrtf(static_cast<float>(g_an.lapSumAmp2[k] / g_an.lapSegments));
        drawDotted(lapPct, cfg.lapLineR, cfg.lapLineG, cfg.lapLineB, 0.95f, lineThick * 0.8f);
    }

    drawLine(livePct, cfg.lineR, cfg.lineG, cfg.lineB, 1.0f, lineThick);

    // Red: the spectrum at the moment it last clipped, fading out.
    const double clipAge = now - g_an.clipTime;
    if (clipAge >= 0.0 && clipAge < cfg.clipFadeSeconds)
        drawLine(g_an.clipAmp, 1.0f, 0.15f, 0.12f, static_cast<float>(1.0 - clipAge / cfg.clipFadeSeconds),
                 lineThick * 0.75f);

    // Anything at or above half the signal's own rate isn't in it at all.
    const float nyquist = static_cast<float>(g_an.sampleRateHz * 0.5);
    if (nyquist < kTopHz)
    {
        const float x0 = xForHz(nyquist);
        renderer.AddFilledRect((x0 + plotR) * 0.5f, (plotT + plotB) * 0.5f, (plotR - x0) * 0.5f, plotH * 0.5f, 0.0f,
                               0.0f, 0.0f, 0.0f, 0.45f);
        char note[48];
        sprintf_s(note, "not in signal above %.0f Hz", nyquist);
        if (renderer.MeasureText(note, textPx) < plotR - x0)
            renderer.AddText((x0 + plotR) * 0.5f, (plotT + plotB) * 0.5f - textPx * 0.5f, note, textPx,
                             0.6f, 0.6f, 0.6f, 1.0f, Renderer::HAlign::Center);
    }
    return true;
}
