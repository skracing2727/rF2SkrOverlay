#include "FuelTyrePanel.h"
#include "FuelStore.h"
#include "TyreWear.h"
#include "SetupInfo.h"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "Shortcuts.h"
#include "RenderThrottle.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <tuple>

namespace
{
    void FormatTimeLeft(double seconds, char* out, size_t outSize)
    {
        if (seconds < 0.0) { sprintf_s(out, outSize, "--:--"); return; }
        if (seconds > 5999.0) { sprintf_s(out, outSize, ">99:59"); return; }
        const int totalSeconds = static_cast<int>(seconds + 0.5);
        sprintf_s(out, outSize, "%d:%02d", totalSeconds / 60, totalSeconds % 60);
    }

    // Tyre life spans (both remaining and full 100%->0%) can run well past
    // an hour on a light-wear set -- plain minutes reads more easily than
    // MM:SS at that scale.
    void FormatMinutes(double seconds, char* out, size_t outSize)
    {
        if (seconds < 0.0) { sprintf_s(out, outSize, "-- min"); return; }
        const double minutes = seconds / 60.0;
        if (minutes > 999.0) { sprintf_s(out, outSize, ">999 min"); return; }
        sprintf_s(out, outSize, "%.0f min", minutes);
    }

    // One box plot (see TyreWearStore::WearDistribution), horizontal, between
    // leftX and rightX on the value scale lo..hi: whiskers as a thin line
    // with end caps, the middle half as a translucent box with an outline,
    // the median as a bright bar, outliers as small dots, and the last
    // counted lap as the red point on top of everything.
    void DrawWearBoxPlot(Renderer& renderer, const TyreWearStore::WearDistribution& d,
                         float leftX, float rightX, float centerY, float height,
                         float lo, float hi, const FuelTyrePanelConfig& cfg)
    {
        if (d.count <= 0 || hi <= lo)
            return;
        const float span = rightX - leftX;
        auto X = [&](float v) { return leftX + (v - lo) / (hi - lo) * span; };
        const float thin = (std::max)(1.0f, height * 0.08f);
        const float halfH = height * 0.5f;
        const float pr = cfg.wearPlotR, pg = cfg.wearPlotG, pb = cfg.wearPlotB;

        const float wl = X(d.whiskerLo), wh = X(d.whiskerHi);
        renderer.AddFilledRect((wl + wh) * 0.5f, centerY, (std::max)((wh - wl) * 0.5f, thin * 0.5f), thin * 0.5f, 0.0f, pr, pg, pb, 0.9f);
        renderer.AddFilledRect(wl, centerY, thin * 0.5f, halfH * 0.6f, 0.0f, pr, pg, pb, 0.9f);
        renderer.AddFilledRect(wh, centerY, thin * 0.5f, halfH * 0.6f, 0.0f, pr, pg, pb, 0.9f);

        const float bl = X(d.q1), br = X(d.q3);
        const float boxHalfW = (std::max)((br - bl) * 0.5f, thin);
        const float boxCx = (bl + br) * 0.5f;
        renderer.AddFilledRect(boxCx, centerY, boxHalfW, halfH, 0.0f, pr, pg, pb, 0.35f);
        renderer.AddFilledRect(boxCx, centerY - halfH + thin * 0.5f, boxHalfW, thin * 0.5f, 0.0f, pr, pg, pb, 0.9f);
        renderer.AddFilledRect(boxCx, centerY + halfH - thin * 0.5f, boxHalfW, thin * 0.5f, 0.0f, pr, pg, pb, 0.9f);
        renderer.AddFilledRect(boxCx - boxHalfW + thin * 0.5f, centerY, thin * 0.5f, halfH, 0.0f, pr, pg, pb, 0.9f);
        renderer.AddFilledRect(boxCx + boxHalfW - thin * 0.5f, centerY, thin * 0.5f, halfH, 0.0f, pr, pg, pb, 0.9f);

        renderer.AddFilledRect(X(d.median), centerY, thin, halfH, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);

        for (int i = 0; i < d.outlierCount; ++i)
            renderer.AddFilledCircle(X(d.outliers[i]), centerY, height * 0.12f, pr, pg, pb, 0.8f, 12);

        if (d.lastLap >= 0.0f)
        {
            const float lx = X(d.lastLap);
            renderer.AddFilledCircle(lx, centerY, height * 0.24f + thin, 0.0f, 0.0f, 0.0f, 0.9f, 16);
            renderer.AddFilledCircle(lx, centerY, height * 0.24f, cfg.wearLastLapR, cfg.wearLastLapG, cfg.wearLastLapB, 1.0f, 16);
        }
    }

    // Value range covering everything a plot will draw (whiskers, outliers,
    // last lap), padded a little so nothing sits on the very edge, and never
    // narrower than 0.05 %/lap so a near-identical run of laps is not blown
    // up into a misleadingly wide box.
    void WearPlotRange(const TyreWearStore::WearDistribution* d, int n, float& lo, float& hi)
    {
        bool any = false;
        for (int i = 0; i < n; ++i)
        {
            if (d[i].count <= 0) continue;
            float a = d[i].whiskerLo, b = d[i].whiskerHi;
            for (int k = 0; k < d[i].outlierCount; ++k) { a = (std::min)(a, d[i].outliers[k]); b = (std::max)(b, d[i].outliers[k]); }
            if (d[i].lastLap >= 0.0f) { a = (std::min)(a, d[i].lastLap); b = (std::max)(b, d[i].lastLap); }
            lo = any ? (std::min)(lo, a) : a;
            hi = any ? (std::max)(hi, b) : b;
            any = true;
        }
        if (!any) { lo = 0.0f; hi = 1.0f; return; }
        const float minSpan = 0.05f;
        if (hi - lo < minSpan)
        {
            const float mid = (lo + hi) * 0.5f;
            lo = mid - minSpan * 0.5f;
            hi = mid + minSpan * 0.5f;
        }
        const float pad = (hi - lo) * 0.06f;
        lo -= pad;
        hi += pad;
    }

    // One "label ......... value" row, value right-aligned -- used for both
    // groups so the whole panel reads as one consistent little table rather
    // than free-form sentences.
    // 'alert' is the existing two-state (normal / red) flag. 'status' is
    // the separate three-state fuel-versus-race verdict, which overrides it
    // where it applies.
    enum class RowStatus { Plain, Good, Marginal, Short };

    struct Row
    {
        char label[32];
        char value[40];
        bool alert;
        RowStatus status = RowStatus::Plain;
    };
}

namespace
{
    // Bottom edge of the panel as last drawn. Cached rather than recomputed
    // on demand because the height depends on which row groups applied that
    // frame, which is most of the work Draw() does.
    float s_lastBottomEdgeY = -1.0f;
    // Per-box bottom edges: BottomEdgeY() (below) reports the LOWEST of the
    // three, since a caller anchoring underneath the whole widget needs to
    // clear whichever box currently sits lowest, and the three no longer
    // necessarily stack in the same fixed order they were drawn in once one
    // has been dragged elsewhere.
    float s_lastFuelBottomY = -1.0f;
    float s_lastTyresBottomY = -1.0f;
    float s_lastBrakeBiasBottomY = -1.0f;
}

float FuelTyrePanel::BottomEdgeY(int screenWidth, int screenHeight)
{
    (void)screenWidth;
    if (s_lastBottomEdgeY >= 0.0f)
        return s_lastBottomEdgeY;
    return screenHeight * g_fuelTyrePanelConfig.fuelPosYFraction;
}

bool FuelTyrePanel::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    // Low tier (~5Hz): fuel/tyre wear change over minutes, not frames, so
    // skip the rebuild on frames not due and replay the last one. The
    // fuel/tyre marginal-or-critical alert checks below still run every time
    // this actually redraws, i.e. still at least ~5Hz -- plenty for a tier
    // transition that only matters once every several laps. Bypassed in
    // layout mode so dragging/resizing the three boxes stays smooth.
    static RenderThrottle s_throttle(0.2);
    if (!Layout::Active() && s_throttle.TryReplay(renderer))
        return true;
    RenderThrottle::Capture throttleCapture(s_throttle, renderer);

    // This widget paints its own background panel, so the renderer's
    // per-string text plate would just double-darken over it. Restored on
    // every exit path by the scope guard, since Draw() returns from several
    // places.
    struct TextBgGuard
    {
        Renderer& r; bool prev;
        explicit TextBgGuard(Renderer& rr) : r(rr), prev(rr.TextBackgroundEnabled()) { r.SetTextBackground(false); }
        ~TextBgGuard() { r.SetTextBackground(prev); }
    } textBgGuard(renderer);

    const FuelTyrePanelConfig& cfg = g_fuelTyrePanelConfig;
    if (!cfg.enabled)
        return false;

    const FuelStore::State fuel = g_fuelStore.Read();
    const TyreWearStore::State tyres = g_tyreWear.Read();
    const SetupInfoStore::State setup = g_setupInfo.Read();
    // Tyre wear is just as worth watching in practice -- that is where tyre
    // life gets learned in the first place. Only the end-of-race projection
    // inside this group is race-specific, and it already gates itself on a
    // valid race-laps-remaining figure.
    const bool showTyres = tyres.valid;
    if (!fuel.valid && !showTyres && !setup.valid)
        return false;

    // Hold the just-completed lap's own numbers for a few seconds after the
    // line rather than the new lap's near-zero ones -- see
    // FuelTyrePanelConfig::tyreFreezeAfterLineSeconds.
    bool tyreFreezeActive = false;
    if (tyres.haveLineFreeze && cfg.tyreFreezeAfterLineSeconds > 0.0f && tyres.lineCrossedAt.QuadPart != 0)
    {
        LARGE_INTEGER freq, now;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&now);
        if (freq.QuadPart != 0)
        {
            const double sinceLine = static_cast<double>(now.QuadPart - tyres.lineCrossedAt.QuadPart) / static_cast<double>(freq.QuadPart);
            tyreFreezeActive = sinceLine >= 0.0 && sinceLine < cfg.tyreFreezeAfterLineSeconds;
        }
    }
    const TyreWearStore::WheelState* tyreWheels = tyreFreezeActive ? tyres.frozenWheel : tyres.wheel;

    // Every *Px value in this config is a literal pixel count tuned against
    // a 3840x2160 reference -- unlike the radar/pedal-trace widgets, which
    // derive their internal sizes as fractions of their own already-scaled
    // panel, so they track resolution automatically. Scaling every pixel
    // value here by the same ratio keeps this panel the same fraction of
    // the screen at any resolution instead of ballooning at lower ones.
    //
    // Fuel, Tyres and Brake Bias are three independently-sized boxes now
    // (see beginBox() below), each with its own scale -- so geometry that
    // feeds a SPECIFIC box's own layout is measured with THAT box's own
    // uiScale, computed per-box further down. resScale is only the bare
    // screen-resolution ratio, with no panel scale folded in, for the ONE
    // value that has to be common to all three: the gap used when
    // auto-anchoring one box directly under another.
    const float resScale = static_cast<float>(screenHeight) / 2160.0f;
    const float fuelUiScale = resScale * cfg.fuelScale;
    const float tyresUiScale = resScale * cfg.tyresScale;
    const float brakeBiasUiScale = resScale * cfg.brakeBiasScale;
    const float groupGapPx = cfg.groupGapPx * resScale;

    // ---- Fuel group: one row per fact, label left / value right. Ordered
    // as a narrative -- what you have and how long it lasts, then what you
    // still need (to finish, and for the whole race). Labels spell out
    // exactly what each number means rather than a bare one-word tag, since
    // several of these are easy to conflate at a glance (fuel remaining vs
    // race remaining, partial need vs whole-race need).
    Row fuelRows[8];
    int fuelRowCount = 0;
    bool fuelInsufficient = false;

    // Green means the tank comfortably outlasts what the race still needs;
    // orange ("marginal") means it only just does; red means it does not.
    // Boundaries are deliberately inclusive on the marginal side -- exactly
    // enough fuel is marginal, not comfortable.
    auto fuelVerdict = [&](double have, double need, double margin) -> RowStatus
    {
        if (need < 0.0) return RowStatus::Plain;
        if (have < need) return RowStatus::Short;
        if (have <= need + margin) return RowStatus::Marginal;
        return RowStatus::Good;
    };

    if (fuel.valid)
    {
        const double lapsFromFuel = (fuel.fuelPerLap > 0.01) ? (fuel.fuelLoad / fuel.fuelPerLap) : -1.0;
        const double minsFromFuel = (fuel.timeLeftSeconds >= 0.0) ? (fuel.timeLeftSeconds / 60.0) : -1.0;

        if (!fuel.isRaceSession)
        {
            // Practice/qualy: there is no race distance to measure fuel
            // against, so "Fuel To Finish", "Fuel Delta" and "Full Race
            // Fuel" are all meaningless here and are dropped. What is
            // actually useful is what the stint was filled with and what is
            // left -- each in BOTH minutes and laps, since which one matters
            // depends on whether the run is being timed or counted.
            const double stintLaps = (fuel.stintStartFuel > 0.0 && fuel.fuelPerLap > 0.01)
                                   ? (fuel.stintStartFuel / fuel.fuelPerLap) : -1.0;
            // Minutes are derived by scaling the live "left" figure in
            // proportion, rather than as a second independently-derived
            // estimate that could visibly disagree with it.
            const double stintMins = (stintLaps >= 0.0 && minsFromFuel >= 0.0 && lapsFromFuel > 0.01)
                                   ? (stintLaps * minsFromFuel / lapsFromFuel) : -1.0;

            Row& stint = fuelRows[fuelRowCount++];
            strcpy_s(stint.label, "Fuel Stint");
            if (stintMins >= 0.0 && stintLaps >= 0.0)
                sprintf_s(stint.value, "%.0f min / %.1f laps", stintMins, stintLaps);
            else
                strcpy_s(stint.value, "-- / --");
            stint.alert = false;

            Row& left = fuelRows[fuelRowCount++];
            strcpy_s(left.label, "Fuel Left");
            if (minsFromFuel >= 0.0 && lapsFromFuel >= 0.0)
                sprintf_s(left.value, "%.0f min / %.1f laps", minsFromFuel, lapsFromFuel);
            else
                strcpy_s(left.value, "-- / --");
            left.alert = false;

            // Running time of the CURRENT stint (since the last pit exit, or
            // session start if there hasn't been one yet) -- session/stint
            // lap counts used to live here too, but those are already on the
            // leaderboard (RelativeTiming's "Total"/"Stint" laps figure), so
            // showing them a second time here just repeated it; this is the
            // one figure that panel doesn't have.
            Row& stintTime = fuelRows[fuelRowCount++];
            strcpy_s(stintTime.label, "Stint Time");
            FormatTimeLeft(fuel.stintTimeSeconds, stintTime.value, sizeof(stintTime.value));
            stintTime.alert = false;
        }
        else
        {
            Row& load = fuelRows[fuelRowCount++];
            strcpy_s(load.label, "Fuel Load");
            sprintf_s(load.value, "%.1f L", fuel.fuelLoad);
            load.alert = false;

            Row& time = fuelRows[fuelRowCount++];
            strcpy_s(time.label, "Fuel Time Left");
            FormatTimeLeft(fuel.timeLeftSeconds, time.value, sizeof(time.value));
            time.alert = false;
            // Only a TIMED race has a race-minutes figure to compare
            // against; in a lap race the laps row below carries the verdict
            // instead and this one stays plain.
            if (fuel.isTimedRace && minsFromFuel >= 0.0 && fuel.raceMinutesLeft >= 0.0)
            {
                time.status = fuelVerdict(minsFromFuel, fuel.raceMinutesLeft, cfg.fuelMarginMinutes);
                if (time.status == RowStatus::Marginal)
                    strcat_s(time.value, "  marginal");
            }

            // Same current fuel load, in LAPS rather than time -- directly
            // comparable against the race's own remaining laps, which a
            // time-based figure alone leaves the driver to work out.
            RowStatus lapsLeftStatus = RowStatus::Plain;
            if (lapsFromFuel >= 0.0)
            {
                Row& lapsLeft = fuelRows[fuelRowCount++];
                strcpy_s(lapsLeft.label, "Fuel Laps Left");
                sprintf_s(lapsLeft.value, "%.1f laps", lapsFromFuel);
                lapsLeft.alert = false;
                // A lap race is compared against its exact remaining laps
                // with a one-lap cushion; a timed race against the same
                // estimate the rest of the panel uses.
                const double needLaps = (fuel.isLapRace && fuel.lapsLeft >= 0.0)
                                      ? fuel.lapsLeft : fuel.raceLapsRemainingEstimate;
                lapsLeft.status = fuelVerdict(lapsFromFuel, needLaps, cfg.fuelMarginLaps);
                if (lapsLeft.status == RowStatus::Marginal)
                    strcat_s(lapsLeft.value, "  marginal");
                lapsLeftStatus = lapsLeft.status;
            }

            // One-off popup on WORSENING transitions only (not every frame
            // while the state holds, and not on improving moves -- finally
            // topping up fuel shouldn't page you). Static, so it survives
            // across frames; each session/car starts fresh via s_lastFuelTier
            // never having seen a Marginal/Short reading yet.
            {
                static int s_lastFuelTier = 0; // 0 good, 1 marginal, 2 short
                int fuelTier = 0;
                if (time.status == RowStatus::Short || lapsLeftStatus == RowStatus::Short) fuelTier = 2;
                else if (time.status == RowStatus::Marginal || lapsLeftStatus == RowStatus::Marginal) fuelTier = 1;
                if (fuelTier > s_lastFuelTier)
                {
                    char msg[128];
                    sprintf_s(msg, "FUEL %s -- %.0f min / %.1f laps left",
                              fuelTier == 2 ? "SHORT" : "MARGINAL", minsFromFuel, lapsFromFuel);
                    Shortcuts::ShowMessage(msg, 3.0);
                }
                s_lastFuelTier = fuelTier;
            }

            if (fuel.fuelNeededForSession >= 0.0)
            {
                fuelInsufficient = fuel.fuelNeededForSession > fuel.fuelLoad;
                Row& need = fuelRows[fuelRowCount++];
                strcpy_s(need.label, "Fuel To Finish");
                sprintf_s(need.value, "%.1f L", fuel.fuelNeededForSession);
                // Red once the session would need more fuel than is loaded
                // -- a stop is actually required, not just informational.
                need.alert = fuelInsufficient;

                // Same comparison as one signed number, so the driver isn't
                // subtracting "Fuel To Finish" from "Fuel Load" themselves.
                Row& delta = fuelRows[fuelRowCount++];
                strcpy_s(delta.label, "Fuel Delta");
                sprintf_s(delta.value, "%+.1f L", fuel.fuelLoad - fuel.fuelNeededForSession);
                delta.alert = fuelInsufficient;
            }

            if (fuel.fuelRequiredForRace >= 0.0)
            {
                // The WHOLE race's requirement from the start -- distinct
                // from "Fuel To Finish" above, which is only the remaining
                // laps/time from right now.
                Row& total = fuelRows[fuelRowCount++];
                strcpy_s(total.label, "Full Race Fuel");
                sprintf_s(total.value, "%.1f L", fuel.fuelRequiredForRace);
                total.alert = false;
            }
        }
    }

    // ---- Tyre group: a 2x2 grid matching the car's actual corners (FL/FR
    // front, RL/RR rear) rather than a linear list, plus one label/value
    // summary row for remaining tyre time. Each cell is two segments so the
    // sudden-increase alert can colour just the %/lap part.
    static const char* kWheelLabel[4] = { "FL", "FR", "RL", "RR" };
    char cellDeg[4][20] = {};
    bool wheelAlert[4] = {};
    static const char* kTyreHeader = "Wear %/lap";
    Row tyreLifeRow = {};
    Row tyreEndRow = {};
    // Projecting tyre life to "the end of the race" means nothing in
    // practice or qualifying -- there is no race to project to, and the row
    // could only ever read "--". Tyre life itself still shows, since that is
    // exactly what a practice run is for.
    const bool showRaceEndProjection = fuel.isRaceSession;
    // 0 = no highlight, 1 = orange (projected 20-40% left at race end),
    // 2 = red (projected below 20%). Per-wheel, plus one summary value (the
    // worst of the four) for the "Tyre @ Race End" row's own highlight.
    int wheelEndOfRaceTier[4] = {};
    int tyreEndOfRaceTier = 0;
    // Wear box plots: only once at least one lap has been counted, and the
    // worst tyre is the one with the highest median wear per lap.
    int worstWearWheel = -1;
    if (showTyres && Shortcuts::Visible(Shortcuts::TyreWearPlots))
    {
        for (int w = 0; w < 4; ++w)
        {
            const TyreWearStore::WearDistribution& d = tyres.distribution[w];
            if (d.count > 0 && (worstWearWheel < 0 || d.median > tyres.distribution[worstWearWheel].median))
                worstWearWheel = w;
        }
    }
    const bool showWearPlots = worstWearWheel >= 0;
    if (showTyres)
    {
        for (int w = 0; w < 4; ++w)
        {
            // Clamped before formatting: a partially-initialised telemetry
            // struct around a session load can hand over a wild wear value,
            // and a long enough number would overrun these buffers --
            // sprintf_s aborts the process on overflow rather than truncating.
            const double deg = (std::min)((std::max)(tyreWheels[w].degradationThisLapPercent, 0.0), 999.0);
            // Bare number: the grid position already says which corner, and
            // the header line above the grid says what the unit is.
            _snprintf_s(cellDeg[w], sizeof(cellDeg[w]), _TRUNCATE, "%.1f", deg);
            wheelAlert[w] = tyreWheels[w].anomalyHigh;
        }

        // One summary line instead of a time per wheel: the tyre that will
        // reach 20% life first, and how long until it does. timeLeftSeconds
        // is time to 0%, so it is scaled linearly down to the 20% mark.
        int worstLifeWheel = -1;
        double worstLifeSeconds = 0.0;
        for (int w = 0; w < 4; ++w)
        {
            const double toZero = tyreWheels[w].timeLeftSeconds;
            const double rem = tyreWheels[w].remainingPercent;
            if (toZero < 0.0 || rem <= 0.0)
                continue;
            const double toTwenty = (std::max)(0.0, toZero * (rem - 20.0) / rem);
            if (worstLifeWheel < 0 || toTwenty < worstLifeSeconds)
            {
                worstLifeWheel = w;
                worstLifeSeconds = toTwenty;
            }
        }
        strcpy_s(tyreLifeRow.label, "To 20% Life");
        if (worstLifeWheel >= 0)
        {
            char t[16];
            FormatMinutes(worstLifeSeconds, t, sizeof(t));
            sprintf_s(tyreLifeRow.value, "%s %s", kWheelLabel[worstLifeWheel], t);
        }
        else
            strcpy_s(tyreLifeRow.value, "--");
        tyreLifeRow.alert = false;

        // Projected remaining % at the END of the race, PER WHEEL --
        // extrapolated linearly from each wheel's own current wear rate
        // (remainingPercent per its own lapsRemaining-to-zero) applied over
        // the race's remaining laps, rather than just flagging whether it
        // reaches zero early. Clamped at 0 once the race's remaining laps
        // would already exceed that wheel's own laps-to-zero. Same
        // orange/red tiers as the summary row below, but applied to each
        // individual tyre's own reading so a single bad corner is visible
        // even when the OTHER three are fine.
        int worstEndOfRaceWheel = -1;
        double worstEndOfRacePercent = 0.0;
        if (showRaceEndProjection && fuel.valid && fuel.raceLapsRemainingEstimate >= 0.0)
        {
            for (int w = 0; w < 4; ++w)
            {
                if (tyres.wheel[w].lapsRemaining <= 0.001)
                    continue;

                double projectedPercent = tyres.wheel[w].remainingPercent
                                         * (1.0 - fuel.raceLapsRemainingEstimate / tyres.wheel[w].lapsRemaining);
                if (projectedPercent < 0.0) projectedPercent = 0.0;

                if (projectedPercent < 20.0) wheelEndOfRaceTier[w] = 2;
                else if (projectedPercent < 40.0) wheelEndOfRaceTier[w] = 1;

                if (worstEndOfRaceWheel < 0 || projectedPercent < worstEndOfRacePercent)
                {
                    worstEndOfRaceWheel = w;
                    worstEndOfRacePercent = projectedPercent;
                }
            }
            // The summary row highlights whichever tyre is worst.
            for (int w = 0; w < 4; ++w)
                tyreEndOfRaceTier = (std::max)(tyreEndOfRaceTier, wheelEndOfRaceTier[w]);
        }

        // The worst tyre's OWN corner + its projected % remaining at the end
        // of the race -- the summary row above only conveys a colour tier,
        // not the actual number, or which of the four tyres it's about.
        strcpy_s(tyreEndRow.label, "Tyre @ Race End");
        if (worstEndOfRaceWheel >= 0)
            sprintf_s(tyreEndRow.value, "%s %.0f%%", kWheelLabel[worstEndOfRaceWheel], worstEndOfRacePercent);
        else
            strcpy_s(tyreEndRow.value, "--");
        tyreEndRow.alert = (tyreEndOfRaceTier == 2);

        // Same one-off, worsening-transition-only popup as the fuel alert
        // above.
        {
            static int s_lastTyreTier = 0; // 0 fine, 1 low, 2 critical
            if (tyreEndOfRaceTier > s_lastTyreTier)
            {
                char msg[128];
                sprintf_s(msg, "TYRE WEAR %s -- %s ~%.0f%% at race end",
                          tyreEndOfRaceTier == 2 ? "CRITICAL" : "LOW",
                          worstEndOfRaceWheel >= 0 ? kWheelLabel[worstEndOfRaceWheel] : "?",
                          worstEndOfRacePercent);
                Shortcuts::ShowMessage(msg, 3.0);
            }
            s_lastTyreTier = tyreEndOfRaceTier;
        }
    }

    // ---- Setup group: brake balance. Front/rear anti-roll bar was also
    // requested, but rFactor2's Internals Plugin API exposes no such field
    // anywhere -- there's genuinely nothing to read it from. ----
    Row setupRows[1];
    int setupRowCount = 0;
    if (setup.valid)
    {
        Row& brake = setupRows[setupRowCount++];
        strcpy_s(brake.label, "Brake Bias");
        // The live value, plus how far it has been moved from the value the
        // car left the garage on. A bare percentage doesn't say whether it
        // has been touched this session; the delta does.
        const double biasDelta = setup.rearBrakeBiasPercent - setup.garageRearBrakeBiasPercent;
        if (setup.garageRearBrakeBiasPercent >= 0.0 && (biasDelta > 0.05 || biasDelta < -0.05))
            sprintf_s(brake.value, "%.1f%% Rear (%+.1f)", setup.rearBrakeBiasPercent, biasDelta);
        else
            sprintf_s(brake.value, "%.1f%% Rear", setup.rearBrakeBiasPercent);
        brake.alert = false;
    }

    // ---- Fuel group geometry: label column + value column, measured at
    // the Fuel box's OWN scale (each of the three boxes can now be resized
    // independently). ----
    const float fuelTextPx = cfg.textHeightPx * fuelUiScale;
    const float fuelLineStep = fuelTextPx + cfg.lineGapPx * fuelUiScale;
    const float fuelColumnGapPx = cfg.columnGapPx * fuelUiScale;
    float fuelLabelW = 0.0f, fuelValueW = 0.0f, fuelGroupW = 0.0f, fuelGroupH = 0.0f;
    if (fuel.valid)
    {
        for (int i = 0; i < fuelRowCount; ++i)
        {
            fuelLabelW = (std::max)(fuelLabelW, renderer.MeasureText(fuelRows[i].label, fuelTextPx));
            fuelValueW = (std::max)(fuelValueW, renderer.MeasureText(fuelRows[i].value, fuelTextPx));
        }
        fuelGroupW = fuelLabelW + fuelColumnGapPx + fuelValueW;
        fuelGroupH = static_cast<float>(fuelRowCount) * fuelLineStep - cfg.lineGapPx * fuelUiScale;
    }

    // ---- Tyre group geometry: two columns (front FL/FR share widths with
    // rear RL/RR per side) so the 2x2 grid stays aligned, plus the summary
    // row -- measured at the Tyres box's own scale. ----
    const float tyresTextPx = cfg.textHeightPx * tyresUiScale;
    const float tyresLineStep = tyresTextPx + cfg.lineGapPx * tyresUiScale;
    const float tyresColumnGapPx = cfg.columnGapPx * tyresUiScale;
    float cellW[4] = {};
    float colLeftW = 0.0f, colRightW = 0.0f, tyreGroupW = 0.0f, tyreGroupH = 0.0f;
    if (showTyres)
    {
        // Fixed-width columns ("99.9") so the grid doesn't shift as the
        // numbers change.
        (void)cellW;
        colLeftW = colRightW = renderer.MeasureText("99.9", tyresTextPx);
        // Each column wide enough for its small box plot underneath.
        if (showWearPlots)
            colLeftW = colRightW = (std::max)(colLeftW, cfg.wearPlotWidthPx * tyresUiScale);
        const float gridW = colLeftW + tyresColumnGapPx * 2.0f + colRightW;

        auto rowW = [&](const Row& r)
        {
            return renderer.MeasureText(r.label, tyresTextPx) + tyresColumnGapPx
                 + renderer.MeasureText(r.value, tyresTextPx);
        };
        tyreGroupW = (std::max)(gridW, renderer.MeasureText(kTyreHeader, tyresTextPx));
        tyreGroupW = (std::max)(tyreGroupW, rowW(tyreLifeRow));
        if (showRaceEndProjection)
            tyreGroupW = (std::max)(tyreGroupW, rowW(tyreEndRow));
        // header + 2 grid rows + life row (+ race-end row, race only)
        const int tyreLineCount = showRaceEndProjection ? 5 : 4;
        tyreGroupH = static_cast<float>(tyreLineCount) * tyresLineStep - cfg.lineGapPx * tyresUiScale;
        if (showWearPlots)
        {
            // Two small-plot rows under the grid rows, then the worst-tyre
            // block: title line, big plot, value line.
            const float gap = cfg.lineGapPx * tyresUiScale;
            tyreGroupH += 2.0f * (cfg.wearPlotHeightPx * tyresUiScale + gap);
            tyreGroupH += tyresLineStep + cfg.wearBigPlotHeightPx * tyresUiScale + gap + tyresLineStep;
        }
    }

    // ---- Setup group geometry: label column + value column, same as Fuel,
    // measured at the Brake Bias box's own scale. ----
    const float brakeTextPx = cfg.textHeightPx * brakeBiasUiScale;
    const float brakeLineStep = brakeTextPx + cfg.lineGapPx * brakeBiasUiScale;
    const float brakeColumnGapPx = cfg.columnGapPx * brakeBiasUiScale;
    float setupLabelW = 0.0f, setupValueW = 0.0f, setupGroupW = 0.0f, setupGroupH = 0.0f;
    if (setup.valid)
    {
        for (int i = 0; i < setupRowCount; ++i)
        {
            setupLabelW = (std::max)(setupLabelW, renderer.MeasureText(setupRows[i].label, brakeTextPx));
            setupValueW = (std::max)(setupValueW, renderer.MeasureText(setupRows[i].value, brakeTextPx));
        }
        setupGroupW = setupLabelW + brakeColumnGapPx + setupValueW;
        setupGroupH = static_cast<float>(setupRowCount) * brakeLineStep - cfg.lineGapPx * brakeBiasUiScale;
    }

    // ---- Fuel, Tyres and Brake Bias are three INDEPENDENT boxes -- each
    // its own background, its own Layout::Register drag/resize handle, its
    // own scale. -1 on a box's own position fraction means "not dragged
    // yet -- sit directly below the box above me", the same sentinel this
    // codebase already uses elsewhere (the shortcut list, the DRS/PB rows)
    // so the default look stays the original single stacked panel until a
    // box is actually moved.
    bool anyDrawn = false;
    float prevBottomY = -1.0f; // bottom edge of whichever box was drawn most recently

    auto drawLabelValueRow = [&](float leftX, float rightX, float uiScaleForBox, float y, const Row& row)
    {
        const float rowTextPx = cfg.textHeightPx * uiScaleForBox;
        renderer.AddText(leftX, y, row.label, rowTextPx, cfg.colorR, cfg.colorG, cfg.colorB, 1.0f, Renderer::HAlign::Left);
        float r = row.alert ? cfg.alertR : cfg.colorR;
        float g = row.alert ? cfg.alertG : cfg.colorG;
        float b = row.alert ? cfg.alertB : cfg.colorB;
        // The fuel-versus-race verdict, where one applies, outranks the
        // plain/alert pair above.
        if (row.status == RowStatus::Good) { r = cfg.fuelGoodR; g = cfg.fuelGoodG; b = cfg.fuelGoodB; }
        else if (row.status == RowStatus::Marginal) { r = cfg.fuelMarginalR; g = cfg.fuelMarginalG; b = cfg.fuelMarginalB; }
        else if (row.status == RowStatus::Short) { r = cfg.fuelShortR; g = cfg.fuelShortG; b = cfg.fuelShortB; }
        renderer.AddText(rightX, y, row.value, rowTextPx, r, g, b, 1.0f, Renderer::HAlign::Right);
    };

    // Registers, backgrounds and returns the box's own (leftX, rightX,
    // centerY, uiScale) -- shared plumbing for all three boxes below, since
    // they only differ in their content and geometry.
    auto beginBox = [&](const char* layoutName, const char* iniSection,
                        float* posXFraction, float* posYFraction, float* scale,
                        float groupW, float groupH) -> std::tuple<float, float, float, float>
    {
        const float boxUiScale = static_cast<float>(screenHeight) / 2160.0f * (*scale);
        const float boxBgPaddingPx = cfg.bgPaddingPx * boxUiScale;
        const float boxBgCornerRadiusPx = cfg.bgCornerRadiusPx * boxUiScale;

        // Sentinel: not yet dragged, so sit flush left directly under the
        // previous box (or at the configured Fuel origin if this is the
        // first box drawn this frame).
        const bool useSentinel = (*posXFraction < 0.0f || *posYFraction < 0.0f);
        const float boxLeftX = useSentinel ? (screenWidth * cfg.fuelPosXFraction) : (screenWidth * (*posXFraction));
        const float boxTopY = useSentinel
                             ? ((prevBottomY >= 0.0f) ? (prevBottomY + groupGapPx) : (screenHeight * cfg.fuelPosYFraction))
                             : (screenHeight * (*posYFraction));

        const float halfW = groupW * 0.5f + boxBgPaddingPx;
        const float halfH = groupH * 0.5f + boxBgPaddingPx;
        const float centerX = boxLeftX + halfW;
        const float centerY = boxTopY + halfH;

        Layout::Register(layoutName, iniSection, "PosXFraction", "PosYFraction",
                         posXFraction, posYFraction,
                         centerX, centerY, halfW, halfH,
                         scale, "Scale");

        if (cfg.bgEnabled)
        {
            renderer.AddFilledRoundedRect(centerX, centerY, halfW, halfH, boxBgCornerRadiusPx, 0.0f,
                                          cfg.bgR, cfg.bgG, cfg.bgB, cfg.bgA);
        }

        prevBottomY = centerY + halfH;
        anyDrawn = true;
        return { centerX - groupW * 0.5f, centerX + groupW * 0.5f, centerY - groupH * 0.5f, boxUiScale };
    };

    if (fuel.valid && cfg.showFuel && Shortcuts::Visible(Shortcuts::FuelPanel))
    {
        auto [leftX, rightX, topY, boxUiScale] =
            beginBox("Fuel", "FuelTyrePanel_Fuel",
                     &g_fuelTyrePanelConfig.fuelPosXFraction, &g_fuelTyrePanelConfig.fuelPosYFraction,
                     &g_fuelTyrePanelConfig.fuelScale, fuelGroupW, fuelGroupH);
        s_lastFuelBottomY = prevBottomY;

        const float boxLineStep = (cfg.textHeightPx + cfg.lineGapPx) * boxUiScale;
        float ly = topY;
        for (int i = 0; i < fuelRowCount; ++i)
        {
            drawLabelValueRow(leftX, rightX, boxUiScale, ly, fuelRows[i]);
            ly += boxLineStep;
        }
    }
    else
    {
        // Not drawn this frame (hidden or no data) -- forget its last
        // position so BottomEdgeY() below doesn't keep leaving room for a
        // box that is no longer there.
        s_lastFuelBottomY = -1.0f;
    }

    if (showTyres && cfg.showTyres && Shortcuts::Visible(Shortcuts::TyresPanel))
    {
        auto [leftX, rightX, topY, boxUiScale] =
            beginBox("Tyres", "FuelTyrePanel_Tyres",
                     &g_fuelTyrePanelConfig.tyresPosXFraction, &g_fuelTyrePanelConfig.tyresPosYFraction,
                     &g_fuelTyrePanelConfig.tyresScale, tyreGroupW, tyreGroupH);
        s_lastTyresBottomY = prevBottomY;

        const float boxTextPx = cfg.textHeightPx * boxUiScale;
        const float boxLineStep = (cfg.textHeightPx + cfg.lineGapPx) * boxUiScale;
        const float boxColumnGapPx = cfg.columnGapPx * boxUiScale;

        // Physical 2x2 layout: front row (FL, FR) above rear row (RL, RR),
        // bare %/lap numbers under one shared header. Red for the
        // sudden-increase alert; otherwise the projected-end-of-race tier
        // (orange/red) so a single bad corner still stands out.
        auto drawWheelCell = [&](float centerX, float y, int w)
        {
            float r = cfg.colorR, g = cfg.colorG, b = cfg.colorB;
            if (wheelAlert[w]) { r = cfg.alertR; g = cfg.alertG; b = cfg.alertB; }
            else if (wheelEndOfRaceTier[w] == 2) { r = cfg.tyreCriticalVsRaceR; g = cfg.tyreCriticalVsRaceG; b = cfg.tyreCriticalVsRaceB; }
            else if (wheelEndOfRaceTier[w] == 1) { r = cfg.tyreLowVsRaceR; g = cfg.tyreLowVsRaceG; b = cfg.tyreLowVsRaceB; }
            renderer.AddText(centerX, y, cellDeg[w], boxTextPx, r, g, b, 1.0f, Renderer::HAlign::Center);
        };

        const float groupCenterX = (leftX + rightX) * 0.5f;
        const float leftColCX = groupCenterX - boxColumnGapPx - colLeftW * 0.5f;
        const float rightColCX = groupCenterX + boxColumnGapPx + colRightW * 0.5f;
        float ly = topY;

        // All four small plots on ONE scale, so a longer box or a point
        // further right really is more wear than the tyre next to it.
        const float smallPlotW = cfg.wearPlotWidthPx * boxUiScale;
        const float smallPlotH = cfg.wearPlotHeightPx * boxUiScale;
        const float plotGap = cfg.lineGapPx * boxUiScale;
        float smallLo = 0.0f, smallHi = 1.0f;
        if (showWearPlots)
            WearPlotRange(tyres.distribution, 4, smallLo, smallHi);
        auto drawSmallPlots = [&](int leftWheel, int rightWheel)
        {
            if (!showWearPlots) return;
            const float cy = ly + smallPlotH * 0.5f;
            DrawWearBoxPlot(renderer, tyres.distribution[leftWheel], leftColCX - smallPlotW * 0.5f, leftColCX + smallPlotW * 0.5f,
                            cy, smallPlotH, smallLo, smallHi, cfg);
            DrawWearBoxPlot(renderer, tyres.distribution[rightWheel], rightColCX - smallPlotW * 0.5f, rightColCX + smallPlotW * 0.5f,
                            cy, smallPlotH, smallLo, smallHi, cfg);
            ly += smallPlotH + plotGap;
        };

        renderer.AddText(groupCenterX, ly, kTyreHeader, boxTextPx, cfg.colorR, cfg.colorG, cfg.colorB, 0.7f, Renderer::HAlign::Center);
        ly += boxLineStep;

        drawWheelCell(leftColCX, ly, 0);   // FL
        drawWheelCell(rightColCX, ly, 1);  // FR
        ly += boxLineStep;
        drawSmallPlots(0, 1);

        drawWheelCell(leftColCX, ly, 2);   // RL
        drawWheelCell(rightColCX, ly, 3);  // RR
        ly += boxLineStep;
        drawSmallPlots(2, 3);

        // The worst tyre, bigger and on its OWN scale for detail, with the
        // numbers written underneath: range ends at the sides, the median
        // and the last lap (in red) in the middle.
        if (showWearPlots)
        {
            const TyreWearStore::WearDistribution& d = tyres.distribution[worstWearWheel];
            char title[48];
            sprintf_s(title, "Worst %s  (%d lap%s)", kWheelLabel[worstWearWheel], d.count, d.count == 1 ? "" : "s");
            renderer.AddText(groupCenterX, ly, title, boxTextPx, cfg.colorR, cfg.colorG, cfg.colorB, 0.7f, Renderer::HAlign::Center);
            ly += boxLineStep;

            // The dot is this lap's wear so far, moving right as the lap
            // goes on (held at its final value briefly after the line), not
            // the last completed lap. Coloured against the median pro-rated
            // to how far through the lap you are: green on target, orange a
            // little over, red well over.
            const TyreWearStore::WheelState& live = tyreWheels[worstWearWheel];
            const float nowWear = static_cast<float>(live.steppedDegradationPercent);
            float dotR = 0.2f, dotG = 0.85f, dotB = 0.3f;
            if (live.steppedLapFraction > 0.1 && d.median > 0.0001f)
            {
                const double ratio = nowWear / (d.median * live.steppedLapFraction);
                if (ratio > 1.15)      { dotR = 0.95f; dotG = 0.2f;  dotB = 0.2f; }
                else if (ratio > 1.03) { dotR = 1.0f;  dotG = 0.6f;  dotB = 0.1f; }
            }

            TyreWearStore::WearDistribution plotD = d;
            plotD.lastLap = -1.0f;              // the live dot replaces the last-lap one
            const float bigH = cfg.wearBigPlotHeightPx * boxUiScale;
            float bigLo = 0.0f, bigHi = 1.0f;
            WearPlotRange(&plotD, 1, bigLo, bigHi);
            bigLo = 0.0f;                       // the dot starts each lap at zero
            if (nowWear > bigHi) bigHi = nowWear * 1.05f;
            const float bigCY = ly + bigH * 0.5f;
            DrawWearBoxPlot(renderer, plotD, leftX, rightX, bigCY, bigH, bigLo, bigHi, cfg);
            {
                const float thin = (std::max)(1.0f, bigH * 0.08f);
                const float dx = leftX + (nowWear - bigLo) / (bigHi - bigLo) * (rightX - leftX);
                renderer.AddFilledCircle(dx, bigCY, bigH * 0.24f + thin, 0.0f, 0.0f, 0.0f, 0.9f, 16);
                renderer.AddFilledCircle(dx, bigCY, bigH * 0.24f, dotR, dotG, dotB, 1.0f, 16);
            }
            ly += bigH + plotGap;

            const float smallTextPx = boxTextPx * 0.8f;
            char buf[32];
            sprintf_s(buf, "%.2f", bigLo);
            renderer.AddText(leftX, ly, buf, smallTextPx, cfg.colorR, cfg.colorG, cfg.colorB, 0.6f, Renderer::HAlign::Left);
            sprintf_s(buf, "%.2f", bigHi);
            renderer.AddText(rightX, ly, buf, smallTextPx, cfg.colorR, cfg.colorG, cfg.colorB, 0.6f, Renderer::HAlign::Right);
            char med[32], last[32];
            sprintf_s(med, "med %.2f   ", d.median);
            sprintf_s(last, "now %.2f", nowWear);
            const float medW = renderer.MeasureText(med, smallTextPx);
            const float lastW = renderer.MeasureText(last, smallTextPx);
            const float startX = groupCenterX - (medW + lastW) * 0.5f;
            renderer.AddText(startX, ly, med, smallTextPx, cfg.colorR, cfg.colorG, cfg.colorB, 1.0f, Renderer::HAlign::Left);
            renderer.AddText(startX + medW, ly, last, smallTextPx, dotR, dotG, dotB, 1.0f, Renderer::HAlign::Left);
            ly += boxLineStep;
        }

        drawLabelValueRow(leftX, rightX, boxUiScale, ly, tyreLifeRow);
        ly += boxLineStep;

        if (showRaceEndProjection)
        {
            // No background highlight here (removed -- the critical tier's
            // red highlight sat directly behind this row's own red alert
            // text, making it unreadable). The value text's own colour
            // already carries the tier via tyreEndRow.alert below.
            drawLabelValueRow(leftX, rightX, boxUiScale, ly, tyreEndRow);
        }
    }
    else
    {
        s_lastTyresBottomY = -1.0f;
    }

    if (setup.valid && cfg.showBrakeBias && Shortcuts::Visible(Shortcuts::BrakeBiasPanel))
    {
        auto [leftX, rightX, topY, boxUiScale] =
            beginBox("Brake bias", "FuelTyrePanel_BrakeBias",
                     &g_fuelTyrePanelConfig.brakeBiasPosXFraction, &g_fuelTyrePanelConfig.brakeBiasPosYFraction,
                     &g_fuelTyrePanelConfig.brakeBiasScale, setupGroupW, setupGroupH);
        s_lastBrakeBiasBottomY = prevBottomY;

        const float boxLineStep = (cfg.textHeightPx + cfg.lineGapPx) * boxUiScale;
        float ly = topY;
        for (int i = 0; i < setupRowCount; ++i)
        {
            drawLabelValueRow(leftX, rightX, boxUiScale, ly, setupRows[i]);
            ly += boxLineStep;
        }
    }
    else
    {
        s_lastBrakeBiasBottomY = -1.0f;
    }

    if (anyDrawn)
        s_lastBottomEdgeY = (std::max)({ s_lastFuelBottomY, s_lastTyresBottomY, s_lastBrakeBiasBottomY });

    return anyDrawn;
}
