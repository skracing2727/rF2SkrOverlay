#pragma once
#include <Windows.h>
#include <string>

// Dashboard: current gear + speed on a dark panel (styled after the native
// in-game HUD's digital display, positioned lower on screen instead), plus a
// separate pit-limiter indicator drawn beside it.
//
// Gear flashes red when RPM is near the shift point (the SDK has no explicit
// "optimal shift RPM" field, so this is a configurable fraction of the
// engine's actual rev limit), and flashes blue as the "shift now" cue.
//
// The blue cue is NOT tied to the physical rev limiter: it fires at a
// TARGET RPM that adaptively learns from your own upshifts (see
// State::learnedShiftRpmFraction), LED BACK by your own measured reaction
// time so that by the time you actually pull the paddle, the shift lands ON
// the target instead of past it. Reaction time is learned from the gap
// between the cue turning on and the upshift actually happening,
// rolling-averaged and used as the lead going forward (see State::shiftReactionAvgSeconds).
class GearDisplayStore
{
public:
    void Init();
    // Called from the game's telemetry callback (player car only).
    void Update(const struct TelemInfoV01& info);
    // Forgets in-memory pit-limit learning state (not the saved file) --
    // called on session end, so a stale in-progress episode doesn't leak
    // into a new session.
    void Clear();
    // Writes any learned value changed since the last flush -- call only
    // while NOT actively driving (see Plugin.cpp). Update() itself never
    // touches disk; a learning event just sets the matching dirty flag
    // below, so the game's telemetry thread can't be stalled by a slow disk
    // or an antivirus scan intercepting the write.
    void FlushIfDirty();

    // Snapshot for drawing.
    struct State
    {
        int gear = 0;              // -1 reverse, 0 neutral, 1+ forward
        double rpm = 0.0;
        double maxRpm = 0.0;
        double speedMs = 0.0;       // world speed, meters/sec
        bool speedLimiterOn = false;
        // The SDK doesn't expose the actual numeric pit-lane limit, but the
        // limiter itself caps speed AT that limit -- so the peak reached
        // while it's engaged converges to the real value with no manual
        // configuration needed. Persisted to disk per car+track combo (see
        // GearDisplay.cpp) and only ever refined DOWNWARD by a later
        // observation, since a higher one can only come from activating the
        // limiter already above the cap (common in a race) rather than a
        // genuine limit change -- so it's ignored instead of corrupting the
        // learned value. 0 until a value has been learned or loaded.
        double learnedPitLimitKmh = 0.0;
        // TARGET fraction of maxRpm -- the RPM the blue cue is trying to get
        // you to actually shift AT (the cue itself fires earlier than this,
        // by the reaction lead; see shiftReactionAvgSeconds below).
        //
        // Adaptively tuned toward wherever YOUR OWN upshifts actually land:
        // shifting consistently later than the cue asked for nudges the
        // target up, shifting earlier nudges it down, and shifting where it
        // asked leaves it alone. Note this error is measured against the
        // CURRENT TARGET, not against maxRpm -- measuring against maxRpm
        // (as this originally did) makes "shift at the rev limiter" the only
        // fixed point, so the target climbs to redline on every shift and
        // stays there no matter how the driver actually drives.
        //
        // 0 until at least one upshift has been observed;
        // GearDisplayConfig::revLimiterRpmFraction is the seed until then.
        double learnedShiftRpmFraction = 0.0;

        // The same learned trigger, but kept SEPARATELY PER GEAR -- indexed
        // by the gear being shifted OUT of. One number for the whole car is
        // wrong on almost any real gearbox: the lower gears are geared much
        // shorter, so they spend far less time near the limiter and get
        // shifted at a different point than 5th or 6th, and averaging the
        // two together produces a cue that is late in the low gears and
        // early in the high ones. Each gear learns from its own upshifts
        // only. 0 for a gear that has not been observed yet, in which case
        // the car-wide value above (and failing that the configured
        // fraction) still applies.
        static const int kMaxGears = 10; // gears 1..9, index 0 unused
        double learnedShiftRpmFractionByGear[kMaxGears] = {};
        // Per-gear counterpart of shiftAvgPercent below, so the on-screen
        // readout can show how the gear you are actually in is being shifted
        // rather than only the car-wide average.
        double shiftAvgPercentByGear[kMaxGears] = {};
        int shiftSampleCountByGear[kMaxGears] = {};

        // Rolling stats on your actual upshifts, as a percentage of maxRpm --
        // average, and the 1st percentile (the low end of your shifts: how
        // early your earliest-shifting outliers are, not just the typical
        // one). 0 samples until at least one upshift has been observed.
        double shiftAvgPercent = 0.0;
        double shiftP1Percent = 0.0;
        int shiftSampleCount = 0;

        // True while the blue "shift now" cue should be showing -- computed
        // in Update() (telemetry thread) rather than at draw time, since
        // arming/clearing it is also what the reaction-time measurement
        // below hangs off of, and that bookkeeping needs to happen exactly
        // once per edge, not once per frame.
        bool shiftCueActive = false;

        // How long it actually takes you to react to the cue and pull the
        // shift, learned from the gap from the cue arming to the upshift actually registering,
        // rolling-averaged. This is what LEADS the cue ahead of the target
        // RPM (see GearDisplay.cpp) -- without it, the cue would fire AT the
        // target and your own reaction delay would push every real shift
        // past it, which is exactly the "it just flashes at the rev
        // limiter" complaint this replaced. -1 / 0 until at least one
        // reaction has been measured.
        double shiftReactionAvgSeconds = -1.0;
        double shiftReactionLatestSeconds = -1.0;
        int shiftReactionSampleCount = 0;

        // True while ABS / TC is actually intervening -- detected from the
        // filtered (post-electronics) brake/throttle sitting meaningfully
        // below the raw input, since there's no direct telemetry field for
        // it. Tracked separately because they come from separate channels
        // (brake for ABS, throttle for TC) and are worth naming individually
        // on screen. Drawn as their own small box beside the gear digit --
        // this used to flash the SPEED box yellow, which now carries the
        // slip feedback below instead.
        bool absActive = false;
        bool tcActive = false;

        // What the tyres are doing longitudinally, for the speed box's
        // background: 0 = nothing to report, 1 = locking, 2 = wheelspin.
        // Resolved in GearDisplay.cpp against the configurable slip
        // thresholds, with a dwell before it shows and a minimum hold once
        // it does.
        int slipState = 0;

        // True while approaching a pit stop (PitEntryStore::State::armed),
        // speed has dropped to/under the pit-lane limit, but the limiter
        // still isn't engaged -- a reminder to actually press the button,
        // cleared the moment it is (or speed goes back over).
        bool showEngageLimiterWarning = false;

        bool valid = false;        // false until the first telemetry update arrives
    };
    State Read();

private:
    CRITICAL_SECTION m_lock;
    bool m_ready = false;
    State m_state;

    std::string m_currentKey;      // sanitized "vehicle__track", identifies which save file applies
    bool m_lastSpeedLimiterOn = false;
    double m_episodeMaxKmh = 0.0;        // running max speed for the CURRENT limiter-engaged episode
    double m_episodeStartSpeedKmh = 0.0; // speed at the instant this episode's limiter engaged
    // Set instead of saving directly whenever a learned value changes;
    // cleared by FlushIfDirty() once actually written.
    bool m_pitLimitDirty = false;
    bool m_shiftPointDirty = false;

    // "Engage the limiter" reminder: edge-triggered long beep, synthesized
    // once in Init() the same way DrsCue builds its double-beep.
    bool m_lastBelowLimitAwaitingEngage = false;
    unsigned char* m_engageWavBuffer = nullptr;
    DWORD m_engageWavSize = 0;
    void PlayEngageBeep();

    // Shift-point learning: keyed by VEHICLE only (rev-limiter behaviour is
    // a car property, not a track one), separate from m_currentKey above.
    std::string m_shiftKey;
    // Gear-change edge for the ABS/TC flash suppression -- separate from the
    // shift-point learning's own edge tracking below, which only fires on
    // UPshifts and only once a sample is complete.
    int m_lastGearForTcCut = -999;
    double m_lastGearChangeET = -1.0;

    // Slip-feedback debounce. A candidate state has to persist for the dwell
    // before it is shown, and the shown state then survives for at least the
    // hold -- so neither a single noisy sample nor a genuinely brief lockup
    // is mishandled. Timed off the sim clock so it stops when the sim does.
    int m_slipCandidate = 0;
    double m_slipCandidateSince = -1.0;
    int m_slipShown = 0;
    double m_slipShownSince = -1.0;

    // Running sum/count per gear behind State::shiftAvgPercentByGear. A
    // plain mean rather than the sorted history the car-wide stats keep,
    // since no percentile is wanted per gear -- just "where do I typically
    // change out of this one".
    double m_shiftSumByGear[State::kMaxGears] = {};
    int m_shiftCountByGear[State::kMaxGears] = {};

    bool m_hasLastShiftSample = false;
    int m_lastGearForShift = 0;
    double m_lastRpmForShift = 0.0;
    // RAW (unfiltered -- what the driver actually pressed, not post-TC)
    // throttle from the sample just before the gear change, same convention
    // as m_lastRpmForShift. Gates the learner: only a shift taken at
    // genuine full throttle says anything about where the optimal upshift
    // point is. Without this, a deliberate short-shift into a corner (which
    // is BY DEFINITION off-throttle -- you are about to brake, not power
    // through) reads as "shifted early" and drags the target down to match
    // it, even though nothing was wrong with the earlier, full-throttle
    // shifts either side of it.
    double m_lastThrottleForShift = 0.0;

    // RPM rate-of-climb, from consecutive telemetry samples' own elapsed
    // time -- used to convert the reaction-time lead (seconds) into an RPM
    // offset for the cue trigger point. -1 sentinel on m_lastTimeForRate
    // means no previous sample yet, so the very first tick doesn't compute
    // a rate against a bogus baseline.
    double m_lastRpmForRate = 0.0;
    double m_lastTimeForRate = -1.0;
    // EMA of the above. The raw per-sample rate is noisy and occasionally
    // unavailable (two telemetry samples carrying the same elapsed time),
    // and a rate that momentarily reads zero would collapse the cue's RPM
    // lead to nothing -- flicking the cue off and back on, which in turn
    // re-arms the reaction timer mid-cue and measures a falsely short
    // reaction. Smoothing keeps the lead steady across those gaps.
    double m_rpmRateSmoothed = 0.0;

    // "Shift now" cue arming, for reaction-time measurement. Armed the instant the cue turns on; consumed (reaction
    // recorded) by the next upshift; abandoned if the cue turns back off
    // first (lifted off, didn't shift) or nothing happens within the
    // timeout.
    bool m_shiftCuePending = false;
    double m_shiftCueArmedET = -1.0;
    // The cue LATCHES once shown, and the RPM it first triggered at is kept
    // so it can be released against a fixed point. Re-testing the live
    // trigger every tick does not work: the trigger is
    // target - (lead * climb rate), and the climb rate collapses as the
    // engine nears the limit, so the trigger RPM RISES -- sometimes faster
    // than the engine is climbing. The cue then flicks off and re-arms
    // moments later, which both stutters the light and restarts the
    // reaction timer, so the measured reaction comes out below
    // shiftReactionMinSeconds and is thrown away. That is why 530 logged
    // shifts produced zero reaction samples.
    bool m_shiftCueLatched = false;
    double m_shiftCueArmRpm = 0.0;
    static const int kShiftReactionHistorySize = 20;
    double m_shiftReactionHistory[kShiftReactionHistorySize] = {};
    int m_shiftReactionHistoryNext = 0;
    int m_shiftReactionHistoryCount = 0;
    double m_avgShiftReactionSeconds = -1.0; // owned by the telemetry thread; read back only under m_lock via m_state
    void RecordShiftReaction(double reactionSeconds);
    double ShiftReactionLeadTimeSeconds() const;

    // Rolling shift-percentage history for shiftAvgPercent/shiftP1Percent --
    // in-memory only (a live "how am I shifting" stat, not a persisted
    // control value like learnedShiftRpmFraction), circular once full.
    static const int kShiftHistorySize = 200;
    double m_shiftHistory[kShiftHistorySize] = {};
    int m_shiftHistoryCount = 0;
    int m_shiftHistoryNext = 0;
    void RecomputeShiftStats();

    void LoadPitLimit(const char* vehicleName, const char* trackName);
    void SavePitLimit() const;
    void LoadShiftPoint(const char* vehicleName);
    void SaveShiftPoint() const;
};

extern GearDisplayStore g_gearDisplay;

class Renderer;

namespace GearDisplay
{
    // Draws the gear+speed panel and the pit-limiter box. Returns false if
    // nothing was drawn (no telemetry yet).
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight);
}
