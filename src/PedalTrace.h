#pragma once
#include <Windows.h>

// Scrolling pedal trace: throttle and brake over time, plus wheel lockup and
// wheel spin markers.
//
// Two lines are drawn per pedal:
//   * raw    - what the driver actually pressed
//   * actual - what the car ended up with after TC (throttle) or ABS (brake)
// The gap between them is the electronics intervening.
class PedalTraceStore
{
public:
    void Init();
    // Called from the game's telemetry callback (player car only).
    void Update(const struct TelemInfoV01& info);
    void Clear();

    struct Sample
    {
        float throttleRaw;
        float throttleActual;   // after traction control
        float brakeRaw;
        float brakeActual;      // after ABS
        float lockup;           // 0..1, worst wheel
        float spin;             // 0..1, worst wheel
        float ffb;              // -1..1, force feedback: + = pushing the wheel right
    };

    // Brings the caller's own copy of the history up to date, copying under
    // the telemetry lock only the samples committed since its last call --
    // one or two a frame, not the whole 10 s. 'ring' holds kHistory samples,
    // each at (absolute sample number % kHistory); 'total' is the caller's
    // count of samples copied so far (0 the first time) and comes back as
    // the number ever committed, so the newest sample is number total - 1.
    // Returns how many of the newest samples the ring now holds -- the
    // valid ones are numbers total - n .. total - 1.
    int ReadNew(Sample* ring, long long& total);

    // RAW slip ratios, for consumers that need to judge slip against their
    // own thresholds rather than the trace's display scaling. Sample::lockup
    // and ::spin above are already normalised through slipDeadzone /
    // slipFullScale for drawing a bar, which throws away the actual number --
    // fine for a VU meter, useless for deciding whether braking is at the
    // grip peak or past it.
    struct SlipState
    {
        bool valid = false;
        // Most negative slip anywhere on the car, as a POSITIVE magnitude.
        // Any single wheel locking matters, so this is the worst of four.
        double worstLock = 0.0;
        // Front-axle average, same sign convention. Used for the "at the
        // grip peak" judgement, where the worst wheel would be wrong: one
        // wheel nudging past peak while the other three are fine is not the
        // car being over the limit.
        double frontAvgLock = 0.0;
        // Largest positive slip on the DRIVEN axle only.
        double worstSpin = 0.0;
        double brakeInput = 0.0;
        // Per-wheel locking, same raw-magnitude convention as worstLock
        // above but kept separate per corner rather than reduced to the
        // single worst one -- for the 4-corner lockup box, which needs to
        // know WHICH wheel(s), not just how bad the worst one is. Order is
        // the SDK's fixed wheel order: 0=FL, 1=FR, 2=RL, 3=RR.
        double perWheelLock[4] = { 0.0, 0.0, 0.0, 0.0 };
    };
    SlipState ReadSlip();

    // Wall-clock seconds since the car was last NOT (stationary AND off both
    // pedals) -- i.e. since it was last actually moving or a pedal was in use.
    // Deliberately wall-clock rather than tied to telemetry ticks, so this
    // keeps growing correctly even if Update() stops being called entirely
    // (e.g. back at the menu).
    double SecondsSinceActive();

    // History length = duration * sample rate. Sample rate is kSampleIntervalSec
    // in PedalTrace.cpp (currently 144Hz), so 1440 = 10 seconds.
    static const int kHistory = 1440;  // 10 seconds at 144Hz

private:
    CRITICAL_SECTION m_lock;
    bool m_ready = false;

    Sample m_samples[kHistory] = {};
    int m_head = 0;        // next write slot
    int m_count = 0;
    long long m_total = 0; // samples ever committed (for stable thinning buckets)

    LARGE_INTEGER m_lastSample = {};
    LARGE_INTEGER m_freq = {};

    // Updated in Update() whenever the car is moving OR a pedal is pressed;
    // read (wall-clock, wait-free of new samples) via SecondsSinceActive().
    LARGE_INTEGER m_lastActiveTime = {};

    // The game's physics/telemetry callback fires faster than the 144Hz commit
    // rate below -- rather than discarding those extra calls, they're
    // accumulated here and folded into the next committed sample: averaged for
    // the pedal channels (so ABS/TC pulsing faster than 144Hz shows as a smooth
    // partial value instead of whichever instant happened to land on the
    // sample tick), peak-held for lockup/spin (so a brief slip event between
    // sample ticks can't be missed entirely).
    double m_accThrottleRaw = 0.0, m_accThrottleActual = 0.0;
    double m_accBrakeRaw = 0.0, m_accBrakeActual = 0.0;
    double m_accLockupPeak = 0.0, m_accSpinPeak = 0.0;

    // Raw slip, published every callback rather than at the 144Hz commit
    // rate -- a lockup lasting less than one commit interval still matters
    // to whoever is watching for it.
    SlipState m_slip;
    double m_accFfb = 0.0;
    // rF2's own sign for the FFB value isn't documented. It is worked out
    // from the data instead: the self-aligning force pushes the wheel back
    // AGAINST the steering, so steering x force summed over real steering
    // inputs comes out negative when + already means "pushing right".
    double m_ffbSignEvidence = 0.0;
    float m_ffbSign = 1.0f;
    bool m_ffbSignLogged = false;
    int m_accCount = 0;

    // Diagnostic only: measures the real telemetry callback rate so the
    // averaging window size is based on fact, not assumption. Logged once.
    int m_rateLogCallCount = 0;
    LARGE_INTEGER m_rateLogStart = {};
    bool m_rateLogged = false;
};

extern PedalTraceStore g_pedalTrace;

// Must be declared at global scope: declaring it inside the namespace below
// would create a distinct PedalTrace::Renderer type instead of referring to
// the real one.
class Renderer;

namespace PedalTrace
{
    // Draws into the current frame. Returns false if there is nothing to show.
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight);
}
