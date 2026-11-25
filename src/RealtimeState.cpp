#include "RealtimeState.h"
#include "EcoThread.h"
#include <Windows.h>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <winhttp.h>
#include "Log.h"

// rF2 REST car lookup (localhost only), see VehicleKey below.
#pragma comment(lib, "winhttp.lib")

std::atomic<bool> g_inRealtime{ false };
std::atomic<int> g_fuelMult{ -1 };
std::atomic<int> g_headlightsOn{ -1 };
std::atomic<bool> g_playerAiControl{ false };
std::atomic<int> g_autoShift{ -1 };

namespace
{
    // Written on the scoring thread, read on the telemetry and render
    // threads. Short fixed buffers under a critical section rather than a
    // std::string, so a reader can never see a half-reallocated string.
    // g_keyLock also guards the worker's request/result fields below.
    CRITICAL_SECTION g_keyLock;
    bool g_keyLockReady = false;
    char g_vehicleKey[64] = "unknown";   // published car key
    char g_classKey[64] = "unknown";     // the class published with it

    // Scoring thread only.
    char s_rawClass[64] = "";
    char s_rawEntry[64] = "";
    bool s_haveRaw = false;
    bool s_pending = false;              // waiting for the worker's answer
    ULONGLONG s_deadline = 0;
    unsigned s_gen = 0;
    std::map<std::string, std::string> s_modelByEntry;   // "" = REST had no single match

    // REST worker (review F169/F261). One thread, woken per car change.
    HANDLE g_thread = nullptr;
    HANDLE g_wake = nullptr;
    std::atomic<bool> g_stop{ false };
    std::atomic<HINTERNET> g_session{ nullptr };   // so Shutdown can abort a request
    // Under g_keyLock.
    unsigned g_reqGen = 0;
    char g_reqEntry[64] = "";
    bool g_reqWaiting = false;
    unsigned g_resGen = 0;
    bool g_resReady = false;
    bool g_resCacheable = false;   // a real answer (not a connection failure)
    std::string g_resModel;        // "" = no match / not reachable
    char g_resWhy[96] = "";

    const INTERNET_PORT kRestPort = 5397;           // rF2's default WebUI port
    const size_t kMaxBody = 64u * 1024u * 1024u;
    const ULONGLONG kAnswerWaitMs = 3000;

    void Publish(const char* car, const char* cls, const char* why)
    {
        EnterCriticalSection(&g_keyLock);
        strncpy_s(g_vehicleKey, sizeof(g_vehicleKey), car, _TRUNCATE);
        strncpy_s(g_classKey, sizeof(g_classKey), cls, _TRUNCATE);
        LeaveCriticalSection(&g_keyLock);
        Log::Write("VehicleKey: car key '%s' (class '%s', entry '%s') -- %s", car, cls, s_rawEntry, why);
    }

    // The entry name, or the class if the mod leaves that blank, never empty.
    const char* Fallback()
    {
        return s_rawEntry[0] ? s_rawEntry : (s_rawClass[0] ? s_rawClass : "unknown");
    }

    // --- JSON (just enough for /rest/race/car: an array of flat objects) --

    // Reads a JSON string starting at s[i] == '"'; i ends after the closing quote.
    std::string ReadJsonString(const std::string& s, size_t& i)
    {
        std::string out;
        ++i;
        while (i < s.size() && s[i] != '"')
        {
            char c = s[i++];
            if (c != '\\' || i >= s.size()) { out.push_back(c); continue; }
            c = s[i++];
            switch (c)
            {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': case 'f': break;
            case 'u':
            {
                unsigned cp = 0;
                for (int k = 0; k < 4 && i < s.size(); ++k, ++i)
                {
                    const char h = s[i];
                    cp = cp * 16 + ((h >= '0' && h <= '9') ? h - '0' : ((h | 32) >= 'a' && (h | 32) <= 'f') ? (h | 32) - 'a' + 10 : 0);
                }
                if (cp < 0x80) out.push_back(static_cast<char>(cp));
                else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                else { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
                break;
            }
            default: out.push_back(c); break;   // \" \\ \/
            }
        }
        ++i;
        return out;
    }

    // "...\Installed\Vehicles\<MODEL>\<version>\<livery>.VEH" -> <MODEL>.
    std::string ModelFromVehFile(const std::string& veh)
    {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : veh)
        {
            if (c == '\\' || c == '/') { if (!cur.empty()) parts.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) parts.push_back(cur);
        for (size_t k = 0; k + 2 < parts.size(); ++k)
            if (_stricmp(parts[k].c_str(), "Vehicles") == 0)
                return parts[k + 1];
        if (parts.size() >= 3) return parts[parts.size() - 3];   // folder above the version folder
        if (parts.size() == 2) return parts[0];
        return std::string();
    }

    // Scoring names are char[64]: a longer REST name matches on its first 63.
    bool SameEntry(const std::string& restName, const char* entry)
    {
        const size_t n = strlen(entry);
        if (restName.size() == n) return restName == entry;
        return n == 63 && restName.size() > n && restName.compare(0, n, entry) == 0;
    }

    // The model of the one vehicle whose "name" is this entry; "" with a
    // reason when there is none or the name is in more than one model.
    std::string FindModel(const std::string& body, const char* entry, char* why, size_t whySize)
    {
        std::string model;
        int matches = 0;
        bool ambiguous = false;
        int depth = 0;
        std::string name, veh, key;
        int nameDepth = -1, vehDepth = -1;   // object depth each was read at
        bool haveKey = false;
        for (size_t i = 0; i < body.size();)
        {
            const char c = body[i];
            if (c == '"')
            {
                std::string str = ReadJsonString(body, i);
                size_t j = i;
                while (j < body.size() && (body[j] == ' ' || body[j] == '\t' || body[j] == '\r' || body[j] == '\n')) ++j;
                if (j < body.size() && body[j] == ':') { key.swap(str); haveKey = true; i = j + 1; continue; }
                if (haveKey)
                {
                    // An outer field is not overwritten by a nested object's.
                    if (key == "name" && (nameDepth < 0 || depth <= nameDepth)) { name = str; nameDepth = depth; }
                    else if (key == "vehFile" && (vehDepth < 0 || depth <= vehDepth)) { veh = str; vehDepth = depth; }
                }
                haveKey = false;
                continue;
            }
            if (c == '{' || c == '[')
                ++depth;
            else if (c == '}' || c == ']')
            {
                // One vehicle object closes: both fields must be its own.
                if (c == '}' && nameDepth == depth && vehDepth == depth && SameEntry(name, entry))
                {
                    const std::string m = ModelFromVehFile(veh);
                    if (!m.empty())
                    {
                        if (matches > 0 && _stricmp(m.c_str(), model.c_str()) != 0) ambiguous = true;
                        model = m;
                        ++matches;
                    }
                }
                if (nameDepth >= depth) nameDepth = -1;
                if (vehDepth >= depth) vehDepth = -1;
                --depth;
            }
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != ',' && c != ':') haveKey = false;
            ++i;
        }
        if (ambiguous) { sprintf_s(why, whySize, "entry name is in more than one vehicle folder"); return std::string(); }
        if (matches == 0) { sprintf_s(why, whySize, "entry not found in the REST car list"); return std::string(); }
        sprintf_s(why, whySize, "vehicle folder from rF2 REST");
        return model;
    }

    // GET http://127.0.0.1:5397/rest/race/car. Local machine only; nothing
    // is ever sent anywhere else. Short timeouts so a car change never
    // waits long for a game that has the web interface turned off.
    bool FetchCarList(std::string& body, char* why, size_t whySize)
    {
        HINTERNET session = WinHttpOpen(L"rF2SkrOverlay", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) { sprintf_s(why, whySize, "WinHttpOpen failed (%lu)", GetLastError()); return false; }
        WinHttpSetTimeouts(session, 500, 500, 1000, 1500);
        g_session.store(session);
        if (g_stop.load()) { if (g_session.exchange(nullptr)) WinHttpCloseHandle(session); return false; }
        bool ok = false;
        HINTERNET connect = WinHttpConnect(session, L"127.0.0.1", kRestPort, 0);
        HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", L"/rest/race/car", nullptr,
                                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
        if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            && WinHttpReceiveResponse(request, nullptr))
        {
            DWORD status = 0, len = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
            if (status == 200)
            {
                ok = true;
                char buf[16384];
                for (;;)
                {
                    DWORD got = 0;
                    if (!WinHttpReadData(request, buf, sizeof(buf), &got)) { ok = false; break; }
                    if (got == 0) break;
                    if (body.size() + got > kMaxBody) { ok = false; break; }
                    body.append(buf, got);
                }
                if (!ok) sprintf_s(why, whySize, "REST read failed or too large");
            }
            else
                sprintf_s(why, whySize, "REST answered HTTP %lu", status);
        }
        else
            sprintf_s(why, whySize, "rF2 REST not reachable on port %u (%lu)", static_cast<unsigned>(kRestPort), GetLastError());
        if (request) WinHttpCloseHandle(request);
        if (connect) WinHttpCloseHandle(connect);
        if (g_session.exchange(nullptr)) WinHttpCloseHandle(session);   // else Shutdown closed it
        return ok;
    }

    DWORD WINAPI WorkerProc(LPVOID)
    {
        while (WaitForSingleObject(g_wake, INFINITE) == WAIT_OBJECT_0 && !g_stop.load())
        {
            unsigned gen = 0;
            char entry[64] = "";
            EnterCriticalSection(&g_keyLock);
            const bool have = g_reqWaiting;
            if (have) { gen = g_reqGen; strcpy_s(entry, g_reqEntry); g_reqWaiting = false; }
            LeaveCriticalSection(&g_keyLock);
            if (!have) continue;

            char why[96] = "";
            std::string body, model;
            bool cacheable = false;
            try
            {
                if (FetchCarList(body, why, sizeof(why)))
                {
                    model = FindModel(body, entry, why, sizeof(why));
                    cacheable = true;
                }
            }
            catch (...) { sprintf_s(why, "REST lookup failed"); model.clear(); cacheable = false; }

            EnterCriticalSection(&g_keyLock);
            g_resGen = gen;
            g_resReady = true;
            g_resCacheable = cacheable;
            g_resModel = model;
            strcpy_s(g_resWhy, why);
            LeaveCriticalSection(&g_keyLock);
        }
        return 0;
    }
}

void VehicleKey::Init()
{
    // Must run before Get()/SetFromScoring() can be called from more than
    // one thread. A lazy "if not ready, InitializeCriticalSection" guard
    // here instead would itself be a race: the scoring and telemetry
    // callbacks this lock protects can land on different game threads, so
    // two threads finding g_keyLockReady false at once could both call
    // InitializeCriticalSection on the same object -- undefined behaviour,
    // not just a missed lock. Calling this once from Startup(), which the
    // SDK guarantees happens before any callback fires, avoids the race
    // instead of narrowing it.
    if (!g_keyLockReady)
    {
        InitializeCriticalSection(&g_keyLock);
        g_keyLockReady = true;
    }
    // Restarted too if the plugin is started again after Shutdown().
    if (g_thread)
        return;
    g_stop.store(false);
    if (!g_wake)
        g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (g_wake)
        g_thread = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
        MakeThreadEfficient(g_thread);
    if (!g_thread)
        Log::Write("VehicleKey: REST worker not started -- car keys use the entry name");
}

void VehicleKey::Shutdown()
{
    if (!g_thread) return;
    g_stop.store(true);
    if (HINTERNET s = g_session.exchange(nullptr))
        WinHttpCloseHandle(s);   // aborts a request in flight
    SetEvent(g_wake);
    if (WaitForSingleObject(g_thread, 5000) != WAIT_OBJECT_0)
        Log::Write("VehicleKey: REST worker did not stop in time");
    CloseHandle(g_thread);
    g_thread = nullptr;
}

void VehicleKey::SetFromScoring(const char* vehicleClass, const char* vehicleName)
{
    if (!g_keyLockReady) return;
    const char* cls = vehicleClass ? vehicleClass : "";
    const char* entry = vehicleName ? vehicleName : "";

    if (!s_haveRaw || strncmp(cls, s_rawClass, sizeof(s_rawClass) - 1) != 0
        || strncmp(entry, s_rawEntry, sizeof(s_rawEntry) - 1) != 0)
    {
        strncpy_s(s_rawClass, cls, _TRUNCATE);
        strncpy_s(s_rawEntry, entry, _TRUNCATE);
        s_haveRaw = true;
        ++s_gen;
        s_pending = false;
        const char* clsKey = s_rawClass[0] ? s_rawClass : Fallback();
        auto it = s_modelByEntry.find(s_rawEntry);
        if (it != s_modelByEntry.end())
            Publish(it->second.empty() ? Fallback() : it->second.c_str(), clsKey,
                    it->second.empty() ? "entry name (REST had no match earlier)" : "vehicle folder (cached)");
        else if (!s_rawEntry[0] || !g_thread)
            Publish(Fallback(), clsKey, "entry name (no REST lookup)");
        else
        {
            // Keep the previous key until the answer: the stores then
            // rekey once, straight to the right file.
            EnterCriticalSection(&g_keyLock);
            g_reqGen = s_gen;
            strcpy_s(g_reqEntry, s_rawEntry);
            g_reqWaiting = true;
            g_resReady = false;
            LeaveCriticalSection(&g_keyLock);
            SetEvent(g_wake);
            s_pending = true;
            s_deadline = GetTickCount64() + kAnswerWaitMs;
        }
    }

    if (!s_pending) return;
    bool ready = false, cacheable = false;
    std::string model;
    char why[96] = "";
    EnterCriticalSection(&g_keyLock);
    if (g_resReady && g_resGen == s_gen)
    {
        ready = true;
        cacheable = g_resCacheable;
        model = g_resModel;
        strcpy_s(why, g_resWhy);
        g_resReady = false;
    }
    LeaveCriticalSection(&g_keyLock);
    const char* clsKey = s_rawClass[0] ? s_rawClass : Fallback();
    if (ready)
    {
        s_pending = false;
        if (cacheable) s_modelByEntry[s_rawEntry] = model;
        char note[160];
        sprintf_s(note, "%s%s", model.empty() ? "entry name fallback: " : "", why);
        Publish(model.empty() ? Fallback() : model.c_str(), clsKey, note);
    }
    else if (GetTickCount64() > s_deadline)
    {
        // No answer in time: the entry name, and a fresh try next change.
        s_pending = false;
        Publish(Fallback(), clsKey, "entry name fallback: REST did not answer in time");
    }
}

void VehicleKey::Get(char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    if (!g_keyLockReady) { strncpy_s(out, outSize, "unknown", _TRUNCATE); return; }
    EnterCriticalSection(&g_keyLock);
    strncpy_s(out, outSize, g_vehicleKey, _TRUNCATE);
    LeaveCriticalSection(&g_keyLock);
}

void VehicleKey::GetClass(char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    if (!g_keyLockReady) { strncpy_s(out, outSize, "unknown", _TRUNCATE); return; }
    EnterCriticalSection(&g_keyLock);
    strncpy_s(out, outSize, g_classKey, _TRUNCATE);
    LeaveCriticalSection(&g_keyLock);
}
