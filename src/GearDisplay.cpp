#include "GearDisplay.h"
#include "FfbGraph.h"
#include "InternalsPlugin.hpp"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "RealtimeState.h"
#include "PedalTrace.h"
#include "TrackAlerts.h"
#include "Shortcuts.h"
#include "Log.h"
#include "RenderThrottle.h"
#include "StoreFile.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

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

    void GetPitLaneLimitPath(const char* key, char* out, size_t outSize, bool makeDir)
    {
        char dllPath[MAX_PATH] = {};
        HMODULE hModule = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCSTR>(&GetPitLaneLimitPath), &hModule);
        GetModuleFileNameA(hModule, dllPath, MAX_PATH);
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';
        char dir[MAX_PATH];
        sprintf_s(dir, "%sPitLaneLimits", dllPath);
        if (makeDir)
            CreateDirectoryA(dir, nullptr);
        sprintf_s(out, outSize, "%s\\%s.txt", dir, key);
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

    // The saved files, read and written OUTSIDE m_lock: reads from Update()
    // before it takes the lock, writes from FlushIfDirty() (the save thread)
    // after it has let go -- so neither the telemetry callback's own work nor
    // the render thread's Read() ever waits behind the disk.

    // 0 = nothing saved (or an implausible saved value).
    double ReadPitLimitFile(const char* key, const char* vehicleName, const char* trackName)
    {
        char dir[MAX_PATH];
        GetPitLimitDir(dir, sizeof(dir));
        char path[MAX_PATH];
        sprintf_s(path, "%s\\%s.txt", dir, key);

        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") != 0 || !f)
        {
            Log::Write("GearDisplay: no saved pit limit yet for %s / %s", vehicleName, trackName);
            return 0.0;
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
            Log::Write("GearDisplay: ignoring implausible saved pit limit %.2f km/h for %s / %s", loaded, vehicleName, trackName);
            return 0.0;
        }
        Log::Write("GearDisplay: loaded pit limit %.1f km/h for %s / %s", loaded, vehicleName, trackName);
        return loaded;
    }

    // The pit-lane limit set for a track; 0 = none set.
    double ReadPitLaneLimitFile(const char* key)
    {
        char path[MAX_PATH];
        GetPitLaneLimitPath(key, path, sizeof(path), false);
        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") != 0 || !f)
            return 0.0;
        char line[64];
        const double kmh = fgets(line, sizeof(line), f) ? atof(line) : 0.0;
        fclose(f);
        if (kmh < 10.0 || kmh > 400.0)
            return 0.0;
        Log::Write("GearDisplay: pit lane limit for %s is %.0f km/h (set)", key, kmh);
        return kmh;
    }

    // kmh <= 0 removes the track's file (back to the learned limit).
    void WritePitLaneLimitFile(const char* key, double kmh)
    {
        char path[MAX_PATH];
        GetPitLaneLimitPath(key, path, sizeof(path), kmh > 0.0);
        if (kmh <= 0.0)
        {
            DeleteFileA(path);
            Log::Write("GearDisplay: pit lane limit for %s cleared", key);
            return;
        }
        char tmp[StoreFile::kTmpPathSize];
        FILE* f = StoreFile::Begin(path, tmp, sizeof(tmp), "w");
        if (!f || (fprintf(f, "%.1f\n", kmh), !StoreFile::Commit(f, tmp, path)))
        {
            Log::Write("GearDisplay: failed to save pit lane limit to %s", path);
            return;
        }
        Log::Write("GearDisplay: saved pit lane limit %.0f km/h to %s", kmh, path);
    }

    void WritePitLimitFile(const char* key, double kmh)
    {
        char dir[MAX_PATH];
        GetPitLimitDir(dir, sizeof(dir));
        CreateDirectoryA(dir, nullptr); // no-op (ERROR_ALREADY_EXISTS) if it's already there

        char path[MAX_PATH];
        sprintf_s(path, "%s\\%s.txt", dir, key);

        char tmp[StoreFile::kTmpPathSize];
        FILE* f = StoreFile::Begin(path, tmp, sizeof(tmp), "w");
        if (!f)
        {
            Log::Write("GearDisplay: failed to save pit limit to %s", path);
            return;
        }
        fprintf(f, "%.2f\n", kmh);
        if (!StoreFile::Commit(f, tmp, path))
        {
            Log::Write("GearDisplay: failed to save pit limit to %s", path);
            return;
        }
        Log::Write("GearDisplay: saved pit limit %.1f km/h to %s", kmh, path);
    }

    // Shift targets, one file per car ENTRY (ShiftTargets\<entry>.txt):
    // line 1 the target fraction of max RPM, then one line per gear (from
    // 1st) with that gear's learned cue correction. A car with no file yet
    // starts from the old per-CLASS learned shift point (ShiftPoints\
    // <class>.txt line 1, rounded to whole percent) and no corrections.
    void GetShiftTargetDir(char* out, size_t outSize)
    {
        char dir[MAX_PATH];
        GetShiftPointDir(dir, sizeof(dir));
        char* slash = strrchr(dir, '\\');
        if (slash) *(slash + 1) = '\0';
        sprintf_s(out, outSize, "%sShiftTargets", dir);
    }

    void ReadShiftTargetFile(const char* key, const char* classKey, double& target,
                             double corr[GearDisplayStore::State::kMaxGears])
    {
        target = 0.0;
        for (int g = 0; g < GearDisplayStore::State::kMaxGears; ++g) corr[g] = 0.0;
        char dir[MAX_PATH], path[MAX_PATH];
        GetShiftTargetDir(dir, sizeof(dir));
        sprintf_s(path, "%s\\%s.txt", dir, key);
        FILE* f = nullptr;
        char line[64];
        if (fopen_s(&f, path, "r") == 0 && f)
        {
            target = fgets(line, sizeof(line), f) ? atof(line) : 0.0;
            for (int g = 1; g < GearDisplayStore::State::kMaxGears; ++g)
            {
                if (!fgets(line, sizeof(line), f)) break;
                corr[g] = atof(line);
            }
            fclose(f);
            Log::Write("GearDisplay: shift target %.0f%% of max RPM for %s", target * 100.0, key);
            return;
        }
        // Seed from the old per-class file.
        GetShiftPointDir(dir, sizeof(dir));
        sprintf_s(path, "%s\\%s.txt", dir, classKey);
        if (fopen_s(&f, path, "r") == 0 && f)
        {
            const double old = fgets(line, sizeof(line), f) ? atof(line) : 0.0;
            fclose(f);
            if (old > 0.5 && old <= 1.0)
                target = floor(old * 100.0 + 0.5) / 100.0;
        }
        Log::Write("GearDisplay: no shift target yet for %s -- starting at %.0f%%", key,
                   (target > 0.0 ? target : g_gearDisplayConfig.revLimiterRpmFraction) * 100.0);
    }

    void WriteShiftTargetFile(const char* key, double target, const double corr[GearDisplayStore::State::kMaxGears])
    {
        char dir[MAX_PATH];
        GetShiftTargetDir(dir, sizeof(dir));
        CreateDirectoryA(dir, nullptr); // no-op (ERROR_ALREADY_EXISTS) if it's already there
        char path[MAX_PATH];
        sprintf_s(path, "%s\\%s.txt", dir, key);
        char tmp[StoreFile::kTmpPathSize];
        FILE* f = StoreFile::Begin(path, tmp, sizeof(tmp), "w");
        if (!f)
        {
            Log::Write("GearDisplay: failed to save shift target to %s", path);
            return;
        }
        fprintf(f, "%.4f\n", target);
        for (int g = 1; g < GearDisplayStore::State::kMaxGears; ++g)
            fprintf(f, "%.4f\n", corr[g]);
        if (!StoreFile::Commit(f, tmp, path))
            Log::Write("GearDisplay: failed to save shift target to %s", path);
    }
}

void GearDisplayStore::Init()
{
    if (m_ready)
        return;
    InitializeCriticalSection(&m_lock);

    m_ready = true;
}

void GearDisplayStore::Update(const TelemInfoV01& info)
{
    if (!m_ready)
        return;

    const double speed = sqrt(info.mLocalVel.x * info.mLocalVel.x +
                              info.mLocalVel.y * info.mLocalVel.y +
                              info.mLocalVel.z * info.mLocalVel.z);
    const bool limiterOn = info.mSpeedLimiter != 0 && !g_playerAiControl.load(std::memory_order_relaxed);

    // Car/track change. The raw names are compared with last tick's copies
    // first, so an ordinary tick builds no strings at all; the sanitized keys
    // are only worked out on an actual change, and the saved files read here,
    // BEFORE m_lock -- the render thread's Read() takes that lock every frame.
    // m_currentKey/m_shiftKey are only ever written by this thread (under the
    // lock, for FlushIfDirty), so reading them here without it is safe.
    // Vehicle CLASS, shared from the scoring thread -- see RealtimeState.h.
    char vehKey[64];
    VehicleKey::Get(vehKey, sizeof(vehKey));
    const bool rekeyAfterClear = m_keysInvalidated.exchange(false);
    bool pitKeyChanged = false, shiftKeyChanged = false, laneKeyChanged = false;
    std::string newPitKey, newShiftKey, newLaneKey;
    double loadedPitLimitKmh = 0.0, loadedLaneLimitKmh = 0.0;
    double loadedShiftTarget = 0.0;
    double loadedShiftCorr[State::kMaxGears] = {};
    if (rekeyAfterClear || !m_haveRawNames
        || strncmp(vehKey, m_rawVehKey, sizeof(m_rawVehKey)) != 0
        || strncmp(info.mTrackName, m_rawTrackName, sizeof(m_rawTrackName)) != 0
        || strncmp(info.mVehicleName, m_rawEntryName, sizeof(m_rawEntryName)) != 0)
    {
        memcpy(m_rawVehKey, vehKey, sizeof(m_rawVehKey));
        memcpy(m_rawTrackName, info.mTrackName, sizeof(m_rawTrackName));
        memcpy(m_rawEntryName, info.mVehicleName, sizeof(m_rawEntryName));
        m_haveRawNames = true;
        // Shift targets are per car ENTRY (asked for 2026-11-24: one class
        // holds cars with very different engines); the pit limit is per
        // vehicle class + track.
        const std::string classKey = Sanitize(vehKey, sizeof(vehKey));
        newShiftKey = info.mVehicleName[0] ? Sanitize(info.mVehicleName, sizeof(info.mVehicleName)) : classKey;
        newPitKey = classKey + "__" + Sanitize(info.mTrackName, sizeof(info.mTrackName));
        // Clear() forgets the pit-limit key, so the next tick reloads it
        // even for the same car and track.
        pitKeyChanged = rekeyAfterClear || newPitKey != m_currentKey;
        shiftKeyChanged = newShiftKey != m_shiftKey;
        if (pitKeyChanged)
            loadedPitLimitKmh = ReadPitLimitFile(newPitKey.c_str(), vehKey, info.mTrackName);
        // The set pit-lane limit is the track's, whatever the car.
        newLaneKey = Sanitize(info.mTrackName, sizeof(info.mTrackName));
        laneKeyChanged = rekeyAfterClear || newLaneKey != m_laneKey;
        if (laneKeyChanged)
            loadedLaneLimitKmh = ReadPitLaneLimitFile(newLaneKey.c_str());
        if (shiftKeyChanged)
            ReadShiftTargetFile(newShiftKey.c_str(), classKey.c_str(), loadedShiftTarget, loadedShiftCorr);
    }

    EnterCriticalSection(&m_lock);

    if (pitKeyChanged)
    {
        m_currentKey = newPitKey;
        m_state.learnedPitLimitKmh = loadedPitLimitKmh;
        m_lastSpeedLimiterOn = false;
        m_episodeMaxKmh = 0.0;
    }
    if (laneKeyChanged)
    {
        m_laneKey = newLaneKey;
        m_state.pitLaneLimitKmh = loadedLaneLimitKmh;
    }
    if (shiftKeyChanged)
    {
        m_shiftKey = newShiftKey;
        m_state.shiftTargetFraction = loadedShiftTarget;
        for (int g = 0; g < State::kMaxGears; ++g)
            m_state.cueCorrectionByGear[g] = loadedShiftCorr[g];
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
                // Written later by FlushIfDirty() on the save thread, at the
                // next line crossing. Key and value are copied now, so a
                // car/track change before then still saves to the right file.
                strncpy_s(m_pitSaveKey, m_currentKey.c_str(), _TRUNCATE);
                m_pitSaveKmh = m_episodeMaxKmh;
                m_pitLimitDirty = true;
            }
        }
        m_episodeMaxKmh = 0.0;
    }
    m_lastSpeedLimiterOn = limiterOn;

    // Shift-point learning (the vehicle-only key and its file are handled
    // at the top of Update(), with the pit limit's).
    const int gear = static_cast<int>(info.mGear);
    if (m_hasLastShiftSample && gear > m_lastGearForShift && m_lastGearForShift >= 1 && m_state.maxRpm > 100.0)
    {
        // m_lastRpmForShift is the RPM from the sample just BEFORE the gear
        // change registered -- i.e. the RPM you actually changed gear at.
        const double shiftRpm = m_lastRpmForShift;
        // The gear this upshift left -- the one whose cue needs to fire, and
        // so the one that learns from this sample.
        const int fromGear = (m_lastGearForShift < State::kMaxGears) ? m_lastGearForShift : (State::kMaxGears - 1);
        // Where this shift landed vs the target you set: + = later (higher
        // RPM) than the target, - = earlier.
        const double actualFraction = shiftRpm / m_state.maxRpm;
        const double target = (m_state.shiftTargetFraction > 0.0)
                            ? m_state.shiftTargetFraction
                            : g_gearDisplayConfig.revLimiterRpmFraction;
        const double gearError = actualFraction - target;
        // Only a shift made in response to the cue says anything about the
        // cue's timing: shifting before it lit up is your own choice. Also
        // counts a shift just after the cue went out -- lifting to shift
        // drops it (wantsToAccelerate) a moment before the gear changes.
        const bool wasCued = m_shiftCuePending
            || (m_shiftCueArmedET > 0.0 && info.mElapsedTime - m_shiftCueArmedET >= 0.0
                && info.mElapsedTime - m_shiftCueArmedET <= g_gearDisplayConfig.shiftReactionMaxSeconds);

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

            // Outlier gate: a shift this far off the target is a different
            // event (missed gear, lifting off a spinning wheel ...).
            const double outlierBand = g_gearDisplayConfig.shiftLearnOutlierBand;
            if (!wasCued)
            {
                Log::Write("GearDisplay: shift out of gear %d at %.1f%% (target %.0f%%) -- before the cue, timing not learned",
                           fromGear, actualFraction * 100.0, target * 100.0);
            }
            else if (outlierBand > 0.0 && fabs(gearError) > outlierBand)
            {
                Log::Write("GearDisplay: shift out of gear %d at %.1f%% (target %.0f%%) -- OUTLIER, not learned",
                           fromGear, actualFraction * 100.0, target * 100.0);
            }
            else
            {
                // Landed late -> fire earlier next time, and the reverse.
                // A fraction of the error each time, so one odd shift does
                // not swing it; within ~0.3% is on target.
                constexpr double kToleranceFraction = 0.003;
                constexpr double kLearnRate = 0.3;
                double& corr = m_state.cueCorrectionByGear[fromGear];
                if (fabs(gearError) > kToleranceFraction)
                {
                    corr += kLearnRate * gearError;
                    if (corr < -0.05) corr = -0.05;
                    if (corr > 0.10) corr = 0.10;
                    strncpy_s(m_shiftSaveKey, m_shiftKey.c_str(), _TRUNCATE);
                    m_shiftSaveTarget = target;
                    memcpy(m_shiftSaveCorr, m_state.cueCorrectionByGear, sizeof(m_shiftSaveCorr));
                    m_shiftPointDirty = true;
                }
                Log::Write("GearDisplay: shift out of gear %d at %.1f%% (target %.0f%%, %s) -- gear %d cue correction now %+.3f",
                           fromGear, actualFraction * 100.0, target * 100.0,
                           gearError > kToleranceFraction ? "late" : gearError < -kToleranceFraction ? "early" : "on target",
                           fromGear, corr);
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
        const double targetFraction = (m_state.shiftTargetFraction > 0.0)
                                    ? m_state.shiftTargetFraction
                                    : g_gearDisplayConfig.revLimiterRpmFraction;
        const double gearCorr = (gear >= 1 && gear < State::kMaxGears) ? m_state.cueCorrectionByGear[gear] : 0.0;

        // Only means anything while genuinely trying to accelerate -- a
        // rev-matched downshift's RPM blip happens with the driver already
        // OFF the power and about to brake, and without this gate it could
        // both light the cue and then LATCH it on for the whole braking
        // zone (the latch below only releases on an RPM drop, which engine
        // braking may not deliver quickly). Checked every tick, including
        // while latched, so lifting off the throttle releases it
        // immediately rather than waiting on the RPM hysteresis.
        const bool wantsToAccelerate = info.mUnfilteredThrottle >= g_gearDisplayConfig.shiftCueMinThrottle;

        // Nothing to shift into in top gear (mMaxGears = the car's forward
        // gears; 0 = not reported, then no limit). Asked for 2026-11-23.
        const bool topGear = info.mMaxGears > 0 && gear >= static_cast<int>(info.mMaxGears);
        bool cueActive = false;
        if (gear >= 1 && !topGear && m_state.maxRpm > 1.0 && wantsToAccelerate)
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
                // Reaction-time lead, plus this gear's learned correction so
                // the shift actually lands on the target.
                double cueRpm = targetRpm - leadRpm - gearCorr * m_state.maxRpm;
                if (cueRpm < m_state.maxRpm * 0.5) cueRpm = m_state.maxRpm * 0.5;
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
    // Callers flush (Persist::FlushAllNow) before Clear() -- see Plugin.cpp.
    // A save still waiting regardless (that flush timed out) is kept rather
    // than dropped: the save slots carry their own key and values, so they
    // can no longer end up written under a cleared key, which is why they
    // used to be dropped here.
    m_lastSpeedLimiterOn = false;
    m_episodeMaxKmh = 0.0;
    LeaveCriticalSection(&m_lock);
    // The pit-limit key is forgotten, so the next session reloads it from
    // disk even for the same car and track. Only Update() (telemetry) touches
    // the key, so it is told to rather than the key being cleared from here.
    m_keysInvalidated.store(true);
}

void GearDisplayStore::AdjustShiftTarget(int steps)
{
    if (!m_ready || steps == 0)
        return;
    EnterCriticalSection(&m_lock);
    double t = m_state.shiftTargetFraction > 0.0 ? m_state.shiftTargetFraction : g_gearDisplayConfig.revLimiterRpmFraction;
    int pct = static_cast<int>(floor(t * 100.0 + 0.5)) + steps;
    if (pct < 70) pct = 70;
    if (pct > 100) pct = 100;
    m_state.shiftTargetFraction = pct / 100.0;
    if (!m_shiftKey.empty())
    {
        strncpy_s(m_shiftSaveKey, m_shiftKey.c_str(), _TRUNCATE);
        m_shiftSaveTarget = m_state.shiftTargetFraction;
        memcpy(m_shiftSaveCorr, m_state.cueCorrectionByGear, sizeof(m_shiftSaveCorr));
        m_shiftPointDirty = true;
    }
    LeaveCriticalSection(&m_lock);
}

void GearDisplayStore::FlushIfDirty()
{
    if (!m_ready)
        return;
    // Save thread, any time. Copy what is dirty under the lock, write after
    // letting go: Update() (telemetry) and Read() (render, every frame) both
    // take m_lock.
    bool savePit = false, saveShift = false;
    char pitKey[sizeof(m_pitSaveKey)], shiftKey[sizeof(m_shiftSaveKey)];
    double pitKmh = 0.0, shiftTarget = 0.0;
    double shiftCorr[State::kMaxGears] = {};
    EnterCriticalSection(&m_lock);
    if (m_pitLimitDirty)
    {
        memcpy(pitKey, m_pitSaveKey, sizeof(pitKey));
        pitKmh = m_pitSaveKmh;
        m_pitLimitDirty = false;
        savePit = pitKey[0] != '\0';
    }
    if (m_shiftPointDirty)
    {
        memcpy(shiftKey, m_shiftSaveKey, sizeof(shiftKey));
        shiftTarget = m_shiftSaveTarget;
        memcpy(shiftCorr, m_shiftSaveCorr, sizeof(shiftCorr));
        m_shiftPointDirty = false;
        saveShift = shiftKey[0] != '\0';
    }
    LeaveCriticalSection(&m_lock);

    if (savePit)
        WritePitLimitFile(pitKey, pitKmh);
    if (saveShift)
        WriteShiftTargetFile(shiftKey, shiftTarget, shiftCorr);
}

void GearDisplayStore::FlushSettingsIfDirty()
{
    if (!m_ready)
        return;
    char key[sizeof(m_laneSaveKey)];
    double kmh = 0.0;
    EnterCriticalSection(&m_lock);
    const bool save = m_laneLimitDirty && m_laneSaveKey[0] != '\0';
    m_laneLimitDirty = false;
    memcpy(key, m_laneSaveKey, sizeof(key));
    kmh = m_laneSaveKmh;
    LeaveCriticalSection(&m_lock);
    if (save)
        WritePitLaneLimitFile(key, kmh);
}

void GearDisplayStore::SetPitLaneLimit(double kmh)
{
    if (!m_ready)
        return;
    if (kmh < 0.0) kmh = 0.0;
    EnterCriticalSection(&m_lock);
    // Copied with its track now, so leaving the track before the save still
    // writes it to the right file.
    if (!m_laneKey.empty())
    {
        m_state.pitLaneLimitKmh = kmh;
        strncpy_s(m_laneSaveKey, m_laneKey.c_str(), _TRUNCATE);
        m_laneSaveKmh = kmh;
        m_laneLimitDirty = true;
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
    // The pit-lane limit: the one set for this track, else the learned one.
    const double pitLimitKmh = state.pitLaneLimitKmh > 0.0 ? state.pitLaneLimitKmh : state.learnedPitLimitKmh;
    const bool pitOverSpeed = state.speedLimiterOn && pitLimitKmh > 0.0 && state.speedMs * 3.6 > pitLimitKmh + 0.5;
    if (state.speedLimiterOn) strcpy_s(absTcBuf, pitOverSpeed ? "OVER SPEED" : "PIT");

    // Forced visible (with a placeholder) in layout mode -- otherwise a box
    // that only ever appears while ABS/TC is actively cutting could never be
    // seen, let alone dragged into place, during a normal setup session.
    const bool absTcPlaceholder = Layout::Preview() && absTcBuf[0] == 0;
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
    // OVER SPEED is wider: the box grows (to the left, away from the gear
    // digit) only while it says that.
    const float absTcTextW = renderer.MeasureText(pitOverSpeed ? "OVER SPEED" : "ABS TC", absTcTextHeightPx);
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

    // Pit limiter on: the ABS/TC box steps aside; the PIT LIMITER bar drawn
    // across the whole dashboard row further down takes over.
    // The full-width PIT LIMITER bar is retired (2026-11-21): the limiter now
    // shows only as "PIT" flashing in the ABS/TC box, so gear and speed stay
    // visible.
    const bool pitSquare = false;
    if (pitSquare) {}
    else if (showAbsTc)
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
                // Pit limiter on: green at or under the pit-lane limit, red
                // reading OVER SPEED above it. The limit is the one set for
                // this track (wheel knob), else the one learned from driving
                // with the limiter on (PitLimits\<car>_<track>.txt); with
                // neither, amber.
                // Flashes the whole time the limiter is on; twice as fast
                // while over the limit.
                const bool fastBlink = TickBlink(cfg.blinkPeriodSeconds * 0.5) < 0.5f;
                float r = 1.0f, g = 0.65f, b = 0.1f, a = cfg.bgA * fade * (blinkOn ? 1.0f : 0.3f); // amber: limit unknown
                if (pitOverSpeed)
                { r = 0.95f; g = 0.12f; b = 0.10f; a = cfg.bgA * fade * (fastBlink ? 1.0f : 0.25f); }
                else if (pitLimitKmh > 0.0)
                { r = 0.15f; g = 0.85f; b = 0.25f; }
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

    // While the PIT LIMITER bar sits over this row, the gear and speed
    // panels are not drawn at all: text is layered above every filled box,
    // so the digits would otherwise show through the bar. Moved clear of
    // the row, the bar leaves them alone.
    bool hideRowForPitBar = false;
    if (pitSquare)
    {
        const PitLimiterConfig& pl = g_pitLimiterConfig;
        if (pl.barXFraction < 0.0f || pl.barYFraction < 0.0f)
            hideRowForPitBar = true;
        else
        {
            const float rowLeft = cfg.absTcBoxEnabled ? (absTcCenterX - absTcPanelW * 0.5f) : (gearPanelCenterX - gearPanelW * 0.5f);
            const float rowRight = cfg.speedEnabled ? (speedPanelCenterX + speedPanelW * 0.5f) : (gearPanelCenterX + gearPanelW * 0.5f);
            const float bs = (pl.barScale > 0.05f) ? pl.barScale : 0.05f;
            const float bhw = (rowRight - rowLeft) * 0.5f * bs, bhh = panelH * 0.5f * bs;
            const float bx = screenWidth * pl.barXFraction, by = screenHeight * pl.barYFraction;
            hideRowForPitBar = bx - bhw < rowRight && bx + bhw > rowLeft
                            && by - bhh < panelCenterY + panelH * 0.5f && by + bhh > panelCenterY - panelH * 0.5f;
        }
    }
    if (!hideRowForPitBar)
    {
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

    } // !hideRowForPitBar

    // --- Warning lines above the dashboard (TrackAlerts.h) ---
    // Nearest the speed box: the pit-lane limit once a pit stop is
    // requested, until the limiter is on (the PIT box takes over then).
    // Above that, centred on gear + speed: a stopped/slow car ahead, yellow
    // sectors, or a full-course yellow.
    {
        const TrackAlertConfig& ta = g_trackAlertConfig;
        const TrackAlertStore::State al = g_trackAlerts.Read();
        const float taTextPx = ta.textHeightPx * uiScale;
        const float taGapPx = ta.gapPx * uiScale;
        const float rowTop = panelCenterY - panelH * 0.5f;
        float lineBottom = rowTop - taGapPx;   // bottom edge of the next line up
        const double unitDiv = cfg.speedUnitsKmh ? 1.0 : 1.60934;
        const char* unitName = cfg.speedUnitsKmh ? "km/h" : "mph";
        auto drawLine = [&](float cx, const char* text, bool visible)
        {
            const float w = renderer.MeasureText(text, taTextPx);
            const float cy = lineBottom - bgPaddingPx * 0.5f - taTextPx * 0.5f;
            if (visible)
            {
                if (cfg.backgroundEnabled)
                    renderer.AddFilledRoundedRect(cx, cy, w * 0.5f + bgPaddingPx * 0.5f, taTextPx * 0.5f + bgPaddingPx * 0.5f,
                                                  bgCornerRadiusPx, 0.0f, cfg.bgR, cfg.bgG, cfg.bgB, cfg.bgA * fade);
                renderer.AddText(cx, cy - taTextPx * 0.5f, text, taTextPx, ta.r, ta.g, ta.b, fade, Renderer::HAlign::Center);
            }
            lineBottom = cy - taTextPx * 0.5f - bgPaddingPx * 0.5f - taGapPx;
        };

        if (ta.enabled && ta.showPitLimitReminder && al.valid && al.pitRequested && !state.speedLimiterOn)
        {
            char pitBuf[48];
            if (state.pitLaneLimitKmh > 0.0)
                sprintf_s(pitBuf, "PIT LIMIT %.0f %s", state.pitLaneLimitKmh / unitDiv, unitName);
            else if (state.learnedPitLimitKmh > 0.0)
                sprintf_s(pitBuf, "PIT LIMIT ~%.0f %s", state.learnedPitLimitKmh / unitDiv, unitName);
            else
                strcpy_s(pitBuf, "PIT LIMIT");
            drawLine(cfg.speedEnabled ? speedPanelCenterX : gearPanelCenterX, pitBuf, true);
        }

        if (ta.enabled && al.valid)
        {
            char alertBuf[128] = "";
            bool flash = false;
            if (ta.showSectorYellow)
            {
                for (int i = 0; i < 3; ++i)
                {
                    if (!al.sectorYellow[i]) continue;
                    char part[16];   // "YELLOW S1" is 9 characters + the terminator
                    sprintf_s(part, alertBuf[0] ? " S%d" : "YELLOW S%d", i + 1);
                    strcat_s(alertBuf, part);
                }
            }
            if (al.haveCar)
            {
                const double d = TrackAlertStore::DistanceNow(al);
                char part[64], count[16] = "";
                if (al.carCount > 1)
                    sprintf_s(count, " (%d cars)", al.carCount);
                sprintf_s(part, "%sSTOPPED %.0f m%s%s", alertBuf[0] ? "   " : "",
                          d, count, al.carOffTrack ? " OFF TRACK" : "");
                strcat_s(alertBuf, part);
                flash = d <= ta.flashWithinMeters;
            }
            else if (al.fullCourseYellow && !alertBuf[0])
            {
                strcpy_s(alertBuf, "FULL COURSE YELLOW");
            }
            if (alertBuf[0])
            {
                const float left = gearPanelCenterX - gearPanelW * 0.5f;
                const float right = cfg.speedEnabled ? (speedPanelCenterX + speedPanelW * 0.5f) : (gearPanelCenterX + gearPanelW * 0.5f);
                drawLine((left + right) * 0.5f, alertBuf, !flash || blinkOn);
            }
        }
    }

    // Pit limiter engaged: one red-orange bar laid across the whole
    // ABS/TC + gear + speed row (in the line of sight, unlike a side box),
    // reading "PIT LIMITER" and speed minus the pit-lane limit. Flashes once
    // more than half a km/h over the limit.
    const bool pitBarPlaceholder = false;   // bar retired, see pitSquare
    if (pitSquare || pitBarPlaceholder)
    {
        PitLimiterConfig& pl = g_pitLimiterConfig;
        const double limitKmh = (state.learnedPitLimitKmh > 0.0) ? state.learnedPitLimitKmh : pl.speedLimitKmh;
        const double speedKmhNow = state.speedMs * 3.6;
        const double unitDiv = cfg.speedUnitsKmh ? 1.0 : 1.60934;
        char barBuf[40];
        // Gear and speed rather than speed-over-limit: the bar covers the
        // gear/speed row, so it carries them itself.
        (void)limitKmh;
        sprintf_s(barBuf, "PIT   %s   %.0f", gearBuf, speedKmhNow / unitDiv);

        const float left = cfg.absTcBoxEnabled ? (absTcCenterX - absTcPanelW * 0.5f)
                                               : (gearPanelCenterX - gearPanelW * 0.5f);
        const float right = cfg.speedEnabled ? (speedPanelCenterX + speedPanelW * 0.5f)
                                             : (gearPanelCenterX + gearPanelW * 0.5f);
        // Default size is the dashboard row; BarScale grows/shrinks it, and
        // BarX/YFraction move it (-1 = auto, over the row, resolved to real
        // fractions the first time so a drag starts from where it is).
        if (pl.barXFraction < 0.0f || pl.barYFraction < 0.0f)
        {
            pl.barXFraction = ((left + right) * 0.5f) / screenWidth;
            pl.barYFraction = panelCenterY / screenHeight;
        }
        const float bs = (pl.barScale > 0.05f) ? pl.barScale : 0.05f;
        const float barHalfW = (right - left) * 0.5f * bs;
        const float barHalfH = panelH * 0.5f * bs;
        const float cx = screenWidth * pl.barXFraction;
        const float cy = screenHeight * pl.barYFraction;
        Layout::Register("Pit limiter bar", "PitLimiter", "BarXFraction", "BarYFraction",
                         &g_pitLimiterConfig.barXFraction, &g_pitLimiterConfig.barYFraction,
                         cx, cy, barHalfW, barHalfH, &g_pitLimiterConfig.barScale, "BarScale");
        float textH = barHalfH;
        const float avail = barHalfW * 2.0f - bgPaddingPx * 2.0f * bs;
        const float w = renderer.MeasureText(barBuf, textH);
        if (w > avail && w > 0.0f) textH *= avail / w;
        const bool over = pitSquare && speedKmhNow > limitKmh + 0.5;
        const float a = (over && !blinkOn) ? 0.4f * fade : fade;
        renderer.AddFilledRoundedRect(cx, cy, barHalfW, barHalfH,
                                      bgCornerRadiusPx, 0.0f, 1.0f, 0.32f, 0.05f, pitBarPlaceholder ? 0.5f * fade : a);
        renderer.AddText(cx, cy - textH * 0.5f, barBuf, textH,
                         1.0f, 1.0f, 1.0f, fade, Renderer::HAlign::Center);
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

    // Rolling "how am I actually shifting" readout, below the whole
    // dashboard -- needs at least one observed upshift to show anything.
    // Gated by its own shortcut (Ctrl+Shift+V by default) as well as the INI
    // enable, same convention as the tyre lockup box.
    // Forced visible (with a placeholder) in layout mode too -- otherwise a
    // box that needs an observed upshift to show anything could never be
    // seen, let alone positioned, before you had actually driven this
    // session.
    if (cfg.showShiftStats && Shortcuts::Visible(Shortcuts::ShiftStats)
        && (state.shiftSampleCount > 0 || Layout::Preview()))
    {
        // Car-wide average and 1st percentile, plus -- once the gear
        // currently engaged has shifts of its own on record -- that gear's
        // own average, which is the number the cue is actually using.
        // The gear part is always there, so the line does not change length
        // (and the box size) at every gear change: in neutral -- which every
        // shift passes through -- it keeps the last gear's figure, and a
        // gear with no shifts on record yet shows "--".
        char statsBuf[128];
        static char s_gearPart[32] = "";
        if (state.gear >= 1 && state.gear < GearDisplayStore::State::kMaxGears)
        {
            if (state.shiftSampleCountByGear[state.gear] > 0)
                sprintf_s(s_gearPart, " | G%d %.1f%%", state.gear, state.shiftAvgPercentByGear[state.gear]);
            else
                sprintf_s(s_gearPart, " | G%d --", state.gear);
        }
        if (state.shiftSampleCount == 0)
        {
            strcpy_s(statsBuf, "Shift Avg -- | 1% --");
        }
        else
        {
            sprintf_s(statsBuf, "Target %.0f%% | Avg %.1f%% | 1%% %.1f%%%s",
                      (state.shiftTargetFraction > 0.0 ? state.shiftTargetFraction : cfg.revLimiterRpmFraction) * 100.0,
                      state.shiftAvgPercent, state.shiftP1Percent, s_gearPart);
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
        // Width only ever grows (reset when the box is resized), so it does
        // not twitch as the numbers change; the text is left-aligned in it.
        static float s_statsW = 0.0f, s_statsPx = 0.0f;
        if (s_statsPx != shiftStatsTextHeightPxScaled) { s_statsPx = shiftStatsTextHeightPxScaled; s_statsW = 0.0f; }
        const float statsTextW = renderer.MeasureText(statsBuf, shiftStatsTextHeightPxScaled);
        if (statsTextW > s_statsW) s_statsW = statsTextW;
        const float statsHalfW = s_statsW * 0.5f + shiftStatsBgPaddingPxScaled;
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

        renderer.AddText(statsX - s_statsW * 0.5f, statsY - shiftStatsTextHeightPxScaled * 0.5f, statsBuf,
                         shiftStatsTextHeightPxScaled,
                         cfg.shiftStatsR, cfg.shiftStatsG, cfg.shiftStatsB, fade, Renderer::HAlign::Left);
    }

    return true;
}
