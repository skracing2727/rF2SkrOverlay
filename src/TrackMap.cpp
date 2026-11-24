#include "Conditions.h"
#include "TrackMap.h"
#include "InternalsPlugin.hpp"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include "DriverAbbrev.h"
#include "Log.h"
#include "RenderThrottle.h"
#include "LapDelta.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <atomic>
#include <algorithm>

namespace
{
    const int kMaxCars = 128;

    struct Pt { float x, z; };

    // --- Scoring -> render handoff (all under g_lock) -----------------------
    //
    // This module keeps its OWN car snapshot rather than reading CarState:
    // CarState holds world x/z, and the map places cars by lap distance (see
    // PlaceByLapDist).
    struct CarSample
    {
        long   id = -1;
        double lapDist = 0.0;
        double pathLateral = 0.0;
        bool   isPlayer = false;
    };

    CRITICAL_SECTION g_lock;
    bool g_lockReady = false;

    char g_pendingTrack[64] = {};
    bool g_pendingChanged = false;

    CarSample g_curCars[kMaxCars];
    int g_curCount = 0;
    LARGE_INTEGER g_curTime = {};
    CarSample g_prevCars[kMaxCars];
    int g_prevCount = 0;
    LARGE_INTEGER g_prevTime = {};

    std::atomic<long long> g_lastScoringQpc{ 0 };
    LARGE_INTEGER g_qpcFreq = {};

    // A map ready to show: what BuildMapData makes from a file's or a
    // recording's points. Built off the render thread (the map-load worker,
    // the scoring thread); Draw only swaps it in (ApplyMap).
    struct MapData
    {
        std::vector<Pt>    raw;       // as read from the file / recording
        std::vector<float> lapDist;   // lap distance at each index, strictly increasing
        float trackLength = 0.0f;
        int   s1 = -1, s2 = -1;       // sector indices into both
        bool  ok = false;
    };

    // A finished recording, queued for the disk write (g_saveQueue, see
    // FlushIfDirty). The render thread gets it separately, already built
    // into a map (g_newMap), to show at once.
    struct Recording
    {
        char track[64] = {};
        std::vector<Pt> map;   // (x, -z)
        std::vector<Pt> dist;  // (lapDist, elevation)
        int s1 = -1, s2 = -1;
    };
    char    g_newMapTrack[64] = {};
    MapData g_newMap;
    bool    g_haveNewRecording = false;
    Recording g_saveQueue;
    bool g_saveDirty = false;

    // Render thread -> scoring thread: the loaded track has no map, so record
    // one. Every load sets it explicitly; a track change clears it.
    std::atomic<bool> g_wantRecording{ false };
    // EndSession -> render thread: drop the loaded map on the next Draw. The
    // render thread owns those vectors, and clearing them from the game
    // thread while Draw is iterating them would crash.
    std::atomic<bool> g_resetRequested{ false };

    // --- Time gained/lost round the lap, for colouring the track -----------
    //
    // Scoring thread: the player's delta to each reference (kRefPb = personal
    // best, kRefSb = own session best, kRefFastest = the session's fastest
    // lap by anyone in the class) at every mini-sector boundary of the lap
    // (TrackMapConfig::deltaSectorCount equal stretches), interpolated
    // between the ~5Hz samples. A stretch's gain or loss is the difference
    // between its two boundaries. NaN = not reached.
    // Copied to g_shDelta under g_lock each tick for the render thread.
    const int kRefPb = 0, kRefSb = 1, kRefFastest = 2, kRefCount = 3;
    struct DeltaLaps
    {
        float binM = 0.0f;
        std::vector<float> cur[kRefCount];   // lap in progress
        std::vector<float> prev[kRefCount];  // last completed lap
        // The player's own time into the lap at each boundary (lap in
        // progress / last lap), and the session's fastest time through each
        // mini-sector by anyone in the class -- for purple.
        std::vector<float> curT, prevT;
        std::vector<float> best;
    };
    DeltaLaps g_delta;               // scoring thread
    DeltaLaps g_shDelta;             // shared, under g_lock
    double g_deltaTrackLen = 0.0;
    int    g_deltaSectorCount = 0;
    long   g_deltaLaps = -1;
    bool   g_deltaHaveLast = false;
    double g_deltaLastDist = 0.0;
    float  g_deltaLast[kRefCount] = {};
    bool   g_deltaLastValid[kRefCount] = {};
    double g_deltaLastT = 0.0;       // player's time into lap at the last sample

    // --- The session's fastest lap by anyone in the player's class ---------
    //
    // Scoring thread. Every car's lap in progress is kept as (lap distance,
    // time into lap) samples; a clean, valid lap that spans the whole track
    // and beats the best so far becomes the reference, using rF2's own lap
    // validity.
    struct CarLap
    {
        long id = -1;
        long laps = -1;
        bool active = false;
        bool pit = false;
        short validLatch = 2;          // minimum mCountLapFlag seen this lap
        double lastDist = -1.0;
        double lapTimeAtStart = 0.0;   // mLastLapTime when this lap began
        std::vector<std::pair<float, float>> samples;
        // A finished lap whose time has not reached scoring yet.
        bool pending = false;
        double pendingSinceET = 0.0;
        std::vector<std::pair<float, float>> pendingSamples;
    };
    std::vector<CarLap> g_carLaps;
    std::vector<std::pair<float, float>> g_fastTrace; // (lapDist, timeIntoLap)
    double g_fastLapTime = -1.0;
    double g_fastTrackLen = 0.0;

    void FastestReset()
    {
        g_carLaps.clear();
        g_fastTrace.clear();
        g_fastLapTime = -1.0;
        std::fill(g_delta.best.begin(), g_delta.best.end(), NAN);
    }

    // Time into a lap at a lap distance, from its (distance, time) samples:
    // from the line (time 0) to the first sample, between samples, and from
    // the last sample to the line at the lap time.
    bool LapTimeAt(const std::vector<std::pair<float, float>>& s, double lapTime, double trackLen,
                   double d, double& out)
    {
        if (s.size() < 2 || lapTime <= 0.0)
            return false;
        if (d <= s.front().first)
        {
            out = s.front().first > 0.01f ? s.front().second * (d / s.front().first) : s.front().second;
            return true;
        }
        if (d >= s.back().first)
        {
            const double span = trackLen - s.back().first;
            const double f = span > 0.01 ? (d - s.back().first) / span : 1.0;
            out = s.back().second + (lapTime - s.back().second) * (f > 1.0 ? 1.0 : f);
            return true;
        }
        size_t lo = 0, hi = s.size() - 1;
        while (hi - lo > 1)
        {
            const size_t mid = (lo + hi) / 2;
            if (s[mid].first <= d) lo = mid; else hi = mid;
        }
        const double span = s[hi].first - s[lo].first;
        const double f = span > 0.001 ? (d - s[lo].first) / span : 0.0;
        out = s[lo].second + (s[hi].second - s[lo].second) * f;
        return true;
    }

    // Folds one clean, valid lap's mini-sector times into the session bests.
    void AddSectorBests(const std::vector<std::pair<float, float>>& s, double lapTime, double trackLen)
    {
        std::vector<float>& best = g_delta.best;
        if (best.empty() || g_delta.binM <= 0.0f || trackLen != g_deltaTrackLen)
            return;
        double t0 = 0.0;
        for (size_t b = 0; b < best.size(); ++b)
        {
            double t1 = 0.0;
            const double d1 = (b + 1 == best.size()) ? trackLen : (b + 1) * static_cast<double>(g_delta.binM);
            if (!LapTimeAt(s, lapTime, trackLen, d1, t1))
                return;
            const float st = static_cast<float>(t1 - t0);
            if (st > 0.0f && (isnan(best[b]) || st < best[b]))
                best[b] = st;
            t0 = t1;
        }
    }

    void FastestTryFinish(const VehicleScoringInfoV01& v, double lapTime, double trackLen,
                          std::vector<std::pair<float, float>>& samples)
    {
        const bool spans = samples.size() >= 30 && trackLen > 100.0
            && samples.front().first < 100.0f && samples.back().first > trackLen - 150.0;
        if (!spans || lapTime <= 0.0)
            return;
        AddSectorBests(samples, lapTime, trackLen);
        if (g_fastLapTime > 0.0 && lapTime >= g_fastLapTime)
            return;
        g_fastTrace.swap(samples);
        g_fastLapTime = lapTime;
        g_fastTrackLen = trackLen;
        Log::Write("TrackMap: session fastest now %.3fs by %s%s", lapTime, v.mDriverName, v.mIsPlayer ? " (you)" : "");
    }

    void FastestStep(const ScoringInfoV01& info, const VehicleScoringInfoV01& player)
    {
        const double trackLen = info.mLapDist;
        const double et = info.mCurrentET;
        for (long i = 0; i < info.mNumVehicles; ++i)
        {
            const VehicleScoringInfoV01& v = info.mVehicle[i];
            if (strncmp(v.mVehicleClass, player.mVehicleClass, sizeof(v.mVehicleClass)) != 0)
                continue;
            CarLap* c = nullptr;
            for (CarLap& x : g_carLaps)
                if (x.id == v.mID) { c = &x; break; }
            if (!c)
            {
                g_carLaps.push_back(CarLap{});
                c = &g_carLaps.back();
                c->id = v.mID;
            }

            if (c->pending)
            {
                if (v.mLastLapTime > 0.0 && v.mLastLapTime != c->lapTimeAtStart)
                {
                    FastestTryFinish(v, v.mLastLapTime, trackLen, c->pendingSamples);
                    c->pending = false;
                    c->pendingSamples.clear();
                }
                else if (et - c->pendingSinceET > 5.0)
                {
                    c->pending = false;
                    c->pendingSamples.clear();
                }
            }

            if (v.mInPits) c->pit = true;
            if (v.mCountLapFlag < c->validLatch) c->validLatch = v.mCountLapFlag;

            if (v.mTotalLaps != c->laps)
            {
                if (c->active && c->laps >= 0 && v.mTotalLaps == c->laps + 1 && !c->pit && c->validLatch == 2)
                {
                    if (v.mLastLapTime > 0.0 && v.mLastLapTime != c->lapTimeAtStart)
                        FastestTryFinish(v, v.mLastLapTime, trackLen, c->samples);
                    else
                    {
                        c->pending = true;
                        c->pendingSinceET = et;
                        c->pendingSamples.swap(c->samples);
                    }
                }
                c->laps = v.mTotalLaps;
                c->pit = v.mInPits != 0;
                c->validLatch = 2;
                c->lastDist = -1.0;
                c->samples.clear();
                // While a lap is still pending, its time is what the next one
                // must differ from -- the old value is kept until it arrives.
                if (!c->pending)
                    c->lapTimeAtStart = v.mLastLapTime;
                c->active = v.mLapDist >= 0.0 && v.mLapDist < 200.0 && !v.mInPits;
            }
            if (!c->active)
                continue;

            const double d = v.mLapDist;
            if (c->lastDist >= 0.0 && fabs(d - c->lastDist) > 150.0)
            {
                c->active = false; // teleport -- back to the garage, a reset
                continue;
            }
            if (c->lastDist < 0.0 || d > c->lastDist)
            {
                c->samples.push_back({ static_cast<float>(d), static_cast<float>(et - v.mLapStartET) });
                c->lastDist = d;
            }
        }
    }

    // Time into the session-fastest lap at a lap distance.
    bool FastestTimeAt(double d, double& out)
    {
        return LapTimeAt(g_fastTrace, g_fastLapTime, g_fastTrackLen, d, out);
    }

    bool ReferenceTimeAt(int ref, double d, double& out)
    {
        if (ref == kRefFastest)
            return FastestTimeAt(d, out);
        return g_lapDelta.GetReferenceTimeAt(d, ref == kRefSb, out);
    }

    void DeltaReset(std::vector<float>* laps, size_t bounds)
    {
        for (int m = 0; m < kRefCount; ++m)
            laps[m].assign(bounds, NAN);
    }

    void DeltaStep(const ScoringInfoV01& info, const VehicleScoringInfoV01& v)
    {
        const TrackMapConfig& cfg = g_trackMapConfig;
        const double trackLen = info.mLapDist;
        if (trackLen < 100.0)
            return;

        // A new track length or mini-sector count starts everything over.
        if (trackLen != g_deltaTrackLen || cfg.deltaSectorCount != g_deltaSectorCount)
        {
            g_deltaTrackLen = trackLen;
            g_deltaSectorCount = cfg.deltaSectorCount;
            g_delta.binM = static_cast<float>(trackLen / g_deltaSectorCount);
            const size_t bounds = static_cast<size_t>(g_deltaSectorCount) + 1;
            DeltaReset(g_delta.cur, bounds);
            DeltaReset(g_delta.prev, bounds);
            g_delta.curT.assign(bounds, NAN);
            g_delta.prevT.assign(bounds, NAN);
            g_delta.best.assign(bounds - 1, NAN); // session bests are per mini-sector, so these start over too
            g_deltaLaps = -1;
            g_deltaHaveLast = false;
        }
        const size_t bounds = g_delta.cur[0].size();

        // Every car's laps, for the session-fastest reference and the
        // per-mini-sector session bests (needs the bins above).
        FastestStep(info, v);

        const double d = v.mLapDist;
        if (v.mTotalLaps != g_deltaLaps)
        {
            const bool completed = g_deltaLaps >= 0 && v.mTotalLaps == g_deltaLaps + 1;
            if (completed)
            {
                // Close the lap off at the line with the last sample's delta
                // (a few metres short of it) -- not from the reference, which
                // may already have been replaced with this very lap.
                for (int m = 0; m < kRefCount; ++m)
                {
                    if (g_deltaHaveLast && g_deltaLastValid[m])
                        g_delta.cur[m][bounds - 1] = g_deltaLast[m];
                    g_delta.prev[m] = g_delta.cur[m];
                }
                // Own time at the line: the last sample carried on at the
                // current speed (the lap time may not be in scoring yet).
                if (g_deltaHaveLast)
                {
                    const double spd = sqrt(v.mLocalVel.x * v.mLocalVel.x + v.mLocalVel.z * v.mLocalVel.z);
                    g_delta.curT[bounds - 1] = static_cast<float>(
                        g_deltaLastT + (trackLen - g_deltaLastDist) / (spd > 1.0 ? spd : 1.0));
                }
                g_delta.prevT = g_delta.curT;
            }
            DeltaReset(g_delta.cur, bounds);
            g_delta.curT.assign(bounds, NAN);
            g_deltaLaps = v.mTotalLaps;
            // A lap joined part way (out of the pits, back from the monitor)
            // colours from where it was joined: each stretch is a difference
            // of two deltas, so it does not need the lap's start.
            g_deltaHaveLast = completed && !v.mInPits;
            if (g_deltaHaveLast)
            {
                g_deltaLastDist = 0.0;
                for (int m = 0; m < kRefCount; ++m)
                {
                    g_deltaLast[m] = 0.0f;
                    g_deltaLastValid[m] = true;
                    g_delta.cur[m][0] = 0.0f;
                }
                g_deltaLastT = 0.0;
                g_delta.curT[0] = 0.0f;
            }
        }

        const bool usable = !v.mInPits && d >= 0.0
            && (!g_deltaHaveLast || (d >= g_deltaLastDist && d - g_deltaLastDist < 300.0));
        if (!usable)
        {
            g_deltaHaveLast = false; // a reset or teleport: stop until the line
        }
        else
        {
            const double t = info.mCurrentET - v.mLapStartET;
            if (g_deltaHaveLast && d > g_deltaLastDist)
            {
                const size_t b0 = static_cast<size_t>(floor(g_deltaLastDist / g_delta.binM)) + 1;
                const size_t b1 = static_cast<size_t>(floor(d / g_delta.binM));
                for (size_t b = b0; b <= b1 && b < bounds; ++b)
                {
                    const double f = (b * g_delta.binM - g_deltaLastDist) / (d - g_deltaLastDist);
                    g_delta.curT[b] = static_cast<float>(g_deltaLastT + (t - g_deltaLastT) * f);
                }
            }
            g_deltaLastT = t;
            for (int m = 0; m < kRefCount; ++m)
            {
                double refT = 0.0;
                const bool ok = ReferenceTimeAt(m, d, refT);
                const float delta = static_cast<float>(t - refT);
                if (ok && g_deltaHaveLast && g_deltaLastValid[m] && d > g_deltaLastDist)
                {
                    const size_t b0 = static_cast<size_t>(floor(g_deltaLastDist / g_delta.binM)) + 1;
                    const size_t b1 = static_cast<size_t>(floor(d / g_delta.binM));
                    for (size_t b = b0; b <= b1 && b < bounds; ++b)
                    {
                        const double f = (b * g_delta.binM - g_deltaLastDist) / (d - g_deltaLastDist);
                        g_delta.cur[m][b] = static_cast<float>(g_deltaLast[m] + (delta - g_deltaLast[m]) * f);
                    }
                }
                g_deltaLast[m] = delta;
                g_deltaLastValid[m] = ok;
            }
            g_deltaLastDist = d;
            g_deltaHaveLast = true;
        }

        EnterCriticalSection(&g_lock);
        g_shDelta = g_delta;
        LeaveCriticalSection(&g_lock);
    }

    // --- Recorder (scoring thread only) -------------------------------------
    struct Recorder
    {
        bool active = false;
        long laps = -1;
        bool pitLap = false;
        double lastDist = -1.0;
        int lastSector = -1;
        std::vector<Pt> map;
        std::vector<Pt> dist;
        int s1 = -1, s2 = -1;
    };
    Recorder g_rec;

    // --- Loaded map (render thread only) ------------------------------------
    char  g_loadedTrack[64] = {};
    bool  g_haveMap = false;
    bool  g_loadAttempted = false;

    std::vector<Pt>    g_raw;      // as read from the file / recording
    std::vector<Pt>    g_baked;    // after mirror + rotation
    std::vector<float> g_lapDist;  // lap distance at each index, strictly increasing
    float g_trackLength = 0.0f;
    int   g_sector1Index = -1;
    int   g_sector2Index = -1;

    float g_minX = 0.0f, g_maxX = 0.0f, g_minZ = 0.0f, g_maxZ = 0.0f;

    // This circuit's own angle and flip (see ReadTrackSettings), and what
    // the baked points were last built with.
    float g_trackRotationDeg = 0.0f;
    bool  g_trackMirror = false;
    float g_bakedRotationDeg = 0.0f;
    bool  g_bakedMirror = false;
    bool  g_bakeValid = false;

    float g_boxHalfH = 0.0f; // the box's half height as last drawn, for the layout controls under it
    float g_fitScale = 1.0f;
    float g_fitOffsetX = 0.0f, g_fitOffsetY = 0.0f;

    void EnsureLock()
    {
        if (g_lockReady)
            return;
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }

    // --- Files ---------------------------------------------------------------

    bool ExtractPoints(const std::string& text, const char* wantedId, std::vector<Pt>& out)
    {
        char idTag[64];
        sprintf_s(idTag, "id=\"%s\"", wantedId);
        const size_t idPos = text.find(idTag);
        if (idPos == std::string::npos)
            return false;
        const size_t ptsPos = text.find("points=\"", idPos);
        if (ptsPos == std::string::npos)
            return false;
        const size_t start = ptsPos + 8;
        const size_t end = text.find('"', start);
        if (end == std::string::npos)
            return false;

        out.clear();
        out.reserve((end - start) / 16 + 1); // "1234.5678,-123.4567 " is ~20 characters a point
        const char* p = text.c_str() + start;
        const char* stop = text.c_str() + end;
        while (p < stop)
        {
            while (p < stop && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
                ++p;
            if (p >= stop) break;
            char* next = nullptr;
            const double a = strtod(p, &next);
            if (next == p) break;
            p = next;
            if (p < stop && *p == ',') ++p;
            const double b = strtod(p, &next);
            if (next == p) break;
            p = next;
            out.push_back({ static_cast<float>(a), static_cast<float>(b) });
        }
        return !out.empty();
    }

    void ExtractSectorIndices(const std::string& text, int& s1, int& s2)
    {
        s1 = s2 = -1;
        const size_t open = text.find("<desc>");
        if (open == std::string::npos) return;
        const size_t close = text.find("</desc>", open);
        if (close == std::string::npos) return;
        const std::string body = text.substr(open + 6, close - (open + 6));
        int a = 0, b = 0;
        if (sscanf_s(body.c_str(), "%d,%d", &a, &b) == 2) { s1 = a; s2 = b; }
    }

    bool ReadWholeFile(const char* path, std::string& out)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || !f)
            return false;
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size <= 0 || size > 8 * 1024 * 1024) { fclose(f); return false; }
        out.resize(static_cast<size_t>(size));
        const size_t got = fread(&out[0], 1, static_cast<size_t>(size), f);
        fclose(f);
        out.resize(got);
        return got > 0;
    }

    void OwnMapDir(char* out, size_t outSize)
    {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&OwnMapDir), &self);
        char path[MAX_PATH] = {};
        GetModuleFileNameA(self, path, MAX_PATH);
        char* lastSlash = strrchr(path, '\\');
        if (lastSlash) *(lastSlash + 1) = 0;
        strcpy_s(out, outSize, path);
        strcat_s(out, outSize, "rF2SkrOverlayPublic_TrackMaps\\");
    }

    // Track names come from the game; anything Windows will not take in a
    // file name becomes an underscore. Only for this plugin's own files --
    // TinyPedal's are looked up under the raw name, as it saved them.
    void SafeFileName(const char* track, char* out, size_t outSize)
    {
        size_t n = 0;
        for (const char* p = track; *p && n + 1 < outSize; ++p)
            out[n++] = strchr("\\/:*?\"<>|", *p) ? '_' : *p;
        out[n] = '\0';
    }

    void XmlEscape(const char* in, std::string& out)
    {
        out.clear();
        for (const char* p = in; *p; ++p)
        {
            switch (*p)
            {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += *p; break;
            }
        }
    }

    // Same layout TinyPedal writes, so either tool can read the other's.
    // Persist thread only (FlushIfDirty), with no lock held. Written to a
    // temp file and moved over the old one, so a crash or a full disk mid-
    // write never leaves a half-written map for the next load to choke on.
    void WriteRecording(const Recording& rec)
    {
        if (rec.map.empty() || rec.map.size() != rec.dist.size())
            return;

        char dir[MAX_PATH];
        OwnMapDir(dir, sizeof(dir));
        CreateDirectoryA(dir, nullptr); // already existing is fine
        char name[64];
        SafeFileName(rec.track, name, sizeof(name));
        char path[MAX_PATH];
        sprintf_s(path, "%s%s.svg", dir, name);

        float minX = rec.map[0].x, maxX = minX, minZ = rec.map[0].z, maxZ = minZ;
        for (const Pt& p : rec.map)
        {
            minX = (std::min)(minX, p.x); maxX = (std::max)(maxX, p.x);
            minZ = (std::min)(minZ, p.z); maxZ = (std::max)(maxZ, p.z);
        }

        char tmp[MAX_PATH + 8];
        sprintf_s(tmp, "%s.tmp", path);
        FILE* f = nullptr;
        if (fopen_s(&f, tmp, "w") != 0 || !f)
        {
            Log::Write("TrackMap: could not write '%s' (read-only? UAC?)", path);
            return;
        }
        std::string title;
        XmlEscape(rec.track, title);
        const float m = 20.0f;
        fprintf(f, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
        fprintf(f, "<!-- Track map recorded by rF2SkrOverlayPublic, in TinyPedal's format -->\n");
        fprintf(f, "<svg viewBox=\"%.4f %.4f %.4f %.4f\" version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\">\n",
                minX - m, minZ - m, (maxX - minX) + 2.0f * m, (maxZ - minZ) + 2.0f * m);
        fprintf(f, "\t<title>%s</title>\n", title.c_str());
        fprintf(f, "\t<desc>%d,%d</desc>\n", rec.s1, rec.s2);
        fprintf(f, "\t<polyline id=\"map\" fill=\"none\" stroke=\"black\" stroke-width=\"20\" points=\"");
        for (size_t i = 0; i < rec.map.size(); ++i)
            fprintf(f, "%s%.4f,%.4f", i ? " " : "", rec.map[i].x, rec.map[i].z);
        fprintf(f, "\"/>\n");
        fprintf(f, "\t<polyline id=\"dist\" fill=\"none\" stroke=\"none\" points=\"");
        for (size_t i = 0; i < rec.dist.size(); ++i)
            fprintf(f, "%s%.4f,%.4f", i ? " " : "", rec.dist[i].x, rec.dist[i].z);
        fprintf(f, "\"/>\n</svg>\n");
        const bool written = !ferror(f);
        const bool closed = fclose(f) == 0;
        if (!written || !closed || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
        {
            DeleteFileA(tmp);
            Log::Write("TrackMap: could not write '%s' (read-only? UAC? disk full?)", path);
            return;
        }
        Log::Write("TrackMap: saved '%s' (%d points)", path, static_cast<int>(rec.map.size()));
    }

    // --- Building a map from points ------------------------------------------

    // Turns the parallel arrays into something a lap distance can be looked up
    // in: rotated so the smallest distance is first, then forced strictly
    // increasing. The sector indices are shifted with the rotation -- they
    // point INTO these arrays.
    //
    // TinyPedal's recordings are nearly monotonic but not quite -- across the
    // 54 maps here there are 68 small backward steps, usually a few metres,
    // worst 40m on a 13.5km Le Mans lap. They are recording artefacts, and
    // clamping collapses those few points onto one distance rather than
    // letting a binary search walk off the rails.
    void NormalizeLapDist(std::vector<Pt>& map, std::vector<float>& dist, int& s1, int& s2)
    {
        if (dist.size() < 2)
            return;

        int minIdx = 0;
        for (size_t i = 1; i < dist.size(); ++i)
            if (dist[i] < dist[minIdx])
                minIdx = static_cast<int>(i);

        if (minIdx != 0)
        {
            std::rotate(map.begin(), map.begin() + minIdx, map.end());
            std::rotate(dist.begin(), dist.begin() + minIdx, dist.end());
            const int n = static_cast<int>(dist.size());
            if (s1 >= 0) s1 = (s1 - minIdx + n) % n;
            if (s2 >= 0) s2 = (s2 - minIdx + n) % n;
        }

        for (size_t i = 1; i < dist.size(); ++i)
            if (dist[i] <= dist[i - 1])
                dist[i] = dist[i - 1] + 0.01f;
    }

    // Shared by the file loader and a fresh recording. 'distPts' may be null
    // or a different length, in which case arc length stands in for it.
    // Fills 'out' only, so it runs off the render thread: on the map-load
    // worker for a file, on the scoring thread for a recording.
    void BuildMapData(const char* what, std::vector<Pt>&& mapPts, const std::vector<Pt>* distPts, int s1, int s2,
                      MapData& out)
    {
        out.raw = std::move(mapPts);
        out.lapDist.clear();
        out.s1 = s1;
        out.s2 = s2;
        out.trackLength = 0.0f;
        out.ok = false;
        std::vector<Pt>& raw = out.raw;
        std::vector<float>& lapDist = out.lapDist;
        if (raw.size() < 2)
            return;

        lapDist.resize(raw.size());
        if (distPts && distPts->size() == raw.size())
        {
            for (size_t i = 0; i < distPts->size(); ++i)
                lapDist[i] = (*distPts)[i].x;
        }
        else
        {
            // The points are in metres, so a cumulative sum stands in for lap
            // distance and keeps a map without a usable dist array usable.
            Log::Write("TrackMap: '%s' has no matching dist array -- using arc length", what);
            float acc = 0.0f;
            lapDist[0] = 0.0f;
            for (size_t i = 1; i < raw.size(); ++i)
            {
                const float dx = raw[i].x - raw[i - 1].x;
                const float dz = raw[i].z - raw[i - 1].z;
                acc += sqrtf(dx * dx + dz * dz);
                lapDist[i] = acc;
            }
        }

        NormalizeLapDist(raw, lapDist, out.s1, out.s2);

        // Close the loop: the distance from the last point back to the first.
        // Measured from the FIRST point's distance, which is rarely exactly 0.
        const float dxc = raw[0].x - raw.back().x;
        const float dzc = raw[0].z - raw.back().z;
        out.trackLength = (lapDist.back() - lapDist.front()) + sqrtf(dxc * dxc + dzc * dzc);

        Log::Write("TrackMap: '%s' -> %d points, sectors %d/%d, lapDist %.1f..%.1f, length %.1f",
                   what, static_cast<int>(raw.size()),
                   out.s1, out.s2,
                   lapDist.front(), lapDist.back(), out.trackLength);
        out.ok = true;
    }

    // Render thread: makes a built map the loaded one. Swapped, not copied
    // -- 'm' is left holding the previous map, for the caller to free.
    bool ApplyMap(MapData& m)
    {
        g_raw.swap(m.raw);
        g_lapDist.swap(m.lapDist);
        g_trackLength = m.trackLength;
        g_sector1Index = m.s1;
        g_sector2Index = m.s2;
        g_bakeValid = false;
        return m.ok;
    }

    // Finds and reads the circuit's SVG (own map folder first, then
    // TinyPedal's). File I/O only -- run on the map-load worker thread.
    bool ReadMapText(const char* trackName, std::string& text)
    {
        if (!trackName || !trackName[0])
            return false;
        char ownDir[MAX_PATH];
        OwnMapDir(ownDir, sizeof(ownDir));
        char safeName[64];
        SafeFileName(trackName, safeName, sizeof(safeName));
        char candidates[2][MAX_PATH];
        sprintf_s(candidates[0], "%s%s.svg", ownDir, safeName);
        sprintf_s(candidates[1], "%s%s.svg", g_trackMapConfig.tinyPedalMapDir, trackName);
        for (int i = 0; i < 2; ++i)
            if (ReadWholeFile(candidates[i], text))
            {
                Log::Write("TrackMap: loaded '%s'", candidates[i]);
                return true;
            }
        Log::Write("TrackMap: no map file for track '%s'", trackName);
        return false;
    }

    // Parses the text ReadMapText got and builds the map from it (map-load
    // worker). A few thousand numbers for a 45 KB map -- it used to run on
    // the render thread when the worker handed back the text.
    void ParseMap(const char* trackName, const std::string& text, bool read, MapData& out)
    {
        out = MapData{};
        if (!trackName || !trackName[0] || !read)
            return;

        std::vector<Pt> mapPts, distPts;
        if (!ExtractPoints(text, "map", mapPts))
        {
            Log::Write("TrackMap: '%s' has no usable polyline id=map", trackName);
            return;
        }
        int s1 = -1, s2 = -1;
        ExtractSectorIndices(text, s1, s2);
        const bool haveDist = ExtractPoints(text, "dist", distPts);
        BuildMapData(trackName, std::move(mapPts), haveDist ? &distPts : nullptr, s1, s2, out);
    }

    // --- Recorder --------------------------------------------------------------

    // Scoring thread. Captures the player's line from one line crossing to
    // the next -- the same rules TinyPedal records by: start within 200m of
    // the line, no pit visit during the lap, no teleport (a jump of more than
    // 150m between ~5Hz samples is a reset to the garage, not driving).
    void RecordStep(const ScoringInfoV01& info, const VehicleScoringInfoV01& v)
    {
        Recorder& r = g_rec;
        if (!g_wantRecording.load(std::memory_order_relaxed))
        {
            r.active = false;
            r.laps = -1;
            return;
        }
        if (v.mInPits)
            r.pitLap = true;

        if (v.mTotalLaps != r.laps)
        {
            if (r.active && r.laps >= 0 && v.mTotalLaps == r.laps + 1)
            {
                const double trackLen = info.mLapDist; // ScoringInfoV01::mLapDist is the TRACK length
                const bool spans = r.map.size() >= 50 && trackLen > 100.0
                    && r.dist.front().x < 100.0f && r.dist.back().x > trackLen - 150.0;
                const bool clean = !r.pitLap && v.mLastLapTime > 0.0;
                if (spans && clean)
                {
                    Recording rec;
                    strncpy_s(rec.track, sizeof(rec.track), info.mTrackName, _TRUNCATE);
                    rec.map = r.map;
                    rec.dist = r.dist;
                    rec.s1 = r.s1;
                    rec.s2 = r.s2;
                    Log::Write("TrackMap: recorded '%s' -- %d points, sectors %d/%d, lap %.3fs",
                               rec.track, static_cast<int>(rec.map.size()), rec.s1, rec.s2, v.mLastLapTime);
                    // Built into a map here (once per recording), so the
                    // render thread only has to swap it in.
                    MapData built;
                    BuildMapData(rec.track, std::vector<Pt>(rec.map), &rec.dist, rec.s1, rec.s2, built);
                    // Swapped in under the lock; whatever they replace is
                    // freed after it is released.
                    EnterCriticalSection(&g_lock);
                    strcpy_s(g_newMapTrack, sizeof(g_newMapTrack), rec.track);
                    std::swap(g_newMap, built);
                    g_haveNewRecording = true;
                    std::swap(g_saveQueue, rec);
                    g_saveDirty = true;
                    LeaveCriticalSection(&g_lock);
                    g_wantRecording.store(false, std::memory_order_relaxed);
                }
                else
                {
                    Log::Write("TrackMap: recorded lap not used (%d points, %.0f..%.0fm of %.0fm, pit %d, lap %.3fs)",
                               static_cast<int>(r.map.size()),
                               r.dist.empty() ? 0.0 : r.dist.front().x, r.dist.empty() ? 0.0 : r.dist.back().x,
                               trackLen, r.pitLap ? 1 : 0, v.mLastLapTime);
                }
            }
            r.active = v.mLapDist >= 0.0 && v.mLapDist < 200.0 && !v.mInPits;
            r.laps = v.mTotalLaps;
            r.pitLap = v.mInPits != 0;
            r.lastDist = -1.0;
            r.lastSector = v.mSector;
            r.map.clear();
            r.dist.clear();
            r.s1 = r.s2 = -1;
        }
        if (!r.active)
            return;

        const double d = v.mLapDist;
        if (r.lastDist >= 0.0 && fabs(d - r.lastDist) > 150.0)
        {
            r.active = false;
            return;
        }
        // Forward progress only: a spin that rolls the distance back must
        // not fold the line back on itself.
        if (r.lastDist < 0.0 || d - r.lastDist >= 2.0)
        {
            r.map.push_back({ static_cast<float>(v.mPos.x), static_cast<float>(-v.mPos.z) });
            r.dist.push_back({ static_cast<float>(d), static_cast<float>(v.mPos.y) });
            r.lastDist = d;
        }
        // mSector: 1 = sector 1, 2 = sector 2, 0 = sector 3.
        if (v.mSector != r.lastSector)
        {
            if (v.mSector == 2) r.s1 = static_cast<int>(r.map.size()) - 1;
            else if (v.mSector == 0) r.s2 = static_cast<int>(r.map.size()) - 1;
            r.lastSector = v.mSector;
        }
    }

    // --- Transform -------------------------------------------------------------

    void Rebake()
    {
        if (g_raw.empty()) { g_bakeValid = false; return; }

        float cx = 0.0f, cz = 0.0f;
        for (const Pt& p : g_raw) { cx += p.x; cz += p.z; }
        cx /= static_cast<float>(g_raw.size());
        cz /= static_cast<float>(g_raw.size());

        const float rad = g_trackRotationDeg * 3.14159265358979f / 180.0f;
        const float cs = cosf(rad), sn = sinf(rad);
        const float mir = g_trackMirror ? -1.0f : 1.0f;

        g_baked.resize(g_raw.size());
        for (size_t i = 0; i < g_raw.size(); ++i)
        {
            const float dx = (g_raw[i].x - cx) * mir;
            const float dz = (g_raw[i].z - cz);
            g_baked[i].x = dx * cs - dz * sn;
            g_baked[i].z = dx * sn + dz * cs;
        }

        g_minX = g_maxX = g_baked[0].x;
        g_minZ = g_maxZ = g_baked[0].z;
        for (const Pt& p : g_baked)
        {
            if (p.x < g_minX) g_minX = p.x;
            if (p.x > g_maxX) g_maxX = p.x;
            if (p.z < g_minZ) g_minZ = p.z;
            if (p.z > g_maxZ) g_maxZ = p.z;
        }

        g_bakedRotationDeg = g_trackRotationDeg;
        g_bakedMirror = g_trackMirror;
        g_bakeValid = true;
    }

    void BakedToScreen(float bx, float bz, float& sx, float& sy)
    {
        sx = g_fitOffsetX + bx * g_fitScale;
        sy = g_fitOffsetY + bz * g_fitScale;
    }

    // Places a car on the map from its LAP DISTANCE rather than its world
    // position: it indexes along the SAME array the outline is drawn from, so
    // dots and track are in one space by construction, whatever coordinate
    // convention the file was recorded in. 'lateral' is metres from the
    // recorded line, applied perpendicular to the local path direction so
    // cars running side by side do not stack.
    void PlaceByLapDist(double lapDist, double lateral, float& sx, float& sy)
    {
        const int n = static_cast<int>(g_baked.size());
        if (n < 2) { sx = sy = 0.0f; return; }

        float d = static_cast<float>(lapDist);
        if (g_trackLength > 1.0f)
        {
            while (d < g_lapDist[0]) d += g_trackLength;
            while (d > g_lapDist[0] + g_trackLength) d -= g_trackLength;
        }

        int lo = 0, hi = n - 1;
        if (d >= g_lapDist[n - 1])
        {
            lo = n - 1;
        }
        else
        {
            while (hi - lo > 1)
            {
                const int mid = (lo + hi) / 2;
                if (g_lapDist[mid] <= d) lo = mid; else hi = mid;
            }
        }
        const int i0 = lo;
        const int i1 = (lo + 1) % n;

        // Segment span, wrapping past the last point back to the first.
        float span = (i1 == 0) ? (g_lapDist[0] + g_trackLength - g_lapDist[i0])
                               : (g_lapDist[i1] - g_lapDist[i0]);
        if (span < 0.001f) span = 0.001f;
        float t = (d - g_lapDist[i0]) / span;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;

        const float bx = g_baked[i0].x + (g_baked[i1].x - g_baked[i0].x) * t;
        const float bz = g_baked[i0].z + (g_baked[i1].z - g_baked[i0].z) * t;

        float ox = 0.0f, oz = 0.0f;
        if (lateral != 0.0 && g_trackMapConfig.lateralEnabled)
        {
            float dx = g_baked[i1].x - g_baked[i0].x;
            float dz = g_baked[i1].z - g_baked[i0].z;
            const float len = sqrtf(dx * dx + dz * dz);
            if (len > 0.0001f)
            {
                dx /= len; dz /= len;
                // The mirror flips which side "left of the line" lands on.
                const float sign = (g_trackMapConfig.lateralFlip ? -1.0f : 1.0f) * (g_trackMirror ? -1.0f : 1.0f);
                const float lat = static_cast<float>(lateral) * sign;
                ox = -dz * lat;
                oz =  dx * lat;
            }
        }

        BakedToScreen(bx + ox, bz + oz, sx, sy);
    }

    // --- The track line on the GPU -----------------------------------------------

    // The coloured line is cut into about this many pieces (whole runs of
    // mini-sectors), each its own static batch -- a compromise between how
    // much a colour change rebuilds and how many batches are drawn a frame.
    const int kRibbonGroups = 12;
    const int kMaxRibbonPieces = 16;

    // A run of the line kept in a Renderer::StaticBatch, with a copy of
    // exactly what it was built from: rebuilt only when that changes. A
    // straight memcmp of a few KB -- it replaced an FNV hash run byte by byte
    // over the whole line twenty times a second.
    struct RibbonBatch
    {
        Renderer::StaticBatch batch;   // a static: the renderer keeps a pointer to it
        std::vector<float> xy, rgb;
        float style[6] = {};
    };

    bool SameFloats(const std::vector<float>& kept, const float* p, size_t n)
    {
        return kept.size() == n && (n == 0 || memcmp(kept.data(), p, n * sizeof(float)) == 0);
    }

    void DrawRibbon(Renderer& renderer, RibbonBatch& rb, const float* xy, int count, bool closed,
                    float w, float r, float g, float b, float a, const float* rgb)
    {
        const float style[6] = { w, r, g, b, a, closed ? 1.0f : 0.0f };
        const size_t nXy = count > 0 ? static_cast<size_t>(count) * 2 : 0;
        const size_t nRgb = rgb ? nXy / 2 * 3 : 0;
        if (rb.batch.valid && memcmp(style, rb.style, sizeof(style)) == 0
            && SameFloats(rb.xy, xy, nXy) && SameFloats(rb.rgb, rgb, nRgb))
        {
            renderer.DrawStatic(rb.batch);
            return;
        }
        const size_t mark = renderer.BeginStatic();
        renderer.AddPolylineSmooth(xy, count, closed, w, r, g, b, a, rgb);
        renderer.EndStatic(mark, rb.batch);
        rb.xy.assign(xy, xy + nXy);
        if (rgb) rb.rgb.assign(rgb, rgb + nRgb);
        else rb.rgb.clear();
        memcpy(rb.style, style, sizeof(style));
    }

    // Where the closed, coloured line (n points, 'xy' snapped -- see Draw) is
    // cut into pieces: one cut where each group of mini-sectors starts, and
    // one as near the start of the array as it can be, so the pieces draw in
    // the order the single line's segments did (that order decides which
    // side is on top where a circuit crosses itself). Returns the cuts, as
    // segment indices (segment j runs from point j to j + 1), ascending.
    //
    // A cut goes half way along a segment whose two ends have the same
    // colour and no sharp bend. The pieces are then open lines whose square
    // ends meet half way along that segment, and they draw exactly what the
    // single line did: every other point keeps the same neighbours and so
    // the same mitred corner, the cut segment's two halves cover what it
    // covered, in the one colour it had. A sharp bend is avoided because
    // past ~133 degrees the renderer clamps its mitre, and the corner would
    // then no longer line up with a square end.
    int SplitRibbon(const float* xy, const float* rgb, const unsigned short* group, int n, int* cuts, int maxCuts)
    {
        if (n < 8)
            return 0;
        auto dir = [&](int from, int to, float& dx, float& dy)
        {
            dx = xy[to * 2] - xy[from * 2];
            dy = xy[to * 2 + 1] - xy[from * 2 + 1];
            const float len = sqrtf(dx * dx + dy * dy);
            if (len > 0.0001f) { dx /= len; dy /= len; }
            return len;
        };
        // No sharper than 120 degrees at point c.
        auto gentle = [&](int c)
        {
            float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
            if (dir((c - 1 + n) % n, c, ax, ay) < 0.0001f || dir(c, (c + 1) % n, bx, by) < 0.0001f)
                return false;
            return ax * bx + ay * by > -0.5f;
        };
        auto canCut = [&](int j)
        {
            const int j1 = (j + 1) % n;
            float dx = 0.0f, dy = 0.0f;
            return memcmp(rgb + j * 3, rgb + j1 * 3, 3 * sizeof(float)) == 0
                && dir(j, j1, dx, dy) >= 1.0f && gentle(j) && gentle(j1);
        };
        int count = 0;
        bool seeking = true;   // the first cut, from the start of the array
        for (int j = 0; j < n && count < maxCuts; ++j)
        {
            if (j > 0 && group[j] != group[j - 1])
                seeking = true;
            if (seeking && canCut(j))
            {
                cuts[count++] = j;
                seeking = false;
            }
        }
        return count;
    }

    // --- Per-track rotation and mirror -----------------------------------------

    // Sanitised into an INI key name: '=' or a newline in a key would corrupt
    // the file, so anything not alphanumeric becomes an underscore.
    void TrackKey(const char* prefix, const char* track, char* out, size_t outSize)
    {
        strcpy_s(out, outSize, prefix);
        size_t n = strlen(out);
        for (const char* p = track; *p && n + 1 < outSize; ++p)
        {
            const unsigned char c = static_cast<unsigned char>(*p);
            const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                            (c >= 'a' && c <= 'z');
            out[n++] = ok ? static_cast<char>(c) : '_';
        }
        out[n] = '\0';
    }

    // The global RotationDegrees/Mirror are only the fallback for a circuit
    // never turned -- read from the config each time, never from the last
    // circuit's value, so a new track does not inherit the previous one's angle.
    // (Reads the INI: map-load worker thread.)
    void ReadTrackSettings(const char* track, float& rotDeg, bool& mirror)
    {
        rotDeg = g_trackMapConfig.rotationDegrees;
        mirror = g_trackMapConfig.mirror;
        if (!track || !track[0])
            return;
        char key[96];
        TrackKey("Rotation_", track, key, sizeof(key));
        rotDeg = Config::ReadFloatLive("TrackMap", key, rotDeg);
        TrackKey("Mirror_", track, key, sizeof(key));
        mirror = Config::ReadFloatLive("TrackMap", key, mirror ? 1.0f : 0.0f) > 0.5f;
    }

    // A track change does everything slow on a worker thread: reads the SVG,
    // parses it and builds the map (ParseMap), and reads the circuit's INI
    // settings. Draw starts it and, once the thread has exited, only swaps
    // the result in. It used to read and parse all of it on the render
    // thread (a hitch at every track load), and later still parsed there.
    // The worker touches nothing but g_mapJob, and Draw reads g_mapJob only
    // after the thread has ended.
    struct MapJob
    {
        HANDLE thread = nullptr;
        char track[64] = {};
        unsigned gen = 0;
        MapData map;
        float rotDeg = 0.0f;
        bool mirror = false;
    } g_mapJob;
    unsigned g_mapGen = 0;   // bumped by DropLoadedMap: a load in flight is then stale

    DWORD WINAPI MapJobProc(LPVOID)
    {
        std::string text;
        const bool read = ReadMapText(g_mapJob.track, text);
        ParseMap(g_mapJob.track, text, read, g_mapJob.map);
        ReadTrackSettings(g_mapJob.track, g_mapJob.rotDeg, g_mapJob.mirror);
        return 0;
    }

    void PersistTrackRotation()
    {
        if (!g_loadedTrack[0])
            return;
        char key[96];
        TrackKey("Rotation_", g_loadedTrack, key, sizeof(key));
        if (!Config::WriteFloat("TrackMap", key, g_trackRotationDeg))
            Log::Write("TrackMap: failed to write [TrackMap] %s to the INI", key);
        else
            Log::Write("TrackMap: rotation for '%s' -> %.1f deg", g_loadedTrack, g_trackRotationDeg);
    }

    void PersistTrackMirror()
    {
        if (!g_loadedTrack[0])
            return;
        char key[96];
        TrackKey("Mirror_", g_loadedTrack, key, sizeof(key));
        if (!Config::WriteBool("TrackMap", key, g_trackMirror))
            Log::Write("TrackMap: failed to write [TrackMap] %s to the INI", key);
        else
            Log::Write("TrackMap: mirror for '%s' -> %d", g_loadedTrack, g_trackMirror ? 1 : 0);
    }

    // Layout-mode control state.
    bool g_rotDragging = false;
    bool g_prevButton = false;

    void DropLoadedMap()
    {
        ++g_mapGen;
        g_haveMap = false;
        g_loadAttempted = false;
        g_loadedTrack[0] = 0;
        g_raw.clear();
        g_baked.clear();
        g_lapDist.clear();
        g_bakeValid = false;
    }
}

void TrackMap::OnScoring(const ScoringInfoV01& info)
{
    EnsureLock();
    if (g_qpcFreq.QuadPart == 0)
        QueryPerformanceFrequency(&g_qpcFreq);

    const VehicleScoringInfoV01* player = nullptr;

    EnterCriticalSection(&g_lock);

    if (strncmp(g_pendingTrack, info.mTrackName, sizeof(g_pendingTrack) - 1) != 0)
    {
        strncpy_s(g_pendingTrack, sizeof(g_pendingTrack), info.mTrackName, _TRUNCATE);
        g_pendingChanged = true;
        g_wantRecording.store(false, std::memory_order_relaxed);
        g_rec = Recorder{};
        FastestReset();
    }

    // Roll current -> previous, then take a fresh sample, so Draw can
    // interpolate between two real samples rather than extrapolate from one.
    for (int i = 0; i < g_curCount; ++i)
        g_prevCars[i] = g_curCars[i];
    g_prevCount = g_curCount;
    g_prevTime = g_curTime;

    g_curCount = 0;
    if (info.mVehicle)
    {
        const int n = (info.mNumVehicles < kMaxCars) ? info.mNumVehicles : kMaxCars;
        for (int i = 0; i < n; ++i)
        {
            const VehicleScoringInfoV01& v = info.mVehicle[i];
            CarSample& s = g_curCars[g_curCount++];
            s.id = v.mID;
            s.lapDist = v.mLapDist;
            s.pathLateral = v.mPathLateral;
            s.isPlayer = v.mIsPlayer != 0;
            if (s.isPlayer)
                player = &v;
        }
    }
    QueryPerformanceCounter(&g_curTime);

    LeaveCriticalSection(&g_lock);

    g_lastScoringQpc.store(g_curTime.QuadPart, std::memory_order_relaxed);

    if (player)
    {
        RecordStep(info, *player);
        DeltaStep(info, *player);
    }
}

// Persist thread, while scoring and the render thread keep running: the
// queued recording is taken under g_lock (a swap -- the lock is held for a
// few pointer moves, not a copy of the points) and written after it is
// released, so neither callback ever waits on the disk.
void TrackMap::FlushIfDirty()
{
    if (!g_lockReady)
        return;
    Recording rec;
    bool dirty = false;
    EnterCriticalSection(&g_lock);
    if (g_saveDirty)
    {
        std::swap(rec, g_saveQueue);
        g_saveDirty = false;
        dirty = true;
    }
    LeaveCriticalSection(&g_lock);
    if (dirty)
        WriteRecording(rec);
}

void TrackMap::Clear()
{
    // The loaded map belongs to the render thread -- ask it to drop it rather
    // than clearing vectors Draw may be iterating right now. Dropping it
    // matters: otherwise the last session's circuit would carry over.
    g_resetRequested.store(true, std::memory_order_relaxed);
    g_wantRecording.store(false, std::memory_order_relaxed);
    g_lastScoringQpc.store(0, std::memory_order_relaxed);
    g_rec = Recorder{};
    g_deltaTrackLen = 0.0; // the next scoring tick starts the lap colours over
    FastestReset();

    if (!g_lockReady)
        return;
    EnterCriticalSection(&g_lock);
    g_shDelta = DeltaLaps{};
    g_curCount = 0;
    g_prevCount = 0;
    g_pendingTrack[0] = 0;
    g_pendingChanged = false;
    g_haveNewRecording = false;
    g_newMapTrack[0] = 0;
    g_newMap = MapData{};
    LeaveCriticalSection(&g_lock);
}

void TrackMap::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    const TrackMapConfig& cfg = g_trackMapConfig;
    if (!cfg.enabled)
        return;

    if (g_resetRequested.exchange(false, std::memory_order_relaxed))
        DropLoadedMap();

    if (g_lockReady)
    {
        char wanted[64] = {};
        bool changed = false;
        char freshTrack[64] = {};
        MapData fresh;
        bool haveFresh = false;
        EnterCriticalSection(&g_lock);
        if (g_pendingChanged && !g_mapJob.thread)
        {
            strcpy_s(wanted, sizeof(wanted), g_pendingTrack);
            g_pendingChanged = false;
            changed = true;
        }
        if (g_haveNewRecording)
        {
            strcpy_s(freshTrack, sizeof(freshTrack), g_newMapTrack);
            std::swap(fresh, g_newMap);
            g_haveNewRecording = false;
            haveFresh = true;
        }
        LeaveCriticalSection(&g_lock);

        bool jobFinished = false;
        if (changed)
        {
            strcpy_s(g_mapJob.track, sizeof(g_mapJob.track), wanted);
            g_mapJob.gen = g_mapGen;
            g_mapJob.map = MapData{};
            g_mapJob.thread = CreateThread(nullptr, 0, MapJobProc, nullptr, 0, nullptr);
            if (!g_mapJob.thread)
            {
                // No worker: load here rather than never.
                Log::Write("TrackMap: could not start the map loader (error %lu) -- loading on the render thread",
                           GetLastError());
                MapJobProc(nullptr);
                jobFinished = true;
            }
            changed = false;
        }
        // Polled rather than waited on: the worker's exit can itself be held
        // up behind the loader lock while the game loads a session, and a
        // wait here would hold the frame up with it. Once the handle is
        // signalled the thread has ended and everything it wrote is visible.
        if (g_mapJob.thread && WaitForSingleObject(g_mapJob.thread, 0) == WAIT_OBJECT_0)
        {
            CloseHandle(g_mapJob.thread);
            g_mapJob.thread = nullptr;
            jobFinished = true;
        }
        if (jobFinished)
        {
            if (g_mapJob.gen == g_mapGen)
            {
                strcpy_s(g_loadedTrack, sizeof(g_loadedTrack), g_mapJob.track);
                g_trackRotationDeg = g_mapJob.rotDeg;
                g_trackMirror = g_mapJob.mirror;
                g_haveMap = ApplyMap(g_mapJob.map);
                changed = true;
            }
            g_mapJob.map = MapData{}; // the map swapped out, or a stale load's result
        }
        if (changed)
        {
            g_loadAttempted = true;
            const bool record = cfg.recordMissing && !g_haveMap && g_loadedTrack[0] != 0;
            g_wantRecording.store(record, std::memory_order_relaxed);
            if (record)
                Log::Write("TrackMap: will record '%s' from the next clean lap", g_loadedTrack);
        }
        // A recording made for the circuit now loaded: show it at once.
        if (haveFresh && strcmp(freshTrack, g_loadedTrack) == 0)
            g_haveMap = ApplyMap(fresh);
    }

    // Nothing until the first scoring tick of the session.
    if (!Layout::Active())
    {
        const long long last = g_lastScoringQpc.load(std::memory_order_relaxed);
        if (last == 0 || g_qpcFreq.QuadPart == 0)
            return;
    }

    const float uiScale = static_cast<float>(screenHeight) / 2160.0f * cfg.scale;
    const float halfSize = cfg.sizePx * uiScale * 0.5f;
    const float centerX = screenWidth * cfg.posXFraction;
    const float centerY = screenHeight * cfg.posYFraction;
    const float margin = (cfg.lineWidthPx + cfg.outlineWidthPx) * uiScale;

    // The box takes the track's own shape (as rotated): SizePixels is its
    // LONGER side and the other follows the track's aspect, so a wide
    // circuit gets a wide box instead of a square with empty bands. Kept to
    // at least a third of the long side, so a very thin layout still leaves
    // room for the car labels. Square while there is no map.
    if (g_haveMap && g_raw.size() >= 2
        && (!g_bakeValid || g_bakedRotationDeg != g_trackRotationDeg || g_bakedMirror != g_trackMirror))
        Rebake();
    const bool mapReady = g_haveMap && g_raw.size() >= 2 && g_bakeValid;
    float halfW = halfSize, halfH = halfSize;
    if (mapReady)
    {
        const float sx = (std::max)(g_maxX - g_minX, 0.001f);
        const float sz = (std::max)(g_maxZ - g_minZ, 0.001f);
        const float aspect = (std::max)((std::min)(sx, sz) / (std::max)(sx, sz), 0.33f);
        const float shortHalf = margin + (halfSize - margin) * aspect;
        if (sx >= sz) halfH = shortHalf; else halfW = shortHalf;
    }
    g_boxHalfH = halfH;

    Layout::Register("Track map", "TrackMap", "PosXFraction", "PosYFraction",
                     &g_trackMapConfig.posXFraction, &g_trackMapConfig.posYFraction,
                     centerX, centerY, halfW, halfH,
                     &g_trackMapConfig.scale, "Scale");

    // Cars move at scoring rate but are interpolated at frame rate, so this
    // is kept brisk enough that the motion still reads as smooth.
    static RenderThrottle s_throttle(0.05);
    if (!Layout::Active() && s_throttle.TryReplay(renderer))
        return;
    RenderThrottle::Capture throttleCapture(s_throttle, renderer);

    if (cfg.bgEnabled)
    {
        const float pad = cfg.bgPaddingPx * uiScale;
        renderer.AddFilledRoundedRect(centerX, centerY, halfW + pad, halfH + pad,
                                      cfg.bgCornerRadiusPx * uiScale, 0.0f,
                                      cfg.bgR, cfg.bgG, cfg.bgB, cfg.bgA);
    }

    if (!g_haveMap || g_raw.size() < 2)
    {
        const bool hadBg = renderer.TextBackgroundEnabled();
        renderer.SetTextBackground(false);
        const float px = cfg.labelHeightPx * uiScale;
        if (g_wantRecording.load(std::memory_order_relaxed))
        {
            renderer.AddText(centerX, centerY - px * 1.1f, "Recording map", px,
                             cfg.trackR, cfg.trackG, cfg.trackB, 0.9f, Renderer::HAlign::Center);
            renderer.AddText(centerX, centerY + px * 0.1f, "drive one clean lap", px * 0.8f,
                             cfg.trackR, cfg.trackG, cfg.trackB, 0.7f, Renderer::HAlign::Center);
        }
        else
        {
            char msg[96];
            if (g_loadAttempted && g_loadedTrack[0])
                sprintf_s(msg, "No map for %s", g_loadedTrack);
            else
                strcpy_s(msg, "No track map");
            renderer.AddText(centerX, centerY - px * 0.5f, msg, px,
                             cfg.trackR, cfg.trackG, cfg.trackB, 0.8f, Renderer::HAlign::Center);
        }
        renderer.SetTextBackground(hadBg);
        return;
    }

    if (!mapReady)
        return;

    const float spanX = (g_maxX - g_minX) > 0.001f ? (g_maxX - g_minX) : 1.0f;
    const float spanZ = (g_maxZ - g_minZ) > 0.001f ? (g_maxZ - g_minZ) : 1.0f;
    const float fitX = (halfW - margin) * 2.0f / spanX;
    const float fitZ = (halfH - margin) * 2.0f / spanZ;
    g_fitScale = (fitX < fitZ) ? fitX : fitZ;
    g_fitOffsetX = centerX - (g_minX + g_maxX) * 0.5f * g_fitScale;
    g_fitOffsetY = centerY - (g_minZ + g_maxZ) * 0.5f * g_fitScale;

    const float lineW = cfg.lineWidthPx * uiScale;
    const float outlineW = lineW + cfg.outlineWidthPx * uiScale * 2.0f;

    // The map is recorded every 2 m, so a few-hundred-pixel map has several
    // points per pixel. Keep only points at least kMinSpacingPx apart on
    // screen: no visible change, and a fraction of the vertices to upload
    // (this geometry is replayed every frame between throttled rebuilds).
    const float kMinSpacingPx = 2.5f;
    static std::vector<float> s_screen;
    static std::vector<float> s_screenDist; // lap distance of each kept point
    s_screen.clear();
    s_screenDist.clear();
    for (size_t i = 0; i < g_baked.size(); ++i)
    {
        float sx = 0.0f, sy = 0.0f;
        BakedToScreen(g_baked[i].x, g_baked[i].z, sx, sy);
        if (!s_screen.empty())
        {
            const float dx = sx - s_screen[s_screen.size() - 2];
            const float dy = sy - s_screen[s_screen.size() - 1];
            if (dx * dx + dy * dy < kMinSpacingPx * kMinSpacingPx)
                continue;
        }
        s_screen.push_back(sx);
        s_screen.push_back(sy);
        s_screenDist.push_back(i < g_lapDist.size() ? g_lapDist[i] : 0.0f);
    }
    // The closing segment runs back to the first point; drop a last point
    // sitting on top of it.
    int screenCount = static_cast<int>(s_screen.size() / 2);
    if (screenCount > 2)
    {
        const float dx = s_screen[(screenCount - 1) * 2] - s_screen[0];
        const float dy = s_screen[(screenCount - 1) * 2 + 1] - s_screen[1];
        if (dx * dx + dy * dy < kMinSpacingPx * kMinSpacingPx)
            --screenCount;
    }
    // Snapped to 1/256 px, far below anything visible: the coloured line is
    // drawn in pieces cut half way along a segment (see SplitRibbon), and on
    // this grid the half-way point and both halves' directions come out
    // exact, so the two pieces' ends are the same vertices -- no hairline
    // gap or overlap where they meet. After the thinning, which still sees
    // the unsnapped points.
    for (int i = 0; i < screenCount * 2; ++i)
        s_screen[i] = floorf(s_screen[i] * 256.0f + 0.5f) * (1.0f / 256.0f);

    // Per-point track colour by time gained/lost there (see DeltaStep), when
    // a colour mode is on and that reference has a lap. s_group: which run
    // of mini-sectors each point is in, for cutting the line
    // into pieces.
    static std::vector<float> s_rgb;
    static std::vector<unsigned short> s_group;
    bool coloured = false;
    if (cfg.deltaColourMode >= 1 && cfg.deltaColourMode <= kRefCount)
    {
        static DeltaLaps s_laps;
        EnterCriticalSection(&g_lock);
        s_laps = g_shDelta;
        LeaveCriticalSection(&g_lock);
        const int m = cfg.deltaColourMode - 1;
        const std::vector<float>& cur = s_laps.cur[m];
        const std::vector<float>& prev = s_laps.prev[m];
        if (s_laps.binM > 0.0f && cur.size() >= 2 && prev.size() == cur.size())
        {
            const size_t bins = cur.size() - 1;
            const float trackLen = s_laps.binM * static_cast<float>(bins);
            const size_t binsPerGroup = (bins + kRibbonGroups - 1) / kRibbonGroups;
            s_rgb.resize(static_cast<size_t>(screenCount) * 3);
            s_group.resize(static_cast<size_t>(screenCount));
            for (int i = 0; i < screenCount; ++i)
            {
                float d = fmodf(s_screenDist[i], trackLen);
                if (d < 0.0f) d += trackLen;
                size_t b = static_cast<size_t>(d / s_laps.binM);
                if (b >= bins) b = bins - 1;
                s_group[i] = static_cast<unsigned short>(b / binsPerGroup);
                // This lap where driven, else last lap's, fainter.
                float loss = NAN, sectorTime = NAN, strength = 1.0f;
                if (!isnan(cur[b]) && !isnan(cur[b + 1]))
                {
                    loss = cur[b + 1] - cur[b];
                    if (s_laps.curT.size() == cur.size())
                        sectorTime = s_laps.curT[b + 1] - s_laps.curT[b];
                }
                else if (!isnan(prev[b]) && !isnan(prev[b + 1]))
                {
                    loss = prev[b + 1] - prev[b];
                    if (s_laps.prevT.size() == prev.size())
                        sectorTime = s_laps.prevT[b + 1] - s_laps.prevT[b];
                    strength = cfg.deltaAheadStrength;
                }
                float r = cfg.trackR, g = cfg.trackG, bl = cfg.trackB;
                // Purple: the session's fastest time through this mini-sector
                // by anyone in the class, matched or beaten (a small margin,
                // since a lap that set it reads back the same time). It wins
                // over the gain/loss colour, at full strength.
                const bool purple = !isnan(sectorTime) && b < s_laps.best.size() && !isnan(s_laps.best[b])
                    && sectorTime <= s_laps.best[b] + 0.002f;
                if (purple)
                {
                    coloured = true;
                    r = cfg.purpleR;
                    g = cfg.purpleG;
                    bl = cfg.purpleB;
                }
                else if (!isnan(loss))
                {
                    coloured = true;
                    // Solid colour, no shading by size: red = lost, green = gained.
                    const float t = 1.0f;
                    const float tr = loss > 0.0f ? cfg.lossR : cfg.gainR;
                    const float tg = loss > 0.0f ? cfg.lossG : cfg.gainG;
                    const float tb = loss > 0.0f ? cfg.lossB : cfg.gainB;
                    r += (tr - r) * t; g += (tg - g) * t; bl += (tb - bl) * t;
                }
                s_rgb[i * 3] = r; s_rgb[i * 3 + 1] = g; s_rgb[i * 3 + 2] = bl;
            }
        }
    }

    // The line is kept on the GPU (Renderer::StaticBatch) and only rebuilt
    // when what it is made of changes: its points, widths or colours. At Spa
    // it is ~20k vertices; sent every frame it was ~90% of the overlay's CPU.
    //
    // One joined ribbon rather than a quad per segment: separate quads left a
    // notch on the outside of every bend and doubled-up halos at each joint.
    //
    // The dark outline underneath never changes colour, so it is a batch of
    // its own, rebuilt only when the shape does. The coloured line is cut
    // into a dozen pieces of a few mini-sectors each (SplitRibbon): a
    // mini-sector turning green or red rebuilds the one or two pieces it is
    // in, not the whole ribbon twice over -- that was ~20k vertices and half
    // a megabyte to the GPU at every mini-sector boundary.
    static RibbonBatch s_outline, s_line;
    static RibbonBatch s_pieces[kMaxRibbonPieces];
    if (cfg.outlineWidthPx > 0.0f)
        DrawRibbon(renderer, s_outline, s_screen.data(), screenCount, true, outlineW,
                   cfg.outlineR, cfg.outlineG, cfg.outlineB, cfg.outlineA, nullptr);
    int cuts[kMaxRibbonPieces];
    const int pieces = coloured
        ? SplitRibbon(s_screen.data(), s_rgb.data(), s_group.data(), screenCount, cuts, kMaxRibbonPieces) : 0;
    if (pieces < 2)
    {
        DrawRibbon(renderer, s_line, s_screen.data(), screenCount, true, lineW,
                   cfg.trackR, cfg.trackG, cfg.trackB, cfg.trackA, coloured ? s_rgb.data() : nullptr);
    }
    else
    {
        // Piece p runs from half way along segment cuts[p] to half way along
        // cuts[p + 1], the last one round past the end of the array.
        static std::vector<float> s_pieceXy, s_pieceRgb;
        auto addPoint = [&](int i)
        {
            s_pieceXy.push_back(s_screen[i * 2]);
            s_pieceXy.push_back(s_screen[i * 2 + 1]);
            s_pieceRgb.insert(s_pieceRgb.end(), s_rgb.begin() + i * 3, s_rgb.begin() + i * 3 + 3);
        };
        auto addHalfWay = [&](int j)
        {
            const int j1 = (j + 1) % screenCount;
            s_pieceXy.push_back(s_screen[j * 2] + (s_screen[j1 * 2] - s_screen[j * 2]) * 0.5f);
            s_pieceXy.push_back(s_screen[j * 2 + 1] + (s_screen[j1 * 2 + 1] - s_screen[j * 2 + 1]) * 0.5f);
            // Both ends of a cut segment have this colour (SplitRibbon).
            s_pieceRgb.insert(s_pieceRgb.end(), s_rgb.begin() + j * 3, s_rgb.begin() + j * 3 + 3);
        };
        for (int p = 0; p < pieces; ++p)
        {
            const int from = cuts[p];
            const int to = cuts[(p + 1) % pieces];
            s_pieceXy.clear();
            s_pieceRgb.clear();
            addHalfWay(from);
            for (int i = (from + 1) % screenCount; ; i = (i + 1) % screenCount)
            {
                addPoint(i);
                if (i == to) break;
            }
            addHalfWay(to);
            DrawRibbon(renderer, s_pieces[p], s_pieceXy.data(), static_cast<int>(s_pieceXy.size() / 2), false,
                       lineW, cfg.trackR, cfg.trackG, cfg.trackB, cfg.trackA, s_pieceRgb.data());
        }
    }

    // Which reference the colours are against, top-left inside the box.
    if (cfg.deltaColourMode >= 1 && cfg.deltaColourMode <= kRefCount)
    {
        const bool hadBg = renderer.TextBackgroundEnabled();
        renderer.SetTextBackground(false);
        const float px = cfg.labelHeightPx * uiScale * 0.85f;
        const char* what = cfg.deltaColourMode == 1 ? "vs my PB"
                         : cfg.deltaColourMode == 2 ? "vs my SB"
                         : "vs session fastest";
        char buf[48];
        sprintf_s(buf, coloured ? "%s" : "%s (no lap yet)", what);
        // Above the map's box, never inside it, so the track line can not
        // run through the text.
        const float boxTop = centerY - halfH - (cfg.bgEnabled ? cfg.bgPaddingPx * uiScale : 0.0f);
        renderer.AddText(centerX - halfW, boxTop - px * 1.25f, buf, px,
                         1.0f, 1.0f, 1.0f, 0.75f, Renderer::HAlign::Left);
        renderer.SetTextBackground(hadBg);
    }

    // North: world +Z, turned with the map. The mirror only flips x, so it
    // does not move. See TrackMapConfig::showNorth.
    if (cfg.showNorth)
    {
        const float rad = g_trackRotationDeg * 3.14159265f / 180.0f;
        const float ux = sinf(rad), uy = -cosf(rad);   // unit vector to north on screen
        // Outside the map's box, above its top-right corner, on a small dark
        // disc: inside the box the track ran through it whenever the
        // circuit filled that corner (2026-11-23).
        const float pad = cfg.bgEnabled ? cfg.bgPaddingPx * uiScale : 0.0f;
        const float discR = 17.0f * uiScale;
        const float ax = centerX + halfW + pad - discR;
        const float ay = centerY - halfH - pad - discR - 4.0f * uiScale;
        renderer.AddFilledCircle(ax, ay, discR, 0.05f, 0.05f, 0.05f, 0.8f);
        const float len = discR * 1.4f;
        const float tipX = ax + ux * len * 0.5f, tipY = ay + uy * len * 0.5f;
        const float tailX = ax - ux * len * 0.5f, tailY = ay - uy * len * 0.5f;
        const float head = len * 0.4f;
        const float thick = 3.0f * uiScale;
        // Shaft plus two barbs at 30 degrees either side.
        renderer.AddLineSmooth(tailX, tailY, tipX, tipY, thick, 1.0f, 1.0f, 1.0f, 0.85f);
        const float c30 = 0.866f, s30 = 0.5f;
        const float bx = -ux, by = -uy;
        renderer.AddLineSmooth(tipX, tipY, tipX + (bx * c30 - by * s30) * head, tipY + (bx * s30 + by * c30) * head,
                               thick, 1.0f, 1.0f, 1.0f, 0.85f);
        renderer.AddLineSmooth(tipX, tipY, tipX + (bx * c30 + by * s30) * head, tipY + (-bx * s30 + by * c30) * head,
                               thick, 1.0f, 1.0f, 1.0f, 0.85f);
        const bool hadBg = renderer.TextBackgroundEnabled();
        renderer.SetTextBackground(false);
        // "N" just left of the disc, upright, so it never sits on the arrow.
        const float np = 22.0f * uiScale;
        renderer.AddText(ax - discR - 5.0f * uiScale, ay - np * 0.5f, "N", np,
                         1.0f, 1.0f, 1.0f, 0.9f, Renderer::HAlign::Right);
        renderer.SetTextBackground(hadBg);
    }

    // Re-recording in progress (see TrackMap::Rerecord): the old map stays
    // until the new lap is in.
    if (g_wantRecording.load(std::memory_order_relaxed))
    {
        const bool hadBg = renderer.TextBackgroundEnabled();
        renderer.SetTextBackground(false);
        const float px = cfg.labelHeightPx * uiScale * 0.85f;
        renderer.AddText(centerX, centerY + halfH - px * 1.3f, "Re-recording map: drive a clean lap", px,
                         1.0f, 0.85f, 0.2f, 0.9f, Renderer::HAlign::Center);
        renderer.SetTextBackground(hadBg);
    }

    if (cfg.showSectors)
    {
        struct Mark { int index; float r, g, b; };
        const Mark marks[3] = {
            { 0,              cfg.startR,  cfg.startG,  cfg.startB },
            { g_sector1Index, cfg.sectorR, cfg.sectorG, cfg.sectorB },
            { g_sector2Index, cfg.sectorR, cfg.sectorG, cfg.sectorB },
        };
        const int n = static_cast<int>(g_baked.size());
        for (const Mark& m : marks)
        {
            if (m.index < 0 || m.index >= n) continue;
            const int next = (m.index + 1) % n;
            float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
            BakedToScreen(g_baked[m.index].x, g_baked[m.index].z, ax, ay);
            BakedToScreen(g_baked[next].x, g_baked[next].z, bx, by);
            float dx = bx - ax, dy = by - ay;
            const float len = sqrtf(dx * dx + dy * dy);
            if (len < 0.0001f) continue;
            dx /= len; dy /= len;
            const float half = (lineW + cfg.outlineWidthPx * uiScale) * 0.9f;
            renderer.AddLineSmooth(ax - dy * half, ay + dx * half,
                                   ax + dy * half, ay - dx * half,
                                   cfg.markWidthPx * uiScale, m.r, m.g, m.b, 1.0f);
        }
    }

    // --- Cars --------------------------------------------------------------
    CarSample cars[kMaxCars];
    int carCount = 0;
    {
        EnterCriticalSection(&g_lock);

        // Interpolate lap distance between the two most recent scoring
        // samples, so dots glide at frame rate rather than stepping five
        // times a second.
        double frac = 1.0;
        if (g_qpcFreq.QuadPart != 0 && g_prevTime.QuadPart != 0 && g_curTime.QuadPart != 0)
        {
            double period = static_cast<double>(g_curTime.QuadPart - g_prevTime.QuadPart) /
                            static_cast<double>(g_qpcFreq.QuadPart);
            if (period > 0.5) period = 0.5;
            if (period > 0.001)
            {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                double elapsed = static_cast<double>(now.QuadPart - g_curTime.QuadPart) /
                                 static_cast<double>(g_qpcFreq.QuadPart);
                if (elapsed < 0.0) elapsed = 0.0;
                if (elapsed > period) elapsed = period;
                frac = elapsed / period;
            }
        }

        for (int i = 0; i < g_curCount && carCount < kMaxCars; ++i)
        {
            CarSample s = g_curCars[i];
            if (frac < 1.0)
            {
                for (int j = 0; j < g_prevCount; ++j)
                {
                    if (g_prevCars[j].id != s.id) continue;
                    const double a = g_prevCars[j].lapDist;
                    double b = s.lapDist;
                    // Crossing the line makes b jump back to near zero;
                    // unwrap before interpolating, or the dot sprints
                    // backwards round the whole circuit.
                    if (g_trackLength > 1.0f && a - b > g_trackLength * 0.5)
                        b += g_trackLength;
                    s.lapDist = a + (b - a) * frac;
                    s.pathLateral = g_prevCars[j].pathLateral +
                                    (s.pathLateral - g_prevCars[j].pathLateral) * frac;
                    break;
                }
            }
            cars[carCount++] = s;
        }
        LeaveCriticalSection(&g_lock);
    }

    if (carCount <= 0)
        return;

    // Every car's place, colour and code from one DriverAbbrev snapshot (one
    // lock). The sort below used to ask DriverAbbrev for a place -- a lock
    // and a search of the field -- inside every comparison, ~1000 times a
    // redraw with 60 cars, and the colour and code once more per car.
    static DriverAbbrev::CarInfo s_info[kMaxCars];
    const int infoCount = DriverAbbrev::Snapshot(s_info, kMaxCars);
    int infoOf[kMaxCars];   // each car's entry in s_info, or -1 if scoring has not described it yet
    int rank[kMaxCars];
    for (int i = 0; i < carCount; ++i)
    {
        infoOf[i] = -1;
        for (int j = 0; j < infoCount; ++j)
            if (s_info[j].id == cars[i].id) { infoOf[i] = j; break; }
        const int place = infoOf[i] >= 0 ? s_info[infoOf[i]].place : 0;
        rank[i] = cars[i].isPlayer ? 100000 : (place ? place : 9999);
    }

    // Leader first, so an overlapping label is kept by the car further up the
    // order -- except your own car, drawn last so it sits on top of the pack.
    int order[kMaxCars];
    for (int i = 0; i < carCount; ++i) order[i] = i;
    std::stable_sort(order, order + carCount, [&](int a, int b) { return rank[a] < rank[b]; });

    const float dotR = cfg.carRadiusPx * uiScale;
    const float labelPx = cfg.labelHeightPx * uiScale;
    const float minSep = cfg.labelMinSeparationPx * uiScale;

    float labelX[kMaxCars], labelY[kMaxCars];
    int labelCount = 0;

    const bool hadBg = renderer.TextBackgroundEnabled();
    renderer.SetTextBackground(false);

    for (int k = 0; k < carCount; ++k)
    {
        const CarSample& c = cars[order[k]];
        const DriverAbbrev::CarInfo* info = infoOf[order[k]] >= 0 ? &s_info[infoOf[order[k]]] : nullptr;
        float cxp = 0.0f, cyp = 0.0f;
        PlaceByLapDist(c.lapDist, c.pathLateral, cxp, cyp);

        // Your own car keeps its fixed colour by default, so it is findable
        // without remembering which hue you were given this session.
        float r = cfg.carR, g = cfg.carG, b = cfg.carB;
        if (c.isPlayer && cfg.playerOwnColor)
        {
            r = cfg.playerR; g = cfg.playerG; b = cfg.playerB;
        }
        else if (info && info->hasColor)
        {
            r = info->r; g = info->g; b = info->b;
        }

        // A larger dot as well as its own colour: two independent cues.
        const float thisDotR = c.isPlayer ? dotR * cfg.playerRadiusScale : dotR;
        renderer.AddFilledCircle(cxp, cyp, thisDotR + cfg.carOutlineWidthPx * uiScale,
                                 cfg.outlineR, cfg.outlineG, cfg.outlineB, 1.0f, 16);
        renderer.AddFilledCircle(cxp, cyp, thisDotR, r, g, b, 1.0f, 16);

        if (!cfg.showLabels)
            continue;

        bool crowded = false;
        for (int i = 0; i < labelCount && !crowded; ++i)
        {
            const float ddx = labelX[i] - cxp;
            const float ddy = labelY[i] - cyp;
            crowded = (ddx * ddx + ddy * ddy) < (minSep * minSep);
        }
        if (crowded)
            continue;

        const char* code = info ? info->code : "";
        if (!code[0])
            continue;

        renderer.AddText(cxp, cyp + thisDotR + labelPx * 0.15f, code, labelPx,
                         cfg.labelR, cfg.labelG, cfg.labelB, 1.0f,
                         Renderer::HAlign::Center);

        if (labelCount < kMaxCars)
        {
            labelX[labelCount] = cxp;
            labelY[labelCount] = cyp;
            ++labelCount;
        }
    }

    renderer.SetTextBackground(hadBg);
}

// The rotation slider and mirror button, shown only in layout mode.
//
// Absolute slider over -180..+180: the knob position directly encodes the
// angle, so it is always reachable and always says what the current value is
// -- the map itself gives no clue how far it has been turned. Draw() rebakes
// whenever the angle no longer matches what it baked with, so the circuit
// turns under the cursor as the knob moves.
void TrackMap::DrawLayoutControls(Renderer& renderer, int screenWidth, int screenHeight, HWND hwnd)
{
    if (!Layout::Active())
    {
        g_rotDragging = false;
        return;
    }
    // Nothing to rotate, and no circuit name to save an angle under.
    if (!g_haveMap || !g_loadedTrack[0])
        return;

    const TrackMapConfig& cfg = g_trackMapConfig;
    const float uiScale = static_cast<float>(screenHeight) / 2160.0f;

    // Directly under the map, centred on it, so it follows the map when the
    // map is dragged somewhere else.
    const float mapCx = screenWidth * cfg.posXFraction;
    const float mapCy = screenHeight * cfg.posYFraction;
    const float mapHalf = g_boxHalfH; // this frame's box, as Draw sized it

    const float sliderW = 420.0f * uiScale;
    const float sliderH = 10.0f * uiScale;
    const float buttonW = 120.0f * uiScale;
    const float buttonH = 44.0f * uiScale;
    const float gap = 24.0f * uiScale;
    const float totalW = sliderW + gap + buttonW;
    const float sliderX = mapCx - totalW * 0.5f;
    const float sliderY = mapCy + mapHalf + 70.0f * uiScale;
    const float buttonCx = sliderX + sliderW + gap + buttonW * 0.5f;
    const float knobR = 12.0f * uiScale;
    const float labelPx = 36.0f * uiScale;
    const float capPx = 28.0f * uiScale;

    const float kMin = -180.0f, kMax = 180.0f;
    const float frac = (g_trackRotationDeg - kMin) / (kMax - kMin);
    const float clampedFrac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
    const float knobX = sliderX + clampedFrac * sliderW;

    float cx = 0.0f, cy = 0.0f;
    const bool haveCursor = Layout::CursorPosition(hwnd, screenWidth, screenHeight, cx, cy);
    const bool buttonDown = haveCursor && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool clickEdge = buttonDown && !g_prevButton;

    if (clickEdge && !g_rotDragging &&
        cx >= sliderX - knobR && cx <= sliderX + sliderW + knobR &&
        cy >= sliderY - knobR * 1.5f && cy <= sliderY + knobR * 1.5f)
    {
        g_rotDragging = true;
    }
    if (g_rotDragging && buttonDown)
    {
        float t = (cx - sliderX) / sliderW;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        g_trackRotationDeg = kMin + t * (kMax - kMin);
    }
    else if (g_rotDragging && !buttonDown)
    {
        g_rotDragging = false;
        // Written on RELEASE, not while dragging -- one queued write per
        // drag rather than one per frame.
        PersistTrackRotation();
    }

    if (clickEdge && !g_rotDragging &&
        fabsf(cx - buttonCx) <= buttonW * 0.5f && fabsf(cy - sliderY) <= buttonH * 0.5f)
    {
        g_trackMirror = !g_trackMirror;
        PersistTrackMirror();
    }
    g_prevButton = buttonDown;

    const bool hadBg = renderer.TextBackgroundEnabled();
    renderer.SetTextBackground(false);

    renderer.AddFilledRoundedRect(sliderX + sliderW * 0.5f, sliderY,
                                  sliderW * 0.5f, sliderH * 0.5f, sliderH * 0.5f, 0.0f,
                                  1.0f, 1.0f, 1.0f, 0.35f);
    // A centre tick, so "no rotation" is findable by eye.
    renderer.AddFilledRect(sliderX + sliderW * 0.5f, sliderY,
                           1.5f * uiScale, sliderH * 1.4f, 0.0f,
                           1.0f, 1.0f, 1.0f, 0.55f);
    renderer.AddFilledCircle(knobX, sliderY, knobR, 1.0f, 0.85f, 0.2f,
                             g_rotDragging ? 1.0f : 0.9f);

    // Mirror button: lit while the map is flipped.
    renderer.AddFilledRoundedRect(buttonCx, sliderY, buttonW * 0.5f, buttonH * 0.5f, 8.0f * uiScale, 0.0f,
                                  g_trackMirror ? 1.0f : 0.25f, g_trackMirror ? 0.85f : 0.25f,
                                  g_trackMirror ? 0.2f : 0.25f, 0.9f);
    renderer.AddText(buttonCx, sliderY - capPx * 0.5f, "Mirror", capPx,
                     g_trackMirror ? 0.0f : 1.0f, g_trackMirror ? 0.0f : 1.0f, g_trackMirror ? 0.0f : 1.0f,
                     1.0f, Renderer::HAlign::Center);

    char cap[96];
    sprintf_s(cap, "Map rotation  %.0f deg", g_trackRotationDeg);
    renderer.AddText(mapCx, sliderY - buttonH * 0.5f - labelPx - capPx - 6.0f * uiScale,
                     cap, labelPx, 1.0f, 1.0f, 1.0f, 0.85f, Renderer::HAlign::Center);
    sprintf_s(cap, "saved for %s", g_loadedTrack);
    renderer.AddText(mapCx, sliderY - buttonH * 0.5f - capPx - 2.0f * uiScale,
                     cap, capPx, 1.0f, 1.0f, 1.0f, 0.65f, Renderer::HAlign::Center);

    renderer.SetTextBackground(hadBg);
}

bool TrackMap::HasMap()
{
    return g_haveMap && g_loadedTrack[0] != 0;
}

float TrackMap::RotationDegrees()
{
    return g_trackRotationDeg;
}

void TrackMap::SetRotationDegrees(float degrees, bool persist)
{
    if (!HasMap())
        return;
    if (degrees < -180.0f) degrees = -180.0f;
    if (degrees > 180.0f) degrees = 180.0f;
    g_trackRotationDeg = degrees;
    if (persist)
        PersistTrackRotation();
}

bool TrackMap::Mirrored()
{
    return g_trackMirror;
}

void TrackMap::ToggleMirror()
{
    if (!HasMap())
        return;
    g_trackMirror = !g_trackMirror;
    PersistTrackMirror();
}

void TrackMap::ToggleRerecord()
{
    if (!HasMap())
        return;
    const bool now = !g_wantRecording.load(std::memory_order_relaxed);
    g_wantRecording.store(now, std::memory_order_relaxed);
    Log::Write("TrackMap: %s re-recording '%s'", now ? "started" : "cancelled", g_loadedTrack);
}

bool TrackMap::Rerecording()
{
    return HasMap() && g_wantRecording.load(std::memory_order_relaxed);
}

void TrackMap::Shutdown()
{
    // After the Present hook is removed, so Draw no longer touches the handle.
    if (g_mapJob.thread)
        WaitForSingleObject(g_mapJob.thread, 3000);
}
