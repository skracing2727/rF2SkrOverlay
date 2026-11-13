#include "Shortcuts.h"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "LapDelta.h"
#include "MiniSector.h"
#include "FuelTyrePanel.h"
#include "Log.h"
#include "Version.h"

#include "TrackMap.h"
#include <Windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <atomic>
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
        bool prevDown;
    };

    // Order here is the order the on-screen list reads in -- grouped roughly
    // by what they are for (safety, driving reference, timing, housekeeping)
    // rather than alphabetically, so the list is scannable.
    Entry g_entries[Shortcuts::ElementCount] =
    {
        { "Radar",                 "ShowRadar",          'R', true,  false },
        { "Pedal trace",           "ShowPedalTrace",     'P', true,  false },
        { "Frametime stats",       "ShowPerf",           'F', true,  false },
        { "Gear + speed",          "ShowGear",           'G', true,  false },
        { "Delta bar (session)",   "ShowLapDelta",       'D', true,  false },
        { "Delta bar (best)",      "ShowBestDelta",      'A', true,  false },
        { "Delta-speed strip",     "ShowMiniSector",     'M', false, false },
        { "Relative timing",       "ShowRelativeTiming", 'T', true,  false },
        { "Fuel + tyres",          "ShowFuelTyre",       'U', true,  false },

        { "DRS sign + beep",       "ShowDrs",            'D', true,  false },
        { "Clock",                 "ShowClock",          'C', true,  false },
        { "Side proximity bars",   "ShowSideBar",        'S', true,  false },
        { "Fuel box",              "ShowFuelPanel",      'N', true,  false },
        { "Tyres box",             "ShowTyresPanel",     'Y', true,  false },
        { "Brake bias box",        "ShowBrakeBiasPanel", 'K', true,  false },
        { "Tyre lockup box",       "ShowTyreLockup",     'L', true,  false },
        { "Shift stats",           "ShowShiftStats",     'V', true,  false },
        { "Tyre temps (corner)",   "ShowTyreTemps",      'W', true,  false },
        { "FFB spectrum",          "ShowFfbGraph",       0,   true,  false },
        { "Track map",             "ShowTrackMap",       0,   true,  false },
        { "Tyre wear plots",       "ShowTyreWearPlots",  0,   true,  false },
        { "DLL cost (by module)",  "ShowDllCost",        0,   false, false },
        { "Cover boxes",           "ShowCoverBoxes",     0,   false, false },
    };

    bool g_listDown = false;
    bool g_layoutDown = false;
    bool g_masterDown = false;
    // Edge state for clicking a row in the list. Held here rather than in
    // Layout because this is a different gesture on a different surface --
    // Layout's own button state belongs to dragging panels.
    bool g_listClickDown = false;

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
    float g_listScrollGrabPx = 0.0f; // cursor offset from the thumb's top while dragging
    // Wheel buttons that step the track map colour mode. Read through winmm's joystick API, which sees buttons 1-32 of
    // every game controller. joyGetPosEx is slow (a fraction of a ms per
    // device -- it showed up as ~0.25 ms/frame in the DLL-cost panel when it
    // ran per frame), so it runs on its own thread at ~100 Hz -- fast enough
    // for an encoder's click pulse -- and hands over the counted clicks.
    HANDLE g_joyThread = nullptr;
    std::atomic<bool> g_joyStop{ false };
    // Track map colour mode buttons (TrackMapConfig::deltaPrevButton /
    // deltaNextButton): +1 per next press, -1 per previous, applied in Poll.
    std::atomic<int> g_mapModeSteps{ 0 };

    const char* MapModeName(int mode)
    {
        return mode == 1 ? "vs my personal best" : mode == 2 ? "vs my session best"
             : mode == 3 ? "vs session fastest (anyone)" : "off";
    }

    bool ButtonDown(DWORD buttons, int btn)
    {
        return btn >= 1 && btn <= 32 && (buttons & (1u << (btn - 1))) != 0;
    }

    DWORD WINAPI JoyThreadProc(void*)
    {
        bool present[16] = {};
        ULONGLONG lastScan = 0;
        bool prevWas = false, nextWas = false;
        while (!g_joyStop.load())
        {
            const int prevBtn = g_trackMapConfig.deltaPrevButton;
            const int nextBtn = g_trackMapConfig.deltaNextButton;
            const int needed = (std::max)(prevBtn, nextBtn);

            // Every few seconds: which controllers exist and have enough
            // buttons. Only those are polled, so pedals and shifters with
            // fewer buttons cost nothing.
            const ULONGLONG nowMs = GetTickCount64();
            if (lastScan == 0 || nowMs - lastScan > 3000)
            {
                lastScan = nowMs;
                const UINT devs = joyGetNumDevs();
                bool any = false;
                for (UINT id = 0; id < 16; ++id)
                {
                    JOYCAPSW caps = {};
                    present[id] = id < devs && needed >= 1
                        && joyGetDevCapsW(id, &caps, sizeof(caps)) == JOYERR_NOERROR
                        && static_cast<int>(caps.wNumButtons) >= needed;
                    any = any || present[id];
                }
                // A driver that under-reports its button count would leave
                // nothing to poll; fall back to every controller that answers.
                if (!any)
                {
                    for (UINT id = 0; id < 16 && id < devs; ++id)
                    {
                        JOYINFOEX ji = {};
                        ji.dwSize = sizeof(ji);
                        ji.dwFlags = JOY_RETURNBUTTONS;
                        present[id] = needed >= 1 && joyGetPosEx(id, &ji) == JOYERR_NOERROR;
                    }
                }
            }

            DWORD buttons = 0;
            for (UINT id = 0; id < 16; ++id)
            {
                if (!present[id]) continue;
                JOYINFOEX ji = {};
                ji.dwSize = sizeof(ji);
                ji.dwFlags = JOY_RETURNBUTTONS;
                if (joyGetPosEx(id, &ji) == JOYERR_NOERROR)
                    buttons |= ji.dwButtons;
                else
                    present[id] = false; // unplugged; the next scan finds it again
            }
            const bool prevNow = ButtonDown(buttons, prevBtn);
            const bool nextNow = ButtonDown(buttons, nextBtn);
            if (prevNow && !prevWas) g_mapModeSteps.fetch_sub(1);
            if (nextNow && !nextWas) g_mapModeSteps.fetch_add(1);
            prevWas = prevNow;
            nextWas = nextNow;
            Sleep(10);
        }
        return 0;
    }
    bool g_resetSbDown = false;
    bool g_resetPbDown = false;
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
        g_entries[Shortcuts::Radar].key = cfg.radar;
        g_entries[Shortcuts::PedalTrace].key = cfg.pedalTrace;
        g_entries[Shortcuts::Perf].key = cfg.perf;
        g_entries[Shortcuts::Gear].key = cfg.gear;
        g_entries[Shortcuts::LapDelta].key = cfg.lapDelta;
        g_entries[Shortcuts::BestDelta].key = cfg.bestDelta;
        g_entries[Shortcuts::MiniSector].key = cfg.miniSector;
        g_entries[Shortcuts::RelativeTiming].key = cfg.relativeTiming;
        g_entries[Shortcuts::FuelTyre].key = cfg.fuelTyre;

        g_entries[Shortcuts::Drs].key = cfg.drs;
        g_entries[Shortcuts::Clock].key = cfg.clock;
        g_entries[Shortcuts::SideBar].key = cfg.sideBar;
        g_entries[Shortcuts::FuelPanel].key = cfg.fuelPanel;
        g_entries[Shortcuts::TyresPanel].key = cfg.tyresPanel;
        g_entries[Shortcuts::BrakeBiasPanel].key = cfg.brakeBiasPanel;
        g_entries[Shortcuts::TyreLockup].key = cfg.tyreLockup;
        g_entries[Shortcuts::ShiftStats].key = cfg.shiftStats;
        g_entries[Shortcuts::TyreTemps].key = cfg.tyreTemps;
        g_entries[Shortcuts::FfbGraph].key = cfg.ffbGraph;
        g_entries[Shortcuts::TrackMap].key = cfg.trackMap;
        g_entries[Shortcuts::TyreWearPlots].key = cfg.tyreWearPlots;
        g_entries[Shortcuts::DllCost].key = cfg.dllCost;
        g_entries[Shortcuts::CoverBoxes].key = cfg.coverBoxes;

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
            g_entries[Shortcuts::TyreWearPlots].visible = cfg.showTyreWearPlots;
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
}

void Shortcuts::Poll()
{
    const ShortcutConfig& cfg = g_shortcutConfig;
    if (!cfg.enabled)
        return;

    SyncFromConfig();

    // Ctrl AND Shift, deliberately: Ctrl+<letter> alone collides with plenty
    // of the sim's own bindings, and Shift alone is a common in-car control.
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool modifiers = ctrl && shift;

    for (int i = 0; i < ElementCount; ++i)
    {
        Entry& e = g_entries[i];
        // For A-Z the virtual-key code IS the uppercase ASCII value, which is
        // exactly what ReadChar normalises to. key <= 0 means unassigned
        // (see ShortcutConfig) -- GetAsyncKeyState takes 1-254, so this is
        // skipped entirely rather than passing 0 to it.
        const bool down = e.key > 0 && modifiers && (GetAsyncKeyState(e.key) & 0x8000) != 0;
        if (down && !e.prevDown)
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
        e.prevDown = down;
    }

    const bool listDown = cfg.list > 0 && modifiers && (GetAsyncKeyState(cfg.list) & 0x8000) != 0;
    if (listDown && !g_listDown)
    {
        g_listOpen = !g_listOpen;
        QueryPerformanceCounter(&g_listTime);
    }
    g_listDown = listDown;

    // Reset session best -- in-memory only, so no confirmation needed.
    const bool sbDown = cfg.resetSessionBest > 0 && modifiers && (GetAsyncKeyState(cfg.resetSessionBest) & 0x8000) != 0;
    if (sbDown && !g_resetSbDown)
    {
        g_lapDelta.ResetSessionBest();
        g_miniSector.InvalidateReference();
        sprintf_s(g_toastText, "Session best cleared");
        ShowPopup(Popup::Toast, 2.0);
    }
    g_resetSbDown = sbDown;

    // Reset personal best -- DESTRUCTIVE (it deletes the saved file), so the
    // first press only arms it and says what is about to go; a second press
    // inside the window does it, and letting the window lapse cancels.
    const bool pbDown = cfg.resetPersonalBest > 0 && modifiers && (GetAsyncKeyState(cfg.resetPersonalBest) & 0x8000) != 0;
    if (pbDown && !g_resetPbDown)
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
    g_resetPbDown = pbDown;

    if (g_pbArmed && SecondsSince(g_pbArmTime) > cfg.resetConfirmSeconds)
        g_pbArmed = false;

    // Layout mode: drag panels with the mouse. Announced with a toast as well
    // as Layout's own banner, because it changes what a mouse click does.
    const bool layoutDown = cfg.layoutMode > 0 && modifiers && (GetAsyncKeyState(cfg.layoutMode) & 0x8000) != 0;
    if (layoutDown && !g_layoutDown)
    {
        Layout::Toggle();
        if (g_popup != Popup::Warning)
        {
            if (Layout::Active())
            {
                // Press rF2's own Toggle Cursor control for the driver rather
                // than just telling them which key it is -- but only if the
                // cursor is not already up, since the control is a toggle:
                // sending it while the cursor is already showing would hide
                // it again instead of doing nothing.
                if (!Layout::IsCursorVisible())
                {
                    int scanCode = 0;
                    if (Config::ReadToggleCursorScanCode(scanCode))
                    {
                        INPUT inputs[2] = {};
                        inputs[0].type = INPUT_KEYBOARD;
                        inputs[0].ki.wScan = static_cast<WORD>(scanCode);
                        inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;
                        inputs[1] = inputs[0];
                        inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
                        SendInput(2, inputs, sizeof(INPUT));
                    }
                }

                char cursorKey[16];
                Config::ReadToggleCursorKeyName(cursorKey, sizeof(cursorKey));
                sprintf_s(g_toastText, "Layout mode ON -- enable the mouse cursor to drag (Toggle Cursor: %s)",
                          cursorKey);
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
    const bool masterDown = cfg.masterToggle > 0 && modifiers &&
                            (GetAsyncKeyState(cfg.masterToggle) & 0x8000) != 0;
    if (masterDown && !g_masterDown)
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
    g_masterDown = masterDown;

    g_layoutDown = layoutDown;

    // --- Wheel buttons: track map colour mode previous / next ---
    // Read on the background thread (see JoyThreadProc); only the clicks it
    // counted are applied here.
    TrackMapConfig& tmc = g_trackMapConfig;
    auto validBtn = [](int b) { return b >= 1 && b <= 32; };
    if (validBtn(tmc.deltaPrevButton) || validBtn(tmc.deltaNextButton))
    {
        if (!g_joyThread)
        {
            g_joyStop.store(false);
            g_joyThread = CreateThread(nullptr, 0, JoyThreadProc, nullptr, 0, nullptr);
            if (!g_joyThread)
                Log::Write("Shortcuts: could not start the wheel-button thread (error %lu)", GetLastError());
        }
        // Track map colouring: step through off / PB / SB / session fastest.
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
    }

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
        const float textPx = 30.0f * listScale;
        const float lineStep = textPx * 1.4f;
        const float padding = 22.0f * listScale;
        const float titlePx = 34.0f * listScale;

        // The visibility toggles, then the ACTIONS -- resets and panel
        // positioning -- which are not Elements and so are not in g_entries.
        const float infoPx = textPx * 0.85f;
        const int kMaxLines = ElementCount + 40;
        char lines[kMaxLines][96];         // info rows only: full text
        char keyPart[kMaxLines][24];       // hotkey rows: "Ctrl+Shift+X"
        char labelPart[kMaxLines][80];     // hotkey rows: the label after the key column
        int rowElementIndex[kMaxLines];   // -1 = not a toggleable element row
        bool rowIsInfo[kMaxLines];        // dimmer, smaller -- not a hotkey at all
        int rowCustom[kMaxLines];         // -1, or a menu-setting row id (see addSettingRow)
        for (int i = 0; i < kMaxLines; ++i) rowCustom[i] = -1;
        int n = 0;
        float widest = renderer.MeasureText("Overlay shortcuts", titlePx);
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
        char cursorKeyName[16];
        Config::ReadToggleCursorKeyName(cursorKeyName, sizeof(cursorKeyName));
        char cursorInfo[64];
        sprintf_s(cursorInfo, "Enable mouse cursor: Toggle Cursor (%s)", cursorKeyName);
        addInfo(cursorInfo);

        // The two most important bindings, first: how to get back to this
        // list, and how to rearrange the HUD. Everything else can be
        // rediscovered from here, but these two are how you reach it.
        addAction(cfg.list, "This list");
        addAction(cfg.layoutMode, "Layout mode -- drag / resize panels");

        for (int i = 0; i < ElementCount; ++i)
            addElement(i);

        addAction(cfg.masterToggle, "Overlay on/off (everything)");
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
        const float infoLineStep = infoPx * 1.4f;
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
                || i == coverGreyRow || i == coverAlphaRow)
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
                                      0.08f, 0.08f, 0.08f, 0.6f);

        float y = topY;
        renderer.AddText(leftX, y, "Overlay shortcuts", titlePx, 1.0f, 1.0f, 1.0f, 1.0f);
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
        const bool clickEdge = clickNow && !g_listClickDown;
        g_listClickDown = clickNow;

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
            }
            QueryPerformanceCounter(&g_listTime);
        }
        else if (clickEdge && hoveredRow >= 0)
        {
            Entry& e = g_entries[rowElementIndex[hoveredRow]];
            e.visible = !e.visible;
            Persist(e);
            if (g_popup != Popup::Warning)
            {
                sprintf_s(g_toastText, "%s %s", e.label, e.visible ? "shown" : "hidden");
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

    renderer.SetTextBackground(prevTextBg);
}
