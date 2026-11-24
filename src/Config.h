#pragma once
#include <Windows.h>   // VK_* codes for the shortcut key defaults

// Runtime-editable settings, loaded from an INI file that lives next to the DLL
// (Bin64\Plugins\rF2SkrOverlayPublic.ini). If the file doesn't exist, a fully
// commented default is written out on first load.
//
// Changing values only needs the file saved and the game restarted (or the
// plugin re-ticked in Settings -> Gameplay -> Plugins) -- no rebuild.
//
// Deliberately NOT included here: anything that sizes a fixed-size array at
// compile time (pedal trace history length, vertex buffer capacity). Those stay
// as source constants since changing them needs a rebuild regardless.

// A translucent plate drawn behind text so it stays readable over bright
// track, sky or a white car. Applied by Renderer::AddText itself rather than
// per widget, so nothing new can be added later WITHOUT one. Widgets that
// already draw their own background panel switch it off around their text
// (see Renderer::SetTextBackground) instead of double-darkening.
struct TextBackgroundConfig
{
    bool enabled = true;
    float r = 0.04f, g = 0.04f, b = 0.04f;
    float alpha = 0.55f;
    float paddingPx = 6.0f;      // at 2160p, scaled with the screen
    float cornerRadiusPx = 5.0f;
};
extern TextBackgroundConfig g_textBackgroundConfig;

// Font used for all text: indexes into Renderer::kFontNames / kWeightNames,
// stored in the INI by name ([Text] Font= / FontWeight=). Changeable from
// the shortcut list.
struct TextFontConfig
{
    int font = 0;     // Auto: Consolas, or the built-in font if it's missing
    int weight = 0;   // Light
};
extern TextFontConfig g_textFontConfig;

// Wall-clock time of day, top right, styled to match the frametime stats.
struct ClockConfig
{
    bool enabled = true;
    bool showSeconds = false;
    // Bottom LEFT, flush to both edges: x=0 with left alignment, and a y that
    // is computed from the bottom edge at draw time (see DrawClock) rather
    // than set here, since it depends on the text height. The text plate's
    // own padding is cancelled for this widget so "flush" really is flush.
    float posXFraction = 0.0f;
    float posYFraction = 1.0f;   // 1.0 = bottom edge; the text height is subtracted at draw time

    // Panel size multiplier, dragged from the corner handle in layout mode
    // (Ctrl+Shift+5) and clamped there to 0.5-2.5. Folded into this panel's
    // own uiScale, so text, padding, gaps and corner radii all scale in
    // exact proportion rather than the box growing around fixed-size text.
    float scale = 1.0f;

    float textHeightPx = 34.0f;   // at 2160p
    float r = 0.85f, g = 0.85f, b = 0.85f;
};
extern ClockConfig g_clockConfig;

// UpdateCheck.h: ask GitHub once per game start whether a newer release is
// out, and say so in the menus.
struct UpdateCheckConfig
{
    bool enabled = true;
};
extern UpdateCheckConfig g_updateCheckConfig;

// What the DLL itself costs (Profiler.h): a bar of the last 10 s, split by
// module. Draggable/resizable in layout mode like the rest.
struct DllCostConfig
{
    float posXFraction = 0.01f;   // top-left corner
    float posYFraction = 0.62f;
    float scale = 1.0f;
    float textHeightPx = 24.0f;   // at 2160p
    float barWidthPx = 900.0f;
    float barHeightPx = 34.0f;
    // A module gets a name in the bar only if it is at least this share of
    // the total.
    float labelMinShare = 0.08f;
    // Diagnostic: every 10 s of driving, log the render-thread frames that
    // cost the DLL the most and which modules were in them (rF2SkrOverlay.log).
    bool logSpikes = false;
};
extern DllCostConfig g_dllCostConfig;

// Per-element Ctrl+Shift+<letter> visibility toggles. Each letter is a single
// character in the INI, so the whole scheme can be remapped without a
// rebuild. Ctrl+Shift+<list> shows the full list on screen, and the same list
// is briefly advertised when you start driving.
struct ShortcutConfig
{
    bool enabled = true;

    // All bindings are Ctrl+Shift+<key>, stored as VIRTUAL-KEY CODES and
    // written in the INI as "R", "5" or "F11" (see Config::ReadKey). 0 means
    // UNASSIGNED -- Config::KeyName prints that as "Unassigned" in the
    // on-screen list rather than a phantom key, and Shortcuts::Poll() skips
    // GetAsyncKeyState for it entirely, so an unassigned element simply
    // cannot be triggered by a keypress until you give it one in the INI.
    //
    // Only List and LayoutMode ship with a default binding: everything else
    // is reachable FROM the list (open it, click the row), so the list
    // itself and the means to reposition things are the only two keys that
    // have to work before you have ever opened the list. Every other
    // element still has its own key SLOT here -- assign one in the INI (a
    // letter, a digit, or "F1".."F12") and it works exactly the same as
    // these two.
    int radar = 0;
    int pedalTrace = 0;
    int perf = 0;                // frametime / FPS stats
    int gear = 0;                // gear + speed
    int lapDelta = 0;            // SB (session-best) delta row
    int bestDelta = 0;           // PB (all-time) delta row
    int miniSector = 0;          // delta-speed strip
    int relativeTiming = 0;
    int fuelTyre = 0;
    int brakeCue = 0;            // brake cue info AND its beep, together
    int drs = 0;                 // DRS beep AND the on-screen "OPEN DRS"/"DRS ENGAGED" sign, together
    // List and LayoutMode are the two most important bindings -- everything
    // else can be rediscovered from the on-screen list, but you need one of
    // these two to get there in the first place -- so they sit first, on 1
    // and 2, rather than wherever they landed alphabetically. The only two
    // bindings that ship assigned by default.
    int list = '1';              // show the shortcut list
    int layoutMode = '2';        // drag-to-position mode on/off
    int clock = 0;
    int sideBar = 0;             // side proximity bars, independent of the radar itself
    // Independent toggles for the three FuelTyre boxes -- fuelTyre above
    // remains a master switch for all three at once.
    int fuelPanel = 0;
    int tyresPanel = 0;
    int brakeBiasPanel = 0;
    int tyreLockup = 0;          // 4-corner brake lockup box, next to the speedo
    int shiftStats = 0;          // "how am I actually shifting" readout below the dashboard
    int tyreTemps = 0;           // per-corner tyre temperature box (camber check)
    int ffbGraph = 0;            // FFB frequency spectrum panel
    int trackMap = 0;            // overhead track map
    int dllCost = 0;             // what the DLL itself costs, by module
    int coverBoxes = 0;          // boxes hiding parts of the car's own display

    // Master on/off for everything this plugin draws. Distinct from the
    // per-element toggles: those are about which panels you want, this is
    // about wanting the overlay at all.
    int masterToggle = 'H';
    // Ctrl+Shift+A: draw the HUD even when not driving (watching a team-mate
    // drive your shared car, the monitor, a replay). Kept until toggled off.
    int alwaysOnToggle = 'A';
    bool alwaysOn = false;

    int resetSessionBest = 0;    // Zero the session best -- in-memory, no confirm
    int resetPersonalBest = 0;   // WIPE the saved PB -- destructive, press twice

    // Initial visibility, PERSISTED: whenever a hotkey toggles an element the
    // matching key is written straight back to the INI, so the last state
    // survives restarting the game.
    bool showRadar = true;
    bool showPedalTrace = true;
    bool showPerf = true;
    bool showGear = true;
    bool showLapDelta = true;
    bool showBestDelta = true;
    bool showMiniSector = false;  // little used, so off unless asked for
    bool showRelativeTiming = true;
    bool showFuelTyre = true;
    bool showBrakeCue = false;
    bool showDrs = true;
    bool showClock = true;
    bool showSideBar = true;
    bool showFuelPanel = true;
    bool showTyresPanel = true;
    bool showBrakeBiasPanel = true;
    bool showTyreLockup = true;
    bool showShiftStats = true;
    bool showTyreTemps = true;
    bool showFfbGraph = true;
    bool showTrackMap = true;
    bool showDllCost = false;    // a diagnostic, so off unless asked for
    bool showCoverBoxes = false;  // off until wanted: an unplaced box sits mid-screen
    bool showOverlay = true;     // master switch, persisted like the rest

    // How long the "press Ctrl+Shift+I" hint stays up after you start
    // driving, and how long a toggle confirmation lingers.
    float startupHintSeconds = 6.0f;

    // How long a destructive reset stays armed waiting for its second press.
    float resetConfirmSeconds = 4.0f;

    // The transient message box (toggle confirmations, the startup hint, the
    // INI-write warning). Below centre rather than on it, because the middle
    // of the screen is the apex of whatever corner is being taken when one
    // of these fires.
    float messageCenterXFraction = 0.5f;
    float messageCenterYFraction = 0.62f;
    float messageScale = 1.0f;

    // The rotary setting box: the setting each side rotary changes and its
    // value; while a rotary is being turned it lists the settings instead.
    bool showKnobBox = true;
    float knobBoxCenterXFraction = 0.5f;
    float knobBoxCenterYFraction = 0.80f;
    float knobBoxScale = 1.0f;
    float knobNeighbourSeconds = 5.0f;
    // Keys some rotary settings press (DirectInput scan codes, e.g. 0x1B for
    // ]; 0 = none). Bind the same keys in rF2 to the matching controls:
    // Increase / Decrease Vertical FOV, Increment / Decrement Mixture,
    // Traction Control Up / Down, Traction Control 2 Up / Down (power cut),
    // Traction Control Slip Angle Up / Down, Headlights, Inc / Dec Front and
    // Rear ARB, Antilock Brake System Up / Down, Adjust Seat Fore / Aft and
    // Up / Down. All unbound by default, since nobody's rF2 bindings match.
    int fovUpKey = 0;
    int fovDownKey = 0;
    int mixtureUpKey = 0;
    int mixtureDownKey = 0;
    int tcUpKey = 0;
    int tcDownKey = 0;
    int tcCutUpKey = 0;
    int tcCutDownKey = 0;
    int tcSlipUpKey = 0;
    int tcSlipDownKey = 0;
    int headlightsKey = 0;
    int frontArbUpKey = 0;
    int frontArbDownKey = 0;
    int rearArbUpKey = 0;
    int rearArbDownKey = 0;
    int absUpKey = 0;
    int absDownKey = 0;
    // "Brake bias": Bias Forward / Bias Rearward.
    int biasForwardKey = 0;
    int biasRearwardKey = 0;
    int seatForeKey = 0;
    int seatAftKey = 0;
    int seatUpKey = 0;
    int seatDownKey = 0;
    // Wheel rotary buttons (DirectInput button numbers 1-128, 0 = none). The
    // CENTRE rotary walks the on/off list and elementToggleButton switches
    // the picked entry. The LEFT and RIGHT rotaries each pick a setting, and
    // their Up/Down buttons change it. All 0 by default: every wheel numbers
    // its buttons differently. The keyboard works without any of these
    // (Home/End, Insert/Delete, Page Up/Page Down).
    int elementToggleButton = 0;
    int centreKnobPrevButton = 0;
    int centreKnobNextButton = 0;
    int rightKnobPrevButton = 0;
    int rightKnobNextButton = 0;
    int leftKnobPrevButton = 0;
    int leftKnobNextButton = 0;
    int leftSettingUpButton = 0;
    int leftSettingDownButton = 0;
    int rightSettingUpButton = 0;
    int rightSettingDownButton = 0;

    // The shortcut list. -1 is a sentinel meaning "auto-anchor below the
    // fuel/tyre panel's real bottom edge", which is what it did before it
    // was draggable -- so a taller panel pushed it down instead of
    // overlapping it. Dragging it resolves the sentinel and the list becomes
    // fixed, GIVING UP that auto-anchoring: it is then your responsibility
    // to leave room for the fuel panel at its tallest.
    float listPosXFraction = -1.0f;
    float listPosYFraction = -1.0f;
    float listScale = 1.0f;
    // Text size preset, the S / M / L boxes at the top of the list: 0 small, 1 medium, 2 large.
    int listTextSize = 0;
};
extern ShortcutConfig g_shortcutConfig;

struct RadarConfig
{
    bool enabled = true;

    float widthFraction = 1.0f / 6.0f;
    float centerXFraction = 0.5f;
    float centerYFraction = 1.0f / 3.0f;

    // Panel size multiplier, dragged from the corner handle in layout mode
    // (Ctrl+Shift+5) and clamped there to 0.5-2.5. Folded into this panel's
    // own uiScale, so text, padding, gaps and corner radii all scale in
    // exact proportion rather than the box growing around fixed-size text.
    float scale = 1.0f;


    float rangeForwardM = 10.0f;
    float rangeRearM = 10.0f;
    float rangeSideM = 20.0f;
    float fadeStartFraction = 0.7f;

    float carHalfWidthM = 1.0f;
    float carHalfLengthM = 2.3f;
    float carCornerRadiusM = 0.45f;

    // When true, a car whose VehicleScoringInfoV01 class/name matches a known
    // category (GT3, GT4, TCR/touring, prototype, open-wheel) is drawn at
    // that category's typical real footprint instead of the flat box above --
    // so a touring car and a GT3 car on the same grid read as the different
    // sizes they actually are. The box above remains the fallback for any car
    // that matches nothing, and is used for everyone when this is off. See
    // Radar.cpp's LookupCarFootprint for the table.
    bool useRealCarSize = true;

    // Added to every car's half-width AND half-length after the lookup
    // above (or the flat box, if that's off) -- a flat safety margin on top
    // of whatever size a car is drawn at, e.g. to account for mirrors,
    // wheel-to-wheel contact risk, or just personal preference for more
    // breathing room than the bodywork box alone gives. 0 = off.
    float footprintBufferMeters = 0.0f;

    float playerR = 0.15f, playerG = 1.0f, playerB = 0.15f;   // green
    float otherR = 1.0f, otherG = 0.85f, otherB = 0.1f;        // yellow -- clear ahead/behind, no immediate danger
    float dangerR = 1.0f, dangerG = 0.15f, dangerB = 0.15f;    // red -- alongside, or within DangerGapMeters front/back

    // A car counts as danger (red) when it is inside BOTH a longitudinal
    // window and lateral overlap of the player's own width. Both are raw
    // centre-to-centre metres, matching what the side-indicator debug
    // readout prints, rather than bumper gaps.
    //
    // Asymmetric on purpose: a car overlapping you from behind is the one
    // you're most likely to turn into, so it stays flagged for longer than
    // one slipping past ahead. Replaces an earlier test that baked in a
    // whole car length plus a bumper gap, giving an effective +/-5.6m, which
    // lit up for cars a clear car-length clear of you fore and aft.
    float dangerAheadM = 4.0f;
    float dangerBehindM = 5.0f;

    // Lateral half-window. Shared by the radar's red blips AND the on-screen
    // side indicator, so the two always agree on WHETHER a car counts --
    // they must never diverge on that. It doubles as the side indicator's
    // fully-green colour anchor: a car right at this edge is the mildest
    // thing worth showing, and the bar ramps green -> yellow -> red as it
    // closes from here.
    float dangerLateralM = 10.0f;

    // A brief, very translucent yellow wash pulsing over the whole radar
    // whenever any tracked car is in the red "danger" zone above -- a
    // peripheral-vision cue that something is close enough to need
    // attention, without the transparency ever climbing high enough to
    // obscure the car blips drawn on top of it.
    bool dangerFlashEnabled = false;
    float dangerFlashR = 1.0f, dangerFlashG = 0.9f, dangerFlashB = 0.1f;
    float dangerFlashMaxAlpha = 0.18f;
    float dangerFlashPeriodSeconds = 0.5f; // one full pulse cycle

    // Diagnostic: logs the player's own world heading and, once a second,
    // the nearest other car's world heading plus the computed lateral/
    // forward/rotation values -- ground truth for tracking down the icon
    // rotation bug, since the icon's orientation can't be judged from a
    // screenshot alone. Temporary; safe to leave off otherwise.
    bool logOrientationDebug = false;

    // --- On-screen side indicator (the red bars down the screen edges) ---
    // Which cars trigger a bar is NOT set here: that is the shared danger
    // window above, so the bars and the red radar blips can never disagree.
    // Everything below is purely how a bar looks once it is showing.
    //
    // Maximum width of one bar, as a fraction of screen width. This is the
    // width at the end nearest the car; the far end tapers in.
    float sideBarWidthFraction = 0.35f;

    // Size multiplier on the width above, and a multiplier on both alpha
    // values below -- adjusted live via the resize handle and opacity slider
    // that appear next to the bar in layout mode (see ProximityHue.cpp's
    // DrawLayoutControls). Not folded through Layout::Register like every
    // other panel's scale: that module's drag model is "grab a box anywhere
    // on screen and move it", which does not fit a bar that must always stay
    // welded to the screen edge -- so this widget gets its own small bespoke
    // controls instead.
    float sideBarScale = 1.0f;
    float sideBarOpacityMultiplier = 1.0f;

    // The bar is one flat colour throughout -- closeness is carried by
    // opacity and shape, never hue.
    float sideBarR = 1.0f, sideBarG = 0.0f, sideBarB = 0.0f;

    // Lateral distance drives base opacity: full strength at or inside
    // SideBarNearMeters, falling to the far value at DangerLateralMeters
    // (where the bar first appears). These are the alpha at the SCREEN EDGE
    // -- the inner-edge fade below means the bar reads as roughly half this
    // across its width.
    float sideBarNearM = 3.0f;
    float sideBarAlphaNear = 0.60f;
    float sideBarAlphaFar = 0.18f;

    // Alpha multiplier at the bar's inner edge. 0 dissolves it completely;
    // anything above 0 leaves a hard vertical line across the view.
    float sideBarInnerEdgeAlpha = 0.0f;

    // Forward offset leans the bar toward the end the car is at: ahead
    // weights the top, behind the bottom, level leaves it even. EndFade is
    // how far the far end's opacity drops at the extreme of the danger
    // window (1 = to nothing), EndTaper how far it narrows (deliberately not
    // 1, so the tip reads as a bar leaning rather than a spike), and Gamma
    // sharpens the response to SMALL offsets -- below 1 a modest offset
    // already produces an obvious lean instead of a barely-visible one.
    float sideBarVerticalEndFade = 1.0f;
    float sideBarVerticalFadeGamma = 0.5f;
    float sideBarWidthEndTaper = 0.75f;

    // Asymmetric on purpose: a warning whose whole job is a safety buffer
    // must not be delayed on the way IN, but a slow release stops the bar
    // strobing when a car hovers right on the activation boundary.
    float sideBarFadeInSeconds = 0.10f;
    float sideBarFadeOutSeconds = 0.35f;

    // Fade-to-green: as a contribution's own time-based fade-out above
    // progresses (the car has left the danger window and its bar is dying
    // away), the bar's colour shifts continuously from red toward this
    // colour instead of just dimming to nothing -- a "returning to safe"
    // cue built into the bar itself. Replaces an earlier separate green
    // "all clear" flash, which was dropped in favour of this.
    bool sideBarFadeToGreenEnabled = true;
    float sideBarFadeToGreenR = 0.15f, sideBarFadeToGreenG = 1.0f, sideBarFadeToGreenB = 0.15f;

    // "Too close" flash: independent of the rule above that decides whether
    // the bar shows at all. Once a car IS triggering the bar, if it is also
    // this close laterally, that car's own contribution blinks between the
    // bar's normal colour and this one (yellow by default) instead of
    // sitting solid -- extra emphasis for a genuinely tight pass. Both
    // MetersMax and FlashHzMax are the slider ceilings for the two layout-
    // mode controls below (0..MetersMax, 1..FlashHzMax).
    bool sideBarTooCloseFlashEnabled = true;
    float sideBarTooCloseMeters = 3.0f;
    float sideBarTooCloseMetersMax = 5.0f;
    float sideBarTooCloseFlashHz = 4.0f;
    float sideBarTooCloseFlashHzMax = 10.0f;
    float sideBarTooCloseFlashR = 1.0f, sideBarTooCloseFlashG = 0.85f, sideBarTooCloseFlashB = 0.0f; // yellow
};

struct PedalConfig
{
    bool enabled = true;

    float widthFraction = 0.3f;
    float heightFraction = 0.08f;

    // Where the panel's centre sits on screen, as a 0..1 fraction -- same
    // convention as RadarConfig's centerX/YFraction. 0.5, 0.5 is dead centre.
    float centerXFraction = 0.5f;
    // Low on screen, clear of the gear/speed panel above it.
    float centerYFraction = 0.9516f;

    // Panel size multiplier, dragged from the corner handle in layout mode
    // (Ctrl+Shift+5) and clamped there to 0.5-2.5. Folded into this panel's
    // own uiScale, so text, padding, gaps and corner radii all scale in
    // exact proportion rather than the box growing around fixed-size text.
    float scale = 1.0f;


    // Same dark, semi-transparent style as the other HUD text boxes
    // (FrameLatency, LapDelta, RelativeTiming, FuelTyrePanel), for a
    // consistent look across the whole overlay.
    float bgR = 0.08f, bgG = 0.08f, bgB = 0.08f, bgA = 0.6f;
    float bgCornerRadiusPx = 10.0f;

    // Thin bezel outline, matching the gear/speed dashboard's, so the two
    // widgets read as one consistent "digital dash" style.
    bool borderEnabled = true;
    float borderThicknessPx = 2.0f;
    float borderR = 0.5f, borderG = 0.5f, borderB = 0.5f;

    // AMBER brake / CYAN throttle, replacing the old red/green pair. Green is
    // a poor HUD colour on a race track (the scenery is grass and trees) and
    // red/green is the worst possible pairing for the ~8% of men with
    // deuteranopia. Moving the pedals here also leaves red/green meaning
    // exactly one thing across this HUD -- faster/slower versus the reference
    // -- instead of two unrelated things.
    float throttleRawR = 0.15f, throttleRawG = 0.85f, throttleRawB = 0.95f;  // cyan
    float throttleActR = 0.05f, throttleActG = 0.45f, throttleActB = 0.55f;  // darker cyan
    float brakeRawR = 1.00f, brakeRawG = 0.70f, brakeRawB = 0.00f;           // amber
    float brakeActR = 0.60f, brakeActG = 0.40f, brakeActB = 0.00f;           // darker amber
    // Matched to their associated pedal: lockup happens under braking, spin
    // happens under throttle, so each defaults to that pedal's raw colour.
    float lockupR = 1.00f, lockupG = 0.70f, lockupB = 0.00f;  // amber, like brake
    float spinR = 0.15f, spinG = 0.85f, spinB = 0.95f;        // cyan, like throttle

    // Force-feedback line (was steering input), drawn across the same plot
    // area as throttle/brake but bipolar (-1..1 mapped to the full plot
    // height, 0 = dead centre) rather than 0..1 from the floor. INI keys
    // kept as SteeringR/G/B so existing colour tuning carries over.
    float steeringR = 0.95f, steeringG = 0.15f, steeringB = 0.15f; // red

    // Wheel slip sensitivity, as a fraction difference between wheel surface
    // speed and ground speed under that wheel.
    // Asymmetric on purpose. Locking under braking and wheelspin under power
    // are different events with different tolerances -- a few percent of
    // lock-up is a genuine flat-spot risk worth flagging, whereas a similar
    // amount of positive slip is just how a rear-drive car puts power down
    // out of a slow corner and is not worth lighting up for. Each side gets
    // its own deadzone and full-scale.
    float slipDeadzone = 0.06f;      // LOCKING: slip below this is ignored
    float slipFullScale = 0.35f;     // LOCKING: slip at/above this is fully saturated
    float spinDeadzone = 0.10f;      // SPIN: higher -- normal power-down slip should not trigger
    float spinFullScale = 0.45f;
    float minSpeedForSlipMs = 2.0f; // m/s -- below this, slip is meaningless (near-stationary)
    // Which axle counts as DRIVEN for spin detection: 0 = all four wheels,
    // 1 = rear only, 2 = front only. Wheelspin can only happen on the driven
    // axle, so scanning all four just adds noise -- but the SDK exposes no
    // drivetrain field, so this cannot be detected and has to be told.
    // Defaults to rear. LOCKING always scans all four regardless, since any
    // wheel can lock under braking.
    int spinAxle = 1;
    // Multiplies the lockup/spin magnitude before display (height AND
    // intensity both scale with it, since both are driven by the same
    // magnitude value) -- makes the indicators reach full size/brightness
    // from a smaller amount of actual slip, so they read as more prominent.
    float slipIntensityMultiplier = 6.0f;
    // Scales the lockup/spin VU-meter bars' max height (as a fraction of
    // their strip) beyond the base 0.42 built into the drawing code -- 2.0
    // doubles it.
    float slipBarHeightScale = 2.0f;

    // The whole panel fades out after being both stationary AND off the
    // pedals for this long -- requiring both avoids fading during a normal
    // lift-and-coast mid-corner (moving, no pedal input), while still fading
    // at a stop, in the pits, or back at the menu. Fades back in over the
    // same duration once either condition is no longer true. Uses wall-clock
    // time, so it keeps fading even if telemetry stops arriving altogether.
    float idleFadeSeconds = 2.0f;

    // The traces are thinned to about one point per this many screen pixels
    // of plot width (the 10 s history is more samples than the plot has
    // pixels). Lower = finer and more vertices to upload each frame.
    float pointSpacingPx = 2.0f;
};

// Shared cadence for the perf overlay's own text refresh. Deliberately does
// NOT touch the smoothing/interpolation time constants used purely for
// on-screen visual smoothness (delta-bar fill easing, colour fades,
// MiniSector's fade-in) -- those affect how things LOOK, not how much work
// gets done, so folding them in here would change visuals while trying to
// isolate a CPU cost.
struct PerfUpdateConfig
{
    // Clamped to [0.2, 30.0] on load -- below that the on-screen readouts
    // go stale enough to be useless, above it you're re-polling PDH/thread
    // times faster than they meaningfully change.
    float updateHz = 2.0f;
};
extern PerfUpdateConfig g_perfUpdateConfig;

struct FrameLatencyConfig
{
    bool enabled = true;

    float textHeightPx = 26.208f;      // 30% larger than 20.16

    // FPS, Display Latency, CPU Busy, and GPU Busy are all joined into one
    // horizontal line ("stat | stat | stat"). cpuGpuPosXFraction is the
    // box's horizontal centre; cpuGpuPosYFraction is its own outer BOTTOM
    // edge (padding included, not a text anchor point) -- so a small
    // fraction here places the box just above that point, e.g. above the
    // in-game rearview mirror, which occupies roughly the top ~0.12 of a
    // 3840x2160 reference screen (see LapDeltaConfig::centerYFraction).
    float cpuGpuPosXFraction = 0.5f;
    float cpuGpuPosYFraction = 0.05f;
    // See the note on RadarConfig::scale.
    float scale = 1.0f;

    // When true (checked first, overrides everything else below), the box
    // sits flush against the screen's own bottom-right corner, padding
    // included.
    bool alignBottomRightCorner = false;

    // When true (checked second, overrides the X/Y fractions but not
    // AlignBottomRightCorner above), the box sits flush against the top of
    // the screen, horizontally centred at cpuGpuPosXFraction -- its own
    // TOP edge (padding included) touches y=0 exactly, rather than the
    // fraction-based positioning below (which anchors the box's BOTTOM
    // edge, so it can't itself put the box flush against the top).
    bool alignTop = true;

    // When true (and both aligns above are false), cpuGpuPosXFraction is
    // ignored and the box's right edge is instead lined up with the pedal
    // trace's own right edge, so the two bottom-of-screen widgets read as
    // one aligned stack.
    bool alignRightToPedalTrace = false;

    float colorR = 1.0f, colorG = 1.0f, colorB = 1.0f;

    // Smoothed frames-per-second, from the real Present-to-Present interval.
    bool showFps = true;

    // Rough total input-to-photon latency estimate (half the average physics
    // tick period + Display Latency above) -- a LOWER BOUND, since it can't
    // see the physical controller's own USB/HID polling latency. See
    // InputLagEstimate.h for the full method and caveats.
    bool showInputLagEstimate = false;
    // Frame time in ms (1000/FPS), shown next to FPS when ShowFps is on.
    bool showFrameTime = true;
    // DXGI-reported present-to-display latency (exclusive fullscreen only).
    bool showDisplayLatency = false;

    // --- Extra diagnostic stats (off by default -- opt in via INI) ---
    //
    // Per-frame CPU compute time of the render/main thread (the one calling
    // Present), via GetThreadTimes deltas -- NOT process-wide CPU%, which is
    // meaningless on a multi-core box when only a couple of threads are
    // doing real work.
    bool showCpuFrameTime = false;
    // Process working-set RAM, via K32GetProcessMemoryInfo.
    bool showProcessRam = false;
    // GPU VRAM currently committed to this process's budget, via
    // IDXGIAdapter3::QueryVideoMemoryInfo (DXGI 1.4+; silently omitted if
    // unavailable, e.g. very old GPU driver).
    bool showVram = false;
    // This plugin's own RAM (DLL image + its heap), and the whole PC's RAM in use / installed.
    bool showPluginRam = false;
    bool showPcRam = false;
    // The game's replay recording (ReplayInfo.h): off / to RAM / to disk with
    // the recording file's size so far, and the replay fidelity setting.
    bool showReplay = false;
    // FPS min / 1%-low / max over the last few seconds, alongside the live
    // smoothed FPS already shown above.
    bool showFpsRange = false;
    // Real GPU busy time in ms: OS "GPU Engine" perf-counter busy% (see
    // GpuEngineUsage.h) times the current frame period -- the same category
    // of figure CapFrameX/Task Manager show, and immune to the pacing/idle
    // time a Present-to-Present timestamp measurement would pick up under
    // a frame cap.
    bool showGpuFrameTime = false;
    // AMD GPU power draw in watts, via ADL2 (see AmdGpuPower.h). Silently
    // stays unavailable on non-AMD hardware or if the driver doesn't
    // expose a power sensor ADL2 recognises.
    bool showGpuPower = false;

    // Dark grey panel behind all the readout lines.
    bool bgEnabled = true;
    float bgPaddingPx = 1.0f;           // tightened from 8.0
    float bgCornerRadiusPx = 8.0f;
    float bgR = 0.08f, bgG = 0.08f, bgB = 0.08f, bgA = 0.6f;
};

struct GearDisplayConfig
{
    bool enabled = true;

    // Default sits just above the pedal trace's default position
    // (PedalConfig::centerYFraction = 0.9149, height 0.066667 -> top edge
    // ~0.8816), same horizontal centre.
    float centerXFraction = 0.5f;
    float centerYFraction = 0.83f;

    // --- Speed-box slip feedback ---
    //
    // The speed panel's background reports what the tyres are actually
    // doing longitudinally: amber under locking, cyan under wheelspin.
    //
    // An "ideal braking" green band was considered and deliberately left
    // out: peak-grip slip moves with compound, tyre temperature, load and
    // surface, and the SDK exposes no grip curve to read the real figure
    // from, so any fixed band would be a guess presented as fact.
    bool slipFeedbackEnabled = true;
    // Braking slip (as a positive magnitude) above this is locking, judged
    // on the WORST wheel -- any one wheel locking is worth flagging even if
    // the axle average looks fine. Comfortably past the ~0.08-0.15 where
    // peak braking grip typically sits, so braking hard and well does not
    // trip it.
    float lockSlipThreshold = 0.20f;
    // Driven-axle slip above this is wheelspin. Well above the 0.10-0.15 of
    // normal power-down slip on corner exit, so it flags provoked spin
    // rather than a good exit.
    float spinSlipThreshold = 0.25f;
    // Ignore all of the above unless the brake is actually pressed this
    // hard -- engine braking and coasting produce slip that would otherwise
    // tint the box green through every corner entry.
    float slipFeedbackBrakeInput = 0.20f;
    // A state must persist this long before it shows (one noisy sample must
    // not flash the box), and once shown it holds at least this long (a real
    // but brief lockup must not be a single invisible frame).
    float slipFeedbackDwellSeconds = 0.06f;
    float slipFeedbackHoldSeconds = 0.25f;
    // Lock amber and spin cyan match PedalConfig's lockup/spin colours, so
    // the two widgets tell the same story in the same colours.
    float lockSlipR = 1.00f, lockSlipG = 0.70f, lockSlipB = 0.00f;
    float spinSlipR = 0.15f, spinSlipG = 0.85f, spinSlipB = 0.95f;

    // --- ABS / TC indicator ---
    //
    // Its own small box to the LEFT of the gear digit, drawn only while the
    // electronics are actually intervening, naming which one. This used to
    // be a yellow flash of the SPEED box, which now carries the slip
    // feedback above -- and "the electronics caught it" and "the tyres are
    // slipping" are different enough facts to deserve separate places.
    // Not separately positionable: it rides with the gear panel.
    bool absTcBoxEnabled = true;
    // Which interventions light the box (toggled from the shortcut list).
    // The pit-limiter "PIT" readout is not affected by either.
    bool showAbs = true;
    bool showTc = true;

    float absTcBoxGapPx = 14.0f;
    // Colour comes from absTcFlash* above -- the same yellow this used to
    // flash on the speed box, kept so the meaning of the colour is unchanged
    // and any tuning of it carries over.

    // --- Four-corner tyre lockup box, next to the speed panel ---
    //
    // A 2x2 grid matching the car's actual corners (FL/FR front, RL/RR rear
    // -- same physical layout as FuelTyrePanel's own tyre grid), each
    // quadrant lighting up BY INTENSITY (not just on/off) the moment that
    // wheel is locking under braking. Reuses lockSlipThreshold above as the
    // "locking at all" cutoff; tyreLockupFullScale is where a quadrant
    // reaches full intensity. Independently positioned (sentinel auto-anchor,
    // same convention as shiftStats below) rather than welded to the gear
    // panel, since it may be wanted somewhere else entirely once
    // it has been seen in place.
    bool tyreLockupBoxEnabled = true;
    float tyreLockupPosXFraction = -1.0f;
    float tyreLockupPosYFraction = -1.0f;
    float tyreLockupScale = 1.0f;
    // How far past lockSlipThreshold a wheel's raw slip needs to be for that
    // quadrant to read as FULLY lit -- the intensity in between is a linear
    // ramp (a scale) rather than a flat on/off.
    float tyreLockupFullScale = 0.6f;
    // Once a corner stops locking it does not vanish instantly: its
    // intensity fades linearly to zero over this many seconds, so a lockup
    // on corner entry is still visible on corner exit / into the next
    // braking zone, not just for the instant it happened.
    float tyreLockupFadeSeconds = 5.0f;
    float tyreLockupR = 1.00f, tyreLockupG = 0.70f, tyreLockupB = 0.00f; // matches lockSlipR/G/B above

    // Panel size multiplier, dragged from the corner handle in layout mode
    // (Ctrl+Shift+5) and clamped there to 0.5-2.5. Folded into this panel's
    // own uiScale, so text, padding, gaps and corner radii all scale in
    // exact proportion rather than the box growing around fixed-size text.
    float scale = 1.0f;

    float gearTextHeightPx = 165.0f;   // 110 * 1.5

    float normalR = 1.0f, normalG = 1.0f, normalB = 1.0f; // white

    // No SDK field gives an explicit "optimal shift RPM" -- this is a
    // fraction of the engine's actual rev limit instead.
    float shiftRpmFraction = 0.93f;
    float shiftR = 1.0f, shiftG = 0.15f, shiftB = 0.15f;   // red

    // Target fraction of maxRpm to actually shift AT -- used as the seed
    // before any upshift has been observed, and as the fallback whenever
    // learnedShiftRpmFraction/learnedShiftRpmFractionByGear haven't got a
    // value yet. Despite the name this is no longer "flash at the true rev
    // limiter": the blue cue below fires LED BACK from this point by the
    // reaction-time lead, so a shift landing on it is the intended outcome,
    // not something that already used up all the RPM.
    float revLimiterRpmFraction = 0.96f;
    float revLimiterR = 0.25f, revLimiterG = 0.5f, revLimiterB = 1.0f; // blue: the "shift now" cue

    // A THIRD state, above the blue cue: you are genuinely about to hit the
    // physical rev limiter, regardless of what the adaptive target thinks --
    // e.g. holding a gear on purpose, or the cue's own target having drifted
    // somewhere that leaves too little margin. Checked directly against
    // maxRpm, never against the learned target, so it can't be tuned away by
    // the same learning that drives the blue cue. Takes priority over both
    // the blue cue and the red warning.
    float trueLimiterWarnFraction = 0.995f;
    float trueLimiterR = 0.6f, trueLimiterG = 0.0f, trueLimiterB = 0.8f; // purple

    // Gear-panel background warnings, independent of and checked BEFORE the
    // RPM-based ones above: being in neutral or first gear is itself the
    // thing worth flagging, whatever the RPM happens to be at that moment
    // (idling in neutral is never near the limiter anyway). Neutral takes
    // priority over first gear when somehow both could apply.
    bool neutralWarnEnabled = true;
    float neutralWarnR = 1.0f, neutralWarnG = 0.05f, neutralWarnB = 0.05f;  // bright red
    bool firstGearWarnEnabled = true;
    float firstGearWarnR = 1.0f, firstGearWarnG = 0.45f, firstGearWarnB = 0.0f; // orange

    // How far ahead of the target RPM (in seconds of RPM-climb) the blue cue
    // fires, so your own reaction time is spent BEFORE the target instead of
    // pushing the actual shift past it -- the seed here is only used until
    // enough real reactions have been measured (see
    // State::shiftReactionAvgSeconds); ShiftReactionAdaptiveEnabled=0 pins it
    // to the seed permanently instead. Min/max bound both the seed and the
    // learned value, so that a
    // driver who reliably shifts later than the cue (rather than reacting to
    // it) can't march the lead outward without limit. TimeoutSeconds
    // abandons a cue that never gets answered by an upshift at all (lifted
    // off, braked for a corner instead) rather than pairing it with some
    // unrelated later shift.
    bool shiftReactionAdaptiveEnabled = true;
    float shiftReactionDefaultSeconds = 0.15f;
    float shiftReactionMinSeconds = 0.02f;
    float shiftReactionMaxSeconds = 0.6f;
    float shiftReactionTimeoutSeconds = 2.0f;

    // The cue only means anything while you are actually trying to
    // accelerate toward the limiter -- gated on raw throttle so a
    // rev-matched downshift's blip (which can spike RPM briefly with the
    // driver already off the power and braking) doesn't light the cue, and
    // more importantly doesn't LATCH it lit for the rest of the braking
    // zone. Deliberately lower than gearLearnMinThrottle: this is "are you
    // trying to go, at all", not "are you fully committed", since a partial-
    // throttle corner exit that's still nearing the limiter in a low gear
    // genuinely does want the cue.
    float shiftCueMinThrottle = 0.5f;

    // A shift landing further than this from the target it was aiming at
    // (as a fraction of maxRpm) is treated as an outlier and excluded from
    // learning and stats alike (see gearLearnMinThrottle below for the more
    // common case this doesn't catch). Without this one 72%-of-redline shift
    // moves the target by ~3.7% in a single step at the current learn rate.
    float shiftLearnOutlierBand = 0.10f;

    // Only an upshift taken at genuine full throttle says anything about
    // where the optimal shift point is -- a deliberate short-shift into a
    // corner is, by definition, off-throttle (you are about to brake, not
    // power through), and its RPM has nothing to do with the car's redline.
    // shiftLearnOutlierBand alone does NOT catch this: a short-shift 9%
    // below a 97% target lands inside a 10% outlier band and gets learned
    // as a real correction, dragging the target down to match a shift that
    // was never trying to reach it. Checked on the RAW (unfiltered) pedal --
    // what the driver actually pressed -- from the sample just before the
    // gear change. Below this threshold the shift is excluded from BOTH
    // learning and the on-screen shift stats: it isn't data about the
    // upshift point at all, so counting it in "Shift Avg" would be just as
    // misleading as feeding it to the learner.
    float gearLearnMinThrottle = 0.95f;

    float blinkPeriodSeconds = 0.15f; // one full on/off cycle

    // Gear and speed each sit in their OWN dark rounded-corner panel (like the
    // reference in-game HUD's separate segment displays), rather than one
    // shared box -- gapPx is the empty space between the two.
    bool backgroundEnabled = true;
    float bgPaddingPx = 26.0f;         // margin around the text inside each panel
    float bgCornerRadiusPx = 14.0f;
    float gapPx = 20.0f;               // gap between the gear panel and the speed panel
    float bgR = 0.02f, bgG = 0.02f, bgB = 0.02f, bgA = 0.75f;

    // Thin bezel outline around each panel, matching the pedal trace's, so
    // the two widgets read as one consistent "digital dash" style rather than
    // flat boxes.
    bool borderEnabled = false;
    float borderThicknessPx = 2.0f;
    float borderR = 0.5f, borderG = 0.5f, borderB = 0.5f;

    // Speed, drawn in its own panel to the right of the gear panel.
    bool speedEnabled = true;
    bool speedUnitsKmh = true;      // false = mph
    float speedTextHeightPx = 96.0f;   // 64 * 1.5
    float speedR = 1.0f, speedG = 1.0f, speedB = 1.0f; // white
    float dividerR = 0.4f, dividerG = 0.4f, dividerB = 0.4f; // unused now boxes are separate; kept for INI compatibility

    // Speed panel's BACKGROUND (not the number, same convention as the gear
    // panel's shift/rev-limiter flash) flashes this colour while ABS or TC
    // is actually intervening -- detected as the SDK's filtered (post-
    // electronics) brake/throttle sitting meaningfully below the raw input,
    // since there's no direct "ABS/TC active" telemetry field.
    float absTcFlashR = 1.0f, absTcFlashG = 0.9f, absTcFlashB = 0.1f; // yellow
    float absTcThreshold = 0.05f;
    // rF2 also cuts throttle through the SAME filtered channel during a gear
    // change (the auto-lift / ignition cut), which is byte-for-byte the
    // signature TC produces -- so the flash fired on every upshift. The
    // THROTTLE half of the test is therefore ignored for this long after any
    // gear change. The brake half (real ABS) is unaffected and never
    // suppressed.
    float tcIgnoreAfterShiftSeconds = 0.35f;

    // Whole dashboard (gear + speed + pit box) fades out under the same
    // condition as the pedal trace -- stationary AND off both pedals -- and
    // back in as soon as either is no longer true. Shares the pedal trace's
    // own activity tracking rather than duplicating it, so the two widgets
    // always agree on "idle".
    float idleFadeSeconds = 2.0f;

    // Rolling "how am I actually shifting" readout, below the dashboard --
    // average and 1st-percentile shift point, both as a percentage of
    // maxRpm. Needs at least one observed upshift to show anything.
    bool showShiftStats = true;
    float shiftStatsTextHeightPx = 20.0f;
    float shiftStatsGapPx = 8.0f;
    float shiftStatsR = 0.8f, shiftStatsG = 0.8f, shiftStatsB = 0.8f;
    // -1 sentinel means "auto-anchor just below the gear panel" -- same
    // sentinel convention as ShortcutConfig::listPosXFraction/YFraction.
    // Resolved once below into an absolute fraction the first time this
    // draws, then draggable/resizable independently of the gear panel from
    // there on (own Layout::Register entry, own scale).
    float shiftStatsPosXFraction = -1.0f;
    float shiftStatsPosYFraction = -1.0f;
    float shiftStatsScale = 1.0f;
    // Same dark translucent plate every other readout box uses (the
    // pit-limiter box, etc) -- this one used to draw
    // bare text straight over whatever's on screen behind it.
    bool shiftStatsBgEnabled = true;
    float shiftStatsBgPaddingPx = 8.0f;
    float shiftStatsBgCornerRadiusPx = 8.0f;
    float shiftStatsBgR = 0.08f, shiftStatsBgG = 0.08f, shiftStatsBgB = 0.08f, shiftStatsBgA = 0.6f;
};

struct PitLimiterConfig
{
    // The old separate PIT box beside the dashboard (fill colour,
    // limit/delta readout) is off by default: the limiter now shows as "PIT"
    // in the ABS/TC box (GearDisplay.cpp), which this flag does not affect.
    bool enabled = false;
    // Only drawn while the limiter is actually engaged -- an indicator that's
    // always on screen regardless of relevance would just be noise.

    // Horizontal position is automatic -- flush against whichever of the
    // gear/speed dashboard's panels ends up rightmost, same gap as between
    // them. This is a small vertical-only nudge, in fractions of screen
    // height, relative to GearDisplayConfig's centre.
    float offsetYFraction = 0.0f;

    float textHeightPx = 56.0f;
    float paddingPx = 18.0f;

    float textR = 1.0f, textG = 1.0f, textB = 1.0f;       // white

    // Fallback only -- normally learned automatically from the limiter's own
    // cap and persisted per car+track (see GearDisplayStore); this is what's
    // used before a value has been learned or loaded for the current combo.
    float speedLimitKmh = 80.0f;

    // Green at/under the limit, fading through orange, to red as speed goes
    // further over it -- blinks once over by OverLimitFarKmh or more rather
    // than at the first km/h over.
    float fillR = 0.15f, fillG = 1.0f, fillB = 0.15f;                  // green, at/under the limit
    float midR = 1.0f, midG = 0.6f, midB = 0.1f;                       // orange, moderately over
    float overLimitR = 1.0f, overLimitG = 0.15f, overLimitB = 0.15f;   // red, way over
    float overLimitFarKmh = 15.0f;             // km/h over the limit that reads as fully red + starts blinking
    float overLimitFlashPeriodSeconds = 0.2f;  // one full on/off flash cycle, once in the "way over" zone

    // Two extra lines beneath "PIT": the limit itself, and current speed
    // minus that limit (positive = still over, negative = under) -- both in
    // the same units as the main speed readout (GearDisplayConfig::speedUnitsKmh).
    bool showLimitAndDelta = true;
    float infoTextHeightPx = 24.0f;
    // The delta line specifically -- deliberately large and always plain
    // white (textR/G/B), since the box's own fill colour already carries the
    // over/under-limit signal (red/green); a same-coloured delta number just
    // disappears into it when at/under the limit.
    float deltaTextHeightPx = 44.0f;

    // PIT LIMITER bar: centre position (fractions of screen; -1 = auto,
    // laid over the gear/speed row) and size multiplier. Draggable/resizable
    // in layout mode.
    float barXFraction = -1.0f;
    float barYFraction = -1.0f;
    float barScale = 1.0f;
};

// Warning lines above the gear/speed boxes (see TrackAlerts.h): a stopped
// car (or several) ahead with a distance countdown, yellow-flag sectors,
// and the pit-lane speed limit as a reminder once a pit stop is requested.
// The pit-lane limit itself is set per track with a wheel knob ("Pit lane
// limit") and saved in a PitLaneLimits folder next to this INI.
struct TrackAlertConfig
{
    bool enabled = true;
    // Only cars this far ahead of you along the lap are looked at.
    float rangeMeters = 1000.0f;
    // Under this a car counts as stopped -- a racing car this slow on track
    // is as good as stopped.
    float stoppedKmh = 40.0f;
    // Slow-corner guard: where your own best lap was itself that slow (a
    // hairpin taken at 45 km/h), a car only counts under this share of your
    // speed there, so normal traffic in the hairpin is not reported. Ignored
    // until there is a best lap for this car and track. 0 = off.
    float slowCornerFraction = 0.8f;
    // Stopped cars this close to each other along the track are one incident:
    // one line, the distance to the first, and how many cars.
    float groupMeters = 150.0f;
    // How long a car has to stay slow before it is reported, and fast again
    // before the warning clears -- so a car that just runs wide or a spin
    // that is over in a moment does not flash up.
    float dwellSeconds = 1.0f;
    // Inside this distance the warning flashes.
    float flashWithinMeters = 200.0f;
    // Which sector has a yellow flag (needs local yellows on the server).
    bool showSectorYellow = true;
    // "PIT LIMIT 60 km/h" above the speed box once a pit stop is requested.
    bool showPitLimitReminder = true;
    // Text height (at 2160p, scaled with the dashboard) and the gap above
    // the dashboard / between the two lines.
    float textHeightPx = 56.0f;
    float gapPx = 10.0f;
    float r = 1.0f, g = 0.85f, b = 0.1f;    // yellow
};

struct LapDeltaConfig
{
    bool enabled = true;

    float centerXFraction = 0.5f;
    // Positioned well clear of the in-game rearview mirror at the top --
    // increased from 0.18 to avoid overlap at various resolutions/mirror sizes.
    // The MiniSector strips anchored above it move along with it. Raised
    // again from 0.30 to clear it by a further 2cm on a
    // 32" 16:9 panel -- that screen is ~39.9cm tall, so 2cm is ~0.050 of
    // screen height. Then a further 1cm (~0.025) on top of that.
    float centerYFraction = 0.225f;

    // Panel size multiplier, dragged from the corner handle in layout mode
    // (Ctrl+Shift+5) and clamped there to 0.5-2.5. Folded into this panel's
    // own uiScale, so text, padding, gaps and corner radii all scale in
    // exact proportion rather than the box growing around fixed-size text.
    float scale = 1.0f;

    // A third of the screen. The MiniSector strips sit directly above this
    // bar and match its width by default (see MiniSectorConfig), so the
    // three read as one stack -- change this and they follow.
    float barWidthFraction = 0.3333f;
    // Must stay taller than textHeightPx below -- the bar IS the Session
    // row's background now (the number is drawn centred on top of it, no
    // separate grey panel), so a bar thinner than its own text just looks
    // like the text is spilling out of it.
    float barHeightPx = 54.0f;
    float numberGapPx = 6.0f;
    float textHeightPx = 41.6f;        // 20% smaller than the doubled 52.0
    float rowGapPx = 10.0f;            // between the stacked Session and Best rows

    // The PB (all-time best) row and the DRS indicator each have their own
    // position, so they can be placed independently of the session-best bar
    // they used to be welded beneath.
    //
    // -1 is a SENTINEL meaning "wherever you used to sit": stacked under the
    // session row for the PB, just below both rows for DRS. On the first
    // frame that position is resolved into a real fraction, in place, and
    // the element is free-standing from then on. That keeps the default
    // layout exactly as it was while making both draggable -- a hardcoded
    // default pair would have moved them the moment this build was
    // installed, and the correct value depends on whether the session row
    // drew at all that frame.
    float bestCenterXFraction = -1.0f;
    float bestCenterYFraction = -1.0f;
    float drsCenterXFraction = -1.0f;
    float drsCenterYFraction = -1.0f;

    // Delta beyond this (seconds) fills the bar completely -- also the
    // WINDOW'S FLOOR for the auto-sizing below: the bar's range never
    // shrinks smaller than this no matter how clean the previous lap was.
    float maxBarSeconds = 1.0f;

    // Auto-sizing: the window resets to the floor (maxBarSeconds above) at
    // the start of every lap, then grows live, in whole-second steps, only
    // if THAT lap's own delta actually needs it -- so a bad lap never just
    // pins the bar at max with no further information, but a clean lap
    // right after it starts back at the tightest, most readable scale
    // rather than staying pre-sized from history. Never grows past
    // barWindowMaxSeconds. Set to 0 to go back to a fixed maxBarSeconds-wide
    // window, like before.
    bool autoBarWindowEnabled = true;
    float barWindowMaxSeconds = 10.0f;

    // The Best row has no bar at all -- just a plain number, coloured green
    // when ahead of the reference, red when behind. No brightness/trend
    // involved here.
    float numberAheadR = 0.15f, numberAheadG = 1.0f, numberAheadB = 0.15f;   // green, delta <= 0
    float numberBehindR = 1.0f, numberBehindG = 0.15f, numberBehindB = 0.15f; // red, delta > 0

    // The Session row's number is drawn centred ON its bar instead, always
    // plain white -- the bar's own fill colour (trend-based) already carries
    // the gaining/losing signal, so the number doesn't need to repeat it.
    float barNumberR = 1.0f, barNumberG = 1.0f, barNumberB = 1.0f;

    // A thin dark outline around that number -- without it, half the text
    // sits over the bar's plain grey background and half over its coloured
    // fill (wherever that boundary happens to land), giving it two
    // different, inconsistent backgrounds at once. The outline keeps it
    // readable against either.
    bool barNumberOutlineEnabled = true;
    float barNumberOutlineR = 0.0f, barNumberOutlineG = 0.0f, barNumberOutlineB = 0.0f;
    float barNumberOutlineThicknessPx = 2.0f;

    // The BAR's colour follows the TREND instead: flat green while the delta
    // is moving more negative (gaining time), flat red while it's moving
    // more positive (losing time) -- independent of whether the delta itself
    // is still ahead or behind, so bleeding back an advantage shows red on
    // the bar immediately. Single flat colour each way, no brightness scale.
    float gainingR = 0.15f, gainingG = 1.0f, gainingB = 0.15f;      // green, gaining
    float losingR = 1.0f, losingG = 0.15f, losingB = 0.15f;         // red, losing

    // How long a colour change (gaining <-> losing) takes to actually reach
    // its target, rather than snapping the instant the trend flips -- a hard
    // cut reads as a strobe if the trend keeps flickering right at the
    // boundary; a crossfade reads as one colour smoothly becoming the other.
    float barColorFadeSeconds = 1.0f;

    // The trend is measured as (delta now - delta this many seconds ago) /
    // that same span -- a longer window reads as a real gain/loss trend,
    // a short one is noisier but reacts faster.
    float trendWindowSeconds = 2.0f;

    // The BAR's colour is decided separately from trendWindowSeconds above,
    // and from the drawn value rather than from a fresh sample of the raw
    // delta. The rule is exactly what the eye sees: the bar is red while it
    // is GROWING and green while it is SHRINKING, on either side of centre.
    // (Growing to the right means losing more time; growing to the left
    // means gaining more, so the same "growing = worse" reading does not
    // apply there -- what is compared is the delta value itself, which
    // increasing always means worse and decreasing always means better.)
    //
    // Sampled every frame against the drawn value from colorWindowSeconds
    // ago. That window is short because the colour should track the bar,
    // but not so short that the ~5Hz scoring steps dominate it.
    // trendWindowSeconds is left alone: it still drives the between-update
    // extrapolation, where a longer, steadier estimate is wanted.
    float colorWindowSeconds = 0.4f;
    // Changes smaller than this over the whole window count as static, and
    // the previous colour is held -- otherwise the bar strobes red/green on
    // measurement noise whenever the delta is genuinely flat.
    float colorDeadbandSeconds = 0.004f;

    // After crossing the finish line, HOLD the delta shown at the line for
    // this many seconds instead of immediately restarting from ~0 for the
    // new lap -- the one moment the number matters most is also the moment
    // it normally vanishes, and the home straight is where there is time to
    // read it. 0 disables the hold.
    float freezeAfterLineSeconds = 3.0f;

    // Dark grey panel behind the bar+number, same idea as the FPS/perf
    // stats box -- keeps it readable over any track scenery.
    bool bgEnabled = true;
    float bgPaddingPx = 4.0f;          // tightened from 10.0 -- the two rows' boxes were overlapping
    float bgCornerRadiusPx = 8.0f;
    float bgR = 0.08f, bgG = 0.08f, bgB = 0.08f, bgA = 0.6f;

    // Small green box just below the bar(s): "OPEN DRS" while legal but not
    // yet activated, "DRS ENGAGED" once it is -- hidden the rest of the
    // time. Driven by DrsCueStore, drawn here since it's positioned
    // relative to this bar.
    bool drsIndicatorEnabled = true;
    float drsIndicatorGapPx = 10.0f;
    float drsIndicatorTextHeightPx = 28.0f;
    float drsIndicatorPaddingPx = 10.0f;
    float drsIndicatorBgCornerRadiusPx = 8.0f;
    float drsIndicatorBgR = 0.15f, drsIndicatorBgG = 0.8f, drsIndicatorBgB = 0.15f, drsIndicatorBgA = 0.85f;
    float drsIndicatorTextR = 1.0f, drsIndicatorTextG = 1.0f, drsIndicatorTextB = 1.0f;
    // While it's legal to open DRS but not yet engaged, the whole box
    // flashes on/off to draw the eye -- once actually engaged it holds
    // solid, since at that point it's just confirming state, not asking for
    // action. One full on/off cycle, in seconds.
    float drsIndicatorFlashPeriodSeconds = 0.4f;

    // The DRS box and the PB row are separately positionable, so they are
    // separately SIZEABLE too -- each has its own corner handle in layout
    // mode. Scale above governs the delta bar itself and no longer reaches
    // these two.
    float drsScale = 1.0f;
    float bestScale = 1.0f;

    // Small live readout of rF2's own lap-validity state (mCountLapFlag,
    // mPitState) below the bar -- added after a session where every lap
    // silently failed to build a reference (flag stuck at 1, a pit-lane
    // crossing on every lap) with nothing on screen to explain why until the
    // log was checked afterward. Purely diagnostic, so off by default;
    // turn on with [LapDelta] ShowValidityDebug=1.
    bool showValidityDebug = false;
    float validityDebugTextHeightPx = 22.0f;
    float validityDebugGapPx = 6.0f;
};

// Two lap-long strips above the delta bar: previous completed lap on top, the
// lap in progress below it, each mini-sector coloured by how it compared to
// the reference lap. See MiniSector.h for why the binning is by distance.
struct MiniSectorConfig
{
    bool enabled = true;

    // Mini-sectors per lap. The strip always spans exactly one lap whatever
    // the track length, so this is a COUNT, not a length -- each segment is
    // trackLength/count metres. Capped by MiniSectorStore::kMaxSegments.
    int segmentCount = 200;

    // Width follows the delta bar's own (LapDeltaConfig::barWidthFraction) so
    // the strips and the bar under them always line up, however that one is
    // nudged. Set MatchDeltaBarWidth=0 to size them independently via
    // widthFraction instead.
    bool matchDeltaBarWidth = true;
    float widthFraction = 0.3333f;
    float centerXFraction = 0.5f;
    // Vertical position. -1 is a SENTINEL meaning "stacked directly above
    // the lap-delta bar", which is where this strip lived when it had no Y
    // of its own -- dragging the delta bar moved it too, and it could not be
    // moved sideways independently at all. The first frame resolves the
    // sentinel into a real fraction and the strip becomes a free-standing
    // box like every other panel.
    float centerYFraction = -1.0f;


    // Panel size multiplier, dragged from the corner handle in layout mode
    // (Ctrl+Shift+5) and clamped there to 0.5-2.5. Folded into this panel's
    // own uiScale, so text, padding, gaps and corner radii all scale in
    // exact proportion rather than the box growing around fixed-size text.
    float scale = 1.0f;

    float barHeightPx = 54.0f;
    float rowGapPx = 6.0f;            // between the two strips
    float gapAboveDeltaBarPx = 10.0f; // between the lower strip and the delta bar

    // Which reference the segments are timed against -- the session's best
    // lap, or the all-time best persisted to disk. Same two references
    // LapDelta itself shows.
    bool useSessionReference = true;

    // The colour scale is a fixed LADDER of rungs (scaleStepKmh, 2x, 3x, ...
    // up to scaleStepCount rungs), not a value that fits smoothly to the
    // data -- full green/red saturation sits at +/- the SMALLEST rung that
    // still covers scalePercentile% of |speed delta| currently on screen
    // (both strips combined). So most laps sit on the tightest rung (most
    // sensitive), and one genuinely bad segment jumps the WHOLE scale up a
    // rung rather than just clipping unremarked. Recomputed every frame, so
    // the previous lap's strip is always shown under the CURRENT rung, not
    // whatever was live when it was the current lap.
    float scaleStepKmh = 5.0f;
    int scaleStepCount = 6;             // ladder tops out at 6 * 5 = 30 km/h
    float scalePercentile = 95.0f;

    // Small "+X / -X km/h" readout beside the strips showing the rung
    // currently in use -- needed because it is no longer a fixed, memorised
    // number.
    bool showScaleLabels = true;
    float scaleLabelTextHeightPx = 20.0f;
    float scaleLabelGapPx = 10.0f; // gap between the strip stack and the labels

    float fasterR = 0.15f, fasterG = 1.0f, fasterB = 0.15f;  // green, quicker than reference -- also the "+X" label colour
    float slowerR = 1.0f, slowerG = 0.15f, slowerB = 0.15f;  // red, slower than reference -- also the "-X" label colour
    float neutralR = 0.55f, neutralG = 0.55f, neutralB = 0.55f; // grey, on the reference

    // TEST scale, requested to compare against the default two-stop
    // red/green lerp above: red (full slower) -> a middle "intermediate"
    // colour (blue by default) -> green (full faster), instead of running
    // red straight into green through its own muddy midpoint. Off by
    // default -- flip on in the INI to try it.
    bool useTriColorScale = false;
    float triMidR = 0.15f, triMidG = 0.15f, triMidB = 1.0f; // blue

    // Two extra stops (a diverging RdYlGn-style scale) between neutral and
    // each full-saturation end, so a moderate gain/loss reads as a visibly
    // DIFFERENT shade from a small one -- a flat two-colour lerp made every
    // non-trivial deficit look the same shade of "pretty red" or "pretty
    // green" well before it reached the scale's actual ends, which hid real
    // differences in how much time was actually being gained or lost.
    // Halfway to "faster" (green) end.
    float midFasterR = 0.65f, midFasterG = 0.80f, midFasterB = 0.15f;   // yellow-green
    // Halfway to "slower" (red) end.
    float midSlowerR = 0.95f, midSlowerG = 0.55f, midSlowerB = 0.10f;   // amber/orange

    // A segment this close to zero deficit is noise, not a real pace
    // difference -- 5Hz-resync jitter and the per-segment timing itself can
    // flip the sign from one narrow slice to the next even at genuinely even
    // pace, which read as the strip flickering red/green for no reason.
    // Anything inside the deadband is drawn neutral instead of tinting
    // either way; a real gain or loss of a few km/h is unaffected.
    float neutralDeadbandKmh = 0.8f;
    float alpha = 0.95f;

    // A continuous thin line along each strip's own bottom edge, GREEN where
    // that mini-sector was driven under acceleration and RED where the brake
    // was used. Replaces the single tick that used to mark only where a
    // braking application STARTED: the line shows the whole braking zone and
    // its length, which is the part worth comparing lap to lap, and it costs
    // no extra vertical space. Braking is flagged per segment, so "on the
    // brakes" means one consistent thing everywhere. Sized to sit inside the
    // gap below the strip (rowGapPx / gapAboveDeltaBarPx) rather than growing
    // the widget, so keep it shorter than the smaller of those two.
    bool accelLineEnabled = true;
    float accelLineHeightPx = 5.0f;
    // Each pedal is tested against its own threshold here rather than
    // sharing one: this line only wants to know whether a pedal is being
    // used at all.
    float accelLineBrakeThreshold = 0.10f;
    float accelLineThrottleThreshold = 0.10f;
    // Same amber/brake, cyan/throttle pair the pedal trace uses, so the two
    // widgets read as one colour code rather than two.
    float accelLineAccelR = 0.15f, accelLineAccelG = 0.85f, accelLineAccelB = 0.95f;
    float accelLineBrakeR = 1.00f, accelLineBrakeG = 0.70f, accelLineBrakeB = 0.00f;
    float accelLineAlpha = 1.0f;

    bool bgEnabled = true;
    float bgPaddingPx = 6.0f;
    float bgCornerRadiusPx = 8.0f;
    float bgR = 0.08f, bgG = 0.08f, bgB = 0.08f, bgA = 0.6f;
};

struct RelativeTimingConfig
{
    bool enabled = true;

    // How many cars the table shows at once -- the slider in the shortcuts
    // list (Ctrl+Shift+<List>) drives this live. In a race it is the total
    // window size (leader + however many places fit around the player); in
    // practice/qualy it is how many of the nearest-by-distance cars are
    // kept. Stored as a float to reuse Config's existing WriteFloat/
    // ReadFloat live-persistence path (same convention as every other
    // slider-driven value here), rounded to an int wherever it's used.
    // Clamped to [kMinVisibleCars, kMaxVisibleCars] -- see RelativeTiming.h
    // for why kMaxRows has the headroom it does.
    float visibleCars = 6.0f;
    // Races: list the cars in the order they are on TRACK around you (any
    // lap, marked +1L / -1L), with on-track gaps -- instead of the places
    // either side of yours in the standings. Off = the old standings window.
    bool raceTrackOrder = true;
    static constexpr float kMinVisibleCars = 3.0f;
    static constexpr float kMaxVisibleCars = 12.0f;

    // (posXFraction, posYFraction) anchors the panel's own top-right outer
    // corner, padding included -- per the "top right = timing metrics" HUD
    // layout convention. 1.0, 0.0 is the screen's exact top-right corner --
    // flush against it, since the panel's own padding already provides the
    // visual margin.
    float posXFraction = 1.0f;
    float posYFraction = 0.0f;

    // Panel size multiplier, dragged from the corner handle in layout mode
    // (Ctrl+Shift+5) and clamped there to 0.5-2.5. Folded into this panel's
    // own uiScale, so text, padding, gaps and corner radii all scale in
    // exact proportion rather than the box growing around fixed-size text.
    float scale = 1.0f;


    float textHeightPx = 44.0f;
    float rowGapPx = 12.0f;
    float columnGapPx = 32.0f;

    float colorR = 1.0f, colorG = 1.0f, colorB = 1.0f;              // normal (white) row text
    float playerR = 1.0f, playerG = 0.85f, playerB = 0.2f;          // the player's own row
    // Place and name of a car at least a full lap behind the player (race
    // sessions only -- laps completed means nothing in practice).
    float lapDownR = 0.35f, lapDownG = 0.65f, lapDownB = 1.0f;      // blue
    // A car a lap or more AHEAD of you (lapping you, or will).
    float lapUpR = 1.0f, lapUpG = 0.45f, lapUpB = 0.35f;            // red

    // Gap-number colour follows the TREND, same idea as LapDelta's bar: a
    // positive trend always means the gap is moving in the player's favour
    // (closing on a car ahead, pulling away from one behind, or closing on
    // the leader in the player's own row) -- one rule colours every row.
    float gainingR = 0.15f, gainingG = 1.0f, gainingB = 0.15f;      // green
    float losingR = 1.0f, losingG = 0.15f, losingB = 0.15f;         // red

    // Lap-time colour (non-player rows only) compares that car's last lap
    // directly against the PLAYER's own last lap: red if they were faster
    // than you (they're catching/beating you this lap), green if slower
    // (you're gaining on or pulling away from them this lap).
    float lapFasterR = 1.0f, lapFasterG = 0.15f, lapFasterB = 0.15f; // red -- their lap beat yours
    float lapSlowerR = 0.15f, lapSlowerG = 1.0f, lapSlowerB = 0.15f; // green -- yours beat theirs

    // How often the gaining/losing colour re-judges direction: measured as
    // (gap now - gap this many seconds ago) / that same span. The gap NUMBER
    // itself updates continuously (every ~5Hz scoring tick, smoothly
    // interpolated for display) -- only the colour is held to this cadence,
    // so a short value keeps the colour changing in step with the number
    // rather than visibly lagging behind it by however long the old, longer
    // window took to next re-judge.
    float trendWindowSeconds = 0.5f;

    // In a race, a car on a DIFFERENT lap that is within this many seconds
    // of you ON TRACK gets a second row showing that track-relative gap --
    // the place-based window can't surface it (if you are last, nobody is
    // "2 places behind" you), so without this the leader about to lap you
    // shows only his standings gap. Marked with the lap difference, e.g. "+1L".
    float trackRelativeThresholdSeconds = 10.0f;

    // Diagnostic: log the SDK's own mTimeBehindNext/mTimeBehindLeader once a
    // second, to check whether those fields update continuously (in which
    // case they'd be a better gap source than the reconstructed lap trace).
    bool logSdkGapFields = false;
    // Diagnostic: in races, every 10 s, log each shown row's gap next to
    // the SDK figures it came from. LogRaceGaps=1 in the INI.
    bool logRaceGaps = false;

    // Whole-row background highlight when a car (any row, including the
    // player's own) just changed track position -- faint green for a gain,
    // faint red for a loss, fading out over positionChangeHoldSeconds.
    float positionGainR = 0.15f, positionGainG = 1.0f, positionGainB = 0.15f;
    float positionGainA = 0.35f;
    float positionLossR = 1.0f, positionLossG = 0.15f, positionLossB = 0.15f;
    float positionLossA = 0.35f;
    float positionChangeHoldSeconds = 15.0f;
    // A raw place change only counts once the new place has held steady for
    // this long -- otherwise the back-and-forth jostling typical of the
    // first lap/first corner would flash the highlight constantly.
    float positionChangeDebounceSeconds = 1.0f;

    // Dark grey, semi-transparent panel, same style as the other HUD boxes.
    bool bgEnabled = true;
    float bgPaddingPx = 24.0f;
    float bgCornerRadiusPx = 16.0f;
    float bgR = 0.08f, bgG = 0.08f, bgB = 0.08f, bgA = 0.6f;
    // Tyre compound column, using the F1 colour code: S red, M yellow,
    // H white, I green, W blue. An unrecognised mod compound shows its
    // first letter in grey.
    //
    // Compound is telemetry-only and scoring carries no tyre data at all, so
    // turning this on also widens telemetry to the whole field (see
    // WantsTelemetryUpdates in Plugin.cpp) -- there is no cheaper way to
    // know what the rest of the field is running.
    bool showTyre = true;
    // Laps since each car's last pit stop ("L12"), see PitStint.h.
    bool showPitLaps = false;
    // Car number ("#19", read from the entry name) between place and name.
    bool showCarNumber = true;
    // Gap box: a small second table of the two places ahead of you and the
    // one behind, with the same columns as the standings. Its own position
    // and size; -1 = auto, just under the standings (resolved on first draw).
    bool showGapBox = true;
    float gapPosXFraction = -1.0f;
    float gapPosYFraction = -1.0f;
    float gapScale = 1.0f;

};

struct FuelTyrePanelConfig
{
    bool enabled = true;

    // Fuel, Tyres and Brake Bias are three INDEPENDENTLY placed/sized boxes
    // (each its own Layout::Register entry), not one stacked panel -- so
    // each gets its own enable/position/scale. -1 on a position fraction is
    // the same "auto-anchor below the previous box" sentinel used elsewhere
    // in this codebase (the shortcut list, the DRS/PB rows): it resolves
    // once, to directly under the box above it, on the FIRST DRAG, and is
    // fixed from then on -- so the default look is the original single
    // stacked panel until a box is actually moved.
    bool showFuel = true;
    float fuelPosXFraction = 0.0f;
    float fuelPosYFraction = 0.0f;
    float fuelScale = 1.0f;

    bool showTyres = true;
    float tyresPosXFraction = -1.0f;
    float tyresPosYFraction = -1.0f;
    float tyresScale = 1.0f;

    bool showBrakeBias = true;
    float brakeBiasPosXFraction = -1.0f;
    float brakeBiasPosYFraction = -1.0f;
    float brakeBiasScale = 1.0f;

    float textHeightPx = 44.0f;
    float lineGapPx = 16.0f;
    float columnGapPx = 80.0f; // between the FL/RL and FR/RR columns in the tyre grid

    float colorR = 1.0f, colorG = 1.0f, colorB = 1.0f;    // normal (white) text -- everything defaults to this
    float alertR = 1.0f, alertG = 0.15f, alertB = 0.15f;  // a tyre's sudden-degradation-spike red

    // Highlights the "Tyres Left" row's BACKGROUND based on the worst tyre's
    // PROJECTED remaining % at the end of the race (extrapolated from its
    // current wear rate and the race's remaining laps), race sessions only:
    // orange between 20% and 40%, red below 20%, both semi-transparent so
    // the text underneath stays readable.
    float tyreLowVsRaceR = 1.0f, tyreLowVsRaceG = 0.55f, tyreLowVsRaceB = 0.0f;    // orange, 20-40%
    float tyreLowVsRaceA = 0.5f;
    float tyreCriticalVsRaceR = 1.0f, tyreCriticalVsRaceG = 0.15f, tyreCriticalVsRaceB = 0.15f; // red, below 20%
    float tyreCriticalVsRaceA = 0.5f;

    // Vertical gap used when auto-anchoring one box directly under another
    // (see the PosXFraction/PosYFraction sentinel comment above) -- each box
    // now paints its own background, so there is no longer a shared panel to
    // put a rule inside.
    float groupGapPx = 20.0f;

    // After crossing the line, HOLD the tyre wear numbers as they stood for
    // the lap just finished, for this many seconds, instead of letting them
    // reset toward 0 for the new lap immediately -- the one moment that
    // lap's real number is worth reading is also the moment it would
    // otherwise vanish. 0 disables. Same idea as LapDelta's
    // FreezeAfterLineSeconds.
    float tyreFreezeAfterLineSeconds = 5.0f;


    // Fuel-versus-race verdict, applied to the "Fuel Time Left" and "Fuel
    // Laps Left" values in RACE sessions. Green means the tank comfortably
    // outlasts what the race still needs; orange (with the value tagged
    // "marginal") means it only just does; red means it does not. The two
    // margins below are what "only just" means -- minutes for a timed race,
    // laps for a lap-limited one.
    float fuelMarginMinutes = 2.0f;
    float fuelMarginLaps = 1.0f;
    float fuelGoodR = 0.25f, fuelGoodG = 0.9f, fuelGoodB = 0.3f;
    float fuelMarginalR = 1.0f, fuelMarginalG = 0.6f, fuelMarginalB = 0.1f;
    float fuelShortR = 1.0f, fuelShortG = 0.2f, fuelShortB = 0.2f;

    bool bgEnabled = true;
    float bgPaddingPx = 24.0f;
    float bgCornerRadiusPx = 16.0f;
    float bgR = 0.08f, bgG = 0.08f, bgB = 0.08f, bgA = 0.6f;
};

// The drag-to-position layout mode's own appearance. Only ever drawn while
// that mode is active, so none of this costs anything in normal use.
struct LayoutConfig
{
    // Alignment grid. Minor lines are faint by design: at 4K a 100px spacing
    // is already ~38 columns, and anything more prominent reads as a grey
    // wash rather than as a grid. The major line every few minors is what
    // the eye counts by, and the screen-centre pair is brightest because
    // "centred" is by far the most commonly wanted alignment.
    bool gridEnabled = true;
    float gridMinorPx = 100.0f;
    float gridMajorPx = 500.0f;
    float gridR = 1.0f, gridG = 1.0f, gridB = 1.0f;
    float gridMinorAlpha = 0.10f;
    float gridMajorAlpha = 0.28f;
    float gridCenterAlpha = 0.55f;

    // The mode banner's own position. Draggable like anything else -- it
    // covers the top of the screen by default, which is exactly where some
    // panels want to live. Only ever visible inside layout mode, so it can
    // only be moved from within.
    float bannerPosXFraction = 0.5f;
    float bannerPosYFraction = 0.12f;
};

struct BrakeCueConfig
{
    bool enabled = true;

    float brakeThreshold = 0.2f;       // brake input above this counts as "braking" -- lowered from 0.8 so the recorded point is where braking BEGINS rather than where it peaks, which also brings it close to reactionThreshold (0.1) so the two now mark nearly the same moment in one application
    float minPointSpacingM = 15.0f;    // ignore a new point this close to the last one (pumped brakes, same zone)
    // Only beep for big stops: a point whose speed drop on the reference lap
    // (braking start down to the slowest speed before the next point) is
    // below this stays silent. Slider in the shortcut list.
    float minSpeedDropKmh = 50.0f;
    static constexpr float kMinSpeedDropMin = 0.0f;
    static constexpr float kMinSpeedDropMax = 150.0f;
    float leadTimeSec = 0.3f;          // the beep fires this long before the reference point (the seed instead, if adaptiveLeadTimeEnabled)

    int beepFrequencyHz = 880;         // A5 -- clear without being harsh
    int beepDurationMs = 120;

    // --- Reaction-time measurement and adaptive lead time ---

    // Brake input above this counts as "the pedal has now moved", for timing
    // the reaction to a beep. Deliberately far lower than brakeThreshold
    // above (which only marks a genuinely committed braking point worth
    // recording) -- this just wants the first instant of pedal movement.
    float reactionThreshold = 0.10f;

    // A beep with no pedal movement within this long is abandoned rather
    // than matched against whatever braking eventually follows -- otherwise
    // missing a corner entirely (off track, a spin, simply ignoring the cue)
    // would record a multi-second "reaction".
    float reactionTimeoutSeconds = 3.0f;

    // Once a reaction has been measured, the beep fires the driver's OWN
    // average reaction time before the reference point instead of
    // leadTimeSec -- so the pedal actually moves AT the reference lap's
    // brake point. Reacting slowly pushes the beep earlier, reacting quickly
    // pulls it later.
    //
    // OFF by default: it cannot tell a slow reaction from braking later than
    // the reference ON PURPOSE. Braking later and later as you find time
    // reads as an ever slower "reaction", so the beep moves earlier, the gap
    // grows again, and it runs away. A fixed
    // leadTimeSec does not drift; the reference points themselves still move
    // later whenever a faster lap is recorded.
    bool adaptiveLeadTimeEnabled = false;

    // Individual measurements outside this range are discarded rather than
    // dragging the average around -- below the floor is not a reaction to
    // anything (already braking, or noise), above the ceiling is a missed
    // cue rather than a slow one. A real reaction to a sound while driving
    // is ~0.15-0.6s, so the ceiling is already generous; it also doubles as
    // the bound on how early the cue can ever be moved (see BrakeCue.cpp's
    // LeadTimeSeconds).
    float reactionMinSeconds = 0.02f;
    float reactionMaxSeconds = 1.5f;

    // On-screen "Brake React" readout: average and most recent measurement.
    bool showReactionStats = true;
    float reactionStatsCenterXFraction = 0.5f;
    // Positioned below the pedals panel (PedalConfig::centerYFraction = 0.9516)
    // with padding, so it sits comfortably at the bottom without competing for space.
    float reactionStatsCenterYFraction = 0.982f;
    float reactionStatsTextHeightPx = 28.0f;
    // Size multiplier for the reaction-stats readout -- see the note on
    // RadarConfig::scale.
    float reactionStatsScale = 1.0f;
    float reactionStatsR = 1.0f, reactionStatsG = 1.0f, reactionStatsB = 1.0f;
    bool reactionStatsBgEnabled = true;
    float reactionStatsBgPaddingPx = 8.0f;
    float reactionStatsBgCornerRadiusPx = 8.0f;
    float reactionStatsBgR = 0.08f, reactionStatsBgG = 0.08f, reactionStatsBgB = 0.08f, reactionStatsBgA = 0.6f;
};

struct DrsCueConfig
{
    bool enabled = true;

    int beepFrequencyHz = 1200;
    int beepDurationMs = 90;   // each of the two tones
    int gapMs = 80;            // silence between them
};

// Per-corner tyre temperature box: peak SURFACE temps and the inner-layer
// ("subsurface") temps at corner exit, for all four tyres, so camber can be
// judged from the inside-vs-outside spread. See TyreTemps.h.
struct TyreTempsConfig
{
    bool enabled = true;

    float posXFraction = 0.80f;
    float posYFraction = 0.62f;
    float scale = 1.0f;

    float textHeightPx = 28.0f;
    float labelHeightPx = 18.0f;
    float rowGapPx = 6.0f;
    float columnGapPx = 12.0f;

    // The SDK reports left/centre/right in the wheel's own frame, not
    // inside/outside. The default mapping (index 0 = outside on left-side
    // wheels, inside on right-side wheels) follows from rF2's local +x
    // pointing left, but is unverified against real data -- each corner is
    // logged with the raw values so it can be checked. Flip here if the
    // labels turn out to be mirrored.
    bool swapInsideOutside = false;

    // A corner starts once |lateral g| holds above CornerEnterG for
    // EnterHoldSeconds, and ends once it holds below CornerExitG for
    // ExitHoldSeconds; surface temperatures keep being tracked for
    // TailSeconds afterwards, since they keep rising slightly after the
    // tyre unloads. Anything shorter than MinCornerSeconds is ignored.
    float cornerEnterG = 0.5f;
    float cornerExitG = 0.3f;
    float enterHoldSeconds = 0.3f;
    float exitHoldSeconds = 0.3f;
    float tailSeconds = 0.7f;
    float minCornerSeconds = 0.6f;

    // Inside minus outside, in degrees C. Inside a little hotter than
    // outside is the usual camber target; the band below is a starting
    // guess, not a rule -- tune it to what works for the car.
    float spreadGoodMinC = 3.0f;
    float spreadGoodMaxC = 15.0f;

    // Bar chart: one height scale shared by all four tyres, refitted each
    // corner (hottest zone = full height), never stretched over less than BarMinRangeC, so a 1C
    // difference doesn't look dramatic). Colour is absolute: blue below
    // BarColdC, green in the window, red above BarHotC.
    float barMinRangeC = 10.0f;
    float barColdC = 70.0f;
    float barHotC = 100.0f;

    bool bgEnabled = true;
    float bgPaddingPx = 10.0f;
    float bgCornerRadiusPx = 8.0f;
    float bgR = 0.08f, bgG = 0.08f, bgB = 0.08f, bgA = 0.6f;
};

extern RadarConfig g_radarConfig;
extern TyreTempsConfig g_tyreTempsConfig;

// FFB frequency spectrum, on Moza Pit House's EQ points. See FfbGraph.h.
struct FfbGraphConfig
{
    bool enabled = true;
    float posXFraction = 0.50f;
    float posYFraction = 0.80f;
    float scale = 1.0f;
    float widthPx = 1100.0f;
    float heightPx = 320.0f;
    float paddingPx = 14.0f;
    float textHeightPx = 22.0f;
    float cornerRadiusPx = 8.0f;
    // Realtime: the line shows only the latest ~1 s. Off: a rolling average
    // over windowSeconds. The clipping % always covers windowSeconds.
    bool realtime = true;
    float windowSeconds = 10.0f;
    // |force| at or above this counts as clipped (rF2's range is -1..+1).
    float clipLevel = 0.995f;
    // The red "at the moment it clipped" curve fades out over this.
    float clipFadeSeconds = 2.0f;
    // Log-scale y axis range, percent of full force.
    float minPercent = 0.5f;
    float maxPercent = 200.0f;
    // Frequency bands per decade on the log axis (1-10 Hz, 10-100 Hz, ...):
    // each point is the energy of all 1 Hz bins in its band. 0 = one point
    // per 1 Hz bin.
    int bandsPerDecade = 5;
    float bgR = 0.13f, bgG = 0.13f, bgB = 0.13f, bgA = 0.85f;
    float lineR = 0.95f, lineG = 0.42f, lineB = 0.15f; // Pit House orange
    // Dotted line: average over the last completed lap (or the lap so far).
    bool showLapAverage = true;
    float lapLineR = 1.0f, lapLineG = 0.80f, lapLineB = 0.55f; // pale orange
};
extern FfbGraphConfig g_ffbGraphConfig;

// Overhead track map. The outline comes from this plugin's own recordings or
// TinyPedal's saved maps (see TrackMap.h), so the settings here are about how
// it is PRESENTED -- where it sits, how big, and which way round.
struct TrackMapConfig
{
    bool enabled = true;

    // Right-hand side, above centre -- clear of the radar and the dashboard.
    float posXFraction = 0.86f;
    float posYFraction = 0.30f;
    float scale = 1.0f;
    float sizePx = 460.0f;        // box side at 2160p, before scale

    // Fallbacks for a circuit never turned or flipped. Each circuit's own
    // angle and flip live in this same section as Rotation_<track> and
    // Mirror_<track>, set with the slider/button under the map in layout mode.
    float rotationDegrees = 0.0f;
    bool mirror = false;

    // Record the player's next clean lap when neither this plugin nor
    // TinyPedal has a map for the circuit.
    bool recordMissing = true;

    float lineWidthPx = 11.0f;     // track ribbon
    float trackR = 0.82f, trackG = 0.82f, trackB = 0.84f, trackA = 1.0f;
    float outlineWidthPx = 3.0f;   // dark edge each side of the ribbon
    float outlineR = 0.05f, outlineG = 0.05f, outlineB = 0.06f, outlineA = 1.0f;

    // Track colour by where you gained or lost time on the lap, vs your
    // personal best (1), your own session best (2) or the session's fastest
    // lap by anyone in your class (3); 0 = plain. The lap is cut into
    // deltaSectorCount equal mini-sectors (a slider in the options list),
    // each coloured by the time gained (green) or lost (red) over it, full
    // colour at deltaFullSeconds -- or purple when it matches or beats the
    // session's fastest time through that mini-sector by anyone in your
    // class (from every valid lap). The lap being driven colours as you go; the
    // stretch still ahead shows the last lap, fainter. Wheel buttons step
    // through the modes: deltaPrevButton back, deltaNextButton on, wrapping
    // through off (0 = no button).
    int   deltaColourMode = 0;   // 0 off, 1 PB, 2 SB, 3 session fastest
    int   deltaPrevButton = 30;
    int   deltaNextButton = 31;
    int   deltaSectorCount = 80;
    static constexpr int kMinDeltaSectors = 10;
    static constexpr int kMaxDeltaSectors = 200;
    float deltaFullSeconds = 0.05f;
    float gainR = 0.15f, gainG = 0.85f, gainB = 0.25f;
    float purpleR = 0.62f, purpleG = 0.22f, purpleB = 0.95f;
    float lossR = 0.95f, lossG = 0.18f, lossB = 0.12f;
    float deltaAheadStrength = 0.5f; // last lap's colour on the stretch ahead

    // Small arrow in the map's corner pointing to world +Z, which tracks
    // are normally built facing north. Turns with the map's rotation.
    bool  showNorth = true;

    bool  showSectors = true;
    float markWidthPx = 5.0f;
    float startR = 1.0f, startG = 0.27f, startB = 0.0f;
    float sectorR = 0.0f, sectorG = 0.67f, sectorB = 1.0f;

    float carRadiusPx = 11.0f;
    float carOutlineWidthPx = 2.5f;
    // Fallback only -- every car scoring has described gets its own colour
    // from DriverAbbrev's palette.
    float carR = 0.85f, carG = 0.85f, carB = 0.88f;
    // Your own car keeps ONE fixed colour, and a slightly bigger dot, so it
    // is findable without remembering which hue you were given.
    bool  playerOwnColor = true;
    float playerR = 0.15f, playerG = 0.85f, playerB = 0.25f;
    float playerRadiusScale = 1.25f;

    // Cars are placed by LAP DISTANCE along the recorded path, then nudged
    // sideways by mPathLateral so side-by-side cars do not stack. The sign
    // depends on the handedness of the recording -- hence a flip.
    bool  lateralEnabled = true;
    bool  lateralFlip = false;

    bool  showLabels = true;
    float labelHeightPx = 26.0f;
    // A label is dropped when its dot lands within this of one already
    // labelled, so a start-line pack does not become an unreadable stack.
    float labelMinSeparationPx = 34.0f;
    float labelR = 1.0f, labelG = 1.0f, labelB = 1.0f;

    bool  bgEnabled = true;
    float bgPaddingPx = 14.0f;
    float bgCornerRadiusPx = 12.0f;
    float bgR = 0.04f, bgG = 0.04f, bgB = 0.04f, bgA = 0.55f;

    // Where TinyPedal keeps its recorded maps -- another application's
    // install path, which this plugin cannot assume.
    char tinyPedalMapDir[260] = "C:\\Program Files (x86)\\TinyPedal\\trackmap\\";
};
extern TrackMapConfig g_trackMapConfig;

// Plain rounded boxes for blanking out parts of the car's own in-car display
// that compete with this overlay. Up to kMaxBoxes, each placed by its
// top-left corner and sized in width and height independently in layout
// mode (its corner handle). Drawn first, under every other panel. Shown or
// hidden together (Shortcuts::CoverBoxes); 'count' is set in the options
// list. Positions and sizes are part of the per-car layout (CarLayout.h).
struct CoverBoxConfig
{
    static constexpr int kMaxBoxes = 4;
    int count = 1;
    float posXFraction[kMaxBoxes] = { 0.45f, 0.40f, 0.35f, 0.30f };
    float posYFraction[kMaxBoxes] = { 0.45f, 0.40f, 0.35f, 0.30f };
    float widthPx[kMaxBoxes] = { 300.0f, 300.0f, 300.0f, 300.0f };   // at 2160p
    float heightPx[kMaxBoxes] = { 150.0f, 150.0f, 150.0f, 150.0f };
    // The boxes' shade, 0 = black .. 1 = white -- their own, separate from
    // every other panel's background. A slider in the options list.
    float grey = 0.04f;
    float cornerRadiusPx = 12.0f;
    float a = 1.0f; // opaque: it is there to hide things
};
extern CoverBoxConfig g_coverBoxConfig;
extern PedalConfig g_pedalConfig;
extern MiniSectorConfig g_miniSectorConfig;
extern LayoutConfig g_layoutConfig;
extern FrameLatencyConfig g_frameLatencyConfig;
extern GearDisplayConfig g_gearDisplayConfig;
extern PitLimiterConfig g_pitLimiterConfig;
extern TrackAlertConfig g_trackAlertConfig;
extern LapDeltaConfig g_lapDeltaConfig;
extern RelativeTimingConfig g_relativeTimingConfig;
// Fuel-save delta bar for stint-length fuel targets -- see FuelSave.h.
struct FuelSaveConfig
{
    bool enabled = true;
    float stintMinutes = 0.0f;      // target stint length; 0 = bar off
    float reserveLitres = 0.5f;     // left in the tank at the stint's end
    float barWindowLitres = 0.10f;  // delta that fills half the bar
    float centerXFraction = 0.5f;
    float centerYFraction = 0.22f;
    float scale = 1.0f;
};
extern FuelSaveConfig g_fuelSaveConfig;
// Tyre-wear target panel -- see TyreSave.h.
struct TyreSaveConfig
{
    bool enabled = true;
    float targetWearPercent = 70.0f;  // max wear (0 = fresh) to reach at the target time
    float targetMinutes = 100.0f;     // how long the set must last; 0 = panel off
    float centerXFraction = 0.85f;
    float centerYFraction = 0.30f;
    float scale = 1.0f;
};
extern TyreSaveConfig g_tyreSaveConfig;
// Water / oil temperature box -- see EngineTemps.h.
struct EngineTempsConfig
{
    bool enabled = true;
    float waterAmberMargin = 6.0f;  // C above the learned normal that turns it amber
    float oilAmberMargin = 8.0f;
    float centerXFraction = 0.85f;
    float centerYFraction = 0.45f;
    float scale = 1.0f;
};
extern EngineTempsConfig g_engineTempsConfig;
// FFB strength box -- see FfbLevel.h.
struct FfbLevelConfig
{
    bool enabled = true;
    float centerXFraction = 0.85f;
    float centerYFraction = 0.55f;
    float scale = 1.0f;
};
extern FfbLevelConfig g_ffbLevelConfig;
// Track conditions box -- see Conditions.h.
struct ConditionsConfig
{
    bool enabled = true;
    float wetsAtWetness = 0.20f;  // average racing-line wetness at which wets are called
    float nearMargin = 0.05f;     // amber "near crossover" band below that
    bool realWeather = false;     // also fetch the real-world forecast (Open-Meteo), see Weather.h
    float centerXFraction = 0.85f;
    float centerYFraction = 0.65f;
    float scale = 1.0f;
};
extern ConditionsConfig g_conditionsConfig;
extern FuelTyrePanelConfig g_fuelTyrePanelConfig;
extern BrakeCueConfig g_brakeCueConfig;
extern DrsCueConfig g_drsCueConfig;



namespace Config
{
    // Reads the INI next to the DLL, writing a default one first if it's
    // missing. Call once, from plugin Startup().
    void Load();

    // Queues one boolean key to be written back to the live INI -- used for
    // the persisted element visibility, so a toggle survives restarting the
    // game. Deliberately a single-key write (WritePrivateProfileString)
    // rather than re-serialising the whole file, so hand-written comments and
    // tuning edits in that INI are never touched. Does NOT touch disk here:
    // see FlushPendingWrites(). Returns false only if the write could not
    // even be queued (INI path unset) -- a genuine disk-write failure (a
    // UAC-protected install directory is a real possibility) can only be
    // known once FlushPendingWrites() actually runs.
    bool WriteBool(const char* section, const char* key, bool value);

    // Same single-key deferred write, for a position fraction -- used by the
    // drag-to-position layout mode and the keyboard nudges, so a panel moved
    // in-game stays where it was put. Written with four decimals, which is
    // finer than one screen pixel at any resolution anyone runs.
    bool WriteFloat(const char* section, const char* key, float value);

    // Same deferred single-key write, for a word (the text font's name).
    bool WriteString(const char* section, const char* key, const char* value);
    // Reads a word from the INI (empty if absent) -- or, if it was written
    // but not flushed yet, the value still waiting in the queue.
    void ReadStringLive(const char* section, const char* key, char* buf, size_t bufSize);

    // Actually performs every write queued by WriteBool/WriteFloat above
    // since the last call, in one batch, then clears the queue. This is the
    // ONLY place either of those touches disk, and it runs only on the
    // Persist thread, out of the car (the disk rule, Persist.cpp): a write
    // from the render or a game thread was a real, measured source of Plugin
    // CPU spikes while driving (WritePrivateProfileString is synchronous disk
    // I/O, and a toggle or a layout-mode drag can happen mid-session). The
    // queue itself is locked, so any thread may queue at any time. Returns
    // false if any single write failed, so the caller can surface ONE
    // warning for the whole batch rather than one per key.
    bool FlushPendingWrites();

    // Reads one float from the live INI, for keys whose NAME is not
    // known until runtime and so cannot be part of the normal Load(). The
    // track map's per-circuit rotation is the case this exists for: the key
    // is "Rotation_<TrackName>", and there is one per circuit ever driven.
    // A value queued but not flushed yet wins over the file. Hits disk, so
    // call it on a map load, not per frame.
    float ReadFloatLive(const char* section, const char* key, float def);

    // Renders a virtual-key code the way the INI writes it and the on-screen
    // list shows it: "R", "5", "F11". Anything unrecognised comes back as
    // "?" rather than an unprintable byte.
    void KeyName(int vk, char* out, size_t outSize);

    // Reads rFactor 2's OWN "Toggle Cursor" control straight out of
    // Controller.JSON -- a file this plugin does not own and does not write
    // to. Layout mode needs the mouse to be free to drag anything, and the
    // key for that lives entirely in the sim's own bindings, not this
    // plugin's INI, so the prompt has to go and look rather than assume F9.
    // Writes a short display string into 'out': a key name if bound on the
    // keyboard, or "not bound" if the control is unassigned, bound to a
    // device other than the keyboard, or the file could not be found/parsed
    // -- all three read the same to a driver ("I do not have a keyboard key
    // for this"), so they are not distinguished on screen.
    void ReadToggleCursorKeyName(char* out, size_t outSize);

    // Same lookup as ReadToggleCursorKeyName, but returns the raw DirectInput
    // scan code instead of a display string -- what's needed to actually
    // synthesize the keypress (layout mode auto-freeing the cursor) rather
    // than just show it in a toast. Returns false under the same conditions
    // ReadToggleCursorKeyName falls back to "not bound" (unassigned, bound to
    // a non-keyboard device, or the file missing/unparseable).
    bool ReadToggleCursorScanCode(int& outCode);
}
