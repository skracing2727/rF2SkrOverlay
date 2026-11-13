#include "PitEntry.h"
#include "InternalsPlugin.hpp"
#include "Config.h"
#include "Log.h"
#include <cmath>
#include <cstring>

PitEntryStore g_pitEntry;

namespace
{
    // Same DLL-relative path resolution used elsewhere, so the per-track
    // offset files live next to the INI.
    void GetEntryOffsetDir(char* out, size_t outSize)
    {
        char dllPath[MAX_PATH] = {};
        HMODULE hModule = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCSTR>(&GetEntryOffsetDir), &hModule);
        GetModuleFileNameA(hModule, dllPath, MAX_PATH);
        char* lastSlash = strrchr(dllPath, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';
        sprintf_s(out, outSize, "%sPitEntryOffsets", dllPath);
    }

    // Bounded by the SDK's fixed buffer size: mTrackName is a plain char[64]
    // with no guaranteed terminator.
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
}

void PitEntryStore::Init()
{
    if (m_ready) return;
    InitializeCriticalSection(&m_lock);
    m_ready = true;
}

void PitEntryStore::LoadEntryOffset(const char* trackName)
{
    char dir[MAX_PATH];
    GetEntryOffsetDir(dir, sizeof(dir));
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s.txt", dir, m_trackKey.c_str());

    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f)
    {
        m_entryOffsetMeters = g_pitLimiterConfig.entryOffsetMeters;
        Log::Write("PitEntry: no per-track entry offset for %s -- using INI default %.1fm", trackName, m_entryOffsetMeters);
        return;
    }
    char line[64];
    m_entryOffsetMeters = fgets(line, sizeof(line), f) ? atof(line) : g_pitLimiterConfig.entryOffsetMeters;
    fclose(f);
    Log::Write("PitEntry: loaded entry offset %.1fm for %s", m_entryOffsetMeters, trackName);
}

void PitEntryStore::SaveEntryOffset(const char* trackName) const
{
    char dir[MAX_PATH];
    GetEntryOffsetDir(dir, sizeof(dir));
    CreateDirectoryA(dir, nullptr);
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s.txt", dir, m_trackKey.c_str());

    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f)
    {
        Log::Write("PitEntry: failed to save entry offset to %s", path);
        return;
    }
    fprintf(f, "%.2f\n", m_entryOffsetMeters);
    fclose(f);
    Log::Write("PitEntry: auto-calibrated entry offset %.2fm for %s", m_entryOffsetMeters, trackName);
}

void PitEntryStore::OnScoring(const ScoringInfoV01& info)
{
    if (!m_ready || info.mVehicle == nullptr)
        return;

    const VehicleScoringInfoV01* player = nullptr;
    for (long i = 0; i < info.mNumVehicles; ++i)
        if (info.mVehicle[i].mIsPlayer) { player = &info.mVehicle[i]; break; }
    if (!player)
        return;

    m_trackLength = info.mLapDist;

    const std::string trackKey = Sanitize(info.mTrackName, sizeof(info.mTrackName));
    if (trackKey != m_trackKey)
    {
        m_trackKey = trackKey;
        LoadEntryOffset(info.mTrackName);
        m_hasInPitsBaseline = false;
    }

    const int pitState = static_cast<int>(player->mPitState); // 0=none,1=request,2=entering,3=stopped,4=exiting

    // Only a false->true mInPits transition while a stop is being approached
    // counts as a real entry-line crossing.
    if (!m_hasInPitsBaseline)
    {
        m_lastInPits = player->mInPits;
        m_hasInPitsBaseline = true;
    }
    else if (player->mInPits && !m_lastInPits && (pitState == 1 || pitState == 2))
    {
        // Shortest-path signed difference, so a crossing on the other side
        // of the start/finish line from mPitLapDist stays a small correction.
        double observedOffset = player->mLapDist - player->mPitLapDist;
        if (m_trackLength > 0.0)
        {
            if (observedOffset > m_trackLength * 0.5) observedOffset -= m_trackLength;
            else if (observedOffset < -m_trackLength * 0.5) observedOffset += m_trackLength;
        }
        m_entryOffsetMeters = observedOffset;
        SaveEntryOffset(info.mTrackName);
    }
    m_lastInPits = player->mInPits;

    if (pitState == 1 && !m_armed && m_trackLength > 0.0)
    {
        m_armed = true;
        m_pitLineLapDist = player->mPitLapDist + m_entryOffsetMeters;
        m_pitLineLapDist = fmod(fmod(m_pitLineLapDist, m_trackLength) + m_trackLength, m_trackLength);
    }
    else if (pitState == 0 && m_armed)
    {
        m_armed = false;
    }

    double distToLine = 0.0;
    if (m_armed && m_trackLength > 0.0)
        distToLine = fmod(m_pitLineLapDist - player->mLapDist + m_trackLength, m_trackLength);

    EnterCriticalSection(&m_lock);
    m_liveState.armed = m_armed;
    m_liveState.distanceToLineM = distToLine;
    LeaveCriticalSection(&m_lock);
}

void PitEntryStore::Clear()
{
    m_armed = false;
    if (!m_ready) return;
    EnterCriticalSection(&m_lock);
    m_liveState = State{};
    LeaveCriticalSection(&m_lock);
}

PitEntryStore::State PitEntryStore::Read()
{
    State copy;
    if (!m_ready) return copy;
    EnterCriticalSection(&m_lock);
    copy = m_liveState;
    LeaveCriticalSection(&m_lock);
    return copy;
}