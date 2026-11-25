#include "TyreSave.h"
#include "TyreWear.h"
#include "FuelStore.h"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "Log.h"
#include "Persist.h"
#include "RenderThrottle.h"
#include "StoreFile.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

// The bookkeeping (Update) runs on the scoring thread and Draw on the render
// thread; neither opens the profile file itself: loading and saving are
// handed to the save thread (TyreSave::FlushIfDirty, run by Persist.cpp) and
// the result picked up on a later update -- the same hand-over as FuelSave.cpp.
namespace
{
    const int kBins = 50;
    float s_profile[4][kBins + 1] = {};   // per tyre: cumulative share of a lap's wear, 0..1
    bool s_haveProfile[4] = {};
    char s_profilePath[MAX_PATH] = "";
    char s_fuelProfilePath[MAX_PATH] = "";
    char s_seedPath[MAX_PATH] = "";       // old per-class profile, read if s_profilePath is missing

    float s_lapWorn[4][kBins + 1];        // this lap: wear % on reaching each bin, -1 = not reached
    bool s_lapClean = false;
    bool s_lapDriven = false;   // driven by you all lap: gates learning only (review F252)

    long s_lap = -1;
    double s_wornAtLapStart[4] = { -1, -1, -1, -1 }; // wear % (0 = fresh) at the current lap's start
    double s_prevRemaining[4] = { -1, -1, -1, -1 };
    double s_target[4] = { -1, -1, -1, -1 };          // required wear %/lap, this lap
    double s_lastLap[4] = { -1, -1, -1, -1 };         // last lap's wear %
    double s_lastLapTarget[4] = { -1, -1, -1, -1 };   // what was required on that lap
    double s_wearSum[4] = {};             // this set: summed wear % of completed laps
    int s_wearLaps[4] = {};               //            and how many laps
    // Driving time on each wheel's set: a fronts-only stop restarts only the
    // fronts' clocks (review F25).
    double s_setSeconds[4] = {};
    double s_driveSeconds = 0.0;          // driving time, never reset
    double s_driveAtLapStart = -1.0;
    double s_pace = 0.0;                  // clean-lap pace for laps left (review F21)
    float s_cfgMinutes = -1.0f, s_cfgWear = -1.0f; // target settings the targets were worked from
    bool s_fracSettle = false;            // lap changed, scoring lapFrac not yet in the new lap (F26)
    std::atomic<bool> s_resetRequested{ false };
    double s_lapFracNow = 0.0;            // settled lap fraction, for Draw's live tint

    // Everything above: written by Update (scoring thread, exclusive), read
    // by Draw (render thread, shared). Review F25.
    SRWLOCK s_stateLock = SRWLOCK_INIT;

    // Hand-over between Update (scoring thread) and FlushIfDirty (save thread).
    // s_ioLock is only ever held to copy these, never across file I/O.
    SRWLOCK s_ioLock = SRWLOCK_INIT;
    struct ProfileIo
    {
        bool saveDirty = false;               // a learned profile is waiting to be written
        char savePath[MAX_PATH] = "";
        float save[4][kBins + 1] = {};
        bool loadPending = false;             // Draw wants loadPath read
        char loadPath[MAX_PATH] = "";
        char seedPath[MAX_PATH] = "";
        bool loadDone = false;                // read: result below, for loadPath
        bool loadOk = false;
        float loaded[4][kBins + 1] = {};
    } s_io;
    std::atomic<bool> s_loadReady{ false };      // loadDone, checked without the lock
    bool s_loadWaiting = false;                  // render thread: s_profilePath's file not applied yet

    // Update: a new car+track's profile (s_profilePath already set).
    // Until it arrives each tyre's share is the plain distance fraction,
    // exactly as for a car+track with no profile yet.
    void RequestLoad()
    {
        for (int w = 0; w < 4; ++w) s_haveProfile[w] = false;
        s_loadWaiting = true;
        AcquireSRWLockExclusive(&s_ioLock);
        strcpy_s(s_io.loadPath, s_profilePath);
        strcpy_s(s_io.seedPath, s_seedPath);
        s_io.loadPending = true;
        s_io.loadDone = false;
        s_loadReady.store(false);
        ReleaseSRWLockExclusive(&s_ioLock);
        // Load only: a car change mid-lap must not write anything (review F275).
        Persist::RequestLoad();
    }

    // Update, each call while a load is outstanding.
    void ApplyLoadIfReady()
    {
        if (!s_loadWaiting || !s_loadReady.load())
            return;
        AcquireSRWLockExclusive(&s_ioLock);
        const bool mine = s_io.loadDone && strcmp(s_io.loadPath, s_profilePath) == 0;
        if (mine)
        {
            if (s_io.loadOk)
                memcpy(s_profile, s_io.loaded, sizeof(s_profile));
            for (int w = 0; w < 4; ++w) s_haveProfile[w] = s_io.loadOk;
        }
        s_io.loadDone = false;
        s_loadReady.store(false);
        ReleaseSRWLockExclusive(&s_ioLock);
        if (mine)
            s_loadWaiting = false;
    }

    // Update, after learning a lap: written by the next save pass.
    void QueueSave()
    {
        if (!s_profilePath[0]) return;
        for (int w = 0; w < 4; ++w) if (!s_haveProfile[w]) return;
        AcquireSRWLockExclusive(&s_ioLock);
        strcpy_s(s_io.savePath, s_profilePath);
        memcpy(s_io.save, s_profile, sizeof(s_io.save));
        s_io.saveDirty = true;
        ReleaseSRWLockExclusive(&s_ioLock);
        // Learned at the line, but a frame or more after the line's own
        // save request may already have been served.
        Persist::RequestLearned();
    }

    // Save thread only.
    bool ReadProfile(const char* path, float out[4][kBins + 1])
    {
        FILE* f = nullptr;
        if (!path[0] || fopen_s(&f, path, "r") != 0 || !f) return false;
        int n = 0;
        for (; n <= kBins; ++n)
            if (fscanf_s(f, "%f %f %f %f", &out[0][n], &out[1][n], &out[2][n], &out[3][n]) != 4) break;
        fclose(f);
        if (n != kBins + 1) return false;
        // Hand-edited file: reject nan/inf, then clamp and order it the way
        // LearnLap would (review F388).
        for (int w = 0; w < 4; ++w)
        {
            for (int i = 0; i <= kBins; ++i)
                if (!std::isfinite(out[w][i])) return false;
            out[w][0] = 0.0f;
            out[w][kBins] = 1.0f;
            for (int i = 1; i < kBins; ++i)
            {
                float v = out[w][i];
                if (v < out[w][i - 1]) v = out[w][i - 1];
                if (v > 1.0f) v = 1.0f;
                out[w][i] = v;
            }
        }
        return true;
    }

    bool WriteProfile(const char* path, const float profile[4][kBins + 1])
    {
        char tmp[StoreFile::kTmpPathSize];
        FILE* f = StoreFile::Begin(path, tmp, sizeof(tmp), "w");
        if (!f) { Log::Write("TyreSave: cannot write %s", path); return false; }
        for (int i = 0; i <= kBins; ++i)
            fprintf(f, "%.5f %.5f %.5f %.5f\n", profile[0][i], profile[1][i], profile[2][i], profile[3][i]);
        if (!StoreFile::Commit(f, tmp, path))
        {
            Log::Write("TyreSave: cannot write %s", path);
            return false;
        }
        return true;
    }

    void LearnLap(int w, double lapTotal)
    {
        if (lapTotal < 0.02) return;
        float share[kBins + 1];
        share[0] = 0.0f;
        share[kBins] = 1.0f;
        int missing = 0;
        for (int i = 1; i < kBins; ++i)
        {
            if (s_lapWorn[w][i] < 0.0f) { ++missing; share[i] = -1.0f; continue; }
            share[i] = static_cast<float>(s_lapWorn[w][i] / lapTotal);
        }
        if (missing > kBins / 10) return;
        for (int i = 1; i < kBins; ++i)
        {
            if (share[i] >= 0.0f) continue;
            int j = i + 1;
            while (j < kBins && share[j] < 0.0f) ++j;
            const float a = share[i - 1], b = share[j];
            share[i] = a + (b - a) / static_cast<float>(j - (i - 1));
        }
        for (int i = 1; i <= kBins; ++i)
        {
            if (share[i] < share[i - 1]) share[i] = share[i - 1];
            if (share[i] > 1.0f) share[i] = 1.0f;
        }
        if (!s_haveProfile[w])
            for (int i = 0; i <= kBins; ++i) s_profile[w][i] = share[i];
        else
            for (int i = 0; i <= kBins; ++i) s_profile[w][i] = 0.7f * s_profile[w][i] + 0.3f * share[i];
        s_haveProfile[w] = true;
    }

    double ShareAt(int w, double frac)
    {
        if (frac <= 0.0) return 0.0;
        if (frac >= 1.0) return 1.0;
        if (!s_haveProfile[w]) return frac;
        const double x = frac * kBins;
        const int i = static_cast<int>(x);
        const double t = x - i;
        return s_profile[w][i] + (s_profile[w][i + 1] - s_profile[w][i]) * t;
    }

    void ResetLapTrace()
    {
        for (int w = 0; w < 4; ++w)
        {
            for (int i = 0; i <= kBins; ++i) s_lapWorn[w][i] = -1.0f;
            s_lapWorn[w][0] = 0.0f;
        }
    }

    // mask: bit per wheel that got fresh tyres.
    void NewSet(unsigned mask = 0xF)
    {
        for (int w = 0; w < 4; ++w)
        {
            if (!(mask & (1u << w))) continue;
            s_setSeconds[w] = 0.0;
            s_target[w] = -1.0; s_lastLap[w] = -1.0; s_lastLapTarget[w] = -1.0; s_wearSum[w] = 0.0; s_wearLaps[w] = 0;
        }
        s_lapClean = false;
        Log::Write("TyreSave: fresh tyres (mask %X), set clock restarted", mask);
    }

    // Required wear per lap: what is left to the target wear (from the lap
    // start), over the laps that still fit in each set's target time.
    // 0 = already at/over the max wear with time left.
    void ComputeTargets(const TyreSaveConfig& cfg, double fallbackLapTime)
    {
        s_cfgMinutes = cfg.targetMinutes;
        s_cfgWear = cfg.targetWearPercent;
        const double lapTime = s_pace > 5.0 ? s_pace : fallbackLapTime;
        if (!(lapTime > 5.0) || s_wornAtLapStart[0] < 0.0) return;
        double lapsLeft[4];
        for (int w = 0; w < 4; ++w)
        {
            lapsLeft[w] = (cfg.targetMinutes * 60.0 - s_setSeconds[w]) / lapTime;
            const double left = cfg.targetWearPercent - s_wornAtLapStart[w];
            s_target[w] = (lapsLeft[w] > 0.5) ? (left > 0.0 ? left / lapsLeft[w] : 0.0) : -1.0;
        }
        Log::Write("TyreSave: lap targets %.3f %.3f %.3f %.3f %%/lap (%.1f laps left FL, pace %.1f s)",
                   s_target[0], s_target[1], s_target[2], s_target[3], lapsLeft[0], lapTime);
    }

    // amber on target -> green under / red over; t in -1..1
    void DeltaColour(double t, float& r, float& g, float& b)
    {
        if (t > 1.0) t = 1.0;
        if (t < -1.0) t = -1.0;
        const float ar = 1.0f, ag = 0.75f, ab = 0.15f;
        if (t >= 0.0)
        {
            const float k = static_cast<float>(t);
            r = ar + (0.95f - ar) * k; g = ag + (0.25f - ag) * k; b = ab + (0.2f - ab) * k;
        }
        else
        {
            const float k = static_cast<float>(-t);
            r = ar + (0.2f - ar) * k; g = ag + (0.85f - ag) * k; b = ab + (0.3f - ab) * k;
        }
    }
}

void TyreSave::RequestReset()
{
    s_resetRequested.store(true);
}

void TyreSave::FlushIfDirty(bool loadOnly)
{
    // Save thread, any time (see Persist.cpp): copy out under s_ioLock,
    // touch the disk after letting go. Writes before reading, so a profile
    // saved and then asked for again comes back as saved.
    bool doSave = false, doLoad = false;
    char savePath[MAX_PATH] = "", loadPath[MAX_PATH] = "", seedPath[MAX_PATH] = "";
    float save[4][kBins + 1];
    AcquireSRWLockExclusive(&s_ioLock);
    if (!loadOnly && s_io.saveDirty)
    {
        strcpy_s(savePath, s_io.savePath);
        memcpy(save, s_io.save, sizeof(save));
        s_io.saveDirty = false;
        doSave = true;
    }
    if (s_io.loadPending)
    {
        strcpy_s(loadPath, s_io.loadPath);
        strcpy_s(seedPath, s_io.seedPath);
        s_io.loadPending = false;
        doLoad = true;
    }
    ReleaseSRWLockExclusive(&s_ioLock);

    // A failed write is queued again (review F278), unless a newer save
    // replaced it; a few tries only, so a read-only folder can't spin.
    static int s_saveFailures = 0;
    if (doSave)
    {
        if (WriteProfile(savePath, save))
            s_saveFailures = 0;
        else if (++s_saveFailures <= 3)
        {
            AcquireSRWLockExclusive(&s_ioLock);
            if (!s_io.saveDirty)
            {
                strcpy_s(s_io.savePath, savePath);
                memcpy(s_io.save, save, sizeof(s_io.save));
                s_io.saveDirty = true;
            }
            ReleaseSRWLockExclusive(&s_ioLock);
        }
    }
    if (doLoad)
    {
        float loaded[4][kBins + 1] = {};
        bool ok = ReadProfile(loadPath, loaded);
        // Per-car keys (2026-11-24): seeded once from the old per-class file.
        if (!ok && seedPath[0] && !StoreFile::Exists(loadPath) && ReadProfile(seedPath, loaded))
        {
            ok = true;
            Log::Write("TyreSave: no tyre profile yet at %s -- seeded from old class file %s", loadPath, seedPath);
        }
        AcquireSRWLockExclusive(&s_ioLock);
        // Dropped if Update has asked for another file in the meantime.
        if (!s_io.loadPending && strcmp(s_io.loadPath, loadPath) == 0)
        {
            memcpy(s_io.loaded, loaded, sizeof(s_io.loaded));
            s_io.loadOk = ok;
            s_io.loadDone = true;
            s_loadReady.store(true);
        }
        ReleaseSRWLockExclusive(&s_ioLock);
    }
}

void TyreSave::Update()
{
    // Scoring thread (~5 Hz, after the fuel and tyre-wear stores), whether
    // or not the panel shows: the set clock and lap bookkeeping used to run
    // in Draw and stopped while the panel was hidden (review F25).
    const FuelStore::State fuel = g_fuelStore.Read();
    const TyreWearStore::State tyres = g_tyreWear.Read();
    if (!fuel.valid || !tyres.valid)
        return;
    const TyreSaveConfig& cfg = g_tyreSaveConfig;

    AcquireSRWLockExclusive(&s_stateLock);
    double lapFrac = fuel.currentLapFractional - floor(fuel.currentLapFractional);
    if (lapFrac < 0.0) lapFrac = 0.0;
    const double lapTime = fuel.lastLapTimeSeconds;
    // Scoring's lap position lags telemetry's lap change by up to 200 ms:
    // hold it at 0 until it has wrapped into the new lap (review F26).
    if (s_fracSettle)
    {
        if (lapFrac < 0.5) s_fracSettle = false;
        else lapFrac = 0.0;
    }

    double worn[4];
    for (int w = 0; w < 4; ++w) worn[w] = 100.0 - tyres.wheel[w].remainingPercent;

    if (s_resetRequested.exchange(false))
    {
        // Session ended: rF2 fits fresh tyres for the next one (F353).
        NewSet();
        s_lap = -1;
        s_pace = 0.0;
        s_driveAtLapStart = -1.0;
        for (int w = 0; w < 4; ++w) { s_prevRemaining[w] = -1.0; s_wornAtLapStart[w] = -1.0; }
        s_fuelProfilePath[0] = 0;   // re-read the profile below
    }
    // Per car+track profile file, next to the fuel one.
    if (strcmp(fuel.profilePath, s_fuelProfilePath) != 0)
    {
        strcpy_s(s_fuelProfilePath, fuel.profilePath);
        strcpy_s(s_profilePath, fuel.profilePath);
        char* dot = strstr(s_profilePath, "_profile.txt");
        if (dot) strcpy_s(dot, sizeof(s_profilePath) - (dot - s_profilePath), "_tyreprofile.txt");
        else s_profilePath[0] = 0;
        strcpy_s(s_seedPath, fuel.legacyProfilePath);
        char* seedDot = strstr(s_seedPath, "_profile.txt");
        if (seedDot) strcpy_s(seedDot, sizeof(s_seedPath) - (seedDot - s_seedPath), "_tyreprofile.txt");
        else s_seedPath[0] = 0;
        RequestLoad();
        s_lap = -1;
        NewSet();
    }
    ApplyLoadIfReady();

    // Fresh tyres (any wheel's remaining life jumps up): new set.
    // Any rise past noise counts, even after <1% wear (review F387); only
    // the changed wheels restart (F25).
    unsigned fresh = 0;
    for (int w = 0; w < 4; ++w)
    {
        if (s_prevRemaining[w] >= 0.0 && tyres.wheel[w].remainingPercent > s_prevRemaining[w] + 0.05)
            fresh |= 1u << w;
        s_prevRemaining[w] = tyres.wheel[w].remainingPercent;
    }
    if (fresh)
    {
        NewSet(fresh);
        for (int w = 0; w < 4; ++w) if (fresh & (1u << w)) s_wornAtLapStart[w] = worn[w];
    }

    if (fuel.telemLapNumber != s_lap)
    {
        if (s_lap >= 0 && fuel.telemLapNumber == s_lap + 1 && s_wornAtLapStart[0] >= 0.0)
        {
            bool learned = false;
            // Pace from clean laps only: a pit or safety-car lap creeps it
            // up slowly, a faster lap pulls it down quickly (review F21).
            if (s_lapClean)
            {
                double cand = lapTime > 5.0 ? lapTime
                            : (s_driveAtLapStart >= 0.0 ? s_driveSeconds - s_driveAtLapStart : 0.0);
                if (cand > 5.0)
                {
                    if (s_pace <= 5.0) s_pace = cand;
                    else if (cand < s_pace) s_pace = 0.5 * (s_pace + cand);
                    else s_pace += 0.2 * ((cand < s_pace * 1.1 ? cand : s_pace * 1.1) - s_pace);
                }
            }
            for (int w = 0; w < 4; ++w)
            {
                const double used = worn[w] - s_wornAtLapStart[w];
                if (used <= 0.0) continue;
                // Partial out-laps and pit laps are not representative (F20).
                if (s_lapClean)
                {
                    s_wearSum[w] += used;
                    ++s_wearLaps[w];
                }
                // 0 = over the max: keep showing Last, it is over budget (F24).
                if (s_target[w] >= 0.0)
                {
                    s_lastLap[w] = used;
                    s_lastLapTarget[w] = s_target[w];
                }
                // Not while the saved profile is still on its way in:
                // learning onto "none yet" would overwrite it.
                if (s_lapClean && s_lapDriven && !s_loadWaiting) { LearnLap(w, used); learned = true; }
            }
            if (learned) QueueSave();
        }
        s_lap = fuel.telemLapNumber;
        for (int w = 0; w < 4; ++w) s_wornAtLapStart[w] = worn[w];
        ResetLapTrace();
        s_lapClean = !fuel.inPits;
        s_lapDriven = true;
        s_driveAtLapStart = s_driveSeconds;
        if (lapFrac > 0.9) { s_fracSettle = true; lapFrac = 0.0; }
        ComputeTargets(cfg, lapTime);
    }
    else if (cfg.targetMinutes != s_cfgMinutes || cfg.targetWearPercent != s_cfgWear)
        ComputeTargets(cfg, lapTime);   // a knob turned: do not wait for the line (F21)

    // Set clock: only time actually driven on track (lap progress moving,
    // not in the pits). Adding whole lap times instead counted any garage
    // or pit-lane wait inside a lap, which could add hours.
    {
        static LARGE_INTEGER s_clkPrev = {};
        static double s_lastProgress = -1.0;
        LARGE_INTEGER q, qf;
        QueryPerformanceCounter(&q);
        QueryPerformanceFrequency(&qf);
        double cdt = s_clkPrev.QuadPart ? static_cast<double>(q.QuadPart - s_clkPrev.QuadPart) / qf.QuadPart : 0.0;
        s_clkPrev = q;
        const bool moving = s_lastProgress >= 0.0 && fuel.currentLapFractional != s_lastProgress;
        s_lastProgress = fuel.currentLapFractional;
        // Called at ~5 Hz with lap progress: stop once it has stalled for 1 s.
        static double s_stall = 0.0;
        s_stall = moving ? 0.0 : s_stall + (cdt > 0.0 && cdt < 0.5 ? cdt : 1.0);
        if (cdt > 0.0 && cdt < 0.5 && !fuel.inPits && s_stall < 1.0)
        {
            s_driveSeconds += cdt;
            for (int w = 0; w < 4; ++w) s_setSeconds[w] += cdt;
        }
    }

    if (fuel.inPits) s_lapClean = false;
    if (!g_localPlayerDriving.load(std::memory_order_relaxed)) s_lapDriven = false;
    const int bin = static_cast<int>(lapFrac * kBins);
    if (bin > 0 && bin < kBins)
        for (int w = 0; w < 4; ++w)
            if (s_lapWorn[w][bin] < 0.0f)
                s_lapWorn[w][bin] = static_cast<float>(worn[w] - s_wornAtLapStart[w]);
    s_lapFracNow = lapFrac;
    ReleaseSRWLockExclusive(&s_stateLock);
}

bool TyreSave::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    TyreSaveConfig& cfg = g_tyreSaveConfig;
    if (!cfg.enabled || cfg.targetMinutes <= 0.0f)
        return false;

    // Medium tier (~25Hz): the box tint is eased over 1.5 s and the numbers
    // change once a lap (the bookkeeping is in Update). Bypassed in
    // layout mode so dragging/resizing stays smooth.
    static RenderThrottle s_throttle(0.04);
    if (!Layout::Active() && s_throttle.TryReplay(renderer))
        return true;
    RenderThrottle::Capture throttleCapture(s_throttle, renderer);

    const FuelStore::State fuel = g_fuelStore.Read();
    const TyreWearStore::State tyres = g_tyreWear.Read();
    const bool placeholder = Layout::Preview();
    const bool live = fuel.valid && tyres.valid;
    if (!live && !placeholder)
        return false;

    // Bookkeeping is done by Update; only read it here.
    AcquireSRWLockShared(&s_stateLock);
    const double lapFrac = s_lapFracNow;
    double worn[4];
    for (int w = 0; w < 4; ++w) worn[w] = 100.0 - tyres.wheel[w].remainingPercent;

    // Geometry (2160p reference): 2x2 boxes in car order, a header above.
    const float s = static_cast<float>(screenHeight) / 2160.0f * (cfg.scale > 0.05f ? cfg.scale : 0.05f);
    const float gap = 10.0f * s;
    const float textPx = 34.0f * s;
    const float headPx = 40.0f * s;
    const float cornerPx = 36.0f * s;   // % and laps left, 50% bigger than the old 24
    const float inset = 6.0f * s;
    // Sized to the content, no dead space (2026-11-23): the corner line plus
    // the three lines stacked tight, and as wide as the widest corner text.
    const float boxHalfW = renderer.MeasureText("100% 99 laps", cornerPx) * 0.5f + 2.0f * inset;
    const float boxHalfH = (cornerPx + 3.0f * textPx) * 0.5f + inset;
    const float cx = screenWidth * cfg.centerXFraction;
    const float cy = screenHeight * cfg.centerYFraction;
    // Registered as wide as the header too, so layout mode covers it (F27).
    const float headHalfW = renderer.MeasureText("TYRES  max 100%  |  100:00 / 100:00", headPx) * 0.5f;
    const float gridHalfW = 2.0f * boxHalfW + gap * 0.5f;
    const float totalHalfW = headHalfW > gridHalfW ? headHalfW : gridHalfW;
    const float totalHalfH = 2.0f * boxHalfH + gap * 0.5f + (headPx + gap) * 0.5f;
    Layout::Register("Tyre wear target", "TyreSave", "CenterXFraction", "CenterYFraction",
                     &cfg.centerXFraction, &cfg.centerYFraction,
                     cx, cy, totalHalfW, totalHalfH, &cfg.scale, "Scale");

    const bool prevBg = renderer.TextBackgroundEnabled();
    renderer.SetTextBackground(false);

    // Header: target and how far into the set's time.
    const float top = cy - totalHalfH;
    {
        // The oldest set's clock (wheels can differ after a partial change).
        double setNow = 0.0;
        for (int w = 0; w < 4; ++w) if (s_setSeconds[w] > setNow) setNow = s_setSeconds[w];
        const int a = static_cast<int>(setNow), b = static_cast<int>(cfg.targetMinutes * 60.0f + 0.5f);
        char head[96];
        sprintf_s(head, "TYRES  max %.0f%%  |  %d:%02d / %d:%02d", cfg.targetWearPercent, a / 60, a % 60, b / 60, b % 60);
        renderer.AddText(cx, top, head, headPx, 0.9f, 0.9f, 0.9f, 1.0f, Renderer::HAlign::Center);
    }

    // Live tint, smoothed like the fuel bar.
    static LARGE_INTEGER s_prevQpc = {};
    static double s_smDelta[4] = {};
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double dt = s_prevQpc.QuadPart ? static_cast<double>(now.QuadPart - s_prevQpc.QuadPart) / freq.QuadPart : 0.0;
    s_prevQpc = now;
    if (dt < 0.0 || dt > 1.0) dt = 1.0;

    const float gridTop = top + headPx + gap;
    // The box texts refresh 5x a second (user, 2026-11-24): readable instead
    // of twitching at the 25 Hz rebuild, which stays for the box colours.
    // Layout mode always shows the live values.
    struct WheelText { char l1[48], l2[48], l3[48], pct[32]; float t[3], a[3]; };
    static WheelText s_wt[4];
    static bool s_wtValid = false;
    static ULONGLONG s_wtMs = 0;
    const ULONGLONG wtNow = GetTickCount64();
    const bool wtDue = !s_wtValid || wtNow - s_wtMs >= 200 || Layout::Active();
    if (wtDue) { s_wtValid = true; s_wtMs = wtNow; }
    for (int w = 0; w < 4; ++w)
    {
        const int col = w % 2, row = w / 2;
        const float bx = cx + (col == 0 ? -1.0f : 1.0f) * (boxHalfW + gap * 0.5f);
        const float by = gridTop + boxHalfH + row * (2.0f * boxHalfH + gap);

        const bool haveTarget = live && s_target[w] > 0.0;
        const bool overMax = live && s_target[w] == 0.0;   // F24: over budget, not "on plan"
        float r = 0.15f, g = 0.15f, b = 0.15f;
        if (!std::isfinite(s_smDelta[w])) s_smDelta[w] = 0.0;
        if (overMax)
        {
            DeltaColour(1.0, r, g, b);
            r *= 0.55f; g *= 0.55f; b *= 0.55f;
        }
        else if (haveTarget)
        {
            const double used = worn[w] - s_wornAtLapStart[w];
            const double expected = s_target[w] * ShareAt(w, lapFrac);
            // Relative to the lap's requirement; fully coloured at +/-25%.
            const double rel = (used - expected) / (s_target[w] * 0.25);
            s_smDelta[w] += (rel - s_smDelta[w]) * (1.0 - exp(-dt / 1.5));
            DeltaColour(s_smDelta[w], r, g, b);
            r *= 0.55f; g *= 0.55f; b *= 0.55f;
        }
        renderer.AddFilledRoundedRect(bx, by, boxHalfW, boxHalfH, 8.0f * s, 0.0f, r, g, b, 0.75f);

        char l1[48], l2[48];
        if (live && s_lastLap[w] > 0.0) sprintf_s(l1, "Last %.2f", s_lastLap[w]);
        else strcpy_s(l1, "Last --");
        if (haveTarget) sprintf_s(l2, "Need %.2f", s_target[w]);
        else if (overMax) strcpy_s(l2, "Over max");
        else strcpy_s(l2, "Need --");

        float tr = 1.0f, tg = 1.0f, tb = 1.0f;
        if (live && s_lastLap[w] > 0.0 && s_lastLapTarget[w] >= 0.0)
        {
            const bool over = s_lastLap[w] > s_lastLapTarget[w];
            tr = over ? 1.0f : 0.35f; tg = over ? 0.35f : 0.95f; tb = over ? 0.3f : 0.4f;
        }
        // Average wear per lap on this set, green if it is within what is
        // needed now, red if over.
        const double avg = s_wearLaps[w] > 0 ? s_wearSum[w] / s_wearLaps[w] : -1.0;
        char l3[48];
        float ar = 0.95f, ag = 0.95f, ab = 0.95f;
        if (live && avg > 0.0)
        {
            sprintf_s(l3, "Avg %.2f", avg);
            if (haveTarget || overMax)
            {
                const bool over = avg > s_target[w];
                ar = over ? 1.0f : 0.35f; ag = over ? 0.35f : 0.95f; ab = over ? 0.3f : 0.4f;
            }
        }
        else strcpy_s(l3, "Avg --");
        // Top row: corner line first, then the three; bottom row: the three,
        // then the corner line -- each tyre's corner text on the car's side.
        WheelText& wt = s_wt[w];
        if (wtDue)
        {
            strcpy_s(wt.l1, l1); strcpy_s(wt.l2, l2); strcpy_s(wt.l3, l3);
            wt.t[0] = tr; wt.t[1] = tg; wt.t[2] = tb; wt.a[0] = ar; wt.a[1] = ag; wt.a[2] = ab;
        }
        const float boxTop = by - boxHalfH + inset;
        const float linesTop = row == 0 ? boxTop + cornerPx : boxTop;
        renderer.AddText(bx, linesTop, wt.l1, textPx, wt.t[0], wt.t[1], wt.t[2], 1.0f, Renderer::HAlign::Center);
        renderer.AddText(bx, linesTop + textPx, wt.l3, textPx, wt.a[0], wt.a[1], wt.a[2], 1.0f, Renderer::HAlign::Center);
        renderer.AddText(bx, linesTop + 2.0f * textPx, wt.l2, textPx, 0.95f, 0.95f, 0.95f, 1.0f, Renderer::HAlign::Center);

        // Tyre life left, in the box's outer corner -- the car's own corner
        // for that tyre (FL top-left ... RR bottom-right).
        if (live || placeholder)
        {
            // % left, and laps left to the target max wear at the set's
            // average wear per lap.
            char pct[32];
            const double rem = live ? tyres.wheel[w].remainingPercent : 100.0;
            if (live && avg > 0.0)
            {
                double laps = (cfg.targetWearPercent - worn[w]) / avg;
                if (laps < 0.0) laps = 0.0;
                // Capped so very low wear cannot spill into the next box (F20/F27).
                if (laps >= 100.0) sprintf_s(pct, "%.0f%% 99+ laps", rem);
                else sprintf_s(pct, "%.0f%% %.0f laps", rem, floor(laps));
            }
            else sprintf_s(pct, "%.0f%%", rem);
            if (wtDue) strcpy_s(wt.pct, pct);
            const float tx = col == 0 ? bx - boxHalfW + inset : bx + boxHalfW - inset;
            const float ty = row == 0 ? boxTop : boxTop + 3.0f * textPx;
            renderer.AddText(tx, ty, wt.pct, cornerPx, 0.85f, 0.85f, 0.85f, 0.95f,
                             col == 0 ? Renderer::HAlign::Left : Renderer::HAlign::Right);
        }
    }

    renderer.SetTextBackground(prevBg);
    ReleaseSRWLockShared(&s_stateLock);
    return true;
}
