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

// Draw runs on the render thread, so it never opens the profile file itself:
// loading and saving are handed to the save thread (TyreSave::FlushIfDirty,
// run by Persist.cpp) and the result picked up on a later frame -- the same
// hand-over as FuelSave.cpp.
namespace
{
    const int kBins = 50;
    float s_profile[4][kBins + 1] = {};   // per tyre: cumulative share of a lap's wear, 0..1
    bool s_haveProfile[4] = {};
    char s_profilePath[MAX_PATH] = "";
    char s_fuelProfilePath[MAX_PATH] = "";

    float s_lapWorn[4][kBins + 1];        // this lap: wear % on reaching each bin, -1 = not reached
    bool s_lapClean = false;

    long s_lap = -1;
    double s_wornAtLapStart[4] = { -1, -1, -1, -1 }; // wear % (0 = fresh) at the current lap's start
    double s_prevRemaining[4] = { -1, -1, -1, -1 };
    double s_target[4] = { -1, -1, -1, -1 };          // required wear %/lap, this lap
    double s_lastLap[4] = { -1, -1, -1, -1 };         // last lap's wear %
    double s_lastLapTarget[4] = { -1, -1, -1, -1 };   // what was required on that lap
    double s_wearSum[4] = {};             // this set: summed wear % of completed laps
    int s_wearLaps[4] = {};               //            and how many laps
    double s_setSeconds = 0.0;            // driving time on this set, completed laps
    double s_lastLapTime = 0.0;

    // Hand-over between Draw (render thread) and FlushIfDirty (save thread).
    // s_ioLock is only ever held to copy these, never across file I/O.
    SRWLOCK s_ioLock = SRWLOCK_INIT;
    struct ProfileIo
    {
        bool saveDirty = false;               // a learned profile is waiting to be written
        char savePath[MAX_PATH] = "";
        float save[4][kBins + 1] = {};
        bool loadPending = false;             // Draw wants loadPath read
        char loadPath[MAX_PATH] = "";
        bool loadDone = false;                // read: result below, for loadPath
        bool loadOk = false;
        float loaded[4][kBins + 1] = {};
    } s_io;
    std::atomic<bool> s_loadReady{ false };      // loadDone, checked without the lock
    bool s_loadWaiting = false;                  // render thread: s_profilePath's file not applied yet

    // Render thread: a new car+track's profile (s_profilePath already set).
    // Until it arrives each tyre's share is the plain distance fraction,
    // exactly as for a car+track with no profile yet.
    void RequestLoad()
    {
        for (int w = 0; w < 4; ++w) s_haveProfile[w] = false;
        s_loadWaiting = true;
        AcquireSRWLockExclusive(&s_ioLock);
        strcpy_s(s_io.loadPath, s_profilePath);
        s_io.loadPending = true;
        s_io.loadDone = false;
        s_loadReady.store(false);
        ReleaseSRWLockExclusive(&s_ioLock);
        Persist::RequestLearned();
    }

    // Render thread, each frame while a load is outstanding.
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

    // Render thread, after learning a lap: written by the next save pass.
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
        return n == kBins + 1;
    }

    void WriteProfile(const char* path, const float profile[4][kBins + 1])
    {
        char tmp[StoreFile::kTmpPathSize];
        FILE* f = StoreFile::Begin(path, tmp, sizeof(tmp), "w");
        if (!f) { Log::Write("TyreSave: cannot write %s", path); return; }
        for (int i = 0; i <= kBins; ++i)
            fprintf(f, "%.5f %.5f %.5f %.5f\n", profile[0][i], profile[1][i], profile[2][i], profile[3][i]);
        if (!StoreFile::Commit(f, tmp, path))
            Log::Write("TyreSave: cannot write %s", path);
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

    void NewSet()
    {
        s_setSeconds = 0.0;
        for (int w = 0; w < 4; ++w) { s_target[w] = -1.0; s_lastLap[w] = -1.0; s_lastLapTarget[w] = -1.0; s_wearSum[w] = 0.0; s_wearLaps[w] = 0; }
        s_lapClean = false;
        Log::Write("TyreSave: fresh tyres, set clock restarted");
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

void TyreSave::FlushIfDirty()
{
    // Save thread, any time (see Persist.cpp): copy out under s_ioLock,
    // touch the disk after letting go. Writes before reading, so a profile
    // saved and then asked for again comes back as saved.
    bool doSave = false, doLoad = false;
    char savePath[MAX_PATH] = "", loadPath[MAX_PATH] = "";
    float save[4][kBins + 1];
    AcquireSRWLockExclusive(&s_ioLock);
    if (s_io.saveDirty)
    {
        strcpy_s(savePath, s_io.savePath);
        memcpy(save, s_io.save, sizeof(save));
        s_io.saveDirty = false;
        doSave = true;
    }
    if (s_io.loadPending)
    {
        strcpy_s(loadPath, s_io.loadPath);
        s_io.loadPending = false;
        doLoad = true;
    }
    ReleaseSRWLockExclusive(&s_ioLock);

    if (doSave)
        WriteProfile(savePath, save);
    if (doLoad)
    {
        float loaded[4][kBins + 1] = {};
        const bool ok = ReadProfile(loadPath, loaded);
        AcquireSRWLockExclusive(&s_ioLock);
        // Dropped if Draw has asked for another file in the meantime.
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

bool TyreSave::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    TyreSaveConfig& cfg = g_tyreSaveConfig;
    if (!cfg.enabled || cfg.targetMinutes <= 0.0f)
        return false;

    // Medium tier (~25Hz): the box tint is eased over 1.5 s and the numbers
    // change once a lap, the set clock adds up real elapsed time between
    // updates (so it does not care how often they come), and the lap
    // bookkeeping only needs a sample per 1/50th of a lap. Bypassed in
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

    double lapFrac = fuel.currentLapFractional - floor(fuel.currentLapFractional);
    if (lapFrac < 0.0) lapFrac = 0.0;
    const double lapTime = fuel.lastLapTimeSeconds;

    double worn[4];
    for (int w = 0; w < 4; ++w) worn[w] = 100.0 - tyres.wheel[w].remainingPercent;

    if (live)
    {
        // Per car+track profile file, next to the fuel one.
        if (strcmp(fuel.profilePath, s_fuelProfilePath) != 0)
        {
            strcpy_s(s_fuelProfilePath, fuel.profilePath);
            strcpy_s(s_profilePath, fuel.profilePath);
            char* dot = strstr(s_profilePath, "_profile.txt");
            if (dot) strcpy_s(dot, sizeof(s_profilePath) - (dot - s_profilePath), "_tyreprofile.txt");
            else s_profilePath[0] = 0;
            RequestLoad();
            s_lap = -1;
            NewSet();
        }
        ApplyLoadIfReady();

        // Fresh tyres (any wheel's remaining life jumps up): new set.
        bool fresh = false;
        for (int w = 0; w < 4; ++w)
        {
            if (s_prevRemaining[w] >= 0.0 && tyres.wheel[w].remainingPercent > s_prevRemaining[w] + 1.0)
                fresh = true;
            s_prevRemaining[w] = tyres.wheel[w].remainingPercent;
        }
        if (fresh)
        {
            NewSet();
            for (int w = 0; w < 4; ++w) s_wornAtLapStart[w] = worn[w];
        }

        if (fuel.telemLapNumber != s_lap)
        {
            if (s_lap >= 0 && fuel.telemLapNumber == s_lap + 1 && s_wornAtLapStart[0] >= 0.0)
            {
                bool learned = false;
                for (int w = 0; w < 4; ++w)
                {
                    const double used = worn[w] - s_wornAtLapStart[w];
                    if (used <= 0.0) continue;
                    s_wearSum[w] += used;
                    ++s_wearLaps[w];
                    if (s_target[w] > 0.0)
                    {
                        s_lastLap[w] = used;
                        s_lastLapTarget[w] = s_target[w];
                    }
                    // Not while the saved profile is still on its way in:
                    // learning onto "none yet" would overwrite it.
                    if (s_lapClean && !s_loadWaiting) { LearnLap(w, used); learned = true; }
                }
                if (learned) QueueSave();
            }
            s_lap = fuel.telemLapNumber;
            for (int w = 0; w < 4; ++w) s_wornAtLapStart[w] = worn[w];
            ResetLapTrace();
            s_lapClean = !fuel.inPits;

            // Required wear per lap: what is left to the target wear, over
            // the laps that still fit in the set's target time.
            if (lapTime > 5.0)
            {
                const double lapsLeft = (cfg.targetMinutes * 60.0 - s_setSeconds) / lapTime;
                for (int w = 0; w < 4; ++w)
                {
                    const double left = cfg.targetWearPercent - worn[w];
                    s_target[w] = (lapsLeft > 0.5) ? (left > 0.0 ? left / lapsLeft : 0.0) : -1.0;
                }
                Log::Write("TyreSave: lap targets %.3f %.3f %.3f %.3f %%/lap (%.1f laps left)",
                           s_target[0], s_target[1], s_target[2], s_target[3], lapsLeft);
            }
        }

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
            // Lap progress arrives at ~5 Hz: keep counting through the frames
            // between updates, but stop once it has stalled for 1 s.
            static double s_stall = 0.0;
            s_stall = moving ? 0.0 : s_stall + (cdt > 0.0 && cdt < 0.5 ? cdt : 1.0);
            if (cdt > 0.0 && cdt < 0.5 && !fuel.inPits && s_stall < 1.0)
                s_setSeconds += cdt;
        }

        if (fuel.inPits) s_lapClean = false;
        const int bin = static_cast<int>(lapFrac * kBins);
        if (bin > 0 && bin < kBins)
            for (int w = 0; w < 4; ++w)
                if (s_lapWorn[w][bin] < 0.0f)
                    s_lapWorn[w][bin] = static_cast<float>(worn[w] - s_wornAtLapStart[w]);
    }

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
    const float totalHalfW = 2.0f * boxHalfW + gap * 0.5f;
    const float totalHalfH = 2.0f * boxHalfH + gap * 0.5f + (headPx + gap) * 0.5f;
    Layout::Register("Tyre wear target", "TyreSave", "CenterXFraction", "CenterYFraction",
                     &cfg.centerXFraction, &cfg.centerYFraction,
                     cx, cy, totalHalfW, totalHalfH, &cfg.scale, "Scale");

    const bool prevBg = renderer.TextBackgroundEnabled();
    renderer.SetTextBackground(false);

    // Header: target and how far into the set's time.
    const float top = cy - totalHalfH;
    {
        const double setNow = s_setSeconds;
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
    for (int w = 0; w < 4; ++w)
    {
        const int col = w % 2, row = w / 2;
        const float bx = cx + (col == 0 ? -1.0f : 1.0f) * (boxHalfW + gap * 0.5f);
        const float by = gridTop + boxHalfH + row * (2.0f * boxHalfH + gap);

        const bool haveTarget = live && s_target[w] > 0.0;
        float r = 0.15f, g = 0.15f, b = 0.15f;
        if (haveTarget)
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
        else if (live && s_target[w] == 0.0) strcpy_s(l2, "At target");
        else strcpy_s(l2, "Need --");

        float tr = 1.0f, tg = 1.0f, tb = 1.0f;
        if (live && s_lastLap[w] > 0.0 && s_lastLapTarget[w] > 0.0)
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
            if (haveTarget)
            {
                const bool over = avg > s_target[w];
                ar = over ? 1.0f : 0.35f; ag = over ? 0.35f : 0.95f; ab = over ? 0.3f : 0.4f;
            }
        }
        else strcpy_s(l3, "Avg --");
        // Top row: corner line first, then the three; bottom row: the three,
        // then the corner line -- each tyre's corner text on the car's side.
        const float boxTop = by - boxHalfH + inset;
        const float linesTop = row == 0 ? boxTop + cornerPx : boxTop;
        renderer.AddText(bx, linesTop, l1, textPx, tr, tg, tb, 1.0f, Renderer::HAlign::Center);
        renderer.AddText(bx, linesTop + textPx, l3, textPx, ar, ag, ab, 1.0f, Renderer::HAlign::Center);
        renderer.AddText(bx, linesTop + 2.0f * textPx, l2, textPx, 0.95f, 0.95f, 0.95f, 1.0f, Renderer::HAlign::Center);

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
                sprintf_s(pct, "%.0f%% %.0f laps", rem, floor(laps));
            }
            else sprintf_s(pct, "%.0f%%", rem);
            const float tx = col == 0 ? bx - boxHalfW + inset : bx + boxHalfW - inset;
            const float ty = row == 0 ? boxTop : boxTop + 3.0f * textPx;
            renderer.AddText(tx, ty, pct, cornerPx, 0.85f, 0.85f, 0.85f, 0.95f,
                             col == 0 ? Renderer::HAlign::Left : Renderer::HAlign::Right);
        }
    }

    renderer.SetTextBackground(prevBg);
    return true;
}
