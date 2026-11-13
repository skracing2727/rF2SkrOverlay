#include "Layout.h"
#include "Renderer.h"
#include "Config.h"
#include "Log.h"
#include <cstdio>
#include <cstring>
#include <cmath>

namespace
{
    struct Panel
    {
        const char* name;
        const char* section;
        const char* xKey;
        const char* yKey;
        float* xFraction;
        float* yFraction;
        // The box, stored RELATIVE TO THE ANCHOR the two fractions place --
        // so it can be reconstructed at any later time from the current
        // fraction values, including on frames where the panel itself is not
        // drawing. See the header for why that matters.
        float offsetX, offsetY;   // box centre minus anchor position, in pixels
        float halfW, halfH;
        // Null on a panel that cannot be resized.
        float* scale;
        const char* scaleKey;
        const char* scaleSection;   // where scaleKey persists -- see Register()
        void (*onDragStart)();
        // Non-null on a box with independent width/height (RegisterBox).
        float* boxW;
        float* boxH;
        const char* wKey;
        const char* hKey;
        bool seenThisFrame;
        // The screen the offsets were measured on. A resolution change
        // invalidates a pixel offset, so a stale one is dropped rather than
        // drawn in the wrong place.
        int screenW, screenH;
    };

    // Generous: there are currently about two dozen draggable panels, cover
    // boxes included. Overflowing this silently drops the extras rather than
    // corrupting anything.
    const int kMaxPanels = 40;
    Panel g_panels[kMaxPanels];
    int g_panelCount = 0;

    bool g_active = false;

    // Backbuffer size, stashed so Register() can turn an absolute box centre
    // into an anchor-relative offset. Set by BeginFrame, which runs before
    // any panel registers.
    int g_lastScreenW = 0;
    int g_lastScreenH = 0;

    // The drag in progress. Identified by the config float pointer rather
    // than an index, because the registry is rebuilt every frame and a
    // panel's index in it can change (a panel above it being hidden mid-drag,
    // say) while the thing being dragged has not.
    float* g_dragX = nullptr;
    float* g_dragY = nullptr;
    const char* g_dragSection = nullptr;
    const char* g_dragXKey = nullptr;
    const char* g_dragYKey = nullptr;
    // Where in the panel it was grabbed, as a fraction offset -- so the panel
    // does not jump its centre to the cursor on the first frame.
    float g_grabOffsetX = 0.0f;
    float g_grabOffsetY = 0.0f;
    bool g_prevButtonDown = false;

    // Resize drag, which is a separate gesture from a move: grabbing the
    // corner handle rather than the body. Scale is driven by how far the
    // cursor is from the panel CENTRE compared with where it started, so the
    // corner tracks the cursor naturally in both axes at once.
    float* g_resizeScale = nullptr;
    const char* g_resizeSection = nullptr;
    const char* g_resizeKey = nullptr;
    float g_resizeStartScale = 1.0f;
    float g_resizeStartDist = 1.0f;
    // The panel's centre AT THE MOMENT IT WAS GRABBED, in pixels -- fixed for
    // the rest of the drag rather than re-read from the panel every frame.
    // Several panels (e.g. the perf stats box in its top-flush alignment)
    // anchor an EDGE and let their own centre move as they grow, so re-
    // deriving the centre live during a resize fed the box's own growth back
    // into the very distance measurement driving that growth -- the resize
    // would accelerate/overshoot or fight the cursor instead of tracking it.
    // A centre fixed at grab time removes that feedback loop entirely.
    float g_resizeCenterX = 0.0f;
    float g_resizeCenterY = 0.0f;

    // Width/height resize of a RegisterBox panel: its top-left corner stays
    // where it was at grab time and the bottom-right follows the cursor.
    float* g_boxW = nullptr;
    float* g_boxH = nullptr;
    const char* g_boxSection = nullptr;
    const char* g_boxWKey = nullptr;
    const char* g_boxHKey = nullptr;
    float g_boxLeft = 0.0f;
    float g_boxTop = 0.0f;
    const float kMinBoxPx = 20.0f; // at 2160p

    // Below half, text stops being legible; above 2.5x a panel swallows the
    // screen and buries whatever is under it.
    const float kMinScale = 0.5f;
    const float kMaxScale = 2.5f;

    float ClampScale(float v)
    {
        if (v < kMinScale) return kMinScale;
        if (v > kMaxScale) return kMaxScale;
        return v;
    }

    // Kept off the extreme edges so a panel can always be grabbed again.
    float Clamp01(float v)
    {
        if (v < 0.0f) return 0.0f;
        if (v > 1.0f) return 1.0f;
        return v;
    }

    // Cursor position in BACKBUFFER pixels. The window's client area and the
    // backbuffer are usually the same size but need not be (a scaled
    // presentation), so the client-relative position is converted through the
    // client rect rather than used directly.
    bool CursorInBackbuffer(HWND hwnd, int bbW, int bbH, float& outX, float& outY)
    {
        if (!hwnd || bbW <= 0 || bbH <= 0)
            return false;

        // Only when the cursor is actually on screen. GetCursorPos keeps
        // reporting a position while the cursor is hidden, so without this a
        // click would be acted on at wherever the invisible pointer happened
        // to be sitting.
        CURSORINFO ci = {};
        ci.cbSize = sizeof(ci);
        if (!GetCursorInfo(&ci) || (ci.flags & CURSOR_SHOWING) == 0)
            return false;

        POINT p;
        if (!GetCursorPos(&p) || !ScreenToClient(hwnd, &p))
            return false;

        RECT client;
        if (!GetClientRect(hwnd, &client))
            return false;
        const int clientW = client.right - client.left;
        const int clientH = client.bottom - client.top;
        if (clientW <= 0 || clientH <= 0)
            return false;

        outX = (static_cast<float>(p.x) / clientW) * bbW;
        outY = (static_cast<float>(p.y) / clientH) * bbH;
        return true;
    }
}

void Layout::BeginFrame(int screenWidth, int screenHeight)
{
    g_lastScreenW = screenWidth;
    g_lastScreenH = screenHeight;

    // Deliberately does NOT clear the registry -- see the header. Panels are
    // only marked unseen, so a panel that stops drawing keeps its remembered
    // box and stays draggable.
    for (int i = 0; i < g_panelCount; ++i)
        g_panels[i].seenThisFrame = false;
}

bool Layout::CursorPosition(HWND hwnd, int bbW, int bbH, float& outX, float& outY)
{
    return CursorInBackbuffer(hwnd, bbW, bbH, outX, outY);
}

bool Layout::IsCursorVisible()
{
    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    return GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0;
}

bool Layout::SuppressFade()
{
    return g_active;
}

namespace
{
    // Where a remembered panel's box currently sits, rebuilt from its live
    // anchor fractions plus the cached offset.
    void PanelBox(const Panel& p, int screenW, int screenH,
                  float& cx, float& cy)
    {
        cx = screenW * (*p.xFraction) + p.offsetX;
        cy = screenH * (*p.yFraction) + p.offsetY;
    }
}

void Layout::Register(const char* name,
                      const char* section, const char* xKey, const char* yKey,
                      float* xFraction, float* yFraction,
                      float centerX, float centerY, float halfW, float halfH,
                      float* scale, const char* scaleKey,
                      void (*onDragStart)(),
                      const char* scaleSection)
{
    if (!xFraction || !yFraction)
        return;

    // Re-registering an already-known panel updates it in place. Identified
    // by its config-float pointer, which is a stable global, rather than by
    // index or by name string.
    Panel* p = nullptr;
    for (int i = 0; i < g_panelCount; ++i)
    {
        if (g_panels[i].xFraction == xFraction && g_panels[i].yFraction == yFraction)
        {
            p = &g_panels[i];
            break;
        }
    }
    if (!p)
    {
        if (g_panelCount >= kMaxPanels)
            return;
        p = &g_panels[g_panelCount++];
    }

    p->name = name;
    p->section = section;
    p->xKey = xKey;
    p->yKey = yKey;
    p->xFraction = xFraction;
    p->yFraction = yFraction;
    p->halfW = halfW;
    p->halfH = halfH;
    p->scale = scale;
    p->scaleKey = scaleKey;
    p->scaleSection = scaleSection ? scaleSection : section;
    p->onDragStart = onDragStart;
    p->boxW = nullptr;
    p->boxH = nullptr;
    p->wKey = nullptr;
    p->hKey = nullptr;
    p->seenThisFrame = true;
    p->screenW = g_lastScreenW;
    p->screenH = g_lastScreenH;
    // The box relative to the anchor, which is the whole point -- an
    // absolute pixel centre would be meaningless on a frame where the panel
    // is not drawing.
    if (g_lastScreenW > 0 && g_lastScreenH > 0)
    {
        p->offsetX = centerX - g_lastScreenW * (*xFraction);
        p->offsetY = centerY - g_lastScreenH * (*yFraction);
    }
}

void Layout::RegisterBox(const char* name,
                         const char* section, const char* xKey, const char* yKey,
                         float* xFraction, float* yFraction,
                         float centerX, float centerY, float halfW, float halfH,
                         float* widthPx, const char* wKey, float* heightPx, const char* hKey)
{
    Register(name, section, xKey, yKey, xFraction, yFraction, centerX, centerY, halfW, halfH);
    for (int i = 0; i < g_panelCount; ++i)
    {
        if (g_panels[i].xFraction == xFraction && g_panels[i].yFraction == yFraction)
        {
            g_panels[i].boxW = widthPx;
            g_panels[i].boxH = heightPx;
            g_panels[i].wKey = wKey;
            g_panels[i].hKey = hKey;
            break;
        }
    }
}

bool Layout::Active()
{
    return g_active;
}

void Layout::Toggle()
{
    g_active = !g_active;
    if (!g_active)
    {
        // Leaving mid-drag keeps wherever the panel currently sits, but the
        // release that would have saved it never arrives -- so save now.
        if (g_dragX && g_dragY)
        {
            Config::WriteFloat(g_dragSection, g_dragXKey, *g_dragX);
            Config::WriteFloat(g_dragSection, g_dragYKey, *g_dragY);
        }
        if (g_resizeScale)
            Config::WriteFloat(g_resizeSection, g_resizeKey, *g_resizeScale);
        if (g_boxW && g_boxH)
        {
            Config::WriteFloat(g_boxSection, g_boxWKey, *g_boxW);
            Config::WriteFloat(g_boxSection, g_boxHKey, *g_boxH);
        }
        g_dragX = nullptr;
        g_dragY = nullptr;
        g_resizeScale = nullptr;
        g_boxW = nullptr;
        g_boxH = nullptr;
        g_prevButtonDown = false;

        // Every Config::Write* call above (and every one made by a panel's
        // own release-handler during this layout session, e.g. the side bar
        // sliders) only QUEUES the write -- it is not actually on disk until
        // Config::FlushPendingWrites() runs, which otherwise only happens
        // back at the garage/menu (see RealtimeState.h). Layout mode is
        // routinely toggled off while still driving, so without this, a
        // session that ends uncleanly (crash, alt-F4, a plugin reload)
        // between turning layout mode off and next reaching the menu would
        // silently lose every position/size change just made. Forced here
        // regardless of g_inRealtime, since the whole point is not to wait.
        if (!Config::FlushPendingWrites())
            Log::Write("Layout: one or more queued INI writes failed on exiting layout mode (read-only? UAC?)");
    }
}

bool Layout::NudgeFraction(float* fraction, const char* section, const char* key, float delta)
{
    if (!fraction)
        return true;
    *fraction = Clamp01(*fraction + delta);
    return Config::WriteFloat(section, key, *fraction);
}

void Layout::Poll(HWND hwnd, int bbW, int bbH)
{
    // Outside layout mode the mouse is not read at all -- no cursor query, no
    // button query, nothing. This is the whole reason layout mode exists.
    if (!g_active)
    {
        g_prevButtonDown = false;
        g_dragX = nullptr;
        g_dragY = nullptr;
        g_resizeScale = nullptr;
        g_boxW = nullptr;
        g_boxH = nullptr;
        return;
    }

    float cx = 0.0f, cy = 0.0f;
    const bool haveCursor = CursorInBackbuffer(hwnd, bbW, bbH, cx, cy);
    const bool buttonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    if (!haveCursor)
    {
        g_prevButtonDown = buttonDown;
        return;
    }

    const float fx = cx / bbW;
    const float fy = cy / bbH;

    // Handle size in pixels, matching what Draw paints.
    const float handlePx = 22.0f * (static_cast<float>(bbH) / 2160.0f);

    if (buttonDown && !g_prevButtonDown)
    {
        // Corner handles are tested BEFORE panel bodies: a handle sits on its
        // own panel's edge, so body-first would always win and resizing
        // would be unreachable.
        for (int i = 0; i < g_panelCount; ++i)
        {
            Panel& p = g_panels[i];
            if ((!p.scale && !p.boxW) || p.screenW != bbW || p.screenH != bbH) continue;
            float pcx, pcy;
            PanelBox(p, bbW, bbH, pcx, pcy);
            const float hx = pcx + p.halfW;
            const float hy = pcy + p.halfH;
            if (cx < hx - handlePx || cx > hx + handlePx) continue;
            if (cy < hy - handlePx || cy > hy + handlePx) continue;

            if (p.boxW && p.boxH)
            {
                if (p.onDragStart) p.onDragStart();
                g_boxW = p.boxW;
                g_boxH = p.boxH;
                g_boxSection = p.section;
                g_boxWKey = p.wKey;
                g_boxHKey = p.hKey;
                g_boxLeft = pcx - p.halfW;
                g_boxTop = pcy - p.halfH;
                break;
            }

            const float dx = cx - pcx, dy = cy - pcy;
            const float dist = sqrtf(dx * dx + dy * dy);
            if (dist < 1.0f) continue;

            if (p.onDragStart) p.onDragStart();
            g_resizeScale = p.scale;
            g_resizeSection = p.scaleSection;
            g_resizeKey = p.scaleKey;
            g_resizeStartScale = *p.scale;
            g_resizeStartDist = dist;
            g_resizeCenterX = pcx;
            g_resizeCenterY = pcy;
            break;
        }
        if (g_resizeScale || g_boxW)
        {
            g_prevButtonDown = buttonDown;
            return;
        }

        // Press: grab the SMALLEST panel under the cursor. Panels overlap --
        // the clock and the shortcut list sit over the fuel panel, the gear
        // box sits inside the radar's reach -- and the smaller one is
        // invariably the one being aimed at.
        float bestArea = 0.0f;
        Panel* best = nullptr;
        for (int i = 0; i < g_panelCount; ++i)
        {
            Panel& p = g_panels[i];
            if (p.screenW != bbW || p.screenH != bbH) continue; // measured at another resolution
            float pcx, pcy;
            PanelBox(p, bbW, bbH, pcx, pcy);
            if (cx < pcx - p.halfW || cx > pcx + p.halfW) continue;
            if (cy < pcy - p.halfH || cy > pcy + p.halfH) continue;
            const float area = p.halfW * p.halfH;
            if (!best || area < bestArea)
            {
                best = &p;
                bestArea = area;
            }
        }

        if (best)
        {
            if (best->onDragStart) best->onDragStart();
            g_dragX = best->xFraction;
            g_dragY = best->yFraction;
            g_dragSection = best->section;
            g_dragXKey = best->xKey;
            g_dragYKey = best->yKey;
            // The offset between the cursor and the anchor, which is what
            // gets held constant for the rest of the drag.
            g_grabOffsetX = fx - *best->xFraction;
            g_grabOffsetY = fy - *best->yFraction;
        }
    }
    else if (buttonDown && g_resizeScale)
    {
        // Measured against the centre FIXED at grab time (g_resizeCenterX/Y),
        // not re-read from the panel this frame -- see the comment on those
        // globals for why re-deriving a moving centre here fed the box's own
        // growth back into the distance driving that growth.
        const float dx = cx - g_resizeCenterX, dy = cy - g_resizeCenterY;
        const float dist = sqrtf(dx * dx + dy * dy);
        *g_resizeScale = ClampScale(g_resizeStartScale * (dist / g_resizeStartDist));
    }
    else if (buttonDown && g_boxW && g_boxH)
    {
        const float px = static_cast<float>(bbH) / 2160.0f; // sizes are kept at 2160p
        const float w = (cx - g_boxLeft) / px;
        const float h = (cy - g_boxTop) / px;
        *g_boxW = w > kMinBoxPx ? w : kMinBoxPx;
        *g_boxH = h > kMinBoxPx ? h : kMinBoxPx;
    }
    else if (buttonDown && g_dragX && g_dragY)
    {
        *g_dragX = Clamp01(fx - g_grabOffsetX);
        *g_dragY = Clamp01(fy - g_grabOffsetY);
    }
    else if (!buttonDown && g_prevButtonDown && g_resizeScale)
    {
        if (!Config::WriteFloat(g_resizeSection, g_resizeKey, *g_resizeScale))
            Log::Write("Layout: failed to write [%s] %s to the INI", g_resizeSection, g_resizeKey);
        else
            Log::Write("Layout: saved [%s] %s=%.3f", g_resizeSection, g_resizeKey, *g_resizeScale);
        g_resizeScale = nullptr;
    }
    else if (!buttonDown && g_prevButtonDown && g_boxW && g_boxH)
    {
        const bool okW = Config::WriteFloat(g_boxSection, g_boxWKey, *g_boxW);
        const bool okH = Config::WriteFloat(g_boxSection, g_boxHKey, *g_boxH);
        if (!okW || !okH)
            Log::Write("Layout: failed to write [%s] %s/%s to the INI", g_boxSection, g_boxWKey, g_boxHKey);
        g_boxW = nullptr;
        g_boxH = nullptr;
    }
    else if (!buttonDown && g_prevButtonDown && g_dragX && g_dragY)
    {
        // Release: persist once, rather than writing to the INI on every
        // frame of the drag.
        const bool okX = Config::WriteFloat(g_dragSection, g_dragXKey, *g_dragX);
        const bool okY = Config::WriteFloat(g_dragSection, g_dragYKey, *g_dragY);
        if (!okX || !okY)
            Log::Write("Layout: failed to write [%s] %s/%s to the INI (read-only? UAC?)",
                       g_dragSection, g_dragXKey, g_dragYKey);
        else
            Log::Write("Layout: saved [%s] %s=%.4f %s=%.4f",
                       g_dragSection, g_dragXKey, *g_dragX, g_dragYKey, *g_dragY);
        g_dragX = nullptr;
        g_dragY = nullptr;
    }

    g_prevButtonDown = buttonDown;
}

void Layout::Draw(Renderer& renderer, int screenWidth, int screenHeight, bool inCar)
{
    if (!g_active)
        return;

    const LayoutConfig& lcfg = g_layoutConfig;
    const float uiScale = static_cast<float>(screenHeight) / 2160.0f;

    // --- Alignment grid. Minor lines are deliberately faint: at 4K a 100px
    // spacing is already ~38 columns, and anything more prominent reads as a
    // grey wash rather than a grid. The major lines every fifth are what the
    // eye actually counts by, and the centre lines are brightest because
    // "centred" is the alignment asked for most often.
    if (lcfg.gridEnabled && lcfg.gridMinorPx >= 4.0f)
    {
        const float minor = lcfg.gridMinorPx;
        const float major = (lcfg.gridMajorPx >= minor) ? lcfg.gridMajorPx : minor;
        const float thin = 1.0f;
        const float centerXf = screenWidth * 0.5f;
        const float centerYf = screenHeight * 0.5f;

        // Anchored to the screen CENTRE rather than the top-left corner, so
        // there is always a line exactly on centre (odd count either side of
        // it) instead of two edge-anchored grids that only coincide with the
        // separately-drawn centre crosshair when the resolution happens to
        // divide evenly by the spacing -- at a typical 3840px width, half
        // (1920) is not a multiple of 100, so the old edge-anchored grid
        // never actually had a line at the middle at all. Stepping in whole
        // multiples of 'minor' from centre also guarantees every major line
        // is exactly one of the minor positions, so the two always overlap.
        const int majorStep = (minor > 0.0f) ? static_cast<int>(major / minor + 0.5f) : 1;
        const int maxKX = static_cast<int>(centerXf / minor) + 1;
        for (int k = -maxKX; k <= maxKX; ++k)
        {
            const float x = centerXf + static_cast<float>(k) * minor;
            if (x < 0.0f || x > screenWidth) continue;
            const bool isMajor = (majorStep > 0) && (k % majorStep == 0);
            const float a = isMajor ? lcfg.gridMajorAlpha : lcfg.gridMinorAlpha;
            renderer.AddFilledRect(x, centerYf, thin * 0.5f, centerYf, 0.0f,
                                   lcfg.gridR, lcfg.gridG, lcfg.gridB, a);
        }
        const int maxKY = static_cast<int>(centerYf / minor) + 1;
        for (int k = -maxKY; k <= maxKY; ++k)
        {
            const float y = centerYf + static_cast<float>(k) * minor;
            if (y < 0.0f || y > screenHeight) continue;
            const bool isMajor = (majorStep > 0) && (k % majorStep == 0);
            const float a = isMajor ? lcfg.gridMajorAlpha : lcfg.gridMinorAlpha;
            renderer.AddFilledRect(centerXf, y, centerXf, thin * 0.5f, 0.0f,
                                   lcfg.gridR, lcfg.gridG, lcfg.gridB, a);
        }

        // Screen centre, brightest of the three.
        renderer.AddFilledRect(screenWidth * 0.5f, screenHeight * 0.5f, 1.0f, screenHeight * 0.5f, 0.0f,
                               lcfg.gridR, lcfg.gridG, lcfg.gridB, lcfg.gridCenterAlpha);
        renderer.AddFilledRect(screenWidth * 0.5f, screenHeight * 0.5f, screenWidth * 0.5f, 1.0f, 0.0f,
                               lcfg.gridR, lcfg.gridG, lcfg.gridB, lcfg.gridCenterAlpha);
    }

    const float outlinePx = 3.0f * uiScale;
    const float labelPx = 26.0f * uiScale;

    for (int i = 0; i < g_panelCount; ++i)
    {
        const Panel& p = g_panels[i];
        if (p.screenW != screenWidth || p.screenH != screenHeight)
            continue; // box was measured at a different resolution

        float cx, cy;
        PanelBox(p, screenWidth, screenHeight, cx, cy);

        const bool dragging = (p.xFraction == g_dragX);
        // Three states, so it is obvious what you are holding and what is
        // only being remembered: amber = dragging, white = drawing right
        // now, dim blue = remembered from earlier (hidden or out of
        // realtime), which is still perfectly draggable.
        float r, g, b;
        if (dragging)            { r = 1.0f;  g = 0.85f; b = 0.20f; }
        else if (p.seenThisFrame){ r = 0.35f; g = 0.85f; b = 1.00f; }
        else                     { r = 0.45f; g = 0.45f; b = 0.60f; }

        renderer.AddFilledRect(cx, cy - p.halfH, p.halfW, outlinePx * 0.5f, 0.0f, r, g, b, 0.9f);
        renderer.AddFilledRect(cx, cy + p.halfH, p.halfW, outlinePx * 0.5f, 0.0f, r, g, b, 0.9f);
        renderer.AddFilledRect(cx - p.halfW, cy, outlinePx * 0.5f, p.halfH, 0.0f, r, g, b, 0.9f);
        renderer.AddFilledRect(cx + p.halfW, cy, outlinePx * 0.5f, p.halfH, 0.0f, r, g, b, 0.9f);

        renderer.AddText(cx, cy - p.halfH - labelPx * 1.2f, p.name, labelPx,
                         r, g, b, 1.0f, Renderer::HAlign::Center);

        // Resize handle, bottom-right. Solid rather than an outline so it
        // reads as a grabbable thing rather than more panel edge.
        if (p.scale || p.boxW)
        {
            const float handlePx = 22.0f * uiScale;
            const bool resizing = p.scale ? (p.scale == g_resizeScale) : (p.boxW == g_boxW);
            const float hr = resizing ? 1.0f : r;
            const float hg = resizing ? 0.85f : g;
            const float hb = resizing ? 0.20f : b;
            renderer.AddFilledRect(cx + p.halfW, cy + p.halfH, handlePx * 0.5f, handlePx * 0.5f, 0.0f,
                                   hr, hg, hb, 0.95f);
            if (resizing)
            {
                char sc[32];
                if (p.scale)
                    sprintf_s(sc, "%.2fx", *p.scale);
                else
                    sprintf_s(sc, "%.0f x %.0f", *p.boxW, *p.boxH);
                renderer.AddText(cx + p.halfW, cy + p.halfH + handlePx, sc, labelPx,
                                 hr, hg, hb, 1.0f, Renderer::HAlign::Center);
            }
        }

        // The exact fractions, on the panel being dragged -- these are the
        // numbers going into the INI, so matching one panel to another (or
        // typing an exact value in later) does not have to be done by eye.
        if (dragging)
        {
            char pos[48];
            sprintf_s(pos, "x %.4f   y %.4f", *p.xFraction, *p.yFraction);
            renderer.AddText(cx, cy + p.halfH + labelPx * 0.3f, pos, labelPx,
                             r, g, b, 1.0f, Renderer::HAlign::Center);
        }
    }

    // Banner, top centre -- deliberately loud, since layout mode changes what
    // a mouse click means and must never be left on by accident.
    const float bannerPx = 40.0f * uiScale;
    const float hintPx = 30.0f * uiScale;
    // Built from the LIVE key binding rather than a fixed string -- this
    // used to say "Ctrl+Shift+5" unconditionally, which went stale the
    // first time that binding was ever changed in the INI (it did, when the
    // key moved to make room for List/LayoutMode at the front).
    char layoutKeyName[16];
    Config::KeyName(g_shortcutConfig.layoutMode, layoutKeyName, sizeof(layoutKeyName));
    char banner[128];
    sprintf_s(banner, "LAYOUT MODE -- drag a panel to move it, its corner square to resize. Ctrl+Shift+%s to exit.",
              layoutKeyName);

    // Read live rather than assumed: this used to hardcode "F9", which was
    // simply wrong the moment a driver rebound rF2's own Toggle Cursor
    // control away from its default (see Config::ReadToggleCursorKeyName).
    char cursorKeyName[16];
    Config::ReadToggleCursorKeyName(cursorKeyName, sizeof(cursorKeyName));
    char hint2Buf[96];
    sprintf_s(hint2Buf, "Get in the car and press %s (rF2 Controls: \"Toggle Cursor\") to free the mouse.",
              cursorKeyName);

    // Layout mode is really meant to be used FROM THE COCKPIT -- lining
    // panels up against the car's own dash, mirrors and HUD only means
    // something once you can see them, and the boxes shown out of the car
    // are just remembered positions, not a preview of what you'll see while
    // driving. Say both things out of the car; once in the car, just remind
    // how to get the mouse cursor so a panel can actually be dragged.
    const char* hint1 = "Not in the car -- these are remembered positions. Layout mode works best while driving.";
    const char* hint2 = hint2Buf;
    char hint3Buf[80];
    sprintf_s(hint3Buf, "Toggle Cursor (%s) to show/hide the mouse for dragging.", cursorKeyName);
    const char* hint3 = hint3Buf;

    float bannerW = renderer.MeasureText(banner, bannerPx);
    {
        const float w1 = inCar ? 0.0f : renderer.MeasureText(hint1, hintPx);
        const float w2 = inCar ? 0.0f : renderer.MeasureText(hint2, hintPx);
        const float w3 = inCar ? renderer.MeasureText(hint3, hintPx) : 0.0f;
        if (w1 > bannerW) bannerW = w1;
        if (w2 > bannerW) bannerW = w2;
        if (w3 > bannerW) bannerW = w3;
    }

    const float lineStep = hintPx * 1.35f;
    const float blockH = bannerPx + lineStep * (inCar ? 1.0f : 2.0f);
    const float bannerX = screenWidth * lcfg.bannerPosXFraction;
    const float bannerY = screenHeight * lcfg.bannerPosYFraction;
    const float pad = 20.0f * uiScale;
    Register("Layout banner", "Layout", "BannerPosXFraction", "BannerPosYFraction",
             &g_layoutConfig.bannerPosXFraction, &g_layoutConfig.bannerPosYFraction,
             bannerX, bannerY + blockH * 0.5f, bannerW * 0.5f + pad, blockH * 0.5f + pad);

    renderer.AddFilledRoundedRect(bannerX, bannerY + blockH * 0.5f,
                                  bannerW * 0.5f + pad, blockH * 0.5f + pad,
                                  10.0f * uiScale, 0.0f, 0.08f, 0.08f, 0.08f, 0.6f);
    renderer.AddText(bannerX, bannerY, banner, bannerPx,
                     1.0f, 0.85f, 0.2f, 1.0f, Renderer::HAlign::Center);
    if (!inCar)
    {
        renderer.AddText(bannerX, bannerY + bannerPx + lineStep * 0.0f, hint1, hintPx,
                         1.0f, 0.6f, 0.3f, 1.0f, Renderer::HAlign::Center);
        renderer.AddText(bannerX, bannerY + bannerPx + lineStep * 1.0f, hint2, hintPx,
                         1.0f, 0.6f, 0.3f, 1.0f, Renderer::HAlign::Center);
    }
    else
    {
        renderer.AddText(bannerX, bannerY + bannerPx + lineStep * 0.0f, hint3, hintPx,
                         1.0f, 0.6f, 0.3f, 1.0f, Renderer::HAlign::Center);
    }
}
