#include "ProximityHue.h"
#include "Renderer.h"
#include "CarState.h"
#include "Config.h"
#include "Layout.h"
#include "Log.h"
#include <cmath>
#include <cstdio>

ProximityHue g_proximityHue;

// Every knob below lives in RadarConfig (the [Radar] section of the INI), so
// the whole look of the bars can be tuned without a rebuild. What is NOT
// tunable here is WHICH cars trigger a bar: that comes from the shared
// Danger*Meters window, so the bars and the red radar blips can never
// disagree.
namespace
{
    // Lateral distance drives the bar's base opacity. At/inside sideBarNearM
    // it is at full strength; at the far end it is faint but still clearly
    // present. The FAR anchor is deliberately not its own setting: it is
    // dangerLateralM, the same value that decides whether the car counts at
    // all, so the bar is always exactly at its faint end the moment it
    // appears and the ramp can never be anchored outside its own activation
    // range.
    float BarAlpha(const RadarConfig& cfg, double lateralAbsM)
    {
        const double nearAtM = cfg.sideBarNearM;
        const double farAtM = cfg.dangerLateralM;

        // At or inside the near anchor, or a degenerate config with the far
        // anchor pulled inside the near one: flat full strength.
        if (lateralAbsM <= nearAtM || farAtM <= nearAtM)
            return cfg.sideBarAlphaNear;
        if (lateralAbsM >= farAtM)
            return cfg.sideBarAlphaFar;

        const double t = (lateralAbsM - nearAtM) / (farAtM - nearAtM); // 0 near, 1 far
        return cfg.sideBarAlphaNear
             + static_cast<float>(t) * (cfg.sideBarAlphaFar - cfg.sideBarAlphaNear);
    }

    // Maps the car's forward offset onto a top/bottom bias, so the bar is
    // strongest at the end of the screen the car actually is: ahead of you
    // weights the TOP, behind you weights the BOTTOM, and level with you
    // leaves it even. Both the alpha and the width come off the SAME pair of
    // dim factors, so the fade and the taper always lean the same way by the
    // same amount. Normalised against the same window that decides whether
    // the car counts at all, so the extremes of the shape line up exactly
    // with the extremes of the activation zone.
    // dimTop/dimBottom (0 = this end at full strength, 1 = this end fully
    // tapered away) are handed back raw, ungated by sideBarVerticalEndFade,
    // so callers that want to key something OTHER than alpha off "how far
    // into the taper is this end" -- e.g. the fade-to-green colour blend --
    // get the full 0..1 range regardless of how strongly the alpha itself
    // is configured to fade.
    void BarVerticalShape(const RadarConfig& cfg, double forwardM,
                          float baseAlpha,
                          float& aTop, float& aBottom,
                          float& wTop, float& wBottom,
                          float& dimTop, float& dimBottom)
    {
        const double aheadLimitM = cfg.dangerAheadM;
        const double behindLimitM = cfg.dangerBehindM;

        double n = 0.0;
        if (forwardM > 0.0 && aheadLimitM > 0.0)
            n = forwardM / aheadLimitM;       // 0 .. +1 ahead
        else if (forwardM < 0.0 && behindLimitM > 0.0)
            n = forwardM / behindLimitM;      // 0 .. -1 behind
        if (n > 1.0) n = 1.0;
        if (n < -1.0) n = -1.0;

        const float ahead = static_cast<float>(n);
        dimTop = (ahead < 0.0f) ? -ahead : 0.0f;    // car behind -> dim the top
        dimBottom = (ahead > 0.0f) ? ahead : 0.0f;  // car ahead  -> dim the bottom
        // Gamma bends the curve so a MODEST offset already produces an
        // obvious lean; left linear, a third of the way up the window dims
        // the far end by only a third, which across a 2160px bar is
        // near-invisible.
        const float gamma = cfg.sideBarVerticalFadeGamma;
        if (dimTop > 0.0f) dimTop = powf(dimTop, gamma);
        if (dimBottom > 0.0f) dimBottom = powf(dimBottom, gamma);

        aTop = baseAlpha * (1.0f - cfg.sideBarVerticalEndFade * dimTop);
        aBottom = baseAlpha * (1.0f - cfg.sideBarVerticalEndFade * dimBottom);
        wTop = 1.0f - cfg.sideBarWidthEndTaper * dimTop;
        wBottom = 1.0f - cfg.sideBarWidthEndTaper * dimBottom;
    }

    void LerpColor(float r0, float g0, float b0, float r1, float g1, float b1, float t,
                   float& r, float& g, float& b)
    {
        r = r0 + (r1 - r0) * t;
        g = g0 + (g1 - g0) * t;
        b = b0 + (b1 - b0) * t;
    }

    void AdvanceFade(const RadarConfig& cfg, float& fade, bool active, float dt)
    {
        const float seconds = active ? cfg.sideBarFadeInSeconds : cfg.sideBarFadeOutSeconds;
        // A zero or negative time means "instant" rather than a divide by zero.
        const float rate = (seconds > 0.0001f) ? (1.0f / seconds) : 1.0e9f;
        const float target = active ? 1.0f : 0.0f;
        if (fade < target)
        {
            fade += rate * dt;
            if (fade > target) fade = target;
        }
        else if (fade > target)
        {
            fade -= rate * dt;
            if (fade < target) fade = target;
        }
    }
}

void ProximityHue::Draw(Renderer& renderer, int screenWidth, int screenHeight, HWND hwnd)
{
    const RadarConfig& cfg = g_radarConfig;

    CarPos cars[128];
    double playerHeading = 0.0;
    bool hasPlayer = false;
    const int count = g_carState.Read(cars, 128, playerHeading, hasPlayer);

    const CarPos* player = nullptr;
    for (int i = 0; i < count; ++i)
    {
        if (cars[i].isPlayer) { player = &cars[i]; break; }
    }

    // No usable car data (not on track, between sessions). Drop everything so
    // the next session starts clean rather than resuming half-faded bars.
    // Layout mode still gets its resize handle/slider -- arranging the HUD
    // does not require anyone else to be on track.
    if (count == 0 || !hasPlayer || !player)
    {
        for (int i = 0; i < kMaxContributions; ++i)
            m_contributions[i] = Contribution();
        m_lastDrawTime.QuadPart = 0;
        if (Layout::Active())
            DrawLayoutControls(renderer, screenWidth, screenHeight, hwnd);
        return;
    }

    const double cosH = cos(-playerHeading);
    const double sinH = sin(-playerHeading);

    // Which tracked slots the danger test found again THIS frame. Anything
    // not marked is fading out (its car left the window, or vanished from the
    // snapshot entirely).
    bool stillActive[kMaxContributions] = {};

    // Deliberately mirrors DrawRadar()'s per-car pipeline step for step. If
    // that one's range/fade/danger rules change, change these to match --
    // the whole point of this widget is that it agrees with the red blips.
    for (int i = 0; i < count; ++i)
    {
        if (cars[i].isPlayer)
            continue;

        const double dx = cars[i].x - player->x;
        const double dz = cars[i].z - player->z;

        // Rotate world delta into player-forward-up radar space.
        const double relX = dx * cosH - dz * sinH;
        const double relZ = dx * sinH + dz * cosH;
        const double lateral = -relX;  // +ve = to the player's right
        const double forward = -relZ;  // +ve = ahead of the player

        // Elliptical range cull -- without this, "alongside" below has no
        // lateral bound at all and fires for cars on a completely different
        // part of the track that merely happen to be level with you.
        const double longRange = (forward >= 0.0) ? cfg.rangeForwardM : cfg.rangeRearM;
        const double nx = lateral / cfg.rangeSideM;
        const double nz = forward / longRange;
        const double edgeDist = sqrt(nx * nx + nz * nz);
        if (edgeDist > 1.0)
            continue;

        // A blip faded to nothing at the radar's edge isn't visibly red
        // either, so it shouldn't light the bar.
        double alpha = 1.0;
        if (edgeDist > cfg.fadeStartFraction)
        {
            const double t = (edgeDist - cfg.fadeStartFraction) / (1.0 - cfg.fadeStartFraction);
            alpha = 1.0 - t;
            alpha = alpha * alpha * (3.0 - 2.0 * alpha);
        }
        if (alpha <= 0.004)
            continue;

        // Byte-for-byte the same test Radar.cpp uses for a red blip, off the
        // same config values -- a bar appearing and a blip going red must
        // always mean the same thing.
        const bool inLongitudinalWindow =
            (forward <= cfg.dangerAheadM) && (forward >= -cfg.dangerBehindM);
        const bool lateralOverlap = fabs(lateral) <= cfg.dangerLateralM;
        const bool danger = inLongitudinalWindow && lateralOverlap;

        if (!danger)
            continue;

        // Every triggering car gets its own tracked slot, matched by car id
        // so its fade survives across frames. Cars are NOT merged per side:
        // their bars superimpose at draw time, so two cars alongside read
        // stronger than one.
        int slot = -1;
        int firstFree = -1;
        for (int s = 0; s < kMaxContributions; ++s)
        {
            if (m_contributions[s].id == cars[i].id) { slot = s; break; }
            if (firstFree < 0 && m_contributions[s].id < 0) firstFree = s;
        }
        if (slot < 0) slot = firstFree;
        if (slot < 0)
            continue;   // more simultaneous threats than slots; ignore extras

        Contribution& c = m_contributions[slot];
        if (c.id != cars[i].id)
        {
            c = Contribution();       // reusing a free slot: start from zero fade
            c.id = cars[i].id;
        }
        c.onLeft = lateral < 0.0;
        c.lateralM = fabs(lateral);
        c.forwardM = forward;
        stillActive[slot] = true;
    }

    // Frame delta for the fade. Clamped so a hitch, a pause, or the first
    // call after the overlay was hidden can't jump the fade in one step.
    if (m_freq.QuadPart == 0)
        QueryPerformanceFrequency(&m_freq);
    LARGE_INTEGER nowTicks;
    QueryPerformanceCounter(&nowTicks);
    float dt = 0.0f;
    if (m_lastDrawTime.QuadPart != 0 && m_freq.QuadPart != 0)
    {
        dt = static_cast<float>(static_cast<double>(nowTicks.QuadPart - m_lastDrawTime.QuadPart)
                                / static_cast<double>(m_freq.QuadPart));
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.1f) dt = 0.1f;
    }
    m_lastDrawTime = nowTicks;

    const float barW = cfg.sideBarWidthFraction * cfg.sideBarScale * screenWidth;
    const float screenH = static_cast<float>(screenHeight);
    const float edge = static_cast<float>(screenWidth);
    const float innerAlpha = cfg.sideBarInnerEdgeAlpha;

    // Each tracked car draws its own bar; they superimpose through the
    // renderer's normal alpha blending, so overlapping bars compound rather
    // than one masking another.
    //
    // Position is only refreshed while the car is still triggering (the loop
    // above), so a contribution that is fading OUT keeps the opacity and
    // shape it had when the car left instead of snapping.
    //
    // Each bar stays welded to the screen edge; it is the INNER edge that
    // moves, so the taper eats into the screen rather than pulling the bar
    // away from the periphery where it needs to be seen. Horizontally it is
    // solid at the screen edge and fades to nothing at the inner edge, so
    // there is no hard vertical line cutting across the view -- it just
    // dissolves. kInnerEdgeAlpha is the multiplier applied at that inner
    // edge; the outer edge always keeps the full value.
    for (int s = 0; s < kMaxContributions; ++s)
    {
        Contribution& c = m_contributions[s];
        if (c.id < 0)
            continue;

        AdvanceFade(cfg, c.fade, stillActive[s], dt);
        if (c.fade <= 0.001f && !stillActive[s])
        {
            c = Contribution();   // fully faded out: release the slot
            continue;
        }

        // "Too close" blink: independent of whichever rule decided this car
        // is triggering the bar at all (that's already baked into c.fade/
        // c.lateralM by this point) -- swaps the WHOLE bar's colour between
        // its normal colour and the flash colour, rather than dimming it, so
        // it reads as a full-bar alert rather than a fade -- this overrides
        // the fade-to-green shading below entirely rather than blending with
        // it, on the same reasoning: an alert should read as one flat alarm
        // colour, not a gradient. Phase comes off the real clock rather than
        // a per-contribution timer, so multiple cars flashing at once stay
        // in sync with each other.
        bool tooCloseFlashOn = false;
        float flashR = 0.0f, flashG = 0.0f, flashB = 0.0f;
        if (cfg.sideBarTooCloseFlashEnabled && c.lateralM < cfg.sideBarTooCloseMeters
            && cfg.sideBarTooCloseFlashHz > 0.01f && m_freq.QuadPart != 0)
        {
            const double nowSeconds = static_cast<double>(nowTicks.QuadPart) / static_cast<double>(m_freq.QuadPart);
            const double period = 1.0 / static_cast<double>(cfg.sideBarTooCloseFlashHz);
            const double phase = fmod(nowSeconds, period) / period; // 0..1
            if (phase >= 0.5)
            {
                tooCloseFlashOn = true;
                flashR = cfg.sideBarTooCloseFlashR;
                flashG = cfg.sideBarTooCloseFlashG;
                flashB = cfg.sideBarTooCloseFlashB;
            }
        }

        float aTop, aBottom, wTop, wBottom, dimTop, dimBottom;
        BarVerticalShape(cfg, c.forwardM, BarAlpha(cfg, c.lateralM) * c.fade * cfg.sideBarOpacityMultiplier,
                         aTop, aBottom, wTop, wBottom, dimTop, dimBottom);

        if (tooCloseFlashOn || !cfg.sideBarFadeToGreenEnabled)
        {
            // Flat single colour across the whole bar -- either the alert
            // override above, or fade-to-green turned off entirely.
            const float barR = tooCloseFlashOn ? flashR : cfg.sideBarR;
            const float barG = tooCloseFlashOn ? flashG : cfg.sideBarG;
            const float barB = tooCloseFlashOn ? flashB : cfg.sideBarB;
            if (c.onLeft)
            {
                renderer.AddGradientQuad(0.0f, barW * wTop,
                                         0.0f, barW * wBottom,
                                         0.0f, screenH,
                                         barR, barG, barB,
                                         aTop, aTop * innerAlpha,
                                         aBottom, aBottom * innerAlpha);
            }
            else
            {
                renderer.AddGradientQuad(edge - barW * wTop, edge,
                                         edge - barW * wBottom, edge,
                                         0.0f, screenH,
                                         barR, barG, barB,
                                         aTop * innerAlpha, aTop,
                                         aBottom * innerAlpha, aBottom);
            }
            continue;
        }

        // Fade-to-green: this is a SPATIAL effect, not a temporal one -- it
        // rides the same top/bottom lean that BarVerticalShape already
        // computes from the car's forward offset. Dead alongside (forwardM
        // ~ 0), dimTop and dimBottom are both ~0, so the whole bar is one
        // uniform colour; as the car drifts ahead or behind, the end that
        // BarVerticalShape is tapering away also shifts toward green, so the
        // "this end is fading" cue is colour AND opacity together, and the
        // still-solid end (where the car actually is) stays pure red.
        //
        // AddGradientQuad only takes one flat colour per call, so a true
        // colour gradient needs the bar sliced into stacked segments, each
        // with its own colour interpolated at its own dim fraction; more
        // segments would read smoother but cost more quads for no visible
        // gain at typical bar heights.
        const int kFadeSegments = 20;
        for (int seg = 0; seg < kFadeSegments; ++seg)
        {
            const float f0 = static_cast<float>(seg) / kFadeSegments;
            const float f1 = static_cast<float>(seg + 1) / kFadeSegments;
            const float a0 = aTop + (aBottom - aTop) * f0;
            const float a1 = aTop + (aBottom - aTop) * f1;
            const float w0 = wTop + (wBottom - wTop) * f0;
            const float w1 = wTop + (wBottom - wTop) * f1;
            const float dim0 = dimTop + (dimBottom - dimTop) * f0;
            const float dim1 = dimTop + (dimBottom - dimTop) * f1;
            const float dimSeg = (dim0 + dim1) * 0.5f;

            float segR, segG, segB;
            LerpColor(cfg.sideBarR, cfg.sideBarG, cfg.sideBarB,
                      cfg.sideBarFadeToGreenR, cfg.sideBarFadeToGreenG, cfg.sideBarFadeToGreenB,
                      dimSeg, segR, segG, segB);

            const float y0 = screenH * f0;
            const float y1 = screenH * f1;
            if (c.onLeft)
            {
                renderer.AddGradientQuad(0.0f, barW * w0,
                                         0.0f, barW * w1,
                                         y0, y1,
                                         segR, segG, segB,
                                         a0, a0 * innerAlpha,
                                         a1, a1 * innerAlpha);
            }
            else
            {
                renderer.AddGradientQuad(edge - barW * w0, edge,
                                         edge - barW * w1, edge,
                                         y0, y1,
                                         segR, segG, segB,
                                         a0 * innerAlpha, a0,
                                         a1 * innerAlpha, a1);
            }
        }
    }

    if (Layout::Active())
        DrawLayoutControls(renderer, screenWidth, screenHeight, hwnd);

}

namespace
{
    float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}

// Two small controls, shown only in layout mode, sitting beside the RIGHT
// bar at mid-screen height -- clear of every other panel's default position.
// Both are ABSOLUTE sliders -- knob position directly encodes the value
// along a fixed range -- rather than a relative "grab and drag from
// wherever it started" gesture. The width control used to be a left/right
// drag, but with the bars pinned to opposite screen edges, "drag toward the
// middle to grow" only reads correctly from the right bar's side and
// reads backwards from the left's. A VERTICAL up/down fader avoids that:
// up = bigger reads the same regardless of which bar you're looking at,
// and matches the opacity slider's own already-liked absolute-position feel.
void ProximityHue::DrawLayoutControls(Renderer& renderer, int screenWidth, int screenHeight, HWND hwnd)
{
    RadarConfig& cfg = g_radarConfig;
    const float uiScale = static_cast<float>(screenHeight) / 2160.0f;

    // Fixed screen fraction, deliberately NOT anchored off the bar's own
    // current edge (screenWidth - barW - ...): a bar dragged wide enough
    // (SideBarScale can reach 3.0, wider than the screen) would drag its own
    // controls off-screen with it -- exactly what happened before this was
    // an absolute slider. A fixed spot is always reachable regardless of
    // how large or small the bar currently is.
    const float colX = screenWidth * 0.85f;
    // Second column for the too-close threshold/rate sliders, clear of the
    // width/opacity pair above.
    const float colX2 = screenWidth * 0.72f;

    const float widthTrackH = 180.0f * uiScale;
    const float widthTrackW = 10.0f * uiScale;
    const float widthTrackTopY = screenHeight * 0.5f - 140.0f * uiScale - widthTrackH * 0.5f;
    const float widthTrackBotY = widthTrackTopY + widthTrackH;

    const float opacityTrackW = 180.0f * uiScale;
    const float opacityTrackH = 10.0f * uiScale;
    const float opacityTrackX = colX - opacityTrackW * 0.5f;
    const float opacityTrackY = screenHeight * 0.5f + 140.0f * uiScale;

    // Same shapes as the width/opacity pair, mirrored into the second column.
    const float closeTrackH = widthTrackH;
    const float closeTrackW = widthTrackW;
    const float closeTrackTopY = widthTrackTopY;
    const float closeTrackBotY = widthTrackBotY;

    const float hzTrackW = opacityTrackW;
    const float hzTrackH = opacityTrackH;
    const float hzTrackX = colX2 - hzTrackW * 0.5f;
    const float hzTrackY = opacityTrackY;

    const float knobR = 12.0f * uiScale;
    const float labelPx = 36.0f * uiScale;
    const float capPx = 28.0f * uiScale;

    // Bar width on screen is sideBarWidthFraction * sideBarScale * screenWidth,
    // so the minimum scale is derived rather than a flat constant -- this way
    // "5% of screen width" stays true as the actual floor regardless of
    // whatever sideBarWidthFraction the INI's base width happens to be.
    const float kMinScale = (cfg.sideBarWidthFraction > 0.0001f) ? (0.05f / cfg.sideBarWidthFraction) : 0.05f;
    const float kMaxScale = 3.0f;
    const float kMinOpacity = 0.1f, kMaxOpacity = 2.0f;
    const float kMinCloseM = 0.0f, kMaxCloseM = cfg.sideBarTooCloseMetersMax;
    const float kMinHz = 1.0f, kMaxHz = cfg.sideBarTooCloseFlashHzMax;

    float cx = 0.0f, cy = 0.0f;
    const bool haveCursor = Layout::CursorPosition(hwnd, screenWidth, screenHeight, cx, cy);
    const bool buttonDown = haveCursor && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool clickEdge = buttonDown && !m_controlsPrevButtonDown;
    const bool anyDragging = m_resizeDragging || m_sliderDragging || m_tooCloseMetersDragging || m_tooCloseHzDragging;

    // --- Width slider (vertical): top of the track is the maximum size,
    // bottom is the minimum.
    const float widthFrac = ClampF((cfg.sideBarScale - kMinScale) / (kMaxScale - kMinScale), 0.0f, 1.0f);
    const float widthKnobY = widthTrackBotY - widthFrac * widthTrackH;

    if (clickEdge && !anyDragging &&
        cx >= colX - knobR && cx <= colX + knobR &&
        cy >= widthTrackTopY - knobR && cy <= widthTrackBotY + knobR)
    {
        m_resizeDragging = true;
    }
    if (m_resizeDragging && buttonDown)
    {
        const float t = ClampF((widthTrackBotY - cy) / widthTrackH, 0.0f, 1.0f);
        cfg.sideBarScale = kMinScale + t * (kMaxScale - kMinScale);
    }
    else if (m_resizeDragging && !buttonDown)
    {
        m_resizeDragging = false;
        if (!Config::WriteFloat("Radar", "SideBarScale", cfg.sideBarScale))
            Log::Write("ProximityHue: failed to write [Radar] SideBarScale to the INI");
    }

    // --- Opacity slider (horizontal, unchanged).
    const float opacityFrac = ClampF((cfg.sideBarOpacityMultiplier - kMinOpacity) / (kMaxOpacity - kMinOpacity), 0.0f, 1.0f);
    const float opacityKnobX = opacityTrackX + opacityFrac * opacityTrackW;

    if (clickEdge && !anyDragging &&
        cx >= opacityTrackX - knobR && cx <= opacityTrackX + opacityTrackW + knobR &&
        cy >= opacityTrackY - knobR * 1.5f && cy <= opacityTrackY + knobR * 1.5f)
    {
        m_sliderDragging = true;
    }
    if (m_sliderDragging && buttonDown)
    {
        const float t = ClampF((cx - opacityTrackX) / opacityTrackW, 0.0f, 1.0f);
        cfg.sideBarOpacityMultiplier = kMinOpacity + t * (kMaxOpacity - kMinOpacity);
    }
    else if (m_sliderDragging && !buttonDown)
    {
        m_sliderDragging = false;
        if (!Config::WriteFloat("Radar", "SideBarOpacityMultiplier", cfg.sideBarOpacityMultiplier))
            Log::Write("ProximityHue: failed to write [Radar] SideBarOpacityMultiplier to the INI");
    }

    // --- Too-close threshold slider (vertical, second column): 0m at the
    // bottom of the track, SideBarTooCloseMetersMax at the top.
    const float closeFrac = ClampF((cfg.sideBarTooCloseMeters - kMinCloseM) / (kMaxCloseM - kMinCloseM), 0.0f, 1.0f);
    const float closeKnobY = closeTrackBotY - closeFrac * closeTrackH;

    if (clickEdge && !anyDragging &&
        cx >= colX2 - knobR && cx <= colX2 + knobR &&
        cy >= closeTrackTopY - knobR && cy <= closeTrackBotY + knobR)
    {
        m_tooCloseMetersDragging = true;
    }
    if (m_tooCloseMetersDragging && buttonDown)
    {
        const float t = ClampF((closeTrackBotY - cy) / closeTrackH, 0.0f, 1.0f);
        cfg.sideBarTooCloseMeters = kMinCloseM + t * (kMaxCloseM - kMinCloseM);
    }
    else if (m_tooCloseMetersDragging && !buttonDown)
    {
        m_tooCloseMetersDragging = false;
        if (!Config::WriteFloat("Radar", "SideBarTooCloseMeters", cfg.sideBarTooCloseMeters))
            Log::Write("ProximityHue: failed to write [Radar] SideBarTooCloseMeters to the INI");
    }

    // --- Blink-rate slider (horizontal, second column): 1Hz at the left,
    // SideBarTooCloseFlashHzMax at the right.
    const float hzFrac = ClampF((cfg.sideBarTooCloseFlashHz - kMinHz) / (kMaxHz - kMinHz), 0.0f, 1.0f);
    const float hzKnobX = hzTrackX + hzFrac * hzTrackW;

    if (clickEdge && !anyDragging &&
        cx >= hzTrackX - knobR && cx <= hzTrackX + hzTrackW + knobR &&
        cy >= hzTrackY - knobR * 1.5f && cy <= hzTrackY + knobR * 1.5f)
    {
        m_tooCloseHzDragging = true;
    }
    if (m_tooCloseHzDragging && buttonDown)
    {
        const float t = ClampF((cx - hzTrackX) / hzTrackW, 0.0f, 1.0f);
        cfg.sideBarTooCloseFlashHz = kMinHz + t * (kMaxHz - kMinHz);
    }
    else if (m_tooCloseHzDragging && !buttonDown)
    {
        m_tooCloseHzDragging = false;
        if (!Config::WriteFloat("Radar", "SideBarTooCloseFlashHz", cfg.sideBarTooCloseFlashHz))
            Log::Write("ProximityHue: failed to write [Radar] SideBarTooCloseFlashHz to the INI");
    }

    m_controlsPrevButtonDown = buttonDown;

    // --- Draw both, with a short instruction under each title so the
    // gesture doesn't have to be guessed at, brighter while actively grabbed.
    renderer.AddFilledRoundedRect(colX, (widthTrackTopY + widthTrackBotY) * 0.5f,
                                  widthTrackW * 0.5f, widthTrackH * 0.5f, widthTrackW * 0.5f, 0.0f,
                                  1.0f, 1.0f, 1.0f, 0.35f);
    renderer.AddFilledCircle(colX, widthKnobY, knobR, 1.0f, 0.85f, 0.2f, m_resizeDragging ? 1.0f : 0.9f);
    renderer.AddText(colX, widthTrackTopY - knobR - labelPx - capPx - 6.0f * uiScale, "Bar width", labelPx,
                     1.0f, 1.0f, 1.0f, 0.85f, Renderer::HAlign::Center);
    renderer.AddText(colX, widthTrackTopY - knobR - capPx - 2.0f * uiScale, "drag up/down", capPx,
                     1.0f, 1.0f, 1.0f, 0.65f, Renderer::HAlign::Center);

    renderer.AddFilledRoundedRect(opacityTrackX + opacityTrackW * 0.5f, opacityTrackY,
                                  opacityTrackW * 0.5f, opacityTrackH * 0.5f, opacityTrackH * 0.5f, 0.0f,
                                  1.0f, 1.0f, 1.0f, 0.35f);
    renderer.AddFilledCircle(opacityKnobX, opacityTrackY, knobR, 1.0f, 0.85f, 0.2f, m_sliderDragging ? 1.0f : 0.9f);
    renderer.AddText(opacityTrackX, opacityTrackY - knobR - labelPx - capPx - 6.0f * uiScale, "Bar opacity", labelPx,
                     1.0f, 1.0f, 1.0f, 0.85f);
    renderer.AddText(opacityTrackX, opacityTrackY - knobR - capPx - 2.0f * uiScale, "drag left/right", capPx,
                     1.0f, 1.0f, 1.0f, 0.65f);

    char closeCap[24], hzCap[24];
    sprintf_s(closeCap, "%.1fm", cfg.sideBarTooCloseMeters);
    sprintf_s(hzCap, "%.1fHz", cfg.sideBarTooCloseFlashHz);

    renderer.AddFilledRoundedRect(colX2, (closeTrackTopY + closeTrackBotY) * 0.5f,
                                  closeTrackW * 0.5f, closeTrackH * 0.5f, closeTrackW * 0.5f, 0.0f,
                                  1.0f, 1.0f, 1.0f, 0.35f);
    renderer.AddFilledCircle(colX2, closeKnobY, knobR, 1.0f, 0.85f, 0.2f, m_tooCloseMetersDragging ? 1.0f : 0.9f);
    renderer.AddText(colX2, closeTrackTopY - knobR - labelPx - capPx - 6.0f * uiScale, "Too-close range", labelPx,
                     1.0f, 1.0f, 1.0f, 0.85f, Renderer::HAlign::Center);
    renderer.AddText(colX2, closeTrackTopY - knobR - capPx - 2.0f * uiScale, closeCap, capPx,
                     1.0f, 1.0f, 1.0f, 0.65f, Renderer::HAlign::Center);

    renderer.AddFilledRoundedRect(hzTrackX + hzTrackW * 0.5f, hzTrackY,
                                  hzTrackW * 0.5f, hzTrackH * 0.5f, hzTrackH * 0.5f, 0.0f,
                                  1.0f, 1.0f, 1.0f, 0.35f);
    renderer.AddFilledCircle(hzKnobX, hzTrackY, knobR, 1.0f, 0.85f, 0.2f, m_tooCloseHzDragging ? 1.0f : 0.9f);
    renderer.AddText(hzTrackX, hzTrackY - knobR - labelPx - capPx - 6.0f * uiScale, "Blink rate", labelPx,
                     1.0f, 1.0f, 1.0f, 0.85f);
    renderer.AddText(hzTrackX, hzTrackY - knobR - capPx - 2.0f * uiScale, hzCap, capPx,
                     1.0f, 1.0f, 1.0f, 0.65f);
}
