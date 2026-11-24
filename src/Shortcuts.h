#pragma once
#include <Windows.h>

// Per-element visibility, driven by Ctrl+Shift+<letter> hotkeys whose letters
// are all configurable in the INI ([Shortcuts]).
//
// This replaces the old Ctrl+Tab Gray-code cycle, which walked a fixed
// sequence of combined states: with three toggles that was already eight
// steps to reach a specific combination, and it does not extend to a dozen
// elements at all. Every element now has its own key and its own independent
// on/off, so nothing has to be cycled past to reach anything else.
//
// Visibility is PERSISTED: a toggle writes its Show<Element> key straight
// back to the INI, so the last state survives restarting the game. The write
// is done per key in place (never a whole-file rewrite), so the user's own
// hand-written comments and tuning edits in that file are left untouched.
//
// Toggling and drawing live on the render thread (polled once per Present,
// drawn in the same pass). Reading the keyboard and wheel buttons, sending
// the rotary menu's key presses and reading rF2's Controller.JSON happen on
// Shortcuts' own input thread, which hands the results over through atomics
// (and one small lock), so a frame never waits on any of them.
class Renderer;

namespace Shortcuts
{
    enum Element
    {
        Radar = 0,        // radar itself; the side proximity bars are now Shortcuts::SideBar
        PedalTrace,
        Perf,             // frametime / FPS stats
        Gear,             // gear + speed
        LapDelta,         // SB (session-best) delta row
        BestDelta,        // PB (all-time) delta row
        MiniSector,       // delta-speed strip
        RelativeTiming,
        FuelTyre,
        BrakeCue,         // brake cue info AND its beep
        Drs,              // DRS "OPEN DRS"/"DRS ENGAGED" sign AND its beep
        Clock,
        SideBar,          // side proximity bars -- independent of the radar itself
        // Independent toggles for the three FuelTyre boxes. FuelTyre above
        // remains a master switch for all three at once; these let just one
        // or two be hidden without taking down the whole panel.
        FuelPanel,
        TyresPanel,
        BrakeBiasPanel,
        TyreLockup,       // 4-corner brake lockup box, next to the speedo
        ShiftStats,       // "how am I actually shifting" readout below the dashboard
        TyreTemps,        // per-corner tyre temperature box (camber check)
        FfbGraph,         // FFB frequency spectrum
        TrackMap,         // overhead track map
        DllCost,          // what the DLL itself costs, by module (Profiler.h)
        CoverBoxes,       // plain boxes hiding parts of the car's own display (CoverBox.h)
        ElementCount
    };

    // Once per Present, before anything draws.
    void Poll();

    // Plugin shutdown: stops the wheel-button thread before the DLL goes.
    void Shutdown();

    bool Visible(Element e);

    // For per-car layouts (CarLayout.h): an element's Show<Element> INI key,
    // its own on/off (unlike Visible(), not forced on in layout mode), and
    // setting it directly (persisted like a toggle).
    const char* IniKey(Element e);
    bool Toggled(Element e);
    void SetVisible(Element e, bool visible);

    // Master on/off for everything this plugin draws.
    //
    // Separate from Visible() rather than another Element: those say which
    // panels you want when the overlay is up, this says whether it is up at
    // all. Checked once, around the whole draw block -- so with it off the
    // plugin costs one branch per frame and paints nothing but its own
    // toast, which has to keep working or the key could not be found again.
    bool OverlayEnabled();
    // HUD drawn even out of the car (Ctrl+Shift+A).
    bool AlwaysOn();
    // The shortcut list is open.
    bool ListOpen();

    // rF2's own "Toggle Cursor" key as display text ("F9", or "not bound"),
    // for the layout-mode banner. A cached copy of what Controller.JSON says,
    // kept fresh by the input thread, so it is cheap enough to call every
    // frame.
    void ToggleCursorKeyName(char* out, size_t outSize);

    // Shows a message in the same transient box as toggle confirmations and
    // the startup hint, for callers outside this file that need to surface a
    // one-off notice -- e.g. FuelTyrePanel's fuel/tyre marginal-or-critical
    // alerts. Replaces whatever message is currently showing, same as every
    // other popup here.
    void ShowMessage(const char* text, double holdSeconds);

    // Called when the player starts actually driving, which arms the one-off
    // "press Ctrl+Shift+I for shortcuts" hint. Safe to call every frame --
    // only a false->true transition arms it, so the hint appears on getting
    // in the car and not again on every subsequent frame.
    void NoteRealtime(bool inRealtime);

    // Called on StartSession: re-arms the hint for the new session.
    void ResetHint();

    // Toast, startup hint, the full shortcut list, and the INI-write warning.
    // Drawn last so it sits over everything else.
    //
    // 'hwnd' is the swap chain's output window, used only to hit-test the
    // mouse against the list's rows: with the list open and the cursor
    // visible (rF2's own "Toggle Cursor" control), rows can be clicked to
    // toggle an element rather than having to recall its hotkey. When the
    // cursor is hidden -- i.e. while actually driving -- the mouse is not
    // read at all.
    void Draw(Renderer& renderer, int screenWidth, int screenHeight, HWND hwnd);
}
