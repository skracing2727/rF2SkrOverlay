#include "Shortcuts.h"
#include "SetupInfo.h"
#include "UpdateCheck.h"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "LapDelta.h"
#include "MiniSector.h"
#include "FuelTyrePanel.h"
#include "Log.h"
#include "Version.h"

#include "TrackMap.h"
#include "RealtimeState.h"
#include "GearDisplay.h"
#include <Windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#include <vector>
#include <algorithm>
#include <atomic>
extern std::atomic<bool> g_resetTimingRequested;   // Plugin.cpp
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    struct Entry
    {
        const char* label;   // shown in the on-screen list
        const char* iniKey;  // Show<Element> key this persists to
        int key;             // virtual-key code, filled from config each poll
        bool visible;
    };

    // Order here is the order the on-screen list reads in -- grouped roughly
    // by what they are for (safety, driving reference, timing, housekeeping)
    // rather than alphabetically, so the list is scannable.
    Entry g_entries[Shortcuts::ElementCount] =
    {
        { "Radar",                 "ShowRadar",          'R', true },
        { "Pedal trace",           "ShowPedalTrace",     'P', true },
        { "Frametime stats",       "ShowPerf",           'F', true },
        { "Gear + speed",          "ShowGear",           'G', true },
        { "Delta bar (session)",   "ShowLapDelta",       'D', true },
        { "Delta bar (best)",      "ShowBestDelta",      'A', true },
        { "Delta-speed strip",     "ShowMiniSector",     'M', false },
        { "Relative timing",       "ShowRelativeTiming", 'T', true },
        { "Fuel + tyres",          "ShowFuelTyre",       'U', true },
        { "Brake cue + beep",      "ShowBrakeCue",       'B', false },

        { "DRS sign + beep",       "ShowDrs",            'D', true },
        { "Clock",                 "ShowClock",          'C', true },
        { "Side proximity bars",   "ShowSideBar",        'S', true },
        { "Fuel box",              "ShowFuelPanel",      'N', true },
        { "Tyres box",             "ShowTyresPanel",     'Y', true },
        { "Brake bias box",        "ShowBrakeBiasPanel", 'K', true },
        { "Tyre lockup box",       "ShowTyreLockup",     'L', true },
        { "Shift stats",           "ShowShiftStats",     'V', true },
        { "Tyre temps (corner)",   "ShowTyreTemps",      'W', true },
        { "FFB spectrum",          "ShowFfbGraph",       0,   true },
        { "Track map",             "ShowTrackMap",       0,   true },
        { "DLL cost (by module)",  "ShowDllCost",        0,   false },
        { "Cover boxes",           "ShowCoverBoxes",     0,   false },
    };

    // Edge state for clicking a row in the list. Held here rather than in
    // Layout because this is a different gesture on a different surface --
    // Layout's own button state belongs to dragging panels.
    bool g_listClickDown = false;
    // Moving / resizing the list itself while it is open (no layout mode
    // needed): drag the title strip to move it, any corner square to resize
    // it with the opposite corner held in place.
    bool g_listMoving = false;
    float g_listGrabDX = 0.0f, g_listGrabDY = 0.0f;
    bool g_listResizing = false;
    float g_listAnchorX = 0.0f, g_listAnchorY = 0.0f, g_listSideX = 0.0f, g_listSideY = 0.0f;
    float g_listStartDist = 1.0f, g_listStartScale = 1.0f;

    bool g_listOpen = false;      // the list is a toggle; it also auto-closes
    // Drag state for the relative-timing "cars shown" slider in the list --
    // same absolute-position-slider convention ProximityHue.cpp's own
    // sliders use, held here since this is a different surface/gesture.
    bool g_carsSliderDragging = false;

    // Shortcut-list scrolling, for when the list is taller than the screen
    // below it. Scrolls by whole rows (the first row shown), since the
    // renderer has no clipping -- a row is either drawn whole or not at all.
    // Driven by a draggable bar (no mouse wheel: that would need the game's
    // window procedure hooked). Kept across opens, clamped every frame.
    int g_listFirstRow = 0;
    bool g_listScrollDragging = false;
    bool g_mapRotDragging = false; // the track map rotation slider
    bool g_mapSectorsDragging = false; // the track map mini-sector count slider
    bool g_coverGreyDragging = false;   // the cover box shade slider
    bool g_coverAlphaDragging = false;  // the cover box opacity slider
    bool g_beepDropDragging = false;    // the brake beep minimum speed drop slider
    float g_listScrollGrabPx = 0.0f; // cursor offset from the thumb's top while dragging
    // The input thread (JoyThreadProc): wheel buttons (the three rotaries of
    // the rotary menu and the track map colour mode buttons), the keyboard
    // hotkeys, the key presses the rotary menu sends, and rF2's Toggle
    // Cursor binding. Wheel buttons are read through DirectInput, which sees
    // all 128 buttons of a device. Polling devices is slow (a fraction of a
    // ms each), so it runs on its own thread at ~100 Hz -- fast enough for
    // an encoder's click pulse -- and hands over the counted clicks.
    HANDLE g_joyThread = nullptr;
    std::atomic<bool> g_joyStop{ false };
    // Track map colour mode buttons (TrackMapConfig::deltaPrevButton /
    // deltaNextButton): +1 per next press, -1 per previous, applied in Poll.
    std::atomic<int> g_mapModeSteps{ 0 };
    // Centre rotary (+1 next / -1 prev) over the on/off list, and presses of
    // the on/off button. Also fed by Home/End.
    std::atomic<int> g_elemSteps{ 0 };
    std::atomic<int> g_elemToggles{ 0 };
    // Per side rotary (0 = left, 1 = right): setting-pick clicks and up/down
    // presses. The picks are also fed by Insert/Delete and Page Up/Page Down.
    std::atomic<int> g_slotSel[2] = { 0, 0 };
    std::atomic<int> g_slotAdj[2] = { 0, 0 };

    // The order both the mouse list and the centre rotary go through, in
    // groups. >= 0 and < ElementCount: a panel; >= 1000: an extra on/off row
    // (custom row id = code - 1000; 26 = reset timing tower, an action);
    // < 0: a group heading (-1 - index into kGroupName), shown in the mouse
    // list and skipped by the rotary.
    const char* const kGroupName[] = { "DRIVING", "TIMING", "FUEL, TYRES & BRAKES", "CAR & FFB", "TRACK & WEATHER", "SCREEN" };
    // "Escape": first entry of the centre rotary's list (not in the mouse
    // list). Toggling it presses Esc in rF2 -- closes the chat box (which
    // otherwise swallows the rotary keys as typed text) or backs out to the
    // garage -- and hides the rotary list.
    const int kCloseEntry = 9999;
    const int kListOrder[] = {
        kCloseEntry,
        -1, Shortcuts::Radar, Shortcuts::SideBar, Shortcuts::Gear, Shortcuts::ShiftStats, Shortcuts::TyreLockup,
            Shortcuts::Drs, Shortcuts::BrakeCue, Shortcuts::PedalTrace,
        -2, Shortcuts::LapDelta, Shortcuts::BestDelta, Shortcuts::MiniSector, Shortcuts::RelativeTiming,
            1028, 1027, 1025, 1026,
        -3, Shortcuts::FuelPanel, 1019, Shortcuts::TyresPanel, 1020, Shortcuts::TyreTemps, Shortcuts::BrakeBiasPanel,
        -4, 1021, 1022, Shortcuts::FfbGraph,
        -5, Shortcuts::TrackMap, 1023, 1024,
        -6, Shortcuts::Clock, Shortcuts::CoverBoxes, Shortcuts::Perf, Shortcuts::DllCost, 1029,
    };
    const int kListOrderCount = static_cast<int>(sizeof(kListOrder) / sizeof(kListOrder[0]));
    const int kResetTowerCode = 1026;
    const int kUpdateCheckCode = 1029;
    // Rotary position: an index into kListOrder, always on an entry (never a heading).
    int g_elemIndex = 1;

    // A 10-line window over a wrapping list for the rotary boxes: the
    // highlight moves, the window stays put until the highlight reaches the
    // last line at the top or bottom, then the window jumps 5 lines that
    // way (animated) so the highlight is back near the middle.
    struct ScrollWin
    {
        static const int kRows = 10;
        long long top = 0;          // unbounded; item = top mod count
        float anim = 0.0f;          // drawn top, easing towards 'top'
        bool init = false;
        LARGE_INTEGER last = {};
        void Update(int sel, int count)
        {
            LARGE_INTEGER now, freq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            const float dt = last.QuadPart ? static_cast<float>(now.QuadPart - last.QuadPart) / static_cast<float>(freq.QuadPart) : 0.0f;
            last = now;
            if (count <= kRows) { top = 0; anim = 0.0f; init = true; return; }
            const int rr = static_cast<int>(((sel - top) % count + count) % count);
            if (!init || rr >= kRows) { top = sel - kRows / 2; anim = static_cast<float>(top); init = true; }
            // Jump when the highlight steps onto the last (or first) line.
            // Half a window, so the highlight lands back in the middle.
            else if (rr >= kRows - 1) top += kRows / 2;
            else if (rr <= 0) top -= kRows / 2;
            const float k = (std::min)(1.0f, dt * 14.0f);   // ~0.15 s slide
            anim += (static_cast<float>(top) - anim) * k;
            if (fabsf(anim - static_cast<float>(top)) < 0.002f) anim = static_cast<float>(top);
        }
    };
    ScrollWin g_elemWin, g_knobWin;

    // Cuts 's' down (ending "..") until it fits maxW at this text size.
    void FitText(const Renderer& renderer, char* s, size_t size, float maxW, float px)
    {
        if (renderer.MeasureText(s, px) <= maxW)
            return;
        char orig[160];
        strncpy_s(orig, s, _TRUNCATE);
        for (int n = static_cast<int>(strlen(orig)) - 1; n > 0; --n)
        {
            sprintf_s(s, size, "%.*s..", n, orig);
            if (renderer.MeasureText(s, px) <= maxW)
                return;
        }
    }

    // Draws a ScrollWin list in a box of FIXED width -- sized from every
    // item's name, so it does not move or resize as the selection changes.
    // Rows are numbered and left-aligned (" 1." .. "19.", so the names line
    // up); an item's value, if any, is right-aligned in its own column.
    // leftSide: the box's left edge is at edgeX (else its right edge).
    // label(i, name, value, r, g, b) fills item i's name (128) and value (64,
    // may be empty) and its colour.
    // A row's icon: (renderer, item, centre x, centre y, half-size, r, g, b, alpha).
    typedef void (*RowIconFn)(Renderer&, int, float, float, float, float, float, float, float);
    template <typename LabelFn>
    void DrawScrollList(Renderer& renderer, ScrollWin& win, int count, int sel, bool leftSide, float edgeX,
                        float centerY, float textPx, float s, float alpha, LabelFn label, const char* caption = nullptr,
                        RowIconFn icon = nullptr)
    {
        const float iconW = icon ? textPx * 1.4f : 0.0f;
        win.Update(sel, count);
        const int rows = (std::min)(count, ScrollWin::kRows);
        const float rowStep = textPx * 1.35f;
        const float padding = 14.0f * s;
        const float colGap = textPx * 1.0f;
        char name[128], value[64], numbered[160];
        float r, g, b;
        float nameW = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            label(i, name, value, r, g, b);
            sprintf_s(numbered, "%2d. %s", i + 1, name);
            nameW = (std::max)(nameW, renderer.MeasureText(numbered, textPx) + iconW);
        }
        const float valueW = renderer.MeasureText("0000000000000000", textPx);   // 16 characters
        const float boxW = nameW + colGap + valueW + 2.0f * padding;
        const float contentH = rows * rowStep;
        const float halfH = contentH * 0.5f + padding;
        const float left = leftSide ? edgeX : edgeX - boxW;
        const float cx = left + boxW * 0.5f;
        renderer.AddFilledRoundedRect(cx, centerY, boxW * 0.5f, halfH, 10.0f * s, 0.0f, 0.08f, 0.08f, 0.08f, 0.6f * alpha);
        if (caption && caption[0])
        {
            // Just above the box, on its own small dark strip.
            const float capPx = textPx * 0.75f;
            const float capW = renderer.MeasureText(caption, capPx);
            const float capY = centerY - halfH - capPx * 1.6f;
            renderer.AddFilledRoundedRect(cx, capY + capPx * 0.5f, capW * 0.5f + padding, capPx * 0.5f + padding * 0.4f,
                                          6.0f * s, 0.0f, 0.08f, 0.08f, 0.08f, 0.6f * alpha);
            renderer.AddText(cx, capY, caption, capPx, 0.85f, 0.85f, 0.85f, alpha, Renderer::HAlign::Center);
        }
        const float top = centerY - contentH * 0.5f;
        const float base = floorf(win.anim);
        const float frac = win.anim - base;
        for (int k = -1; k <= rows; ++k)
        {
            const float slot = static_cast<float>(k) - frac;          // 0..rows-1 = fully inside
            const float vis = (std::min)((std::min)(slot + 1.0f, static_cast<float>(rows) - slot), 1.0f);
            if (vis <= 0.0f) continue;
            const long long item = static_cast<long long>(base) + k;
            const int i = static_cast<int>((item % count + count) % count);
            const float y = top + slot * rowStep;
            const bool selected = i == sel;
            if (selected)
                renderer.AddFilledRect(cx, y + textPx * 0.5f, boxW * 0.5f - padding * 0.5f, rowStep * 0.5f, 0.0f,
                                       1.0f, 1.0f, 1.0f, 0.18f * alpha * vis);
            label(i, name, value, r, g, b);
            sprintf_s(numbered, "%2d. %s", i + 1, name);
            const float a = (selected ? 1.0f : 0.7f) * alpha * vis;
            if (icon)
            {
                // After the number, before the name.
                const float numW = renderer.MeasureText("00. ", textPx);
                icon(renderer, i, left + padding + numW + textPx * 0.5f, y + textPx * 0.5f, textPx * 0.42f, r, g, b, a);
                char num[8];
                sprintf_s(num, "%2d. ", i + 1);
                renderer.AddText(left + padding, y, num, textPx, r, g, b, a, Renderer::HAlign::Left);
                renderer.AddText(left + padding + numW + iconW, y, name, textPx, r, g, b, a, Renderer::HAlign::Left);
            }
            else
                renderer.AddText(left + padding, y, numbered, textPx, r, g, b, a, Renderer::HAlign::Left);
            if (value[0])
            {
                FitText(renderer, value, sizeof(value), valueW, textPx);
                renderer.AddText(left + boxW - padding, y, value, textPx, r, g, b, a, Renderer::HAlign::Right);
            }
        }
    }

    int StepListOrder(int pos, int steps)
    {
        const int dir = steps > 0 ? 1 : -1;
        for (int k = 0; k < (steps > 0 ? steps : -steps); ++k)
            do { pos = ((pos + dir) % kListOrderCount + kListOrderCount) % kListOrderCount; }
            while (kListOrder[pos] < 0);
        return pos;
    }
    LARGE_INTEGER g_elemSelTime = {};   // last centre-rotary/on-off use; 0 = never

    // Which setting each side rotary changes. No braking-aid settings here.
    // In list order, grouped (2026-11-23): brakes and chassis, engine and TC,
    // race strategy, HUD, cover boxes, driving position, lights.
    enum { kKnobAbs, kKnobBias, kKnobFrontArb, kKnobRearArb,
           kKnobTcLevel, kKnobTcCut, kKnobTcSlip, kKnobMixture, kKnobShiftTarget,
           kKnobFuelStint, kKnobTyreWear, kKnobTyreTime,
           kKnobPitLimit, kKnobMapColour,
           kKnobCoverOpacity, kKnobCoverShade,
           kKnobFov, kKnobSeatX, kKnobSeatY,
           kKnobHeadlights, kKnobSettingCount };
    const char* const kKnobSettingName[kKnobSettingCount] = {
        "ABS", "Brake bias", "Front anti-roll bar", "Rear anti-roll bar",
        "Traction control", "TC power cut", "TC slip", "Engine mixture", "Shift point",
        "Fuel stint length", "Tyre max wear", "Tyre target time",
        "Pit lane limit", "Track map colour",
        "Cover box opacity", "Cover box shade",
        "Field of view", "Seat fore/aft", "Seat height",
        "Headlights" };
    enum { kGroupChassis, kGroupEngine, kGroupStrategy, kGroupHud, kGroupCover, kGroupSeat, kGroupLights };
    const int kKnobGroup[kKnobSettingCount] = {
        kGroupChassis, kGroupChassis, kGroupChassis, kGroupChassis,
        kGroupEngine, kGroupEngine, kGroupEngine, kGroupEngine, kGroupEngine,
        kGroupStrategy, kGroupStrategy, kGroupStrategy,
        kGroupHud, kGroupHud,
        kGroupCover, kGroupCover,
        kGroupSeat, kGroupSeat, kGroupSeat,
        kGroupLights };
    // One colour per group: brakes/chassis red, engine/TC orange, strategy
    // green, HUD violet, cover boxes grey, driving position blue, lights yellow.
    void KnobGroupColour(int k, float& r, float& g, float& b)
    {
        static const float kCol[7][3] = {
            { 1.0f, 0.45f, 0.45f }, { 1.0f, 0.62f, 0.2f }, { 0.45f, 0.95f, 0.5f }, { 0.8f, 0.6f, 1.0f },
            { 0.75f, 0.75f, 0.75f }, { 0.45f, 0.72f, 1.0f }, { 1.0f, 0.9f, 0.35f } };
        const float* c = kCol[kKnobGroup[k]];
        r = c[0]; g = c[1]; b = c[2];
    }

    // Small line-drawing icon for each knob setting, in its group colour,
    // drawn in the lists and the knob box (2026-11-24). (cx, cy) centre,
    // h half-size. Shapes only -- no image files.
    enum KnobIcon { IcAbs, IcBias, IcArbFront, IcArbRear, IcTc, IcTcCut, IcTcSlip, IcMixture, IcShift,
                    IcFuel, IcTyreWear, IcTyreTime, IcPitLimit, IcWarning, IcRedFrom, IcMap,
                    IcCoverOpacity, IcCoverShade, IcFov, IcSeatX, IcSeatY, IcLights };
    int KnobIconFor(int k)
    {
        switch (k)
        {
        case kKnobAbs: return IcAbs;
        case kKnobBias: return IcBias;
        case kKnobFrontArb: return IcArbFront;
        case kKnobRearArb: return IcArbRear;
        case kKnobTcLevel: return IcTc;
        case kKnobTcCut: return IcTcCut;
        case kKnobTcSlip: return IcTcSlip;
        case kKnobMixture: return IcMixture;
        case kKnobShiftTarget: return IcShift;
        case kKnobFuelStint: return IcFuel;
        case kKnobTyreWear: return IcTyreWear;
        case kKnobTyreTime: return IcTyreTime;
        case kKnobPitLimit: return IcPitLimit;
        case kKnobMapColour: return IcMap;
        case kKnobCoverOpacity: return IcCoverOpacity;
        case kKnobCoverShade: return IcCoverShade;
        case kKnobFov: return IcFov;
        case kKnobSeatX: return IcSeatX;
        case kKnobSeatY: return IcSeatY;
        default: return IcLights;
        }
    }
    void DrawKnobIcon(Renderer& rd, int k, float cx, float cy, float h, float r, float g, float b, float a)
    {
        const float th = (std::max)(1.5f, h * 0.16f);
        auto line = [&](float x1, float y1, float x2, float y2, float w = 0.0f)
        { rd.AddLineSmooth(cx + x1 * h, cy + y1 * h, cx + x2 * h, cy + y2 * h, w > 0.0f ? w : th, r, g, b, a); };
        auto tyre = [&](float x, float y, float rad)
        { rd.AddRing(cx + x * h, cy + y * h, rad * h * 0.62f, rad * h, r, g, b, a, 24); };
        auto ring = [&](float x, float y, float rad)
        { rd.AddRing(cx + x * h, cy + y * h, rad * h - th, rad * h, r, g, b, a, 24); };
        auto dot = [&](float x, float y, float rad, float alpha = 1.0f)
        { rd.AddFilledCircle(cx + x * h, cy + y * h, rad * h, r, g, b, a * alpha, 16); };
        auto box = [&](float x, float y, float hw, float hh, float alpha)
        { rd.AddFilledRect(cx + x * h, cy + y * h, hw * h, hh * h, 0.0f, r, g, b, a * alpha); };
        auto frame = [&](float x, float y, float hw, float hh)
        { rd.AddRectOutline(cx + x * h, cy + y * h, hw * h, hh * h, th, r, g, b, a); };
        auto poly = [&](const float* pts, int n, bool closed)
        {
            float xy[32];
            for (int i = 0; i < n && i < 16; ++i) { xy[i * 2] = cx + pts[i * 2] * h; xy[i * 2 + 1] = cy + pts[i * 2 + 1] * h; }
            rd.AddPolylineSmooth(xy, n, closed, th, r, g, b, a);
        };
        auto label = [&](const char* t, float px)
        {
            const bool bg = rd.TextBackgroundEnabled();
            rd.SetTextBackground(false);
            rd.AddText(cx, cy - px * h * 0.5f, t, px * h, r, g, b, a, Renderer::HAlign::Center);
            rd.SetTextBackground(bg);
        };
        switch (KnobIconFor(k))
        {
        case IcAbs:          tyre(0, 0, 0.8f); line(-1.0f, -0.5f, -1.0f, 0.5f); line(1.0f, -0.5f, 1.0f, 0.5f); break;
        case IcBias:         line(-0.9f, 0.15f, 0.9f, -0.15f); dot(-0.75f, -0.15f, 0.32f); dot(0.75f, -0.4f, 0.2f);
                             { const float t[] = { -0.25f, 0.85f, 0.25f, 0.85f, 0.0f, 0.2f }; poly(t, 3, true); } break;
        case IcArbFront:
        case IcArbRear:
        {
            const float y = KnobIconFor(k) == IcArbFront ? -0.55f : 0.55f;
            frame(0, 0, 0.45f, 0.9f);
            line(-0.95f, y, 0.95f, y, th * 1.6f); dot(-0.95f, y, 0.16f); dot(0.95f, y, 0.16f);
            break;
        }
        case IcTc:           tyre(0.25f, 0, 0.75f); line(-1.0f, -0.35f, -0.6f, -0.35f); line(-1.0f, 0.0f, -0.55f, 0.0f); line(-1.0f, 0.35f, -0.6f, 0.35f); break;
        case IcTcCut:        tyre(0, 0, 0.8f); line(-0.85f, 0.85f, 0.85f, -0.85f); break;
        case IcTcSlip:       tyre(0, -0.2f, 0.65f); line(-0.95f, 0.75f, -0.15f, 0.75f); line(0.15f, 0.75f, 0.95f, 0.75f); break;
        case IcMixture:      dot(0, 0.3f, 0.55f); { const float t[] = { -0.5f, 0.12f, 0.0f, -0.9f, 0.5f, 0.12f }; poly(t, 3, true); }
                             rd.AddFilledRect(cx, cy - 0.15f * h, 0.3f * h, 0.3f * h, 0.785f, r, g, b, a); break;
        case IcShift:
        {
            float arc[16];
            for (int i = 0; i < 8; ++i)
            {
                const float ang = 3.6652f - i * (3.6652f - (-0.5236f)) / 7.0f;   // 210 deg .. -30 deg
                arc[i * 2] = cosf(ang) * 0.9f; arc[i * 2 + 1] = -sinf(ang) * 0.9f + 0.2f;
            }
            poly(arc, 8, false);
            line(0, 0.2f, 0.5f, -0.45f); dot(0, 0.2f, 0.16f);
            break;
        }
        case IcFuel:         frame(-0.15f, 0.1f, 0.5f, 0.75f); box(-0.15f, 0.35f, 0.5f, 0.5f, 0.45f);
                             line(0.35f, -0.35f, 0.8f, -0.1f); line(0.8f, -0.1f, 0.8f, 0.6f); break;
        case IcTyreWear:     tyre(0, -0.15f, 0.7f); box(0, 0.8f, 0.85f, 0.08f, 1.0f); break;
        case IcTyreTime:     ring(0, 0, 0.9f); line(0, 0, 0, -0.6f); line(0, 0, 0.45f, 0.15f); break;
        case IcPitLimit:     ring(0, 0, 0.95f); label("P", 1.15f); break;
        case IcWarning:      { const float t[] = { 0.0f, -0.9f, 0.95f, 0.8f, -0.95f, 0.8f }; poly(t, 3, true); }
                             line(0, -0.35f, 0, 0.25f); dot(0, 0.5f, 0.1f); break;
        case IcRedFrom:      box(0, 0, 0.9f, 0.55f, 0.45f); frame(0, 0, 0.9f, 0.55f); break;
        case IcMap:          { const float t[] = { -0.8f, -0.3f, -0.3f, -0.8f, 0.4f, -0.6f, 0.9f, -0.1f,
                                                  0.6f, 0.5f, 0.0f, 0.3f, -0.4f, 0.8f, -0.9f, 0.4f }; poly(t, 8, true); } break;
        case IcCoverOpacity: frame(-0.25f, -0.25f, 0.6f, 0.6f); box(0.25f, 0.25f, 0.6f, 0.6f, 0.45f); break;
        case IcCoverShade:   frame(0, 0, 0.8f, 0.8f); box(-0.4f, 0, 0.4f, 0.8f, 0.8f); break;
        case IcFov:          line(-0.9f, 0, 0.8f, -0.75f); line(-0.9f, 0, 0.8f, 0.75f);
                             { const float t[] = { 0.65f, -0.65f, 0.9f, 0.0f, 0.65f, 0.65f }; poly(t, 3, false); } dot(-0.9f, 0, 0.16f); break;
        case IcSeatX:        line(-0.55f, -0.85f, -0.35f, 0.2f); line(-0.35f, 0.2f, 0.55f, 0.2f);
                             line(-0.8f, 0.7f, 0.8f, 0.7f); line(0.8f, 0.7f, 0.55f, 0.5f); line(-0.8f, 0.7f, -0.55f, 0.5f); break;
        case IcSeatY:        line(-0.75f, -0.75f, -0.55f, 0.3f); line(-0.55f, 0.3f, 0.25f, 0.3f);
                             line(0.75f, -0.85f, 0.75f, 0.85f); line(0.75f, -0.85f, 0.55f, -0.6f); line(0.75f, -0.85f, 0.95f, -0.6f);
                             line(0.75f, 0.85f, 0.55f, 0.6f); line(0.75f, 0.85f, 0.95f, 0.6f); break;
        default:             dot(-0.45f, 0, 0.45f); line(0.15f, -0.5f, 0.95f, -0.65f); line(0.15f, 0.0f, 0.95f, 0.0f); line(0.15f, 0.5f, 0.95f, 0.65f); break;
        }
    }
    int g_knobSetting = kKnobTcCut;        // the active rotary's setting (for drawing)
    int g_knobSel[2] = { kKnobTcCut, kKnobFuelStint }; // left / right rotary's setting
    int g_activeSlot = 0;                  // rotary used last
    LARGE_INTEGER g_knobSelTime = {};      // last setting pick; 0 = never

    bool EntryVisible(int e);   // defined after g_entries' helpers below

    // A setting is offered only while the thing it belongs to is switched
    // on; car controls are always offered.
    bool KnobAvailable(int k)
    {
        switch (k)
        {
        case kKnobFuelStint:    return g_fuelSaveConfig.enabled;
        case kKnobShiftTarget:  return g_entries[Shortcuts::Gear].visible;
        case kKnobTyreWear:
        case kKnobTyreTime:     return g_tyreSaveConfig.enabled;
        case kKnobPitLimit:     return g_entries[Shortcuts::Gear].visible;
        case kKnobMapColour:    return EntryVisible(Shortcuts::TrackMap);
        case kKnobCoverOpacity:
        case kKnobCoverShade:   return EntryVisible(Shortcuts::CoverBoxes);
        default:                return true;
        }
    }
    int StepKnob(int k, int dir)
    {
        for (int i = 0; i < kKnobSettingCount; ++i)
        {
            k = ((k + dir) % kKnobSettingCount + kKnobSettingCount) % kKnobSettingCount;
            if (KnobAvailable(k)) return k;
        }
        return k;
    }

    const char* MapModeName(int mode)
    {
        return mode == 1 ? "vs my personal best" : mode == 2 ? "vs my session best"
             : mode == 3 ? "vs session fastest (anyone)" : "off";
    }

    // Settings with no telemetry value to set (FOV, mixture, TC, ARB, ABS,
    // seat, headlights) press rF2's own controls instead: a keyboard key
    // (DirectInput scan code from ShortcutConfig) sent with SendInput, which
    // you bind to that control once in rF2. One queue serves them all, so
    // presses go out one at a time in the order they were asked for.
    //
    // Poll only QUEUES the presses; the input thread (JoyThreadProc) sends
    // them. SendInput does not return until every low-level keyboard hook on
    // the system has seen the key (SimHub, Discord, ...), and a slow one
    // held the render thread up for milliseconds.
    enum { kKeyFov, kKeyMixture, kKeyTc, kKeyTcCut, kKeyTcSlip, kKeyHeadlights, kKeyFrontArb, kKeyRearArb, kKeyAbs, kKeyBias, kKeySeatX, kKeySeatY, kKeyEscape, kKeyCount };
    struct KeyPress { int which; bool up; };
    // Filled by Poll (render thread), emptied by PumpKeyPresses (input
    // thread). The lock is only held to add or take an entry.
    SRWLOCK g_keyQueueLock = SRWLOCK_INIT;
    std::vector<KeyPress> g_keyQueue;
    WORD g_heldScan = 0;        // key currently held down; 0 = none (PumpKeyPresses only)
    int g_keyNetSteps[kKeyCount] = {}; // net steps sent this session, for the box (render thread)
    // rF2's Toggle Cursor key, to be tapped once (down and up together) for
    // turning layout mode on; -1 = nothing to send. Not part of the timed
    // queue -- it never was -- so it goes out at once.
    std::atomic<int> g_tapScan{ -1 };

    int KeyFor(int which, bool up)
    {
        const ShortcutConfig& c = g_shortcutConfig;
        switch (which)
        {
        case kKeyFov:        return up ? c.fovUpKey : c.fovDownKey;
        case kKeyMixture:    return up ? c.mixtureUpKey : c.mixtureDownKey;
        case kKeyTc:         return up ? c.tcUpKey : c.tcDownKey;
        case kKeyTcCut:      return up ? c.tcCutUpKey : c.tcCutDownKey;
        case kKeyTcSlip:     return up ? c.tcSlipUpKey : c.tcSlipDownKey;
        case kKeyHeadlights: return c.headlightsKey;
        case kKeyFrontArb:   return up ? c.frontArbUpKey : c.frontArbDownKey;
        case kKeyRearArb:    return up ? c.rearArbUpKey : c.rearArbDownKey;
        case kKeyAbs:        return up ? c.absUpKey : c.absDownKey;
        case kKeyBias:       return up ? c.biasForwardKey : c.biasRearwardKey;
        case kKeySeatX:      return up ? c.seatForeKey : c.seatAftKey;
        case kKeySeatY:      return up ? c.seatUpKey : c.seatDownKey;
        case kKeyEscape:  return 0x01;   // DIK_ESCAPE
        default:             return 0;
        }
    }

    void QueueKeyPresses(int which, int steps)
    {
        g_keyNetSteps[which] += steps;
        AcquireSRWLockExclusive(&g_keyQueueLock);
        for (int i = 0; i < (steps < 0 ? -steps : steps) && g_keyQueue.size() < 64; ++i)
            g_keyQueue.push_back({ which, steps > 0 });
        ReleaseSRWLockExclusive(&g_keyQueueLock);
    }

    void SendScan(WORD dik, bool up)
    {
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = static_cast<WORD>(dik & 0x7F);
        in.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0) | ((dik & 0x80) ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(1, &in, sizeof(in));
    }

    // Input thread, every pass (or Poll, if that thread could not start).
    void PumpKeyPresses()
    {
        // Layout mode's Toggle Cursor press: a single tap, down and up in
        // one SendInput, sent as soon as it is asked for.
        const int tap = g_tapScan.exchange(-1);
        if (tap >= 0)
        {
            INPUT inputs[2] = {};
            inputs[0].type = INPUT_KEYBOARD;
            inputs[0].ki.wScan = static_cast<WORD>(tap);
            inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;
            inputs[1] = inputs[0];
            inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
            SendInput(2, inputs, sizeof(INPUT));
        }

        // Each press is held ~60 ms with ~50 ms between presses, by time
        // rather than by frames: a one-frame tap (~5 ms at high frame rates)
        // is often missed by rF2's own input polling.
        static ULONGLONG s_pressedAt = 0, s_releasedAt = 0;
        const ULONGLONG nowMs = GetTickCount64();
        if (g_heldScan)
        {
            if (nowMs - s_pressedAt < 60) return;
            SendScan(g_heldScan, true);
            g_heldScan = 0;
            s_releasedAt = nowMs;
            return;
        }
        if (nowMs - s_releasedAt < 50) return;
        KeyPress kp = {};
        AcquireSRWLockExclusive(&g_keyQueueLock);
        const bool have = !g_keyQueue.empty();
        if (have)
        {
            kp = g_keyQueue.front();
            g_keyQueue.erase(g_keyQueue.begin());
        }
        ReleaseSRWLockExclusive(&g_keyQueueLock);
        if (!have) return;
        const int dik = KeyFor(kp.which, kp.up);
        if (dik <= 0 || dik > 0xFF) return;   // unbound: the press is dropped
        g_heldScan = static_cast<WORD>(dik);
        SendScan(g_heldScan, false);
        s_pressedAt = nowMs;
    }

    // Brake bias: the car's real value from telemetry (front share), so it
    // reads right however it was changed (knob, wheel button, garage).
    // Shift point: the RPM you want to shift at (GearDisplay.h).
    void ShiftTargetValue(char* out, size_t outSize)
    {
        const GearDisplayStore::State gs = g_gearDisplay.Read();
        const double t = gs.shiftTargetFraction > 0.0 ? gs.shiftTargetFraction : g_gearDisplayConfig.revLimiterRpmFraction;
        sprintf_s(out, outSize, "%.0f%% of max RPM", t * 100.0);
    }

    void BiasValue(char* out, size_t outSize)
    {
        const SetupInfoStore::State si = g_setupInfo.Read();
        if (si.valid) sprintf_s(out, outSize, "%.1f%% front", 100.0 - si.rearBrakeBiasPercent);
        else strcpy_s(out, outSize, "in the car only");
    }

    void KnobValue(int setting, char* out, size_t outSize)
    {
        // A key-pressing setting with no key bound says so, rather than
        // counting steps that never reach the game.
        auto steps = [&](int which)
        {
            if (KeyFor(which, true) <= 0 && KeyFor(which, false) <= 0) strcpy_s(out, outSize, "no key set");
            else sprintf_s(out, outSize, "%+d steps", g_keyNetSteps[which]);
        };
        switch (setting)
        {
        case kKnobMapColour:    strcpy_s(out, outSize, MapModeName(g_trackMapConfig.deltaColourMode)); break;
        case kKnobCoverOpacity: sprintf_s(out, outSize, "%d%%", static_cast<int>(g_coverBoxConfig.a * 100.0f + 0.5f)); break;
        case kKnobCoverShade:   sprintf_s(out, outSize, "%d%% white", static_cast<int>(g_coverBoxConfig.grey * 100.0f + 0.5f)); break;
        case kKnobFov:          steps(kKeyFov); break;
        case kKnobSeatX:        steps(kKeySeatX); break;
        case kKnobSeatY:        steps(kKeySeatY); break;
        case kKnobMixture:      steps(kKeyMixture); break;
        case kKnobShiftTarget:  ShiftTargetValue(out, outSize); break;
        case kKnobTcLevel:      steps(kKeyTc); break;
        case kKnobTcCut:        steps(kKeyTcCut); break;
        case kKnobTcSlip:       steps(kKeyTcSlip); break;
        case kKnobFrontArb:     steps(kKeyFrontArb); break;
        case kKnobRearArb:      steps(kKeyRearArb); break;
        case kKnobAbs:          steps(kKeyAbs); break;
        case kKnobBias:         BiasValue(out, outSize); break;
        case kKnobTyreWear:     sprintf_s(out, outSize, "%d%%", static_cast<int>(g_tyreSaveConfig.targetWearPercent + 0.5f)); break;
        case kKnobPitLimit:
        {
            // In the dashboard's own speed units. "off" = no limit set for
            // this track: the PIT box uses the learned one.
            const GearDisplayStore::State gs = g_gearDisplay.Read();
            const double div = g_gearDisplayConfig.speedUnitsKmh ? 1.0 : 1.60934;
            const char* unit = g_gearDisplayConfig.speedUnitsKmh ? "km/h" : "mph";
            if (gs.pitLaneLimitKmh > 0.0)
                sprintf_s(out, outSize, "%.0f %s (this track)", gs.pitLaneLimitKmh / div, unit);
            else if (gs.learnedPitLimitKmh > 0.0)
                sprintf_s(out, outSize, "off (learned ~%.0f %s)", gs.learnedPitLimitKmh / div, unit);
            else
                strcpy_s(out, outSize, "off");
            break;
        }
        case kKnobTyreTime:
            if (g_tyreSaveConfig.targetMinutes <= 0.0f) strcpy_s(out, outSize, "off");
            else sprintf_s(out, outSize, "%d min", static_cast<int>(g_tyreSaveConfig.targetMinutes + 0.5f));
            break;
        case kKnobFuelStint:
            if (g_fuelSaveConfig.stintMinutes <= 0.0f) strcpy_s(out, outSize, "off");
            else sprintf_s(out, outSize, "%d min", static_cast<int>(g_fuelSaveConfig.stintMinutes + 0.5f));
            break;
        case kKnobHeadlights:
        {
            if (KeyFor(kKeyHeadlights, true) <= 0) { strcpy_s(out, outSize, "no key set"); break; }
            const int on = g_headlightsOn.load(std::memory_order_relaxed);
            strcpy_s(out, outSize, on < 0 ? "turn to switch" : (on ? "on" : "off"));
            break;
        }
        default:                if (outSize) out[0] = 0; break;
        }
    }

    // rF2's own Toggle Cursor binding (Config::ReadToggleCursorKeyName /
    // ReadToggleCursorScanCode): shown in the list and the layout banner,
    // and pressed on turning layout mode on. Each of those reads
    // Controller.JSON, which gave 8-26 ms spikes on the render thread (spike
    // log, 2026-11-23). So the input thread reads it once at start and
    // again only when the file's last-write time or size changes (looked at
    // every ~2 s -- the binding only changes in rF2's own Controls menu),
    // and the render thread just copies the result.
    struct CursorKey
    {
        char name[16];
        int scanCode;
        bool haveScan;          // bound to a keyboard key: scanCode is usable
    };
    SRWLOCK g_cursorKeyLock = SRWLOCK_INIT;
    CursorKey g_cursorKey = { "not bound", 0, false };

    CursorKey CopyCursorKey()
    {
        AcquireSRWLockShared(&g_cursorKeyLock);
        const CursorKey k = g_cursorKey;
        ReleaseSRWLockShared(&g_cursorKeyLock);
        return k;
    }

    struct CursorKeyWatch
    {
        bool started = false;   // first read done
        ULONGLONG checkedMs = 0;
        char path[MAX_PATH] = "";
        bool exists = false;
        FILETIME written = {};
        DWORD sizeLow = 0, sizeHigh = 0;
    };

    // The file Config.cpp's lookup reads: this DLL sits in
    // <rF2>\Bin64\Plugins, the file in <rF2>\UserData\player.
    void ControllerJsonPath(char* out, size_t outSize)
    {
        out[0] = 0;
        HMODULE hSelf = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(&ControllerJsonPath), &hSelf))
            return;
        char path[MAX_PATH];
        if (GetModuleFileNameA(hSelf, path, MAX_PATH) == 0)
            return;
        for (int i = 0; i < 3; ++i)
        {
            char* slash = strrchr(path, '\\');
            if (!slash)
                return;
            *slash = 0;
        }
        strcat_s(path, "\\UserData\\player\\Controller.JSON");
        strcpy_s(out, outSize, path);
    }

    // Input thread (or Poll, if that thread could not start).
    void RefreshCursorKey(CursorKeyWatch& w)
    {
        const ULONGLONG nowMs = GetTickCount64();
        if (w.started && nowMs - w.checkedMs < 2000)
            return;
        w.checkedMs = nowMs;
        if (!w.started)
            ControllerJsonPath(w.path, sizeof(w.path));
        WIN32_FILE_ATTRIBUTE_DATA fa = {};
        const bool exists = w.path[0] != 0 && GetFileAttributesExA(w.path, GetFileExInfoStandard, &fa) != 0;
        if (w.started && exists == w.exists
            && (!exists || (CompareFileTime(&fa.ftLastWriteTime, &w.written) == 0
                            && fa.nFileSizeLow == w.sizeLow && fa.nFileSizeHigh == w.sizeHigh)))
            return;
        w.started = true;
        w.exists = exists;
        w.written = fa.ftLastWriteTime;
        w.sizeLow = fa.nFileSizeLow;
        w.sizeHigh = fa.nFileSizeHigh;

        // The reads themselves happen here, outside the lock.
        CursorKey k = {};
        Config::ReadToggleCursorKeyName(k.name, sizeof(k.name));
        k.haveScan = Config::ReadToggleCursorScanCode(k.scanCode);
        AcquireSRWLockExclusive(&g_cursorKeyLock);
        g_cursorKey = k;
        ReleaseSRWLockExclusive(&g_cursorKeyLock);
    }

    // An element's Ctrl+Shift key (virtual-key code; <= 0 = unassigned).
    int ElementKey(const ShortcutConfig& cfg, int e)
    {
        switch (e)
        {
        case Shortcuts::Radar:          return cfg.radar;
        case Shortcuts::PedalTrace:     return cfg.pedalTrace;
        case Shortcuts::Perf:           return cfg.perf;
        case Shortcuts::Gear:           return cfg.gear;
        case Shortcuts::LapDelta:       return cfg.lapDelta;
        case Shortcuts::BestDelta:      return cfg.bestDelta;
        case Shortcuts::MiniSector:     return cfg.miniSector;
        case Shortcuts::RelativeTiming: return cfg.relativeTiming;
        case Shortcuts::FuelTyre:       return 0;   // no master switch any more (see Overlay.cpp)
        case Shortcuts::BrakeCue:       return cfg.brakeCue;
        case Shortcuts::Drs:            return cfg.drs;
        case Shortcuts::Clock:          return cfg.clock;
        case Shortcuts::SideBar:        return cfg.sideBar;
        case Shortcuts::FuelPanel:      return cfg.fuelPanel;
        case Shortcuts::TyresPanel:     return cfg.tyresPanel;
        case Shortcuts::BrakeBiasPanel: return cfg.brakeBiasPanel;
        case Shortcuts::TyreLockup:     return cfg.tyreLockup;
        case Shortcuts::ShiftStats:     return cfg.shiftStats;
        case Shortcuts::TyreTemps:      return cfg.tyreTemps;
        case Shortcuts::FfbGraph:       return cfg.ffbGraph;
        case Shortcuts::TrackMap:       return cfg.trackMap;
        case Shortcuts::DllCost:        return cfg.dllCost;
        case Shortcuts::CoverBoxes:     return cfg.coverBoxes;
        default:                        return 0;
        }
    }

    // The Ctrl+Shift hotkeys: the element toggles (0..ElementCount-1), then
    // the actions, in the order Poll applies them.
    enum { kHotList = Shortcuts::ElementCount, kHotResetSb, kHotResetPb, kHotLayout, kHotMaster, kHotAlwaysOn, kHotCount };
    // Presses counted by the input thread, taken (and zeroed) by Poll.
    std::atomic<int> g_hotPresses[kHotCount] = {};

    int HotKey(const ShortcutConfig& cfg, int h)
    {
        switch (h)
        {
        case kHotList:     return cfg.list;
        case kHotResetSb:  return cfg.resetSessionBest;
        case kHotResetPb:  return cfg.resetPersonalBest;
        case kHotLayout:   return cfg.layoutMode;
        case kHotMaster:   return cfg.masterToggle;
        case kHotAlwaysOn: return cfg.alwaysOnToggle;
        default:           return ElementKey(cfg, h);
        }
    }

    // Render thread: how many presses of hotkey h arrived since last asked.
    int TakePresses(int h)
    {
        // A plain load first: on almost every frame there is nothing to take.
        return g_hotPresses[h].load(std::memory_order_relaxed) != 0 ? g_hotPresses[h].exchange(0) : 0;
    }

    struct KeyboardState
    {
        bool hotWas[kHotCount] = {};
        bool padWas[6] = {};
    };

    // When Poll last ran (GetTickCount64), set every frame. The keys used to
    // be read only from Poll, i.e. only while rF2 was drawing frames, so the
    // input thread reads them only then too: keys typed into another program
    // while the game is not drawing are not picked up and applied later.
    std::atomic<ULONGLONG> g_lastPollMs{ 0 };

    // The keyboard, read on the input thread at ~100 Hz: every Ctrl+Shift
    // hotkey, plus (padKeys) the six keys standing in for the rotaries.
    // These were ~8 GetAsyncKeyState calls -- each a trip into the kernel --
    // on every frame of the render thread. A press is still counted on its
    // down edge only (no key repeat), exactly as before.
    void PollKeyboard(KeyboardState& ks, bool padKeys)
    {
        // Not drawing (see g_lastPollMs): not read at all, and the edge
        // state stays as it was -- just as when Poll itself was not running.
        // The poll time is loaded before the clock is read, so it can never
        // be the later of the two.
        const ULONGLONG lastPollMs = g_lastPollMs.load(std::memory_order_relaxed);
        if (GetTickCount64() - lastPollMs > 250)
            return;
        const ShortcutConfig& cfg = g_shortcutConfig;
        // Ctrl AND Shift, deliberately: Ctrl+<letter> alone collides with plenty
        // of the sim's own bindings, and Shift alone is a common in-car control.
        const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool modifiers = ctrl && shift;
        for (int h = 0; h < kHotCount; ++h)
        {
            // For A-Z the virtual-key code IS the uppercase ASCII value, which is
            // exactly what ReadChar normalises to. key <= 0 means unassigned
            // (see ShortcutConfig) -- GetAsyncKeyState takes 1-254, so this is
            // skipped entirely rather than passing 0 to it.
            const int key = HotKey(cfg, h);
            const bool down = key > 0 && modifiers && (GetAsyncKeyState(key) & 0x8000) != 0;
            if (down && !ks.hotWas[h])
                g_hotPresses[h].fetch_add(1);
            ks.hotWas[h] = down;
        }
        if (!padKeys)
            return;
        // Keyboard as the three rotaries: Insert/Delete = left
        // setting list up/down, Home/End = the on/off list, Page Up/Page Down
        // = right setting list. "Up" moves the highlight up, the same as
        // turning that rotary counter-clockwise.
        const int keys[6] = { VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT };
        for (int k = 0; k < 6; ++k)
        {
            const bool down = (GetAsyncKeyState(keys[k]) & 0x8000) != 0;
            if (down && !ks.padWas[k])
            {
                const int dir = (k % 2 == 0) ? 1 : -1;   // even = up
                if (k < 2) g_slotSel[0].fetch_add(dir);
                else if (k < 4) g_elemSteps.fetch_add(dir);
                else g_slotSel[1].fetch_add(dir);
            }
            ks.padWas[k] = down;
        }
    }

    // Highest wheel button configured (the rotaries and the track map colour
    // mode buttons); 0 = none set -- they all default to 0 -- and then
    // DirectInput is not even started.
    int MaxWheelButton()
    {
        const ShortcutConfig& sc = g_shortcutConfig;
        const TrackMapConfig& tm = g_trackMapConfig;
        const int b[] = { sc.centreKnobPrevButton, sc.centreKnobNextButton, sc.elementToggleButton,
                          sc.leftKnobPrevButton, sc.leftKnobNextButton, sc.rightKnobPrevButton, sc.rightKnobNextButton,
                          sc.leftSettingUpButton, sc.leftSettingDownButton,
                          sc.rightSettingUpButton, sc.rightSettingDownButton,
                          tm.deltaPrevButton, tm.deltaNextButton };
        int most = 0;
        for (int v : b)
            if (v >= 1 && v <= 128 && v > most)
                most = v;
        return most;
    }

    // Without a cooperative level set, DirectInput reads the devices
    // non-exclusively in the background, so rF2's own use of the wheel is
    // untouched.
    struct DiState
    {
        IDirectInput8W* di = nullptr;
        bool diFailed = false;          // DirectInput8Create failed: not tried again
        IDirectInputDevice8W* dev[16] = {};
        GUID guid[16] = {};
        int buttonCount[16] = {};       // buttons each device says it has
        int count = 0;
        int mostCount = 0;              // most devices open at once so far
        bool scanned = false;           // the first look has been done
        ULONGLONG lastScanMs = 0;
        bool shortLogged = false, lostLogged = false;
    };

    BOOL CALLBACK EnumJoyCb(LPCDIDEVICEINSTANCEW inst, void* ctx)
    {
        DiState& st = *static_cast<DiState*>(ctx);
        if (st.count >= 16) return DIENUM_STOP;
        // Already open from an earlier look: a working device is left alone.
        for (int i = 0; i < st.count; ++i)
            if (IsEqualGUID(st.guid[i], inst->guidInstance))
                return DIENUM_CONTINUE;
        IDirectInputDevice8W* d = nullptr;
        if (FAILED(st.di->CreateDevice(inst->guidInstance, &d, nullptr)) || !d)
            return DIENUM_CONTINUE;
        if (FAILED(d->SetDataFormat(&c_dfDIJoystick2)))
        {
            d->Release();
            return DIENUM_CONTINUE;
        }
        DIDEVCAPS caps = {};
        caps.dwSize = sizeof(caps);
        // A device that will not say counts as having all 128, so it is
        // never taken for one that is missing a configured button.
        st.buttonCount[st.count] = SUCCEEDED(d->GetCapabilities(&caps)) ? static_cast<int>(caps.dwButtons) : 128;
        d->Acquire();
        st.guid[st.count] = inst->guidInstance;
        st.dev[st.count++] = d;
        return DIENUM_CONTINUE;
    }

    // Closes device i; the last one moves into its slot.
    void DiDropDevice(DiState& st, int i)
    {
        st.dev[i]->Unacquire();
        st.dev[i]->Release();
        --st.count;
        st.dev[i] = st.dev[st.count];
        st.guid[i] = st.guid[st.count];
        st.buttonCount[i] = st.buttonCount[st.count];
        st.dev[st.count] = nullptr;
    }

    void DiReleaseDevices(DiState& st)
    {
        while (st.count > 0)
            DiDropDevice(st, st.count - 1);
    }

    // Reads every open controller's buttons into 'buttons' (1 = down, OR-ed
    // across devices), opening DirectInput and the devices as needed.
    // maxBtn: the highest button number configured (0 = none).
    //
    // The device list is built on the first call, then looked at again every
    // ~2 s for as long as something seems missing: no device at all, fewer
    // than were open before (unplugged, or stopped answering), or a
    // configured button past the last button of every open device (so the
    // device carrying it is not there). It used to be rebuilt only after a
    // device failed, so with 0 controllers found at startup (a log of
    // 2026-11-23) the wheel buttons stayed dead the whole session. A device
    // that works is never touched by a later look.
    void ReadWheelButtons(DiState& st, int maxBtn, unsigned char* buttons)
    {
        if (maxBtn <= 0)
            return;
        if (!st.di)
        {
            if (st.diFailed)
                return;
            if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
                                          reinterpret_cast<void**>(&st.di), nullptr)) || !st.di)
            {
                st.di = nullptr;
                st.diFailed = true;
                Log::Write("Shortcuts: DirectInput unavailable, wheel buttons not read");
                return;
            }
        }

        int mostButtons = 0;
        for (int i = 0; i < st.count; ++i)
            mostButtons = (std::max)(mostButtons, st.buttonCount[i]);
        const bool missing = st.count == 0 || st.count < st.mostCount || maxBtn > mostButtons;
        if (!st.scanned || (missing && GetTickCount64() - st.lastScanMs >= 2000))
        {
            st.di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumJoyCb, &st, DIEDFL_ATTACHEDONLY);
            st.lastScanMs = GetTickCount64();   // after: the look itself can take a while
            // Logged on the first look, then only when more devices are
            // open than ever before -- not on every look while one is missing.
            if (!st.scanned)
                Log::Write("Shortcuts: reading wheel buttons from %d controller(s)%s", st.count,
                           st.count ? "" : " -- none yet, looking again every 2 s");
            else if (st.count > st.mostCount)
                Log::Write("Shortcuts: controller(s) found, now reading wheel buttons from %d", st.count);
            st.scanned = true;
            if (st.count > st.mostCount)
                st.mostCount = st.count;
            mostButtons = 0;
            for (int i = 0; i < st.count; ++i)
                mostButtons = (std::max)(mostButtons, st.buttonCount[i]);
            if (st.count > 0 && maxBtn > mostButtons && !st.shortLogged)
            {
                st.shortLogged = true;
                Log::Write("Shortcuts: wheel button %d is past the last button of every controller found (%d) -- "
                           "looking again every 2 s", maxBtn, mostButtons);
            }
        }

        for (int i = 0; i < st.count; )
        {
            IDirectInputDevice8W* d = st.dev[i];
            d->Poll();
            DIJOYSTATE2 js = {};
            HRESULT hr = d->GetDeviceState(sizeof(js), &js);
            if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
            {
                d->Acquire();
                hr = d->GetDeviceState(sizeof(js), &js);
            }
            if (FAILED(hr))
            {
                // Stopped answering (unplugged?): closed, and looked for
                // again from now on, since fewer are open than were.
                if (!st.lostLogged)
                {
                    st.lostLogged = true;
                    Log::Write("Shortcuts: a controller stopped answering (0x%08lX) -- looking for it again every 2 s",
                               static_cast<unsigned long>(hr));
                }
                DiDropDevice(st, i);
                continue;   // slot i now holds the last device, not yet read
            }
            for (int b = 0; b < 128; ++b)
                buttons[b] |= (js.rgbButtons[b] & 0x80) ? 1 : 0;
            ++i;
        }
    }

    bool ButtonDown(const unsigned char* buttons, int btn)
    {
        return btn >= 1 && btn <= 128 && buttons[btn - 1] != 0;
    }

    // The input thread: the keyboard hotkeys, the queued key presses, rF2's
    // Toggle Cursor binding and the wheel buttons, at ~100 Hz. The render
    // thread only takes the results (atomics, or a copy under a small lock),
    // so none of this -- kernel calls, other programs' keyboard hooks, a
    // file read -- can hold up a frame.
    DWORD WINAPI JoyThreadProc(void*)
    {
        DiState st;
        KeyboardState keys;
        CursorKeyWatch cursorWatch;
        // Previous state per watched button, for press edges.
        const int kWatched = 13;
        bool was[kWatched] = {};
        while (!g_joyStop.load())
        {
            // Before the keys, so a hotkey never finds the binding unread.
            RefreshCursorKey(cursorWatch);
            // The six rotary keys always work, wheel buttons or not.
            PollKeyboard(keys, true);
            PumpKeyPresses();

            unsigned char buttons[128] = {};
            ReadWheelButtons(st, MaxWheelButton(), buttons);

            // Each watched button and where its press goes.
            const ShortcutConfig& sc = g_shortcutConfig;
            const TrackMapConfig& tm = g_trackMapConfig;
            struct Watch { int btn; std::atomic<int>* target; int delta; };
            const Watch watch[kWatched] = {
                { sc.centreKnobPrevButton,   &g_elemSteps,    -1 },
                { sc.centreKnobNextButton,   &g_elemSteps,    +1 },
                { sc.elementToggleButton,    &g_elemToggles,  +1 },
                { sc.leftKnobPrevButton,     &g_slotSel[0],   -1 },
                { sc.leftKnobNextButton,     &g_slotSel[0],   +1 },
                { sc.rightKnobPrevButton,    &g_slotSel[1],   -1 },
                { sc.rightKnobNextButton,    &g_slotSel[1],   +1 },
                { sc.leftSettingUpButton,    &g_slotAdj[0],   +1 },
                { sc.leftSettingDownButton,  &g_slotAdj[0],   -1 },
                { sc.rightSettingUpButton,   &g_slotAdj[1],   +1 },
                { sc.rightSettingDownButton, &g_slotAdj[1],   -1 },
                { tm.deltaPrevButton,        &g_mapModeSteps, -1 },
                { tm.deltaNextButton,        &g_mapModeSteps, +1 },
            };
            for (int i = 0; i < kWatched; ++i)
            {
                const bool now = ButtonDown(buttons, watch[i].btn);
                if (now && !was[i]) watch[i].target->fetch_add(watch[i].delta);
                was[i] = now;
            }
            Sleep(10);
        }
        // A key still held down when the plugin stops is let go, rather
        // than left down as far as Windows is concerned.
        if (g_heldScan)
        {
            SendScan(g_heldScan, true);
            g_heldScan = 0;
        }
        DiReleaseDevices(st);
        if (st.di)
            st.di->Release();
        return 0;
    }
    bool g_wasRealtime = false;
    // Hint is shown once per session, and only after realtime has stayed
    // true for a couple of seconds -- the SDK's mInRealtime can blip true
    // around session end/restart, which used to fire it at the wrong time.
    bool g_hintShownThisSession = false;
    LARGE_INTEGER g_realtimeSince = {};
    bool g_loadedFromConfig = false;

    // A single timed message, used for the transient text (toggle
    // confirmation, startup hint, INI-write warning) -- only one can
    // meaningfully be on screen at a time, and a later one should replace an
    // earlier one rather than overlap it. The LIST is separate, since it has
    // to persist while its own idle timer runs.
    enum class Popup { None, Toast, Hint, Warning };
    Popup g_popup = Popup::None;
    char g_toastText[160] = "";
    LARGE_INTEGER g_popupTime = {};
    LARGE_INTEGER g_listTime = {};
    LARGE_INTEGER g_pbArmTime = {};
    bool g_pbArmed = false;
    LARGE_INTEGER g_freq = {};
    double g_popupSeconds = 0.0;

    const double kFadeSeconds = 0.35;
    const double kWarningSeconds = 10.0;

    void EnsureFreq()
    {
        if (g_freq.QuadPart == 0)
            QueryPerformanceFrequency(&g_freq);
    }

    double SecondsSince(const LARGE_INTEGER& then)
    {
        EnsureFreq();
        if (then.QuadPart == 0 || g_freq.QuadPart == 0)
            return 1.0e9;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart - then.QuadPart) / static_cast<double>(g_freq.QuadPart);
    }

    void ShowPopup(Popup kind, double holdSeconds)
    {
        EnsureFreq();
        g_popup = kind;
        g_popupSeconds = holdSeconds;
        QueryPerformanceCounter(&g_popupTime);
    }

    // 0 = expired, otherwise the alpha to draw at.
    float PopupAlpha()
    {
        if (g_popup == Popup::None)
            return 0.0f;
        const double elapsed = SecondsSince(g_popupTime);
        if (elapsed > g_popupSeconds + kFadeSeconds)
        {
            g_popup = Popup::None;
            return 0.0f;
        }
        if (elapsed <= g_popupSeconds)
            return 1.0f;
        return static_cast<float>(1.0 - (elapsed - g_popupSeconds) / kFadeSeconds);
    }

    void SyncFromConfig()
    {
        const ShortcutConfig& cfg = g_shortcutConfig;
        // The same mapping the input thread polls by (ElementKey).
        for (int i = 0; i < Shortcuts::ElementCount; ++i)
            g_entries[i].key = ElementKey(cfg, i);

        // Visibility comes from the INI ONCE, on the first poll. After that
        // the live state is authoritative -- re-reading it every frame would
        // undo every toggle the instant it was made.
        if (!g_loadedFromConfig)
        {
            g_entries[Shortcuts::Radar].visible = cfg.showRadar;
            g_entries[Shortcuts::PedalTrace].visible = cfg.showPedalTrace;
            g_entries[Shortcuts::Perf].visible = cfg.showPerf;
            g_entries[Shortcuts::Gear].visible = cfg.showGear;
            g_entries[Shortcuts::LapDelta].visible = cfg.showLapDelta;
            g_entries[Shortcuts::BestDelta].visible = cfg.showBestDelta;
            g_entries[Shortcuts::MiniSector].visible = cfg.showMiniSector;
            g_entries[Shortcuts::RelativeTiming].visible = cfg.showRelativeTiming;
            g_entries[Shortcuts::FuelTyre].visible = cfg.showFuelTyre;
            g_entries[Shortcuts::BrakeCue].visible = cfg.showBrakeCue;

            g_entries[Shortcuts::Drs].visible = cfg.showDrs;
            g_entries[Shortcuts::Clock].visible = cfg.showClock;
            g_entries[Shortcuts::SideBar].visible = cfg.showSideBar;
            g_entries[Shortcuts::FuelPanel].visible = cfg.showFuelPanel;
            g_entries[Shortcuts::TyresPanel].visible = cfg.showTyresPanel;
            g_entries[Shortcuts::BrakeBiasPanel].visible = cfg.showBrakeBiasPanel;
            g_entries[Shortcuts::TyreLockup].visible = cfg.showTyreLockup;
            g_entries[Shortcuts::ShiftStats].visible = cfg.showShiftStats;
            g_entries[Shortcuts::TyreTemps].visible = cfg.showTyreTemps;
            g_entries[Shortcuts::FfbGraph].visible = cfg.showFfbGraph;
            g_entries[Shortcuts::TrackMap].visible = cfg.showTrackMap;
            g_entries[Shortcuts::DllCost].visible = cfg.showDllCost;
            g_entries[Shortcuts::CoverBoxes].visible = cfg.showCoverBoxes;
            g_loadedFromConfig = true;
        }
    }

    // Persist one toggle. A failure here is worth surfacing on screen rather
    // than swallowing: the INI lives under Program Files, which is
    // UAC-protected, and a silent failure looks exactly like "the plugin
    // keeps forgetting my settings" with no explanation.
    void Persist(const Entry& e)
    {
        if (Config::WriteBool("Shortcuts", e.iniKey, e.visible))
            return;
        sprintf_s(g_toastText, "Could not save to rF2SkrOverlayPublic.ini -- toggles will not persist");
        ShowPopup(Popup::Warning, kWarningSeconds);
        Log::Write("Shortcuts: failed to write [Shortcuts] %s to the INI (read-only? UAC?)", e.iniKey);
    }

    bool EntryVisible(int e)
    {
        return e >= 0 && e < Shortcuts::ElementCount && g_entries[e].visible;
    }

    // Label / on-state / toggle per list code (see kListOrder), shared by the
    // mouse list and the centre rotary so the two always agree.
    const char* ListEntryLabel(int code)
    {
        if (code >= 0 && code < Shortcuts::ElementCount) return g_entries[code].label;
        switch (code - 1000)
        {
        case 19: return "Fuel use delta bar (fuel save)";
        case 20: return "Tyre wear target";
        case 21: return "Water / oil temps";
        case 22: return "FFB strength";
        case 23: return "Track conditions";
        case 24: return "Real weather forecast";
        case 25: return "Laps since pit (standings)";
        case 27: return "Car numbers (standings)";
        case 28: return "Gap box (2 ahead, 2 behind)";
        case 29: return "Update check (GitHub, at game start)";
        default: return "Reset timing tower (gaps return after one lap)";
        }
    }
    bool ListEntryOn(int code)
    {
        if (code >= 0 && code < Shortcuts::ElementCount) return g_entries[code].visible;
        switch (code - 1000)
        {
        case 19: return g_fuelSaveConfig.enabled;
        case 20: return g_tyreSaveConfig.enabled;
        case 21: return g_engineTempsConfig.enabled;
        case 22: return g_ffbLevelConfig.enabled;
        case 23: return g_conditionsConfig.enabled;
        case 24: return g_conditionsConfig.realWeather;
        case 25: return g_relativeTimingConfig.showPitLaps;
        case 27: return g_relativeTimingConfig.showCarNumber;
        case 28: return g_relativeTimingConfig.showGapBox;
        case 29: return g_updateCheckConfig.enabled;
        default: return true;
        }
    }
    void ToggleListEntry(int code)
    {
        if (code >= 0 && code < Shortcuts::ElementCount)
        {
            Entry& e = g_entries[code];
            e.visible = !e.visible;
            Persist(e);
            return;
        }
        switch (code - 1000)
        {
        case 19:
            g_fuelSaveConfig.enabled = !g_fuelSaveConfig.enabled;
            Config::WriteBool("FuelSave", "Enabled", g_fuelSaveConfig.enabled);
            break;
        case 20:
            g_tyreSaveConfig.enabled = !g_tyreSaveConfig.enabled;
            Config::WriteBool("TyreSave", "Enabled", g_tyreSaveConfig.enabled);
            break;
        case 21:
            g_engineTempsConfig.enabled = !g_engineTempsConfig.enabled;
            Config::WriteBool("EngineTemps", "Enabled", g_engineTempsConfig.enabled);
            break;
        case 22:
            g_ffbLevelConfig.enabled = !g_ffbLevelConfig.enabled;
            Config::WriteBool("FfbLevel", "Enabled", g_ffbLevelConfig.enabled);
            break;
        case 23:
            g_conditionsConfig.enabled = !g_conditionsConfig.enabled;
            Config::WriteBool("Conditions", "Enabled", g_conditionsConfig.enabled);
            break;
        case 24:
            g_conditionsConfig.realWeather = !g_conditionsConfig.realWeather;
            Config::WriteBool("Conditions", "RealWeather", g_conditionsConfig.realWeather);
            break;
        case 25:
            g_relativeTimingConfig.showPitLaps = !g_relativeTimingConfig.showPitLaps;
            Config::WriteBool("RelativeTiming", "ShowPitLaps", g_relativeTimingConfig.showPitLaps);
            break;
        case 28:
            g_relativeTimingConfig.showGapBox = !g_relativeTimingConfig.showGapBox;
            Config::WriteBool("RelativeTiming", "ShowGapBox", g_relativeTimingConfig.showGapBox);
            break;
        case 27:
            g_relativeTimingConfig.showCarNumber = !g_relativeTimingConfig.showCarNumber;
            Config::WriteBool("RelativeTiming", "ShowCarNumber", g_relativeTimingConfig.showCarNumber);
            break;
        case 26:
            g_resetTimingRequested = true;
            break;
        case 29:
            // Saved like the others; switched on, it also checks now if it
            // has not this game start (a thread of its own, see UpdateCheck.h).
            g_updateCheckConfig.enabled = !g_updateCheckConfig.enabled;
            Config::WriteBool("Updates", "CheckForUpdates", g_updateCheckConfig.enabled);
            if (g_updateCheckConfig.enabled)
                UpdateCheck::Start();
            break;
        }
    }
}

void Shortcuts::Poll()
{
    const ShortcutConfig& cfg = g_shortcutConfig;
    if (!cfg.enabled)
        return;

    SyncFromConfig();
    g_lastPollMs.store(GetTickCount64(), std::memory_order_relaxed);

    // The input thread (JoyThreadProc) reads the keyboard and the wheel,
    // sends the queued key presses and keeps rF2's Toggle Cursor binding
    // fresh -- all of which used to run here and could stall a frame. Started
    // once, wheel buttons set or not (it only opens DirectInput once one is);
    // should it fail to start, the same work runs here instead, so the
    // shortcuts keep working (the wheel buttons alone are then not read).
    static bool s_threadTried = false;
    if (!s_threadTried)
    {
        s_threadTried = true;
        g_joyStop.store(false);
        g_joyThread = CreateThread(nullptr, 0, JoyThreadProc, nullptr, 0, nullptr);
        if (!g_joyThread)
            Log::Write("Shortcuts: could not start the input thread (error %lu) -- reading keys on the render thread",
                       GetLastError());
    }
    if (!g_joyThread)
    {
        static KeyboardState s_keys;
        static CursorKeyWatch s_cursorWatch;
        RefreshCursorKey(s_cursorWatch);
        PollKeyboard(s_keys, true);
    }

    // The Ctrl+Shift hotkeys, as counted by PollKeyboard: one count per
    // press, applied in the same order as always.
    for (int i = 0; i < ElementCount; ++i)
    {
        Entry& e = g_entries[i];
        for (int n = TakePresses(i); n > 0; --n)
        {
            e.visible = !e.visible;
            Persist(e);
            if (g_popup != Popup::Warning)
            {
                sprintf_s(g_toastText, "%s %s", e.label, e.visible ? "shown" : "hidden");
                ShowPopup(Popup::Toast, 1.5);
            }
            // Choosing something from the list closes it -- that behaviour
            // was liked and is deliberately kept.
            g_listOpen = false;
        }
    }

    for (int n = TakePresses(kHotList); n > 0; --n)
    {
        g_listOpen = !g_listOpen;
        QueryPerformanceCounter(&g_listTime);
    }

    // Reset session best -- in-memory only, so no confirmation needed.
    for (int n = TakePresses(kHotResetSb); n > 0; --n)
    {
        g_lapDelta.ResetSessionBest();
        g_miniSector.InvalidateReference();
        sprintf_s(g_toastText, "Session best cleared");
        ShowPopup(Popup::Toast, 2.0);
    }

    // Reset personal best -- DESTRUCTIVE (it deletes the saved file), so the
    // first press only arms it and says what is about to go; a second press
    // inside the window does it, and letting the window lapse cancels.
    for (int n = TakePresses(kHotResetPb); n > 0; --n)
    {
        if (g_pbArmed && SecondsSince(g_pbArmTime) <= cfg.resetConfirmSeconds)
        {
            g_lapDelta.ResetPersonalBest();
            g_miniSector.InvalidateReference();
            g_pbArmed = false;
            sprintf_s(g_toastText, "Personal best DELETED");
            ShowPopup(Popup::Toast, 3.0);
        }
        else
        {
            char what[96];
            g_lapDelta.DescribePersonalBest(what, sizeof(what));
            g_pbArmed = true;
            QueryPerformanceCounter(&g_pbArmTime);
            char kn[16];
            Config::KeyName(cfg.resetPersonalBest, kn, sizeof(kn));
            sprintf_s(g_toastText, "Delete personal best (%s)? Press Ctrl+Shift+%s again",
                      what, kn);
            ShowPopup(Popup::Toast, cfg.resetConfirmSeconds);
        }
    }

    if (g_pbArmed && SecondsSince(g_pbArmTime) > cfg.resetConfirmSeconds)
        g_pbArmed = false;

    // Layout mode: drag panels with the mouse. Announced with a toast as well
    // as Layout's own banner, because it changes what a mouse click does.
    for (int n = TakePresses(kHotLayout); n > 0; --n)
    {
        Layout::Toggle();
        if (g_popup != Popup::Warning)
        {
            if (Layout::Active())
            {
                // The cached binding (see CopyCursorKey): no file read here.
                const CursorKey cursorKey = CopyCursorKey();
                // Press rF2's own Toggle Cursor control for the driver rather
                // than just telling them which key it is -- but only if the
                // cursor is not already up, since the control is a toggle:
                // sending it while the cursor is already showing would hide
                // it again instead of doing nothing. The tap itself is sent
                // by the input thread (see PumpKeyPresses).
                if (!Layout::IsCursorVisible() && cursorKey.haveScan)
                    g_tapScan.store(cursorKey.scanCode);

                sprintf_s(g_toastText, "Layout mode ON -- enable the mouse cursor to drag (Toggle Cursor: %s)",
                          cursorKey.name);
                ShowPopup(Popup::Toast, 4.0);
            }
            else
            {
                sprintf_s(g_toastText, "Layout mode off");
                ShowPopup(Popup::Toast, 2.0);
            }
        }
    }
    // Master on/off for the whole overlay. Deliberately ABOVE the list
    // handling: with the overlay hidden, its own toast is the only thing
    // still drawn, which is what keeps the key findable again.
    for (int n = TakePresses(kHotMaster); n > 0; --n)
    {
        g_shortcutConfig.showOverlay = !g_shortcutConfig.showOverlay;
        if (!Config::WriteBool("Shortcuts", "ShowOverlay", g_shortcutConfig.showOverlay))
            Log::Write("Shortcuts: failed to persist ShowOverlay");
        sprintf_s(g_toastText, "Overlay %s",
                  g_shortcutConfig.showOverlay ? "ON" : "OFF");
        ShowPopup(Popup::Toast, 1.5);
        // A hidden overlay with the list still open reads as broken.
        if (!g_shortcutConfig.showOverlay)
            g_listOpen = false;
    }

    // HUD always on, also when not driving.
    for (int n = TakePresses(kHotAlwaysOn); n > 0; --n)
    {
        g_shortcutConfig.alwaysOn = !g_shortcutConfig.alwaysOn;
        Config::WriteBool("Shortcuts", "AlwaysOn", g_shortcutConfig.alwaysOn);
        sprintf_s(g_toastText, "HUD always on: %s", g_shortcutConfig.alwaysOn ? "ON" : "OFF");
        ShowPopup(Popup::Toast, 1.5);
    }

    // --- Wheel buttons and the rotary menu ---
    // Buttons are read on the input thread (see JoyThreadProc), which only
    // opens DirectInput once a button is actually configured; only the
    // clicks it counted are applied here. The keyboard stands in for the
    // three rotaries (PollKeyboard) and always works, so the menu is usable
    // without any wheel buttons set (they all default to 0).
    TrackMapConfig& tmc = g_trackMapConfig;

    // Track map colouring: step through off / PB / SB / session fastest / wet.
    const int mapSteps = g_mapModeSteps.exchange(0);
    if (mapSteps != 0)
    {
        tmc.deltaColourMode = (((tmc.deltaColourMode + mapSteps) % 4) + 4) % 4;
        Config::WriteFloat("TrackMap", "DeltaColourMode", static_cast<float>(tmc.deltaColourMode));
        if (g_popup != Popup::Warning)
        {
            sprintf_s(g_toastText, "Track map colour: %s", MapModeName(tmc.deltaColourMode));
            ShowPopup(Popup::Toast, 1.5);
        }
    }

    // Centre rotary + on/off button: the on/off list.
    const int elemSteps = g_elemSteps.exchange(0);
    const int elemToggles = g_elemToggles.exchange(0);
    if (elemSteps != 0)
        // Sign flipped: counter-clockwise (and "up" keys) move the highlight
        // UP the list, clockwise DOWN, matching how the list is drawn.
        g_elemIndex = StepListOrder(g_elemIndex, -elemSteps);
    bool closeList = false;
    if (elemToggles % 2 != 0)
    {
        if (kListOrder[g_elemIndex] == kCloseEntry) { closeList = true; QueueKeyPresses(kKeyEscape, 1); }
        else ToggleListEntry(kListOrder[g_elemIndex]);
    }
    if (closeList)
        g_elemSelTime.QuadPart = 0;
    else if (elemSteps != 0 || elemToggles != 0)
        QueryPerformanceCounter(&g_elemSelTime);

    // Left and right rotaries each pick a setting -- only among those whose
    // display is switched on (car settings always) -- and their up/down
    // buttons change it.
    for (int slot = 0; slot < 2; ++slot)
    {
        g_knobSetting = g_knobSel[slot];
        const int selSteps = g_slotSel[slot].exchange(0);
        const int adjSteps = g_slotAdj[slot].exchange(0);
        if (selSteps != 0 || adjSteps != 0)
            g_activeSlot = slot;
        if (!KnobAvailable(g_knobSetting))
            g_knobSetting = StepKnob(g_knobSetting, 1);
        // Flipped like the on/off list: counter-clockwise up, clockwise down.
        for (int i = 0; i < selSteps; ++i) g_knobSetting = StepKnob(g_knobSetting, -1);
        for (int i = 0; i > selSteps; --i) g_knobSetting = StepKnob(g_knobSetting, 1);
        if (adjSteps != 0)
        {
            switch (g_knobSetting)
            {
            case kKnobMapColour:
                tmc.deltaColourMode = (((tmc.deltaColourMode + adjSteps) % 4) + 4) % 4;
                Config::WriteFloat("TrackMap", "DeltaColourMode", static_cast<float>(tmc.deltaColourMode));
                break;
            case kKnobCoverOpacity:
            case kKnobCoverShade:
            {
                float& v = (g_knobSetting == kKnobCoverOpacity) ? g_coverBoxConfig.a : g_coverBoxConfig.grey;
                float nv = floorf((v + adjSteps * 0.05f) * 20.0f + 0.5f) / 20.0f;
                if (nv < 0.0f) nv = 0.0f;
                if (nv > 1.0f) nv = 1.0f;
                v = nv;
                Config::WriteFloat("CoverBox", g_knobSetting == kKnobCoverOpacity ? "Alpha" : "Grey", nv);
                break;
            }
            case kKnobFov:      QueueKeyPresses(kKeyFov, adjSteps); break;
            case kKnobSeatX:    QueueKeyPresses(kKeySeatX, adjSteps); break;
            case kKnobSeatY:    QueueKeyPresses(kKeySeatY, adjSteps); break;
            case kKnobMixture:  QueueKeyPresses(kKeyMixture, adjSteps); break;
            case kKnobShiftTarget: g_gearDisplay.AdjustShiftTarget(adjSteps); break;
            case kKnobTcLevel:  QueueKeyPresses(kKeyTc, adjSteps); break;
            case kKnobTcCut:    QueueKeyPresses(kKeyTcCut, adjSteps); break;
            case kKnobTcSlip:   QueueKeyPresses(kKeyTcSlip, adjSteps); break;
            case kKnobFrontArb: QueueKeyPresses(kKeyFrontArb, adjSteps); break;
            case kKnobRearArb:  QueueKeyPresses(kKeyRearArb, adjSteps); break;
            case kKnobAbs:      QueueKeyPresses(kKeyAbs, adjSteps); break;
            case kKnobBias:     QueueKeyPresses(kKeyBias, adjSteps); break;
            case kKnobFuelStint:
            {
                float v = floorf(g_fuelSaveConfig.stintMinutes + 0.5f) + static_cast<float>(adjSteps);
                if (v < 0.0f) v = 0.0f;
                if (v > 240.0f) v = 240.0f;
                g_fuelSaveConfig.stintMinutes = v;
                Config::WriteFloat("FuelSave", "StintMinutes", v);
                break;
            }
            case kKnobTyreWear:
            {
                float v = floorf(g_tyreSaveConfig.targetWearPercent + 0.5f) + static_cast<float>(adjSteps);
                if (v < 5.0f) v = 5.0f;
                if (v > 100.0f) v = 100.0f;
                g_tyreSaveConfig.targetWearPercent = v;
                Config::WriteFloat("TyreSave", "TargetWearPercent", v);
                break;
            }
            case kKnobPitLimit:
            {
                // One click = 1 km/h (or mph). From "off" the first click
                // starts at the learned limit (else 60 km/h / 40 mph);
                // turning below 20 switches it back off. Saved for this
                // track once you are out of the car.
                const GearDisplayStore::State gs = g_gearDisplay.Read();
                const double div = g_gearDisplayConfig.speedUnitsKmh ? 1.0 : 1.60934;
                double v;
                if (gs.pitLaneLimitKmh > 0.0)
                    v = floor(gs.pitLaneLimitKmh / div + 0.5) + adjSteps;
                else if (gs.learnedPitLimitKmh > 0.0)
                    v = floor(gs.learnedPitLimitKmh / div + 0.5);
                else
                    v = g_gearDisplayConfig.speedUnitsKmh ? 60.0 : 40.0;
                if (v > 250.0) v = 250.0;
                g_gearDisplay.SetPitLaneLimit(v < 20.0 ? 0.0 : v * div);
                break;
            }
            case kKnobTyreTime:
            {
                float v = floorf(g_tyreSaveConfig.targetMinutes + 0.5f) + 5.0f * static_cast<float>(adjSteps);
                if (v < 0.0f) v = 0.0f;
                if (v > 600.0f) v = 600.0f;
                g_tyreSaveConfig.targetMinutes = v;
                Config::WriteFloat("TyreSave", "TargetMinutes", v);
                break;
            }
            case kKnobHeadlights:
                // A toggle: one press per turn, whichever way and however
                // many clicks the turn was.
                QueueKeyPresses(kKeyHeadlights, 1);
                break;
            default:
                break;
            }
        }
        if (selSteps != 0)
            QueryPerformanceCounter(&g_knobSelTime);
        // Using a setting rotary leaves the on/off list view straight away.
        if (selSteps != 0 || adjSteps != 0)
            g_elemSelTime.QuadPart = 0;
        // The setting box (drawn in Draw) shows the setting; with it hidden,
        // fall back to a message.
        if ((selSteps != 0 || adjSteps != 0) && !g_shortcutConfig.showKnobBox && g_popup != Popup::Warning)
        {
            char value[64];
            KnobValue(g_knobSetting, value, sizeof(value));
            sprintf_s(g_toastText, "%s %s: %s", slot ? "R" : "L",
                      kKnobSettingName[g_knobSetting], value);
            ShowPopup(Popup::Toast, 5.0);
        }
        g_knobSel[slot] = g_knobSetting;
    }
    g_knobSetting = g_knobSel[g_activeSlot];

    // The presses queued above (and the Toggle Cursor tap) normally go out
    // from the input thread; only without one are they sent from here.
    if (!g_joyThread)
        PumpKeyPresses();
}

void Shortcuts::Shutdown()
{
    if (!g_joyThread)
        return;
    g_joyStop.store(true);
    WaitForSingleObject(g_joyThread, 1000);
    CloseHandle(g_joyThread);
    g_joyThread = nullptr;
}

void Shortcuts::ToggleCursorKeyName(char* out, size_t outSize)
{
    const CursorKey k = CopyCursorKey();
    strncpy_s(out, outSize, k.name, _TRUNCATE);
}

bool Shortcuts::ListOpen()
{
    return g_listOpen;
}

bool Shortcuts::AlwaysOn()
{
    return g_shortcutConfig.alwaysOn;
}

bool Shortcuts::OverlayEnabled()
{
    // With shortcuts disabled outright the overlay stays up, for the same
    // reason Visible() returns true there: turning the feature off in the INI
    // must not strand the overlay hidden with no key left to bring it back.
    if (!g_shortcutConfig.enabled)
        return true;
    return g_shortcutConfig.showOverlay;
}

void Shortcuts::ShowMessage(const char* text, double holdSeconds)
{
    strncpy_s(g_toastText, text, _TRUNCATE);
    ShowPopup(Popup::Toast, holdSeconds);
}

const char* Shortcuts::IniKey(Element e)
{
    return (e >= 0 && e < ElementCount) ? g_entries[e].iniKey : "";
}

bool Shortcuts::Toggled(Element e)
{
    return (e >= 0 && e < ElementCount) ? g_entries[e].visible : true;
}

void Shortcuts::SetVisible(Element e, bool visible)
{
    if (e < 0 || e >= ElementCount || g_entries[e].visible == visible)
        return;
    g_entries[e].visible = visible;
    Persist(g_entries[e]);
}

bool Shortcuts::Visible(Element e)
{
    if (e < 0 || e >= ElementCount)
        return true;
    // With shortcuts disabled everything stays visible rather than stuck at
    // whatever was last toggled -- otherwise turning the feature off in the
    // INI could leave an element permanently hidden with no way back.
    if (!g_shortcutConfig.enabled)
        return true;
    // Layout mode needs every panel visible to be draggable/resizeable,
    // regardless of whatever it was individually toggled to -- a panel you
    // deliberately hid (the clock, say) is still one you might want to
    // reposition before ever turning it back on.
    if (Layout::Active())
        return true;
    return g_entries[e].visible;
}

void Shortcuts::ResetHint()
{
    g_hintShownThisSession = false;
    g_wasRealtime = false;
    g_realtimeSince.QuadPart = 0;
}

void Shortcuts::NoteRealtime(bool inRealtime)
{
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);

    if (inRealtime && !g_wasRealtime)
        g_realtimeSince = now;
    g_wasRealtime = inRealtime;

    if (!inRealtime || g_hintShownThisSession || !g_shortcutConfig.enabled
        || g_shortcutConfig.startupHintSeconds <= 0.0f || g_realtimeSince.QuadPart == 0)
        return;

    const double stableSeconds = static_cast<double>(now.QuadPart - g_realtimeSince.QuadPart)
                               / static_cast<double>(freq.QuadPart);
    if (stableSeconds < 2.0)
        return;

    char listKey[16], layoutKey[16];
    Config::KeyName(g_shortcutConfig.list, listKey, sizeof(listKey));
    Config::KeyName(g_shortcutConfig.layoutMode, layoutKey, sizeof(layoutKey));
    sprintf_s(g_toastText, "Ctrl+Shift+%s for shortcuts, Ctrl+Shift+%s to edit layout",
              listKey, layoutKey);
    ShowPopup(Popup::Hint, g_shortcutConfig.startupHintSeconds);
    g_hintShownThisSession = true;
}

void Shortcuts::Draw(Renderer& renderer, int screenWidth, int screenHeight, HWND hwnd)
{
    const ShortcutConfig& cfg = g_shortcutConfig;
    const float uiScale = static_cast<float>(screenHeight) / 2160.0f;

    // These draw their own panel, so suppress the per-string plate that
    // Renderer::AddText would otherwise put behind every line.
    const bool prevTextBg = renderer.TextBackgroundEnabled();
    renderer.SetTextBackground(false);

    // --- The shortcut list: a plain toggle now -- no auto-close timer.
    // It used to time itself out so it could never be left up by accident,
    // but that meant it could vanish mid-use (reading it, or clicking
    // through several rows) with no warning. Only the hotkey (or clicking
    // an element row, which still restarts nothing since there is no timer
    // left to restart) closes it now.
    //
    // NOT forced up just because layout mode is active: layout mode is used
    // from the cockpit (see Layout.cpp), and out of the car nothing but the
    // perf overlay stays up at all. Opening it once with its own hotkey
    // while driving is enough for Layout to know its box and let it be
    // dragged/resized from then on (see Layout.h).
    if (g_listOpen)
    {
        const float listScale = uiScale * cfg.listScale;
        // Text size preset (S / M / L boxes at the top): small is the compact
        // size of 2026-11-22, medium the size before it, large one more step
        // of the same ratio. Rows stay compact at every size.
        static const float kTextPx[3] = { 21.0f, 30.0f, 43.0f };
        static const float kTitlePx[3] = { 24.0f, 34.0f, 49.0f };
        static const float kPadPx[3] = { 15.0f, 22.0f, 31.0f };
        const int sizeIdx = (cfg.listTextSize >= 0 && cfg.listTextSize <= 2) ? cfg.listTextSize : 0;
        const float textPx = kTextPx[sizeIdx] * listScale;
        const float lineStep = textPx * 1.22f;
        const float padding = kPadPx[sizeIdx] * listScale;
        const float titlePx = kTitlePx[sizeIdx] * listScale;
        const float sizeBoxPx = titlePx * 1.15f;               // S / M / L boxes
        const float sizeBoxGap = titlePx * 0.3f;
        const float sizeBoxesW = 3.0f * sizeBoxPx + 2.0f * sizeBoxGap;

        // The visibility toggles, then the ACTIONS -- resets and panel
        // positioning -- which are not Elements and so are not in g_entries.
        const float infoPx = textPx * 0.85f;
        const int kMaxLines = ElementCount + 80;
        char lines[kMaxLines][96];         // info rows only: full text
        char keyPart[kMaxLines][24];       // hotkey rows: "Ctrl+Shift+X"
        char labelPart[kMaxLines][80];     // hotkey rows: the label after the key column
        int rowElementIndex[kMaxLines];   // -1 = not a toggleable element row
        bool rowIsInfo[kMaxLines];        // dimmer, smaller -- not a hotkey at all
        int rowCustom[kMaxLines];         // -1, or a menu-setting row id (see addSettingRow)
        for (int i = 0; i < kMaxLines; ++i) rowCustom[i] = -1;
        int n = 0;
        float widest = renderer.MeasureText("Overlay shortcuts", titlePx) + titlePx * 1.5f + sizeBoxesW;
        // The key text varies in width ("1" vs "Ctrl+Shift+F1"), so the
        // labels after it land at different x per row unless drawn as a
        // second, fixed-x column starting past the widest key in the list.
        float widestKeyPx = 0.0f;
        const float keyLabelGapPx = 22.0f * (uiScale * cfg.listScale);

        // BOUNDS FIRST, in both builders. These write into fixed stack
        // arrays sized kMaxLines, and the row count is the sum of a dozen
        // scattered calls below -- so the day someone adds a row past the
        // cap, the failure without this is a stack smash inside the render
        // thread, not a missing line. Dropping the row is the right loss.
        auto addInfo = [&](const char* text)
        {
            if (n >= kMaxLines)
                return;
            strcpy_s(lines[n], text);
            rowElementIndex[n] = -1;
            rowIsInfo[n] = true;
            const float w = renderer.MeasureText(text, infoPx);
            if (w > widest) widest = w;
            ++n;
        };
        // key <= 0 means unassigned (see ShortcutConfig) -- shown as a bare
        // "Unassigned" rather than the nonsensical "Ctrl+Shift+Unassigned".
        auto addActionRow = [&](int key, const char* label)
        {
            if (n >= kMaxLines)
                return;
            if (key <= 0)
            {
                strcpy_s(keyPart[n], "Unassigned");
            }
            else
            {
                char kn[16];
                Config::KeyName(key, kn, sizeof(kn));
                sprintf_s(keyPart[n], "Ctrl+Shift+%s", kn);
            }
            strcpy_s(labelPart[n], label);
            rowElementIndex[n] = -1;
            rowIsInfo[n] = false;
            const float kw = renderer.MeasureText(keyPart[n], textPx);
            if (kw > widestKeyPx) widestKeyPx = kw;
            ++n;
        };
        auto addAction = [&](int key, const char* label)
        {
            addActionRow(key, label);
        };
        auto addElement = [&](int elemIndex)
        {
            char label[80];
            sprintf_s(label, "%s%s", g_entries[elemIndex].label,
                      g_entries[elemIndex].visible ? "" : "   (hidden)");
            const int before = n;
            addActionRow(g_entries[elemIndex].key, label);
            // Only tag the row if one was actually added: without this a
            // dropped row would retag the PREVIOUS row as this element, so
            // clicking it would toggle the wrong panel.
            if (n > before)
                rowElementIndex[n - 1] = elemIndex;
        };

        // How to actually USE the list -- not hotkeys, so they carry no key
        // binding and cannot be clicked.
        addInfo("Click a row below to toggle it (mouse cursor must be visible)");
        // rF2's Toggle Cursor key: the input thread's cached copy (see
        // CopyCursorKey). Reading rF2's controller file here gave 8-26 ms
        // spikes (spike log, 2026-11-23).
        const CursorKey cursorKey = CopyCursorKey();
        char cursorInfo[64];
        sprintf_s(cursorInfo, "Enable mouse cursor: Toggle Cursor (%s)", cursorKey.name);
        addInfo(cursorInfo);

        // The two most important bindings, first: how to get back to this
        // list, and how to rearrange the HUD. Everything else can be
        // rediscovered from here, but these two are how you reach it.
        addAction(cfg.list, "This list");
        addAction(cfg.layoutMode, "Layout mode -- drag / resize panels");


        addAction(cfg.masterToggle, "Overlay on/off (everything)");
        addAction(cfg.alwaysOnToggle, g_shortcutConfig.alwaysOn ? "HUD always on (also not driving): ON" : "HUD always on (also not driving): off");
        addAction(cfg.resetSessionBest, "Reset session best");
        addAction(cfg.resetPersonalBest, "Delete personal best (press twice)");

        // Label for the relative-timing "cars shown" slider drawn manually
        // below the normal row loop (see there) -- reserved here as a plain
        // info row so the panel's own width/height sizing accounts for it
        // like every other line, even though its actual control is custom.
        const int carsSliderLabelRow = n;
        char carsSliderLabel[64];
        sprintf_s(carsSliderLabel, "Relative timing: cars shown (%d)",
                  static_cast<int>(g_relativeTimingConfig.visibleCars + 0.5f));
        addInfo(carsSliderLabel);

        // Clickable settings rows, so none of it needs the INI.
        auto addSettingRow = [&](int id, const char* label)
        {
            const int before = n;
            addActionRow(0, label);
            if (n > before)
            {
                strcpy_s(keyPart[n - 1], "Click");
                rowCustom[n - 1] = id;
            }
        };

        // Every panel and on/off option, in groups (kListOrder, shared with
        // the centre rotary).
        for (int code : kListOrder)
        {
            if (code == kCloseEntry) continue;   // rotary only
            if (code < 0) { addInfo(kGroupName[-1 - code]); continue; }
            if (code < 1000) { addElement(code); continue; }
            const int id = code - 1000;
            char line[80];
            if (code == kResetTowerCode) sprintf_s(line, ">> %s", ListEntryLabel(code));
            else sprintf_s(line, "%s%s", ListEntryLabel(code),
                           ListEntryOn(code) ? "" : (code == kUpdateCheckCode ? "   (off)" : "   (hidden)"));
            addSettingRow(id, line);
        }
        // How to drive the list without the mouse.
        addInfo("Rotaries: Home/End on/off list, Insert/Delete left setting, PgUp/PgDn right setting");
        {
            const ShortcutConfig& w = g_shortcutConfig;
            if (w.centreKnobPrevButton > 0 || w.centreKnobNextButton > 0 || w.elementToggleButton > 0
                || w.leftKnobPrevButton > 0 || w.leftKnobNextButton > 0 || w.rightKnobPrevButton > 0 || w.rightKnobNextButton > 0
                || w.leftSettingUpButton > 0 || w.leftSettingDownButton > 0 || w.rightSettingUpButton > 0 || w.rightSettingDownButton > 0)
            {
                char knobLabel[160];
                sprintf_s(knobLabel, "Wheel: btn %d/%d on/off item, %d toggles; left %d/%d setting, %d/%d up/down; right %d/%d, %d/%d",
                          w.centreKnobPrevButton, w.centreKnobNextButton, w.elementToggleButton,
                          w.leftKnobPrevButton, w.leftKnobNextButton, w.leftSettingUpButton, w.leftSettingDownButton,
                          w.rightKnobPrevButton, w.rightKnobNextButton, w.rightSettingUpButton, w.rightSettingDownButton);
                addInfo(knobLabel);
            }
            else
                addInfo("Wheel: no rotary buttons set ([Shortcuts] *KnobPrev/NextButton etc. in the INI)");
        }
        char lsLine[64];

        // ABS / TC box beside the gear digit: each on or off.
        sprintf_s(lsLine, "  [%c] ABS indicator (gear box)", g_gearDisplayConfig.showAbs ? 'x' : ' ');
        addSettingRow(8, lsLine);
        sprintf_s(lsLine, "  [%c] TC indicator (gear box)", g_gearDisplayConfig.showTc ? 'x' : ' ');
        addSettingRow(9, lsLine);

        // Track map: colouring, and this circuit's rotation / mirror /
        // re-record (the same as the layout-mode controls under the map).
        int mapRotLabelRow = -1;
        char mapRotLabel[64];
        int mapSectorsLabelRow = -1;
        char mapSectorsLabel[64];
        {
            const TrackMapConfig& t = g_trackMapConfig;
            const char* mode = MapModeName(t.deltaColourMode);
            char mapLine[96];
            if (t.deltaPrevButton > 0 || t.deltaNextButton > 0)
                sprintf_s(mapLine, "Track map colour: %s  (click, or wheel btn %d/%d)", mode, t.deltaPrevButton, t.deltaNextButton);
            else
                sprintf_s(mapLine, "Track map colour: %s  (click to change)", mode);
            addSettingRow(13, mapLine);
            mapSectorsLabelRow = n;
            sprintf_s(mapSectorsLabel, "Track map mini-sectors (%d per lap)", t.deltaSectorCount);
            addInfo(mapSectorsLabel);
            if (TrackMap::HasMap())
            {
                mapRotLabelRow = n;
                sprintf_s(mapRotLabel, "Track map rotation (%.0f deg, this circuit)", TrackMap::RotationDegrees());
                addInfo(mapRotLabel);
                sprintf_s(mapLine, "  [%c] Track map mirrored", TrackMap::Mirrored() ? 'x' : ' ');
                addSettingRow(14, mapLine);
                addSettingRow(15, TrackMap::Rerecording() ? ">> Re-recording map on the next clean lap (click to cancel)"
                                                          : ">> Re-record this track map");
            }
        }

        const int beepDropLabelRow = n;
        char beepDropLabel[64];
        if (g_brakeCueConfig.minSpeedDropKmh < 0.5f)
            strcpy_s(beepDropLabel, "Brake beep: every braking point");
        else
            sprintf_s(beepDropLabel, "Brake beep: only stops losing %.0f+ km/h", g_brakeCueConfig.minSpeedDropKmh);
        addInfo(beepDropLabel);

        // Cover boxes: how many (each placed and sized in layout mode), and
        // their one shared shade.
        int coverGreyRow = -1;
        char coverGreyLabel[48];
        int coverAlphaRow = -1;
        char coverAlphaLabel[48];
        {
            char coverLine[96];
            sprintf_s(coverLine, "Cover boxes: %d  (click for %s)", g_coverBoxConfig.count,
                      g_coverBoxConfig.count < CoverBoxConfig::kMaxBoxes ? "one more" : "just one");
            addSettingRow(16, coverLine);
            coverGreyRow = n;
            sprintf_s(coverGreyLabel, "Cover box shade (%d%% white)",
                      static_cast<int>(g_coverBoxConfig.grey * 100.0f + 0.5f));
            addInfo(coverGreyLabel);
            coverAlphaRow = n;
            sprintf_s(coverAlphaLabel, "Cover box opacity (%d%%)",
                      static_cast<int>(g_coverBoxConfig.a * 100.0f + 0.5f));
            addInfo(coverAlphaLabel);
        }
        // Text font and weight, each cycled by clicking.
        {
            char fontLine[96];
            if (g_textFontConfig.font == 0)
                sprintf_s(fontLine, "Text font: Auto (%s)  (click to change)", renderer.ActiveFontFace());
            else
                sprintf_s(fontLine, "Text font: %s  (click to change)", renderer.ActiveFontFace());
            addSettingRow(17, fontLine);
            sprintf_s(fontLine, "Text weight: %s  (click to change)", Renderer::kWeightNames[g_textFontConfig.weight]);
            addSettingRow(18, fontLine);
        }
        char versionLine[32];
        sprintf_s(versionLine, "ver. %s", RF2RO_VERSION);
        addInfo(versionLine);
        addInfo("Created by Paul Kiernan, Shamrock Kiernan Racing (SKR)");
        addInfo("Discord: paul_kiernan_27");
        addInfo("Thanks to the SRGP community (simracing-gp.net) for testing & feedback");
        addInfo("Thanks to TinyPedal (open source, GPL-3, github.com/TinyPedal) for help with the code");
        addInfo("Guaranteed* to make you 0.3s faster (*not guaranteed)");

        const int totalLines = n;

        // Now that widestKeyPx is final, resolve how wide each hotkey row
        // actually is (key column + gap + its own label) and fold that into
        // the panel width alongside the info rows measured earlier.
        const float labelColumnX = widestKeyPx + keyLabelGapPx;
        for (int i = 0; i < totalLines; ++i)
        {
            if (rowIsInfo[i]) continue;
            const float w = labelColumnX + renderer.MeasureText(labelPart[i], textPx);
            if (w > widest) widest = w;
        }

        // Each row's own vertical pitch, sized off its OWN font size rather
        // than one pitch shared with the (larger) hotkey rows -- an info row
        // drawn at infoPx but spaced as if it were textPx read as double-
        // spaced, most visibly in the version/credits block at the bottom.
        const float infoLineStep = infoPx * 1.2f;
        // Extra room reserved under the cars-shown label for the actual
        // slider bar+knob, which isn't part of the generic text-row system.
        const float carsSliderBarExtraPx = lineStep * 1.1f;
        float rowStep[kMaxLines];
        float rowY[kMaxLines];
        float cumulativeY = 0.0f;
        for (int i = 0; i < totalLines; ++i)
        {
            rowStep[i] = rowIsInfo[i] ? infoLineStep : lineStep;
            if (i == carsSliderLabelRow || i == mapRotLabelRow || i == mapSectorsLabelRow
                || i == coverGreyRow || i == coverAlphaRow || i == beepDropLabelRow)
                rowStep[i] += carsSliderBarExtraPx;
            rowY[i] = cumulativeY;
            cumulativeY += rowStep[i];
        }
        // Flush to the LEFT edge, below the fuel/tyre panel -- anchored to
        // that panel's real bottom edge rather than a fixed fraction, so a
        // taller panel (more rows in a race) pushes this down instead of
        // being overlapped by it.
        // -1 means "auto-anchor below the fuel/tyre panel's real bottom
        // edge", which is what this did before it was draggable -- a taller
        // panel pushed the list down rather than overlapping it. The first
        // drag resolves the sentinel, and the list is fixed from then on:
        // that auto-anchoring is given up in exchange for being placeable.
        if (cfg.listPosYFraction < 0.0f || cfg.listPosXFraction < 0.0f)
        {
            g_shortcutConfig.listPosXFraction = padding / screenWidth;
            g_shortcutConfig.listPosYFraction =
                (FuelTyrePanel::BottomEdgeY(screenWidth, screenHeight) + padding) / screenHeight;
        }
        const float leftX = screenWidth * cfg.listPosXFraction;
        const float topY = screenHeight * cfg.listPosYFraction;

        // Scrolling: the rows get whatever is left between the title and the
        // bottom of the screen. If they all fit, nothing changes; otherwise
        // only a window of whole rows is drawn and a scroll bar appears in an
        // extra strip on the right of the panel.
        const float titleBlockH = titlePx + lineStep * 0.5f;
        float viewH = screenHeight - padding * 2.0f - (topY + titleBlockH);
        if (viewH < lineStep * 3.0f) viewH = lineStep * 3.0f;
        const bool scrolling = cumulativeY > viewH;
        int maxFirstRow = 0;
        if (scrolling)
        {
            // The last first-row that still fills the view down to the end.
            maxFirstRow = totalLines - 1;
            while (maxFirstRow > 0 && cumulativeY - rowY[maxFirstRow - 1] <= viewH)
                --maxFirstRow;
        }
        if (g_listFirstRow > maxFirstRow) g_listFirstRow = maxFirstRow;
        if (g_listFirstRow < 0) g_listFirstRow = 0;
        const float scrollBarStripW = scrolling ? 26.0f * listScale : 0.0f;
        const float panelW = widest + scrollBarStripW;
        const float shownRowsH = scrolling ? viewH : cumulativeY;
        const float blockH = titleBlockH + shownRowsH;

        Layout::Register("Shortcut list", "Shortcuts", "ListPosXFraction", "ListPosYFraction",
                         &g_shortcutConfig.listPosXFraction, &g_shortcutConfig.listPosYFraction,
                         leftX + panelW * 0.5f, topY + blockH * 0.5f,
                         panelW * 0.5f + padding, blockH * 0.5f + padding,
                         &g_shortcutConfig.listScale, "ListScale");

        renderer.AddFilledRoundedRect(leftX + panelW * 0.5f, topY + blockH * 0.5f,
                                      panelW * 0.5f + padding, blockH * 0.5f + padding,
                                      10.0f * listScale, 0.0f,
                                      0.02f, 0.02f, 0.02f, 0.95f);

        float y = topY;
        renderer.AddText(leftX, y, "Overlay shortcuts", titlePx, 1.0f, 1.0f, 1.0f, 1.0f);
        // Text size: three boxes at the right of the title line.
        float sizeBoxX[3];
        const float sizeBoxCY = y + titlePx * 0.5f;
        {
            static const char* const kSizeName[3] = { "S", "M", "L" };
            for (int i = 0; i < 3; ++i)
            {
                sizeBoxX[i] = leftX + panelW - sizeBoxesW + sizeBoxPx * 0.5f + i * (sizeBoxPx + sizeBoxGap);
                const bool on = i == sizeIdx;
                renderer.AddFilledRoundedRect(sizeBoxX[i], sizeBoxCY, sizeBoxPx * 0.5f, sizeBoxPx * 0.5f, 4.0f * listScale, 0.0f,
                                              on ? 0.30f : 0.16f, on ? 0.55f : 0.16f, on ? 0.95f : 0.16f, 1.0f);
                renderer.AddText(sizeBoxX[i], sizeBoxCY - titlePx * 0.42f, kSizeName[i], titlePx * 0.8f,
                                 1.0f, 1.0f, 1.0f, on ? 1.0f : 0.7f, Renderer::HAlign::Center);
            }
        }
        y += titlePx + lineStep * 0.5f;

        // Clicking a row toggles it, so the hotkeys do not have to be
        // recalled at all. Only live while the cursor is actually visible --
        // see Layout::CursorPosition. The click still reaches the game
        // underneath (a plugin cannot consume input without hooking the
        // window procedure), but with the cursor showing you are by
        // definition not mid-corner.
        float cursorX = 0.0f, cursorY = 0.0f;
        const bool haveCursor = Layout::CursorPosition(hwnd, screenWidth, screenHeight, cursorX, cursorY);
        const bool clickNow = haveCursor && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool clickEdge = clickNow && !g_listClickDown;
        g_listClickDown = clickNow;

        // S / M / L text size boxes.
        if (clickEdge && fabsf(cursorY - sizeBoxCY) <= sizeBoxPx * 0.5f)
            for (int i = 0; i < 3; ++i)
                if (fabsf(cursorX - sizeBoxX[i]) <= sizeBoxPx * 0.5f)
                {
                    g_shortcutConfig.listTextSize = i;
                    { char v[4]; sprintf_s(v, "%d", i); Config::WriteString("Shortcuts", "ListTextSize", v); }
                    clickEdge = false;
                    QueryPerformanceCounter(&g_listTime);
                }

        // Move (title strip) and resize (corner squares) while the list is
        // shown. Layout mode does its own dragging, so this stays out of it.
        if (!Layout::Active())
        {
            const float boxL = leftX - padding, boxT = topY - padding;
            const float boxR = leftX + panelW + padding, boxB = topY + blockH + padding;
            const float handle = 14.0f * listScale;
            for (int c = 0; c < 4; ++c)
            {
                const float hx = (c & 1) ? boxL : boxR, hy = (c & 2) ? boxT : boxB;
                renderer.AddFilledRect(hx, hy, handle * 0.5f, handle * 0.5f, 0.0f, 0.45f, 0.75f, 1.0f, 0.9f);
            }
            if (clickEdge)
            {
                for (int c = 0; c < 4 && !g_listResizing; ++c)
                {
                    const float hx = (c & 1) ? boxL : boxR, hy = (c & 2) ? boxT : boxB;
                    if (fabsf(cursorX - hx) <= handle && fabsf(cursorY - hy) <= handle)
                    {
                        g_listResizing = true;
                        g_listSideX = (c & 1) ? 1.0f : -1.0f;   // anchor = opposite corner
                        g_listSideY = (c & 2) ? 1.0f : -1.0f;
                        g_listAnchorX = g_listSideX > 0.0f ? boxR : boxL;
                        g_listAnchorY = g_listSideY > 0.0f ? boxB : boxT;
                        g_listStartDist = (std::max)(1.0f, hypotf(cursorX - g_listAnchorX, cursorY - g_listAnchorY));
                        g_listStartScale = cfg.listScale;
                    }
                }
                if (!g_listResizing && cursorX >= boxL && cursorX <= boxR && cursorY >= boxT && cursorY <= topY + titlePx)
                {
                    g_listMoving = true;
                    g_listGrabDX = cursorX - leftX;
                    g_listGrabDY = cursorY - topY;
                }
                if (g_listResizing || g_listMoving) clickEdge = false;   // not a row click
            }
            if (g_listMoving)
            {
                if (clickNow)
                {
                    g_shortcutConfig.listPosXFraction = (std::min)(0.98f, (std::max)(0.0f, (cursorX - g_listGrabDX) / screenWidth));
                    g_shortcutConfig.listPosYFraction = (std::min)(0.98f, (std::max)(0.0f, (cursorY - g_listGrabDY) / screenHeight));
                }
                else
                {
                    g_listMoving = false;
                    Config::WriteFloat("Shortcuts", "ListPosXFraction", g_shortcutConfig.listPosXFraction);
                    Config::WriteFloat("Shortcuts", "ListPosYFraction", g_shortcutConfig.listPosYFraction);
                }
            }
            if (g_listResizing)
            {
                if (clickNow)
                {
                    const float dist = hypotf(cursorX - g_listAnchorX, cursorY - g_listAnchorY);
                    const float newScale = (std::min)(2.5f, (std::max)(0.4f, g_listStartScale * dist / g_listStartDist));
                    // Everything in the list scales with listScale, so the box
                    // does too: place it so the anchored corner stays put.
                    const float k = cfg.listScale > 0.0f ? newScale / cfg.listScale : 1.0f;
                    const float newW = (boxR - boxL) * k, newH = (boxB - boxT) * k;
                    const float newBoxL = g_listSideX > 0.0f ? g_listAnchorX - newW : g_listAnchorX;
                    const float newBoxT = g_listSideY > 0.0f ? g_listAnchorY - newH : g_listAnchorY;
                    g_shortcutConfig.listPosXFraction = (newBoxL + padding * k) / screenWidth;
                    g_shortcutConfig.listPosYFraction = (newBoxT + padding * k) / screenHeight;
                    g_shortcutConfig.listScale = newScale;
                }
                else
                {
                    g_listResizing = false;
                    Config::WriteFloat("Shortcuts", "ListScale", g_shortcutConfig.listScale);
                    Config::WriteFloat("Shortcuts", "ListPosXFraction", g_shortcutConfig.listPosXFraction);
                    Config::WriteFloat("Shortcuts", "ListPosYFraction", g_shortcutConfig.listPosYFraction);
                }
            }
            if (g_listMoving || g_listResizing)
                QueryPerformanceCounter(&g_listTime);   // keep it open while being handled
        }

        const float rowsTopY = y;

        // --- Scroll bar: drag the thumb, or click the track above/below it
        // to move a page. Handled before the rows so the rows below use the
        // offset this frame actually draws with.
        if (scrolling)
        {
            const float maxOffset = rowY[maxFirstRow];
            const float barX = leftX + widest + padding * 0.5f + scrollBarStripW * 0.5f;
            const float barHalfW = 5.0f * listScale;
            const float trackTop = rowsTopY;
            const float trackH = viewH;
            float thumbH = trackH * viewH / cumulativeY;
            if (thumbH < lineStep * 2.0f) thumbH = lineStep * 2.0f;
            if (thumbH > trackH) thumbH = trackH;
            auto thumbTopFor = [&](float offset)
            {
                return trackTop + (trackH - thumbH) * (maxOffset > 0.0f ? offset / maxOffset : 0.0f);
            };
            // The whole-row first row closest to a pixel offset.
            auto rowForOffset = [&](float offset)
            {
                int best = 0;
                for (int i = 1; i <= maxFirstRow; ++i)
                    if (fabsf(rowY[i] - offset) < fabsf(rowY[best] - offset))
                        best = i;
                return best;
            };

            const float thumbTop = thumbTopFor(rowY[g_listFirstRow]);
            const bool overBar = haveCursor
                && cursorX >= barX - scrollBarStripW * 0.5f - padding * 0.5f
                && cursorX <= barX + scrollBarStripW * 0.5f + padding * 0.5f
                && cursorY >= trackTop && cursorY <= trackTop + trackH;
            const bool overThumb = overBar && cursorY >= thumbTop && cursorY <= thumbTop + thumbH;
            if (clickEdge && overThumb)
            {
                g_listScrollDragging = true;
                g_listScrollGrabPx = cursorY - thumbTop;
            }
            else if (clickEdge && overBar)
            {
                const float page = viewH * 0.9f;
                const float target = rowY[g_listFirstRow] + (cursorY < thumbTop ? -page : page);
                g_listFirstRow = rowForOffset(target < 0.0f ? 0.0f : (target > maxOffset ? maxOffset : target));
            }
            if (!clickNow)
                g_listScrollDragging = false;
            if (g_listScrollDragging && haveCursor && trackH > thumbH)
            {
                float f = (cursorY - g_listScrollGrabPx - trackTop) / (trackH - thumbH);
                if (f < 0.0f) f = 0.0f;
                if (f > 1.0f) f = 1.0f;
                g_listFirstRow = rowForOffset(f * maxOffset);
            }

            const float drawThumbTop = thumbTopFor(rowY[g_listFirstRow]);
            const bool thumbActive = g_listScrollDragging || overThumb;
            renderer.AddFilledRoundedRect(barX, trackTop + trackH * 0.5f, barHalfW, trackH * 0.5f,
                                          barHalfW, 0.0f, 0.4f, 0.4f, 0.45f, 0.5f);
            renderer.AddFilledRoundedRect(barX, drawThumbTop + thumbH * 0.5f, barHalfW, thumbH * 0.5f,
                                          barHalfW, 0.0f,
                                          thumbActive ? 1.0f : 0.85f, thumbActive ? 0.85f : 0.8f,
                                          thumbActive ? 0.2f : 0.8f, 0.95f);
        }
        else
        {
            g_listScrollDragging = false;
        }

        // Rows shown this frame: from the first scrolled-to row down to the
        // last that fits whole in the view.
        const int firstShown = scrolling ? g_listFirstRow : 0;
        const float scrollOffset = rowY[firstShown];
        int endShown = totalLines;
        if (scrolling)
        {
            endShown = firstShown;
            while (endShown < totalLines && rowY[endShown] + rowStep[endShown] - scrollOffset <= viewH + 0.5f)
                ++endShown;
        }

        int hoveredRow = -1;
        if (haveCursor && !g_listScrollDragging
            && cursorX >= leftX - padding && cursorX <= leftX + widest + (scrolling ? 0.0f : padding)
            && cursorY >= rowsTopY)
        {
            // Walk the cumulative per-row offsets rather than dividing by one
            // flat pitch -- info rows are pitched tighter than hotkey rows
            // now, so a single division no longer lands on the right row.
            for (int i = firstShown; i < endShown; ++i)
            {
                const float rowTop = rowsTopY + rowY[i] - scrollOffset;
                if (cursorY >= rowTop && cursorY < rowTop + rowStep[i])
                {
                    // Only the ELEMENT rows toggle. The action rows below
                    // them run resets and mode changes, which must not fire
                    // from a stray click on a list you opened to read.
                    if (rowElementIndex[i] >= 0 || rowCustom[i] >= 0)
                        hoveredRow = i;
                    break;
                }
            }
        }

        for (int i = firstShown; i < endShown; ++i)
        {
            if (i == hoveredRow)
            {
                renderer.AddFilledRect(leftX + widest * 0.5f, y + textPx * 0.5f,
                                       widest * 0.5f + padding * 0.5f, rowStep[i] * 0.5f, 0.0f,
                                       1.0f, 1.0f, 1.0f, 0.16f);
            }
            // Hidden elements are dimmed; info lines are dimmer still (and
            // smaller, since they are context, not something to press); the
            // action lines are always full brightness.
            const int elemIdx = rowElementIndex[i];
            float shade = 1.0f;
            float sizePx = textPx;
            if (rowIsInfo[i]) { shade = 0.62f; sizePx = infoPx; }
            else if (elemIdx >= 0 && !g_entries[elemIdx].visible) shade = 0.55f;
            if (rowIsInfo[i])
            {
                renderer.AddText(leftX, y, lines[i], sizePx, shade, shade, shade, 1.0f);

                if (i == coverGreyRow)
                {
                    // 0..100% white in 1% steps, live while dragging, written
                    // to the INI on release. The track shows the shade itself.
                    float& grey = g_coverBoxConfig.grey;
                    const float trackY = y + infoLineStep + carsSliderBarExtraPx * 0.5f;
                    const float trackLeft = leftX;
                    const float trackRight = leftX + widest;
                    const float trackHalfH = 3.0f * listScale;
                    const float knobR = 9.0f * listScale;
                    const float knobX = trackLeft + (trackRight - trackLeft) * grey;
                    const bool nearSlider = haveCursor
                        && cursorX >= trackLeft - knobR && cursorX <= trackRight + knobR
                        && cursorY >= trackY - knobR * 1.6f && cursorY <= trackY + knobR * 1.6f;
                    if (clickEdge && nearSlider)
                        g_coverGreyDragging = true;
                    if (!clickNow && g_coverGreyDragging)
                    {
                        g_coverGreyDragging = false;
                        Config::WriteFloat("CoverBox", "Grey", grey);
                    }
                    if (g_coverGreyDragging && haveCursor)
                    {
                        float f = (cursorX - trackLeft) / (trackRight - trackLeft);
                        if (f < 0.0f) f = 0.0f;
                        if (f > 1.0f) f = 1.0f;
                        grey = floorf(f * 100.0f + 0.5f) / 100.0f;
                    }
                    const bool sliderActive = g_coverGreyDragging || nearSlider;
                    renderer.AddFilledRoundedRect(trackLeft + (trackRight - trackLeft) * 0.5f, trackY,
                                                  (trackRight - trackLeft) * 0.5f, trackHalfH, trackHalfH, 0.0f,
                                                  0.4f, 0.4f, 0.45f, 0.8f);
                    // Filled part in the chosen shade, so the value reads at a glance.
                    renderer.AddFilledRoundedRect(trackLeft + (knobX - trackLeft) * 0.5f, trackY,
                                                  (knobX - trackLeft) * 0.5f, trackHalfH, trackHalfH, 0.0f,
                                                  grey, grey, grey, 1.0f);
                    renderer.AddFilledCircle(knobX, trackY, knobR,
                                             sliderActive ? 1.0f : 0.85f, sliderActive ? 0.85f : 0.8f, 0.2f, 1.0f);
                }

                if (i == beepDropLabelRow)
                {
                    // 0..150 km/h in 5 km/h steps; written to the INI on release.
                    BrakeCueConfig& bcc = g_brakeCueConfig;
                    const float kMin = BrakeCueConfig::kMinSpeedDropMin;
                    const float kMax = BrakeCueConfig::kMinSpeedDropMax;
                    const float trackY = y + infoLineStep + carsSliderBarExtraPx * 0.5f;
                    const float trackLeft = leftX;
                    const float trackRight = leftX + widest;
                    const float trackHalfH = 3.0f * listScale;
                    const float knobR = 9.0f * listScale;
                    float valueFrac = (bcc.minSpeedDropKmh - kMin) / (kMax - kMin);
                    if (valueFrac < 0.0f) valueFrac = 0.0f;
                    if (valueFrac > 1.0f) valueFrac = 1.0f;
                    const float knobX = trackLeft + (trackRight - trackLeft) * valueFrac;
                    const bool nearSlider = haveCursor
                        && cursorX >= trackLeft - knobR && cursorX <= trackRight + knobR
                        && cursorY >= trackY - knobR * 1.6f && cursorY <= trackY + knobR * 1.6f;
                    if (clickEdge && nearSlider)
                        g_beepDropDragging = true;
                    if (!clickNow && g_beepDropDragging)
                    {
                        g_beepDropDragging = false;
                        Config::WriteFloat("BrakeCue", "MinSpeedDropKmh", bcc.minSpeedDropKmh);
                    }
                    if (g_beepDropDragging && haveCursor)
                    {
                        float f = (cursorX - trackLeft) / (trackRight - trackLeft);
                        if (f < 0.0f) f = 0.0f;
                        if (f > 1.0f) f = 1.0f;
                        bcc.minSpeedDropKmh = floorf((kMin + f * (kMax - kMin)) / 5.0f + 0.5f) * 5.0f;
                    }
                    const bool sliderActive = g_beepDropDragging || nearSlider;
                    renderer.AddFilledRoundedRect(trackLeft + (trackRight - trackLeft) * 0.5f, trackY,
                                                  (trackRight - trackLeft) * 0.5f, trackHalfH, trackHalfH, 0.0f,
                                                  0.4f, 0.4f, 0.45f, 0.8f);
                    renderer.AddFilledRoundedRect(trackLeft + (knobX - trackLeft) * 0.5f, trackY,
                                                  (knobX - trackLeft) * 0.5f, trackHalfH, trackHalfH, 0.0f,
                                                  0.35f, 0.85f, 1.0f, 0.9f);
                    renderer.AddFilledCircle(knobX, trackY, knobR,
                                             sliderActive ? 1.0f : 0.85f, sliderActive ? 0.85f : 0.8f, 0.2f, 1.0f);
                }

                if (i == coverAlphaRow)
                {
                    // 0..100% opacity in 1% steps, live while dragging,
                    // written to the INI on release. The filled part shows
                    // the box's shade at the chosen opacity.
                    float& alpha = g_coverBoxConfig.a;
                    const float grey = g_coverBoxConfig.grey;
                    const float trackY = y + infoLineStep + carsSliderBarExtraPx * 0.5f;
                    const float trackLeft = leftX;
                    const float trackRight = leftX + widest;
                    const float trackHalfH = 3.0f * listScale;
                    const float knobR = 9.0f * listScale;
                    const float knobX = trackLeft + (trackRight - trackLeft) * alpha;
                    const bool nearSlider = haveCursor
                        && cursorX >= trackLeft - knobR && cursorX <= trackRight + knobR
                        && cursorY >= trackY - knobR * 1.6f && cursorY <= trackY + knobR * 1.6f;
                    if (clickEdge && nearSlider)
                        g_coverAlphaDragging = true;
                    if (!clickNow && g_coverAlphaDragging)
                    {
                        g_coverAlphaDragging = false;
                        Config::WriteFloat("CoverBox", "Alpha", alpha);
                    }
                    if (g_coverAlphaDragging && haveCursor)
                    {
                        float f = (cursorX - trackLeft) / (trackRight - trackLeft);
                        if (f < 0.0f) f = 0.0f;
                        if (f > 1.0f) f = 1.0f;
                        alpha = floorf(f * 100.0f + 0.5f) / 100.0f;
                    }
                    const bool sliderActive = g_coverAlphaDragging || nearSlider;
                    renderer.AddFilledRoundedRect(trackLeft + (trackRight - trackLeft) * 0.5f, trackY,
                                                  (trackRight - trackLeft) * 0.5f, trackHalfH, trackHalfH, 0.0f,
                                                  0.4f, 0.4f, 0.45f, 0.8f);
                    renderer.AddFilledRoundedRect(trackLeft + (knobX - trackLeft) * 0.5f, trackY,
                                                  (knobX - trackLeft) * 0.5f, trackHalfH, trackHalfH, 0.0f,
                                                  grey, grey, grey, alpha);
                    renderer.AddFilledCircle(knobX, trackY, knobR,
                                             sliderActive ? 1.0f : 0.85f, sliderActive ? 0.85f : 0.8f, 0.2f, 1.0f);
                }

                if (i == mapSectorsLabelRow)
                {
                    // 10..200 mini-sectors in steps of 5; live while dragging
                    // (the lap's colours start over at each new count),
                    // written to the INI on release.
                    TrackMapConfig& tmc = g_trackMapConfig;
                    const float kMin = static_cast<float>(TrackMapConfig::kMinDeltaSectors);
                    const float kMax = static_cast<float>(TrackMapConfig::kMaxDeltaSectors);
                    const float trackY = y + infoLineStep + carsSliderBarExtraPx * 0.5f;
                    const float trackLeft = leftX;
                    const float trackRight = leftX + widest;
                    const float trackHalfH = 3.0f * listScale;
                    const float knobR = 9.0f * listScale;
                    float valueFrac = (static_cast<float>(tmc.deltaSectorCount) - kMin) / (kMax - kMin);
                    if (valueFrac < 0.0f) valueFrac = 0.0f;
                    if (valueFrac > 1.0f) valueFrac = 1.0f;
                    const float knobX = trackLeft + (trackRight - trackLeft) * valueFrac;
                    const bool nearSlider = haveCursor
                        && cursorX >= trackLeft - knobR && cursorX <= trackRight + knobR
                        && cursorY >= trackY - knobR * 1.6f && cursorY <= trackY + knobR * 1.6f;
                    if (clickEdge && nearSlider)
                        g_mapSectorsDragging = true;
                    if (!clickNow && g_mapSectorsDragging)
                    {
                        g_mapSectorsDragging = false;
                        Config::WriteFloat("TrackMap", "DeltaSectorCount", static_cast<float>(tmc.deltaSectorCount));
                    }
                    if (g_mapSectorsDragging && haveCursor)
                    {
                        float f = (cursorX - trackLeft) / (trackRight - trackLeft);
                        if (f < 0.0f) f = 0.0f;
                        if (f > 1.0f) f = 1.0f;
                        tmc.deltaSectorCount = static_cast<int>(floorf((kMin + f * (kMax - kMin)) / 5.0f + 0.5f)) * 5;
                    }
                    const bool sliderActive = g_mapSectorsDragging || nearSlider;
                    renderer.AddFilledRoundedRect(trackLeft + (trackRight - trackLeft) * 0.5f, trackY,
                                                  (trackRight - trackLeft) * 0.5f, trackHalfH, trackHalfH, 0.0f,
                                                  0.4f, 0.4f, 0.45f, 0.8f);
                    renderer.AddFilledRoundedRect(trackLeft + (knobX - trackLeft) * 0.5f, trackY,
                                                  (knobX - trackLeft) * 0.5f, trackHalfH, trackHalfH, 0.0f,
                                                  0.35f, 0.85f, 1.0f, 0.9f);
                    renderer.AddFilledCircle(knobX, trackY, knobR,
                                             sliderActive ? 1.0f : 0.85f, sliderActive ? 0.85f : 0.8f, 0.2f, 1.0f);
                }

                if (i == mapRotLabelRow)
                {
                    // -180..180 degrees in whole degrees, live while dragging,
                    // saved for the circuit on release.
                    const float kMin = -180.0f, kMax = 180.0f;
                    const float trackY = y + infoLineStep + carsSliderBarExtraPx * 0.5f;
                    const float trackLeft = leftX;
                    const float trackRight = leftX + widest;
                    const float trackHalfH = 3.0f * listScale;
                    const float knobR = 9.0f * listScale;
                    float valueFrac = (TrackMap::RotationDegrees() - kMin) / (kMax - kMin);
                    if (valueFrac < 0.0f) valueFrac = 0.0f;
                    if (valueFrac > 1.0f) valueFrac = 1.0f;
                    const float knobX = trackLeft + (trackRight - trackLeft) * valueFrac;
                    const bool nearSlider = haveCursor
                        && cursorX >= trackLeft - knobR && cursorX <= trackRight + knobR
                        && cursorY >= trackY - knobR * 1.6f && cursorY <= trackY + knobR * 1.6f;
                    if (clickEdge && nearSlider)
                        g_mapRotDragging = true;
                    if (!clickNow && g_mapRotDragging)
                    {
                        g_mapRotDragging = false;
                        TrackMap::SetRotationDegrees(TrackMap::RotationDegrees(), true);
                    }
                    if (g_mapRotDragging && haveCursor)
                    {
                        float f = (cursorX - trackLeft) / (trackRight - trackLeft);
                        if (f < 0.0f) f = 0.0f;
                        if (f > 1.0f) f = 1.0f;
                        TrackMap::SetRotationDegrees(floorf(kMin + f * (kMax - kMin) + 0.5f), false);
                    }
                    const bool sliderActive = g_mapRotDragging || nearSlider;
                    renderer.AddFilledRoundedRect(trackLeft + (trackRight - trackLeft) * 0.5f, trackY,
                                                  (trackRight - trackLeft) * 0.5f, trackHalfH, trackHalfH, 0.0f,
                                                  0.4f, 0.4f, 0.45f, 0.8f);
                    // Centre tick: no rotation.
                    renderer.AddFilledRect(trackLeft + (trackRight - trackLeft) * 0.5f, trackY,
                                           1.5f * listScale, trackHalfH * 2.5f, 0.0f, 1.0f, 1.0f, 1.0f, 0.6f);
                    renderer.AddFilledCircle(knobX, trackY, knobR,
                                             sliderActive ? 1.0f : 0.85f, sliderActive ? 0.85f : 0.8f, 0.2f, 1.0f);
                }

                if (i == carsSliderLabelRow)
                {
                    // Absolute-position slider (knob position directly
                    // encodes the value) -- same feel as ProximityHue's own
                    // sliders. Track spans the panel's own width so it lines
                    // up under the label rather than looking like an
                    // afterthought squeezed in at some arbitrary width.
                    const float trackY = y + infoLineStep + carsSliderBarExtraPx * 0.5f;
                    const float trackLeft = leftX;
                    const float trackRight = leftX + widest;
                    const float trackHalfH = 3.0f * listScale;
                    const float knobR = 9.0f * listScale;

                    float valueFrac = (g_relativeTimingConfig.visibleCars - RelativeTimingConfig::kMinVisibleCars)
                                     / (RelativeTimingConfig::kMaxVisibleCars - RelativeTimingConfig::kMinVisibleCars);
                    if (valueFrac < 0.0f) valueFrac = 0.0f;
                    if (valueFrac > 1.0f) valueFrac = 1.0f;
                    const float knobX = trackLeft + (trackRight - trackLeft) * valueFrac;

                    const bool nearSlider = haveCursor
                        && cursorX >= trackLeft - knobR && cursorX <= trackRight + knobR
                        && cursorY >= trackY - knobR * 1.6f && cursorY <= trackY + knobR * 1.6f;
                    if (clickEdge && nearSlider)
                        g_carsSliderDragging = true;
                    if (!clickNow)
                        g_carsSliderDragging = false;
                    if (g_carsSliderDragging && haveCursor)
                    {
                        float f = (cursorX - trackLeft) / (trackRight - trackLeft);
                        if (f < 0.0f) f = 0.0f;
                        if (f > 1.0f) f = 1.0f;
                        const float newValue = floorf(RelativeTimingConfig::kMinVisibleCars
                            + f * (RelativeTimingConfig::kMaxVisibleCars - RelativeTimingConfig::kMinVisibleCars) + 0.5f);
                        if (newValue != g_relativeTimingConfig.visibleCars)
                        {
                            g_relativeTimingConfig.visibleCars = newValue;
                            Config::WriteFloat("RelativeTiming", "VisibleCars", newValue);
                        }
                    }

                    const bool sliderActive = g_carsSliderDragging || nearSlider;
                    renderer.AddFilledRoundedRect(trackLeft + (trackRight - trackLeft) * 0.5f, trackY,
                                                  (trackRight - trackLeft) * 0.5f, trackHalfH, trackHalfH, 0.0f,
                                                  0.4f, 0.4f, 0.45f, 0.8f);
                    renderer.AddFilledRoundedRect(trackLeft + (knobX - trackLeft) * 0.5f, trackY,
                                                  (knobX - trackLeft) * 0.5f, trackHalfH, trackHalfH, 0.0f,
                                                  0.35f, 0.85f, 1.0f, 0.9f);
                    renderer.AddFilledCircle(knobX, trackY, knobR,
                                             sliderActive ? 1.0f : 0.85f, sliderActive ? 0.85f : 0.8f, 0.2f, 1.0f);
                }
            }
            else
            {
                // Two fixed columns -- the key text left-aligned in its own
                // column, the label starting at the same x on every row --
                // rather than one string per row, whose label x drifted with
                // however wide that row's own key name happened to be.
                renderer.AddText(leftX, y, keyPart[i], sizePx, shade, shade, shade, 1.0f);
                renderer.AddText(leftX + labelColumnX, y, labelPart[i], sizePx, shade, shade, shade, 1.0f);
            }
            y += rowStep[i];
        }

        if (clickEdge && hoveredRow >= 0 && rowCustom[hoveredRow] >= 0)
        {
            switch (rowCustom[hoveredRow])
            {
            case 8:
                g_gearDisplayConfig.showAbs = !g_gearDisplayConfig.showAbs;
                Config::WriteBool("GearDisplay", "ShowAbs", g_gearDisplayConfig.showAbs);
                break;
            case 9:
                g_gearDisplayConfig.showTc = !g_gearDisplayConfig.showTc;
                Config::WriteBool("GearDisplay", "ShowTc", g_gearDisplayConfig.showTc);
                break;
            case 14:
                TrackMap::ToggleMirror();
                break;
            case 15:
                TrackMap::ToggleRerecord();
                break;
            case 16:
                g_coverBoxConfig.count = g_coverBoxConfig.count % CoverBoxConfig::kMaxBoxes + 1;
                Config::WriteFloat("CoverBox", "Count", static_cast<float>(g_coverBoxConfig.count));
                break;
            case 13:
                g_trackMapConfig.deltaColourMode = (g_trackMapConfig.deltaColourMode + 1) % 4;
                Config::WriteFloat("TrackMap", "DeltaColourMode", static_cast<float>(g_trackMapConfig.deltaColourMode));
                break;
            case 17:
                g_textFontConfig.font = (g_textFontConfig.font + 1) % Renderer::kFontCount;
                Config::WriteString("Text", "Font", Renderer::kFontNames[g_textFontConfig.font]);
                renderer.SetFont(g_textFontConfig.font, g_textFontConfig.weight);
                break;
            case 18:
                g_textFontConfig.weight = (g_textFontConfig.weight + 1) % Renderer::kWeightCount;
                Config::WriteString("Text", "FontWeight", Renderer::kWeightNames[g_textFontConfig.weight]);
                renderer.SetFont(g_textFontConfig.font, g_textFontConfig.weight);
                break;
            case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 27: case 28: case 29:
                ToggleListEntry(1000 + rowCustom[hoveredRow]);
                break;
            case 26:
                ToggleListEntry(kResetTowerCode);
                strcpy_s(g_toastText, "Timing tower reset");
                ShowPopup(Popup::Toast, 1.5);
                break;
            }
            QueryPerformanceCounter(&g_listTime);
        }
        else if (clickEdge && hoveredRow >= 0)
        {
            const int code = rowElementIndex[hoveredRow];
            ToggleListEntry(code);
            if (g_popup != Popup::Warning)
            {
                sprintf_s(g_toastText, "%s %s", ListEntryLabel(code),
                          code == kUpdateCheckCode ? (ListEntryOn(code) ? "on" : "off") : (ListEntryOn(code) ? "shown" : "hidden"));
                ShowPopup(Popup::Toast, 1.5);
            }
            // Unlike a hotkey, clicking does NOT close the list: the whole
            // point is turning several things on and off in a row. The idle
            // timer is restarted instead, so it stays up while being used.
            QueryPerformanceCounter(&g_listTime);
        }
    }

    // --- Transient message (toggle confirmation, startup hint, INI warning)
    const float alpha = PopupAlpha();
    if (alpha > 0.0f)
    {
        const float msgScale = uiScale * cfg.messageScale;
        const float textPx = (g_popup == Popup::Warning ? 38.0f : 44.0f) * msgScale;
        const float padding = 20.0f * msgScale;
        const float centerX = screenWidth * cfg.messageCenterXFraction;
        const float centerY = screenHeight * cfg.messageCenterYFraction;
        const float halfW = renderer.MeasureText(g_toastText, textPx) * 0.5f + padding;
        const float halfH = textPx * 0.5f + padding;

        Layout::Register("Messages", "Shortcuts", "MessageCenterXFraction", "MessageCenterYFraction",
                         &g_shortcutConfig.messageCenterXFraction, &g_shortcutConfig.messageCenterYFraction,
                         centerX, centerY, halfW, halfH,
                         &g_shortcutConfig.messageScale, "MessageScale");

        const bool warn = (g_popup == Popup::Warning);
        renderer.AddFilledRoundedRect(centerX, centerY, halfW, halfH, 10.0f * msgScale, 0.0f,
                                      warn ? 0.35f : 0.08f, warn ? 0.06f : 0.08f, warn ? 0.06f : 0.08f, 0.6f * alpha);
        renderer.AddText(centerX, centerY - textPx * 0.5f, g_toastText, textPx,
                         1.0f, warn ? 0.7f : 1.0f, warn ? 0.4f : 1.0f, alpha, Renderer::HAlign::Center);
    }

    // --- Rotary setting box. For 10 s after the centre rotary / on-off
    // button (or Home/End) is used it lists the on/off entries; while a side
    // rotary is being turned it lists the settings; otherwise it shows both
    // side rotaries' settings, left and right.
    float elemView = 0.0f;
    if (g_elemSelTime.QuadPart != 0)
    {
        const float since = static_cast<float>(SecondsSince(g_elemSelTime));
        const float hold = 10.0f; // stays up 10 s after the last use, then fades
        elemView = since < hold ? 1.0f : (since < hold + 0.4f ? 1.0f - (since - hold) / 0.4f : 0.0f);
    }
    // Knob box: both setting knobs' current settings, always shown while
    // driving. While a rotary is in use, its 10-line list shows at the side
    // of the screen: the left knob's on the left, the right knob's and the
    // on/off list on the right (DrawScrollList: fixed width, numbered).
    if (cfg.showKnobBox && g_inRealtime.load(std::memory_order_relaxed))
    {
        const float s = uiScale * cfg.knobBoxScale;
        const float textPx = 40.0f * s;
        const float listPx = 34.0f * s;
        const float padding = 16.0f * s;
        const float colGap = 48.0f * s;

        float nb = 0.0f;
        if (g_knobSelTime.QuadPart != 0)
        {
            LARGE_INTEGER now, freq;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            const float since = static_cast<float>(now.QuadPart - g_knobSelTime.QuadPart) / static_cast<float>(freq.QuadPart);
            const float hold = (std::max)(cfg.knobNeighbourSeconds, 0.0f);
            nb = since < hold ? 1.0f : (since < hold + 0.4f ? 1.0f - (since - hold) / 0.4f : 0.0f);
        }

        char value[64], leftText[128], rightText[128];
        // Fixed width, so the box never changes size with the text: "L" at
        // the far left with the left setting after it, a divider in the
        // middle, the right setting after it and "R" at the far right. Text
        // that does not fit is shortened ("..").
        const float halfTextW = renderer.MeasureText("0000000000000000000000000000000000", textPx);   // 34 characters
        const float markW = renderer.MeasureText("L", textPx);
        const float markGap = textPx * 0.6f;
        KnobValue(g_knobSel[0], value, sizeof(value));
        sprintf_s(leftText, "%s: %s", kKnobSettingName[g_knobSel[0]], value);
        const float boxIconW = textPx * 1.4f;   // icon before each side's text
        FitText(renderer, leftText, sizeof(leftText), halfTextW - boxIconW, textPx);
        KnobValue(g_knobSel[1], value, sizeof(value));
        sprintf_s(rightText, "%s: %s", kKnobSettingName[g_knobSel[1]], value);
        FitText(renderer, rightText, sizeof(rightText), halfTextW - boxIconW, textPx);
        const float centerX = screenWidth * cfg.knobBoxCenterXFraction;
        const float centerY = screenHeight * cfg.knobBoxCenterYFraction;
        const float halfW = colGap * 0.5f + halfTextW + markGap + markW + padding;
        const float halfH = textPx * 0.5f + padding;
        Layout::Register("Knob setting", "Shortcuts", "KnobBoxCenterXFraction", "KnobBoxCenterYFraction",
                         &g_shortcutConfig.knobBoxCenterXFraction, &g_shortcutConfig.knobBoxCenterYFraction,
                         centerX, centerY, halfW, halfH,
                         &g_shortcutConfig.knobBoxScale, "KnobBoxScale");
        renderer.AddFilledRoundedRect(centerX, centerY, halfW, halfH, 10.0f * s, 0.0f, 0.08f, 0.08f, 0.08f, 0.6f);
        renderer.AddFilledRect(centerX, centerY, 1.5f * s, textPx * 0.55f, 0.0f, 1.0f, 1.0f, 1.0f, 0.25f);
        float r, g, b;
        const float y = centerY - textPx * 0.5f;
        renderer.AddText(centerX - halfW + padding, y, "L", textPx, 0.6f, 0.6f, 0.6f, 1.0f, Renderer::HAlign::Left);
        renderer.AddText(centerX + halfW - padding, y, "R", textPx, 0.6f, 0.6f, 0.6f, 1.0f, Renderer::HAlign::Right);
        KnobGroupColour(g_knobSel[0], r, g, b);
        DrawKnobIcon(renderer, g_knobSel[0], centerX - colGap * 0.5f - halfTextW + textPx * 0.5f, centerY, textPx * 0.42f, r, g, b, 1.0f);
        renderer.AddText(centerX - colGap * 0.5f - halfTextW + boxIconW, y, leftText, textPx, r, g, b, 1.0f, Renderer::HAlign::Left);
        KnobGroupColour(g_knobSel[1], r, g, b);
        DrawKnobIcon(renderer, g_knobSel[1], centerX + colGap * 0.5f + textPx * 0.5f, centerY, textPx * 0.42f, r, g, b, 1.0f);
        renderer.AddText(centerX + colGap * 0.5f + boxIconW, y, rightText, textPx, r, g, b, 1.0f, Renderer::HAlign::Left);

        // The rotary lists: at the screen's side edges, mid-height -- the
        // left knob's on the left, the right knob's and the centre (on/off)
        // list on the right.
        const float edgeMargin = screenWidth * 0.012f;
        const float listCenterY = screenHeight * 0.45f;
        // A line above each list naming the button that acts on it.
        char elemCaption[96], knobCaption[96];
        if (cfg.elementToggleButton > 0)
            sprintf_s(elemCaption, "Button %d switches on / off", cfg.elementToggleButton);
        else
            strcpy_s(elemCaption, "No on/off button set ([Shortcuts] ElementToggleButton)");
        {
            const int up = g_activeSlot == 0 ? cfg.leftSettingUpButton : cfg.rightSettingUpButton;
            const int dn = g_activeSlot == 0 ? cfg.leftSettingDownButton : cfg.rightSettingDownButton;
            if (up > 0 && dn > 0) sprintf_s(knobCaption, "Buttons %d / %d change the value", up, dn);
            else knobCaption[0] = '\0';
        }
        if (elemView > 0.0f)
        {
            // Entries only (headings skipped), in list order.
            int flat[kListOrderCount];
            int count = 0, sel = 0;
            for (int i = 0; i < kListOrderCount; ++i)
                if (kListOrder[i] >= 0) { if (i == g_elemIndex) sel = count; flat[count++] = kListOrder[i]; }
            DrawScrollList(renderer, g_elemWin, count, sel, false, screenWidth - edgeMargin, listCenterY, listPx, s, elemView,
                           [&](int i, char* name, char* val, float& r, float& g, float& b)
                           {
                               const int idx = flat[i];
                               val[0] = '\0';
                               if (idx == kCloseEntry) { strcpy_s(name, 128, "<< ESC  (close chat / back to garage)"); r = g = b = 0.9f; return; }
                               if (idx == kResetTowerCode) { strcpy_s(name, 128, ListEntryLabel(idx)); strcpy_s(val, 64, "press to reset"); r = g = b = 0.9f; return; }
                               const bool on = ListEntryOn(idx);
                               strcpy_s(name, 128, ListEntryLabel(idx));
                               strcpy_s(val, 64, on ? "ON" : "OFF");
                               r = on ? 0.45f : 1.0f; g = on ? 1.0f : 0.45f; b = on ? 0.5f : 0.45f;
                           }, elemCaption);
        }
        else if (nb > 0.0f)
        {
            const bool leftKnob = g_activeSlot == 0;
            DrawScrollList(renderer, g_knobWin, kKnobSettingCount, g_knobSetting, leftKnob,
                           leftKnob ? edgeMargin : screenWidth - edgeMargin, listCenterY, listPx, s, nb,
                           [&](int i, char* name, char* val, float& r, float& g, float& b)
                           {
                               KnobGroupColour(i, r, g, b);
                               strcpy_s(name, 128, kKnobSettingName[i]);
                               if (!KnobAvailable(i))
                               {
                                   // Skipped by the knob: its panel is switched off.
                                   r *= 0.45f; g *= 0.45f; b *= 0.45f;
                                   strcpy_s(val, 64, "(panel off)");
                               }
                               else if (i == g_knobSetting) KnobValue(i, val, 64);
                               else val[0] = '\0';
                           }, knobCaption, DrawKnobIcon);
        }
    }

    renderer.SetTextBackground(prevTextBg);
}
