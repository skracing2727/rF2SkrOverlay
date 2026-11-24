#include "EngineTemps.h"
#include "InternalsPlugin.hpp"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "RenderThrottle.h"
#include <atomic>
#include <cmath>
#include <cstdio>

namespace
{
    std::atomic<float> s_water{ 0.0f };
    std::atomic<float> s_oil{ 0.0f };
    std::atomic<bool> s_overheating{ false };
    std::atomic<bool> s_valid{ false };
    // Learned normal (slow average while driving at speed), and how many
    // seconds of such driving it is built on.
    std::atomic<float> s_waterNorm{ 0.0f };
    std::atomic<float> s_oilNorm{ 0.0f };
    std::atomic<float> s_learnSeconds{ 0.0f };

    const float kLearnMinSeconds = 120.0f; // before this the normal is not trusted
    const float kTau = 300.0f;             // averaging time constant, s
}

void EngineTemps::Reset()
{
    s_valid = false;
    s_learnSeconds = 0.0f;
}

void EngineTemps::OnTelemetry(const TelemInfoV01& info)
{
    const float w = static_cast<float>(info.mEngineWaterTemp);
    const float o = static_cast<float>(info.mEngineOilTemp);
    s_water = w;
    s_oil = o;
    s_overheating = info.mOverheating;
    s_valid = w > 1.0f || o > 1.0f;

    const double v = sqrt(info.mLocalVel.x * info.mLocalVel.x + info.mLocalVel.z * info.mLocalVel.z);
    const float dt = static_cast<float>(info.mDeltaTime);
    if (v > 25.0 && dt > 0.0f && dt < 0.5f && !info.mOverheating)
    {
        const float n = s_learnSeconds.load();
        if (n <= 0.0f) { s_waterNorm = w; s_oilNorm = o; }
        else
        {
            // Faster at first so the normal settles in the first minutes.
            const float tau = n < kLearnMinSeconds ? (n + 10.0f) : kTau;
            const float k = dt / tau;
            s_waterNorm = s_waterNorm.load() + (w - s_waterNorm.load()) * k;
            s_oilNorm = s_oilNorm.load() + (o - s_oilNorm.load()) * k;
        }
        s_learnSeconds = n + dt;
    }
}

bool EngineTemps::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    EngineTempsConfig& cfg = g_engineTempsConfig;
    if (!cfg.enabled) return false;
    const bool placeholder = Layout::Preview();
    // Drawn even with no data ("--"), so the box can always be found and a
    // car that reports no temperatures shows as such instead of vanishing.

    // Low tier (~5Hz): whole-degree temperatures that move over seconds,
    // so rebuild at most that often and replay the last build on the
    // frames in between. Bypassed in layout mode so dragging/resizing
    // stays smooth.
    static RenderThrottle s_throttle(0.2);
    if (!Layout::Active() && s_throttle.TryReplay(renderer))
        return true;
    RenderThrottle::Capture throttleCapture(s_throttle, renderer);

    const float s = static_cast<float>(screenHeight) / 2160.0f * (cfg.scale > 0.05f ? cfg.scale : 0.05f);
    const float textPx = 36.0f * s;
    const float pad = 12.0f * s;
    const float halfW = 190.0f * s;
    const float halfH = textPx + pad;
    const float cx = screenWidth * cfg.centerXFraction;
    const float cy = screenHeight * cfg.centerYFraction;
    Layout::Register("Engine temps", "EngineTemps", "CenterXFraction", "CenterYFraction",
                     &cfg.centerXFraction, &cfg.centerYFraction, cx, cy, halfW, halfH, &cfg.scale, "Scale");

    const bool prevBg = renderer.TextBackgroundEnabled();
    renderer.SetTextBackground(false);
    // No temperatures from this car (some mods report 0 to plugins): the
    // game's overheating flag is all there is, so show that as a status.
    if (!s_valid)
    {
        const bool hotNow = s_overheating.load();
        if (hotNow)
            renderer.AddFilledRoundedRect(cx, cy, halfW, halfH, 8.0f * s, 0.0f, 0.85f, 0.12f, 0.1f, 0.9f);
        else
            renderer.AddFilledRoundedRect(cx, cy, halfW, halfH, 8.0f * s, 0.0f, 0.08f, 0.08f, 0.08f, 0.6f);
        renderer.AddText(cx, cy - textPx * 0.5f, hotNow ? "OVERHEATING" : "Engine temp OK", textPx,
                         hotNow ? 1.0f : 0.4f, hotNow ? 1.0f : 0.9f, hotNow ? 1.0f : 0.45f, 1.0f, Renderer::HAlign::Center);
        renderer.SetTextBackground(prevBg);
        return true;
    }
    renderer.AddFilledRoundedRect(cx, cy, halfW, halfH, 8.0f * s, 0.0f, 0.08f, 0.08f, 0.08f, 0.6f);

    const bool learned = s_learnSeconds.load() >= kLearnMinSeconds;
    const bool hot = s_overheating.load();
    const float vals[2] = { s_water.load(), s_oil.load() };
    const float norms[2] = { s_waterNorm.load(), s_oilNorm.load() };
    const float margins[2] = { cfg.waterAmberMargin, cfg.oilAmberMargin };
    const char* const names[2] = { "Water", "Oil" };
    for (int i = 0; i < 2; ++i)
    {
        float r = 0.95f, g = 0.95f, b = 0.95f;
        if (hot) { r = 1.0f; g = 0.3f; b = 0.25f; }
        else if (learned && vals[i] > norms[i] + margins[i]) { r = 1.0f; g = 0.75f; b = 0.15f; }
        char line[64];
        if (!s_valid)
        {
            sprintf_s(line, "%s --", names[i]);
            r = g = b = 0.7f;
        }
        else if (learned)
            sprintf_s(line, "%s %.0fC  (%+.0f)", names[i], vals[i], vals[i] - norms[i]);
        else
            sprintf_s(line, "%s %.0fC", names[i], vals[i]);
        renderer.AddText(cx, cy - halfH + pad + i * textPx, line, textPx, r, g, b, 1.0f, Renderer::HAlign::Center);
    }

    renderer.SetTextBackground(prevBg);
    return true;
}
