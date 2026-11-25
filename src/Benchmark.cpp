#include "Benchmark.h"
#include "InternalsPlugin.hpp"
#include "Renderer.h"
#include "RenderThrottle.h"
#include "Config.h"
#include "Layout.h"
#include "FrameRate.h"
#include "PerfStats.h"
#include "GpuEngineUsage.h"
#include "Log.h"
#include "EcoThread.h"
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <intrin.h>

namespace
{
    // ---- sizes (all storage is fixed; see Benchmark.h) ----
    const int kSeries = 3;               // 0 = present, 1 = CPU, 2 = GPU
    const int kHistoryLaps = 10;         // previous laps kept on screen
    const double kBucketSec = 0.2;       // stored lap lines: one point per 0.2 s
    const int kMaxBuckets = 3000;        // 600 s; a longer lap just stops growing
    const double kBinMs = 0.01;          // histogram bin width (user; frame times are QPC / GPU-timestamp based, sub-microsecond)
    const int kBins = 5001;              // 0..49.99 ms in 0.01 ms bins, [5000] = 50 ms and over
    const double kFBinFps = 0.1;         // FPS histogram bin width (user)
    const int kFBins = 5001;             // 0..499.9 FPS, [5000] = 500 FPS and over
    const double kDotSeconds = 1.0;      // live dots fade out over this long (user: 1 s)
    const int kMaxDots = 4000;           // per series: one dot per frame up to 400 FPS over the 10 s
    const double kMinLapSeconds = 5.0;   // shorter "laps" (resets, pit exits) are not kept

    const float kColour[kSeries][3] = {
        { 0.95f, 0.95f, 0.95f },   // present: white
        { 0.35f, 0.60f, 1.00f },   // CPU: blue
        { 1.00f, 0.30f, 0.25f },   // GPU: red
    };

    // One lap: the averaged frame time per bucket (0 = no frames there) and
    // the frame-time histogram. Average ms, not average FPS, so a bucket's
    // FPS is frames / time -- what a benchmark tool would report.
    struct LapLine
    {
        float ms[kSeries][kMaxBuckets];
        unsigned hist[kSeries][kBins];
        unsigned fhist[kSeries][kFBins];   // the same frames by FPS
        double sumMs[kSeries];  // whole-lap totals, for the lap average FPS
        unsigned frames[kSeries];
        int buckets;          // buckets in use (index of the last one + 1)
        float minMs;          // fastest bucket of any series, for the FPS axis
        float maxMs;          // slowest bucket, for the FPS axis floor
        double lengthSec;
    };

    LapLine g_history[kHistoryLaps];   // ring, g_histNewest = most recent
    int g_histCount = 0;
    // Held exclusive by the render thread while whole laps move or clear,
    // shared by the percentile worker while it scans (see below).
    SRWLOCK g_dataLock = SRWLOCK_INIT;
    struct SrwExclusive { SRWLOCK& l; explicit SrwExclusive(SRWLOCK& x) : l(x) { AcquireSRWLockExclusive(&l); } ~SrwExclusive() { ReleaseSRWLockExclusive(&l); } };
    int g_histNewest = -1;
    LapLine g_current;
    // Running sums of the bucket being filled, per series.
    double g_bucketSum[kSeries] = {};
    int g_bucketN[kSeries] = {};
    int g_bucketIndex = -1;
    // Accumulated average (user, 2026-11-24): every lap's frames at each
    // 0.2 s point of the lap, summed over all laps this session -- the FPS
    // graph's one line per series. Frames / time, like the table's avg.
    double g_accSum[kSeries][kMaxBuckets];
    unsigned g_accN[kSeries][kMaxBuckets];
    int g_accBuckets = 0;
    // The current lap's raw per-bucket sums, added to the accumulated average
    // only when the lap is finished (user: the line is not recomputed with
    // every frame; it changes once per lap).
    double g_curSum[kSeries][kMaxBuckets];
    // Average distribution (user): every finished lap's histogram added
    // together, frame time and FPS, updated at the line -- the histogram's
    // one line per series. Totals in g_accHistFrames.
    unsigned g_accHist[kSeries][kBins];
    unsigned g_accFHist[kSeries][kFBins];
    unsigned long long g_accHistFrames[kSeries];
    unsigned g_curN[kSeries][kMaxBuckets];
    // Average lap distance reached at each 0.2 s point (all laps): maps the
    // time axis to distance for the labels along the top.
    double g_accDist[kMaxBuckets];
    unsigned g_accDistN[kMaxBuckets];

    struct Dot { float lapT; float ms; double wall; };
    Dot g_dots[kSeries][kMaxDots];
    int g_dotNext[kSeries] = {};
    int g_dotCount[kSeries] = {};
    double g_dotLastWall[kSeries] = {};
    // Histogram dots: one per frame at that frame's bin, at the bin's count
    // just after it -- older dots sit lower, so they trace the curve building.
    struct HDot { short bin; short fbin; unsigned count; unsigned fcount; double wall; };
    bool g_histFps = false;   // histogram shows FPS instead of frame time (click toggles)
    bool g_histClickDown = false;
    HDot g_hdots[kSeries][kMaxDots];
    // FPS-graph points for the whole current lap (user: the points stay).
    // One bit per (0.25 s, 2 FPS) cell per series, so a lap of any length
    // costs a fixed 460 KB and drawing is one small square per lit cell.
    const double kCellSec = 0.25;
    const int kCellT = 2400;                 // 600 s
    const float kCellFps = 2.0f;
    const int kCellWords = 8;                // 512 bits: 0..1022 FPS
    unsigned long long g_cells[kSeries][kCellT][kCellWords];
    int g_cellTUsed = 0;                     // time cells in use (last + 1)

    // ---- lap timing, written by the telemetry thread ----
    // The render thread never touches scoring: the player's own telemetry
    // carries the lap start time, and these atomics are all it reads.
    std::atomic<double> s_lapStartET{ -1.0 };
    std::atomic<double> s_lapTime{ 0.0 };        // ET - lap start at that telemetry
    std::atomic<long long> s_telemQpc{ 0 };      // when that telemetry arrived
    std::atomic<bool> s_resetRequested{ false };
    // Lap distance (scoring, ~5 Hz) and track length, for the top axis.
    std::atomic<double> s_lapDist{ -1.0 };
    std::atomic<double> s_lapLen{ 0.0 };

    double g_seenLapStartET = -1.0;   // Present thread's copy
    double g_lapNow = -1.0;           // current lap time this frame, <0 = not sampling
    double g_wallNow = 0.0;
    bool g_active = false;            // sampled last frame (false after the box was off)

    LARGE_INTEGER g_freq = {};
    double NowSec()
    {
        if (g_freq.QuadPart == 0) QueryPerformanceFrequency(&g_freq);
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart) / static_cast<double>(g_freq.QuadPart);
    }

    void ClearLap(LapLine& l)
    {
        memset(&l, 0, sizeof(l));
        l.minMs = 1e9f;
        l.maxMs = 0.0f;
    }

    void ClearCurrent()
    {
        ClearLap(g_current);
        for (int s = 0; s < kSeries; ++s) { g_bucketSum[s] = 0.0; g_bucketN[s] = 0; g_dotCount[s] = 0; g_dotNext[s] = 0; }
        g_bucketIndex = -1;
        memset(g_cells, 0, sizeof(g_cells));
        memset(g_curSum, 0, sizeof(g_curSum)); memset(g_curN, 0, sizeof(g_curN));
        g_cellTUsed = 0;
    }

    void FlushBucket()
    {
        if (g_bucketIndex < 0 || g_bucketIndex >= kMaxBuckets) return;
        for (int s = 0; s < kSeries; ++s)
        {
            if (g_bucketN[s] <= 0) continue;
            const float avg = static_cast<float>(g_bucketSum[s] / g_bucketN[s]);
            g_current.ms[s][g_bucketIndex] = avg;
            g_curSum[s][g_bucketIndex] += g_bucketSum[s];
            g_curN[s][g_bucketIndex] += static_cast<unsigned>(g_bucketN[s]);
            if (avg < g_current.minMs) g_current.minMs = avg;
            if (avg > g_current.maxMs) g_current.maxMs = avg;
            g_bucketSum[s] = 0.0; g_bucketN[s] = 0;
        }
        if (g_bucketIndex + 1 > g_current.buckets) g_current.buckets = g_bucketIndex + 1;
        // Distance reached at this point of the lap. Scoring lags the line by
        // up to ~0.2 s, so early in a lap a distance from the end of the last
        // lap is skipped rather than averaged in.
        {
            const double d = s_lapDist.load(std::memory_order_relaxed), len = s_lapLen.load(std::memory_order_relaxed);
            const bool stale = len > 0.0 && g_bucketIndex * kBucketSec < 10.0 && d > len * 0.5;
            if (d >= 0.0 && !stale) { g_accDist[g_bucketIndex] += d; ++g_accDistN[g_bucketIndex]; }
        }
    }

    // Line crossed: keep the finished lap (one 37 KB copy per lap, not per frame).
    void CommitLap()
    {
        FlushBucket();
        if (g_current.lengthSec >= kMinLapSeconds && g_current.buckets > 0)
        {
            g_histNewest = (g_histNewest + 1) % kHistoryLaps;
            g_history[g_histNewest] = g_current;
            if (g_histCount < kHistoryLaps) ++g_histCount;
            for (int sIdx = 0; sIdx < kSeries; ++sIdx)
                for (int b = 0; b < g_current.buckets && b < kMaxBuckets; ++b)
                {
                    g_accSum[sIdx][b] += g_curSum[sIdx][b];
                    g_accN[sIdx][b] += g_curN[sIdx][b];
                }
            if (g_current.buckets > g_accBuckets) g_accBuckets = g_current.buckets;
            for (int sIdx = 0; sIdx < kSeries; ++sIdx)
            {
                for (int k = 0; k < kBins; ++k) g_accHist[sIdx][k] += g_current.hist[sIdx][k];
                for (int k = 0; k < kFBins; ++k) g_accFHist[sIdx][k] += g_current.fhist[sIdx][k];
                g_accHistFrames[sIdx] += g_current.frames[sIdx];
            }
        }
        ClearCurrent();
    }

    void AddSample(int series, double ms)
    {
        if (g_lapNow < 0.0 || !(ms > 0.01) || ms > 2000.0) return;
        const int b = static_cast<int>(g_lapNow / kBucketSec);
        if (b != g_bucketIndex) { FlushBucket(); g_bucketIndex = b; }
        if (b < kMaxBuckets) { g_bucketSum[series] += ms; ++g_bucketN[series]; }
        int bin = static_cast<int>(ms / kBinMs);
        if (bin >= kBins - 1) bin = kBins - 1;
        const unsigned cnt = ++g_current.hist[series][bin];
        int fbin = static_cast<int>(1000.0 / ms / kFBinFps);
        if (fbin >= kFBins - 1) fbin = kFBins - 1;
        const unsigned fcnt = ++g_current.fhist[series][fbin];
        g_current.sumMs[series] += ms;
        {
            const int ct = static_cast<int>(g_lapNow / kCellSec);
            int cf = static_cast<int>(1000.0 / ms / kCellFps);
            if (cf >= kCellWords * 64) cf = kCellWords * 64 - 1;
            if (ct >= 0 && ct < kCellT)
            {
                g_cells[series][ct][cf >> 6] |= 1ull << (cf & 63);
                if (ct + 1 > g_cellTUsed) g_cellTUsed = ct + 1;
            }
        }
        ++g_current.frames[series];
        if (g_lapNow > g_current.lengthSec) g_current.lengthSec = g_lapNow;
        // Live dots: one per frame (user request). Above 400 FPS the ring
        // holds less than the full 10 s, so the oldest dots drop early.
        {
            g_dotLastWall[series] = g_wallNow;
            Dot& d = g_dots[series][g_dotNext[series]];
            d.lapT = static_cast<float>(g_lapNow);
            d.ms = static_cast<float>(ms);
            d.wall = g_wallNow;
            HDot& h = g_hdots[series][g_dotNext[series]];
            h.bin = static_cast<short>(bin); h.count = cnt; h.fbin = static_cast<short>(fbin); h.fcount = fcnt; h.wall = g_wallNow;
            g_dotNext[series] = (g_dotNext[series] + 1) % kMaxDots;
            if (g_dotCount[series] < kMaxDots) ++g_dotCount[series];
        }
    }

    // Works out this frame's lap time and handles the line crossing.
    void UpdateLap()
    {
        g_wallNow = NowSec();
        if (s_resetRequested.exchange(false))
        {
            SrwExclusive lock(g_dataLock);   // the percentile worker may be scanning
            ClearCurrent();
            g_histCount = 0; g_histNewest = -1;
            memset(g_accSum, 0, sizeof(g_accSum)); memset(g_accN, 0, sizeof(g_accN)); g_accBuckets = 0;
            memset(g_accDist, 0, sizeof(g_accDist)); memset(g_accDistN, 0, sizeof(g_accDistN));
            memset(g_accHist, 0, sizeof(g_accHist)); memset(g_accFHist, 0, sizeof(g_accFHist)); memset(g_accHistFrames, 0, sizeof(g_accHistFrames));
            g_seenLapStartET = -1.0;
        }
        const long long q = s_telemQpc.load(std::memory_order_acquire);
        const double lapStart = s_lapStartET.load(std::memory_order_relaxed);
        const double telemAge = q != 0 ? g_wallNow - static_cast<double>(q) / static_cast<double>(g_freq.QuadPart) : 1e9;
        // No fresh player telemetry (menus, monitor, loading): do not sample.
        if (lapStart < 0.0 || telemAge > 1.0) { g_lapNow = -1.0; return; }
        if (g_seenLapStartET >= 0.0 && fabs(lapStart - g_seenLapStartET) > 0.01)
        {
            SrwExclusive lock(g_dataLock);   // whole-lap copy/clear vs the percentile worker
            CommitLap();
        }
        g_seenLapStartET = lapStart;
        // Telemetry runs at ~90 Hz; between updates, advance by wall time
        // (capped, so a pause freezes the x position rather than running on).
        g_lapNow = s_lapTime.load(std::memory_order_relaxed) + (telemAge < 0.1 ? telemAge : 0.1);
        if (g_lapNow < 0.0) g_lapNow = 0.0;
    }

    // ---- GPU timestamps ----
    // A small ring of (disjoint, timestamp) query pairs. Frame N's timestamp
    // is taken at the end of the HUD draw and sits inside a disjoint query
    // begun one frame earlier; GPU frame time = ts[N] - ts[N-1]. Results are
    // read a few frames later with DONOTFLUSH and skipped while not ready,
    // so the CPU never waits on the GPU. Only the immediate context on the
    // Present thread is used -- the same context the HUD itself draws with.
    const int kQ = 8;
    struct QuerySlot { ID3D11Query* disjoint; ID3D11Query* ts; unsigned long long seq; bool pending; };
    QuerySlot g_q[kQ] = {};
    ID3D11Device* g_qDevice = nullptr;   // compared only; the queries hold the device
    bool g_qFailed = false;
    int g_openSlot = -1, g_nextSlot = 0, g_readSlot = 0;
    unsigned long long g_seq = 0, g_lastSeq = 0;
    UINT64 g_lastTs = 0;
    bool g_lastTsValid = false;

    void ReleaseQueries()
    {
        for (int i = 0; i < kQ; ++i)
        {
            if (g_q[i].disjoint) g_q[i].disjoint->Release();
            if (g_q[i].ts) g_q[i].ts->Release();
            g_q[i] = QuerySlot{};
        }
        g_qDevice = nullptr;
        g_openSlot = -1; g_nextSlot = 0; g_readSlot = 0;
        g_lastTsValid = false;
    }

    bool CreateQueries(ID3D11Device* device)
    {
        ReleaseQueries();
        D3D11_QUERY_DESC dj = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        D3D11_QUERY_DESC ts = { D3D11_QUERY_TIMESTAMP, 0 };
        for (int i = 0; i < kQ; ++i)
        {
            if (FAILED(device->CreateQuery(&dj, &g_q[i].disjoint)) || FAILED(device->CreateQuery(&ts, &g_q[i].ts)))
            {
                ReleaseQueries();
                return false;
            }
        }
        g_qDevice = device;
        return true;
    }

    // ---- drawing helpers ----
    float NiceFpsCeiling(float fps)
    {
        if (fps < 60.0f) fps = 60.0f;
        if (fps > 1000.0f) fps = 1000.0f;
        const float step = fps <= 240.0f ? 30.0f : 100.0f;
        return ceilf(fps / step) * step;
    }

    // ---- percentile worker (efficiency mode, user 2026-11-25) ----
    // The percentile scans (5001 bins per lap and series, over the current
    // lap and the 10 kept laps) run 4x a second on a background thread in
    // EcoQoS, so they go to the E-cores instead of the render thread. The
    // render thread only copies the results. g_dataLock is held exclusive
    // by the render thread while it moves or clears whole laps (CommitLap,
    // reset) and shared by the worker while it scans; the per-frame count
    // increments in AddSample take no lock (a scan may see a count one
    // frame old -- harmless for percentiles).
    struct PctResults
    {
        double fastMs = 1e9, slowMs = 0.0;           // graph 1: fastest 0.1%, slowest 0.5%
        double vMax[2] = { 10.0, 0.0 };              // graph 2, [0] = ms mode, [1] = FPS mode
        double vLow[2] = { 1e9, 1e9 };
        char low[kSeries][2][8] = {};                // table: 1% / 0.1% low FPS text
        bool valid = false;
    };
    SRWLOCK g_resLock = SRWLOCK_INIT;
    PctResults g_res;
    HANDLE g_pctThread = nullptr;
    HANDLE g_pctWake = nullptr;
    std::atomic<bool> g_pctStop{ false };
    std::atomic<long long> g_pctLastDraw{ 0 };       // GetTickCount64 of the last Draw; idle when the box is shut

    // Bin where 'frac' of 'tot' frames is reached; nb when never (off the top), -1 when empty.
    int PctBin(const unsigned* h, int nb, unsigned long long tot, double frac)
    {
        if (tot == 0) return -1;
        const unsigned long long want = static_cast<unsigned long long>(ceil(tot * frac));
        unsigned long long run = 0;
        for (int k = 0; k < nb; ++k) { run += h[k]; if (run >= want) return k; }
        return nb;
    }

    void ComputePercentiles(PctResults& r)
    {
        for (int sIdx = 0; sIdx < kSeries; ++sIdx)
        {
            const unsigned long long n = g_current.frames[sIdx];
            const int f = PctBin(g_current.hist[sIdx], kBins, n, 0.001);
            if (f >= 0 && f < kBins && (f + 0.5) * kBinMs < r.fastMs) r.fastMs = (f + 0.5) * kBinMs;
            for (int i = -1; i < g_histCount; ++i)
            {
                const LapLine& l = i < 0 ? g_current : g_history[i];
                const int sl = PctBin(l.hist[sIdx], kBins, l.frames[sIdx], 0.995);
                if (sl >= 0 && sl < kBins && (sl + 0.5) * kBinMs > r.slowMs) r.slowMs = (sl + 0.5) * kBinMs;
                for (int m = 0; m < 2; ++m)
                {
                    const unsigned* h = m ? l.fhist[sIdx] : l.hist[sIdx];
                    const int nb = m ? kFBins : kBins;
                    const double bw = m ? kFBinFps : kBinMs;
                    const int hi = PctBin(h, nb, l.frames[sIdx], m ? 0.999 : 0.995);
                    if (hi >= 0 && (hi < nb ? (hi + 0.5) * bw : nb * bw) > r.vMax[m]) r.vMax[m] = hi < nb ? (hi + 0.5) * bw : nb * bw;
                    const int lo = PctBin(h, nb, l.frames[sIdx], 0.005);
                    if (lo >= 0 && (lo < nb ? (lo + 0.5) * bw : nb * bw) < r.vLow[m]) r.vLow[m] = lo < nb ? (lo + 0.5) * bw : nb * bw;
                }
            }
            for (int p = 0; p < 2; ++p)
            {
                char* out = r.low[sIdx][p];
                const int k = n < 1000 ? -1 : PctBin(g_current.hist[sIdx], kBins, n, p == 0 ? 0.99 : 0.999);
                if (k < 0 || k >= kBins) strcpy_s(out, 8, "--");
                else if (k >= kBins - 1) strcpy_s(out, 8, "<20");
                else sprintf_s(out, 8, "%.0f", 1000.0 / ((k + 0.5) * kBinMs));
            }
        }
        r.valid = true;
    }

    DWORD WINAPI PctThreadMain(LPVOID)
    {
        while (!g_pctStop.load())
        {
            WaitForSingleObject(g_pctWake, 100);   // 10 Hz, matching the graphs (user)
            if (g_pctStop.load()) break;
            if (GetTickCount64() - static_cast<unsigned long long>(g_pctLastDraw.load()) > 1000) continue;   // box shut
            PctResults r;
            AcquireSRWLockShared(&g_dataLock);
            ComputePercentiles(r);
            ReleaseSRWLockShared(&g_dataLock);
            AcquireSRWLockExclusive(&g_resLock);
            g_res = r;
            ReleaseSRWLockExclusive(&g_resLock);
        }
        return 0;
    }

    // Render thread, from Draw: start the worker once, mark the box live, copy the latest results.
    PctResults GetPercentiles()
    {
        g_pctLastDraw.store(static_cast<long long>(GetTickCount64()));
        if (!g_pctThread && !g_pctStop.load())
        {
            g_pctWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g_pctThread = CreateThread(nullptr, 0, PctThreadMain, nullptr, 0, nullptr);
            if (g_pctThread) MakeThreadEfficient(g_pctThread);
        }
        AcquireSRWLockShared(&g_resLock);
        PctResults r = g_res;
        ReleaseSRWLockShared(&g_resLock);
        return r;
    }
}

void Benchmark::Shutdown()
{
    g_pctStop = true;
    if (g_pctWake) SetEvent(g_pctWake);
    if (g_pctThread)
    {
        WaitForSingleObject(g_pctThread, 2000);
        CloseHandle(g_pctThread);
        g_pctThread = nullptr;
    }
    if (g_pctWake) { CloseHandle(g_pctWake); g_pctWake = nullptr; }
}

void Benchmark::Reset()
{
    s_lapStartET = -1.0;
    s_telemQpc = 0;
    s_resetRequested = true;
}

void Benchmark::OnScoring(const ScoringInfoV01& info)
{
    if (!g_benchmarkConfig.enabled || !info.mVehicle) return;
    for (long i = 0; i < info.mNumVehicles; ++i)
        if (info.mVehicle[i].mIsPlayer)
        {
            s_lapDist.store(info.mVehicle[i].mLapDist, std::memory_order_relaxed);
            s_lapLen.store(info.mLapDist, std::memory_order_relaxed);
            return;
        }
}

void Benchmark::OnTelemetry(const TelemInfoV01& info)
{
    if (!g_benchmarkConfig.enabled) return;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    s_lapStartET.store(info.mLapStartET, std::memory_order_relaxed);
    s_lapTime.store(info.mElapsedTime - info.mLapStartET, std::memory_order_relaxed);
    s_telemQpc.store(t.QuadPart, std::memory_order_release);
}

void Benchmark::OnFrameEnd()
{
    if (!g_benchmarkConfig.enabled) { g_active = false; return; }
    if (!g_active)
    {
        // Back on after a pause: the gap is not a frame, and the GPU chain
        // must restart from a fresh timestamp.
        g_active = true;
        g_lastTsValid = false;
    }
    UpdateLap();
    double ms = 0.0;
    if (FrameRate::GetLastFrameMs(ms)) AddSample(0, ms);
    if (PerfStats::GetLastCpuFrameMs(ms)) AddSample(1, ms);
}

void Benchmark::ReleaseGpu()
{
    ReleaseQueries();
}

void Benchmark::OnGpuMark(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    if (!g_benchmarkConfig.enabled || !device || !ctx) return;
    if (device != g_qDevice)
    {
        if (g_qFailed) return;
        if (!CreateQueries(device))
        {
            g_qFailed = true;
            Log::Write("Benchmark: could not create D3D11 timestamp queries -- no GPU series");
            return;
        }
        Log::Write("Benchmark: GPU timestamp queries created");
    }

    // Close the frame that is open: its timestamp marks the end of this
    // frame's GPU work as submitted so far (the game's frame plus our HUD).
    if (g_openSlot >= 0)
    {
        QuerySlot& q = g_q[g_openSlot];
        ctx->End(q.ts);
        ctx->End(q.disjoint);
        q.seq = ++g_seq;
        q.pending = true;
        g_openSlot = -1;
    }

    // Read back every finished pair, oldest first; stop at the first one
    // the GPU has not reached yet.
    while (g_q[g_readSlot].pending)
    {
        QuerySlot& q = g_q[g_readSlot];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        UINT64 ts = 0;
        if (ctx->GetData(q.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        if (ctx->GetData(q.ts, &ts, sizeof(ts), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        q.pending = false;
        g_readSlot = (g_readSlot + 1) % kQ;
        const bool ok = !dj.Disjoint && dj.Frequency != 0;
        if (ok && g_lastTsValid && q.seq == g_lastSeq + 1 && ts > g_lastTs)
            AddSample(2, static_cast<double>(ts - g_lastTs) * 1000.0 / static_cast<double>(dj.Frequency));
        g_lastTs = ts;
        g_lastSeq = q.seq;
        g_lastTsValid = ok;
    }

    // Open the next frame. If the ring is full (GPU more than kQ frames
    // behind) skip this frame: the sequence gap stops a bogus two-frame delta.
    if (!g_q[g_nextSlot].pending)
    {
        ctx->Begin(g_q[g_nextSlot].disjoint);
        g_openSlot = g_nextSlot;
        g_nextSlot = (g_nextSlot + 1) % kQ;
    }
    else
        ++g_seq;
}

bool Benchmark::Draw(Renderer& renderer, int screenWidth, int screenHeight, HWND hwnd)
{
    BenchmarkConfig& cfg = g_benchmarkConfig;
    if (!cfg.enabled) return false;

    const float s = static_cast<float>(screenHeight) / 2160.0f * (cfg.scale > 0.05f ? cfg.scale : 0.05f);
    const float halfW = 420.0f * s;
    const float halfH = 300.0f * s;
    const float cx = screenWidth * cfg.centerXFraction;
    const float cy = screenHeight * cfg.centerYFraction;
    Layout::Register("Benchmark", "Benchmark", "CenterXFraction", "CenterYFraction",
                     &cfg.centerXFraction, &cfg.centerYFraction, cx, cy, halfW, halfH, &cfg.scale, "Scale");

    // Shape-vertex budget for this box (the renderer's 200k buffer is shared
    // by every panel; 2026-11-24 the box alone hit 330k and everything after
    // it was dropped). Expensive layers check it and stop early.
    const size_t vbBase = renderer.ShapeVertexCount();
    const size_t kVbBudget = 60000;
    auto overBudget = [&]() { return renderer.ShapeVertexCount() - vbBase > kVbBudget; };
    const bool prevBg = renderer.TextBackgroundEnabled();
    renderer.SetTextBackground(false);
    renderer.AddFilledRoundedRect(cx, cy, halfW, halfH, 8.0f * s, 0.0f, 0.08f, 0.08f, 0.08f, 0.6f);

    const float pad = 16.0f * s;
    const float textPx = 26.0f * s;
    const float labelPx = 22.0f * s;
    const float left = cx - halfW + pad;
    const float right = cx + halfW - pad;
    float y = cy - halfH + pad;

    // The numbers (avg, 1% / 0.1% lows) and the GPU caveat are drawn inside
    // the FPS graph's top-left corner, after the lines (user, 2026-11-24).

    // ---- Graph 1: FPS vs lap time ----
    const float axisW = 52.0f * s;
    const float gx0 = left + axisW, gx1 = right;
    // Room above for the lap-distance labels.
    const float g1Top = y + labelPx + 4.0f * s, g1Bot = y + 290.0f * s + textPx + labelPx + 8.0f * s;
    const float gw = gx1 - gx0;
    const float lineW = 2.0f * s;
    const float dotR = 2.5f * s;
    // Histogram geometry and its click (frame time <-> FPS), needed before
    // the graphs because each graph may be a replay this frame.
    const float g2Top = g1Bot + labelPx * 1.3f + renderer.MeasureText("000", labelPx * 0.75f) + 18.0f * s;
    const float g2Bot = cy + halfH - pad - labelPx - 2.0f * s;
    const float g2h = g2Bot - g2Top;
    static RenderThrottle s_g1Throttle(0.1), s_g2Throttle(0.1);
    {
        float mx = 0.0f, my = 0.0f;
        const bool haveCursor = hwnd && Layout::CursorPosition(hwnd, screenWidth, screenHeight, mx, my);
        const bool down = haveCursor && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        if (down && !g_histClickDown && !Layout::Active() && mx >= gx0 && mx <= gx1 && my >= g2Top && my <= g2Bot)
        {
            g_histFps = !g_histFps;
            s_g2Throttle.ForceNext();
        }
        g_histClickDown = down;
    }
    // Drawn at 10 Hz (user): sampling still takes every frame; the graph
    // shows them a tenth of a second at a time. Replayed in between.
    if (Layout::Active() || !s_g1Throttle.TryReplay(renderer))
    {
    RenderThrottle::Capture g1Capture(s_g1Throttle, renderer);
    // X range: the longest lap on screen, in whole 10 s steps.
    double xMax = g_lapNow > g_current.lengthSec ? g_lapNow : g_current.lengthSec;
    float fastestMs = g_current.minMs, slowestMs = g_current.maxMs;
    for (int i = 0; i < g_histCount; ++i)
    {
        if (g_history[i].lengthSec > xMax) xMax = g_history[i].lengthSec;
        if (g_history[i].minMs < fastestMs) fastestMs = g_history[i].minMs;
        if (g_history[i].maxMs > slowestMs) slowestMs = g_history[i].maxMs;
    }
    // (Single frames no longer set the range: one stray near-zero interval --
    // a pause, a GPU timestamp hiccup -- pushed the top to thousands of FPS
    // and flattened everything onto the axis, 2026-11-25. The fastest 0.1%
    // from the histogram below is used instead.)
    xMax = ceil((xMax < 60.0 ? 60.0 : xMax) / 10.0) * 10.0;
    // Auto-scaled both ends (user): fastest +5% up, slowest -5% down, to tidy steps.
    float yMax = fastestMs < 1e8f ? 1000.0f / fastestMs * 1.05f : 60.0f;
    // The frames' own spread, from the lap histograms: the fastest 0.1% sets
    // the top (the points stay on screen now), the slowest 0.5% the break.
    // The scans themselves run on the percentile worker (4x a second, EcoQoS).
    const PctResults pct = GetPercentiles();
    static double s_slowFrameMs = 0.0, s_fastFrameMs = 1e9;
    if (pct.valid) { s_slowFrameMs = pct.slowMs; s_fastFrameMs = pct.fastMs; }
    const double slowFrameMs = s_slowFrameMs;
    if (s_fastFrameMs < fastestMs) fastestMs = static_cast<float>(s_fastFrameMs);
    (void)slowestMs;
    yMax = fastestMs < 1e8f ? 1000.0f / fastestMs * 1.05f : 60.0f;
    if (!std::isfinite(yMax)) yMax = 60.0f;
    if (yMax > 500.0f) yMax = 500.0f;   // hard cap; faster points clamp to the top
    if (yMax < 60.0f) yMax = 60.0f;
    const float yStep = yMax <= 150.0f ? 25.0f : (yMax <= 300.0f ? 50.0f : 100.0f);
    yMax = ceilf(yMax / yStep) * yStep;
    // Axis break (user): when almost no frames (0.5%) are below some FPS,
    // 0..that FPS is squeezed into the bottom 8% with a break mark, so the
    // range the frames actually use gets the height.
    float yBreak = 0.0f;
    if (slowFrameMs > 0.0)
    {
        const float b = floorf(static_cast<float>(1000.0 / slowFrameMs) * 0.9f / yStep) * yStep;
        if (b >= 2.0f * yStep && b < yMax - yStep) yBreak = b;
    }
    const float g1h = g1Bot - g1Top;
    const float breakH = yBreak > 0.0f ? g1h * 0.08f : 0.0f;
    auto X = [&](double t) { return gx0 + static_cast<float>(t / xMax) * gw; };
    auto YF = [&](float fps) {
        if (fps > yMax) fps = yMax;
        if (fps < 0.0f) fps = 0.0f;
        if (yBreak > 0.0f && fps < yBreak) return g1Bot - fps / yBreak * breakH;
        return g1Bot - breakH - (fps - yBreak) / (yMax - yBreak) * (g1h - breakH);
    };
    auto Y = [&](float ms) { return YF(ms > 0.0f ? 1000.0f / ms : 0.0f); };

    // Grid and tick labels.
    renderer.AddLine(gx0, g1Bot, gx1, g1Bot, 1.0f * s, 1.0f, 1.0f, 1.0f, 0.35f);
    renderer.AddText(gx0 - 6.0f * s, g1Bot - labelPx * 0.5f, "0", labelPx, 0.7f, 0.7f, 0.7f, 1.0f, Renderer::HAlign::Right);
    {
        const int yLines = static_cast<int>((yMax - yBreak) / yStep + 0.5f);
        const int yLabelEvery = yLines > 8 ? 2 : 1;
        for (int i = (yBreak > 0.0f ? 0 : 1); i <= yLines; ++i)
        {
            const float v = yBreak + i * yStep;
            const float yy = YF(v);
            renderer.AddLine(gx0, yy, gx1, yy, 1.0f * s, 1.0f, 1.0f, 1.0f, 0.10f);
            if (i % yLabelEvery == 0 || i == 0)
            {
                char buf[16]; sprintf_s(buf, "%.0f", v);
                renderer.AddText(gx0 - 6.0f * s, yy - labelPx * 0.5f, buf, labelPx, 0.7f, 0.7f, 0.7f, 1.0f, Renderer::HAlign::Right);
            }
        }
        if (yBreak > 0.0f)
        {
            // Break mark: two short slashes on the axis between 0 and the break.
            const float my = g1Bot - breakH * 0.5f, d = 5.0f * s;
            for (int k = 0; k < 2; ++k)
            {
                const float oy = my + (k == 0 ? -d * 0.6f : d * 0.6f);
                renderer.AddLine(gx0 - d, oy + d * 0.5f, gx0 + d, oy - d * 0.5f, 1.5f * s, 0.85f, 0.85f, 0.85f, 0.9f);
            }
        }
    }
    const double xStep = xMax <= 120.0 ? 10.0 : (xMax <= 300.0 ? 30.0 : 60.0);
    for (double tt = 0.0; tt <= xMax + 0.001; tt += xStep)
    {
        const float xx = X(tt);
        renderer.AddLine(xx, g1Top, xx, g1Bot, 1.0f * s, 1.0f, 1.0f, 1.0f, 0.06f);
        char buf[16]; sprintf_s(buf, "%.0f", tt);
        // A number that would run into the "s" unit label at the end is left out.
        if (xx + renderer.MeasureText(buf, labelPx) * 0.5f < gx1 - renderer.MeasureText("s", labelPx) - 6.0f * s)
            renderer.AddText(xx, g1Bot + 2.0f * s, buf, labelPx, 0.7f, 0.7f, 0.7f, 1.0f, Renderer::HAlign::Center);
    }
    renderer.AddText(gx1, g1Bot + 2.0f * s, "s", labelPx, 0.7f, 0.7f, 0.7f, 1.0f, Renderer::HAlign::Right);
    // Top axis: lap distance (user), placed where the laps' average distance
    // reaches each 0.5 km (1 km on tracks over 6 km) -- so time and distance
    // both read off the same graph.
    {
        const double len = s_lapLen.load(std::memory_order_relaxed);
        if (len > 100.0 && g_accBuckets > 0)
        {
            const double dStep = len > 6000.0 ? 1000.0 : 500.0;
            const float ly = g1Top - labelPx - 2.0f * s;
            const float kmW = renderer.MeasureText("km", labelPx);
            renderer.AddText(gx1, ly, "km", labelPx, 0.7f, 0.7f, 0.7f, 1.0f, Renderer::HAlign::Right);
            int b = 0;
            for (double d = dStep; d < len; d += dStep)
            {
                while (b < g_accBuckets && !(g_accDistN[b] && g_accDist[b] / g_accDistN[b] >= d)) ++b;
                if (b >= g_accBuckets) break;
                const float xx = X((b + 0.5) * kBucketSec);
                renderer.AddLine(xx, g1Top, xx, g1Top + 6.0f * s, 1.5f * s, 0.7f, 0.7f, 0.7f, 0.8f);
                char buf[16]; sprintf_s(buf, "%.1f", d / 1000.0);
                if (xx + renderer.MeasureText(buf, labelPx) * 0.5f < gx1 - kmW - 6.0f * s)
                    renderer.AddText(xx, ly, buf, labelPx, 0.7f, 0.7f, 0.7f, 1.0f, Renderer::HAlign::Center);
            }
        }
    }

    // One stored lap line, thinned so segments are at least ~3 px long:
    // keeps vertex count flat however long the lap is.
    const float pxPerBucket = static_cast<float>(kBucketSec / xMax) * gw;
    const int step = pxPerBucket >= 3.0f ? 1 : static_cast<int>(ceilf(3.0f / (pxPerBucket > 0.01f ? pxPerBucket : 0.01f)));
    // History laps are drawn coarser (segments ~8 px) than the current lap.
    const int stepHist = (std::max)(step, static_cast<int>(ceilf(8.0f / (pxPerBucket > 0.01f ? pxPerBucket : 0.01f))));
    auto drawLine = [&](const LapLine& l, int sIdx, int lastBucket, float alpha, float w, const float* col, int stp) {
        float px = 0.0f, py = 0.0f; bool have = false;
        for (int b = 0; b < lastBucket; b += stp)
        {
            double sum = 0.0; int n = 0;
            for (int k = b; k < b + stp && k < lastBucket; ++k)
                if (l.ms[sIdx][k] > 0.0f) { sum += l.ms[sIdx][k]; ++n; }
            if (n == 0) { have = false; continue; }   // gap: break the line
            const float xx = X((b + stp * 0.5) * kBucketSec);
            const float yy = Y(static_cast<float>(sum / n));
            if (have)
                renderer.AddLine(px, py, xx, yy, w, col[0], col[1], col[2], alpha);
            px = xx; py = yy; have = true;
        }
    };
    // No per-lap lines any more (user): this lap's dots, then the one
    // accumulated-average line per series drawn below.
    (void)stepHist;
    // Current lap's points: every frame's cell, kept for the whole lap.
    // Time cells are merged until a column is >= 2 px wide, and each column's
    // contiguous lit FPS cells become one rectangle -- a few per column
    // instead of one per cell (vertex budget).
    {
        const float pxPerCell = static_cast<float>(kCellSec / xMax) * gw;
        const int tm = (std::max)(1, static_cast<int>(ceilf(2.0f * s / (pxPerCell > 0.01f ? pxPerCell : 0.01f))));
        const float half = (std::max)(0.75f * s, pxPerCell * tm * 0.5f);
        for (int sIdx = 0; sIdx < kSeries && !overBudget(); ++sIdx)
            for (int ct = 0; ct < g_cellTUsed && ct < kCellT; ct += tm)
            {
                unsigned long long col[kCellWords] = {};
                for (int k = ct; k < ct + tm && k < kCellT; ++k)
                    for (int w = 0; w < kCellWords; ++w) col[w] |= g_cells[sIdx][k][w];
                const float xx = X((ct + tm * 0.5) * kCellSec);
                // Runs of lit bits, found a word at a time (next set bit, then
                // next clear bit) rather than bit by bit.
                const int nBits = kCellWords * 64;
                auto nextBit = [&](int from, bool set) -> int {
                    for (int w = from >> 6; w < kCellWords; ++w)
                    {
                        unsigned long long v = set ? col[w] : ~col[w];
                        if (w == (from >> 6)) v &= ~0ull << (from & 63);
                        if (v) { unsigned long i; _BitScanForward64(&i, v); return w * 64 + static_cast<int>(i); }
                    }
                    return nBits;
                };
                for (int bit = nextBit(0, true); bit < nBits; )
                {
                    const int end = nextBit(bit, false);
                    const float yLo = YF(bit * kCellFps), yHi = YF(end * kCellFps);
                    const float hh = (std::max)(0.75f * s, (yLo - yHi) * 0.5f);
                    renderer.AddFilledRect(xx, (yLo + yHi) * 0.5f, half, hh, 0.0f,
                                           kColour[sIdx][0], kColour[sIdx][1], kColour[sIdx][2], 0.6f);
                    if (end >= nBits) break;
                    bit = nextBit(end, true);
                }
            }
    }
    // The accumulated average over all laps at each point of the lap, with
    // an outline so it reads over the dots (white around CPU and GPU, dark
    // around Present). The bucket being filled now is included, so the line
    // reaches the current position. Segments ~3 px.
    if (g_accBuckets > 0)
    {
        static const float white[3] = { 1.0f, 1.0f, 1.0f }, dark[3] = { 0.05f, 0.05f, 0.05f };
        const int lastB = g_accBuckets;
        auto drawAcc = [&](int sIdx, float alpha, float w, const float* col) {
            float px = 0.0f, py = 0.0f; bool have = false;
            for (int b0 = 0; b0 < lastB && b0 < kMaxBuckets; b0 += step)
            {
                double sum = 0.0; unsigned long long n = 0;
                for (int k = b0; k < b0 + step && k < lastB && k < kMaxBuckets; ++k)
                {
                    sum += g_accSum[sIdx][k]; n += g_accN[sIdx][k];
                }
                if (n == 0) { have = false; continue; }
                const float xx = X((b0 + step * 0.5) * kBucketSec);
                const float yy = Y(static_cast<float>(sum / n));
                if (have) renderer.AddLine(px, py, xx, yy, w, col[0], col[1], col[2], alpha);
                px = xx; py = yy; have = true;
            }
        };
        // Present last, on top: when the GPU is the limit its line sits
        // exactly on the GPU line and would otherwise be hidden under it.
        static const int order[kSeries] = { 1, 2, 0 };
        for (int oi = 0; oi < kSeries; ++oi)
        {
            const int sIdx = order[oi];
            drawAcc(sIdx, 0.9f, lineW + 3.0f * s, sIdx == 0 ? dark : white);
            drawAcc(sIdx, 1.0f, lineW, kColour[sIdx]);
        }
    }
    // Newest frame per series: one bigger dot, so "now" is visible.
    if (g_lapNow >= 0.0)
        for (int sIdx = 0; sIdx < kSeries; ++sIdx)
        {
            if (g_dotCount[sIdx] == 0) continue;
            const Dot& d = g_dots[sIdx][(g_dotNext[sIdx] - 1 + kMaxDots) % kMaxDots];
            if (g_wallNow - d.wall > 0.5) continue;
            renderer.AddFilledCircle(X(d.lapT), Y(d.ms), dotR * 2.0f, kColour[sIdx][0], kColour[sIdx][1], kColour[sIdx][2], 1.0f, 12);
        }

    // ---- Stats, top-left inside graph 1 ----
    //            Present  CPU  GPU
    //   avg        133    195  133
    //   1% low      98    141   99
    //   0.1% low    71    102   74
    // Avg = frames / time over the lap. Lows = FPS at the 99th / 99.9th
    // percentile frame time, from the lap's histogram; "--" under 1000
    // frames, "<20" when it falls in the 50 ms+ bin.
    {
        auto pctLow = [&](int sIdx, double frac, char* out, size_t n) {
            strcpy_s(out, n, pct.valid ? pct.low[sIdx][frac < 0.995 ? 0 : 1] : "--");   // from the percentile worker
        };
        static const char* const names[kSeries] = { "Present", "CPU", "GPU" };
        static const char* const rows[3] = { "avg", "1% low", "0.1% low" };
        const float tp = labelPx;
        const float rowH = tp * 1.15f;
        const float labW = renderer.MeasureText("0.1% low", tp) + 12.0f * s;
        const float colW = renderer.MeasureText("Present", tp) + 14.0f * s;
        // Bottom-left, by the axis origin (user): the frames rarely reach
        // down there, so the table covers the least.
        const float tw = labW + colW * kSeries, th = rowH * 4.0f;
        const float tx = gx0 + 8.0f * s, ty = g1Bot - th - 8.0f * s;
        renderer.AddFilledRoundedRect(tx + tw * 0.5f, ty + th * 0.5f, tw * 0.5f + 6.0f * s, th * 0.5f + 4.0f * s, 6.0f * s, 0.0f,
                                      0.05f, 0.05f, 0.05f, 0.75f);
        for (int sIdx = 0; sIdx < kSeries; ++sIdx)
        {
            const float colX = tx + labW + colW * (sIdx + 1) - 6.0f * s;
            renderer.AddText(colX, ty, names[sIdx], tp, kColour[sIdx][0], kColour[sIdx][1], kColour[sIdx][2], 1.0f, Renderer::HAlign::Right);
            char v[3][16];
            const unsigned lf = g_current.frames[sIdx];
            if (lf > 0 && g_current.sumMs[sIdx] > 0.0) sprintf_s(v[0], "%.0f", 1000.0 * lf / g_current.sumMs[sIdx]);
            else strcpy_s(v[0], "--");
            pctLow(sIdx, 0.99, v[1], sizeof(v[1]));
            pctLow(sIdx, 0.999, v[2], sizeof(v[2]));
            for (int r = 0; r < 3; ++r)
                renderer.AddText(colX, ty + rowH * (r + 1), v[r], tp, kColour[sIdx][0], kColour[sIdx][1], kColour[sIdx][2], 1.0f, Renderer::HAlign::Right);
        }
        for (int r = 0; r < 3; ++r)
            renderer.AddText(tx, ty + rowH * (r + 1), rows[r], tp, 0.75f, 0.75f, 0.75f, 1.0f);
        renderer.AddText(gx1 - 4.0f * s, g1Top + 4.0f * s, "FPS", tp, 0.7f, 0.7f, 0.7f, 1.0f, Renderer::HAlign::Right);
        // GPU caveat, only when it applies: the timestamp interval counts
        // idle GPU time, so the red line is not GPU work when the GPU is NOT
        // the limit. Busy unknown = say nothing.
        double busy = 0.0;
        char wbuf[96] = "";
        if (g_qFailed)
            sprintf_s(wbuf, "GPU timestamps unavailable on this device");
        else if (GpuEngineUsage::GetBusyPercent(busy) && busy < cfg.gpuBusyWarnPercent)
            sprintf_s(wbuf, "GPU not limiting (busy %.0f%%) -- red includes GPU idle", busy);
        if (wbuf[0])
        {
            const float wy = ty - tp - 12.0f * s;   // just above the table
            const float ww = renderer.MeasureText(wbuf, tp);
            renderer.AddFilledRoundedRect(tx + ww * 0.5f, wy + tp * 0.5f, ww * 0.5f + 6.0f * s, tp * 0.5f + 3.0f * s, 6.0f * s, 0.0f,
                                          0.05f, 0.05f, 0.05f, 0.75f);
            renderer.AddText(tx, wy, wbuf, tp, 1.0f, 0.75f, 0.2f, 1.0f);
        }
    }

    }   // graph 1

    // ---- Graph 2: histogram, frame time (0.01 ms bins) or FPS (0.1 FPS
    // bins); a click on it switches (user). Rebuilt once a second (user):
    // each second's frames are added at once. Replayed in between.
    if (Layout::Active() || !s_g2Throttle.TryReplay(renderer))
    {
    RenderThrottle::Capture g2Capture(s_g2Throttle, renderer);
    const bool fm = g_histFps;
    const int NB = fm ? kFBins : kBins;
    const double BW = fm ? kFBinFps : kBinMs;
    auto HP = [&](const LapLine& l, int sIdx) -> const unsigned* { return fm ? l.fhist[sIdx] : l.hist[sIdx]; };
    // X range: 0 .. where nearly all frames are (ms: 99.5%, 5 ms steps,
    // 10..20 ms; FPS: 99.9%, 10 FPS steps, at most 500). Axis break: when
    // under 0.5% of frames are below some value, 0..that is squeezed into
    // the left 6% with a break mark.
    // Refreshed 4x a second (and on a mode switch), like graph 1's scans.
    // From the percentile worker (both modes kept, so a switch is instant).
    const PctResults pct2 = GetPercentiles();
    double vMax = pct2.vMax[fm ? 1 : 0], vLow = pct2.vLow[fm ? 1 : 0];
    if (fm) { vMax = ceil((vMax > 30.0 ? vMax : 30.0) / 10.0) * 10.0; if (vMax > 500.0) vMax = 500.0; }
    else { vMax = ceil(vMax / 5.0) * 5.0; if (vMax > 20.0) vMax = 20.0; }   // user: 0-20 ms at most
    double vBreak = 0.0;
    if (vLow < 1e8)
    {
        const double b = fm ? floor(vLow * 0.9 / 10.0) * 10.0 : floor(vLow * 0.9);
        const double minB = fm ? 20.0 : 2.0;
        if (b >= minB && b < vMax - minB) vBreak = b;
    }
    const int vBins = (std::min)(NB, static_cast<int>(vMax / BW + 0.5));
    const float vBreakW = vBreak > 0.0 ? gw * 0.06f : 0.0f;
    auto HX = [&](double v) {
        if (vBreak > 0.0 && v < vBreak) return gx0 + static_cast<float>(v / vBreak) * vBreakW;
        return gx0 + vBreakW + static_cast<float>((v - vBreak) / (vMax - vBreak)) * (gw - vBreakW);
    };
    // Bins finer than the pixels: the line averages groups ~1.5 px wide
    // (still % per bin).
    const int grp = (std::max)(1, static_cast<int>(ceilf(vBins / ((gw > 1.0f ? gw : 1.0f) / 1.5f))));

    renderer.AddLine(gx0, g2Bot, gx1, g2Bot, 1.0f * s, 1.0f, 1.0f, 1.0f, 0.35f);
    {
        const double span = vMax - vBreak;
        const double vStep = fm ? (span <= 50.0 ? 10.0 : (span <= 150.0 ? 25.0 : (span <= 300.0 ? 50.0 : 100.0)))
                                : (span <= 5.0 ? 1.0 : (span <= 10.0 ? 2.0 : 5.0));
        if (vBreak > 0.0)
        {
            renderer.AddText(gx0, g2Bot + 2.0f * s, "0", labelPx, 0.7f, 0.7f, 0.7f, 1.0f, Renderer::HAlign::Center);
            const float bx = gx0 + vBreakW * 0.5f, d = 5.0f * s;
            for (int k = 0; k < 2; ++k)
            {
                const float ox = bx + (k == 0 ? -d * 0.6f : d * 0.6f);
                renderer.AddLine(ox - d * 0.5f, g2Bot + d, ox + d * 0.5f, g2Bot - d, 1.5f * s, 0.85f, 0.85f, 0.85f, 0.9f);
            }
        }
        for (double v = vBreak; v <= vMax + 0.001; v += vStep)
        {
            const float xx = HX(v);
            renderer.AddLine(xx, g2Top, xx, g2Bot, 1.0f * s, 1.0f, 1.0f, 1.0f, 0.06f);
            char buf[16]; sprintf_s(buf, "%.0f", v);
            // Left out where it would run into the unit label at the end.
            if (xx + renderer.MeasureText(buf, labelPx) * 0.5f < gx1 - renderer.MeasureText(fm ? "FPS" : "ms", labelPx) - 6.0f * s)
                renderer.AddText(xx, g2Bot + 2.0f * s, buf, labelPx, 0.7f, 0.7f, 0.7f, 1.0f, Renderer::HAlign::Center);
        }
        renderer.AddText(gx1, g2Bot + 2.0f * s, fm ? "FPS" : "ms", labelPx, 0.7f, 0.7f, 0.7f, 1.0f, Renderer::HAlign::Right);
        renderer.AddText(gx1 - 4.0f * s, g2Top + 2.0f * s, fm ? "click: frame time" : "click: FPS", labelPx * 0.8f,
                         0.6f, 0.6f, 0.6f, 0.8f, Renderer::HAlign::Right);
    }
    // Dotted FPS markers (user list). Labels turned 90 degrees (user), at
    // the top inside the graph, so close lines (140/144)
    // never collide and all labels sit at the same height; the dashes start
    // under the label.
    {
        static const int fpsMarks[] = { 60, 90, 120, 140, 144, 160, 240 };
        const int nMarks = static_cast<int>(sizeof(fpsMarks) / sizeof(fpsMarks[0]));
        const float dash = 5.0f * s;
        const float digPx = labelPx * 0.75f;
        for (int i = 0; i < nMarks; ++i)
        {
            const double v = fm ? static_cast<double>(fpsMarks[i]) : 1000.0 / fpsMarks[i];
            if (v > vMax || v < vBreak) continue;
            const float xx = HX(v);
            char buf[8]; sprintf_s(buf, "%d", fpsMarks[i]);
            // Turned 90 degrees, reading upward, centred on the line, just
            // above the graph; the line runs the full height (user).
            renderer.AddTextRotated(xx - digPx * 0.5f, g2Top - 3.0f * s, buf, digPx, 1.0f, 0.85f, 0.3f, 0.95f);
            for (float yy = g2Top; yy < g2Bot; yy += dash * 2.0f)
                renderer.AddLine(xx, yy, xx, (yy + dash < g2Bot ? yy + dash : g2Bot), 1.5f * s, 1.0f, 0.85f, 0.3f, 0.55f);
        }
    }
    // Bin width by amount of data (user): 0.1 ms (1 FPS) while a lap has few
    // frames, narrowing to 0.05 / 0.02 / 0.01 ms as it fills, so the curve is
    // not noise. Applied per lap from its own frame count.
    auto dataGrp = [](unsigned long long n) { return n < 20000 ? 10 : (n < 50000 ? 5 : (n < 100000 ? 2 : 1)); };
    // Y scale: the highest (grouped) bin on screen, shared by all laps.
    float pctMax = 0.0f;
    auto scanPct = [&](const unsigned* h, unsigned long long n) {
        if (n == 0) return;
        const int g = (std::max)(grp, dataGrp(n));
        for (int k = 0; k < vBins; k += g)
        {
            unsigned long long sum = 0; int m = 0;
            for (int j = k; j < k + g && j < vBins; ++j) { sum += h[j]; ++m; }
            const float p = 100.0f * sum / (static_cast<float>(n) * m);
            if (p > pctMax) pctMax = p;
        }
    };
    for (int sIdx = 0; sIdx < kSeries; ++sIdx)
    {
        scanPct(HP(g_current, sIdx), g_current.frames[sIdx]);
        scanPct(fm ? g_accFHist[sIdx] : g_accHist[sIdx], g_accHistFrames[sIdx]);
    }
    // Scaled as density -- % of frames per 1 ms (frame time)
    // or per 10 FPS (FPS mode), so the numbers do not depend on the bin
    // size. Internally the scale stays % per bin; F converts.
    {
        const float F = static_cast<float>((fm ? 10.0 : 1.0) / BW);
        float dispMax = pctMax * F;
        const float st = dispMax <= 2.0f ? 0.5f : (dispMax <= 5.0f ? 1.0f : (dispMax <= 20.0f ? 5.0f : (dispMax <= 50.0f ? 10.0f : 20.0f)));
        dispMax = dispMax > 0.0f ? ceilf(dispMax / st) * st : 1.0f;
        pctMax = dispMax / F;
        for (float p = st; p <= dispMax + 0.001f; p += st)
        {
            const float yy = g2Bot - p / dispMax * g2h;
            renderer.AddLine(gx0, yy, gx1, yy, 1.0f * s, 1.0f, 1.0f, 1.0f, 0.06f);
        }
        // No numbers on this axis (user, 2026-11-24): the height is read as
        // shape only; faint gridlines stay as a guide.
    }
    // History curves coarser (~6 px groups) than the current lap's (vertex budget).
    const int grpHist = (std::max)(grp, static_cast<int>(ceilf(vBins / ((gw > 1.0f ? gw : 1.0f) / 6.0f))));
    auto drawHist = [&](const unsigned* h, unsigned long long n, int sIdx, float alpha, float w, int g) {
        if (n == 0) return;
        g = (std::max)(g, dataGrp(n));
        float px = HX(0.0), py = g2Bot;
        for (int k = 0; k < vBins; k += g)
        {
            unsigned long long sum = 0; int m = 0;
            for (int j = k; j < k + g && j < vBins; ++j) { sum += h[j]; ++m; }
            const float xx = HX((k + m * 0.5) * BW);
            const float yy = g2Bot - (100.0f * sum / (static_cast<float>(n) * m)) / pctMax * g2h;
            renderer.AddLine(px, py, xx, yy, w, kColour[sIdx][0], kColour[sIdx][1], kColour[sIdx][2], alpha);
            px = xx; py = yy;
        }
    };
    // No per-lap curves (user): this lap's dots, then the average
    // distribution line drawn last.
    (void)grpHist;
    // 1% and 0.1% lows (user): a light full-height band in each series'
    // colour where its slowest 1% of frames lie, a darker one for the slowest
    // 0.1%, and a dashed full-height line at each cut labelled at the top.
    // Same cut points as the table; none under 1000 frames. Slow frames are
    // the right-hand side in frame time, the left-hand side in FPS.
    {
        auto cutBin = [&](const unsigned* h, unsigned long long tot, double frac) -> int {
            const unsigned long long want = static_cast<unsigned long long>(ceil(tot * frac));
            unsigned long long run = 0;
            for (int k = 0; k < NB; ++k) { run += h[k]; if (run >= want) return k; }
            return NB - 1;
        };
        const float dash = 5.0f * s, tagPx = labelPx * 0.7f;
        for (int sIdx = 0; sIdx < kSeries; ++sIdx)
        {
            const unsigned long long n = g_current.frames[sIdx];
            if (n < 1000) continue;
            const unsigned* h = HP(g_current, sIdx);
            const int c1 = fm ? cutBin(h, n, 0.01) : cutBin(h, n, 0.99);
            const int c2 = fm ? cutBin(h, n, 0.001) : cutBin(h, n, 0.999);
            for (int pass = 0; pass < 2; ++pass)
            {
                const int cut = pass == 0 ? c1 : c2;
                const double v = (cut + (fm ? 1.0 : 0.0)) * BW;
                if (v > vMax || v < vBreak) continue;
                const float xc = HX(v);
                const float xa = fm ? gx0 : xc, xb = fm ? xc : gx1;
                if (xb > xa)
                    renderer.AddFilledRect((xa + xb) * 0.5f, (g2Top + g2Bot) * 0.5f, (xb - xa) * 0.5f, g2h * 0.5f, 0.0f,
                                           kColour[sIdx][0], kColour[sIdx][1], kColour[sIdx][2], pass == 0 ? 0.06f : 0.10f);
                for (float yy = g2Top + tagPx * 1.1f * (kSeries + 0.2f); yy < g2Bot; yy += dash * 2.0f)
                    renderer.AddLine(xc, yy, xc, (yy + dash < g2Bot ? yy + dash : g2Bot), (pass == 0 ? 1.2f : 2.0f) * s,
                                     kColour[sIdx][0], kColour[sIdx][1], kColour[sIdx][2], 0.85f);
                // Labels stacked by series so the three never sit on each other.
                renderer.AddText(xc, g2Top + 2.0f * s + sIdx * tagPx * 1.1f, pass == 0 ? "1%" : "0.1%", tagPx,
                                 kColour[sIdx][0], kColour[sIdx][1], kColour[sIdx][2], 1.0f, Renderer::HAlign::Center);
            }
        }
    }

    // Dots (user, no fade): the current lap's bins at their own %, grouped
    // like its curve (0.1 ms early in the lap, finer as frames add up): one
    // small square per non-empty group at that group's level.
    {
        const float d = dotR * 0.8f;
        for (int sIdx = 0; sIdx < kSeries && !overBudget(); ++sIdx)
        {
            const unsigned long long nNow = g_current.frames[sIdx];
            if (nNow == 0) continue;
            const unsigned* h = HP(g_current, sIdx);
            const int g = (std::max)(grp, dataGrp(nNow));
            for (int k = 0; k < vBins; k += g)
            {
                unsigned long long sum = 0; int m = 0;
                for (int j = k; j < k + g && j < vBins; ++j) { sum += h[j]; ++m; }
                if (sum == 0) continue;
                float c = (100.0f * sum / (static_cast<float>(nNow) * m)) / pctMax;
                if (c > 1.0f) c = 1.0f;
                renderer.AddFilledRect(HX((k + m * 0.5) * BW), g2Bot - c * g2h, d, d, 0.0f,
                                       kColour[sIdx][0], kColour[sIdx][1], kColour[sIdx][2], 0.7f);
            }
        }
    }

    // The average distribution over all finished laps, last, on top of the
    // dots and bands, outlined like the FPS graph's line (user: it was hard
    // to see under the dots). Present on top. None until a lap is finished.
    {
        static const int order[kSeries] = { 1, 2, 0 };
        for (int oi = 0; oi < kSeries; ++oi)
        {
            const int sIdx = order[oi];
            const float oc[3] = { sIdx == 0 ? 0.05f : 1.0f, sIdx == 0 ? 0.05f : 1.0f, sIdx == 0 ? 0.05f : 1.0f };
            const float savedR = kColour[sIdx][0]; (void)savedR;
            // Outline pass: drawHist uses the series colour, so draw the outline directly.
            const unsigned long long n = g_accHistFrames[sIdx];
            if (n == 0) continue;
            const unsigned* h = fm ? g_accFHist[sIdx] : g_accHist[sIdx];
            const int g = (std::max)(grp, dataGrp(n));
            float px = HX(0.0), py = g2Bot;
            for (int k = 0; k < vBins; k += g)
            {
                unsigned long long sum = 0; int m = 0;
                for (int j = k; j < k + g && j < vBins; ++j) { sum += h[j]; ++m; }
                const float xx = HX((k + m * 0.5) * BW);
                const float yy = g2Bot - (100.0f * sum / (static_cast<float>(n) * m)) / pctMax * g2h;
                renderer.AddLine(px, py, xx, yy, lineW + 3.0f * s, oc[0], oc[1], oc[2], 0.9f);
                px = xx; py = yy;
            }
            drawHist(h, n, sIdx, 1.0f, lineW, grp);
        }
    }
    }   // graph 2

    renderer.SetTextBackground(prevBg);
    return true;
}
