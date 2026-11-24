#pragma once
#include <atomic>

// True only while the player is actually driving (between EnterRealtime and
// ExitRealtime) -- as opposed to watching the session monitor, spectating, or
// sitting in a menu, all of which still fire UpdateTelemetry/UpdateScoring
// with stale or placeholder data. Written from the plugin's game-callback
// thread, read from the render thread, so it's atomic rather than protected
// by one of the module locks.
extern std::atomic<bool> g_inRealtime;

// The session's fuel-usage multiplier (0-7x), as reported by the game's
// SetPhysicsOptions callback. -1 until the game has told us. A learned fuel
// rate is only meaningful together with this: the same car burns 6x the
// litres per lap in a 6x-fuel session, so a saved rate must be stored (and
// re-applied) normalised to 1x.
extern std::atomic<int> g_fuelMult;

// The player's headlights from telemetry: 1 on, 0 off, -1 not known yet.
// For the "Headlights" rotary setting.
extern std::atomic<int> g_headlightsOn;

// The player's own car is being driven by the AI (scoring mControl == 1, e.g.
// "Vehicle is now controlled by AI"). rF2 reports the speed limiter as on
// then, even flat out, so the PIT warning ignores the limiter while this is set.
extern std::atomic<bool> g_playerAiControl;

// The player's vehicle CLASS ("MX-5 Cup", "F2", ...), captured once per
// scoring tick and readable from the telemetry-driven widgets too.
//
// This exists because every persisted store used to key its save file on
// mVehicleName, which is the per-ENTRY name -- mods set it per car number or
// even per driver, so a PB, a learned fuel rate and a learned shift point all
// fragmented across liveries and numbers of the same car. mVehicleClass is
// the model/class field and is the right key; it only lives on
// VehicleScoringInfoV01, so the telemetry-side widgets read it from here
// rather than from their own callback.
//
// Falls back to the entry name when the class is blank (some mods leave it
// empty) so a key is never empty. Known trade-off: for a multi-make class
// like GT3 this shares one file between genuinely different cars -- fine for
// the spec series this is actually used with.
namespace VehicleKey
{
    // Call once, single-threaded, before Startup() returns -- i.e. before
    // the game can possibly fire the scoring/telemetry callbacks below on
    // their own (and possibly different) threads. Creates the lock those
    // callbacks share; see RealtimeState.cpp for why this can't just happen
    // lazily on first use from whichever of them gets there first.
    void Init();
    // Scoring thread.
    void SetFromScoring(const char* vehicleClass, const char* vehicleName);
    // Any thread. Copies into 'out'; never empty.
    void Get(char* out, size_t outSize);
}
