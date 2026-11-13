#pragma once
#include <Windows.h>

// Live setup-related readouts that don't belong to any other widget --
// currently just brake balance (TelemInfoV01::mRearBrakeBias). Front/rear
// anti-roll bar settings were also requested, but there is no corresponding
// field anywhere in InternalsPlugin.hpp -- the rFactor2 Internals Plugin API
// doesn't expose ARB setup values, so they can't be shown here.
class SetupInfoStore
{
public:
    void Init();
    // Called from the game's telemetry callback (player car only).
    void OnTelemetry(const struct TelemInfoV01& info);
    void Clear();
    // Re-arms the baseline so the next telemetry sample becomes the new
    // "as left the garage" value -- call on entering the car, so a setup
    // changed in the garage is compared against itself, not the session's
    // very first setup.
    void ResetBaseline();

    struct State
    {
        bool valid = false;
        double rearBrakeBiasPercent = 0.0; // 0..100, fraction of braking on the rear
        // The bias as it stood when the car left the garage/pits -- the
        // setup's own value, before any in-car adjustment. There is no
        // setup-sheet field in the API, so it is captured from the first
        // telemetry sample of the session and held fixed from there.
        // Pairing it with the
        // live value above turns a bare percentage into "how far I have
        // moved it", which is the number that actually gets managed on
        // track. -1 = not captured yet.
        double garageRearBrakeBiasPercent = -1.0;
    };
    State Read();

private:
    CRITICAL_SECTION m_lock;
    bool m_ready = false;
    State m_state;
};

extern SetupInfoStore g_setupInfo;
