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
#include "Config.h"
#include "FrameLatency.h"
#include "GearDisplay.h"
#include "LapDelta.h"
#include "RelativeTiming.h"
#include "FuelStore.h"
#include "TyreWear.h"
#include "SetupInfo.h"
#include "DrsCue.h"
#include "BrakeCue.h"
#include "TrackAlerts.h"
#include "RealtimeState.h"
#include "PerfStats.h"
#include "UpdateCheck.h"
#include "InputLagEstimate.h"
#include "MiniSector.h"
#include "ProximityHue.h"
#include "DriverAbbrev.h"
#include "TyreStore.h"
#include "TyreTemps.h"
#include "AmdGpuPower.h"
#include "Shortcuts.h"
#include "GpuEngineUsage.h"
#include "FfbGraph.h"
#include "Profiler.h"
#include "TrackMap.h"
#include "EngineTemps.h"
#include "FfbLevel.h"
#include "Conditions.h"
#include "Weather.h"
#include "PitStint.h"
#include "FuelSave.h"
#include "TyreSave.h"
#include "Persist.h"
#include "Sound.h"
#include <atomic>

// Set from the options list ("Reset timing tower"); acted on in the
// scoring callback so the store is only ever touched from that thread.
std::atomic<bool> g_resetTimingRequested{ false };
#include "CarLayout.h"

// Latched from scoring, read on the telemetry thread. -1 until the first
// scoring tick names a player.
static std::atomic<long> g_playerId{ -1 };

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
    if (!info.mVehicle) { g_playerId.store(-1); return info; }
    for (int i = 0; i < info.mNumVehicles; ++i)
    {
        const VehicleScoringInfoV01& v = info.mVehicle[i];
        if (!v.mIsPlayer) continue;
        g_playerId.store(v.mID, std::memory_order_relaxed);
        g_playerAiControl.store(v.mControl == 1, std::memory_order_relaxed);
        if (strncmp(s_myVehicle, v.mVehicleName, sizeof(s_myVehicle) - 1) != 0)
        {
            strncpy_s(s_myVehicle, v.mVehicleName, _TRUNCATE);
            Config::WriteString("Session", "LastVehicle", s_myVehicle);
        }
        return info;
    }
    if (s_myVehicle[0])
    {
        for (int i = 0; i < info.mNumVehicles; ++i)
        {
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
    g_brakeCue.FlushIfDirty();
    TrackMap::FlushIfDirty();
    FuelSave::FlushIfDirty();
    TyreSave::FlushIfDirty();
    if (!all)
        return;

    // Out of the car only: settings, per-car layouts, and the log -- last, so
    // anything this pass itself logged goes out with it.
    CarLayout::FlushIfDirty();
    g_gearDisplay.FlushSettingsIfDirty();
    if (!Config::FlushPendingWrites())
        Log::Write("Plugin: one or more queued INI writes failed (read-only? UAC?)");
    Log::FlushBuffered();
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
        FrameLatency::Init();
        g_gearDisplay.Init();
        g_lapDelta.Init();
        g_relativeTiming.Init();
        g_fuelStore.Init();
        g_tyreWear.Init();
        g_setupInfo.Init();
        g_drsCue.Init();
        g_brakeCue.Init();
        g_trackAlerts.Init();
        g_miniSector.Init();
        g_tyreTemps.Init();
        AmdGpuPower::Init(); // no-op, quietly, on non-AMD hardware
        GpuEngineUsage::Init();
        PerfStats::StartPoller();
        g_ffbSpectrum.Init();
        // After every store's Init: the first pass (for the lines logged
        // above) must not find a store whose lock does not exist yet.
        Persist::Start(PersistPass);
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
        Weather::Shutdown();
        Log::Write("SkrPlugin::Shutdown");
        // Last chance -- EndSession's own flush covers a normal session end,
        // but not the game closing (or the plugin being re-ticked off)
        // without one, e.g. mid-session. Done first, before anything below
        // can go wrong.
        Persist::FlushAllNow();
        Hook::Uninstall();
        // File-load workers still running must finish before the DLL can go.
        CarLayout::Shutdown();
        TrackMap::Shutdown();
        Shortcuts::Shutdown();
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
        g_carState.Clear();
        g_pedalTrace.Clear();
        g_gearDisplay.Clear();
        g_lapDelta.Clear();
        g_brakeCue.Clear();
        g_relativeTiming.Clear();
        g_fuelStore.Clear();
        g_tyreWear.Clear();
        EngineTemps::Reset();
        Conditions::Reset();
        Weather::Reset();
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
        // Not a full Clear(): dropping to the monitor is still the same
        // session, and the reference stays this session's best lap.
        g_brakeCue.OnExitRealtime();
        // Out of the car: everything may be written now (Persist.cpp).
        Persist::RequestAll();
    }

    // Scoring: positions of all cars, ~5Hz (radar).
    bool WantsScoringUpdates() override { return true; }

    void UpdateScoring(const ScoringInfoV01& info) override
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

        // My car, also while riding along (see PlayerView). Learning stores
        // (best laps, braking points, mini-sectors) keep the original, so a
        // team-mate's laps never become yours.
        ScoringInfoV01 viewCopy;
        const ScoringInfoV01& view = PlayerView(info, viewCopy);

        // Each call timed for the DLL-cost panel (Profiler.h).
        { PROF("Radar"); g_carState.Update(view); }
        { PROF("Brake cue"); g_brakeCue.OnScoring(info); }
        { PROF("Lap delta"); g_lapDelta.OnScoring(info); }
        { PROF("Conditions"); Conditions::OnScoring(view); }
        { PROF("Weather"); Weather::OnScoring(info); }
        { PROF("Pit stints"); PitStint::OnScoring(info); }
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
        { PROF("Car layout"); CarLayout::OnScoring(view); }
        { PROF("Tyre wear"); g_tyreWear.OnScoring(view); }

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
        s_playerLaps = playerLaps;

        // Out of the car (monitor, garage, between stints): everything,
        // including the INI, the per-car layouts and the log. Asked every
        // tick here; the Persist thread runs at most about one pass a second.
        if (!g_inRealtime)
            Persist::RequestAll();
    }

    // Telemetry scope: 1 = player car only, 2 = every vehicle.
    //
    // Player-only is what the driving HUD needs (pedal trace, fuel use,
    // gear learning) and is far cheaper. The scope is widened only when the
    // timing table's tyre column is switched on, because compound lives in
    // TelemInfoV01 and scoring carries no tyre data at all -- there is no
    // cheaper way to know what the rest of the field is running.
    //
    // The game re-asks this constantly rather than once at startup, so it
    // genuinely can be changed at runtime.
    long WantsTelemetryUpdates() override
    {
        // The tyre column only shows in the standings table and the gap box:
        // with both of those switched off, every other car's telemetry would
        // be fetched and stored for nothing.
        const RelativeTimingConfig& rt = g_relativeTimingConfig;
        return ((rt.enabled || rt.showGapBox) && rt.showTyre) ? 2 : 1;
    }

    void UpdateTelemetry(const TelemInfoV01& info) override
    {
        // Every car's telemetry, when the scope is wide. Fed first and
        // deliberately ABOVE the player filter below -- the whole point of
        // this store is the rest of the field's compounds.
        { PROF("Relative"); TyreStore::OnTelemetry(info); }

        // CRITICAL: with the scope widened to all vehicles, EVERY car's
        // telemetry arrives here. Everything below assumes player-only data
        // -- the gear display learns shift points from it, the tyre and
        // fuel store record against it -- so feeding it twenty cars
        // interleaved would corrupt learned state, and do so silently.
        const long playerId = g_playerId.load(std::memory_order_relaxed);
        if (playerId < 0 || info.mID != playerId)
            return;

        g_headlightsOn.store(info.mHeadlights ? 1 : 0, std::memory_order_relaxed);
        { PROF("Pedal trace"); g_pedalTrace.Update(info); }
        { PROF("Brake cue"); g_brakeCue.OnTelemetry(info); }
        {
            PROF("FFB graph");
            g_ffbSpectrum.OnTelemetry(info.mLapNumber, static_cast<float>(sqrt(
                info.mLocalVel.x * info.mLocalVel.x + info.mLocalVel.y * info.mLocalVel.y + info.mLocalVel.z * info.mLocalVel.z)));
        }
        { PROF("Gear + speed"); g_gearDisplay.Update(info); }
        { PROF("Fuel"); g_fuelStore.OnTelemetry(info); }
        { PROF("Tyre wear"); g_tyreWear.OnTelemetry(info); }
        { PROF("Conditions"); Conditions::OnTelemetry(info); }
        { PROF("Engine temps"); EngineTemps::OnTelemetry(info); }
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
        PROF("FFB graph");
        g_ffbSpectrum.OnForce(forceValue);
        FfbLevel::OnForce(forceValue);
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
