#pragma once
#include <Windows.h>
#include <atomic>
#include <vector>
#include <string>

// Live delta-to-best-lap, like a standard sim-racing "delta bar": compares
// current time-into-lap against a reference lap's time AT THE SAME TRACK
// DISTANCE (not the same elapsed time), so the comparison stays valid even
// off the reference lap's exact line.
//
// Two references are tracked at once:
//  - "Best": persisted to disk per car+track combo (the SDK's own
//    mBestLapTime is session-only and resets on restart), replaced whenever
//    a genuinely faster complete lap is set -- ever, across restarts.
//  - "Session": the fastest complete lap so far in the CURRENT session only,
//    kept in memory and forgotten on session end.
class LapDeltaStore
{
public:
    void Init();
    // Called from the game's scoring callback (~5Hz -- distance-around-track
    // is only available there, not in the higher-rate telemetry callback).
    void OnScoring(const struct ScoringInfoV01& info);
    // Forgets in-memory lap state (not the saved best-lap file) -- called on
    // session end so a stale in-progress trace, and the session-best
    // reference, don't leak into a new session.
    void Clear();
    // Writes the best-lap trace if it's changed since the last flush, and
    // deletes the file of a personal best reset from the hotkey. Called from
    // the save thread (Persist.cpp) at any moment: it swaps the save slot out
    // under m_saveLock and does the file work after. OnScoring() itself never
    // touches disk; setting a new best just fills the slot, so the game's
    // scoring thread can't be stalled by a slow disk or an antivirus scan.
    void FlushIfDirty();

    struct DeltaState
    {
        bool valid = false;          // false until this reference has a lap to compare against
        double deltaSeconds = 0.0;   // current time-into-lap minus reference time at this distance, AS OF updatedAt
        double trendPerSecond = 0.0; // +ve = losing time right now, -ve = gaining time right now
        // Wall-clock time this sample was computed -- distance-around-track
        // (and so deltaSeconds) only updates at the ~5Hz scoring rate, so the
        // renderer extrapolates deltaSeconds forward by trendPerSecond *
        // (now - updatedAt) to move smoothly at full frame rate instead of
        // visibly stepping every scoring tick.
        LARGE_INTEGER updatedAt = {};
    };
    struct State
    {
        DeltaState best;    // vs the all-time, disk-persisted reference
        DeltaState session; // vs the fastest lap so far this session

        // The delta as it stood AT the finish line, held briefly after
        // crossing so it can actually be read on the home straight. Without
        // this the number resets to ~0 for the new lap at the exact instant
        // the completed lap's result becomes meaningful -- the one moment it
        // matters most is the one moment it was never visible.
        bool haveLineFreeze = false;
        double bestAtLine = 0.0;
        double sessionAtLine = 0.0;
        LARGE_INTEGER lineCrossedAt = {};

        // The Session bar's current auto-sized window, in seconds (see
        // LapDeltaConfig::autoBarWindowEnabled) -- what the bar's fill
        // fraction is drawn against, in place of a fixed MaxBarSeconds.
        double barWindowSeconds = 1.0;

        // Live diagnostic readout of rF2's own lap-validity state, published
        // every scoring tick regardless of whether either reference is valid
        // -- see LapDeltaConfig::showValidityDebug for why this exists.
        int countLapFlag = -1;      // this tick's raw mCountLapFlag; -1 = no scoring data yet
        int lapValidityLatch = 2;   // minimum seen so far THIS lap -- what decides at lap end
        int pitState = -1;          // this tick's raw mPitState; -1 = no scoring data yet

        // Bumped whenever the value shown jumps for a reason other than
        // driving (lap completed, SB/PB changed, reset) so the bar's trend
        // colour can drop its history instead of reading the jump as a
        // gain/loss (review F104).
        unsigned refEpoch = 0;
    };
    State Read();

    // Time into the reference lap at a given distance around it, so another
    // widget can time its own stretches of track against the same reference
    // this one uses rather than rebuilding an identical trace. useSession
    // picks the session-best trace over the persisted all-time best. False
    // if that reference has no lap yet.
    //
    // Reads the trace directly, so this is for the SCORING thread only (the
    // same thread that builds it) -- see MiniSector.cpp, which snapshots
    // what it needs once per lap rather than calling this per frame.
    bool GetReferenceTimeAt(double lapDist, bool useSession, double& outTime) const;

    // In-game resets, driven from the Ctrl+Shift hotkeys. ResetSessionBest
    // is in-memory only. ResetPersonalBest is DESTRUCTIVE -- it deletes the
    // saved per-car+track CSV as well as the in-memory trace, so the caller
    // is expected to have confirmed with the user first. The readout clears
    // at once, the trace on the next scoring tick, the file in the next save
    // pass.
    void ResetSessionBest();
    void ResetPersonalBest();
    // "1:36.760" or "none set", for the confirmation prompt -- so the user is
    // told what they are about to delete rather than just asked twice.
    void DescribePersonalBest(char* out, size_t outSize) const;
    // Raw personal-best lap time in seconds for the current car+track, or
    // -1.0 if none is set yet. Unlocked, matching DescribePersonalBest's own
    // convention -- a torn read of one double is accepted as harmless here.
    double PersonalBestSeconds() const;
    // Looks up the personal-best trace's time at a given lap distance, for
    // callers that need a steadier reference than a raw distance/pace
    // estimate but have not yet completed a clean lap THIS session (e.g.
    // RelativeTiming's practice/qualy gap calc) -- the PB trace survives
    // across restarts, so it's usually available from the very first lap of
    // a session. Returns false if there is no saved PB trace yet at all.
    // Unlocked, same convention as PersonalBestSeconds() and
    // DescribePersonalBest().
    bool PersonalBestInterpolateAt(double lapDist, double& outTime) const;
    // The sanitized "vehicle__track" key the saved files are named by, and
    // the folder they live in -- for other stores that save per car+track.
    // Scoring thread only (the key is set in OnScoring).
    const std::string& CurrentKey() const { return m_currentKey; }
    // The same car+track under the old per-CLASS key: read once to seed a
    // car that has no file of its own yet (scoring thread, like CurrentKey).
    const std::string& LegacyKey() const { return m_legacyKey; }
    static void SaveDir(char* out, size_t outSize);

private:
    struct Reference
    {
        std::vector<std::pair<double, double>> trace; // (distance, timeIntoLap), in order of distance
        double lapTimeSeconds = -1.0; // -1 = no reference lap yet
        // trace's distances never step backwards (worked out whenever trace
        // is set), so InterpolateAt can binary-search it.
        bool sorted = true;

        double lastDeltaForTrend = 0.0;
        LARGE_INTEGER lastTrendTime = {};

        bool InterpolateAt(double lapDist, double& outTime) const;
        // Folds in a fresh (delta, now) sample, recomputing trendPerSecond
        // over windowSeconds rather than tick-to-tick, so it reflects a real
        // rate of change (gaining/losing time) rather than sample noise.
        void UpdateTrend(double delta, LARGE_INTEGER now, LARGE_INTEGER freq, double windowSeconds, DeltaState& outState);
    };

    CRITICAL_SECTION m_lock;
    bool m_ready = false;

    Reference m_best;    // persisted to disk
    Reference m_session; // in-memory only, this session

    // The lap currently being driven, accumulated as it happens.
    std::vector<std::pair<double, double>> m_currentTrace;

    std::string m_currentKey;  // sanitized "car__track", identifies which save file applies
    std::string m_legacyKey;   // sanitized "class__track" (pre-2026-11-24 files)
    short m_lastTotalLaps = -1;
    double m_lastLapDist = -1.0;
    // Latch the MINIMUM mCountLapFlag seen across the current lap: one
    // "don't count" moment (a cut) rules the whole lap out. Reset to 2 at
    // each lap start, in Clear() and on a car/track change (review F101).
    short m_lapValidityLatch = 2;
    // Set if, at any point during the lap, the player was not the one
    // driving (AI / not realtime), was in the pits or garage, or the session
    // clock jumped back -- such a lap never becomes a reference (F252, F97).
    bool m_lapTainted = false;
    double m_lastET = -1.0;
    // Key the last car/track had before Clear() emptied m_currentKey, so a
    // PB reset applied before the next key is known still deletes the file
    // m_best was loaded from (review F101).
    std::string m_lastKey;
    // Auto-sizing delta bar window (see LapDeltaConfig::autoBarWindowEnabled)
    // -- reset to the floor at each lap's completion, grown live during the
    // lap if needed. Scoring-thread-only; published to callers via
    // State::barWindowSeconds under m_lock, same as everything else here.
    double m_barWindowSeconds = 1.0;
    // Save slot, guarded by m_saveLock (taken only by the scoring thread,
    // the save thread and Clear -- never by drawing). A new best copies its
    // trace and key in here; FlushIfDirty() swaps them out and writes them.
    // m_bestLapDirty is also cleared by a PB reset, so the reset can't be
    // overwritten by a stale pending save.
    SRWLOCK m_saveLock = SRWLOCK_INIT;
    bool m_bestLapDirty = false;
    std::string m_saveKey;
    double m_saveLapTime = -1.0;
    std::vector<std::pair<double, double>> m_saveTrace;
    bool m_deletePending = false;  // a PB reset's file delete, for the next save pass
    std::string m_deleteKey;

    // Set by ResetPersonalBest() (render thread), carried out by the scoring
    // thread, which owns m_best -- see ApplyPersonalBestReset().
    std::atomic<bool> m_resetPbRequested{ false };
    // Same for the session best (ResetSessionBest): m_session is read
    // unlocked by the scoring thread, so only that thread drops it.
    std::atomic<bool> m_resetSessionRequested{ false };
    void ApplyPersonalBestReset();

    State m_state;
    LARGE_INTEGER m_freq = {};

    void LoadBestLap(const char* vehicleName, const char* trackName, double trackLength);
};

extern LapDeltaStore g_lapDelta;

class Renderer;

namespace LapDelta
{
    // Draws both delta bars + numbers, stacked. Returns false if nothing was
    // drawn (no reference lap yet for either, or the widget is disabled).
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight);

    // The OPEN DRS / DRS ENGAGED sign, its own panel (review F51). Drawn
    // independently of Draw, only while DRS is relevant (or in layout mode).
    bool DrawDrs(Renderer& renderer, int screenWidth, int screenHeight);

    // Height-to-width ratio shared with MiniSector's delta-speed strip, so
    // the two bars -- independently positioned, independently resized --
    // always end up the same height whenever they happen to end up the same
    // width, without their resize handles needing to be linked to each
    // other. Derived from LapDeltaConfig's own BarHeightPixels/
    // BarWidthFraction at the 3840px-wide reference (matching the
    // screenHeight/2160 convention every panel's own uiScale already uses).
    float BarAspectRatio();
}
