// rF2 Internals Plugin entry point.
//
// Loaded by the game itself from Bin64\Plugins, and enabled/disabled in-game via
// Settings -> Gameplay -> Plugins. Because the game controls load timing, there
// is no loader-lock hazard here (unlike a dinput8.dll proxy), and car data
// arrives through the official UpdateScoring callback rather than shared memory.
//
// The one thing the plugin API cannot do is draw in DirectX 11 -- its render
// callbacks are DX9-era -- so the actual drawing still goes through a D3D11
// present hook installed at Startup.
#include "InternalsPlugin.hpp"
#include "Hook.h"
#include <atomic>
#include <vector>
#include <cmath>
#include "Log.h"
#include "CarState.h"
#include "PedalTrace.h"
#include "BrakeCue.h"
#include "Config.h"
#include "FrameLatency.h"
#include "GearDisplay.h"
#include "LapDelta.h"
#include "RelativeTiming.h"
#include "FuelStore.h"
#include "TyreWear.h"
#include "SetupInfo.h"
#include "DrsCue.h"
#include "TrackAlerts.h"
#include "RealtimeState.h"
#include "PerfStats.h"
#include "InputLagEstimate.h"
#include "MiniSector.h"
#include "ProximityHue.h"
#include "DriverAbbrev.h"
#include "TyreStore.h"
#include "TyreTemps.h"
#include "AmdGpuPower.h"
#include "Shortcuts.h"
#include "UpdateCheck.h"
#include "GpuEngineUsage.h"
#include "FfbGraph.h"
#include "Profiler.h"
#include "TrackMap.h"
#include "EngineTemps.h"
#include "Benchmark.h"
#include "FfbLevel.h"
#include "Conditions.h"
#include "Weather.h"
#include "PitStint.h"
#include "FuelSave.h"
#include "TyreSave.h"
#include "Persist.h"
#include "Sound.h"
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <cstring>

// Set from the wheel options list ("Reset timing tower"); acted on in the
// scoring callback so the store is only ever touched from that thread.
std::atomic<bool> g_resetTimingRequested{ false };
#include "CarLayout.h"

// Latched from scoring, read on the telemetry thread. -1 until the first
// scoring tick names a player.
static std::atomic<long> g_playerId{ -1 };

// Who drives the player's car: see Persist.h for the meaning and how the
// learning stores use them (review F30, F167, F252, F253).
std::atomic<bool> g_localPlayerDriving{ false };
std::atomic<bool> g_localPlayerLapClean{ false };

// Review F263: the CRT's default invalid-parameter handler __fastfails the
// whole game on any secure-CRT overflow (sprintf_s into a short buffer...),
// past every __try. The DLL has its own static CRT, so this handler only
// covers the plugin's calls: the call then fails (empty text) instead.
// Counted here, logged from the save pass; nothing else is safe to do inside.
static std::atomic<long> s_crtFaults{ 0 };
static void __cdecl OnCrtBadParam(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
    s_crtFaults.fetch_add(1, std::memory_order_relaxed);
}
// Review F271: exceptions caught at the game-callback boundary (one tick
// skipped instead of an exception unwinding into rF2).
static std::atomic<long> s_cbFaults{ 0 };

// Riding along is only trusted for the car you drove yourself earlier in
// THIS session on this track (review F253): a stranger whose entry has the
// stock name of a car you drove last week is never taken for yours.
static bool s_droveThisSession = false;
static char s_droveTrack[64] = {};

// "My car" when rF2 marks no car as the player's: riding along while a
// team-mate drives the shared car. It is the car last driven (its entry name,
// saved as [Session] LastVehicle so it survives a restart). Without this,
// with no player car known, every car's telemetry reached the driving HUD
// and the gear, speed and pit limiter showed whichever car came last.
static char s_myVehicle[64] = {};
static bool s_myVehicleLoaded = false;
static std::vector<VehicleScoringInfoV01> s_viewVehicles;

// The scoring info as the HUD should see it: the original while driving;
// while riding along, a copy with "my car" flagged as the player.
static const ScoringInfoV01& PlayerView(const ScoringInfoV01& info, ScoringInfoV01& copy)
{
    if (!s_myVehicleLoaded)
    {
        Config::ReadStringLive("Session", "LastVehicle", s_myVehicle, sizeof(s_myVehicle));
        s_myVehicleLoaded = true;
    }
    if (!info.mVehicle) { g_playerId.store(-1); g_localPlayerDriving.store(false); return info; }
    for (int i = 0; i < info.mNumVehicles; ++i)
    {
        const VehicleScoringInfoV01& v = info.mVehicle[i];
        if (!v.mIsPlayer) continue;
        g_playerId.store(v.mID, std::memory_order_relaxed);
        g_playerAiControl.store(v.mControl == 1, std::memory_order_relaxed);
        // 0 = local player; 1 = AI (Esc mid-race, after the flag) (review F167).
        g_localPlayerDriving.store(v.mControl == 0, std::memory_order_relaxed);
        s_droveThisSession = true;
        strncpy_s(s_droveTrack, info.mTrackName, _TRUNCATE);
        if (strncmp(s_myVehicle, v.mVehicleName, sizeof(s_myVehicle) - 1) != 0)
        {
            strncpy_s(s_myVehicle, v.mVehicleName, _TRUNCATE);
            Config::WriteString("Session", "LastVehicle", s_myVehicle);
        }
        return info;
    }
    // Riding along: never "the local player driving", whatever the car does.
    g_localPlayerDriving.store(false, std::memory_order_relaxed);
    if (s_myVehicle[0] && s_droveThisSession
        && strncmp(s_droveTrack, info.mTrackName, sizeof(s_droveTrack) - 1) == 0)
    {
        for (int i = 0; i < info.mNumVehicles; ++i)
        {
            // Only a remote driver (2) in that car is a team-mate (review F253).
            if (info.mVehicle[i].mControl != 2) continue;
            if (strncmp(s_myVehicle, info.mVehicle[i].mVehicleName, sizeof(s_myVehicle) - 1) != 0) continue;
            s_viewVehicles.assign(info.mVehicle, info.mVehicle + info.mNumVehicles);
            s_viewVehicles[i].mIsPlayer = true;
            copy = info;
            copy.mVehicle = s_viewVehicles.data();
            g_playerId.store(info.mVehicle[i].mID, std::memory_order_relaxed);
            g_playerAiControl.store(false, std::memory_order_relaxed);   // a team-mate drives: their limiter is real
            return copy;
        }
    }
    g_playerId.store(-1, std::memory_order_relaxed);   // no car to call mine: telemetry ignored
    return info;
}

// The save pass, run only on the Persist thread (see the disk rule in
// Persist.cpp). Each FlushIfDirty is a cheap dirty check when nothing changed,
// and copies its data under its own lock before writing, so the game and the
// HUD keep running while this works.
static void PersistPass(bool all)
{
    // Learned driving data: at the line, and out of the car.
    g_gearDisplay.FlushIfDirty();
    g_fuelStore.FlushIfDirty();
    g_lapDelta.FlushIfDirty();
    TrackMap::FlushIfDirty();
    FuelSave::FlushIfDirty();
    TyreSave::FlushIfDirty();

    // Faults caught since the last pass (reviews F263, F271).
    static long s_crtLogged = 0, s_cbLogged = 0;
    const long crt = s_crtFaults.load(), cb = s_cbFaults.load();
    if (crt != s_crtLogged || cb != s_cbLogged)
    {
        Log::Write("Plugin: caught %ld CRT invalid-parameter fault(s), %ld callback exception(s) so far",
                   crt, cb);
        s_crtLogged = crt;
        s_cbLogged = cb;
    }

    if (!all)
    {
        // At the line too (on this thread, allowed by the disk rule), so a
        // crash mid-race no longer loses the whole stint's log (review F174).
        Log::FlushBuffered();
        return;
    }

    // Out of the car only: settings, per-car layouts, and the log -- last, so
    // anything this pass itself logged goes out with it.
    CarLayout::FlushIfDirty();
    g_gearDisplay.FlushSettingsIfDirty();
    if (!Config::FlushPendingWrites())
        Log::Write("Plugin: one or more queued INI writes failed (read-only? UAC?)");
    Log::FlushBuffered();
}

// Persist::RequestLoad: only the profile reads, no writes (review F275).
static void PersistLoadPass()
{
    FuelSave::FlushIfDirty(true);
    TyreSave::FlushIfDirty(true);
}

// The player's completed laps at the last scoring tick, to see the line
// being crossed (-1 = no player car).
static long s_playerLaps = -1;

class SkrPlugin : public InternalsPluginV07
{
public:
    SkrPlugin() {}
    ~SkrPlugin() override {}

    void Startup(long version) override
    {
        _set_invalid_parameter_handler(OnCrtBadParam);   // first: see OnCrtBadParam (review F263)
        Log::Init();
        Log::Write("SkrPlugin::Startup (game version %ld)", version);
        VehicleKey::Init();
        Config::Load();
        // Here rather than on the first scoring tick: an INI read is disk
        // access, and the scoring thread is a game callback.
        Config::ReadStringLive("Session", "LastVehicle", s_myVehicle, sizeof(s_myVehicle));
        s_myVehicleLoaded = true;
        g_carState.Init();
        g_pedalTrace.Init();
        g_brakeCue.Init();
        FrameLatency::Init();
        g_gearDisplay.Init();
        g_lapDelta.Init();
        g_relativeTiming.Init();
        g_fuelStore.Init();
        g_tyreWear.Init();
        g_setupInfo.Init();
        g_drsCue.Init();
        g_trackAlerts.Init();
        g_miniSector.Init();
        g_tyreTemps.Init();
        AmdGpuPower::Init(); // no-op, quietly, on non-AMD hardware
        GpuEngineUsage::Init();
        PerfStats::StartPoller();
        g_ffbSpectrum.Init();
        // After every store's Init: the first pass (for the lines logged
        // above) must not find a store whose lock does not exist yet.
        Persist::Start(PersistPass, PersistLoadPass);
        // Its own thread; after Config::Load (it reads [Updates]).
        UpdateCheck::Start();

        // No D3D hook here: installing it makes a probe device and swapchain,
        // and at startup the game may still be switching into full screen --
        // with NVIDIA Surround (all monitors as one display) a user's game
        // hung on a black screen there. Hooked at the first StartSession
        // instead, when the game is up; nothing is drawn in the main menus
        // before that.
        Log::Write("SkrPlugin::Startup: D3D11 hook waits for the first session");
    }

    void Shutdown() override
    {
        Log::Write("SkrPlugin::Shutdown");
        // Last chance -- EndSession's own flush covers a normal session end,
        // but not the game closing (or the plugin being re-ticked off)
        // without one, e.g. mid-session. Done first, before anything below
        // can go wrong -- including the weather thread's untimed join,
        // which could hold the save back for 15 s or more (review F170).
        Persist::FlushAllNow();
        Benchmark::Shutdown();
        Weather::Shutdown();
        Hook::Uninstall();
        // File-load workers still running must finish before the DLL can go.
        CarLayout::Shutdown();
        TrackMap::Shutdown();
        Shortcuts::Shutdown();
        VehicleKey::Shutdown();   // REST car-lookup worker
        PerfStats::StopPoller();
        UpdateCheck::Shutdown();
        AmdGpuPower::Shutdown();
        GpuEngineUsage::Shutdown();
        Sound::Shutdown();
        // Last of all: its final pass writes whatever was logged above.
        Persist::Stop();
    }

    void StartSession() override
    {
        Log::Write("SkrPlugin::StartSession");
        Shortcuts::ResetHint();
        // Again here, not only at EndSession: scoring that arrives between the
        // two can leave the timing table frozen on the old session.
        g_relativeTiming.Clear();
        // The D3D hook goes in here (not at Startup, see there): the game is
        // fully up by now. A no-op once installed.
        if (Hook::Install())
            Log::Write("SkrPlugin::StartSession: D3D11 hook installed");
        else
            Log::Write("SkrPlugin::StartSession: D3D11 hook could not be installed");
    }

    void EndSession() override
    {
        Log::Write("SkrPlugin::EndSession");
        g_inRealtime = false;
        // Last chance to write anything still dirty -- BEFORE the Clear()
        // calls below, which wipe the very vehicle/track keys these saves
        // need. Waits for a full pass on the Persist thread, the only place
        // that writes (see Persist.cpp).
        Persist::FlushAllNow();
        s_droveThisSession = false;   // the ride-along fallback needs a car driven in the next session (F253)
        g_localPlayerDriving = false;
        g_localPlayerLapClean = false;
        g_carState.Clear();
        g_pedalTrace.Clear();
        g_brakeCue.Clear();
        g_gearDisplay.Clear();
        g_lapDelta.Clear();
        g_relativeTiming.Clear();
        g_fuelStore.Clear();
        g_tyreWear.Clear();
        EngineTemps::Reset();
        Benchmark::Reset();
        Conditions::Reset();
        Weather::Reset();
        FfbLevel::Reset();         // the clip meter starts fresh each session (review F143)
        TyreSave::RequestReset();  // the new session's tyre set clock (review F353)
        g_setupInfo.Clear();
        g_drsCue.Clear();
        g_trackAlerts.Clear();
        g_miniSector.Clear();
        g_tyreTemps.Clear();
        g_ffbSpectrum.Clear();
        TrackMap::Clear();
        DriverAbbrev::Clear();
        TyreStore::Clear();
    }

    // Called by the game occasionally with the session's physics options --
    // read-only use here: only the fuel multiplier is recorded, nothing is
    // modified, so the game's own options pass through untouched.
    void SetPhysicsOptions(PhysicsOptionsV01& options) override
    {
        const int mult = static_cast<int>(options.mFuelMult);
        if (g_fuelMult.load() != mult)
        {
            Log::Write("SkrPlugin: fuel multiplier is now %dx", mult);
            g_fuelMult = mult;
        }
        g_autoShift.store(options.mAutoShift, std::memory_order_relaxed);
    }

    void EnterRealtime() override
    {
        // Actually driving now, as opposed to watching the session monitor --
        // that's when the driving HUD (radar, pedal trace, gear/speed) should
        // start drawing.
        Log::Write("SkrPlugin::EnterRealtime");
        g_setupInfo.ResetBaseline();
        Sound::Prepare();   // the game's audio is up by now (see Sound.h)
        g_inRealtime = true;
    }

    void ExitRealtime() override
    {
        // Leaving the car -- stop drawing stale positions.
        Log::Write("SkrPlugin::ExitRealtime");
        g_inRealtime = false;
        g_carState.Clear();
        g_pedalTrace.Clear();
        // NOT a full Clear() for the brake cue: dropping to the monitor is
        // still the same session, and its reference is meant to be the
        // braking points from this session's BEST lap. A full clear here
        // reset the best-lap time too, so the next lap completed after
        // coming back -- however slow -- became the new reference.
        g_brakeCue.OnExitRealtime();
        // Out of the car: everything may be written now (Persist.cpp).
        Persist::RequestAll();
    }

    // Scoring: positions of all cars, ~5Hz (radar).
    bool WantsScoringUpdates() override { return true; }

    // Exceptions stop here: one skipped tick, never an exception unwinding
    // into rF2 (review F271).
    void UpdateScoring(const ScoringInfoV01& info) override
    {
        try { ScoringTick(info); }
        catch (...) { s_cbFaults.fetch_add(1, std::memory_order_relaxed); }
    }

    void ScoringTick(const ScoringInfoV01& info)
    {
        // Authoritative, self-correcting source of truth for the driving
        // HUD's visibility -- the EnterRealtime/ExitRealtime/StartSession/
        // EndSession callback quartet above is NOT reliably paired across
        // every session-restart flow (observed: EndSession fires, but the
        // matching EnterRealtime never arrives for the restarted session,
        // leaving g_inRealtime stuck false -- and so the whole driving HUD
        // hidden -- even though the player is back on track). The SDK's own
        // mInRealtime field is checked every scoring tick (~5Hz) regardless,
        // so any such mismatch self-heals within one tick instead of
        // requiring a plugin reload to recover.
        g_inRealtime = info.mInRealtime;

        // My car, also while riding along (see PlayerView). Stores given the
        // original see no player car while riding along. Who is actually
        // driving is published in g_localPlayerDriving / g_localPlayerLapClean
        // (Persist.h): the learning stores must check those, since the AI or
        // a team-mate can drive a car still flagged as ours (review F167).
        ScoringInfoV01 viewCopy;
        const ScoringInfoV01& view = PlayerView(info, viewCopy);
        if (!g_localPlayerDriving.load(std::memory_order_relaxed))
            g_localPlayerLapClean.store(false, std::memory_order_relaxed);

        // Each call timed for the DLL-cost panel (Profiler.h).
        { PROF("Radar"); g_carState.Update(view); }
        { PROF("Brake cue"); g_brakeCue.OnScoring(info); }
        { PROF("Lap delta"); g_lapDelta.OnScoring(info); }
        { PROF("Conditions"); Conditions::OnScoring(view); }
        { PROF("Weather"); Weather::OnScoring(info); }
        { PROF("Pit stints"); PitStint::OnScoring(info); }
        // The view, so the position keeps moving while riding along (review F54).
        if (g_resetTimingRequested.exchange(false))
        {
            g_relativeTiming.Clear();
            Log::Write("RelativeTiming: reset from the options list");
        }
        { PROF("Relative"); g_relativeTiming.OnScoring(view); }
        { PROF("Fuel"); g_fuelStore.OnScoring(view); }
        { PROF("Track alerts"); g_trackAlerts.OnScoring(view); } // after LapDelta: reads its PB trace
        { PROF("Mini-sectors"); g_miniSector.OnScoring(info); }
        { PROF("Relative"); DriverAbbrev::OnScoring(view); }
        { PROF("Track map"); TrackMap::OnScoring(view); }
        { PROF("Benchmark"); Benchmark::OnScoring(view); }
        { PROF("Car layout"); CarLayout::OnScoring(view); }
        { PROF("Tyre wear"); g_tyreWear.OnScoring(view); }
        // After both stores: the tyre set clock runs even with its panel hidden (review F25).
        { PROF("Tyre target"); TyreSave::Update(); }

        // The two moments anything is written (the disk rule, Persist.cpp).
        // Both only wake the Persist thread; no disk access on this thread.
        //
        // The player crossing the line: their completed-lap count going up
        // while driving. Learned driving data (a new best lap, braking
        // points, fuel rate...) is saved then, a lap at a time, rather than
        // only once out of the car.
        long playerLaps = -1;
        if (view.mVehicle)
            for (long i = 0; i < view.mNumVehicles; ++i)
                if (view.mVehicle[i].mIsPlayer) { playerLaps = view.mVehicle[i].mTotalLaps; break; }
        if (g_inRealtime && s_playerLaps >= 0 && playerLaps > s_playerLaps)
            Persist::RequestLearned();
        // A new lap starts (or no player car): re-arm the "whole lap driven
        // by you" latch, after every learner above has seen the old value.
        if (playerLaps != s_playerLaps)
            g_localPlayerLapClean.store(g_localPlayerDriving.load(std::memory_order_relaxed),
                                        std::memory_order_relaxed);
        s_playerLaps = playerLaps;

        // Out of the car (monitor, garage, between stints): everything,
        // including the INI, the per-car layouts and the log. Asked every
        // tick here; the Persist thread runs at most about one pass a second.
        if (!g_inRealtime)
            Persist::RequestAll();
    }

    // Telemetry scope: 1 = player car only, 2 = every vehicle.
    //
    // Player-only is what the driving HUD needs (pedal trace, brake cue,
    // gear learning) and is far cheaper. The scope is widened to every car
    // when the radar is on (each car's position, heading and speed at
    // telemetry rate instead of ~5 Hz scoring), or when the timing table's
    // tyre column is on, because compound lives in TelemInfoV01 and scoring
    // carries no tyre data at all.
    //
    // The game re-asks this constantly rather than once at startup, so it
    // genuinely can be changed at runtime.
    long WantsTelemetryUpdates() override
    {
        // Without the radar, the tyre column (standings table and gap box)
        // is the only user of other cars' telemetry.
        const RelativeTimingConfig& rt = g_relativeTimingConfig;
        if (g_radarConfig.enabled)
            return 2;   // radar/side bars: every car's position at telemetry rate
        return ((rt.enabled || rt.showGapBox) && rt.showTyre) ? 2 : 1;
    }

    void UpdateTelemetry(const TelemInfoV01& info) override
    {
        try { TelemetryTick(info); }
        catch (...) { s_cbFaults.fetch_add(1, std::memory_order_relaxed); }   // review F271
    }

    void TelemetryTick(const TelemInfoV01& info)
    {
        // Every car's telemetry, when the scope is wide. Fed first and
        // deliberately ABOVE the player filter below -- the whole point of
        // this store is the rest of the field's compounds.
        { PROF("Relative"); TyreStore::OnTelemetry(info); }
        { PROF("Radar"); g_carState.OnTelemetry(info); }   // every car, by mID

        // CRITICAL: with the scope widened to all vehicles, EVERY car's
        // telemetry arrives here. Everything below assumes player-only data
        // -- the gear display learns shift points from it, the brake cue and
        // fuel store record against it -- so feeding it twenty cars
        // interleaved would corrupt learned state, and do so silently.
        const long playerId = g_playerId.load(std::memory_order_relaxed);
        if (playerId < 0 || info.mID != playerId)
            return;

        g_headlightsOn.store(info.mHeadlights ? 1 : 0, std::memory_order_relaxed);
        { PROF("Pedal trace"); g_pedalTrace.Update(info); }
        {
            PROF("FFB graph");
            g_ffbSpectrum.OnTelemetry(info.mLapNumber, static_cast<float>(sqrt(
                info.mLocalVel.x * info.mLocalVel.x + info.mLocalVel.y * info.mLocalVel.y + info.mLocalVel.z * info.mLocalVel.z)));
        }
        { PROF("Brake cue"); g_brakeCue.OnTelemetry(info); }
        { PROF("Gear + speed"); g_gearDisplay.Update(info); }
        { PROF("Fuel"); g_fuelStore.OnTelemetry(info); }
        { PROF("Tyre wear"); g_tyreWear.OnTelemetry(info); }
        { PROF("Engine temps"); EngineTemps::OnTelemetry(info); }
        { PROF("Benchmark"); Benchmark::OnTelemetry(info); }
        { PROF("Brake bias"); g_setupInfo.OnTelemetry(info); }
        { PROF("DRS"); g_drsCue.OnTelemetry(info); }
        { PROF("Input lag"); InputLagEstimate::OnTelemetry(info); }
        { PROF("Mini-sectors"); g_miniSector.OnTelemetry(info); }
        { PROF("Tyre temps"); g_tyreTemps.OnTelemetry(info); }
    }

    // Read-only: records the game's force value for the FFB spectrum and
    // returns false. rF2 ignores a plugin's edited value anyway.
    bool ForceFeedback(double& forceValue) override
    {
        try
        {
            PROF("FFB graph");
            g_ffbSpectrum.OnForce(forceValue);
            FfbLevel::OnForce(forceValue);
        }
        catch (...) { s_cbFaults.fetch_add(1, std::memory_order_relaxed); }   // review F271
        return false;
    }

    void Error(const char* const msg) override
    {
        Log::Write("SkrPlugin::Error from game: %s", msg ? msg : "(null)");
    }
};

//
// Plugin exports required by rFactor2.
//

extern "C" __declspec(dllexport) const char* __cdecl GetPluginName()
{
    return "rF2SkrOverlayPublic - in-frame driving HUD";
}

extern "C" __declspec(dllexport) PluginObjectType __cdecl GetPluginType()
{
    return PO_INTERNALS;
}

extern "C" __declspec(dllexport) int __cdecl GetPluginVersion()
{
    return 7; // matches InternalsPluginV07
}

extern "C" __declspec(dllexport) PluginObject* __cdecl CreatePluginObject()
{
    return static_cast<PluginObject*>(new SkrPlugin());
}

extern "C" __declspec(dllexport) void __cdecl DestroyPluginObject(PluginObject* obj)
{
    delete static_cast<SkrPlugin*>(obj);
}
