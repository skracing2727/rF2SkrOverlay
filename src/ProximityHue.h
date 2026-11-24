#pragma once
#include <Windows.h>

// Simple peripheral-vision side indicator: a bar down the left or right edge
// of the screen whenever a car is beside you -- triggered by exactly the
// condition that turns that car's radar blip RED, so the bar and the red blip
// always agree. The bar is always plain red: how close the car is shows in
// its OPACITY (lateral distance) and its SHAPE (forward offset leans the
// fade and the taper toward the end the car is at), never in its hue.
//
// EVERY triggering car draws its own bar, and they superimpose through normal
// alpha blending -- two cars stacked alongside you on the same side read as
// stronger than one, which is exactly the message. Tracking is per CAR, not
// per side, so each car's contribution fades in and out on its own; a
// per-side "nearest car wins" scheme would both hide the second car entirely
// and pop when the nearest one left and the next took over.
//
// Reads the SAME data the radar does (CarState's interpolated snapshot) and
// applies the SAME pipeline in the same order: elliptical range cull, then
// the fade-alpha cutoff, then the danger window. Two earlier attempts at an
// independent threat calculation both got this wrong -- one skipped the range
// cull entirely (so a car 4m ahead but 30m off to the side, on an adjacent
// piece of track, counted as "alongside"), and one extrapolated opponent
// positions forward from their velocity, which drifted between scoring ticks
// and snapped back at each one, flickering the bar on and off. CarState
// interpolates strictly BETWEEN two real samples, so it cannot overshoot; it
// costs ~0.2s of presentation delay, which is a far better trade than a
// warning that false-fires.
//
// Everything runs in Draw() on the render thread, exactly like the radar,
// from the same per-frame CarState snapshot the radar draws from (Overlay.cpp
// reads it once and hands it to both) -- so nothing here needs a lock or the
// scoring/telemetry callbacks at all.
class Renderer;
struct CarSnapshot;

class ProximityHue
{
public:
    // 'hwnd' is the swap chain's output window -- needed only for the
    // layout-mode resize handle and opacity slider below, to hit-test the
    // mouse the same way Shortcuts' own list-click handling does.
    void Draw(Renderer& renderer, int screenWidth, int screenHeight, HWND hwnd, const CarSnapshot& cars);

private:
    // Layout-mode-only width slider (vertical) and opacity slider
    // (horizontal). NOT built on Layout::Register like every other panel's
    // corner handle: that module's model is "grab a box anywhere on screen
    // and move it", which does not fit a bar that must always stay welded
    // to the screen edge -- so this gets its own small bespoke drag handling
    // instead, using the same GetAsyncKeyState(VK_LBUTTON) +
    // Layout::CursorPosition approach Shortcuts.cpp uses for its own
    // list-row clicks. Both persist to the INI on release, same convention
    // as every Layout::Register drag.
    void DrawLayoutControls(Renderer& renderer, int screenWidth, int screenHeight, HWND hwnd);
    bool m_resizeDragging = false;
    bool m_sliderDragging = false;
    bool m_tooCloseMetersDragging = false;
    bool m_tooCloseHzDragging = false;
    bool m_controlsPrevButtonDown = false;

    // One car's contribution to the bars. Slots persist across frames so the
    // fade is continuous; a slot lives on past the car leaving the danger
    // window, still drawing, until its fade reaches zero.
    //
    // The last known position is held per slot so a contribution that is
    // fading OUT keeps the opacity and shape it had when the car left,
    // instead of snapping to some default for its final few frames.
    struct Contribution
    {
        long id = -1;              // -1 = free slot
        bool onLeft = false;
        float fade = 0.0f;         // 0 = fully hidden, 1 = fully shown
        double lateralM = 0.0;     // drives the base opacity
        double forwardM = 0.0;     // drives the top/bottom fade AND taper
    };

    // Comfortably past the handful of cars that can physically fit inside the
    // danger window at once; extras are simply not tracked.
    static const int kMaxContributions = 16;
    Contribution m_contributions[kMaxContributions];

    LARGE_INTEGER m_lastDrawTime = {};
    LARGE_INTEGER m_freq = {};
};

extern ProximityHue g_proximityHue;
