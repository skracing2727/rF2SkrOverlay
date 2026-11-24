#pragma once
#include <Windows.h>
#include <vector>
#include <string>

// Beeps at each braking point from your own best lap.
//
// While you drive, every point where you start braking is recorded against your
// distance around the lap. If that lap turns out to be a new personal best when
// it completes, those points become the reference; on every lap after that, you
// get a beep at each one as you reach it -- effectively "this is where you braked
// when you set your best time."
//
// Only big stops beep: each point remembers how much speed the reference lap
// lost there, and points below BrakeCueConfig::minSpeedDropKmh (a slider in
// the shortcut list) pass silently.
//
// The beep fires a fixed BrakeCueConfig::leadTimeSec (0.3 s) before each
// reference point. The gap between a beep and the brake pedal moving is still
// measured and shown. Optionally (adaptiveLeadTimeEnabled, off by default) the
// lead time follows the measured average instead -- but that cannot tell a
// slow reaction from braking later than the reference on purpose, and runs
// away as you brake later and later, which is why it is off.
//
// Recording/checking/beeping all happen on the game's telemetry thread, but
// the measured averages are drawn by the render thread, so that snapshot (and
// only that snapshot) is passed across under m_lock, the same way every other
// store here does it.
class BrakeCueStore
{
public:
    void Init();

    // Player-only telemetry, physics rate: brake input + speed for extrapolation.
    void OnTelemetry(const struct TelemInfoV01& info);

    // All-vehicle scoring, ~5Hz: authoritative lap distance (resync), lap
    // completion, lap validity, track length.
    void OnScoring(const struct ScoringInfoV01& info);

    // New session / track -- the reference belongs to the old one, so
    // everything goes, learned reaction times included.
    void Clear();

    // Dropped to the monitor / back to the garage, but still the SAME
    // session. Only the transient in-lap state is dropped (the partial
    // recording, position extrapolation, which cue is next, any beep still
    // waiting on a pedal). The reference braking points, the best lap time
    // they came from, and the learned reaction average all SURVIVE: the
    // reference is meant to be "wherever you braked on your best lap this
    // session", and wiping it here made it "wherever you braked on the first
    // lap after you last looked at the monitor" instead.
    void OnExitRealtime();

    // Writes the saved brake profiles if a new one joined the best five.
    // Called from the save thread (Persist.cpp) at any moment: copies the
    // list under m_saveLock, writes after releasing it.
    void FlushIfDirty();

    // Beep-to-pedal reaction times, for the on-screen readout.
    struct State
    {
        // Rolling average over the last kReactionHistorySize measurements --
        // also what the beep's lead time is set to. -1 = nothing measured yet.
        double avgReactionSeconds = -1.0;
        // The single most recent measurement. -1 = nothing measured yet.
        double latestReactionSeconds = -1.0;
        int sampleCount = 0;
        // The lead time actually in use right now: the learned average once
        // there is one, otherwise BrakeCueConfig::leadTimeSec.
        double leadTimeInUseSeconds = 0.0;
    };
    State Read();

private:
    static const int kMaxPoints = 64; // per lap; real tracks have far fewer corners
    // Beeps come several times a lap, so this is a couple of laps' worth --
    // long enough to be steady, short enough to follow a genuine change in
    // how quickly the driver is reacting (settling in, tiring, a new track).
    static const int kReactionHistorySize = 20;

    CRITICAL_SECTION m_lock;
    bool m_ready = false;

    // Synthesized beep, built once in Init().
    unsigned char* m_wavBuffer = nullptr;
    DWORD m_wavSize = 0;

    double m_trackLength = 0.0;

    // Position tracking (extrapolated between the 5Hz scoring updates).
    double m_lastKnownDist = 0.0;
    double m_forwardSpeed = 0.0;
    LARGE_INTEGER m_lastSyncTime = {};
    LARGE_INTEGER m_freq = {};

    // Recording the current, in-progress lap.
    double m_recording[kMaxPoints];
    // Speed lost at each point: speed where braking began minus the lowest
    // speed before the next point (or the lap end), m/s. Lets the cue skip
    // the small lifts and dabs (BrakeCueConfig::minSpeedDropKmh).
    double m_recordingEntrySpeed[kMaxPoints];
    double m_recordingMinSpeed[kMaxPoints];
    int m_recordingCount = 0;
    bool m_wasBraking = false;

    // The reference (best) lap so far.
    double m_reference[kMaxPoints];
    double m_referenceDropMs[kMaxPoints];
    int m_referenceCount = 0;
    double m_referenceLapTime = -1.0; // -1 = no reference yet
    int m_nextCueIndex = 0;           // next reference point still to fire, this lap

    // Brake pedal round the lap, the highest value in each kBrakeBinM bin.
    // Recorded on the lap in progress (telemetry thread), copied to the
    // reference on a new best (scoring thread) -- so both under m_lock. Recorded whether or
    // not the beep itself is enabled.
    static constexpr double kBrakeBinM = 5.0;
    std::vector<float> m_recBrake;
    std::vector<float> m_refBrake;
    double m_currentDist = 0.0;       // this tick's extrapolated lap distance
    bool m_lapFullyRecorded = false;  // this lap's profile began at the line (scoring thread)

    // The brake profiles of your five fastest valid laps in this car at this
    // track, kept across sessions (BestLaps\<car>__<track>.brake). The
    // reference profile (m_refBrake) is their per-bin median, so it is
    // there from the first lap of a session and one odd lap cannot skew it.
    // Scoring thread only, apart from m_refBrake -- and the save thread's
    // copy in FlushIfDirty, which is why every change to these three takes
    // m_saveLock (nothing else does, so neither telemetry nor drawing ever
    // waits on it). The scoring thread's own reads need no lock.
    static const int kKeptLaps = 5;
    struct SavedLap { double lapTime; std::vector<float> brake; };
    SRWLOCK m_saveLock = SRWLOCK_INIT;
    std::vector<SavedLap> m_savedLaps;  // fastest first
    std::string m_savedKey;             // car+track the list belongs to; empty = not loaded
    bool m_savedDirty = false;
    void LoadSavedLaps(const std::string& key, size_t bins);
    static void WriteSavedLaps(const std::string& key, const std::vector<SavedLap>& laps);
    void AddSavedLap(double lapTime, const std::vector<float>& brake);
    void RebuildReferenceFromSaved();   // takes m_lock itself
    // A finished lap whose time had not reached scoring yet.
    double m_lapTimeAtStart = 0.0;
    bool m_pendingSave = false;
    double m_pendingOldLapTime = 0.0;
    double m_pendingSinceET = 0.0;
    std::vector<float> m_pendingBrake;

    short m_lastTotalLaps = -1;
    // Latch the minimum mCountLapFlag seen across the current lap, not a
    // snapshot at lap completion -- the flag resets to a new lap before being
    // read here, so single-point reads would miss valid laps. Use the latch
    // instead: if it ever hits 2 during the lap, the lap counts.
    short m_lapValidityLatch = 2;

    // --- Reaction-time measurement (telemetry thread only) ---
    // A beep is "pending" from the moment it fires until the pedal is seen
    // to move, or the measurement is abandoned. Only armed when the pedal
    // was BELOW the threshold as the beep fired -- a beep landing while the
    // driver is already on the brakes has no reaction to measure.
    bool m_pendingBeep = false;
    LARGE_INTEGER m_beepTime = {};
    bool m_wasPedalPressed = false; // edge detection for the reaction threshold

    double m_reactionHistory[kReactionHistorySize] = {};
    int m_reactionHistoryCount = 0;
    int m_reactionHistoryNext = 0;
    // The authoritative learned average, owned by the telemetry thread (so
    // timing a beep never has to take the lock). Republished into m_state
    // for the render thread each time it changes. -1 = none yet.
    double m_avgReactionSeconds = -1.0;

    // Guarded by m_lock -- the only state the render thread reads.
    State m_state;

    void PlayBeep();
    // Folds one measurement into the history and republishes the averages.
    void RecordReaction(double reactionSeconds);
    // Learned average if there is one, else the configured seed value.
    double LeadTimeSeconds() const;
};

extern BrakeCueStore g_brakeCue;

class Renderer;

namespace BrakeCue
{
    // Draws the reaction-time readout. Returns false if there is nothing to
    // show (disabled, or no measurement yet).
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight);
}
