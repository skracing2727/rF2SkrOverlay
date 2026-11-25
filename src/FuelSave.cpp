#include "FuelSave.h"
#include "FuelStore.h"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "Log.h"
#include "Persist.h"
#include "RenderThrottle.h"
#include "StoreFile.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

// Lap target aimed at the stint length, a learned fuel-use profile, and a
// stint projection.
//
// Two box modes (FuelSaveConfig::boxMode): 1 = advanced, the target bar and
// its table; 0 = simple, a start/now fuel table with no bar and no target.
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
    bool s_lapDriven = false;             // and driven by you all lap: gates learning only (review F252)

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

    // Stint bookkeeping kept here rather than taken from FuelStore, whose
    // stint restarts at every pit exit (review F12): a pass without fuel
    // carries the stint clock and the start fuel over.
    double s_stintCarry = 0.0;            // stint seconds from before a no-fuel pit pass
    double s_lastStintSec = -1.0;         // FuelStore's stint time last frame
    double s_pitEntryFuel = -1.0;
    bool s_refuelThisVisit = false;
    double s_stintStartFuel = -1.0;       // simple box: fuel at pit exit after a refuel, or first seen
    // Recent clean laps' litres (simple box "Lasts").
    const int kRecent = 3;
    double s_recent[kRecent] = {};
    int s_recentCount = 0, s_recentNext = 0;
    // Clean-lap pace (review F11): in/out-laps never set it.
    double s_bestLap = -1.0;              // fastest clean lap this session
    double s_paceLap = -1.0;              // last clean lap within 10% of the best
    double s_seenLapTime = -1.0;
    bool s_prevLapClean = false;          // the lap just finished was clean
    // Review F14: lap fraction held at 0 until scoring's lap wraps.
    bool s_awaitWrap = false;
    double s_wholeAtEdge = 0.0;

    void UpdatePace(double lapSec)
    {
        if (lapSec <= 5.0 || lapSec == s_seenLapTime) return;
        s_seenLapTime = lapSec;
        if (!s_prevLapClean) return;
        if (s_bestLap < 0.0 || lapSec < s_bestLap) s_bestLap = lapSec;
        if (lapSec <= s_bestLap * 1.10) s_paceLap = lapSec;
    }

    // Review F17: all per-session state, so nothing carries into the next
    // session on the same car+track.
    void ResetSession()
    {
        s_target = -1.0; s_bankDone = 0.0; s_lastLapUsed = -1.0; s_lastLapDiff = 0.0;
        s_lap = -1; s_prevLapStartFuel = -1.0; s_wasInPits = false;
        s_refFuel = -1.0; s_refShare = 0.0; s_lapClean = false;
        s_stintCarry = 0.0; s_lastStintSec = -1.0; s_pitEntryFuel = -1.0; s_refuelThisVisit = false;
        s_stintStartFuel = -1.0;
        s_recentCount = 0; s_recentNext = 0;
        s_bestLap = -1.0; s_paceLap = -1.0; s_seenLapTime = -1.0; s_prevLapClean = false;
        s_awaitWrap = false;
    }

    // Tank size in litres, or -1 if FuelStore does not give it.
    double TankCapacity(const FuelStore::State& fuel)
    {
        // FuelStore publishes mFuelCapacity, 0 when unknown.
        return fuel.fuelCapacity > 0.0 ? fuel.fuelCapacity : -1.0;
    }

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
        char seedPath[MAX_PATH] = "";     // old per-class file, read if loadPath is missing
        bool loadDone = false;            // read: result below, for loadPath
        bool loadOk = false;
        float loaded[kBins + 1] = {};
    } s_io;
    std::atomic<bool> s_loadReady{ false };  // loadDone, checked without the lock
    bool s_loadWaiting = false;              // render thread: s_profilePath's file not applied yet

    // Render thread: a new car+track's profile. Until it arrives (a moment
    // later, from the save thread) the share is the plain distance fraction,
    // exactly as for a car+track with no profile yet.
    void RequestLoad(const char* path, const char* seedPath)
    {
        strcpy_s(s_profilePath, path);
        s_haveProfile = false;
        s_loadWaiting = true;
        AcquireSRWLockExclusive(&s_ioLock);
        strcpy_s(s_io.loadPath, path);
        strcpy_s(s_io.seedPath, seedPath);
        s_io.loadPending = true;
        s_io.loadDone = false;
        s_loadReady.store(false);
        ReleaseSRWLockExclusive(&s_ioLock);
        // Load only: a car change mid-lap must not write anything (review F275).
        Persist::RequestLoad();
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
        if (n != kBins + 1) return false;
        // Review F386: a NaN or a broken curve would never wash out of the
        // blend; reject it and fall back to the plain distance share.
        if (!(fabsf(out[0]) <= 0.05f) || !(fabsf(out[kBins] - 1.0f) <= 0.05f)) return false;
        for (int i = 0; i <= kBins; ++i)
        {
            if (!std::isfinite(out[i]) || out[i] < 0.0f || out[i] > 1.0f) return false;
            if (i > 0 && out[i] < out[i - 1]) return false;
        }
        return true;
    }

    bool WriteProfile(const char* path, const float profile[kBins + 1])
    {
        char tmp[StoreFile::kTmpPathSize];
        FILE* f = StoreFile::Begin(path, tmp, sizeof(tmp), "w");
        if (!f) { Log::Write("FuelSave: cannot write %s", path); return false; }
        for (int i = 0; i <= kBins; ++i) fprintf(f, "%.5f\n", profile[i]);
        if (!StoreFile::Commit(f, tmp, path))
        {
            Log::Write("FuelSave: cannot write %s", path);
            return false;
        }
        return true;
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

void FuelSave::FlushIfDirty(bool loadOnly)
{
    // Save thread, any time (see Persist.cpp): copy out under s_ioLock,
    // touch the disk after letting go. Writes before reading, so a profile
    // saved and then asked for again comes back as saved.
    bool doSave = false, doLoad = false;
    char savePath[MAX_PATH] = "", loadPath[MAX_PATH] = "", seedPath[MAX_PATH] = "";
    float save[kBins + 1];
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
        float loaded[kBins + 1] = {};
        bool ok = ReadProfile(loadPath, loaded);
        // Per-car keys (2026-11-24): a car's first profile is seeded from
        // the old per-class file (read only; saved under the car's name).
        if (!ok && seedPath[0] && !StoreFile::Exists(loadPath) && ReadProfile(seedPath, loaded))
        {
            ok = true;
            Log::Write("FuelSave: no fuel profile yet at %s -- seeded from old class file %s", loadPath, seedPath);
        }
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
    // Simple mode (BoxMode=0) needs no stint target, so it works with
    // StintMinutes=0; advanced mode is the target bar and stays off without one.
    const bool advanced = cfg.boxMode != 0;
    if (!cfg.enabled || (advanced && cfg.stintMinutes <= 0.0f))
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
    // Review F17: an invalid store means the session ended (or the car left):
    // nothing from it may carry into the next session on the same car+track.
    if (!fuel.valid)
        ResetSession();
    if (!fuel.valid && !placeholder)
        return false;

    const double stintTargetSec = (cfg.stintMinutes > 0.0f) ? cfg.stintMinutes * 60.0 : 0.0;
    const double reserve = cfg.reserveLitres;
    double lapFrac = fuel.currentLapFractional - floor(fuel.currentLapFractional);
    if (lapFrac < 0.0) lapFrac = 0.0;
    // Review F11: stint maths uses a clean-lap pace, not an in/out-lap.
    auto paceLap = [&]() { return (s_paceLap > 5.0) ? s_paceLap : fuel.lastLapTimeSeconds; };
    // Stint time across pit passes without fuel (review F12).
    auto stintSec = [&]() { return (fuel.stintTimeSeconds >= 0.0) ? s_stintCarry + fuel.stintTimeSeconds : -1.0; };
    auto targetFrom = [&](double fuelNow) -> double
    {
        const double lt = paceLap();
        if (stintTargetSec <= 0.0 || lt <= 5.0 || stintSec() < 0.0) return -1.0;
        const double lapsLeft = (stintTargetSec - stintSec()) / lt;
        return (lapsLeft > 0.5) ? (fuelNow - reserve) / lapsLeft : -1.0;
    };

    if (fuel.valid)
    {
        if (strcmp(fuel.profilePath, s_profilePath) != 0)
        {
            RequestLoad(fuel.profilePath, fuel.legacyProfilePath);
            ResetSession();
        }
        ApplyLoadIfReady();
        if (s_stintStartFuel < 0.0) s_stintStartFuel = fuel.fuelLoad;   // first seen

        // FuelStore restarts its stint clock at every pit exit; carry it
        // over a pass that put no fuel in (drive-through, tyres only).
        if (fuel.stintTimeSeconds >= 0.0 && s_lastStintSec >= 0.0 && fuel.stintTimeSeconds + 1.0 < s_lastStintSec)
            s_stintCarry = s_refuelThisVisit ? 0.0 : s_stintCarry + s_lastStintSec;
        s_lastStintSec = fuel.stintTimeSeconds;

        if (fuel.inPits && !s_wasInPits)
        {
            s_pitEntryFuel = fuel.fuelLoad;
            s_refuelThisVisit = false;
        }
        // Review F12: only fuel going in starts a new stint, not a pit pass.
        const bool refuelled = (s_prevLapStartFuel >= 0.0 && fuel.fuelLoad > s_prevLapStartFuel + 0.5)
                            || (fuel.inPits && s_pitEntryFuel >= 0.0 && fuel.fuelLoad > s_pitEntryFuel + 0.5);
        if (refuelled)
        {
            s_target = -1.0;
            s_bankDone = 0.0;
            s_lapClean = false;
            // Review F341: last lap belonged to the old stint and target.
            s_lastLapUsed = -1.0;
            s_lastLapDiff = 0.0;
            s_prevLapStartFuel = fuel.fuelLoad;
            s_pitEntryFuel = fuel.fuelLoad;
            s_stintStartFuel = fuel.fuelLoad;
            if (fuel.inPits) s_refuelThisVisit = true;
            else s_stintCarry = 0.0;
        }
        // Pit exit: set the target now, from the fuel in the tank and the
        // stint time left, instead of at the next line crossing. A pass
        // without fuel keeps the target it had.
        if (s_wasInPits && !fuel.inPits)
        {
            if (s_refuelThisVisit) s_stintStartFuel = fuel.fuelLoad;
            if (s_refuelThisVisit || s_target <= 0.0)
            {
                s_target = targetFrom(fuel.fuelLoad);
                s_refFuel = fuel.fuelLoad;
                s_refShare = ShareAt(lapFrac);
                if (s_target > 0.0)
                    Log::Write("FuelSave: target at pit exit %.3f L/lap (%.2f L, %.0f s lap)",
                               s_target, fuel.fuelLoad, paceLap());
            }
        }
        s_wasInPits = fuel.inPits;

        if (fuel.telemLapNumber != s_lap)
        {
            // Finished lap: score it against the stint target and learn it.
            s_prevLapClean = false;
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
                    if (s_lapClean)
                    {
                        s_recent[s_recentNext] = used;
                        s_recentNext = (s_recentNext + 1) % kRecent;
                        if (s_recentCount < kRecent) ++s_recentCount;
                    }
                    // Not while the saved profile is still on its way in:
                    // learning onto "none yet" would overwrite it.
                    if (s_lapClean && s_lapDriven && !s_loadWaiting)
                        LearnLap(used);
                }
                s_prevLapClean = s_lapClean;
            }
            UpdatePace(fuel.lastLapTimeSeconds);
            s_lap = fuel.telemLapNumber;
            s_prevLapStartFuel = fuel.fuelAtLapStart;
            s_refFuel = -1.0;
            s_refShare = 0.0;
            ResetLapTrace();
            s_lapClean = !fuel.inPits;
            s_lapDriven = true;
            // Review F14: scoring's lap fraction lags telemetry's lap change
            // by up to 200 ms; until it wraps, it still reads ~0.99.
            s_awaitWrap = true;
            s_wholeAtEdge = floor(fuel.currentLapFractional);

            // Re-work the target at every lap start: whatever litres/lap makes
            // the fuel now in the tank last to the stint's target length.
            // Saving raises it, overspending lowers it.
            if (!fuel.inPits)
            {
                s_target = targetFrom(fuel.fuelAtLapStart);
                if (s_target > 0.0)
                    Log::Write("FuelSave: lap target %.3f L/lap (%.2f L, %.0f s lap)",
                               s_target, fuel.fuelAtLapStart, paceLap());
            }
        }
        UpdatePace(fuel.lastLapTimeSeconds);   // lap time arriving a frame late

        if (s_awaitWrap)
        {
            if (lapFrac < 0.5 || floor(fuel.currentLapFractional) != s_wholeAtEdge) s_awaitWrap = false;
            else lapFrac = 0.0;
        }
        if (fuel.inPits) s_lapClean = false;
        if (!g_localPlayerDriving.load(std::memory_order_relaxed)) s_lapDriven = false;
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
    const float pad = 10.0f * s;
    const float rowStep = infoPx * 1.2f;
    const float cx = screenWidth * cfg.centerXFraction;
    const float cy = screenHeight * cfg.centerYFraction;
    const float green[3] = { 0.35f, 0.9f, 0.4f }, red[3] = { 1.0f, 0.35f, 0.3f }, white[3] = { 0.9f, 0.9f, 0.9f };
    const float amber[3] = { 1.0f, 0.75f, 0.15f };

    const double lapTime = paceLap();
    auto timeLaps = [](char* out, size_t n, double sec, double laps)
    {
        if (sec < 0.0) { strcpy_s(out, n, "--"); return; }
        const int a = static_cast<int>(sec + 0.5);
        if (laps >= 0.0) sprintf_s(out, n, "%d:%02d (%.1f laps)", a / 60, a % 60, laps);
        else sprintf_s(out, n, "%d:%02d", a / 60, a % 60);
    };
    // "41:20 (18.0 laps)", or laps only without a lap time.
    auto lasts = [&](char* out, size_t n, double litres, double rate, double* lapsOut)
    {
        *lapsOut = -1.0;
        if (rate <= 0.05 || litres < 0.0) { strcpy_s(out, n, "--"); return; }
        const double laps = litres / rate;
        *lapsOut = laps;
        if (lapTime > 5.0) timeLaps(out, n, laps * lapTime, laps);
        else sprintf_s(out, n, "%.1f laps", laps);
    };

    const bool prevBg = renderer.TextBackgroundEnabled();
    renderer.SetTextBackground(false);

    if (!advanced)
    {
        // Simple box: fuel at stint start and now, and how long each lasts
        // at recent use (no reserve taken off). The Now/Lasts cell is
        // coloured against what is left of the race: red runs out first,
        // amber under one lap spare, green more; white outside races.
        char fStart[32], fNow[32], lStart[48], lNow[48];
        const float* nowCol = white;
        if (fuel.valid)
        {
            double rate = fuel.fuelPerLap;
            if (s_recentCount > 0)
            {
                double sum = 0.0;
                for (int i = 0; i < s_recentCount; ++i) sum += s_recent[i];
                rate = sum / s_recentCount;
            }
            sprintf_s(fStart, "%.1f L", s_stintStartFuel);
            sprintf_s(fNow, "%.1f L", fuel.fuelLoad);
            double lapsS = -1.0, lapsN = -1.0;
            lasts(lStart, sizeof(lStart), s_stintStartFuel, rate, &lapsS);
            lasts(lNow, sizeof(lNow), fuel.fuelLoad, rate, &lapsN);
            // raceLapsRemainingEstimate already runs a timed race to the end
            // of the lap you are on when the clock hits 0:00.
            if (fuel.isRaceSession && lapsN >= 0.0 && fuel.raceLapsRemainingEstimate >= 0.0 && lapTime > 5.0)
            {
                const double spareSec = (lapsN - fuel.raceLapsRemainingEstimate) * lapTime;
                const double oneLap = ((s_bestLap > 5.0) ? s_bestLap : lapTime) * 1.2;
                nowCol = (spareSec < 0.0) ? red : (spareSec < oneLap ? amber : green);
            }
        }
        else
        {
            strcpy_s(fStart, "60.0 L"); strcpy_s(fNow, "41.5 L");
            strcpy_s(lStart, "60:00 (26.0 laps)"); strcpy_s(lNow, "41:20 (18.0 laps)");
        }
        const char* hdr[3] = { "", "At stint start", "Now" };
        const char* r1[3] = { "Fuel", fStart, fNow };
        const char* r2[3] = { "Lasts", lStart, lNow };
        const char* const* rows[3] = { hdr, r1, r2 };
        float colW[3] = {};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                colW[c] = (std::max)(colW[c], renderer.MeasureText(rows[r][c], infoPx));
        const float halfW = (colW[0] + colW[1] + colW[2] + 2.0f * infoPx) * 0.5f + pad;
        const float halfH = rowStep * 1.5f + pad * 0.5f;
        Layout::Register("Fuel save", "FuelSave", "CenterXFraction", "CenterYFraction",
                         &cfg.centerXFraction, &cfg.centerYFraction, cx, cy, halfW, halfH, &cfg.scale, "Scale");
        // Standard panel plate (0.08 grey, 60%); shapes draw before text.
        renderer.AddFilledRoundedRect(cx, cy, halfW, halfH, 8.0f * s, 0.0f, 0.08f, 0.08f, 0.08f, 0.6f);
        const float left = cx - halfW + pad;
        const float col1R = left + colW[0] + infoPx + colW[1];
        const float col2R = cx + halfW - pad;
        const float top = cy - rowStep * 1.5f;
        for (int r = 0; r < 3; ++r)
        {
            const float y = top + r * rowStep + (rowStep - infoPx) * 0.5f;
            const float hg = (r == 0) ? 0.7f : 0.9f;   // column titles a little dimmer
            const float* c2 = (r == 2) ? nowCol : white;
            renderer.AddText(left, y, rows[r][0], infoPx, 0.9f, 0.9f, 0.9f, 1.0f, Renderer::HAlign::Left);
            renderer.AddText(col1R, y, rows[r][1], infoPx, hg, hg, hg, 1.0f, Renderer::HAlign::Right);
            renderer.AddText(col2R, y, rows[r][2], infoPx, r == 0 ? hg : c2[0], r == 0 ? hg : c2[1], r == 0 ? hg : c2[2],
                             1.0f, Renderer::HAlign::Right);
        }
        renderer.SetTextBackground(prevBg);
        return true;
    }

    constexpr int kRows = 6;
    const float tableH = rowStep * kRows + pad;
    Layout::Register("Fuel save", "FuelSave", "CenterXFraction", "CenterYFraction",
                     &cfg.centerXFraction, &cfg.centerYFraction,
                     cx, cy + (gap + tableH) * 0.5f, barHalfW, barHalfH + (gap + tableH) * 0.5f,
                     &cfg.scale, "Scale");

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

    // The table under the bar: one question per row, label left, value right.
    //   Stint target   32:00 (11.0 laps), 6.10 L/lap   the stint you set
    //   Stint left     23:38 (9.2 laps)    still to drive in it
    //   Fuel lasts     56:22 (17.5 laps)   at last lap's use; green if it
    //                                      covers "Stint left", red if not
    //   Fuel per lap target  6.10 L, last -0.22   L/lap you can afford, and
    //                                      last lap's difference to it
    //   Pit stops left 2                   races: stops still needed
    //   Fuel for last stop  38.4 L         races: litres at the final stop
    // Drawn with or without a target (review F338), so the race rows stay
    // up in the pit lane and after the stint time has run out.
    const double rate = (s_lastLapUsed > 0.05) ? s_lastLapUsed : fuel.fuelPerLap;
    double lapsTarget = -1.0, leftSec = -1.0, lapsLeft = -1.0, lastsSec = -1.0, lastsLaps = -1.0;
    if (lapTime > 5.0) lapsTarget = stintTargetSec / lapTime;
    if (fuel.valid && stintSec() >= 0.0)
    {
        leftSec = stintTargetSec - stintSec();
        if (leftSec < 0.0) leftSec = 0.0;
        if (lapTime > 5.0) lapsLeft = leftSec / lapTime;
    }
    if (fuel.valid && rate > 0.05)
    {
        lastsLaps = (fuel.fuelLoad - reserve) / rate;
        if (lastsLaps < 0.0) lastsLaps = 0.0;
        if (lapTime > 5.0) lastsSec = lastsLaps * lapTime;
    }

    char line[96];
    if (haveTarget)
        sprintf_s(line, s_haveProfile ? "FUEL %+.2f L" : "FUEL %+.2f L (learning)", delta);
    else if (placeholder && !fuel.valid)
        sprintf_s(line, "FUEL +0.00 L");
    else if (lapsLeft >= 0.0 && lapsLeft <= 0.5)
        sprintf_s(line, "FUEL: stint time up");
    else if (fuel.inPits)
        sprintf_s(line, "FUEL: target set after pit exit");
    else
        sprintf_s(line, "FUEL: target after first lap");
    renderer.AddText(cx, cy - textPx * 0.5f, line, textPx, 1.0f, 1.0f, 1.0f, 1.0f, Renderer::HAlign::Center);

    char v1[48], v2[48], v3[48], v4[64], v5[32], v6[48];
    timeLaps(v1, sizeof(v1), stintTargetSec, lapsTarget);
    {
        // ... and the litres per lap that stint allows.
        char lpl[24];
        if (haveTarget) sprintf_s(lpl, ", %.2f L/lap", s_target);
        else strcpy_s(lpl, ", -- L/lap");
        strcat_s(v1, lpl);
    }
    timeLaps(v2, sizeof(v2), leftSec, lapsLeft);
    if (lastsSec >= 0.0) timeLaps(v3, sizeof(v3), lastsSec, lastsLaps);
    else if (lastsLaps >= 0.0) sprintf_s(v3, "%.1f laps", lastsLaps);
    else strcpy_s(v3, "--");
    const bool lastsKnown = lastsLaps >= 0.0 && lapsLeft >= 0.0;
    const bool lastsOk = !lastsKnown || lastsLaps >= lapsLeft;
    // The litres per lap you can afford (the target), then last lap's
    // difference to it: - under (green), + over (red).
    if (!haveTarget)
        strcpy_s(v4, "--");
    else if (s_lastLapUsed > 0.0)
        sprintf_s(v4, "%.2f L, last %+.2f", s_target, s_lastLapDiff);
    else
        sprintf_s(v4, "%.2f L, last --", s_target);
    const bool lastOk = s_lastLapUsed <= 0.0 || s_lastLapDiff <= 0.0;

    // Stops and last-stop litres, races only (review F19/F313; same idea as
    // CrewChief: the shortfall to the flag incl. reserve, split into full
    // tanks plus what is left for the final stop).
    // Fuel per stop (2026-11-24): the shortfall is split evenly over the
    // stops left -- the ones the fuel needs, or more if the race rules
    // require more (RequiredStops) -- so the stints are about equal and the
    // car carries the least fuel on average. Same total litres as filling up
    // and splashing at the end, so the same time at the pump.
    bool finishOk = true, haveFinish = false;
    bool perStop = false;
    const int reqLeft = (std::max)(0, cfg.requiredStops - fuel.pitStopsDone);
    if (fuel.valid && fuel.isRaceSession && fuel.fuelNeededForSession >= 0.0)
    {
        haveFinish = true;
        const double shortL = fuel.fuelNeededForSession + reserve - fuel.fuelLoad;
        const double cap = TankCapacity(fuel);
        finishOk = shortL <= 0.0;
        const int fuelStops = finishOk ? 0 : (cap > 1.0 ? static_cast<int>(ceil(shortL / cap - 1e-6)) : -1);
        const int stops = (std::max)(fuelStops, reqLeft);
        if (fuelStops >= 0 && stops > 0)
        {
            perStop = true;
            if (reqLeft > fuelStops) sprintf_s(v5, "%d (required)", stops);
            else sprintf_s(v5, "%d", stops);
            if (finishOk) strcpy_s(v6, "0 L, just stop");
            else sprintf_s(v6, "%.1f L each", shortL / stops);
        }
        else if (finishOk)
        {
            strcpy_s(v5, "0");
            sprintf_s(v6, "none, %.1f L spare", -shortL);
        }
        else if (cap > 1.0)
        {
            const int stops = static_cast<int>(ceil(shortL / cap - 1e-6));
            sprintf_s(v5, "%d", stops);
            sprintf_s(v6, "%.1f L", shortL - (stops - 1) * cap);
        }
        else
        {
            // Tank size unknown: the whole shortfall.
            strcpy_s(v5, "--");
            sprintf_s(v6, "add %.1f L in all", shortL);
        }
    }
    else
    {
        const char* na = (fuel.valid && !fuel.isRaceSession) ? "-- (race only)" : "--";
        strcpy_s(v5, na);
        strcpy_s(v6, na);
    }

    // The table's numbers refresh 5x a second (user, 2026-11-24): readable
    // instead of twitching at the box's 25 Hz rebuild, which stays for the
    // bar above. Layout mode always shows the live values.
    static struct { char v[kRows][64]; const float* col[kRows]; bool perStop; bool valid; ULONGLONG ms; } s_txt = {};
    {
        const ULONGLONG nowMs = GetTickCount64();
        if (!s_txt.valid || nowMs - s_txt.ms >= 200 || Layout::Active())
        {
            const char* vNow[kRows] = { v1, v2, v3, v4, v5, v6 };
            const float* cNow[kRows] = { white, white, lastsKnown ? (lastsOk ? green : red) : white,
                                         haveTarget ? (lastOk ? green : red) : white,
                                         white, haveFinish ? (finishOk ? green : red) : white };
            for (int row = 0; row < kRows; ++row)
            {
                strcpy_s(s_txt.v[row], vNow[row]);
                // The colour arrays are locals; keep which one, not the pointer.
                s_txt.col[row] = cNow[row] == green ? nullptr : (cNow[row] == red ? red : white);
            }
            s_txt.perStop = perStop;
            s_txt.valid = true;
            s_txt.ms = nowMs;
        }
    }
    const char* labels[kRows] = { "Stint target", "Stint left", "Fuel lasts", "Fuel per lap target",
                                  "Pit stops left", s_txt.perStop ? "Fuel per stop" : "Fuel for last stop" };
    const char* values[kRows] = { s_txt.v[0], s_txt.v[1], s_txt.v[2], s_txt.v[3], s_txt.v[4], s_txt.v[5] };
    const float* cols[kRows];
    for (int row = 0; row < kRows; ++row)
        cols[row] = s_txt.col[row] == nullptr ? green : (s_txt.col[row] == red ? red : white);

    const float top = cy + barHalfH + gap;
    // As wide as the bar, or wider if the longest label + value needs it.
    float labelW = 0.0f, valueW = 0.0f;
    for (int row = 0; row < kRows; ++row)
    {
        labelW = (std::max)(labelW, renderer.MeasureText(labels[row], infoPx));
        valueW = (std::max)(valueW, renderer.MeasureText(values[row], infoPx));
    }
    const float tableHalfW = (std::max)(barHalfW, (labelW + valueW + 2.0f * infoPx) * 0.5f + pad);
    const float left = cx - tableHalfW, right = cx + tableHalfW;
    // Standard panel plate (0.08 grey, 60%); shapes draw before text.
    renderer.AddFilledRoundedRect(cx, top + rowStep * kRows * 0.5f, tableHalfW, rowStep * kRows * 0.5f + pad * 0.5f, 8.0f * s, 0.0f,
                                  0.08f, 0.08f, 0.08f, 0.6f);
    for (int row = 0; row < kRows; ++row)
    {
        const float y = top + row * rowStep + (rowStep - infoPx) * 0.5f;
        renderer.AddText(left + pad, y, labels[row], infoPx, 0.9f, 0.9f, 0.9f, 1.0f, Renderer::HAlign::Left);
        renderer.AddText(right - pad, y, values[row], infoPx, cols[row][0], cols[row][1], cols[row][2], 1.0f,
                         Renderer::HAlign::Right);
    }

    renderer.SetTextBackground(prevBg);
    return true;
}
