#pragma once
#include <Windows.h>

// Double-beep the moment DRS/the rear flap becomes legal to activate --
// edge-triggered on TelemInfoV01::mRearFlapLegalStatus transitioning to 2
// (allowed), not held while it stays allowed, so it's one clear cue rather
// than a repeated nag.
//
// Also exposes a live State (CRITICAL_SECTION-protected, read from the
// render thread) for the small "OPEN DRS" / "DRS ENGAGED" indicator drawn
// just below the LapDelta bar.
class DrsCueStore
{
public:
    void Init();
    // Player-only telemetry, physics rate.
    void OnTelemetry(const struct TelemInfoV01& info);
    void Clear(); // new session -- don't carry a stale "already legal" state over

    struct State
    {
        // True while legal-but-not-yet-open OR actually open -- false (so
        // nothing is drawn) once neither applies.
        bool showIndicator = false;
        bool drsOpen = false; // "OPEN DRS" vs "DRS ENGAGED"
    };
    State Read();

private:
    CRITICAL_SECTION m_lock;
    bool m_ready = false;
    State m_state;

    // Synthesized double-beep, built once in Init().
    unsigned char* m_wavBuffer = nullptr;
    DWORD m_wavSize = 0;

    int m_lastLegalStatus = -1; // -1 = unknown yet, so the very first sample can't spuriously fire

    // rF2 reports DRS legal everywhere on tracks with no zones (or free-DRS
    // rules). Legal without a break for this long means "unrestricted": the
    // flashing sign and the beep are muted until it next goes not-legal
    // (review F53). No real zone is anywhere near 40 s long.
    double m_legalSinceET = -1.0;
    bool m_unrestricted = false;

    void PlayBeep();
};

extern DrsCueStore g_drsCue;
