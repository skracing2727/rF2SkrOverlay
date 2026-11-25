#include "ReplayInfo.h"
#include "Log.h"
#include <Windows.h>
#include <cstdio>
#include <share.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    SRWLOCK g_lock = SRWLOCK_INIT;
    ReplayInfo::State g_state;

    // Poller thread only.
    char g_gameDir[MAX_PATH] = "";      // the folder holding Bin64 and UserData
    FILETIME g_jsonWrite = {};
    bool g_jsonRead = false;
    bool g_recording = false, g_toMemory = false;
    int g_fidelity = -1;
    FILETIME g_gameStart = {};
    ULONGLONG g_lastScan = 0;
    std::wstring g_liveFile;            // the recording file found by the last scan

    bool FindGameDir()
    {
        if (g_gameDir[0])
            return true;
        // The DLL is in <game>\Bin64\Plugins: two folders up.
        HMODULE self = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(&FindGameDir), &self))
            return false;
        char path[MAX_PATH];
        if (!GetModuleFileNameA(self, path, MAX_PATH))
            return false;
        for (int up = 0; up < 3; ++up)
        {
            char* slash = strrchr(path, '\\');
            if (!slash) return false;
            *slash = '\0';
        }
        strcpy_s(g_gameDir, path);
        FILETIME create, exitT, kernel, user;
        if (GetProcessTimes(GetCurrentProcess(), &create, &exitT, &kernel, &user))
            g_gameStart = create;
        return true;
    }

    // Value after "key": in a JSON text -- a number or true/false. Enough for
    // the three flat keys wanted here, without a JSON parser.
    bool FindValue(const std::string& text, const char* key, std::string& out)
    {
        const std::string needle = std::string("\"") + key + "\":";
        const size_t at = text.find(needle);
        if (at == std::string::npos)
            return false;
        size_t i = at + needle.size();
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
        size_t j = i;
        while (j < text.size() && text[j] != ',' && text[j] != '\r' && text[j] != '\n' && text[j] != '}') ++j;
        out = text.substr(i, j - i);
        return !out.empty();
    }

    bool Truthy(const std::string& v)
    {
        return v == "true" || (!v.empty() && v != "false" && atoi(v.c_str()) != 0);
    }

    void ReadPlayerJson()
    {
        char path[MAX_PATH];
        sprintf_s(path, "%s\\UserData\\player\\player.JSON", g_gameDir);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa))
            return;
        if (g_jsonRead && CompareFileTime(&fa.ftLastWriteTime, &g_jsonWrite) == 0)
            return;                     // unchanged since the last read
        // Shared open: fopen_s blocked rF2 from saving its own profile while
        // we read it (review F281).
        FILE* f = _fsopen(path, "rb", _SH_DENYNO);
        if (!f)
            return;
        std::string text;
        char buf[16384];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
            text.append(buf, got);
        fclose(f);
        std::string v;
        // Caught mid-save (key missing): keep the old values, read again next poll.
        if (!FindValue(text, "Record Replays", v))
            return;
        g_jsonWrite = fa.ftLastWriteTime;
        g_jsonRead = true;
        g_recording = FindValue(text, "Record Replays", v) && Truthy(v);
        g_toMemory = FindValue(text, "Record To Memory", v) && Truthy(v);
        g_fidelity = FindValue(text, "Replay Base Fidelity", v) ? atoi(v.c_str()) : -1;
        Log::Write("ReplayInfo: record replays %d, to memory %d, fidelity %d",
                   g_recording ? 1 : 0, g_toMemory ? 1 : 0, g_fidelity);
    }

    // The live recording file: the newest _vcr*.tmp CREATED since the game
    // started (creation, not last-write: the directory entry's write time is
    // only brought up to date lazily while the file is held open). Looked for every few seconds; in between only its size is read.
    void ScanForLiveFile()
    {
        wchar_t pattern[MAX_PATH];
        swprintf_s(pattern, L"%hs\\UserData\\Replays\\_vcr*.tmp", g_gameDir);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern, &fd);
        g_liveFile.clear();
        if (h == INVALID_HANDLE_VALUE)
            return;
        FILETIME best = g_gameStart;
        do
        {
            if (CompareFileTime(&fd.ftCreationTime, &best) > 0)
            {
                best = fd.ftCreationTime;
                wchar_t full[MAX_PATH];
                swprintf_s(full, L"%hs\\UserData\\Replays\\%s", g_gameDir, fd.cFileName);
                g_liveFile = full;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void ReplayInfo::Poll()
{
    if (!FindGameDir())
        return;
    ReadPlayerJson();

    State s;
    s.known = g_jsonRead;
    s.recording = g_recording;
    s.toMemory = g_toMemory;
    s.fidelity = g_fidelity;
    if (g_recording && !g_toMemory)
    {
        const ULONGLONG now = GetTickCount64();
        if (g_liveFile.empty() ? now - g_lastScan >= 5000 : now - g_lastScan >= 30000)
        {
            g_lastScan = now;
            ScanForLiveFile();
        }
        // The size from the file's own metadata (no handle, no read): stays
        // current while rF2 keeps appending to it.
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!g_liveFile.empty() && GetFileAttributesExW(g_liveFile.c_str(), GetFileExInfoStandard, &fa))
        {
            const ULONGLONG bytes = (static_cast<ULONGLONG>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
            s.haveFile = true;
            s.fileMB = static_cast<double>(bytes) / (1024.0 * 1024.0);
        }
        else if (!g_liveFile.empty())
        {
            g_liveFile.clear();         // saved and deleted at session end: look again soon
            g_lastScan = 0;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    g_state = s;
    ReleaseSRWLockExclusive(&g_lock);
}

ReplayInfo::State ReplayInfo::Get()
{
    AcquireSRWLockShared(&g_lock);
    const State s = g_state;
    ReleaseSRWLockShared(&g_lock);
    return s;
}

const char* ReplayInfo::FidelityName(int fidelity)
{
    // The game's options screen: Low, Medium, High, Max (value 3 = High was
    // confirmed from its settings cache; 0 is below the menu's range).
    switch (fidelity)
    {
    case 1: return "Low";
    case 2: return "Medium";
    case 3: return "High";
    case 4: return "Max";
    default: return nullptr;
    }
}
