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
#include "PitEntry.h"
#include "RealtimeState.h"
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
#include "CarLayout.h"

// Latched from scoring, read on the telemetry thread. -1 until the first
// scoring tick names a player.
static std::atomic<long> g_playerId{ -1 };

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
        g_pitEntry.Init();
        g_miniSector.Init();
        g_tyreTemps.Init();
        AmdGpuPower::Init(); // no-op, quietly, on non-AMD hardware
        GpuEngineUsage::Init();
        g_ffbSpectrum.Init();

        if (Hook::Install())
            Log::Write("SkrPlugin::Startup: D3D11 hook installed");
        else
            Log::Write("SkrPlugin::Startup: hook not installed yet (will retry on first session)");
    }

    void Shutdown() override
    {
        Log::Write("SkrPlugin::Shutdown");
        // Last chance -- EndSession's own flush covers a normal session end,
        // but not the game closing (or the plugin being re-ticked off)
        // without one, e.g. mid-session.
        Config::FlushPendingWrites();
        Log::FlushBuffered();
        Hook::Uninstall();
        Shortcuts::Shutdown();
        AmdGpuPower::Shutdown();
        GpuEngineUsage::Shutdown();
    }

    void StartSession() override
    {
        Log::Write("SkrPlugin::StartSession");
        Shortcuts::ResetHint();
        // Again here, not only at EndSession: scoring that arrives between the
        // two can leave the timing table frozen on the old session.
        g_relativeTiming.Clear();
        // D3D is definitely up by now; make sure the hook is in place.
        if (Hook::Install())
            Log::Write("SkrPlugin::StartSession: hook confirmed installed");
    }

    void EndSession() override
    {
        Log::Write("SkrPlugin::EndSession");
        g_inRealtime = false;
        // Last chance to write anything still dirty -- BEFORE the Clear()
        // calls below, which wipe the very vehicle/track keys these saves
        // need. Covers a session ending without an intervening scoring tick
        // where g_inRealtime was already false (see UpdateScoring).
        g_gearDisplay.FlushIfDirty();
        g_fuelStore.FlushIfDirty();
        g_lapDelta.FlushIfDirty();
        TrackMap::FlushIfDirty();
        CarLayout::FlushIfDirty();
        if (!Config::FlushPendingWrites())
            Log::Write("Plugin: one or more queued INI writes failed (read-only? UAC?)");
        Log::FlushBuffered();
        g_carState.Clear();
        g_pedalTrace.Clear();
        g_gearDisplay.Clear();
        g_lapDelta.Clear();
        g_relativeTiming.Clear();
        g_fuelStore.Clear();
        g_tyreWear.Clear();
        g_setupInfo.Clear();
        g_drsCue.Clear();
        g_pitEntry.Clear();
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
        g_inRealtime = true;
    }

    void ExitRealtime() override
    {
        // Leaving the car -- stop drawing stale positions.
        Log::Write("SkrPlugin::ExitRealtime");
        g_inRealtime = false;
        g_carState.Clear();
        g_pedalTrace.Clear();
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

        // Each call timed for the DLL-cost panel (Profiler.h).
        { PROF("Radar"); g_carState.Update(info); }
        { PROF("Lap delta"); g_lapDelta.OnScoring(info); }
        { PROF("Relative"); g_relativeTiming.OnScoring(info); }
        { PROF("Fuel"); g_fuelStore.OnScoring(info); }
        { PROF("Pit entry"); g_pitEntry.OnScoring(info); }
        { PROF("Mini-sectors"); g_miniSector.OnScoring(info); }
        { PROF("Relative"); DriverAbbrev::OnScoring(info); }
        { PROF("Track map"); TrackMap::OnScoring(info); }
        { PROF("Car layout"); CarLayout::OnScoring(info); }
        { PROF("Tyre wear"); g_tyreWear.OnScoring(info); }

        // The player's slot ID, for the telemetry filter below. Scoring is
        // the only callback that reports mIsPlayer, so it has to be latched
        // here. Written on the scoring thread and read on the telemetry
        // thread; a long is atomic on x86-64 and a one-tick-stale ID is
        // harmless, which is much cheaper than a lock on a callback that
        // fires thousands of times a second.
        if (info.mVehicle)
        {
            for (int i = 0; i < info.mNumVehicles; ++i)
            {
                if (info.mVehicle[i].mIsPlayer)
                {
                    g_playerId.store(info.mVehicle[i].mID, std::memory_order_relaxed);
                    break;
                }
            }
        }

        // Persist any learned value(s) picked up while driving now that
        // we're not -- back at the monitor, in the garage, between stints.
        // These stores never touch disk from the telemetry/scoring thread
        // themselves any more (see each FlushIfDirty()); this is a no-op
        // dirty-flag check the rest of the time, ~5Hz, so it costs nothing
        // extra while actually on track.
        if (!g_inRealtime)
        {
            g_gearDisplay.FlushIfDirty();
            g_fuelStore.FlushIfDirty();
            g_lapDelta.FlushIfDirty();
            TrackMap::FlushIfDirty();
            CarLayout::FlushIfDirty();
            if (!Config::FlushPendingWrites())
                Log::Write("Plugin: one or more queued INI writes failed (read-only? UAC?)");
            Log::FlushBuffered();
        }
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
        // Player-only is what the driving HUD needs and is far cheaper. The
        // scope widens only when the timing table's tyre column asks for it,
        // because compound lives in TelemInfoV01 and scoring carries no tyre
        // data at all -- there is no cheaper way to know what the rest of
        // the field is running.
        return g_relativeTimingConfig.showTyre ? 2 : 1;
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
        if (playerId >= 0 && info.mID != playerId)
            return;

        { PROF("Pedal trace"); g_pedalTrace.Update(info); }
        {
            PROF("FFB graph");
            g_ffbSpectrum.OnTelemetry(info.mLapNumber, static_cast<float>(sqrt(
                info.mLocalVel.x * info.mLocalVel.x + info.mLocalVel.y * info.mLocalVel.y + info.mLocalVel.z * info.mLocalVel.z)));
        }
        { PROF("Gear + speed"); g_gearDisplay.Update(info); }
        { PROF("Fuel"); g_fuelStore.OnTelemetry(info); }
        { PROF("Tyre wear"); g_tyreWear.OnTelemetry(info); }
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
