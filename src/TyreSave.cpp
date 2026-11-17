#include "TyreSave.h"
#include "TyreWear.h"
#include "FuelStore.h"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "Log.h"
#include <cmath>
#include <cstdio>
#include <cstring>

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
    double s_setSeconds = 0.0;            // driving time on this set, completed laps
    double s_lastLapTime = 0.0;

    void LoadProfile()
    {
        for (int w = 0; w < 4; ++w) s_haveProfile[w] = false;
        FILE* f = nullptr;
        if (fopen_s(&f, s_profilePath, "r") != 0 || !f) return;
        int n = 0;
        for (; n <= kBins; ++n)
            if (fscanf_s(f, "%f %f %f %f", &s_profile[0][n], &s_profile[1][n], &s_profile[2][n], &s_profile[3][n]) != 4) break;
        fclose(f);
        if (n == kBins + 1)
            for (int w = 0; w < 4; ++w) s_haveProfile[w] = true;
    }

    void SaveProfile()
    {
        if (!s_profilePath[0]) return;
        for (int w = 0; w < 4; ++w) if (!s_haveProfile[w]) return;
        FILE* f = nullptr;
        if (fopen_s(&f, s_profilePath, "w") != 0 || !f) { Log::Write("TyreSave: cannot write %s", s_profilePath); return; }
        for (int i = 0; i <= kBins; ++i)
            fprintf(f, "%.5f %.5f %.5f %.5f\n", s_profile[0][i], s_profile[1][i], s_profile[2][i], s_profile[3][i]);
        fclose(f);
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
        for (int w = 0; w < 4; ++w) { s_target[w] = -1.0; s_lastLap[w] = -1.0; s_lastLapTarget[w] = -1.0; }
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

bool TyreSave::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    TyreSaveConfig& cfg = g_tyreSaveConfig;
    if (!cfg.enabled || cfg.targetMinutes <= 0.0f)
        return false;

    const FuelStore::State fuel = g_fuelStore.Read();
    const TyreWearStore::State tyres = g_tyreWear.Read();
    const bool placeholder = Layout::Active();
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
            LoadProfile();
            s_lap = -1;
            NewSet();
        }

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
                if (lapTime > 5.0) s_setSeconds += lapTime;
                bool learned = false;
                for (int w = 0; w < 4; ++w)
                {
                    const double used = worn[w] - s_wornAtLapStart[w];
                    if (used <= 0.0) continue;
                    if (s_target[w] > 0.0)
                    {
                        s_lastLap[w] = used;
                        s_lastLapTarget[w] = s_target[w];
                    }
                    if (s_lapClean) { LearnLap(w, used); learned = true; }
                }
                if (learned) SaveProfile();
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

        if (fuel.inPits) s_lapClean = false;
        const int bin = static_cast<int>(lapFrac * kBins);
        if (bin > 0 && bin < kBins)
            for (int w = 0; w < 4; ++w)
                if (s_lapWorn[w][bin] < 0.0f)
                    s_lapWorn[w][bin] = static_cast<float>(worn[w] - s_wornAtLapStart[w]);
    }

    // Geometry (2160p reference): 2x2 boxes in car order, a header above.
    const float s = static_cast<float>(screenHeight) / 2160.0f * (cfg.scale > 0.05f ? cfg.scale : 0.05f);
    const float boxHalfW = 120.0f * s;
    const float boxHalfH = 46.0f * s;
    const float gap = 10.0f * s;
    const float textPx = 34.0f * s;
    const float headPx = 28.0f * s;
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
        const double setNow = s_setSeconds + (lapTime > 5.0 ? lapFrac * lapTime : 0.0);
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

    const char* const names[4] = { "FL", "FR", "RL", "RR" };
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
        if (live && s_lastLap[w] > 0.0) sprintf_s(l1, "%s  Last %.2f", names[w], s_lastLap[w]);
        else sprintf_s(l1, "%s  Last --", names[w]);
        if (haveTarget) sprintf_s(l2, "Need %.2f", s_target[w]);
        else if (live && s_target[w] == 0.0) strcpy_s(l2, "Need: at target");
        else strcpy_s(l2, "Need --");

        float tr = 1.0f, tg = 1.0f, tb = 1.0f;
        if (live && s_lastLap[w] > 0.0 && s_lastLapTarget[w] > 0.0)
        {
            const bool over = s_lastLap[w] > s_lastLapTarget[w];
            tr = over ? 1.0f : 0.35f; tg = over ? 0.35f : 0.95f; tb = over ? 0.3f : 0.4f;
        }
        renderer.AddText(bx, by - textPx - 2.0f * s, l1, textPx, tr, tg, tb, 1.0f, Renderer::HAlign::Center);
        renderer.AddText(bx, by + 2.0f * s, l2, textPx, 0.95f, 0.95f, 0.95f, 1.0f, Renderer::HAlign::Center);
    }

    renderer.SetTextBackground(prevBg);
    return true;
}
