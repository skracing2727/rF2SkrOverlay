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

// PhysicsOptionsV01::mAutoShift (0 off, 1 upshifts, 2 downshifts, 3 all),
// from SetPhysicsOptions; -1 = not told yet. Shift learning skips the
// gearbox's own upshifts (review F30).
extern std::atomic<int> g_autoShift;

// The player's CAR identity, captured from the scoring thread and readable
// from the telemetry-driven widgets too. Every learned-data store keys its
// save file on it (PB/lap delta, brake points, fuel stats and fuel/tyre save
// profiles, pit limit, shift targets, speed reference).
//
// History: stores first keyed on mVehicleName (the per-ENTRY name, which
// mods set per number or driver), then on mVehicleClass -- which shared one
// file between different cars of a multi-make class such as GT3 (review
// F261/F169). Since 2026-11-24 the key is the car MODEL: the vehicle folder
// of the player's .VEH file, asked from rF2's own web interface on
// localhost (/rest/race/car, the list TinyPedal's brand import uses) on a
// background thread at each car change. When that is unavailable it falls
// back to the entry name. The class at the same moment is kept too, so a
// store with no file for the car yet can seed from its old class file.
namespace VehicleKey
{
    // Call once, single-threaded, before Startup() returns -- i.e. before
    // the game can possibly fire the scoring/telemetry callbacks below on
    // their own (and possibly different) threads. Creates the lock those
    // callbacks share; see RealtimeState.cpp for why this can't just happen
    // lazily on first use from whichever of them gets there first.
    void Init();
    // Plugin Shutdown: stops the REST worker thread.
    void Shutdown();
    // Scoring thread. Starts the REST lookup on a car change; the new key is
    // published once it answers (or after ~3 s with the fallback), so the
    // stores rekey once per car change, not twice.
    void SetFromScoring(const char* vehicleClass, const char* vehicleName);
    // Any thread. The car key; copies into 'out'; never empty.
    void Get(char* out, size_t outSize);
    // Any thread. The vehicle CLASS published together with Get()'s car --
    // the old (pre-2026-11-24) key, used only to seed a car's first file.
    void GetClass(char* out, size_t outSize);
}
