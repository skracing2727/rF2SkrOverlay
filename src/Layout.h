#pragma once
#include <Windows.h>

// Drag-to-position for every HUD panel, plus the keyboard nudges that do the
// same thing without a mouse.
//
// HOW A PANEL BECOMES DRAGGABLE: it calls Register() once per frame, right
// where it already computes the rectangle it draws its own background into,
// handing over that rectangle AND pointers to the two live config floats
// that placed it. Nothing else is needed -- no per-panel drag code, no
// central table of panel geometry that would drift out of step with the
// panels themselves.
//
// WHY POINTERS TO THE FRACTIONS RATHER THAN AN ANCHOR DESCRIPTION: the
// panels do not agree on what their position fraction MEANS. Radar, pedals,
// gear and the delta bar use a CENTRE; the fuel/tyre panel uses its own
// top-LEFT corner; relative timing uses its top-RIGHT; the clock's Y of 1.0
// means the bottom edge. Teaching this module all four conventions would be
// four ways to get it wrong. Instead the drag applies a DELTA: move the
// fraction by however far the mouse moved, in screen fractions. A delta in
// the anchor is a delta in the panel no matter which corner the anchor names,
// so every convention works without this code knowing which one it is
// looking at.
//
// PANELS ARE REMEMBERED, NOT JUST REGISTERED. The registry is NOT cleared
// each frame. A panel's box is stored as an OFFSET FROM ITS OWN ANCHOR, and
// once seen it stays in the registry for the rest of the session, redrawn at
// (anchor + offset) whenever layout mode is on.
//
// This is what makes the mode usable at all. rFactor 2 only frees the mouse
// cursor when the game puts a menu up -- and every one of those exits
// realtime, at which point virtually every panel stops drawing (Radar.cpp
// only draws them inside its g_inRealtime block). A registry rebuilt per
// frame would therefore be EMPTY exactly when the cursor is available: the
// one moment you can drag is the one moment there is nothing to drag. The
// only situation that worked was the mid-race Escape dialog, which happens
// to keep realtime alive. Caching the offset also solves the faded-out panel
// for free -- a pedal trace that has faded to nothing still has a box.
//
// The cost is that a panel must have been SEEN ONCE this session before it
// can be dragged. Nothing knows how big an unseen panel is.
//
// INPUT IS POLLED, NOT HOOKED -- the same GetAsyncKeyState approach
// Shortcuts uses, extended to VK_LBUTTON and GetCursorPos. That means the
// click is OBSERVED but not CONSUMED: the game sees it too. Consuming it
// would need a window-procedure or low-level input hook, which is a far more
// invasive thing to put in a plugin that otherwise only hooks Present, and a
// much better way to hang the game. Hence LAYOUT MODE: outside it the mouse
// is not looked at even once, so behaviour is identical to having none of
// this; inside it you are (in practice) stopped in the pits arranging the
// HUD, where a click reaching the game as well costs nothing.
class Renderer;

namespace Layout
{
    // Marks every remembered panel as not-yet-seen this frame. Called once
    // per Present, BEFORE any panel draws. Panels that do not re-register
    // keep their remembered box and are still draggable -- see the note
    // above.
    void BeginFrame(int screenWidth, int screenHeight);

    // Called by a panel from inside its own Draw, after it has worked out
    // where it sits. 'name' is what layout mode labels the panel with.
    // 'section'/'xKey'/'yKey' are where the dragged position is written back
    // in the INI. 'xFraction'/'yFraction' point at the live config values --
    // dragging mutates them in place, so the very next frame draws in the
    // new spot with no other plumbing.
    //
    // A panel that is hidden simply never registers, and so is not
    // draggable -- which is the wanted behaviour, not a special case.
    // 'scale' and 'scaleKey', when given, make the panel RESIZEABLE: layout
    // mode draws a grab handle at its bottom-right corner, and dragging that
    // multiplies the panel's own scale factor. Every panel already funnels
    // all of its pixel sizing through one uiScale value, so folding this in
    // there resizes text, padding, gaps and corner radii together, in exact
    // proportion -- which is why the scale lives in the panel's config
    // rather than being applied to the box by this module.
    //
    // 'onDragStart', when given, is called once as a drag begins. It exists
    // for panels whose position can be OVERRIDDEN by an alignment flag (the
    // perf stats can be forced flush to the top, or to a screen corner): the
    // flag and the drag are two things fighting over one position, so
    // grabbing the panel clears the flag.
    // 'scaleSection', when given, is where 'scale' gets written on resize --
    // separate from 'section' (which is always where the POSITION persists).
    // The two only differ when 'scale' points at a DIFFERENT panel's config
    // than this one's own -- e.g. the delta-speed strip's resize handle
    // driving the delta bar's shared scale while the two are width-matched
    // -- where writing that drag to this panel's own section would silently
    // persist to the wrong key. Defaults to 'section' for every ordinary
    // panel, where the two are the same thing.
    void Register(const char* name,
                  const char* section, const char* xKey, const char* yKey,
                  float* xFraction, float* yFraction,
                  float centerX, float centerY, float halfW, float halfH,
                  float* scale = nullptr, const char* scaleKey = nullptr,
                  void (*onDragStart)() = nullptr,
                  const char* scaleSection = nullptr);

    // A panel whose corner handle sets its WIDTH and HEIGHT independently
    // (in pixels at 2160p, saved as wKey/hKey in 'section') instead of one
    // uniform scale -- for a plain box with no content to keep in
    // proportion. The panel's position fractions must place its TOP-LEFT
    // corner: the resize keeps that corner fixed and the handle follows the
    // cursor.
    void RegisterBox(const char* name,
                     const char* section, const char* xKey, const char* yKey,
                     float* xFraction, float* yFraction,
                     float centerX, float centerY, float halfW, float halfH,
                     float* widthPx, const char* wKey, float* heightPx, const char* hKey);

    bool Active();
    void Toggle();

    // Mouse polling and the drag itself. Called once per Present AFTER every
    // panel has drawn (and therefore registered), so the hit-test uses this
    // frame's real rectangles. 'hwnd' comes from the swap chain's
    // DXGI_SWAP_CHAIN_DESC::OutputWindow; the backbuffer size is passed
    // separately because it need not match the window's client size.
    void Poll(HWND hwnd, int backbufferWidth, int backbufferHeight);
    // A panel's own control (a slider, a button) took this mouse press:
    // Poll must not also start a drag under it. Called before Poll, any
    // frame of the press; cleared once the button is up (review F124/F132).
    void ClaimPress();

    // True while layout mode is on, for panels that fade themselves out when
    // idle. A panel you cannot see is a panel you cannot grab, so they hold
    // at full opacity (and skip their fully-faded early-out) while the mode
    // is active.
    bool SuppressFade();

    // Layout mode or the shortcut list open: every switched-on panel shows,
    // at full size, with placeholder content where it has no data yet.
    bool Preview();

    // Whether the OS cursor is currently VISIBLE, and where it is in
    // backbuffer pixels. This is the honest gate for any mouse interaction:
    // rFactor 2 only shows a cursor when it has actually released the mouse
    // (a menu, or the "Toggle Cursor" control -- whatever key that's bound
    // to in the driver's own rF2 controls), so reading the mouse exactly
    // while the cursor is showing means never
    // reacting to a click the driver could not see themselves make.
    // Returns false, and leaves the coordinates alone, when it is hidden.
    bool CursorPosition(HWND hwnd, int backbufferWidth, int backbufferHeight,
                        float& outX, float& outY);

    // Same OS-level check as CursorPosition, without needing a window or a
    // position -- just "is it up at all". Used to decide whether entering
    // layout mode needs to press rF2's own Toggle Cursor control for you.
    bool IsCursorVisible();

    // Panel outlines, names, and the mode banner. Only draws anything while
    // layout mode is active.
    // 'inCar' comes from the same realtime flag that gates the panel draws.
    // When false the banner explains how to get a cursor, since that is the
    // only thing standing between the user and actually using this mode.
    void Draw(Renderer& renderer, int screenWidth, int screenHeight, bool inCar);

    // Shifts one position fraction and writes it back to the INI, clamped
    // the same way a drag is. Used by the keyboard nudge hotkeys. Returns
    // false if the INI write failed, so the caller can warn.
    bool NudgeFraction(float* fraction, const char* section, const char* key, float delta);

    // Read-only view of the remembered panel boxes, for the shortcut list's
    // screen map. Box centre and half-size in backbuffer pixels; 'seenThisFrame'
    // false means the panel is not drawing now (its last known box). Returns
    // false for an index out of range or a box measured at another resolution.
    // Box-stats labels: panels registered while an owner tag is set are
    // stamped with it, so the overlay knows which rectangles belong to which
    // drawn box. -1 = none.
    void SetOwnerTag(int tag);
    int OwnerTagOf(int index);
    int RegisteredCount();
    bool GetRegistered(int index, const char** name, float* centerX, float* centerY,
                       float* halfW, float* halfH, bool* seenThisFrame);
}
