#include "GearDisplay.h"
#include "FfbGraph.h"
#include "InternalsPlugin.hpp"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "RealtimeState.h"
#include "PedalTrace.h"
#include "PitEntry.h"
#include "Shortcuts.h"
#include "Log.h"
#include "RenderThrottle.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#pragma comment(lib, "winmm.lib")

GearDisplayStore g_gearDisplay;

namespace
{
    // Same DLL-relative-path resolution used elsewhere (Config.cpp,
    // LapDelta.cpp), so the saved pit-limit files live next to the INI.
    void GetPitLimitDir(char* out, size_t outSize)
    {
        char dllPath[MAX_PATH] = {};
        HMODULE hModule = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCSTR>(&GetPitLimitDir), &hModule);
        GetModuleFileNameA(hModule, dllPath, MAX_PATH);

        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';

        sprintf_s(out, outSize, "%sPitLimits", dllPath);
    }

    void GetShiftPointDir(char* out, size_t outSize)
    {
        char dllPath[MAX_PATH] = {};
        HMODULE hModule = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCSTR>(&GetShiftPointDir), &hModule);
        GetModuleFileNameA(hModule, dllPath, MAX_PATH);

        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';

        sprintf_s(out, outSize, "%sShiftPoints", dllPath);
    }

    // maxLen bounds the scan to the SDK's own fixed buffer size (see the call
    // sites) -- mVehicleName/mTrackName are plain char[64] fields with no
    // documented guarantee of null-termination if a name exactly fills the
    // buffer, so scanning for a '\0' without a bound could read past the end
    // of the struct.
    std::string Sanitize(const char* s, size_t maxLen)
    {
        std::string out;
        for (size_t i = 0; i < maxLen && s[i]; ++i)
        {
            const char c = s[i];
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
            out.push_back(ok ? c : '_');
        }
        if (out.empty()) out = "unknown";
        return out;
    }

    const int kBeepSampleRate = 44100;
}

void GearDisplayStore::Init()
{
    if (m_ready)
        return;
    InitializeCriticalSection(&m_lock);

    // One long beep: speed has dropped to/under the pit-lane limit while
    // approaching a pit stop, but the limiter isn't engaged yet.
    {
        const int freq = g_pitLimiterConfig.engageBeepFrequencyHz;
        // Clamped: user-editable in the INI, and a negative value would wrap
        // around to a huge size once cast to the unsigned byte count below
        // (new[] would then try to allocate ~4GB and throw, uncaught, straight
        // out of Startup() -- crashing the game on load rather than the
        // overlay alone). A large-but-still-positive value could also
        // overflow the plain int multiplication below before the clamp
        // existed.
        int engageBeepDurationMs = g_pitLimiterConfig.engageBeepDurationMs;
        if (engageBeepDurationMs < 0) engageBeepDurationMs = 0;
        if (engageBeepDurationMs > 10000) engageBeepDurationMs = 10000;
        const int toneSamples = (kBeepSampleRate * engageBeepDurationMs) / 1000;
        const int fadeSamples = kBeepSampleRate / 200; // 5ms fade in/out, avoids a click
        const int sampleCount = toneSamples;

        const DWORD dataBytes = static_cast<DWORD>(sampleCount) * 2; // 16-bit mono
        const DWORD totalSize = 44 + dataBytes;

        m_engageWavBuffer = new unsigned char[totalSize];
        unsigned char* p = m_engageWavBuffer;

        auto writeU32 = [&](DWORD v) { memcpy(p, &v, 4); p += 4; };
        auto writeU16 = [&](WORD v) { memcpy(p, &v, 2); p += 2; };

        memcpy(p, "RIFF", 4); p += 4;
        writeU32(36 + dataBytes);
        memcpy(p, "WAVE", 4); p += 4;

        memcpy(p, "fmt ", 4); p += 4;
        writeU32(16);
        writeU16(1);
        writeU16(1);
        writeU32(kBeepSampleRate);
        writeU32(kBeepSampleRate * 2);
        writeU16(2);
        writeU16(16);

        memcpy(p, "data", 4); p += 4;
        writeU32(dataBytes);

        auto writeTone = [&](int samples)
        {
            for (int i = 0; i < samples; ++i)
            {
                double amp = 1.0;
                if (i < fadeSamples) amp = static_cast<double>(i) / fadeSamples;
                else if (i > samples - fadeSamples) amp = static_cast<double>(samples - i) / fadeSamples;

                const double t = static_cast<double>(i) / kBeepSampleRate;
                const double sample = amp * sin(2.0 * 3.14159265358979 * freq * t);
                const short pcm = static_cast<short>(sample * 32000.0);
                memcpy(p, &pcm, 2); p += 2;
            }
        };
        writeTone(toneSamples);

        m_engageWavSize = totalSize;
    }

    m_ready = true;
}

void GearDisplayStore::PlayEngageBeep()
{
    if (!m_engageWavBuffer)
        return;
    PlaySoundA(reinterpret_cast<LPCSTR>(m_engageWavBuffer), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void GearDisplayStore::LoadPitLimit(const char* vehicleName, const char* trackName)
{
    char dir[MAX_PATH];
    GetPitLimitDir(dir, sizeof(dir));
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s.txt", dir, m_currentKey.c_str());

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f)
    {
        m_state.learnedPitLimitKmh = 0.0;
        Log::Write("GearDisplay: no saved pit limit yet for %s / %s", vehicleName, trackName);
        return;
    }
    char line[64];
    const double loaded = fgets(line, sizeof(line), f) ? atof(line) : 0.0;
    fclose(f);

    // Guards against a previously-saved bogus reading (e.g. the limiter
    // being toggled while stationary, before this was fixed at the source in
    // Update()) -- no real pit limit is this low, so treat it the same as
    // "nothing saved yet" rather than propagating garbage forever.
    constexpr double kMinPlausiblePitLimitKmh = 20.0;
    if (loaded < kMinPlausiblePitLimitKmh)
    {
        m_state.learnedPitLimitKmh = 0.0;
        Log::Write("GearDisplay: ignoring implausible saved pit limit %.2f km/h for %s / %s", loaded, vehicleName, trackName);
        return;
    }

    m_state.learnedPitLimitKmh = loaded;
    Log::Write("GearDisplay: loaded pit limit %.1f km/h for %s / %s", m_state.learnedPitLimitKmh, vehicleName, trackName);
}

void GearDisplayStore::SavePitLimit() const
{
    char dir[MAX_PATH];
    GetPitLimitDir(dir, sizeof(dir));
    CreateDirectoryA(dir, nullptr); // no-op (ERROR_ALREADY_EXISTS) if it's already there

    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s.txt", dir, m_currentKey.c_str());

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f)
    {
        Log::Write("GearDisplay: failed to save pit limit to %s", path);
        return;
    }
    fprintf(f, "%.2f\n", m_state.learnedPitLimitKmh);
    fclose(f);
    Log::Write("GearDisplay: saved pit limit %.1f km/h to %s", m_state.learnedPitLimitKmh, path);
}

void GearDisplayStore::LoadShiftPoint(const char* vehicleName)
{
    m_state.learnedShiftRpmFraction = 0.0;

    char dir[MAX_PATH];
    GetShiftPointDir(dir, sizeof(dir));
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s.txt", dir, m_shiftKey.c_str());

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f)
    {
        Log::Write("GearDisplay: no learned shift point yet for %s", vehicleName);
        return;
    }
    // Line 1 is the car-wide value; each following line is one gear,
    // starting at 1st. Files written before the per-gear split have only
    // the first line, which still loads correctly -- the per-gear values
    // stay 0 and fall back to it.
    char line[64];
    m_state.learnedShiftRpmFraction = fgets(line, sizeof(line), f) ? atof(line) : 0.0;
    for (int g = 1; g < State::kMaxGears; ++g)
    {
        if (!fgets(line, sizeof(line), f)) break;
        m_state.learnedShiftRpmFractionByGear[g] = atof(line);
    }
    fclose(f);
    Log::Write("GearDisplay: loaded shift point %.3f of max RPM for %s", m_state.learnedShiftRpmFraction, vehicleName);
}

void GearDisplayStore::SaveShiftPoint() const
{
    char dir[MAX_PATH];
    GetShiftPointDir(dir, sizeof(dir));
    CreateDirectoryA(dir, nullptr); // no-op (ERROR_ALREADY_EXISTS) if it's already there

    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s.txt", dir, m_shiftKey.c_str());

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f)
    {
        Log::Write("GearDisplay: failed to save shift point to %s", path);
        return;
    }
    fprintf(f, "%.4f\n", m_state.learnedShiftRpmFraction);
    for (int g = 1; g < State::kMaxGears; ++g)
        fprintf(f, "%.4f\n", m_state.learnedShiftRpmFractionByGear[g]);
    fclose(f);
}

void GearDisplayStore::Update(const TelemInfoV01& info)
{
    if (!m_ready)
        return;

    const double speed = sqrt(info.mLocalVel.x * info.mLocalVel.x +
                              info.mLocalVel.y * info.mLocalVel.y +
                              info.mLocalVel.z * info.mLocalVel.z);
    const bool limiterOn = info.mSpeedLimiter != 0;

    EnterCriticalSection(&m_lock);

    // Vehicle CLASS, shared from the scoring thread -- see RealtimeState.h.
    char vehKey[64];
    VehicleKey::Get(vehKey, sizeof(vehKey));
    const std::string key = Sanitize(vehKey, sizeof(vehKey)) + "__" + Sanitize(info.mTrackName, sizeof(info.mTrackName));
    if (key != m_currentKey)
    {
        m_currentKey = key;
        LoadPitLimit(vehKey, info.mTrackName);
        m_lastSpeedLimiterOn = false;
        m_episodeMaxKmh = 0.0;
    }

    m_state.gear = static_cast<int>(info.mGear);
    m_state.rpm = info.mEngineRPM;
    m_state.maxRpm = info.mEngineMaxRPM;
    m_state.speedMs = speed;
    m_state.speedLimiterOn = limiterOn;

    // RPM rate-of-climb, from consecutive samples' own elapsed time -- the
    // only thing this feeds is turning the "shift now" cue's reaction-time
    // lead (seconds) into an RPM offset below. Only the climbing case
    // matters (a lift, engine braking or overrun should never make the cue
    // fire EARLIER), so a negative rate is floored to zero before it is
    // smoothed -- which also stops the RPM drop through a gear change from
    // dragging the smoothed value negative and leaving the next gear's cue
    // with no lead at all.
    if (m_lastTimeForRate >= 0.0)
    {
        const double dtRate = info.mElapsedTime - m_lastTimeForRate;
        if (dtRate > 0.0001 && dtRate < 1.0)
        {
            double rawRate = (info.mEngineRPM - m_lastRpmForRate) / dtRate;
            if (rawRate < 0.0) rawRate = 0.0;
            constexpr double kRateSmoothing = 0.2; // ~5 samples of time constant at this callback rate
            m_rpmRateSmoothed = m_rpmRateSmoothed * (1.0 - kRateSmoothing) + rawRate * kRateSmoothing;
        }
    }
    m_lastRpmForRate = info.mEngineRPM;
    m_lastTimeForRate = info.mElapsedTime;
    const double rpmRate = (m_rpmRateSmoothed > 0.0) ? m_rpmRateSmoothed : 0.0;

    // ABS/TC "active right now" isn't a direct telemetry field -- inferred
    // from the filtered (post-electronics) brake/throttle sitting
    // meaningfully below what was actually pressed.
    const double absCut = info.mUnfilteredBrake - info.mFilteredBrake;
    const double tcCut = info.mUnfilteredThrottle - info.mFilteredThrottle;

    // rF2 cuts throttle through this SAME filtered channel to make a gear
    // change (the auto-lift / ignition cut), which is indistinguishable from
    // TC intervening -- so this used to flash yellow on every single upshift.
    // Ignore the throttle half briefly after any gear change. The brake half
    // (real ABS) is never suppressed: shifting does not touch it.
    if (info.mGear != m_lastGearForTcCut)
    {
        m_lastGearForTcCut = info.mGear;
        m_lastGearChangeET = info.mElapsedTime;
    }
    const double sinceShift = info.mElapsedTime - m_lastGearChangeET;
    const bool tcTrustworthy = (m_lastGearChangeET < 0.0)
                             || (sinceShift > g_gearDisplayConfig.tcIgnoreAfterShiftSeconds);

    // ABS: rF2 does not apply it to the filtered pedal (absCut above stays
    // ~0), it releases brake PRESSURE at the locking wheel. Each wheel's
    // pressure is normally exactly pedal x its share of the brake bias; ABS
    // shows as one wheel dipping well under that while another wheel is
    // still at its normal level. (All four lagging together is just the
    // pressure catching up with a fast pedal stab, not ABS.) Confirmed from
    // a logged test, 2026-10-21: dips to 10-30% of expected under ABS.
    bool absFromPressure = false;
    if (info.mUnfilteredBrake > 0.05)
    {
        const double bias = info.mRearBrakeBias;
        double ratio[4];
        for (int w = 0; w < 4; ++w)
        {
            const double share = (w < 2) ? (1.0 - bias) : bias;
            const double expected = info.mUnfilteredBrake * share;
            ratio[w] = (expected > 0.02) ? info.mWheel[w].mBrakePressure / expected : 1.0;
        }
        double lo = ratio[0], hi = ratio[0];
        for (int w = 1; w < 4; ++w) { lo = (std::min)(lo, ratio[w]); hi = (std::max)(hi, ratio[w]); }
        absFromPressure = lo < 0.85 && hi > 0.95;
    }
    m_state.absActive = absFromPressure || (absCut > g_gearDisplayConfig.absTcThreshold);

    m_state.tcActive = (tcTrustworthy && tcCut > g_gearDisplayConfig.absTcThreshold);

    // --- Slip feedback for the speed box ---
    {
        const GearDisplayConfig& gc = g_gearDisplayConfig;
        const PedalTraceStore::SlipState slip = g_pedalTrace.ReadSlip();

        int candidate = 0;
        if (gc.slipFeedbackEnabled && slip.valid)
        {
            // Spin is tested first, and independently of the brake gate: it
            // happens under power, where the brake is by definition not
            // pressed.
            if (slip.worstSpin > gc.spinSlipThreshold)
                candidate = 2;
            else if (slip.brakeInput > gc.slipFeedbackBrakeInput
                     && slip.worstLock > gc.lockSlipThreshold)
                candidate = 1;
        }

        const double nowET = info.mElapsedTime;
        if (candidate != m_slipCandidate)
        {
            m_slipCandidate = candidate;
            m_slipCandidateSince = nowET;
        }

        const bool dwellMet = (m_slipCandidateSince >= 0.0)
                           && (nowET - m_slipCandidateSince >= gc.slipFeedbackDwellSeconds);
        const bool holdExpired = (m_slipShownSince < 0.0)
                              || (nowET - m_slipShownSince >= gc.slipFeedbackHoldSeconds);

        if (dwellMet && candidate != m_slipShown && (holdExpired || candidate != 0))
        {
            // Entering a fault does not wait for the previous state's hold to
            // finish -- that is exactly the moment the driver needs to know.
            // Only CLEARING waits, so a brief lockup stays visible.
            m_slipShown = candidate;
            m_slipShownSince = nowET;
        }
        m_state.slipState = m_slipShown;
    }

    // The limiter caps speed AT the pit-lane limit, so the highest speed
    // reached during one continuous "limiter engaged" episode converges to
    // that real value -- PROVIDED it was actually held there rather than
    // caught mid-transient. Two cases:
    //  - Leaving the garage/pit box: the episode starts from near-standstill
    //    (limiter engaged before the car is even moving), so the whole climb
    //    to the cap is genuine and the peak can be trusted outright -- this
    //    always resets the saved value, up or down, since it's the cleanest
    //    possible reading.
    //  - Any other engagement (mid-pit-lane re-engagement, or a race-time
    //    late activation already going faster than the limit): the peak
    //    could just be whatever transient speed the limiter caught the car
    //    at, so it's only ever allowed to refine the saved value DOWNWARD,
    //    never raise it -- a late activation simply gets ignored instead of
    //    corrupting a value already learned cleanly.
    const double speedKmh = speed * 3.6;
    if (limiterOn)
    {
        if (!m_lastSpeedLimiterOn)
            m_episodeStartSpeedKmh = speedKmh; // captured once, right as the episode begins
        if (speedKmh > m_episodeMaxKmh)
            m_episodeMaxKmh = speedKmh;
    }
    else if (m_lastSpeedLimiterOn && m_episodeMaxKmh > 0.0)
    {
        // Toggling the limiter on/off while stationary or barely moving (very
        // common -- most drivers arm it before actually leaving the box)
        // produces an episode whose peak is just physics noise, near zero.
        // No real pit limit is ever that low, so such an episode is ignored
        // outright rather than being accepted as "the cleanest possible
        // reading" just because it also happens to start under the garage-
        // exit threshold -- that's exactly the case this was meant to catch,
        // not this one. This is what previously saved a bogus ~0.01 km/h
        // "limit" that then never got corrected.
        constexpr double kMinPlausiblePitLimitKmh = 20.0;
        if (m_episodeMaxKmh >= kMinPlausiblePitLimitKmh)
        {
            constexpr double kGarageExitSpeedThresholdKmh = 20.0;
            const bool wasGarageExit = m_episodeStartSpeedKmh < kGarageExitSpeedThresholdKmh;
            const bool noBaselineYet = m_state.learnedPitLimitKmh <= 0.0;
            const bool refinesDownward = m_episodeMaxKmh < m_state.learnedPitLimitKmh - 0.3;
            if (wasGarageExit || noBaselineYet || refinesDownward)
            {
                m_state.learnedPitLimitKmh = m_episodeMaxKmh;
                m_pitLimitDirty = true; // written later by FlushIfDirty(), off this (telemetry) thread's critical path
            }
        }
        m_episodeMaxKmh = 0.0;
    }
    m_lastSpeedLimiterOn = limiterOn;

    // "Engage the limiter" reminder: once a pit stop is actually being
    // approached (PitEntryStore::armed) and speed has dropped to/under
    // the pit-lane limit, but the limiter still isn't ON, edge-trigger a
    // beep telling you that you're now slow enough to turn the limiter on,
    // plus a centred on-screen reminder until either the limiter
    // comes on or speed climbs back over the limit.
    {
        const double effectiveLimitKmh = (m_state.learnedPitLimitKmh > 0.0) ? m_state.learnedPitLimitKmh : g_pitLimiterConfig.speedLimitKmh;
        const PitEntryStore::State pbState = g_pitEntry.Read();
        // pbState.armed alone just means a pit stop is REQUESTED, which
        // stays true for the whole lap (or laps) until the stop actually
        // happens -- without also checking distanceToLineM, any slow corner
        // anywhere on track (nowhere near the pits) at/under the pit-lane
        // speed would falsely trigger this reminder.
        const bool nearPitEntry = pbState.distanceToLineM <= g_pitLimiterConfig.engageWarningMaxDistanceM;
        const bool belowLimitAwaitingEngage = pbState.armed && nearPitEntry && !limiterOn && speedKmh <= effectiveLimitKmh + 0.05;

        if (belowLimitAwaitingEngage && !m_lastBelowLimitAwaitingEngage && g_pitLimiterConfig.engageBeepEnabled)
        {
            PlayEngageBeep();
            Log::Write("GearDisplay: at/under pit limit (%.1f km/h) with limiter still off -- engage reminder", speedKmh);
        }
        m_lastBelowLimitAwaitingEngage = belowLimitAwaitingEngage;
        m_state.showEngageLimiterWarning = belowLimitAwaitingEngage && g_pitLimiterConfig.engageWarningEnabled;
    }

    // Shift-point learning: keyed by vehicle only (rev-limiter behaviour is
    // a car property, not a track one).
    char shiftVehKey[64];
    VehicleKey::Get(shiftVehKey, sizeof(shiftVehKey));
    const std::string shiftKey = Sanitize(shiftVehKey, sizeof(shiftVehKey));
    if (shiftKey != m_shiftKey)
    {
        m_shiftKey = shiftKey;
        LoadShiftPoint(shiftVehKey);
        m_hasLastShiftSample = false;
        // Per-gear averages are per-car observations, not persisted values
        // -- a new car starts them empty.
        for (int g = 0; g < State::kMaxGears; ++g)
        {
            m_shiftSumByGear[g] = 0.0;
            m_shiftCountByGear[g] = 0;
            m_state.shiftAvgPercentByGear[g] = 0.0;
            m_state.shiftSampleCountByGear[g] = 0;
        }
    }

    const int gear = static_cast<int>(info.mGear);
    if (m_hasLastShiftSample && gear > m_lastGearForShift && m_lastGearForShift >= 1 && m_state.maxRpm > 100.0)
    {
        // m_lastRpmForShift is the RPM from the sample just BEFORE the gear
        // change registered -- i.e. the RPM you actually changed gear at.
        const double shiftRpm = m_lastRpmForShift;
        // The gear this upshift left -- the one whose cue needs to fire, and
        // so the one that learns from this sample.
        const int fromGear = (m_lastGearForShift < State::kMaxGears) ? m_lastGearForShift : (State::kMaxGears - 1);
        // Where this shift ACTUALLY landed, as a fraction of maxRpm, and how
        // far that is from the target the cue is currently pointing at.
        //
        // This error used to be measured against maxRpm itself
        // ((maxRpm - shiftRpm) / maxRpm), which made "shift AT the rev
        // limiter" the learner's only fixed point: any shift below the
        // limiter produced a positive error, so the target climbed on EVERY
        // shift until it hit the 0.999 clamp and stayed there. Following the
        // cue then kept confirming it -- a self-fulfilling loop that walked
        // the target to the limiter no matter where the driver actually
        // wanted to shift, which is exactly the "it just flashes at the rev
        // limiter" problem. Measuring against the CURRENT target instead
        // makes the fixed point "wherever you actually shift", so the cue
        // learns the driver's shift point rather than dragging it to redline.
        // Each learned value is corrected against ITS OWN current value, not
        // against a shared error: the car-wide figure used to be updated
        // with the error measured against the PER-GEAR target, so it was
        // being corrected toward a number it does not represent and drifted
        // incoherently (observed swinging 0.877-0.999 while every per-gear
        // value sat near 0.97). It matters because a gear with no samples
        // yet seeds from it.
        const double actualFraction = shiftRpm / m_state.maxRpm;
        const double carWideTarget = (m_state.learnedShiftRpmFraction > 0.0)
                                   ? m_state.learnedShiftRpmFraction
                                   : g_gearDisplayConfig.revLimiterRpmFraction;
        const double gearTarget = (m_state.learnedShiftRpmFractionByGear[fromGear] > 0.0)
                                ? m_state.learnedShiftRpmFractionByGear[fromGear]
                                : carWideTarget;
        const double carWideError = actualFraction - carWideTarget;
        const double gearError = actualFraction - gearTarget; // +ve = shifted LATER (higher RPM) than the cue asked for, -ve = earlier

        // If the "shift now" cue was armed and waiting, this upshift IS the
        // reaction to it -- measure the gap and feed it into the rolling
        // average that sets the cue's own lead time going forward.
        if (m_shiftCuePending)
        {
            const double reaction = info.mElapsedTime - m_shiftCueArmedET;
            if (reaction >= g_gearDisplayConfig.shiftReactionMinSeconds
                && reaction <= g_gearDisplayConfig.shiftReactionMaxSeconds)
            {
                RecordShiftReaction(reaction);
            }
            m_shiftCuePending = false;
        }

        // Full-throttle gate: only an upshift taken with the driver actually
        // flat out says anything about the optimal shift point. A
        // deliberate short-shift into a corner is off-throttle by
        // definition -- checked on the RAW pedal from the sample just
        // before the gear change, since that is what the DRIVER did, not
        // what TC left the engine with. This is stronger than the outlier
        // band below: a short-shift landing only just under the current
        // target (e.g. 9% low against a 10% band) would otherwise sail
        // straight through the outlier check and get learned as a real
        // correction. Excluded from the on-screen stats too, not just
        // learning -- it isn't data about the upshift point at all.
        if (m_lastThrottleForShift >= g_gearDisplayConfig.gearLearnMinThrottle)
        {
            // Rolling stats track every full-throttle shift, regardless of
            // the tolerance gate below -- these are a "how am I actually
            // doing" readout, not the adaptive control value.
            m_shiftHistory[m_shiftHistoryNext] = (shiftRpm / m_state.maxRpm) * 100.0;
            m_shiftHistoryNext = (m_shiftHistoryNext + 1) % kShiftHistorySize;
            if (m_shiftHistoryCount < kShiftHistorySize) ++m_shiftHistoryCount;
            RecomputeShiftStats();

            m_shiftSumByGear[fromGear] += (shiftRpm / m_state.maxRpm) * 100.0;
            ++m_shiftCountByGear[fromGear];
            m_state.shiftAvgPercentByGear[fromGear] = m_shiftSumByGear[fromGear] / m_shiftCountByGear[fromGear];
            m_state.shiftSampleCountByGear[fromGear] = m_shiftCountByGear[fromGear];

            // Outlier gate: a shift this far from what the cue asked for is
            // not a correction to learn from, it is a different event
            // entirely (a missed gear, a shift taken while already lifting
            // off a spinning wheel, etc). Judged against the per-gear
            // target, since that is the one the cue was actually showing.
            const double outlierBand = g_gearDisplayConfig.shiftLearnOutlierBand;
            if (outlierBand > 0.0 && fabs(gearError) > outlierBand)
            {
                Log::Write("GearDisplay: shift out of gear %d at %.0f/%.0f RPM (%.1f%% vs target %.1f%%) -- OUTLIER, not learned",
                           fromGear, shiftRpm, m_state.maxRpm, actualFraction * 100.0, gearTarget * 100.0);
            }
            else
            {
                constexpr double kToleranceFraction = 0.005; // within ~0.5% of maxRpm of the target already -- close enough, leave it alone
                constexpr double kLearnRate = 0.15; // how much of one shift's error to correct -- gradual, so one bad/missed shift doesn't swing it
                bool learned = false;

                // Car-wide value: still maintained, both as the fallback for
                // a gear with no samples yet and as the seed a new gear
                // starts from. Corrected against its OWN error.
                double frac = carWideTarget;
                if (fabs(carWideError) > kToleranceFraction)
                {
                    frac += kLearnRate * carWideError;
                    if (frac < 0.85) frac = 0.85;
                    if (frac > 0.999) frac = 0.999;
                    m_state.learnedShiftRpmFraction = frac;
                    learned = true;
                }

                // This gear's own value, learned from this gear's shifts alone.
                double gearFrac = m_state.learnedShiftRpmFractionByGear[fromGear];
                if (gearFrac <= 0.0) gearFrac = frac; // first sample for this gear: start where the car-wide value already is
                if (fabs(gearError) > kToleranceFraction)
                {
                    gearFrac += kLearnRate * gearError;
                    if (gearFrac < 0.85) gearFrac = 0.85;
                    if (gearFrac > 0.999) gearFrac = 0.999;
                    m_state.learnedShiftRpmFractionByGear[fromGear] = gearFrac;
                    learned = true;
                }

                if (learned)
                {
                    m_shiftPointDirty = true; // written later by FlushIfDirty(), off this (telemetry) thread's critical path
                    Log::Write("GearDisplay: shift out of gear %d at %.0f/%.0f RPM (%.1f%% vs target %.1f%%, %s) -- gear trigger now %.3f, car-wide %.3f",
                               fromGear, shiftRpm, m_state.maxRpm, actualFraction * 100.0, gearTarget * 100.0,
                               (gearError > 0.0) ? "later than cue" : "earlier than cue", gearFrac, frac);
                }
            }
        }
        else
        {
            Log::Write("GearDisplay: shift out of gear %d at %.0f/%.0f RPM -- off-throttle (%.0f%%), not counted",
                       fromGear, shiftRpm, m_state.maxRpm, m_lastThrottleForShift * 100.0);
        }
    }
    // Neutral is skipped rather than tracked as "the last gear" -- a manual/
    // clutch car genuinely passes through it for the length of every shift,
    // and if a telemetry sample happens to land exactly there, treating it
    // as the last-seen gear would erase which forward gear was actually left
    // (and the RPM it was left at) right before the very transition this
    // state exists to catch -- silently dropping that upshift from learning
    // entirely, since 0 fails the ">= 1" check above. Holding both values
    // from the last real gear across a neutral blip lets a 2 -> 0 -> 3
    // sequence still register correctly as an upshift out of 2, at the RPM
    // it was actually turning at in 2nd -- not whatever the engine happened
    // to show mid-shift with the clutch in.
    if (gear != 0)
    {
        m_lastGearForShift = gear;
        m_lastRpmForShift = m_state.rpm;
        m_lastThrottleForShift = info.mUnfilteredThrottle;
    }
    m_hasLastShiftSample = true; // from the NEXT sample onward, there's a previous one to compare against

    // --- "Shift now" cue ---
    //
    // Fires LED BACK from the target RPM by the reaction-time lead, so the
    // actual shift (after you react) lands ON the target instead of past
    // it -- see the class comment in GearDisplay.h for why this replaced a
    // straight "flash at the target" trigger. Recomputed AFTER the upshift
    // handling above, using this tick's (possibly just-changed) gear/RPM.
    {
        double targetFraction = g_gearDisplayConfig.revLimiterRpmFraction;
        if (m_state.learnedShiftRpmFraction > 0.0)
            targetFraction = m_state.learnedShiftRpmFraction;
        if (gear >= 1 && gear < State::kMaxGears && m_state.learnedShiftRpmFractionByGear[gear] > 0.0)
            targetFraction = m_state.learnedShiftRpmFractionByGear[gear];

        // Only means anything while genuinely trying to accelerate -- a
        // rev-matched downshift's RPM blip happens with the driver already
        // OFF the power and about to brake, and without this gate it could
        // both light the cue and then LATCH it on for the whole braking
        // zone (the latch below only releases on an RPM drop, which engine
        // braking may not deliver quickly). Checked every tick, including
        // while latched, so lifting off the throttle releases it
        // immediately rather than waiting on the RPM hysteresis.
        const bool wantsToAccelerate = info.mUnfilteredThrottle >= g_gearDisplayConfig.shiftCueMinThrottle;

        bool cueActive = false;
        if (gear >= 1 && m_state.maxRpm > 1.0 && wantsToAccelerate)
        {
            if (m_shiftCueLatched)
            {
                // Already showing: hold it against the RPM it first fired
                // at, NOT against a freshly recomputed trigger (which rises
                // as the climb rate tapers and would flick the cue off --
                // see m_shiftCueLatched in the header). Released only by a
                // genuine RPM drop: the shift itself, or a real lift.
                constexpr double kReleaseHysteresisFraction = 0.02; // 2% of maxRpm below where it armed
                const double releaseRpm = m_shiftCueArmRpm - m_state.maxRpm * kReleaseHysteresisFraction;
                cueActive = m_state.rpm >= releaseRpm;
            }
            else
            {
                const double targetRpm = m_state.maxRpm * targetFraction;
                const double leadRpm = ShiftReactionLeadTimeSeconds() * rpmRate;
                double cueRpm = targetRpm - leadRpm;
                // Never let the lead push the blue trigger below the red
                // "approaching" threshold -- without this, a large lead
                // (early in the rev range, where RPM climbs fastest) could
                // put cueRpm under shiftRpmFraction entirely, so blue would
                // always beat red to the draw and red would never be seen
                // at all (blue takes priority in Draw()'s if/else chain).
                const double redRpm = m_state.maxRpm * g_gearDisplayConfig.shiftRpmFraction;
                if (cueRpm < redRpm) cueRpm = redRpm;
                cueActive = m_state.rpm >= cueRpm;
                if (cueActive)
                    m_shiftCueArmRpm = cueRpm;
            }
        }
        m_shiftCueLatched = cueActive;

        if (cueActive && !m_shiftCuePending)
        {
            m_shiftCuePending = true;
            m_shiftCueArmedET = info.mElapsedTime;
        }
        else if (!cueActive)
        {
            m_shiftCuePending = false;
        }
        // Still active but nothing has happened for a long time (e.g.
        // deliberately holding it against the limiter) -- abandon the
        // pending reaction measurement rather than letting a much later
        // upshift pair with a stale armed time.
        if (m_shiftCuePending && (info.mElapsedTime - m_shiftCueArmedET) > g_gearDisplayConfig.shiftReactionTimeoutSeconds)
        {
            m_shiftCuePending = false;
        }

        m_state.shiftCueActive = cueActive;
    }

    m_state.valid = true;
    LeaveCriticalSection(&m_lock);
}

void GearDisplayStore::RecomputeShiftStats()
{
    // Called with m_lock already held (from Update()).
    double sorted[kShiftHistorySize];
    std::copy(m_shiftHistory, m_shiftHistory + m_shiftHistoryCount, sorted);
    std::sort(sorted, sorted + m_shiftHistoryCount);

    double sum = 0.0;
    for (int i = 0; i < m_shiftHistoryCount; ++i) sum += sorted[i];
    m_state.shiftAvgPercent = sum / m_shiftHistoryCount;

    // Nearest-rank 1st percentile -- with few samples this is just the
    // minimum, which is the right behaviour (not enough data yet for a real
    // percentile, but "your earliest shift so far" is still meaningful).
    int p1Index = static_cast<int>(0.01 * m_shiftHistoryCount);
    if (p1Index >= m_shiftHistoryCount) p1Index = m_shiftHistoryCount - 1;
    m_state.shiftP1Percent = sorted[p1Index];
    m_state.shiftSampleCount = m_shiftHistoryCount;
}

double GearDisplayStore::ShiftReactionLeadTimeSeconds() const
{
    // m_avgShiftReactionSeconds is owned by this (the telemetry) thread, so
    // no lock is needed to read it here.
    double lead = g_gearDisplayConfig.shiftReactionDefaultSeconds;
    if (g_gearDisplayConfig.shiftReactionAdaptiveEnabled && m_avgShiftReactionSeconds > 0.0)
        lead = m_avgShiftReactionSeconds;

    if (lead < g_gearDisplayConfig.shiftReactionMinSeconds) lead = g_gearDisplayConfig.shiftReactionMinSeconds;
    if (lead > g_gearDisplayConfig.shiftReactionMaxSeconds) lead = g_gearDisplayConfig.shiftReactionMaxSeconds;
    return lead;
}

void GearDisplayStore::RecordShiftReaction(double reactionSeconds)
{
    // Called with m_lock already held (from Update()).
    m_shiftReactionHistory[m_shiftReactionHistoryNext] = reactionSeconds;
    m_shiftReactionHistoryNext = (m_shiftReactionHistoryNext + 1) % kShiftReactionHistorySize;
    if (m_shiftReactionHistoryCount < kShiftReactionHistorySize) ++m_shiftReactionHistoryCount;

    double sum = 0.0;
    for (int i = 0; i < m_shiftReactionHistoryCount; ++i) sum += m_shiftReactionHistory[i];
    m_avgShiftReactionSeconds = sum / m_shiftReactionHistoryCount;

    m_state.shiftReactionAvgSeconds = m_avgShiftReactionSeconds;
    m_state.shiftReactionLatestSeconds = reactionSeconds;
    m_state.shiftReactionSampleCount = m_shiftReactionHistoryCount;

    Log::Write("GearDisplay: shift reaction %.0fms (avg %.0fms over %d), cue lead now %.0fms",
               reactionSeconds * 1000.0, m_avgShiftReactionSeconds * 1000.0,
               m_shiftReactionHistoryCount, ShiftReactionLeadTimeSeconds() * 1000.0);
}

void GearDisplayStore::Clear()
{
    if (!m_ready)
        return;
    EnterCriticalSection(&m_lock);
    // Callers are expected to FlushIfDirty() before Clear() (see Plugin.cpp)
    // -- SavePitLimit() needs m_currentKey, which is about to be wiped
    // below. Dropped here too, defensively, so a dirty flag left set by some
    // future call path can't later write using an already-cleared key.
    m_pitLimitDirty = false;
    m_shiftPointDirty = false;
    m_currentKey.clear();
    m_lastSpeedLimiterOn = false;
    m_episodeMaxKmh = 0.0;
    LeaveCriticalSection(&m_lock);
}

void GearDisplayStore::FlushIfDirty()
{
    if (!m_ready)
        return;
    EnterCriticalSection(&m_lock);
    if (m_pitLimitDirty)
    {
        SavePitLimit();
        m_pitLimitDirty = false;
    }
    if (m_shiftPointDirty)
    {
        SaveShiftPoint();
        m_shiftPointDirty = false;
    }
    LeaveCriticalSection(&m_lock);
}

GearDisplayStore::State GearDisplayStore::Read()
{
    State copy;
    if (!m_ready)
        return copy;

    EnterCriticalSection(&m_lock);
    copy = m_state;
    LeaveCriticalSection(&m_lock);
    return copy;
}

namespace
{
    // Shared blink timer (wall-clock, render-thread-local) for the gear
    // flash -- same reasoning as the pedal trace's idle fade.
    float TickBlink(double periodSeconds)
    {
        static float s_phase = 0.0f;
        static LARGE_INTEGER s_lastTime = {};
        static LARGE_INTEGER s_freq = {};
        if (s_freq.QuadPart == 0)
            QueryPerformanceFrequency(&s_freq);

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = 0.0;
        if (s_lastTime.QuadPart != 0 && s_freq.QuadPart != 0)
        {
            dt = static_cast<double>(now.QuadPart - s_lastTime.QuadPart) / static_cast<double>(s_freq.QuadPart);
            if (dt < 0.0 || dt > 1.0) dt = 0.0;
        }
        s_lastTime = now;

        const double period = (periodSeconds > 0.02) ? periodSeconds : 0.02;
        s_phase = fmodf(s_phase + static_cast<float>(dt / period), 1.0f);
        return s_phase;
    }
}

bool GearDisplay::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    // Medium tier (~25Hz): gear/speed text can't be read faster than this
    // anyway, so skip past all the layout/formatting work below on frames
    // that aren't due and just replay what was drawn last time. Bypassed
    // while layout mode is active so dragging/resizing this panel stays at
    // full responsiveness.
    static RenderThrottle s_throttle(0.04);
    if (!Layout::Active() && s_throttle.TryReplay(renderer))
        return true;
    RenderThrottle::Capture throttleCapture(s_throttle, renderer);

    // This widget paints its own background panel, so the renderer's
    // per-string text plate would just double-darken over it. Restored on
    // every exit path by the scope guard, since Draw() returns from several
    // places.
    struct TextBgGuard
    {
        Renderer& r; bool prev;
        explicit TextBgGuard(Renderer& rr) : r(rr), prev(rr.TextBackgroundEnabled()) { r.SetTextBackground(false); }
        ~TextBgGuard() { r.SetTextBackground(prev); }
    } textBgGuard(renderer);

    const GearDisplayConfig& cfg = g_gearDisplayConfig;
    const PitLimiterConfig& plCfg = g_pitLimiterConfig;

    if (!cfg.enabled)
        return false;

    const GearDisplayStore::State state = g_gearDisplay.Read();
    if (!state.valid)
        return false;

    // Idle fade: shares the pedal trace's own "moving or on the pedals"
    // activity clock rather than duplicating it, so the two widgets always
    // agree on what counts as idle. Wall-clock driven so it keeps fading
    // even if telemetry stops arriving entirely (e.g. back at the menu).
    static float s_displayAlpha = 0.0f;
    static LARGE_INTEGER s_lastDrawTime = {};
    static LARGE_INTEGER s_freq = {};
    if (s_freq.QuadPart == 0)
        QueryPerformanceFrequency(&s_freq);

    LARGE_INTEGER nowFade;
    QueryPerformanceCounter(&nowFade);
    double frameDt = 0.0;
    if (s_lastDrawTime.QuadPart != 0 && s_freq.QuadPart != 0)
    {
        frameDt = static_cast<double>(nowFade.QuadPart - s_lastDrawTime.QuadPart) / static_cast<double>(s_freq.QuadPart);
        if (frameDt < 0.0 || frameDt > 1.0) frameDt = 0.0;
    }
    s_lastDrawTime = nowFade;

    const bool currentlyActive = g_pedalTrace.SecondsSinceActive() < 0.05;
    // A panel you cannot see is a panel you cannot grab, so the idle fade is
    // held off entirely while layout mode is on -- and SNAPPED rather than
    // eased, so it is ready the instant the mode is entered instead of a
    // second later.
    const bool suppressFade = Layout::SuppressFade();
    const float fadeTarget = (currentlyActive || suppressFade) ? 1.0f : 0.0f;
    if (suppressFade)
        s_displayAlpha = 1.0f;
    const float fadeSeconds = (cfg.idleFadeSeconds > 0.01f) ? cfg.idleFadeSeconds : 0.01f;
    const float fadeStep = static_cast<float>(frameDt) / fadeSeconds;

    if (s_displayAlpha < fadeTarget)
        s_displayAlpha = (s_displayAlpha + fadeStep < fadeTarget) ? s_displayAlpha + fadeStep : fadeTarget;
    else if (s_displayAlpha > fadeTarget)
        s_displayAlpha = (s_displayAlpha - fadeStep > fadeTarget) ? s_displayAlpha - fadeStep : fadeTarget;

    if (s_displayAlpha <= 0.004f && !suppressFade)
        return false; // fully faded -- nothing to draw

    const float fade = s_displayAlpha;

    const float phase = TickBlink(cfg.blinkPeriodSeconds);
    const bool blinkOn = phase < 0.5f;

    // Three states, in priority order -- you can only usefully be told the
    // most urgent one at a time:
    //   purple: genuinely about to hit the PHYSICAL rev limiter. Checked
    //     directly against maxRpm, never against the learned target, so it
    //     can't be tuned away by the same adaptive learning that drives the
    //     blue cue -- if the target has drifted somewhere leaving too little
    //     margin, or you're holding a gear on purpose, this still fires.
    //   blue: the "shift now" cue. shiftCueActive is computed in
    //     GearDisplayStore::Update() (the telemetry thread), NOT from a
    //     fixed RPM fraction checked here: it fires LED BACK from the
    //     learned target RPM by your own measured reaction time, so the
    //     actual shift lands ON the target once you react to it (see
    //     GearDisplay.h's class comment).
    //   red: getting close, nothing urgent yet -- SOLID, not flashing.
    // Flashes the GEAR PANEL'S BACKGROUND, not the digit itself -- the
    // number stays a plain, always-legible white; the box is what flags it.
    float gearBgR = cfg.bgR, gearBgG = cfg.bgG, gearBgB = cfg.bgB;
    // Neutral / first-gear warnings take priority over the RPM-based ones
    // below: being in neutral or first gear is itself worth flagging
    // regardless of what the RPM happens to be doing (idling in neutral is
    // never near the limiter, so the checks below would never fire for it
    // anyway) -- e.g. an accidental downshift to neutral, or forgetting
    // you're still in first on a standing start. Solid, not blinking: this
    // is a state readout, not a one-off alert to react to and dismiss.
    // Neutral only turns red once it has held for 0.2 s: a quick upshift
    // passes through neutral for a frame or two, which flashed the box red
    // on every gear change.
    bool neutralHeld = false;
    {
        static LARGE_INTEGER s_neutralSince = {};
        static LARGE_INTEGER s_nFreq = {};
        if (s_nFreq.QuadPart == 0) QueryPerformanceFrequency(&s_nFreq);
        if (state.gear == 0)
        {
            LARGE_INTEGER nowN;
            QueryPerformanceCounter(&nowN);
            if (s_neutralSince.QuadPart == 0) s_neutralSince = nowN;
            neutralHeld = static_cast<double>(nowN.QuadPart - s_neutralSince.QuadPart)
                        / static_cast<double>(s_nFreq.QuadPart) >= 0.2;
        }
        else
        {
            s_neutralSince.QuadPart = 0;
        }
    }
    if (cfg.neutralWarnEnabled && state.gear == 0 && neutralHeld)
    {
        gearBgR = cfg.neutralWarnR; gearBgG = cfg.neutralWarnG; gearBgB = cfg.neutralWarnB;
    }
    else if (cfg.firstGearWarnEnabled && state.gear == 1)
    {
        gearBgR = cfg.firstGearWarnR; gearBgG = cfg.firstGearWarnG; gearBgB = cfg.firstGearWarnB;
    }
    else if (state.maxRpm > 1.0 && state.rpm >= state.maxRpm * cfg.trueLimiterWarnFraction)
    {
        if (blinkOn) { gearBgR = cfg.trueLimiterR; gearBgG = cfg.trueLimiterG; gearBgB = cfg.trueLimiterB; }
    }
    else if (state.shiftCueActive)
    {
        if (blinkOn) { gearBgR = cfg.revLimiterR; gearBgG = cfg.revLimiterG; gearBgB = cfg.revLimiterB; }
    }
    // The solid-red "getting close, nothing urgent yet" state that used to
    // sit here (cfg.shiftRpmFraction/cfg.shiftR) has been removed by
    // request: the blue "shift now" cue above is the only timing
    // aid wanted, and the extra red state ahead of it was just noise. The
    // purple true-limiter warning above and the neutral/first-gear warnings
    // below are unrelated states, kept as they were.

    // Same convention as the gear panel above: colour the BOX, never the
    // digits, so the number stays plain and always legible.
    float speedBgR = cfg.bgR, speedBgG = cfg.bgG, speedBgB = cfg.bgB;
    // Longitudinal tyre state, replacing the ABS/TC yellow this box used to
    // flash (that has moved to its own box beside the gear digit). Solid
    // rather than blinking: it is a readout of what the tyres are doing, not
    // an alert to be noticed once and dismissed.
    if (state.slipState == 1)      { speedBgR = cfg.lockSlipR; speedBgG = cfg.lockSlipG; speedBgB = cfg.lockSlipB; }
    else if (state.slipState == 2) { speedBgR = cfg.spinSlipR; speedBgG = cfg.spinSlipG; speedBgB = cfg.spinSlipB; }

    char gearBuf[4];
    if (state.gear < 0) strcpy_s(gearBuf, "R");
    else if (state.gear == 0) strcpy_s(gearBuf, "N");
    else sprintf_s(gearBuf, "%d", state.gear);

    char speedBuf[8] = "";
    if (cfg.speedEnabled)
    {
        const double speedDisplay = cfg.speedUnitsKmh ? (state.speedMs * 3.6) : (state.speedMs * 2.23694);
        sprintf_s(speedBuf, "%d", static_cast<int>(speedDisplay + 0.5));
    }

    const float panelCenterX = screenWidth * cfg.centerXFraction;
    const float panelCenterY = screenHeight * cfg.centerYFraction;

    // cfg.scale is the layout-mode corner-handle multiplier. Folding it
    // in HERE rather than applying it to the drawn box means every pixel
    // value downstream -- text height, padding, gaps, corner radii --
    // scales with it in exact proportion, because they all already
    // derive from this one value.
    const float uiScale = static_cast<float>(screenHeight) / 2160.0f * cfg.scale;
    const float gearTextHeightPx = cfg.gearTextHeightPx * uiScale;
    const float speedTextHeightPx = cfg.speedTextHeightPx * uiScale;
    const float bgPaddingPx = cfg.bgPaddingPx * uiScale;
    const float bgCornerRadiusPx = cfg.bgCornerRadiusPx * uiScale;
    const float gapPx = cfg.gapPx * uiScale;
    const float borderThicknessPx = cfg.borderThicknessPx * uiScale;
    const float shiftStatsGapPx = cfg.shiftStatsGapPx * uiScale;
    const float plTextHeightPx = g_pitLimiterConfig.textHeightPx * uiScale;
    const float plInfoTextHeightPx = g_pitLimiterConfig.infoTextHeightPx * uiScale;
    const float plDeltaTextHeightPx = g_pitLimiterConfig.deltaTextHeightPx * uiScale;
    const float plPaddingPx = g_pitLimiterConfig.paddingPx * uiScale;
    const float plEngageWarningTextHeightPx = g_pitLimiterConfig.engageWarningTextHeightPx * uiScale;

    const float gearW = renderer.MeasureText(gearBuf, gearTextHeightPx);
    const float speedW = cfg.speedEnabled ? renderer.MeasureText(speedBuf, speedTextHeightPx) : 0.0f;

    // Gear and speed each get their own panel, same height, side by side with
    // a gap -- matching the reference HUD's two separate segment displays
    // rather than one shared box.
    const float gearPanelW = gearW + bgPaddingPx * 2.0f;
    const float speedPanelW = cfg.speedEnabled ? (speedW + bgPaddingPx * 2.0f) : 0.0f;
    const float tallestTextH = (cfg.speedEnabled && speedTextHeightPx > gearTextHeightPx)
                              ? speedTextHeightPx : gearTextHeightPx;
    const float panelH = tallestTextH + bgPaddingPx * 2.0f;

    // The GEAR panel's own centre sits on screen centre (centerXFraction) --
    // the speed panel extends to its right, rather than the pair being
    // centred as a combined unit (which would push the gear number off
    // centre to make room for the speed panel).
    const float gearPanelCenterX = panelCenterX;
    const float speedPanelCenterX = panelCenterX + gearPanelW * 0.5f + gapPx + speedPanelW * 0.5f;

    // --- ABS / TC box, immediately LEFT of the gear digit ---
    //
    // Drawn only while something is actually intervening, so it costs no
    // screen space the rest of the time. It hangs off the left rather than
    // displacing anything: the gear digit stays exactly on the panel's
    // anchor whether this is showing or not, so the dashboard does not
    // shuffle sideways every time ABS bites.
    // Each can be switched off from the shortcut list (cfg.showAbs/showTc).
    const bool absOn = state.absActive && cfg.showAbs;
    const bool tcOn = state.tcActive && cfg.showTc;
    char absTcBuf[12] = "";
    if (absOn && tcOn) strcpy_s(absTcBuf, "ABS TC");
    else if (absOn) strcpy_s(absTcBuf, "ABS");
    else if (tcOn) strcpy_s(absTcBuf, "TC");
    // Pit limiter on: the box shows the whole time it is engaged (coloured
    // by speed vs the pit-lane limit, see below), not only during a cut.
    if (state.speedLimiterOn) strcpy_s(absTcBuf, "PIT");

    // Forced visible (with a placeholder) in layout mode -- otherwise a box
    // that only ever appears while ABS/TC is actively cutting could never be
    // seen, let alone dragged into place, during a normal setup session.
    const bool absTcPlaceholder = Layout::Active() && absTcBuf[0] == 0;
    if (absTcPlaceholder)
        strcpy_s(absTcBuf, "ABS TC");
    const bool showAbsTc = cfg.absTcBoxEnabled && absTcBuf[0] != 0;
    const float absTcGapPx = cfg.absTcBoxGapPx * uiScale;
    // Half the gear digit's own text size -- this box just names which
    // system is intervening, it doesn't need to be as loud as the gear
    // number itself, and the box itself was made narrower.
    const float absTcTextHeightPx = gearTextHeightPx * 0.5f;
    // Sized to the widest string it can ever hold, not to the live text, so
    // the box does not change width as ABS and TC come and go.
    const float absTcTextW = renderer.MeasureText("ABS TC", absTcTextHeightPx);
    const float absTcPanelW = absTcTextW + bgPaddingPx * 2.0f;
    const float absTcCenterX = gearPanelCenterX - gearPanelW * 0.5f - absTcGapPx - absTcPanelW * 0.5f;

    // One draggable unit spanning both boxes -- grabbing the speed readout
    // and having only the gear digit move would be surprising.
    {
        const float unitLeft = showAbsTc ? (absTcCenterX - absTcPanelW * 0.5f)
                                         : (gearPanelCenterX - gearPanelW * 0.5f);
        const float unitRight = cfg.speedEnabled ? (speedPanelCenterX + speedPanelW * 0.5f)
                                                 : (gearPanelCenterX + gearPanelW * 0.5f);
        Layout::Register("Gear / speed", "GearDisplay", "CenterXFraction", "CenterYFraction",
                         &g_gearDisplayConfig.centerXFraction, &g_gearDisplayConfig.centerYFraction,
                         (unitLeft + unitRight) * 0.5f, panelCenterY,
                         (unitRight - unitLeft) * 0.5f, panelH * 0.5f,
                         &g_gearDisplayConfig.scale, "Scale");
    }

    if (showAbsTc)
    {
        if (cfg.backgroundEnabled)
        {
            // The placeholder shown for positioning in layout mode gets the
            // normal panel plate rather than the lit flash colour -- that
            // flash means "intervening right now", which isn't true here.
            if (absTcPlaceholder)
                renderer.AddFilledRoundedRect(absTcCenterX, panelCenterY, absTcPanelW * 0.5f, panelH * 0.5f,
                                              bgCornerRadiusPx, 0.0f, gearBgR, gearBgG, gearBgB, cfg.bgA * fade);
            else if (state.speedLimiterOn)
            {
                // Pit limiter on: green at or under the pit-lane limit,
                // blinking red over it. The limit is the one learned from
                // driving with the limiter on (PitLimits\<car>_<track>.txt);
                // until one is known, a steady amber "PIT".
                const double limitKmh = state.learnedPitLimitKmh;
                const double speedKmhNow = state.speedMs * 3.6;
                float r = 1.0f, g = 0.65f, b = 0.1f, a = cfg.bgA * fade; // amber: limit unknown
                if (limitKmh > 0.0)
                {
                    if (speedKmhNow <= limitKmh + 0.5)
                    { r = 0.15f; g = 0.85f; b = 0.25f; }
                    else
                    { r = 0.95f; g = 0.12f; b = 0.10f; a *= blinkOn ? 1.0f : 0.3f; }
                }
                renderer.AddFilledRoundedRect(absTcCenterX, panelCenterY, absTcPanelW * 0.5f, panelH * 0.5f,
                                              bgCornerRadiusPx, 0.0f, r, g, b, a);
            }
            else
            {
                // Same colours as the pedal trace: ABS in the brake trace's
                // amber, TC in the throttle trace's cyan, flashing. With both
                // cutting at once the flash alternates between the two.
                const PedalConfig& pc = g_pedalConfig;
                const bool useAbs = absOn && (!tcOn || blinkOn);
                const float r = useAbs ? pc.brakeRawR : pc.throttleRawR;
                const float g = useAbs ? pc.brakeRawG : pc.throttleRawG;
                const float b = useAbs ? pc.brakeRawB : pc.throttleRawB;
                const bool both = absOn && tcOn;
                const float a = (both || blinkOn ? 1.0f : 0.35f) * cfg.bgA * fade;
                renderer.AddFilledRoundedRect(absTcCenterX, panelCenterY, absTcPanelW * 0.5f, panelH * 0.5f,
                                              bgCornerRadiusPx, 0.0f, r, g, b, a);
            }
        }
        // Dark text on the lit background: this box only exists while it is
        // saying something, so the whole box lights up rather than just the
        // letters. The layout-mode placeholder dims the text instead, since
        // there is no lit background under it to read dark text against.
        if (absTcPlaceholder)
            renderer.AddText(absTcCenterX, panelCenterY - absTcTextHeightPx * 0.5f, absTcBuf, absTcTextHeightPx,
                             0.5f, 0.5f, 0.5f, fade, Renderer::HAlign::Center);
        else
            renderer.AddText(absTcCenterX, panelCenterY - absTcTextHeightPx * 0.5f, absTcBuf, absTcTextHeightPx,
                             0.05f, 0.05f, 0.05f, fade, Renderer::HAlign::Center);
    }

    if (cfg.backgroundEnabled)
    {
        renderer.AddFilledRoundedRect(gearPanelCenterX, panelCenterY, gearPanelW * 0.5f, panelH * 0.5f,
                                      bgCornerRadiusPx, 0.0f, gearBgR, gearBgG, gearBgB, cfg.bgA * fade);
        if (cfg.speedEnabled)
        {
            renderer.AddFilledRoundedRect(speedPanelCenterX, panelCenterY, speedPanelW * 0.5f, panelH * 0.5f,
                                          bgCornerRadiusPx, 0.0f, speedBgR, speedBgG, speedBgB, cfg.bgA * fade);
        }
    }
    if (cfg.borderEnabled)
    {
        renderer.AddRectOutline(gearPanelCenterX, panelCenterY, gearPanelW * 0.5f, panelH * 0.5f,
                                borderThicknessPx, cfg.borderR, cfg.borderG, cfg.borderB, fade);
        if (cfg.speedEnabled)
        {
            renderer.AddRectOutline(speedPanelCenterX, panelCenterY, speedPanelW * 0.5f, panelH * 0.5f,
                                    borderThicknessPx, cfg.borderR, cfg.borderG, cfg.borderB, fade);
        }
    }

    const float gearY = panelCenterY - gearTextHeightPx * 0.5f;
    renderer.AddText(gearPanelCenterX, gearY, gearBuf, gearTextHeightPx, cfg.normalR, cfg.normalG, cfg.normalB, fade, Renderer::HAlign::Center);

    if (cfg.speedEnabled)
    {
        const float speedY = panelCenterY - speedTextHeightPx * 0.5f;
        renderer.AddText(speedPanelCenterX, speedY, speedBuf, speedTextHeightPx, cfg.speedR, cfg.speedG, cfg.speedB, fade, Renderer::HAlign::Center);

    }

    // --- Tyre lockup box, 2x2 grid immediately RIGHT of the speed panel ---
    //
    // Always drawn (all 4 cells, even at zero slip) so it reads as a fixed
    // instrument rather than something that only appears when things go
    // wrong. Each cell's colour intensity and its printed percentage are
    // the PEAK slip magnitude reached since the brake was last pressed --
    // held indefinitely once off the brake, so a lockup on corner entry is
    // still readable deep into the next straight, not just for a few
    // seconds. A fresh brake application resets that corner's peak and
    // starts tracking the new braking zone. Gated by its own shortcut
    // (Ctrl+Shift+L by default) as well as the INI enable, so it can be
    // turned off entirely like every other panel.
    if (cfg.tyreLockupBoxEnabled && Shortcuts::Visible(Shortcuts::TyreLockup))
    {
        const PedalTraceStore::SlipState slip = g_pedalTrace.ReadSlip();

        static double s_wheelPeak[4] = { 0.0, 0.0, 0.0, 0.0 };
        static bool s_wasBraking = false;

        const bool isBraking = slip.valid && slip.brakeInput > 0.05;
        if (isBraking && !s_wasBraking)
        {
            for (int w = 0; w < 4; ++w) s_wheelPeak[w] = 0.0;
        }
        s_wasBraking = isBraking;

        if (slip.valid)
        {
            for (int w = 0; w < 4; ++w)
            {
                if (slip.perWheelLock[w] > s_wheelPeak[w]) s_wheelPeak[w] = slip.perWheelLock[w];
            }
        }

        const float lockupGapPx = cfg.absTcBoxGapPx * uiScale;
        const float lockupUiScale = uiScale * cfg.tyreLockupScale;
        // Each cell is half the gear digit's height, matching the ABS/TC
        // box's "not as loud as the gear number" sizing convention.
        const float cellH = gearTextHeightPx * 0.5f * cfg.tyreLockupScale;
        const float cellW = cellH * 1.3f;
        const float cellGapPx = 3.0f * lockupUiScale;
        const float boxW = cellW * 2.0f + cellGapPx;
        const float boxH = cellH * 2.0f + cellGapPx;

        // Sentinel auto-anchor: resolves to just right of the speed panel
        // the first time this draws, then independently draggable/
        // resizable from there.
        if (cfg.tyreLockupPosXFraction < 0.0f || cfg.tyreLockupPosYFraction < 0.0f)
        {
            const float initCenterX = speedPanelCenterX + speedPanelW * 0.5f + lockupGapPx + boxW * 0.5f;
            g_gearDisplayConfig.tyreLockupPosXFraction = initCenterX / screenWidth;
            g_gearDisplayConfig.tyreLockupPosYFraction = panelCenterY / screenHeight;
        }
        const float boxCenterX = screenWidth * cfg.tyreLockupPosXFraction;
        const float boxCenterY = screenHeight * cfg.tyreLockupPosYFraction;

        Layout::Register("Tyre lockup", "GearDisplay", "TyreLockupPosXFraction", "TyreLockupPosYFraction",
                         &g_gearDisplayConfig.tyreLockupPosXFraction, &g_gearDisplayConfig.tyreLockupPosYFraction,
                         boxCenterX, boxCenterY, boxW * 0.5f, boxH * 0.5f,
                         &g_gearDisplayConfig.tyreLockupScale, "TyreLockupScale");

        // FL top-left, FR top-right, RL bottom-left, RR bottom-right --
        // same physical-layout convention as FuelTyrePanel's tyre grid.
        const float cellCenters[4][2] = {
            { boxCenterX - cellW * 0.5f - cellGapPx * 0.5f, boxCenterY - cellH * 0.5f - cellGapPx * 0.5f }, // FL
            { boxCenterX + cellW * 0.5f + cellGapPx * 0.5f, boxCenterY - cellH * 0.5f - cellGapPx * 0.5f }, // FR
            { boxCenterX - cellW * 0.5f - cellGapPx * 0.5f, boxCenterY + cellH * 0.5f + cellGapPx * 0.5f }, // RL
            { boxCenterX + cellW * 0.5f + cellGapPx * 0.5f, boxCenterY + cellH * 0.5f + cellGapPx * 0.5f }, // RR
        };

        const float numTextHeightPx = cellH * 0.5f;

        for (int w = 0; w < 4; ++w)
        {
            const double range = cfg.tyreLockupFullScale;
            float intensity = (range > 0.0001) ? static_cast<float>(s_wheelPeak[w] / range) : 0.0f;
            if (intensity > 1.0f) intensity = 1.0f;
            if (intensity < 0.0f) intensity = 0.0f;

            // Faint fixed outline so the box's position and shape read
            // clearly even at zero slip.
            renderer.AddFilledRoundedRect(cellCenters[w][0], cellCenters[w][1], cellW * 0.5f, cellH * 0.5f,
                                          bgCornerRadiusPx * 0.5f, 0.0f, 0.12f, 0.12f, 0.12f, 0.55f * fade);
            if (intensity > 0.001f)
            {
                renderer.AddFilledRoundedRect(cellCenters[w][0], cellCenters[w][1], cellW * 0.5f, cellH * 0.5f,
                                              bgCornerRadiusPx * 0.5f, 0.0f,
                                              cfg.tyreLockupR, cfg.tyreLockupG, cfg.tyreLockupB,
                                              intensity * fade);
            }

            char pctBuf[8];
            sprintf_s(pctBuf, "%.0f%%", s_wheelPeak[w] * 100.0);
            renderer.AddText(cellCenters[w][0], cellCenters[w][1] - numTextHeightPx * 0.5f, pctBuf, numTextHeightPx,
                             1.0f, 1.0f, 1.0f, fade, Renderer::HAlign::Center);
        }
    }

    // Pit limiter: only drawn while actually engaged. Flush against the
    // right edge of the speed panel (or the gear panel, if speed is
    // disabled) using the same gap convention as between gear and speed, and
    // the same row height, so it reads as part of one continuous dashboard
    // rather than a separately floating box.
    if (plCfg.enabled && state.speedLimiterOn)
    {
        const char* pitText = "PIT";

        // Green at/under the pit-lane limit, fading through orange toward
        // red as you get further over it, blinking once you're WAY over
        // (past OverLimitFarKmh) rather than just past it -- a small,
        // momentary excess barely tints the box, while a serious overspeed
        // is unmistakable. The SDK doesn't expose the actual numeric limit,
        // but prefers the value LEARNED from the limiter's own cap (see
        // GearDisplayStore) once available -- persisted across sessions, and
        // only falls back to the configured default before one's been
        // learned or loaded for this car+track.
        const double effectiveLimitKmh = (state.learnedPitLimitKmh > 0.0) ? state.learnedPitLimitKmh : plCfg.speedLimitKmh;
        const double speedKmh = state.speedMs * 3.6;
        const double overKmh = speedKmh - effectiveLimitKmh;

        float pitFillR = plCfg.fillR, pitFillG = plCfg.fillG, pitFillB = plCfg.fillB; // green
        if (overKmh > 0.0)
        {
            const double farSpan = (plCfg.overLimitFarKmh > 0.1) ? plCfg.overLimitFarKmh : 0.1;
            const double severity = (overKmh < farSpan) ? (overKmh / farSpan) : 1.0; // 0..1
            if (severity <= 0.5)
            {
                const float t = static_cast<float>(severity / 0.5);
                pitFillR = plCfg.fillR + (plCfg.midR - plCfg.fillR) * t;
                pitFillG = plCfg.fillG + (plCfg.midG - plCfg.fillG) * t;
                pitFillB = plCfg.fillB + (plCfg.midB - plCfg.fillB) * t;
            }
            else
            {
                const float t = static_cast<float>((severity - 0.5) / 0.5);
                pitFillR = plCfg.midR + (plCfg.overLimitR - plCfg.midR) * t;
                pitFillG = plCfg.midG + (plCfg.overLimitG - plCfg.midG) * t;
                pitFillB = plCfg.midB + (plCfg.overLimitB - plCfg.midB) * t;
            }

            if (overKmh >= farSpan)
            {
                const float pitPhase = TickBlink(plCfg.overLimitFlashPeriodSeconds);
                if (pitPhase >= 0.5f) { pitFillR *= 0.3f; pitFillG *= 0.3f; pitFillB *= 0.3f; }
            }
        }

        // Limit and delta (current - limit; positive = still over, negative
        // = under), in the same units as the main speed readout.
        const double unitDiv = cfg.speedUnitsKmh ? 1.0 : 1.60934;
        const double limitDisplay = effectiveLimitKmh / unitDiv;
        const double speedDisplay = speedKmh / unitDiv;
        const double delta = speedDisplay - limitDisplay;

        char limitBuf[24];
        sprintf_s(limitBuf, "Limit %.0f", limitDisplay);
        char deltaBuf[24];
        sprintf_s(deltaBuf, "%+.1f", delta);

        const float pitTextW = renderer.MeasureText(pitText, plTextHeightPx);
        float contentW = pitTextW;
        float contentH = plTextHeightPx;
        const float lineGap = plInfoTextHeightPx * 0.3f;
        if (plCfg.showLimitAndDelta)
        {
            const float limitTextW = renderer.MeasureText(limitBuf, plInfoTextHeightPx);
            const float deltaTextW = renderer.MeasureText(deltaBuf, plDeltaTextHeightPx);
            if (limitTextW > contentW) contentW = limitTextW;
            if (deltaTextW > contentW) contentW = deltaTextW;
            contentH += lineGap + plInfoTextHeightPx + lineGap + plDeltaTextHeightPx;
        }

        const float pitHalfW = contentW * 0.5f + plPaddingPx;
        // Never shrinks below the dashboard row height, so a PIT box with no
        // extra info still lines up with gear/speed -- only grows taller
        // when the limit/delta lines don't fit within that height.
        float pitHalfH = contentH * 0.5f + plPaddingPx;
        if (pitHalfH < panelH * 0.5f) pitHalfH = panelH * 0.5f;

        const float dashboardRightEdge = cfg.speedEnabled ? (speedPanelCenterX + speedPanelW * 0.5f)
                                                            : (gearPanelCenterX + gearPanelW * 0.5f);
        const float pitCenterX = dashboardRightEdge + gapPx + pitHalfW;
        const float pitCenterY = panelCenterY + screenHeight * plCfg.offsetYFraction;

        renderer.AddFilledRoundedRect(pitCenterX, pitCenterY, pitHalfW, pitHalfH, bgCornerRadiusPx, 0.0f,
                                      pitFillR, pitFillG, pitFillB, fade);

        float lineY = pitCenterY - contentH * 0.5f;
        renderer.AddText(pitCenterX, lineY, pitText, plTextHeightPx,
                         plCfg.textR, plCfg.textG, plCfg.textB, fade, Renderer::HAlign::Center);
        if (plCfg.showLimitAndDelta)
        {
            lineY += plTextHeightPx + lineGap;
            renderer.AddText(pitCenterX, lineY, limitBuf, plInfoTextHeightPx,
                             plCfg.textR, plCfg.textG, plCfg.textB, fade, Renderer::HAlign::Center);
            lineY += plInfoTextHeightPx + lineGap;
            // Plain white, deliberately large -- the box's own fill colour
            // already carries the over/under-limit signal (red/green), so
            // coloring the delta the same way just makes it disappear into
            // the box whenever it matches (e.g. green text on a green box).
            renderer.AddText(pitCenterX, lineY, deltaBuf, plDeltaTextHeightPx,
                             plCfg.textR, plCfg.textG, plCfg.textB, fade, Renderer::HAlign::Center);
        }
    }

    if (state.showEngageLimiterWarning && plCfg.engageWarningEnabled)
    {
        const float warnBlink = TickBlink(plCfg.engageWarningBlinkPeriodSeconds);
        if (warnBlink < 0.5f)
        {
            const char* warnText = "ENGAGE PIT LIMITER";
            const float warnCenterX = screenWidth * plCfg.engageWarningXFraction;
            const float warnCenterY = screenHeight * plCfg.engageWarningYFraction;

            // Registered with the text box itself. AddText places the string
            // with warnCenterY as its TOP, so the box centre is half a line
            // below that.
            const float warnHalfW = renderer.MeasureText(warnText, plEngageWarningTextHeightPx) * 0.5f + bgPaddingPx;
            const float warnHalfH = plEngageWarningTextHeightPx * 0.5f + bgPaddingPx;
            Layout::Register("Pit limiter warning", "PitLimiter",
                             "EngageWarningXFraction", "EngageWarningYFraction",
                             &g_pitLimiterConfig.engageWarningXFraction,
                             &g_pitLimiterConfig.engageWarningYFraction,
                             warnCenterX, warnCenterY + plEngageWarningTextHeightPx * 0.5f,
                             warnHalfW, warnHalfH);

            if (cfg.backgroundEnabled)
            {
                const float warnTextW = renderer.MeasureText(warnText, plEngageWarningTextHeightPx);
                renderer.AddFilledRoundedRect(warnCenterX, warnCenterY + plEngageWarningTextHeightPx * 0.5f,
                                              warnTextW * 0.5f + bgPaddingPx, plEngageWarningTextHeightPx * 0.5f + bgPaddingPx,
                                              bgCornerRadiusPx, 0.0f, cfg.bgR, cfg.bgG, cfg.bgB, cfg.bgA * fade);
            }

            renderer.AddText(warnCenterX, warnCenterY,
                             warnText, plEngageWarningTextHeightPx,
                             plCfg.engageWarningR, plCfg.engageWarningG, plCfg.engageWarningB, fade, Renderer::HAlign::Center);
        }
    }

    // Rolling "how am I actually shifting" readout, below the whole
    // dashboard -- needs at least one observed upshift to show anything.
    // Gated by its own shortcut (Ctrl+Shift+V by default) as well as the INI
    // enable, same convention as the tyre lockup box.
    // Forced visible (with a placeholder) in layout mode too -- otherwise a
    // box that needs an observed upshift to show anything could never be
    // seen, let alone positioned, before you had actually driven this
    // session.
    if (cfg.showShiftStats && Shortcuts::Visible(Shortcuts::ShiftStats)
        && (state.shiftSampleCount > 0 || Layout::Active()))
    {
        // Car-wide average and 1st percentile, plus -- once the gear
        // currently engaged has shifts of its own on record -- that gear's
        // own average, which is the number the cue is actually using.
        char statsBuf[128];
        if (state.shiftSampleCount == 0)
        {
            strcpy_s(statsBuf, "Shift Avg -- | 1% --");
        }
        else if (state.gear >= 1 && state.gear < GearDisplayStore::State::kMaxGears
            && state.shiftSampleCountByGear[state.gear] > 0)
        {
            sprintf_s(statsBuf, "Shift Avg %.1f%% | 1%% %.1f%% | G%d %.1f%%",
                      state.shiftAvgPercent, state.shiftP1Percent,
                      state.gear, state.shiftAvgPercentByGear[state.gear]);
        }
        else
        {
            sprintf_s(statsBuf, "Shift Avg %.1f%% | 1%% %.1f%%", state.shiftAvgPercent, state.shiftP1Percent);
        }
        // Reaction time is what actually sets the cue's lead -- shown
        // alongside the RPM stats once at least one has been measured, as
        // "avg | last".
        if (state.shiftReactionSampleCount > 0)
        {
            char reactBuf[48];
            sprintf_s(reactBuf, " | React %.0fms | %.0fms",
                      state.shiftReactionAvgSeconds * 1000.0, state.shiftReactionLatestSeconds * 1000.0);
            strcat_s(statsBuf, reactBuf);
        }

        // Own independent scale/position -- resolved from the sentinel (-1,
        // -1) into an absolute fraction just below the gear panel the first
        // time this draws, then draggable/resizable on its own from there,
        // same convention as the shortcut list's own auto-anchor sentinel.
        if (cfg.shiftStatsPosXFraction < 0.0f || cfg.shiftStatsPosYFraction < 0.0f)
        {
            g_gearDisplayConfig.shiftStatsPosXFraction = panelCenterX / screenWidth;
            g_gearDisplayConfig.shiftStatsPosYFraction =
                (panelCenterY + panelH * 0.5f + shiftStatsGapPx) / screenHeight;
        }
        const float shiftStatsUiScale = static_cast<float>(screenHeight) / 2160.0f * cfg.shiftStatsScale;
        const float shiftStatsTextHeightPxScaled = cfg.shiftStatsTextHeightPx * shiftStatsUiScale;
        const float shiftStatsBgPaddingPxScaled = cfg.shiftStatsBgPaddingPx * shiftStatsUiScale;
        const float shiftStatsBgCornerRadiusPxScaled = cfg.shiftStatsBgCornerRadiusPx * shiftStatsUiScale;
        const float statsX = screenWidth * cfg.shiftStatsPosXFraction;
        const float statsY = screenHeight * cfg.shiftStatsPosYFraction;
        const float statsTextW = renderer.MeasureText(statsBuf, shiftStatsTextHeightPxScaled);
        const float statsHalfW = statsTextW * 0.5f + shiftStatsBgPaddingPxScaled;
        const float statsHalfH = shiftStatsTextHeightPxScaled * 0.5f + shiftStatsBgPaddingPxScaled;

        Layout::Register("Shift stats", "GearDisplay", "ShiftStatsPosXFraction", "ShiftStatsPosYFraction",
                         &g_gearDisplayConfig.shiftStatsPosXFraction, &g_gearDisplayConfig.shiftStatsPosYFraction,
                         statsX, statsY, statsHalfW, statsHalfH,
                         &g_gearDisplayConfig.shiftStatsScale, "ShiftStatsScale");

        // Same dark translucent plate every other readout box uses --
        // this used to draw bare text straight over whatever's behind it.
        if (cfg.shiftStatsBgEnabled)
        {
            renderer.AddFilledRoundedRect(statsX, statsY, statsHalfW, statsHalfH,
                                          shiftStatsBgCornerRadiusPxScaled, 0.0f,
                                          cfg.shiftStatsBgR, cfg.shiftStatsBgG, cfg.shiftStatsBgB, cfg.shiftStatsBgA * fade);
        }

        renderer.AddText(statsX, statsY - shiftStatsTextHeightPxScaled * 0.5f, statsBuf, shiftStatsTextHeightPxScaled,
                         cfg.shiftStatsR, cfg.shiftStatsG, cfg.shiftStatsB, fade, Renderer::HAlign::Center);
    }

    return true;
}
