#include "FfbLevel.h"
#include "FfbGraph.h"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>

extern FfbSpectrumStore g_ffbSpectrum;

namespace
{
    const float kClip = 0.995f;

    std::atomic<float> s_now{ 0.0f };
    std::atomic<float> s_lapMax{ 0.0f };
    std::atomic<float> s_prevLapMax{ -1.0f };   // -1 = no lap yet
    std::atomic<float> s_prevLapClip{ 0.0f };   // fraction of samples clipped
    std::atomic<bool> s_valid{ false };
    long s_lap = -1;
    long long s_samples = 0, s_clipped = 0;
}

void FfbLevel::OnForce(double value)
{
    const float a = static_cast<float>(fabs(value));
    s_now.store(a, std::memory_order_relaxed);
    s_valid.store(true, std::memory_order_relaxed);

    const long lap = g_ffbSpectrum.Lap();
    if (lap != s_lap)
    {
        if (s_lap >= 0 && lap == s_lap + 1 && s_samples > 0)
        {
            s_prevLapMax.store(s_lapMax.load());
            s_prevLapClip.store(static_cast<float>(s_clipped) / static_cast<float>(s_samples));
        }
        s_lap = lap;
        s_lapMax.store(0.0f);
        s_samples = 0;
        s_clipped = 0;
    }
    if (a > s_lapMax.load(std::memory_order_relaxed)) s_lapMax.store(a, std::memory_order_relaxed);
    ++s_samples;
    if (a >= kClip) ++s_clipped;
}

bool FfbLevel::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    FfbLevelConfig& cfg = g_ffbLevelConfig;
    if (!cfg.enabled) return false;
    const bool placeholder = Layout::Preview();
    if (!s_valid.load() && !placeholder) return false;

    // Peak-hold the live value ~0.3 s, then let it fall, so the number and
    // bar are readable instead of flickering at 400 Hz.
    static LARGE_INTEGER prev = {};
    static float held = 0.0f;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = prev.QuadPart ? static_cast<float>(now.QuadPart - prev.QuadPart) / freq.QuadPart : 0.0f;
    prev = now;
    if (dt < 0.0f || dt > 1.0f) dt = 1.0f;
    const float cur = s_now.load();
    held = cur > held ? cur : held - (held - cur) * (1.0f - expf(-dt / 0.3f));

    const float s = static_cast<float>(screenHeight) / 2160.0f * (cfg.scale > 0.05f ? cfg.scale : 0.05f);
    const float textPx = 32.0f * s;
    const float pad = 10.0f * s;
    const float halfW = 170.0f * s;
    const float barH = 14.0f * s;
    const float halfH = (2.0f * textPx + barH + 3.0f * pad) * 0.5f;
    const float cx = screenWidth * cfg.centerXFraction;
    const float cy = screenHeight * cfg.centerYFraction;
    Layout::Register("FFB strength", "FfbLevel", "CenterXFraction", "CenterYFraction",
                     &cfg.centerXFraction, &cfg.centerYFraction, cx, cy, halfW, halfH, &cfg.scale, "Scale");

    const bool prevBg = renderer.TextBackgroundEnabled();
    renderer.SetTextBackground(false);
    renderer.AddFilledRoundedRect(cx, cy, halfW, halfH, 8.0f * s, 0.0f, 0.08f, 0.08f, 0.08f, 0.6f);

    auto levelColour = [](float v, float& r, float& g, float& b)
    {
        if (v >= kClip) { r = 1.0f; g = 0.3f; b = 0.25f; }
        else if (v >= 0.9f) { r = 1.0f; g = 0.75f; b = 0.15f; }
        else { r = 0.35f; g = 0.85f; b = 0.4f; }
    };

    float y = cy - halfH + pad;
    char line[64];
    const float lapMax = s_lapMax.load();
    sprintf_s(line, "FFB %3.0f%%   lap max %3.0f%%", held * 100.0f, lapMax * 100.0f);
    renderer.AddText(cx, y, line, textPx, 0.95f, 0.95f, 0.95f, 1.0f, Renderer::HAlign::Center);
    y += textPx + pad;

    // Bar: live level, with a tick at this lap's max.
    const float left = cx - halfW + pad, width = 2.0f * (halfW - pad);
    renderer.AddFilledRect(cx, y + barH * 0.5f, halfW - pad, barH * 0.5f, 0.0f, 0.2f, 0.2f, 0.2f, 0.9f);
    float r, g, b;
    levelColour(held, r, g, b);
    const float f = held > 1.0f ? 1.0f : held;
    if (f > 0.001f)
        renderer.AddFilledRect(left + f * width * 0.5f, y + barH * 0.5f, f * width * 0.5f, barH * 0.5f, 0.0f, r, g, b, 0.95f);
    const float m = lapMax > 1.0f ? 1.0f : lapMax;
    renderer.AddFilledRect(left + m * width, y + barH * 0.5f, 1.5f * s, barH * 0.7f, 0.0f, 1.0f, 1.0f, 1.0f, 0.9f);
    y += barH + pad;

    const float pm = s_prevLapMax.load();
    if (pm >= 0.0f)
    {
        const float clip = s_prevLapClip.load() * 100.0f;
        if (clip >= 0.05f) sprintf_s(line, "Last lap max %3.0f%%  clip %.1f%%", pm * 100.0f, clip);
        else sprintf_s(line, "Last lap max %3.0f%%", pm * 100.0f);
        levelColour(pm, r, g, b);
    }
    else
    {
        sprintf_s(line, "Last lap max --");
        r = g = b = 0.8f;
    }
    renderer.AddText(cx, y, line, textPx, r, g, b, 1.0f, Renderer::HAlign::Center);

    renderer.SetTextBackground(prevBg);
    return true;
}
