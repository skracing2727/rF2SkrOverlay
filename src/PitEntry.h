#pragma once
#include <Windows.h>
#include <string>

// Where the pit-lane entry line is, and how far away it is while a pit stop
// is requested -- for the pit limiter's "engage the limiter" reminder (see
// GearDisplay), which should only appear close to pit entry.
//
// VehicleScoringInfoV01::mPitLapDist is the base reference, but it does not
// land exactly on the entry line. mInPits is used as a live auto-calibration
// on top of it: the false->true transition while a stop is being approached
// (mPitState 1=request or 2=entering) gives the true entry line's lap
// distance, and the gap between that and mPitLapDist is saved as a per-track
// offset in a "PitEntryOffsets" folder next to the INI.
// PitLimiterConfig::entryOffsetMeters is the fallback before a track has one.
class PitEntryStore
{
public:
    void Init();
    // All-vehicle scoring, ~5Hz: pit request state, lap distance, pit
    // location, and track length (for wraparound near start/finish).
    void OnScoring(const struct ScoringInfoV01& info);
    void Clear();

    struct State
    {
        bool armed = false;             // a pit stop is requested
        double distanceToLineM = 0.0;   // forward distance to the entry line, valid while armed
    };
    State Read();

private:
    CRITICAL_SECTION m_lock;
    State m_liveState; // written on the scoring thread, read elsewhere
    bool m_ready = false;

    // Scoring thread only.
    std::string m_trackKey;
    double m_entryOffsetMeters = 0.0;
    bool m_hasInPitsBaseline = false;
    bool m_lastInPits = false;
    bool m_armed = false;
    double m_pitLineLapDist = 0.0;
    double m_trackLength = 0.0;
    void LoadEntryOffset(const char* trackName);
    void SaveEntryOffset(const char* trackName) const;
};

extern PitEntryStore g_pitEntry;