#include "PedalTrace.h"
#include "FfbGraph.h"
#include "InternalsPlugin.hpp"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "RenderThrottle.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <cstring>

PedalTraceStore g_pedalTrace;

namespace
{
    // Layout, colors, and slip thresholds now live in g_pedalConfig
    // (Config.h/.cpp), loaded from rF2RadarOverlay.ini next to the DLL. Sample
    // rate stays here since the history buffer's fixed size (PedalTrace.h) is
    // tied to it at compile time.

    // Sample at a fixed rate so the horizontal axis is real time regardless of
    // how often the game happens to call the telemetry callback. Matched to your
    // 144Hz refresh so the trace updates every rendered frame, not just some of
    // them.
    const double kSampleIntervalSec = 1.0 / 144.0;
}

void PedalTraceStore::Init()
{
    if (m_ready)
        return;
    InitializeCriticalSection(&m_lock);
    QueryPerformanceFrequency(&m_freq);
    m_ready = true;
}

void PedalTraceStore::Update(const TelemInfoV01& info)
{
    if (!m_ready)
        return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // Diagnostic: measure the real telemetry callback rate over the first
    // second, logged once, so the accumulation window below is based on a
    // measured fact rather than an assumption about physics rate.
    if (!m_rateLogged)
    {
        if (m_rateLogStart.QuadPart == 0)
            m_rateLogStart = now;
        ++m_rateLogCallCount;
        const double elapsed = static_cast<double>(now.QuadPart - m_rateLogStart.QuadPart) / static_cast<double>(m_freq.QuadPart);
        if (elapsed >= 1.0)
        {
            Log::Write("PedalTrace: measured telemetry callback rate = %.1f Hz (vs %.0fHz commit rate)",
                       m_rateLogCallCount / elapsed, 1.0 / kSampleIntervalSec);
            m_rateLogged = true;
        }
    }

    const double lockDeadzone = g_pedalConfig.slipDeadzone;
    const double lockFullScale = g_pedalConfig.slipFullScale;
    const double spinDeadzone = g_pedalConfig.spinDeadzone;
    const double spinFullScale = g_pedalConfig.spinFullScale;
    const double minSpeedForSlip = g_pedalConfig.minSpeedForSlipMs;
    const int spinAxle = g_pedalConfig.spinAxle;

    // Worst-wheel lockup and spin for THIS callback.
    double worstLock = 0.0;
    double worstSpin = 0.0;

    // Raw (unscaled) slip alongside, for SlipState -- see PedalTrace.h.
    double rawWorstLock = 0.0;
    double rawWorstSpin = 0.0;
    double rawFrontLockSum = 0.0;
    int rawFrontCount = 0;
    bool anyWheelMeasured = false;
    double rawLockByWheel[4] = { 0.0, 0.0, 0.0, 0.0 };
    for (int w = 0; w < 4; ++w)
    {
        const TelemWheelV01& wheel = info.mWheel[w];

        const double radius = static_cast<double>(wheel.mStaticUndeflectedRadius) * 0.01; // cm -> m
        if (radius <= 0.0)
            continue;

        const double ground = fabs(wheel.mLongitudinalGroundVel);
        if (ground < minSpeedForSlip)
            continue;

        const double wheelSurface = fabs(wheel.mRotation) * radius;
        const double slip = (wheelSurface - ground) / ground;

        anyWheelMeasured = true;
        {
            const bool isRearWheel = (w >= 2);
            if (slip < 0.0)
            {
                rawLockByWheel[w] = -slip;
                if (-slip > rawWorstLock) rawWorstLock = -slip;
                if (!isRearWheel) { rawFrontLockSum += -slip; ++rawFrontCount; }
            }
            else if (!isRearWheel)
            {
                // A front wheel not locking still counts toward the front
                // average -- otherwise one wheel with a trace of lock would
                // read as the whole axle being at that level.
                ++rawFrontCount;
            }
            const bool drivenWheel = (spinAxle == 0)
                                  || (spinAxle == 1 && isRearWheel)
                                  || (spinAxle == 2 && !isRearWheel);
            if (drivenWheel && slip > rawWorstSpin) rawWorstSpin = slip;
        }

        const double intensityMul = g_pedalConfig.slipIntensityMultiplier;
        if (slip < -lockDeadzone)
        {
            // Wheel turning slower than the road under it -> locking. Every
            // wheel is checked: any of them can lock under braking.
            const double span = (lockFullScale - lockDeadzone);
            if (span > 0.0)
            {
                const double mag = (-slip - lockDeadzone) / span * intensityMul;
                if (mag > worstLock) worstLock = mag;
            }
        }
        else if (slip > spinDeadzone)
        {
            // Wheel turning faster than the road -> spinning. Only the DRIVEN
            // axle can do this under power, so an undriven wheel reading
            // positive slip is measurement noise, not wheelspin. Wheel order
            // is fixed by the SDK: 0=FL, 1=FR, 2=RL, 3=RR.
            const bool isRear = (w >= 2);
            const bool driven = (spinAxle == 0) || (spinAxle == 1 && isRear) || (spinAxle == 2 && !isRear);
            const double span = (spinFullScale - spinDeadzone);
            if (driven && span > 0.0)
            {
                const double mag = (slip - spinDeadzone) / span * intensityMul;
                if (mag > worstSpin) worstSpin = mag;
            }
        }
    }
    if (worstLock > 1.0) worstLock = 1.0;
    if (worstSpin > 1.0) worstSpin = 1.0;

    // "Active" = moving OR a pedal is pressed. Requiring BOTH stationary AND
    // off-pedals to count as idle means a normal lift-and-coast mid-corner
    // (moving, no pedal input) never triggers the fade -- only a genuine stop
    // (pits, off track, back at the menu) does.
    const double kPedalDeadzone = 0.02;
    const double kStationarySpeedMs = 0.3;
    const double speed = sqrt(info.mLocalVel.x * info.mLocalVel.x +
                              info.mLocalVel.y * info.mLocalVel.y +
                              info.mLocalVel.z * info.mLocalVel.z);
    const bool moving = speed > kStationarySpeedMs;
    const bool pedalPressed = (info.mUnfilteredThrottle > kPedalDeadzone) || (info.mUnfilteredBrake > kPedalDeadzone);

    EnterCriticalSection(&m_lock);

    if (moving || pedalPressed)
        m_lastActiveTime = now;

    m_slip.valid = anyWheelMeasured;
    m_slip.worstLock = rawWorstLock;
    m_slip.frontAvgLock = (rawFrontCount > 0) ? (rawFrontLockSum / rawFrontCount) : 0.0;
    m_slip.worstSpin = rawWorstSpin;
    m_slip.brakeInput = info.mUnfilteredBrake;
    for (int w = 0; w < 4; ++w) m_slip.perWheelLock[w] = rawLockByWheel[w];

    // Fold this callback into the running window. Averaged for the pedal
    // channels; peak-held for lockup/spin so a brief slip event between
    // 144Hz commits can't be missed entirely.
    m_accThrottleRaw += info.mUnfilteredThrottle;
    m_accThrottleActual += info.mFilteredThrottle;
    m_accBrakeRaw += info.mUnfilteredBrake;
    m_accBrakeActual += info.mFilteredBrake;
    if (worstLock > m_accLockupPeak) m_accLockupPeak = worstLock;
    if (worstSpin > m_accSpinPeak) m_accSpinPeak = worstSpin;
    {
        const double ffb = g_ffbSpectrum.Latest();
        const double steer = info.mUnfilteredSteering;
        // Steering is a fraction of full lock: fast corners only need
        // 0.02-0.05, so the threshold has to sit well under that.
        if (fabs(steer) > 0.01 && fabs(ffb) > 0.05)
        {
            m_ffbSignEvidence += steer * ffb;
            // Adopted only once the evidence is clear -- near zero it flips
            // sample to sample, which flipped the trace with it.
            if (fabs(m_ffbSignEvidence) > 50.0)
            {
                const float sign = (m_ffbSignEvidence > 0.0) ? -1.0f : 1.0f;
                if (!m_ffbSignLogged || sign != m_ffbSign)
                {
                    m_ffbSignLogged = true;
                    Log::Write("PedalTrace: FFB sign worked out as %s (steering x force evidence %.0f)",
                               sign > 0.0f ? "as-is" : "inverted", m_ffbSignEvidence);
                }
                m_ffbSign = sign;
            }
        }
        m_accFfb += ffb * m_ffbSign;
    }
    ++m_accCount;

    // Commit at a fixed 144Hz rate regardless of how much faster the game
    // actually calls this -- everything accumulated since the last commit
    // gets folded into one sample rather than discarded.
    bool shouldCommit = true;
    if (m_lastSample.QuadPart != 0 && m_freq.QuadPart != 0)
    {
        const double dt = static_cast<double>(now.QuadPart - m_lastSample.QuadPart) / static_cast<double>(m_freq.QuadPart);
        shouldCommit = dt >= kSampleIntervalSec;
    }

    if (shouldCommit && m_accCount > 0)
    {
        m_lastSample = now;

        Sample& s = m_samples[m_head];
        s.throttleRaw = static_cast<float>(m_accThrottleRaw / m_accCount);
        s.throttleActual = static_cast<float>(m_accThrottleActual / m_accCount);
        s.brakeRaw = static_cast<float>(m_accBrakeRaw / m_accCount);
        s.brakeActual = static_cast<float>(m_accBrakeActual / m_accCount);
        s.lockup = static_cast<float>(m_accLockupPeak);
        s.spin = static_cast<float>(m_accSpinPeak);
        s.ffb = static_cast<float>(m_accFfb / m_accCount);

        m_head = (m_head + 1) % kHistory;
        ++m_total;
        if (m_count < kHistory)
            ++m_count;

        m_accThrottleRaw = m_accThrottleActual = m_accBrakeRaw = m_accBrakeActual = 0.0;
        m_accLockupPeak = m_accSpinPeak = 0.0;
        m_accFfb = 0.0;
        m_accCount = 0;
    }

    LeaveCriticalSection(&m_lock);
}

int PedalTraceStore::ReadNew(Sample* ring, long long& total)
{
    if (!m_ready)
        return 0;

    EnterCriticalSection(&m_lock);

    const long long newest = m_total;
    const int count = m_count;
    // Held now: numbers newest - count .. newest - 1, the last at slot
    // m_head - 1. Anything the caller has from before that range is gone
    // from here anyway (a Clear, or more than kHistory samples since).
    const long long oldest = newest - count;
    long long from = total;
    if (from < oldest || from > newest)
        from = oldest;
    int slot = static_cast<int>((m_head - (newest - from) + kHistory * 2) % kHistory);
    int dst = static_cast<int>(from % kHistory);
    for (long long t = from; t < newest; ++t)
    {
        ring[dst] = m_samples[slot];
        if (++slot == kHistory) slot = 0;
        if (++dst == kHistory) dst = 0;
    }

    LeaveCriticalSection(&m_lock);
    total = newest;
    return count;
}

PedalTraceStore::SlipState PedalTraceStore::ReadSlip()
{
    SlipState copy;
    if (!m_ready) return copy;
    EnterCriticalSection(&m_lock);
    copy = m_slip;
    LeaveCriticalSection(&m_lock);
    return copy;
}

double PedalTraceStore::SecondsSinceActive()
{
    if (!m_ready)
        return 1e9; // never initialized -- treat as long since active

    EnterCriticalSection(&m_lock);
    const LARGE_INTEGER lastActive = m_lastActiveTime;
    LeaveCriticalSection(&m_lock);

    if (lastActive.QuadPart == 0 || m_freq.QuadPart == 0)
        return 1e9; // never been active yet

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - lastActive.QuadPart) / static_cast<double>(m_freq.QuadPart);
}

void PedalTraceStore::Clear()
{
    if (!m_ready)
        return;
    EnterCriticalSection(&m_lock);
    m_head = 0;
    m_count = 0;
    LeaveCriticalSection(&m_lock);
}

namespace
{
    // 0 throttle raw, 1 throttle actual, 2 brake raw, 3 brake actual.
    float ChannelValue(const PedalTraceStore::Sample& s, int channel)
    {
        switch (channel)
        {
        case 0: return s.throttleRaw;
        case 1: return s.throttleActual;
        case 2: return s.brakeRaw;
        default: return s.brakeActual;
        }
    }

    // The points a pedal channel's line is drawn through, into 'keep';
    // returns how many. Flat runs (a pedal resting at 0% or held at 100%)
    // become one segment: a point equal to the points either side of it adds
    // nothing.
    int ThinTrace(const PedalTraceStore::Sample* samples, int count, int channel, int* keep)
    {
        int n = 0;
        keep[n++] = 0;
        int prev = 0;
        for (int i = 1; i < count; ++i)
        {
            const float v = ChannelValue(samples[i], channel);
            if (i < count - 1 && v == ChannelValue(samples[prev], channel)
                && ChannelValue(samples[i + 1], channel) == v)
                continue;
            keep[n++] = i;
            prev = i;
        }
        return n;
    }

    // Draws one pedal channel through its kept points.
    //
    // 'coverNext' (the post-electronics lines, while the panel is fully
    // opaque): the raw line drawn later on top of this one, as the next kept
    // point after each of its kept points (-1 elsewhere). Where that line has
    // the very same segment -- the same two points at the same values, so the
    // TC or ABS did nothing there -- its solid core is the identical quad at
    // full alpha, which hides this line's core pixel for pixel. So only this
    // line's soft edge is sent there, the halo pass of AddLineSmooth alone:
    // the raw line's own edge is translucent and that one still shows
    // through it. With no TC or ABS working that is every segment of both
    // lines, half their vertices. (AddPolylineSmooth was not used instead: it is 18
    // vertices a segment against AddLineSmooth's 12, and its mitred corners
    // pinch the sharp spikes these lines are full of.)
    void DrawTrace(Renderer& renderer, const PedalTraceStore::Sample* samples, const float* xs,
                   const int* keep, int keepCount, float yBottom, float plotH, float thickness,
                   float r, float g, float b, float a, int channel,
                   const int* coverNext = nullptr, int coverChannel = 0)
    {
        for (int k = 1; k < keepCount; ++k)
        {
            const int prev = keep[k - 1];
            const int i = keep[k];
            const float v1 = ChannelValue(samples[prev], channel);
            const float v2 = ChannelValue(samples[i], channel);
            const float x1 = xs[prev];
            const float x2 = xs[i];
            const float y1 = yBottom - v1 * plotH;
            const float y2 = yBottom - v2 * plotH;
            const bool covered = coverNext && coverNext[prev] == i
                && ChannelValue(samples[prev], coverChannel) == v1
                && ChannelValue(samples[i], coverChannel) == v2;
            if (covered)
                renderer.AddLine(x1, y1, x2, y2, thickness + 2.0f, r, g, b, a * 0.33f);
            else
                renderer.AddLineSmooth(x1, y1, x2, y2, thickness, r, g, b, a);
        }
    }
}

bool PedalTrace::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    if (!g_pedalConfig.enabled)
        return false;

    // Not throttled (RenderThrottle) like the FFB graph: a scrolling trace
    // replayed between rebuilds moves in uneven steps at 144 fps, which read
    // as the lines jumping and flickering (tried in 26.11.22.5/.6).

    // The render thread's own copy of the history, kept between frames so
    // each frame copies under the telemetry lock only what is new (it used to
    // copy all 1440 samples). Sample number n is at ring[n % kHistory].
    static PedalTraceStore::Sample ring[PedalTraceStore::kHistory];
    static long long s_total = 0;
    const int rawCount = g_pedalTrace.ReadNew(ring, s_total);
    const long long total = s_total;
    if (rawCount < 2)
        return false;

    const PedalConfig& cfg = g_pedalConfig;

    // Thinned to about one point per PointSpacingPixels of plot width (10 s
    // at 144 Hz is 1440 samples, more than the plot has pixels). Buckets are
    // fixed to ABSOLUTE sample numbers and each point keeps its own sample's
    // x, so a point keeps its shape as it scrolls left. Buckets counted back
    // from the newest sample (as before 26.11.22.8) moved every boundary with
    // each new sample and picked a different sample for every point, so the
    // whole line's shape wobbled while it scrolled. Each point is the last
    // sample of its bucket (plus the newest sample); lockup/spin take the
    // bucket's peak so a brief slip still shows.
    static PedalTraceStore::Sample samples[PedalTraceStore::kHistory];
    static int rawIdx[PedalTraceStore::kHistory];
    int count = 0;
    int stride = 1;
    {
        const float plotW = screenWidth * cfg.widthFraction * cfg.scale;
        const float spacing = cfg.pointSpacingPx > 0.5f ? cfg.pointSpacingPx : 0.5f;
        const int maxPoints = (std::max)(2, static_cast<int>(plotW / spacing) + 1);
        stride = (std::max)(1, (PedalTraceStore::kHistory + maxPoints - 1) / maxPoints);
        // Raw sample i (0 = oldest held) is absolute number first + i, at
        // ring[ri]; 'phase' is (first + i + 1) % stride, kept by counting
        // rather than dividing for every sample.
        const long long first = total - rawCount;
        int ri = static_cast<int>(first % PedalTraceStore::kHistory);
        int phase = static_cast<int>((first + 1) % stride);
        PedalTraceStore::Sample acc = ring[ri];
        for (int i = 0; i < rawCount; ++i)
        {
            const PedalTraceStore::Sample& s = ring[ri];
            const int next = (ri + 1 == PedalTraceStore::kHistory) ? 0 : ri + 1;
            const bool bucketEnd = phase == 0 || i == rawCount - 1;
            phase = (phase + 1 == stride) ? 0 : phase + 1;
            ri = next;
            if (s.lockup > acc.lockup) acc.lockup = s.lockup;
            if (s.spin > acc.spin) acc.spin = s.spin;
            if (!bucketEnd) continue;
            PedalTraceStore::Sample out = s;
            out.lockup = acc.lockup;
            out.spin = acc.spin;
            samples[count] = out;
            rawIdx[count] = i;
            ++count;
            if (i + 1 < rawCount) acc = ring[next];
        }
        if (count < 2)
            return false;
    }

    // Idle fade: render-thread-local state, advanced by real wall-clock time so
    // it keeps fading even if Update() stops being called entirely (menu).
    static float s_displayAlpha = 0.0f;
    static LARGE_INTEGER s_lastDrawTime = {};
    static LARGE_INTEGER s_freq = {};
    if (s_freq.QuadPart == 0)
        QueryPerformanceFrequency(&s_freq);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double frameDt = 0.0;
    if (s_lastDrawTime.QuadPart != 0 && s_freq.QuadPart != 0)
    {
        frameDt = static_cast<double>(now.QuadPart - s_lastDrawTime.QuadPart) / static_cast<double>(s_freq.QuadPart);
        if (frameDt < 0.0 || frameDt > 1.0) frameDt = 0.0; // sanity clamp (pause, first call)
    }
    s_lastDrawTime = now;

    // "Currently active" = the most recent sample was moving or on the pedals.
    // A couple of sample-intervals' worth of tolerance avoids flicker between
    // the ~144Hz sample ticks.
    const bool currentlyActive = g_pedalTrace.SecondsSinceActive() < 0.05;
    // A panel you cannot see is a panel you cannot grab, so the idle fade is
    // held off entirely while layout mode is on -- and SNAPPED rather than
    // eased, so it is ready the instant the mode is entered instead of a
    // second later.
    const bool suppressFade = Layout::SuppressFade();
    const float target = (currentlyActive || suppressFade) ? 1.0f : 0.0f;
    if (suppressFade)
        s_displayAlpha = 1.0f;
    const float fadeSeconds = (cfg.idleFadeSeconds > 0.01f) ? cfg.idleFadeSeconds : 0.01f;
    const float step = static_cast<float>(frameDt) / fadeSeconds;

    if (s_displayAlpha < target)
        s_displayAlpha = (s_displayAlpha + step < target) ? s_displayAlpha + step : target;
    else if (s_displayAlpha > target)
        s_displayAlpha = (s_displayAlpha - step > target) ? s_displayAlpha - step : target;

    if (s_displayAlpha <= 0.004f && !suppressFade)
        return false; // fully faded -- nothing to draw

    // cfg.scale is the layout-mode corner-handle multiplier. Folding it
    // in HERE rather than applying it to the drawn box means every pixel
    // value downstream -- text height, padding, gaps, corner radii --
    // scales with it in exact proportion, because they all already
    // derive from this one value.
    const float uiScale = static_cast<float>(screenHeight) / 2160.0f * cfg.scale;
    const float bgCornerRadiusPx = cfg.bgCornerRadiusPx * uiScale;
    const float borderThicknessPx = cfg.borderThicknessPx * uiScale;

    const float w = screenWidth * cfg.widthFraction * cfg.scale;
    const float h = screenHeight * cfg.heightFraction * cfg.scale;
    const float centerX = screenWidth * cfg.centerXFraction;
    const float centerY = screenHeight * cfg.centerYFraction;
    const float x0 = centerX - w * 0.5f;
    const float y0 = centerY - h * 0.5f;
    const float y1 = centerY + h * 0.5f;

    // Reserve thin strips at the top for the lockup / spin indicators.
    const float stripH = h * 0.11f;
    const float plotTop = y0 + stripH * 2.0f;
    const float plotH = (y1 - plotTop) * 0.94f;
    const float yBottom = y1 - (y1 - plotTop) * 0.03f;

    // Background: spans exactly the 0-100% pedal plot area (yBottom = 0%,
    // plotTop = 100%), not the strips above it, so the gray directly represents
    // the pedal-press scale rather than an arbitrary panel height.
    const float bgCenterX = x0 + w * 0.5f;
    const float bgCenterY = (plotTop + yBottom) * 0.5f;
    const float bgHalfW = w * 0.5f;
    const float bgHalfH = (yBottom - plotTop) * 0.5f;
    // Registered with the FULL panel box (strips included), not the plot-only
    // background above -- the strips are part of what you are dragging.
    Layout::Register("Pedal trace", "PedalTrace", "CenterXFraction", "CenterYFraction",
                     &g_pedalConfig.centerXFraction, &g_pedalConfig.centerYFraction,
                     centerX, centerY, w * 0.5f, h * 0.5f,
                     &g_pedalConfig.scale, "Scale");

    renderer.AddFilledRoundedRect(bgCenterX, bgCenterY, bgHalfW, bgHalfH, bgCornerRadiusPx, 0.0f,
                                  cfg.bgR, cfg.bgG, cfg.bgB, cfg.bgA * s_displayAlpha);
    if (cfg.borderEnabled)
    {
        renderer.AddRectOutline(bgCenterX, bgCenterY, bgHalfW, bgHalfH, borderThicknessPx,
                                cfg.borderR, cfg.borderG, cfg.borderB, s_displayAlpha);
    }

    const float dxRaw = w / static_cast<float>(rawCount - 1);
    const float dx = dxRaw * stride;   // width of one bucket, for the slip bars
    static float xs[PedalTraceStore::kHistory];
    for (int i = 0; i < count; ++i) xs[i] = x0 + rawIdx[i] * dxRaw;
    const float thickness = h * 0.035f > 2.0f ? h * 0.035f : 2.0f;

    // Lockup strip (top) and spin strip (just below it). Each bar grows
    // upward from the floor of its strip as magnitude increases -- like a VU
    // meter -- rather than a fixed-size patch that only changes opacity, so
    // "how much" is readable from shape as well as color intensity.
    const float maxBarHalfH = stripH * 0.42f * cfg.slipBarHeightScale;
    const float lockupFloorY = y0 + stripH * 0.92f;
    const float spinFloorY = y0 + stripH * 1.92f;
    for (int i = 0; i < count; ++i)
    {
        const float x = xs[i];
        if (samples[i].lockup > 0.0f)
        {
            const float halfH = maxBarHalfH * samples[i].lockup;
            renderer.AddFilledRect(x, lockupFloorY - halfH, dx * 0.6f, halfH, 0.0f,
                                   cfg.lockupR, cfg.lockupG, cfg.lockupB, samples[i].lockup * s_displayAlpha);
        }
        if (samples[i].spin > 0.0f)
        {
            const float halfH = maxBarHalfH * samples[i].spin;
            renderer.AddFilledRect(x, spinFloorY - halfH, dx * 0.6f, halfH, 0.0f,
                                   cfg.spinR, cfg.spinG, cfg.spinB, samples[i].spin * s_displayAlpha);
        }
    }

    // Actual (post-electronics) underneath, raw input on top of it. The raw
    // lines' points are worked out first, so the actual lines can leave out
    // the core they would hide (see DrawTrace) -- only while the panel is
    // fully opaque, since mid-fade the raw core no longer hides anything.
    static int s_keep[4][PedalTraceStore::kHistory];
    int keepCount[4];
    for (int ch = 0; ch < 4; ++ch)
        keepCount[ch] = ThinTrace(samples, count, ch, s_keep[ch]);
    const bool opaque = s_displayAlpha >= 1.0f;
    static int s_coverNext[2][PedalTraceStore::kHistory];
    for (int p = 0; p < 2 && opaque; ++p)
    {
        const int rawCh = p * 2;   // 0 throttle, 2 brake
        int* next = s_coverNext[p];
        std::fill(next, next + count, -1);
        for (int k = 1; k < keepCount[rawCh]; ++k)
            next[s_keep[rawCh][k - 1]] = s_keep[rawCh][k];
    }
    DrawTrace(renderer, samples, xs, s_keep[1], keepCount[1], yBottom, plotH, thickness,
              cfg.throttleActR, cfg.throttleActG, cfg.throttleActB, s_displayAlpha, 1,
              opaque ? s_coverNext[0] : nullptr, 0);
    DrawTrace(renderer, samples, xs, s_keep[3], keepCount[3], yBottom, plotH, thickness,
              cfg.brakeActR, cfg.brakeActG, cfg.brakeActB, s_displayAlpha, 3,
              opaque ? s_coverNext[1] : nullptr, 2);
    DrawTrace(renderer, samples, xs, s_keep[0], keepCount[0], yBottom, plotH, thickness,
              cfg.throttleRawR, cfg.throttleRawG, cfg.throttleRawB, s_displayAlpha, 0);
    DrawTrace(renderer, samples, xs, s_keep[2], keepCount[2], yBottom, plotH, thickness,
              cfg.brakeRawR, cfg.brakeRawG, cfg.brakeRawB, s_displayAlpha, 2);

    // Force feedback (replaced the steering line), bipolar (-1..1) across
    // the full plot height with 0 at the vertical centre: up = the wheel
    // being pushed right, down = left. Full height either way = 100%, so a
    // line pinned to the top or bottom edge is clipping.
    for (int i = 1; i < count; ++i)
    {
        const float x1 = xs[i - 1];
        const float x2 = xs[i];
        const float y1 = yBottom - (samples[i - 1].ffb * 0.5f + 0.5f) * plotH;
        const float y2 = yBottom - (samples[i].ffb * 0.5f + 0.5f) * plotH;
        renderer.AddLineSmooth(x1, y1, x2, y2, thickness, cfg.steeringR, cfg.steeringG, cfg.steeringB, s_displayAlpha);
    }

    return true;
}
