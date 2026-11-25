#include "Conditions.h"
#include "InternalsPlugin.hpp"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "Weather.h"
#include "RenderThrottle.h"
#include "Log.h"
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

    std::lock_guard<std::mutex> lk(s_mx);
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
    // Compared with both the node in force and the next one (the game blends
    // between them), with hysteresis so it does not flicker; when hidden the
    // title stays with a status line (review F141).
    static bool s_gameInForce = true;
    if (ng > 0 && valid)
    {
        float d = fabsf(gp[0].tempC - now.air);
        if (ng > 1) { const float d1 = fabsf(gp[1].tempC - now.air); if (d1 < d) d = d1; }
        const float lo = ng > 1 ? fminf(gp[0].tempC, gp[1].tempC) : gp[0].tempC;
        const float hi = ng > 1 ? fmaxf(gp[0].tempC, gp[1].tempC) : gp[0].tempC;
        if (now.air >= lo && now.air <= hi) d = 0.0f;
        if (s_gameInForce && d > 6.0f) s_gameInForce = false;
        else if (!s_gameInForce && d < 4.0f) s_gameInForce = true;
    }
    const bool showGame = true;
    if (ng > 0 && !s_gameInForce) { ng = 0; gStatus = "not in force (real weather?)"; }
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
    // rF2 reports wetness in coarse steps, so the average alone can jump
    // past the amber band: also amber when the wettest part of the line is
    // past the crossover (review F139).
    else if (wet >= cross - cfg.nearMargin || now.wetMax >= cross)
    { call = (dWet > 0.002f || dRain > 0.005f) ? "SLICKS - wets soon" : "SLICKS - near crossover"; r = 1.0f; g = 0.75f; b = 0.15f; }
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

    // Ago may return as little as 5 minutes: scale to a true 10-minute rate (review F145).
    if (haveT && now.et - t10.et > 1.0)
        sprintf_s(line, "Track %.0fC (%+.1f/10m)  Air %.0fC", now.track,
                  (now.track - t10.track) * 600.0 / (now.et - t10.et), now.air);
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
