#include "TyreTemps.h"
#include "InternalsPlugin.hpp"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "Log.h"
#include "RealtimeState.h"
#include "RenderThrottle.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

TyreTempStore g_tyreTemps;

namespace
{
    const double kGravity = 9.80665;
    const double kKelvin = 273.15;
    const double kRadToDeg = 57.29577951308232;

    inline bool IsLeftWheel(int wheel) { return wheel == 0 || wheel == 2; }

    // SDK sample index -> position across the tread. rF2's local +x points
    // left, so index 0 (the leftmost sample) is the OUTSIDE edge on a
    // left-side wheel and the INSIDE edge on a right-side wheel. Unverified
    // against real data -- see TyreTempsConfig::swapInsideOutside.
    inline int PositionForIndex(int wheel, int idx, bool swap)
    {
        int pos = IsLeftWheel(wheel) ? idx : 2 - idx;
        if (swap) pos = 2 - pos;
        return pos;
    }
}

void TyreTempStore::Init()
{
    if (m_ready) return;
    InitializeCriticalSection(&m_lock);
    m_ready = true;
}

void TyreTempStore::Clear()
{
    m_phase = Phase::Idle;
    m_lastET = -1.0;
    m_cornerCounter = 0;
    ResetAccum();
    if (!m_ready) return;
    EnterCriticalSection(&m_lock);
    m_state = State{};
    LeaveCriticalSection(&m_lock);
}

void TyreTempStore::ResetAccum()
{
    for (int w = 0; w < 4; ++w)
    {
        for (int i = 0; i < 3; ++i)
        {
            m_peak[w][i] = -1000.0f;
            m_lastInner[w][i] = 0.0f;
        }
        m_loadSum[w] = 0.0;
        m_camberSum[w] = 0.0;
    }
    m_samples = 0;
}

void TyreTempStore::Accumulate(const TelemInfoV01& info)
{
    for (int w = 0; w < 4; ++w)
    {
        const TelemWheelV01& wh = info.mWheel[w];
        for (int i = 0; i < 3; ++i)
        {
            const float surface = static_cast<float>(wh.mTemperature[i] - kKelvin);
            if (surface > m_peak[w][i]) m_peak[w][i] = surface;
            m_lastInner[w][i] = static_cast<float>(wh.mTireInnerLayerTemperature[i] - kKelvin);
        }
        m_loadSum[w] += wh.mTireLoad;
        m_camberSum[w] += wh.mCamber;
    }
    ++m_samples;
}

void TyreTempStore::Finalize(double drivenSeconds)
{
    const TyreTempsConfig& cfg = g_tyreTempsConfig;
    if (m_samples < 10 || drivenSeconds < cfg.minCornerSeconds)
        return;

    // The loaded side is whichever carried more tyre load over the corner --
    // no sign convention involved.
    const double leftLoad = m_loadSum[0] + m_loadSum[2];
    const double rightLoad = m_loadSum[1] + m_loadSum[3];

    State s;
    s.valid = true;
    s.cornerCount = ++m_cornerCounter;
    s.loadedLeft = leftLoad > rightLoad;
    s.durationSeconds = static_cast<float>(drivenSeconds);
    for (int w = 0; w < 4; ++w)
    {
        for (int i = 0; i < 3; ++i)
        {
            const int pos = PositionForIndex(w, i, cfg.swapInsideOutside);
            s.wheel[w].surfacePeak[pos] = m_peak[w][i];
            s.wheel[w].inner[pos] = m_lastInner[w][i];
        }
    }

    // Raw SDK-order values (index 0/1/2 = the SDK's left/centre/right) plus
    // mean camber, so the inside/outside mapping can be checked against a
    // real corner rather than trusted.
    const double n = static_cast<double>(m_samples);
    Log::Write("TyreTemps: corner %d, %s wheels loaded, %.1fs, %d samples | camber deg FL %.1f FR %.1f RL %.1f RR %.1f | "
               "raw surface peak [0/1/2] FL %.0f/%.0f/%.0f FR %.0f/%.0f/%.0f RL %.0f/%.0f/%.0f RR %.0f/%.0f/%.0f | "
               "raw inner [0/1/2] FL %.0f/%.0f/%.0f FR %.0f/%.0f/%.0f RL %.0f/%.0f/%.0f RR %.0f/%.0f/%.0f",
               s.cornerCount, s.loadedLeft ? "left" : "right", drivenSeconds, m_samples,
               m_camberSum[0] / n * kRadToDeg, m_camberSum[1] / n * kRadToDeg,
               m_camberSum[2] / n * kRadToDeg, m_camberSum[3] / n * kRadToDeg,
               m_peak[0][0], m_peak[0][1], m_peak[0][2], m_peak[1][0], m_peak[1][1], m_peak[1][2],
               m_peak[2][0], m_peak[2][1], m_peak[2][2], m_peak[3][0], m_peak[3][1], m_peak[3][2],
               m_lastInner[0][0], m_lastInner[0][1], m_lastInner[0][2], m_lastInner[1][0], m_lastInner[1][1], m_lastInner[1][2],
               m_lastInner[2][0], m_lastInner[2][1], m_lastInner[2][2], m_lastInner[3][0], m_lastInner[3][1], m_lastInner[3][2]);

    EnterCriticalSection(&m_lock);
    m_state = s;
    LeaveCriticalSection(&m_lock);
}

void TyreTempStore::OnTelemetry(const TelemInfoV01& info)
{
    if (!m_ready)
        return;
    const TyreTempsConfig& cfg = g_tyreTempsConfig;
    if (!cfg.enabled)
        return;

    if (!g_inRealtime)
    {
        m_phase = Phase::Idle;
        return;
    }

    const double et = info.mElapsedTime;
    // A restart or a long stall breaks the timing the phases rely on.
    if (m_lastET >= 0.0 && (et < m_lastET || et - m_lastET > 1.0))
        m_phase = Phase::Idle;
    m_lastET = et;

    // A zeroed sample (no tyre data) is skipped rather than recorded as -273C.
    if (info.mWheel[0].mTemperature[1] < 200.0)
        return;

    const double latG = fabs(info.mLocalAccel.x) / kGravity;
    const bool above = latG >= cfg.cornerEnterG;
    const bool below = latG < cfg.cornerExitG;

    switch (m_phase)
    {
    case Phase::Idle:
        if (above)
        {
            ResetAccum();
            m_pendingStartET = et;
            m_phase = Phase::Pending;
            Accumulate(info);
        }
        break;

    case Phase::Pending:
        if (below)
        {
            m_phase = Phase::Idle;
            break;
        }
        Accumulate(info);
        if (et - m_pendingStartET >= cfg.enterHoldSeconds)
        {
            m_phase = Phase::InCorner;
            m_belowExitStartET = -1.0;
        }
        break;

    case Phase::InCorner:
        Accumulate(info);
        if (below)
        {
            if (m_belowExitStartET < 0.0)
                m_belowExitStartET = et;
            if (et - m_belowExitStartET >= cfg.exitHoldSeconds)
            {
                m_phase = Phase::Tail;
                m_tailStartET = et;
            }
        }
        else
        {
            m_belowExitStartET = -1.0;
        }
        break;

    case Phase::Tail:
        if (above)
        {
            // Next corner arrives before the tail ran out (a chicane): close
            // this one and start the next straight away.
            Finalize(m_tailStartET - m_pendingStartET);
            ResetAccum();
            m_pendingStartET = et;
            m_phase = Phase::Pending;
            Accumulate(info);
        }
        else
        {
            Accumulate(info);
            if (et - m_tailStartET >= cfg.tailSeconds)
            {
                Finalize(m_tailStartET - m_pendingStartET);
                m_phase = Phase::Idle;
            }
        }
        break;
    }
}

TyreTempStore::State TyreTempStore::Read()
{
    State copy;
    if (!m_ready) return copy;
    EnterCriticalSection(&m_lock);
    copy = m_state;
    LeaveCriticalSection(&m_lock);
    return copy;
}

bool TyreTemps::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    // Medium tier (~25Hz): the numbers only change once per corner.
    static RenderThrottle s_throttle(0.04);
    if (!Layout::Active() && s_throttle.TryReplay(renderer))
        return true;
    RenderThrottle::Capture throttleCapture(s_throttle, renderer);

    struct TextBgGuard
    {
        Renderer& r; bool prev;
        explicit TextBgGuard(Renderer& rr) : r(rr), prev(rr.TextBackgroundEnabled()) { r.SetTextBackground(false); }
        ~TextBgGuard() { r.SetTextBackground(prev); }
    } textBgGuard(renderer);

    const TyreTempsConfig& cfg = g_tyreTempsConfig;
    if (!cfg.enabled)
        return false;

    const TyreTempStore::State state = g_tyreTemps.Read();

    const float uiScale = static_cast<float>(screenHeight) / 2160.0f * cfg.scale;
    const float textPx = cfg.textHeightPx * uiScale;
    const float labelPx = cfg.labelHeightPx * uiScale;
    const float rowGapPx = cfg.rowGapPx * uiScale;
    const float colGapPx = cfg.columnGapPx * uiScale;
    const float padPx = cfg.bgPaddingPx * uiScale;
    const float cornerPx = cfg.bgCornerRadiusPx * uiScale;

    // Mini bar chart per tyre: three bars (outside/middle/inside, surface
    // peak temps) read at a glance on corner exit instead of numbers.
    const float barW = textPx * 0.55f;
    const float barGap = textPx * 0.2f;
    const float cellW = 3.0f * barW + 2.0f * barGap;
    const float cellH = textPx * 1.8f;
    const float pairGap = rowGapPx * 2.0f;
    const float titlePx = labelPx * 1.2f;
    const float titleH = titlePx + rowGapPx * 2.0f;

    char title[64];
    if (state.valid)
        sprintf_s(title, "Corner %d: %s turn", state.cornerCount, state.loadedLeft ? "right" : "left");
    else
        sprintf_s(title, "Tyre temps - no corner yet");

    float boxW = 2.0f * cellW + colGapPx + 2.0f * padPx;
    const float titleW = renderer.MeasureText(title, titlePx) + 2.0f * padPx;
    if (titleW > boxW) boxW = titleW;
    const float boxH = state.valid ? (titleH + 2.0f * cellH + pairGap + 2.0f * padPx)
                                   : (titleH + 2.0f * padPx);

    const float centerX = screenWidth * cfg.posXFraction;
    const float centerY = screenHeight * cfg.posYFraction;
    const float left = centerX - boxW * 0.5f;
    const float top = centerY - boxH * 0.5f;

    Layout::Register("Tyre temps", "TyreTemps", "PosXFraction", "PosYFraction",
                     &g_tyreTempsConfig.posXFraction, &g_tyreTempsConfig.posYFraction,
                     centerX, centerY, boxW * 0.5f, boxH * 0.5f,
                     &g_tyreTempsConfig.scale, "Scale");

    if (cfg.bgEnabled)
        renderer.AddFilledRoundedRect(centerX, centerY, boxW * 0.5f, boxH * 0.5f, cornerPx, 0.0f,
                                      cfg.bgR, cfg.bgG, cfg.bgB, cfg.bgA);

    renderer.AddText(centerX, top + padPx, title, titlePx, 0.85f, 0.85f, 0.85f, 1.0f, Renderer::HAlign::Center);

    if (!state.valid)
        return true;

    const float cellsTop = top + padPx + titleH;
    const float cellsLeft = centerX - cellW - colGapPx * 0.5f;

    for (int w = 0; w < 4; ++w)
    {
        const bool leftWheel = (w == 0 || w == 2);
        const bool frontRow = (w < 2);
        const bool loaded = (leftWheel == state.loadedLeft);
        const float alpha = loaded ? 1.0f : 0.5f;

        const float cellLeft = leftWheel ? cellsLeft : (cellsLeft + cellW + colGapPx);
        const float cellTop = cellsTop + (frontRow ? 0.0f : (cellH + pairGap));

        const TyreTempStore::Wheel& wh = state.wheel[w];

        // Screen order follows the car seen from above: outside edge to the
        // outside of the car, so left wheels read outside-middle-inside and
        // right wheels inside-middle-outside.
        static const int kLeftOrder[3] = { TyreTempStore::Outside, TyreTempStore::Middle, TyreTempStore::Inside };
        static const int kRightOrder[3] = { TyreTempStore::Inside, TyreTempStore::Middle, TyreTempStore::Outside };
        const int* order = leftWheel ? kLeftOrder : kRightOrder;

        const float* temps = wh.surfacePeak;
        // One scale for all four tyres, re-fitted every corner: the hottest
        // zone seen on ANY tyre this corner is full height, so tyres compare
        // directly against each other. A too-small spread is not stretched
        // (BarMinRangeC), so a 1C difference doesn't look dramatic.
        float tMin = state.wheel[0].surfacePeak[0], tMax = tMin;
        for (int ww = 0; ww < 4; ++ww)
            for (int c = 0; c < 3; ++c)
            {
                tMin = (std::min)(tMin, state.wheel[ww].surfacePeak[c]);
                tMax = (std::max)(tMax, state.wheel[ww].surfacePeak[c]);
            }
        const float range = (std::max)(tMax - tMin, cfg.barMinRangeC);
        const float base = tMax - range;
        const float cellBottom = cellTop + cellH;

        for (int c = 0; c < 3; ++c)
        {
            const float t = temps[order[c]];
            // 20%..100% of the cell, so the coolest zone is still a visible bar.
            const float frac = 0.2f + 0.8f * (std::min)(1.0f, (std::max)(0.0f, (t - base) / range));
            const float h = cellH * frac;

            float r, gg, b;
            if (t < cfg.barColdC)     { r = 0.3f; gg = 0.6f; b = 1.0f; }
            else if (t > cfg.barHotC) { r = 1.0f; gg = 0.3f; b = 0.3f; }
            else                      { r = 0.3f; gg = 1.0f; b = 0.3f; }

            const float x = cellLeft + c * (barW + barGap);
            renderer.AddFilledRoundedRect(x + barW * 0.5f, cellBottom - h * 0.5f, barW * 0.5f, h * 0.5f,
                                          0.0f, 0.0f, r, gg, b, alpha);
        }
    }

    return true;
}
