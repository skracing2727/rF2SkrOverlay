#include "UpdateCheck.h"
#include "EcoThread.h"
#include "Config.h"
#include "Log.h"
#include "Version.h"
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#pragma comment(lib, "winhttp.lib")

namespace
{
    HANDLE g_thread = nullptr;
    std::atomic<bool> g_newer{false};
    char g_latest[32] = "";             // written before g_newer is set, then never again

    // The open WinHTTP handles, so Shutdown() can close them -- that aborts a
    // blocking call on the worker thread at once.
    SRWLOCK g_handleLock = SRWLOCK_INIT;
    HINTERNET g_session = nullptr, g_connect = nullptr, g_request = nullptr;
    bool g_stopping = false;

    // "26.11.17.3" -> 4 numbers; false if it is not that shape.
    bool ParseVersion(const char* s, int out[4])
    {
        if (*s == 'v' || *s == 'V') ++s;
        return sscanf_s(s, "%d.%d.%d.%d", &out[0], &out[1], &out[2], &out[3]) == 4;
    }

    bool IsNewer(const int a[4], const int b[4])
    {
        for (int i = 0; i < 4; ++i)
            if (a[i] != b[i]) return a[i] > b[i];
        return false;
    }

    void CloseAll()
    {
        AcquireSRWLockExclusive(&g_handleLock);
        if (g_request) { WinHttpCloseHandle(g_request); g_request = nullptr; }
        if (g_connect) { WinHttpCloseHandle(g_connect); g_connect = nullptr; }
        if (g_session) { WinHttpCloseHandle(g_session); g_session = nullptr; }
        ReleaseSRWLockExclusive(&g_handleLock);
    }

    // Stores a freshly opened handle, unless Shutdown() has begun.
    bool Keep(HINTERNET& slot, HINTERNET h)
    {
        AcquireSRWLockExclusive(&g_handleLock);
        const bool ok = h && !g_stopping;
        if (ok) slot = h;
        ReleaseSRWLockExclusive(&g_handleLock);
        if (!ok && h) WinHttpCloseHandle(h);
        return ok;
    }

    // The request's handle for the next call, or null once Shutdown() has begun
    // (review F346: checked between calls so the worker stops at once). Shutdown
    // may still close it during a call; WinHTTP then fails that call, and the
    // value cannot be reused because only this thread opens handles.
    HINTERNET Request()
    {
        AcquireSRWLockShared(&g_handleLock);
        HINTERNET h = g_stopping ? nullptr : g_request;
        ReleaseSRWLockShared(&g_handleLock);
        return h;
    }

    bool Fetch(std::string& body, DWORD& status)
    {
        wchar_t agent[64];
        swprintf_s(agent, L"rF2SkrOverlayPublic/%hs", RF2RO_VERSION);   // GitHub requires a User-Agent
        if (!Keep(g_session, WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)))
            return false;
        // Short limits: a slow network must not keep the thread (or a game
        // shutdown waiting for it) around.
        AcquireSRWLockShared(&g_handleLock);
        HINTERNET session = g_stopping ? nullptr : g_session;
        ReleaseSRWLockShared(&g_handleLock);
        if (!session) return false;
        WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
        if (!Keep(g_connect, WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0)))
            return false;
        AcquireSRWLockShared(&g_handleLock);
        HINTERNET connect = g_stopping ? nullptr : g_connect;
        ReleaseSRWLockShared(&g_handleLock);
        if (!connect) return false;
        if (!Keep(g_request, WinHttpOpenRequest(connect, L"GET", L"/repos/skracing2727/rF2SkrOverlay/releases/latest",
                                                nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE)))
            return false;
        const wchar_t* headers = L"Accept: application/vnd.github+json\r\n";
        HINTERNET req = Request();
        if (!req || !WinHttpSendRequest(req, headers, static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || !(req = Request()) || !WinHttpReceiveResponse(req, nullptr))
            return false;
        DWORD size = sizeof(status);
        if (!(req = Request()) || !WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
            return false;
        char buf[4096];
        DWORD got = 0;
        // The release JSON is a few KB; 256 KB is a hard stop.
        while (body.size() < 256 * 1024 && (req = Request()) && WinHttpReadData(req, buf, sizeof(buf), &got) && got > 0)
            body.append(buf, got);
        return true;
    }

    DWORD Run()
    {
        std::string body;
        DWORD status = 0;
        const bool ok = Fetch(body, status);
        const DWORD err = ok ? 0 : GetLastError();
        CloseAll();
        if (!ok)
        {
            Log::Write("UpdateCheck: no answer from GitHub (error %lu) -- not checked this time", err);
            return 0;
        }
        if (status != 200)
        {
            Log::Write("UpdateCheck: GitHub answered HTTP %lu -- not checked this time", status);
            return 0;
        }
        // "tag_name": "v26.11.17.3" -- the one field needed, without a JSON parser.
        const size_t at = body.find("\"tag_name\"");
        const size_t q1 = at == std::string::npos ? at : body.find('"', body.find(':', at) + 1);
        const size_t q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 - q1 - 1 >= sizeof(g_latest))
        {
            Log::Write("UpdateCheck: no release tag in GitHub's answer");
            return 0;
        }
        char tag[sizeof(g_latest)];
        memcpy(tag, body.data() + q1 + 1, q2 - q1 - 1);
        tag[q2 - q1 - 1] = '\0';

        int latest[4], mine[4];
        if (!ParseVersion(tag, latest) || !ParseVersion(RF2RO_VERSION, mine))
        {
            Log::Write("UpdateCheck: could not compare release '%s' with this build %s", tag, RF2RO_VERSION);
            return 0;
        }
        if (IsNewer(latest, mine))
        {
            sprintf_s(g_latest, "%d.%02d.%02d.%d", latest[0], latest[1], latest[2], latest[3]);
            g_newer.store(true, std::memory_order_release);
            Log::Write("UpdateCheck: newer release on GitHub: %s (this build %s)", g_latest, RF2RO_VERSION);
        }
        else
        {
            Log::Write("UpdateCheck: up to date (latest release %s, this build %s)", tag, RF2RO_VERSION);
        }
        return 0;
    }

    // Review F305/F346: WinHTTP (proxy detection, DNS) can outlast Shutdown()'s
    // 3 s wait. The thread holds its own reference to this DLL so rF2 cannot
    // unload the code under it; FreeLibraryAndExitThread drops it on the way out.
    // The reference is taken in Start(), before the thread exists.
    // An exception must not leave a thread: std::terminate would take the
    // game down (review F271). The real body is ThreadProcBody below.
    DWORD ThreadProcBody(LPVOID param);
    DWORD WINAPI ThreadProc(LPVOID param)
    {
        try { return ThreadProcBody(param); }
        catch (...) { return 0; }
    }

    DWORD ThreadProcBody(LPVOID param)
    {
        HMODULE self = static_cast<HMODULE>(param);
        const DWORD rc = Run();
        if (self)
            FreeLibraryAndExitThread(self, rc);
        return rc;
    }
}

void UpdateCheck::Start()
{
    if (!g_updateCheckConfig.enabled)
    {
        Log::Write("UpdateCheck: off ([Updates] CheckForUpdates=0)");
        return;
    }
    if (g_thread)
        return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&ThreadProc), &self);
    g_thread = CreateThread(nullptr, 0, ThreadProc, self, 0, nullptr);
    MakeThreadEfficient(g_thread);
    if (!g_thread && self)
        FreeLibrary(self);
    if (g_thread)
        SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);
}

void UpdateCheck::Shutdown()
{
    if (!g_thread)
        return;
    AcquireSRWLockExclusive(&g_handleLock);
    g_stopping = true;
    ReleaseSRWLockExclusive(&g_handleLock);
    CloseAll();     // fails whatever WinHTTP call the thread is in
    if (WaitForSingleObject(g_thread, 3000) != WAIT_OBJECT_0)
        Log::Write("UpdateCheck: thread still running at shutdown");
    CloseHandle(g_thread);
    g_thread = nullptr;
}

bool UpdateCheck::NewerAvailable(char* latest, size_t latestSize)
{
    if (!g_newer.load(std::memory_order_acquire))
        return false;
    strcpy_s(latest, latestSize, g_latest);
    return true;
}
