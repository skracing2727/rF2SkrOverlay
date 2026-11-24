#include "FuelSave.h"
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

// Lap target aimed at the stint length, a learned fuel-use profile, and a
// stint projection.
//
// Target: re-worked at every lap start: (fuel then - reserve) / (laps that
// still fit in the stint time left). Saving raises it, overspending lowers it.
//
// Profile: fuel use is not spread evenly round a lap (straights burn, braking
// zones don't), so "expected so far" is target x the share of a normal lap's
// fuel used by this point. The share curve is learned from clean laps (no pit
// visit, no refuel), blended a little per lap, saved per car+track. Until one
// exists the share is just the distance fraction.
//
// Stint projection: elapsed stint time plus what the fuel left lasts at last
// lap's rate, against the stint target.
//
// Draw runs on the render thread, so it never opens the profile file itself:
// loading and saving are handed to the save thread (FuelSave::FlushIfDirty,
// run by Persist.cpp) and the result picked up on a later frame.
namespace
{
    const int kBins = 50;                 // profile resolution: share at i/kBins of the lap
    float s_profile[kBins + 1] = {};      // cumulative share of lap fuel, 0..1
    bool s_haveProfile = false;
    char s_profilePath[MAX_PATH] = "";

    float s_lapUsed[kBins + 1];           // this lap: litres used on reaching each bin, -1 = not reached
    bool s_lapClean = false;              // whole lap seen, no pits, no refuel

    long s_lap = -1;
    double s_prevLapStartFuel = -1.0;

    // Stint
    double s_target = -1.0;               // fixed L/lap for this stint; -1 = not set yet
    double s_bankDone = 0.0;              // completed laps' (target - used) this stint
    double s_lastLapUsed = -1.0;
    double s_lastLapDiff = 0.0;           // last lap used - target
    bool s_wasInPits = false;
    // What this lap's use is measured from: the fuel and lap-profile share
    // at the line, or at pit exit on an out-lap (the line is inside the pit
    // lane at many tracks, so the first crossing after a stop is still "in
    // pits" and the target used to wait a whole lap). -1 = the line.
    double s_refFuel = -1.0;
    double s_refShare = 0.0;

    // Hand-over between Draw (render thread) and FlushIfDirty (save thread).
    // s_ioLock is only ever held to copy these few hundred bytes, never
    // across file I/O, so a frame can't end up waiting on the disk.
    SRWLOCK s_ioLock = SRWLOCK_INIT;
    struct ProfileIo
    {
        bool saveDirty = false;           // a learned profile is waiting to be written
        char savePath[MAX_PATH] = "";
        float save[kBins + 1] = {};
        bool loadPending = false;         // Draw wants loadPath read
        char loadPath[MAX_PATH] = "";
        bool loadDone = false;            // read: result below, for loadPath
        bool loadOk = false;
        float loaded[kBins + 1] = {};
    } s_io;
    std::atomic<bool> s_loadReady{ false };  // loadDone, checked without the lock
    bool s_loadWaiting = false;              // render thread: s_profilePath's file not applied yet

    // Render thread: a new car+track's profile. Until it arrives (a moment
    // later, from the save thread) the share is the plain distance fraction,
    // exactly as for a car+track with no profile yet.
    void RequestLoad(const char* path)
    {
        strcpy_s(s_profilePath, path);
        s_haveProfile = false;
        s_loadWaiting = true;
        AcquireSRWLockExclusive(&s_ioLock);
        strcpy_s(s_io.loadPath, path);
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
            s_haveProfile = s_io.loadOk;
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
    bool ReadProfile(const char* path, float out[kBins + 1])
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") != 0 || !f) return false;
        int n = 0;
        for (; n <= kBins; ++n)
            if (fscanf_s(f, "%f", &out[n]) != 1) break;
        fclose(f);
        return n == kBins + 1;
    }

    void WriteProfile(const char* path, const float profile[kBins + 1])
    {
        char tmp[StoreFile::kTmpPathSize];
        FILE* f = StoreFile::Begin(path, tmp, sizeof(tmp), "w");
        if (!f) { Log::Write("FuelSave: cannot write %s", path); return; }
        for (int i = 0; i <= kBins; ++i) fprintf(f, "%.5f\n", profile[i]);
        if (!StoreFile::Commit(f, tmp, path))
            Log::Write("FuelSave: cannot write %s", path);
    }

    // Folds a finished clean lap into the profile.
    void LearnLap(double lapTotal)
    {
        if (lapTotal < 0.05) return;
        float share[kBins + 1];
        share[0] = 0.0f;
        share[kBins] = 1.0f;
        int missing = 0;
        for (int i = 1; i < kBins; ++i)
        {
            if (s_lapUsed[i] < 0.0f) { ++missing; share[i] = -1.0f; continue; }
            share[i] = static_cast<float>(s_lapUsed[i] / lapTotal);
        }
        if (missing > kBins / 10) return;   // too many gaps (paused, teleported)
        // Fill gaps by interpolation, then force monotonic 0..1.
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
        if (!s_haveProfile)
            for (int i = 0; i <= kBins; ++i) s_profile[i] = share[i];
        else
            for (int i = 0; i <= kBins; ++i) s_profile[i] = 0.7f * s_profile[i] + 0.3f * share[i];
        s_haveProfile = true;
        QueueSave();
    }

    double ShareAt(double frac)
    {
        if (frac <= 0.0) return 0.0;
        if (frac >= 1.0) return 1.0;
        if (!s_haveProfile) return frac;
        const double x = frac * kBins;
        const int i = static_cast<int>(x);
        const double t = x - i;
        return s_profile[i] + (s_profile[i + 1] - s_profile[i]) * t;
    }

    void ResetLapTrace()
    {
        for (int i = 0; i <= kBins; ++i) s_lapUsed[i] = -1.0f;
        s_lapUsed[0] = 0.0f;
    }
}

void FuelSave::FlushIfDirty()
{
    // Save thread, any time (see Persist.cpp): copy out under s_ioLock,
    // touch the disk after letting go. Writes before reading, so a profile
    // saved and then asked for again comes back as saved.
    bool doSave = false, doLoad = false;
    char savePath[MAX_PATH] = "", loadPath[MAX_PATH] = "";
    float save[kBins + 1];
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
        float loaded[kBins + 1] = {};
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

bool FuelSave::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    FuelSaveConfig& cfg = g_fuelSaveConfig;
    if (!cfg.enabled || cfg.stintMinutes <= 0.0f)
        return false;

    // Medium tier (~25Hz), like the delta bar: the fill and the marker are
    // eased over 0.6 s and the colour over 1.5 s (see below), so 25 real
    // updates a second still move smoothly, and the lap bookkeeping only
    // needs a sample per 1/50th of a lap. Bypassed in layout mode so
    // dragging/resizing stays smooth.
    static RenderThrottle s_throttle(0.04);
    if (!Layout::Active() && s_throttle.TryReplay(renderer))
        return true;
    RenderThrottle::Capture throttleCapture(s_throttle, renderer);

    const FuelStore::State fuel = g_fuelStore.Read();
    const bool placeholder = Layout::Preview();
    if (!fuel.valid && !placeholder)
        return false;

    const double stintTargetSec = cfg.stintMinutes * 60.0;
    const double reserve = cfg.reserveLitres;
    const double lapTime = fuel.lastLapTimeSeconds;
    double lapFrac = fuel.currentLapFractional - floor(fuel.currentLapFractional);
    if (lapFrac < 0.0) lapFrac = 0.0;

    if (fuel.valid)
    {
        if (strcmp(fuel.profilePath, s_profilePath) != 0)
        {
            RequestLoad(fuel.profilePath);
            s_target = -1.0; s_bankDone = 0.0; s_lastLapUsed = -1.0; s_lap = -1;
        }
        ApplyLoadIfReady();

        // Pit visit or refuel: new stint, new target.
        const bool refuelled = s_prevLapStartFuel >= 0.0 && fuel.fuelLoad > s_prevLapStartFuel + 0.5;
        if (fuel.inPits || refuelled)
        {
            s_target = -1.0;
            s_bankDone = 0.0;
            s_lapClean = false;
            if (refuelled) s_prevLapStartFuel = fuel.fuelLoad;
        }
        // Pit exit: set the target now, from the fuel in the tank and the
        // stint time left, instead of at the next line crossing.
        if (s_wasInPits && !fuel.inPits && lapTime > 5.0 && fuel.stintTimeSeconds >= 0.0)
        {
            const double lapsLeft = (stintTargetSec - fuel.stintTimeSeconds) / lapTime;
            s_target = (lapsLeft > 0.5) ? (fuel.fuelLoad - reserve) / lapsLeft : -1.0;
            s_refFuel = fuel.fuelLoad;
            s_refShare = ShareAt(lapFrac);
            if (s_target > 0.0)
                Log::Write("FuelSave: target at pit exit %.3f L/lap (%.2f L, %.1f laps of %.0f s left)",
                           s_target, fuel.fuelLoad, lapsLeft, lapTime);
        }
        s_wasInPits = fuel.inPits;

        if (fuel.telemLapNumber != s_lap)
        {
            // Finished lap: score it against the stint target and learn it.
            if (s_lap >= 0 && fuel.telemLapNumber == s_lap + 1 && s_prevLapStartFuel >= 0.0)
            {
                const double used = s_prevLapStartFuel - fuel.fuelAtLapStart;
                if (used > 0.05)
                {
                    // Scored only for a whole lap: an out-lap measured from
                    // pit exit is not one.
                    if (s_target > 0.0 && s_lapClean)
                    {
                        s_lastLapUsed = used;
                        s_lastLapDiff = used - s_target;
                        s_bankDone += s_target - used;
                    }
                    // Not while the saved profile is still on its way in:
                    // learning onto "none yet" would overwrite it.
                    if (s_lapClean && !s_loadWaiting)
                        LearnLap(used);
                }
            }
            s_lap = fuel.telemLapNumber;
            s_prevLapStartFuel = fuel.fuelAtLapStart;
            s_refFuel = -1.0;
            s_refShare = 0.0;
            ResetLapTrace();
            s_lapClean = !fuel.inPits;

            // Re-work the target at every lap start: whatever litres/lap makes
            // the fuel now in the tank last to the stint's target length.
            // Saving raises it, overspending lowers it.
            if (!fuel.inPits && lapTime > 5.0 && fuel.stintTimeSeconds >= 0.0)
            {
                const double lapsLeft = (stintTargetSec - fuel.stintTimeSeconds) / lapTime;
                s_target = (lapsLeft > 0.5) ? (fuel.fuelAtLapStart - reserve) / lapsLeft : -1.0;
                if (s_target > 0.0)
                    Log::Write("FuelSave: lap target %.3f L/lap (%.2f L, %.1f laps of %.0f s left)",
                               s_target, fuel.fuelAtLapStart, lapsLeft, lapTime);
            }
        }

        if (fuel.inPits) s_lapClean = false;
        const int bin = static_cast<int>(lapFrac * kBins);
        if (bin > 0 && bin < kBins && s_lapUsed[bin] < 0.0f)
            s_lapUsed[bin] = static_cast<float>(fuel.fuelAtLapStart - fuel.fuelLoad);
    }

    // Geometry: same 2160p-reference convention as the other panels.
    const float s = static_cast<float>(screenHeight) / 2160.0f * (cfg.scale > 0.05f ? cfg.scale : 0.05f);
    const float barHalfW = 360.0f * s;
    const float barHalfH = 30.0f * s;
    const float textPx = 40.0f * s;
    const float infoPx = 30.0f * s;
    const float gap = 10.0f * s;
    const float cx = screenWidth * cfg.centerXFraction;
    const float cy = screenHeight * cfg.centerYFraction;
    Layout::Register("Fuel save", "FuelSave", "CenterXFraction", "CenterYFraction",
                     &cfg.centerXFraction, &cfg.centerYFraction,
                     cx, cy + (gap + infoPx * 1.2f * 4.0f) * 0.5f, barHalfW, barHalfH + (gap + infoPx * 1.2f * 4.0f) * 0.5f,
                     &cfg.scale, "Scale");

    const bool prevBg = renderer.TextBackgroundEnabled();
    renderer.SetTextBackground(false);

    const bool haveTarget = fuel.valid && s_target > 0.0;
    const double usedThisLap = (s_refFuel >= 0.0 ? s_refFuel : fuel.fuelAtLapStart) - fuel.fuelLoad;
    // Positive = over the target's expected use at this point in the lap
    // (from pit exit on an out-lap).
    double expectedShare = ShareAt(lapFrac) - s_refShare;
    if (expectedShare < 0.0) expectedShare = 0.0;
    const double expected = haveTarget ? s_target * expectedShare : 0.0;
    const double rawDelta = haveTarget ? usedThisLap - expected : 0.0;

    // Smoothing, so nothing on the bar twitches: the delta (and the colour
    // it drives) eases over ~1.5 s, the fill length over ~0.3 s. A fill
    // that drops a lot (new lap) snaps instead of sliding back.
    static LARGE_INTEGER s_prevQpc = {};
    static double s_smDelta = 0.0, s_smFill = 0.0, s_smMark = 0.0;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double dt = s_prevQpc.QuadPart ? static_cast<double>(now.QuadPart - s_prevQpc.QuadPart) / freq.QuadPart : 0.0;
    s_prevQpc = now;
    if (dt < 0.0 || dt > 1.0) dt = 1.0;
    const double fillTarget = haveTarget ? usedThisLap / s_target : 0.0;
    s_smDelta += (rawDelta - s_smDelta) * (1.0 - exp(-dt / 1.5));
    if (fillTarget < s_smFill - 0.3) s_smFill = fillTarget;
    else s_smFill += (fillTarget - s_smFill) * (1.0 - exp(-dt / 0.6));
    // Lap progress only arrives with scoring (~5 Hz), so the marker would
    // step; ease it the same way, snapping back at the line.
    const double markTarget = haveTarget ? expected / s_target : 0.0;
    if (markTarget < s_smMark - 0.3) s_smMark = markTarget;
    else s_smMark += (markTarget - s_smMark) * (1.0 - exp(-dt / 0.6));
    const double delta = s_smDelta;

    renderer.AddFilledRoundedRect(cx, cy, barHalfW, barHalfH, 8.0f * s, 0.0f, 0.15f, 0.15f, 0.15f, 0.6f);
    if (haveTarget)
    {
        // Colour: amber on target, blending to green under and red over;
        // fully green/red at half the bar window either way.
        const double cw = (cfg.barWindowLitres > 0.01f ? cfg.barWindowLitres : 0.01) * 0.5;
        double t = delta / cw;
        if (t > 1.0) t = 1.0;
        if (t < -1.0) t = -1.0;
        const float ar = 1.0f, ag = 0.75f, ab = 0.15f;
        float r, g, b;
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

        // Fill: fuel used this lap, left to right, full width = the target.
        double f = s_smFill;
        if (f < 0.0) f = 0.0;
        if (f > 1.0) f = 1.0;
        const float left = cx - barHalfW;
        const float fillHalfW = static_cast<float>(f) * barHalfW;
        if (fillHalfW > 0.5f)
        {
            const float corner = (8.0f * s < fillHalfW) ? 8.0f * s : fillHalfW;
            renderer.AddFilledRoundedRect(left + fillHalfW, cy, fillHalfW, barHalfH, corner, 0.0f, r, g, b, 0.85f);
        }
        // Marker: where the fill should be by now.
        double m = s_smMark;
        if (m > 1.0) m = 1.0;
        renderer.AddFilledRect(left + static_cast<float>(m) * barHalfW * 2.0f, cy, 2.0f * s, barHalfH, 0.0f, 1.0f, 1.0f, 1.0f, 0.9f);
    }

    char line[96];
    if (haveTarget)
        sprintf_s(line, s_haveProfile ? "FUEL %+.2f L" : "FUEL %+.2f L (learning)", delta);
    else if (placeholder && !fuel.valid)
        sprintf_s(line, "FUEL +0.00 L");
    else if (fuel.inPits)
        sprintf_s(line, "FUEL: target set after pit exit");
    else
        sprintf_s(line, "FUEL: target after first lap");
    renderer.AddText(cx, cy - textPx * 0.5f, line, textPx, 1.0f, 1.0f, 1.0f, 1.0f, Renderer::HAlign::Center);

    // The table under the bar (reworked 2026-11-24, the old two lines were
    // confusing): one question per row, label left, value right.
    //   Stint target  32:00 (11.0 laps)   the stint you set
    //   Stint left    23:38 (9.2 laps)    still to drive in it
    //   Fuel lasts    56:22 (17.5 laps)   at last lap's use; green if it
    //                                     covers "Stint left", red if not
    //   Fuel per lap  avg 4.05, last 0.22 under   average use and last lap
    //                                     against what you can afford
    if (haveTarget)
    {
        const double rate = (s_lastLapUsed > 0.05) ? s_lastLapUsed : fuel.fuelPerLap;
        double lapsTarget = -1.0, leftSec = -1.0, lapsLeft = -1.0, lastsSec = -1.0, lastsLaps = -1.0;
        if (lapTime > 5.0) lapsTarget = stintTargetSec / lapTime;
        if (fuel.stintTimeSeconds >= 0.0)
        {
            leftSec = stintTargetSec - fuel.stintTimeSeconds;
            if (leftSec < 0.0) leftSec = 0.0;
            if (lapTime > 5.0) lapsLeft = leftSec / lapTime;
        }
        if (rate > 0.05)
        {
            lastsLaps = (fuel.fuelLoad - reserve) / rate;
            if (lastsLaps < 0.0) lastsLaps = 0.0;
            if (lapTime > 5.0) lastsSec = lastsLaps * lapTime;
        }
        auto timeLaps = [](char* out, size_t n, double sec, double laps)
        {
            if (sec < 0.0) { strcpy_s(out, n, "--"); return; }
            const int a = static_cast<int>(sec + 0.5);
            if (laps >= 0.0) sprintf_s(out, n, "%d:%02d (%.1f laps)", a / 60, a % 60, laps);
            else sprintf_s(out, n, "%d:%02d", a / 60, a % 60);
        };
        char v1[48], v2[48], v3[48], v4[64];
        timeLaps(v1, sizeof(v1), stintTargetSec, lapsTarget);
        timeLaps(v2, sizeof(v2), leftSec, lapsLeft);
        if (lastsSec >= 0.0) timeLaps(v3, sizeof(v3), lastsSec, lastsLaps);
        else if (lastsLaps >= 0.0) sprintf_s(v3, "%.1f laps", lastsLaps);
        else strcpy_s(v3, "--");
        const bool lastsOk = lastsLaps < 0.0 || lapsLeft < 0.0 || lastsLaps >= lapsLeft;
        char avg[24];
        if (fuel.fuelPerLap > 0.05) sprintf_s(avg, "avg %.2f", fuel.fuelPerLap); else strcpy_s(avg, "avg --");
        if (s_lastLapUsed > 0.0)
            sprintf_s(v4, "%s, last %.2f %s", avg, fabs(s_lastLapDiff), s_lastLapDiff <= 0.0 ? "under" : "over");
        else
            sprintf_s(v4, "%s, last --", avg);
        const bool lastOk = s_lastLapUsed <= 0.0 || s_lastLapDiff <= 0.0;

        const char* labels[4] = { "Stint target", "Stint left", "Fuel lasts", "Fuel per lap" };
        const char* values[4] = { v1, v2, v3, v4 };
        const float green[3] = { 0.35f, 0.9f, 0.4f }, red[3] = { 1.0f, 0.35f, 0.3f }, white[3] = { 0.9f, 0.9f, 0.9f };
        const float* cols[4] = { white, white, lastsOk ? green : red, lastOk ? green : red };

        const float pad = 10.0f * s;
        const float rowStep = infoPx * 1.2f;
        const float top = cy + barHalfH + gap;
        const float left = cx - barHalfW, right = cx + barHalfW;
        // Standard panel plate (0.08 grey, 60%); shapes draw before text.
        renderer.AddFilledRoundedRect(cx, top + rowStep * 2.0f, barHalfW, rowStep * 2.0f + pad * 0.5f, 8.0f * s, 0.0f,
                                      0.08f, 0.08f, 0.08f, 0.6f);
        for (int row = 0; row < 4; ++row)
        {
            const float y = top + row * rowStep + (rowStep - infoPx) * 0.5f;
            renderer.AddText(left + pad, y, labels[row], infoPx, 0.9f, 0.9f, 0.9f, 1.0f, Renderer::HAlign::Left);
            renderer.AddText(right - pad, y, values[row], infoPx, cols[row][0], cols[row][1], cols[row][2], 1.0f,
                             Renderer::HAlign::Right);
        }
    }

    renderer.SetTextBackground(prevBg);
    return true;
}
