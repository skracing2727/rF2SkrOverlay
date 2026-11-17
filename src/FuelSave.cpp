#include "FuelSave.h"
#include "FuelStore.h"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "Log.h"
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

    void LoadProfile(const char* path)
    {
        strcpy_s(s_profilePath, path);
        s_haveProfile = false;
        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") != 0 || !f) return;
        int n = 0;
        for (; n <= kBins; ++n)
            if (fscanf_s(f, "%f", &s_profile[n]) != 1) break;
        fclose(f);
        s_haveProfile = (n == kBins + 1);
    }

    void SaveProfile()
    {
        if (!s_profilePath[0]) return;
        FILE* f = nullptr;
        if (fopen_s(&f, s_profilePath, "w") != 0 || !f) { Log::Write("FuelSave: cannot write %s", s_profilePath); return; }
        for (int i = 0; i <= kBins; ++i) fprintf(f, "%.5f\n", s_profile[i]);
        fclose(f);
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
        SaveProfile();
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

bool FuelSave::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    FuelSaveConfig& cfg = g_fuelSaveConfig;
    if (!cfg.enabled || cfg.stintMinutes <= 0.0f)
        return false;

    const FuelStore::State fuel = g_fuelStore.Read();
    const bool placeholder = Layout::Active();
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
            LoadProfile(fuel.profilePath);
            s_target = -1.0; s_bankDone = 0.0; s_lastLapUsed = -1.0; s_lap = -1;
        }

        // Pit visit or refuel: new stint, new target.
        const bool refuelled = s_prevLapStartFuel >= 0.0 && fuel.fuelLoad > s_prevLapStartFuel + 0.5;
        if (fuel.inPits || refuelled)
        {
            s_target = -1.0;
            s_bankDone = 0.0;
            s_lapClean = false;
            if (refuelled) s_prevLapStartFuel = fuel.fuelLoad;
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
                    if (s_target > 0.0)
                    {
                        s_lastLapUsed = used;
                        s_lastLapDiff = used - s_target;
                        s_bankDone += s_target - used;
                    }
                    if (s_lapClean)
                        LearnLap(used);
                }
            }
            s_lap = fuel.telemLapNumber;
            s_prevLapStartFuel = fuel.fuelAtLapStart;
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
                     cx, cy + (gap + infoPx) * 0.5f, barHalfW, barHalfH + (gap + infoPx) * 0.5f,
                     &cfg.scale, "Scale");

    const bool prevBg = renderer.TextBackgroundEnabled();
    renderer.SetTextBackground(false);

    const bool haveTarget = fuel.valid && s_target > 0.0;
    const double usedThisLap = fuel.fuelAtLapStart - fuel.fuelLoad;
    // Positive = over the target's expected use at this point in the lap.
    const double expected = haveTarget ? s_target * ShareAt(lapFrac) : 0.0;
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

    // Info line: target | bank | last lap verdict, each coloured on its own.
    if (haveTarget)
    {
        char t1[32], t2[32], t3[64];
        sprintf_s(t1, "Target %.2f", s_target);
        // (order on screen: Target | Last | Stint projection)
        // Stint length the recent laps' rate is heading for.
        double projSec = -1.0;
        const double rate = (s_lastLapUsed > 0.05) ? s_lastLapUsed : fuel.fuelPerLap;
        if (rate > 0.05 && lapTime > 5.0 && fuel.stintTimeSeconds >= 0.0)
            projSec = fuel.stintTimeSeconds + ((fuel.fuelLoad - reserve) / rate) * lapTime;
        if (projSec >= 0.0)
        {
            const int a = static_cast<int>(projSec + 0.5), b = static_cast<int>(stintTargetSec + 0.5);
            sprintf_s(t2, "Stint %d:%02d / %d:%02d", a / 60, a % 60, b / 60, b % 60);
        }
        else
            strcpy_s(t2, "Stint --");
        const bool projOk = projSec < 0.0 || projSec >= stintTargetSec;
        if (s_lastLapUsed > 0.0)
        {
            if (s_lastLapDiff <= 0.0) sprintf_s(t3, "Last %.2f: %.2f spare", s_lastLapUsed, -s_lastLapDiff);
            else sprintf_s(t3, "Last %.2f: %.2f over", s_lastLapUsed, s_lastLapDiff);
        }
        else
            strcpy_s(t3, "Last --");
        const float sep = renderer.MeasureText("   ", infoPx);
        const float w1 = renderer.MeasureText(t1, infoPx), w2 = renderer.MeasureText(t2, infoPx), w3 = renderer.MeasureText(t3, infoPx);
        float x = cx - (w1 + w2 + w3 + 2.0f * sep) * 0.5f;
        const float y = cy + barHalfH + gap;
        renderer.AddText(x, y, t1, infoPx, 0.9f, 0.9f, 0.9f, 1.0f);
        x += w1 + sep;
        renderer.AddText(x, y, t2, infoPx, projOk ? 0.35f : 1.0f, projOk ? 0.9f : 0.35f, projOk ? 0.4f : 0.3f, 1.0f);
        x += w2 + sep;
        const bool lastOk = s_lastLapUsed <= 0.0 || s_lastLapDiff <= 0.0;
        renderer.AddText(x, y, t3, infoPx, lastOk ? 0.35f : 1.0f, lastOk ? 0.9f : 0.35f, lastOk ? 0.4f : 0.3f, 1.0f);
    }

    renderer.SetTextBackground(prevBg);
    return true;
}
