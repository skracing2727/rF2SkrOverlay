#include "Conditions.h"
#include "InternalsPlugin.hpp"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "Weather.h"
#include "RenderThrottle.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <cmath>
#include <cstdio>

namespace
{
    struct Sample { double et; float track, air, rain, wetMin, wetAvg, wetMax; };

    std::mutex s_mx;
    bool s_valid = false;
    Sample s_now = {};
    std::deque<Sample> s_hist;   // one sample every kStep seconds, kKeep seconds kept

    // Wet sections along the lap (see WetBins).
    const float kBinM = 20.0f;
    std::atomic<int> s_wetSamples{ 0 }, s_drySamples{ 0 };
    std::vector<signed char> s_bins;
    double s_trackLen = 0.0;
    double s_prevDist = -1.0;
    bool s_anyWet = false;

    const double kStep = 5.0;
    const double kKeep = 660.0;

    // Value of the field kAgo seconds before the newest sample (or the oldest
    // there is if at least half that time is covered), else false.
    bool Ago(double ago, Sample& out)
    {
        if (s_hist.empty()) return false;
        const double want = s_now.et - ago;
        for (auto it = s_hist.rbegin(); it != s_hist.rend(); ++it)
            if (it->et <= want) { out = *it; return true; }
        if (s_now.et - s_hist.front().et >= ago * 0.5) { out = s_hist.front(); return true; }
        return false;
    }
}

void Conditions::Reset()
{
    std::lock_guard<std::mutex> lk(s_mx);
    s_valid = false;
    s_hist.clear();
    s_bins.clear();
    s_trackLen = 0.0;
    s_prevDist = -1.0;
    s_anyWet = false;
}

void Conditions::OnTelemetry(const TelemInfoV01& info)
{
    int wet = 0, dry = 0;
    for (int i = 0; i < 4; ++i)
    {
        const unsigned char t = info.mWheel[i].mSurfaceType;
        if (t == 1) ++wet;
        else if (t == 0) ++dry;
    }
    if (wet) s_wetSamples.fetch_add(wet, std::memory_order_relaxed);
    if (dry) s_drySamples.fetch_add(dry, std::memory_order_relaxed);
}

bool Conditions::WetBins(std::vector<signed char>& bins, float& binM)
{
    std::lock_guard<std::mutex> lk(s_mx);
    if (!s_anyWet || s_bins.empty()) return false;
    bins = s_bins;
    binM = kBinM;
    return true;
}

void Conditions::OnScoring(const ScoringInfoV01& info)
{
    Sample s;
    s.et = info.mCurrentET;
    s.track = static_cast<float>(info.mTrackTemp);
    s.air = static_cast<float>(info.mAmbientTemp);
    s.rain = static_cast<float>(info.mRaining);
    s.wetMin = static_cast<float>(info.mMinPathWetness);
    s.wetMax = static_cast<float>(info.mMaxPathWetness);
    s.wetAvg = static_cast<float>(info.mAvgPathWetness);

    // Player's lap distance, for the wet sections.
    double dist = -1.0;
    bool inPits = true;
    for (long i = 0; i < info.mNumVehicles; ++i)
        if (info.mVehicle[i].mIsPlayer) { dist = info.mVehicle[i].mLapDist; inPits = info.mVehicle[i].mInPits; break; }
    const int wetN = s_wetSamples.exchange(0, std::memory_order_relaxed);
    const int dryN = s_drySamples.exchange(0, std::memory_order_relaxed);

    std::lock_guard<std::mutex> lk(s_mx);
    if (info.mLapDist > 100.0 && fabs(info.mLapDist - s_trackLen) > 1.0)
    {
        s_trackLen = info.mLapDist;
        s_bins.assign(static_cast<size_t>(s_trackLen / kBinM) + 1, -1);
        s_prevDist = -1.0;
        s_anyWet = false;
    }
    if (!s_bins.empty() && dist >= 0.0 && !inPits && wetN + dryN > 0)
    {
        if (s_prevDist >= 0.0)
        {
            double from = s_prevDist, to = dist;
            if (to < from - s_trackLen * 0.5) to += s_trackLen;   // crossed the line
            if (to > from && to - from < 300.0)
            {
                const signed char v = wetN * 2 >= wetN + dryN ? 1 : 0;   // half or more of the wheel samples wet
                if (v) s_anyWet = true;
                const size_t n = s_bins.size();
                for (double d = from; d <= to; d += kBinM * 0.5)
                {
                    size_t b = static_cast<size_t>(fmod(d, s_trackLen) / kBinM);
                    if (b >= n) b = n - 1;
                    s_bins[b] = v;
                }
            }
        }
        s_prevDist = dist;
    }
    else if (inPits) s_prevDist = -1.0;

    // Session restart / time going backwards: the history is no longer valid.
    if (!s_hist.empty() && s.et < s_hist.back().et - 1.0) s_hist.clear();
    s_now = s;
    s_valid = true;
    if (s_hist.empty() || s.et - s_hist.back().et >= kStep) s_hist.push_back(s);
    while (!s_hist.empty() && s.et - s_hist.front().et > kKeep) s_hist.pop_front();
}

bool Conditions::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    ConditionsConfig& cfg = g_conditionsConfig;
    if (!cfg.enabled) return false;

    // Low tier (~5Hz): everything here comes from scoring (5Hz) or from a
    // forecast fetched every 30 s or more, so rebuild at most that often
    // and replay the last build on the frames in between. Bypassed in
    // layout mode so dragging/resizing stays smooth.
    static RenderThrottle s_throttle(0.2);
    if (!Layout::Active() && s_throttle.TryReplay(renderer))
        return true;
    RenderThrottle::Capture throttleCapture(s_throttle, renderer);

    const bool placeholder = Layout::Preview();

    Sample now, w1 = {}, t10 = {};
    bool haveW = false, haveT = false, valid;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        valid = s_valid;
        now = s_now;
        haveW = Ago(60.0, w1);     // wetness / rain trend over the last minute
        haveT = Ago(600.0, t10);   // track temp trend over ten minutes
    }
    if (!valid && !placeholder) return false;

    const float s = static_cast<float>(screenHeight) / 2160.0f * (cfg.scale > 0.05f ? cfg.scale : 0.05f);
    const float textPx = 32.0f * s;
    const float bigPx = 44.0f * s;
    const float pad = 10.0f * s;
    const float halfW = 210.0f * s;
    // Forecasts: game (rF2's own) and real (Open-Meteo), up to 3 lines each.
    Weather::GamePoint gp[3];
    Weather::RealPoint rp[3];
    const char* gStatus = "";
    const char* rStatus = "";
    int ng = Weather::Game(gp, 3, gStatus);
    // With real-world weather rF2's web API still returns the session's
    // pre-set forecast, which the game is not using (e.g. 22C forecast at an
    // 8C track). A first point far from the live air temperature means that:
    // hide the GAME lines rather than show a forecast that is not in force.
    const bool showGame = !(ng > 0 && valid && fabsf(gp[0].tempC - now.air) > 5.0f);
    if (!showGame) ng = 0;
    const int nr = Weather::Real(rp, 3, rStatus);
    const bool showReal = cfg.realWeather;
    const int fcLines = (showGame ? 1 + (ng ? ng : 1) : 0) + (showReal ? 1 + (nr ? nr : 1) : 0);
    const float smallPx = textPx * 0.85f;
    const float halfH = (bigPx + 3.0f * textPx + 5.0f * pad + fcLines * smallPx + 2.0f * pad) * 0.5f;
    const float cx = screenWidth * cfg.centerXFraction;
    const float cy = screenHeight * cfg.centerYFraction;
    Layout::Register("Track conditions", "Conditions", "CenterXFraction", "CenterYFraction",
                     &cfg.centerXFraction, &cfg.centerYFraction, cx, cy, halfW, halfH, &cfg.scale, "Scale");

    const bool prevBg = renderer.TextBackgroundEnabled();
    renderer.SetTextBackground(false);
    renderer.AddFilledRoundedRect(cx, cy, halfW, halfH, 8.0f * s, 0.0f, 0.08f, 0.08f, 0.08f, 0.6f);

    // Tyre call from average racing-line wetness against the crossover.
    const float cross = cfg.wetsAtWetness;
    const float wet = now.wetAvg;
    const float dWet = haveW ? now.wetAvg - w1.wetAvg : 0.0f;
    const float dRain = haveW ? now.rain - w1.rain : 0.0f;
    const char* call; float r, g, b;
    if (wet >= cross) { call = "WETS"; r = 0.3f; g = 0.6f; b = 1.0f; }
    else if (wet >= cross - cfg.nearMargin) { call = dWet > 0.002f ? "SLICKS - wets soon" : "SLICKS - near crossover"; r = 1.0f; g = 0.75f; b = 0.15f; }
    else { call = "SLICKS"; r = 0.35f; g = 0.9f; b = 0.4f; }

    float y = cy - halfH + pad;
    renderer.AddText(cx, y, call, bigPx, r, g, b, 1.0f, Renderer::HAlign::Center);
    y += bigPx + pad;

    auto trend = [](float d, float eps, const char* up, const char* down) { return d > eps ? up : (d < -eps ? down : "steady"); };

    char line[96];
    sprintf_s(line, "Wet %.0f%%  (%.0f-%.0f)  %s", wet * 100.0f, now.wetMin * 100.0f, now.wetMax * 100.0f,
              haveW ? trend(dWet, 0.002f, "wetter", "drying") : "");
    renderer.AddText(cx, y, line, textPx, 0.95f, 0.95f, 0.95f, 1.0f, Renderer::HAlign::Center);
    y += textPx + pad;

    sprintf_s(line, "Rain %.0f%%  %s", now.rain * 100.0f, haveW ? trend(dRain, 0.005f, "heavier", "easing") : "");
    renderer.AddText(cx, y, line, textPx, now.rain > 0.0f ? 0.6f : 0.95f, now.rain > 0.0f ? 0.8f : 0.95f, 0.95f, 1.0f, Renderer::HAlign::Center);
    y += textPx + pad;

    if (haveT) sprintf_s(line, "Track %.0fC (%+.1f/10m)  Air %.0fC", now.track, now.track - t10.track, now.air);
    else sprintf_s(line, "Track %.0fC  Air %.0fC", now.track, now.air);
    renderer.AddText(cx, y, line, textPx, 0.95f, 0.95f, 0.95f, 1.0f, Renderer::HAlign::Center);
    y += textPx + 2.0f * pad;

    auto rainCol = [](float chance, float& r, float& g, float& b)
    {
        if (chance >= 0.5f) { r = 0.35f; g = 0.6f; b = 1.0f; }
        else if (chance >= 0.2f) { r = 0.6f; g = 0.8f; b = 1.0f; }
        else { r = g = b = 0.85f; }
    };
    if (showGame) renderer.AddText(cx, y, "GAME forecast", smallPx, 1.0f, 0.85f, 0.4f, 1.0f, Renderer::HAlign::Center);
    if (showGame) y += smallPx;
    if (showGame && !ng) { renderer.AddText(cx, y, gStatus, smallPx, 0.6f, 0.6f, 0.6f, 1.0f, Renderer::HAlign::Center); y += smallPx; }
    for (int i = 0; i < ng; ++i)
    {
        char when[16];
        if (gp[i].minutesAhead <= 0.0f) strcpy_s(when, "now");
        else if (gp[i].minutesAhead < 90.0f) sprintf_s(when, "+%.0fm", gp[i].minutesAhead);
        else sprintf_s(when, "+%.1fh", gp[i].minutesAhead / 60.0f);
        sprintf_s(line, "%s  rain %.0f%%  %.0fC  %s", when, gp[i].rainChance * 100.0f, gp[i].tempC, Weather::SkyName(gp[i].sky));
        rainCol(gp[i].rainChance, r, g, b);
        renderer.AddText(cx, y, line, smallPx, r, g, b, 1.0f, Renderer::HAlign::Center);
        y += smallPx;
    }
    if (showReal)
    {
        renderer.AddText(cx, y, "REAL forecast", smallPx, 1.0f, 0.85f, 0.4f, 1.0f, Renderer::HAlign::Center);
        y += smallPx;
        if (!nr) { renderer.AddText(cx, y, rStatus, smallPx, 0.6f, 0.6f, 0.6f, 1.0f, Renderer::HAlign::Center); y += smallPx; }
        for (int i = 0; i < nr; ++i)
        {
            char when[16];
            if (rp[i].hoursAhead <= 0) strcpy_s(when, "now");
            else sprintf_s(when, "+%dh", rp[i].hoursAhead);
            sprintf_s(line, "%s  rain %.0f%%  %.1fmm  %.0fC", when, rp[i].rainChance * 100.0f, rp[i].rainMm, rp[i].tempC);
            rainCol(rp[i].rainChance, r, g, b);
            renderer.AddText(cx, y, line, smallPx, r, g, b, 1.0f, Renderer::HAlign::Center);
            y += smallPx;
        }
    }

    renderer.SetTextBackground(prevBg);
    return true;
}
