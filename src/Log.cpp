#include "Log.h"
#include "RealtimeState.h"
#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>

namespace
{
    char g_logPath[MAX_PATH] = {};
    CRITICAL_SECTION g_lock;
    bool g_ready = false;
    std::string g_buffer;
    // Cap against unbounded growth across an extremely long continuous
    // realtime stint -- drops the OLDEST buffered text rather than refusing
    // new lines, since the newest diagnostic context is the more useful half
    // to keep. 256 KB used to hold only ~7 minutes with the old BalanceLog and
    // LogRaceGaps on (~37 KB/min), losing most of a race; 64 MB is ~29 hours
    // at that rate, so in practice nothing is ever dropped.
    const size_t kMaxBufferBytes = 64 * 1024 * 1024;
    // When the cap is hit, trim down to this much in one go rather than a
    // line at a time -- erasing the front of a 64 MB string on every Write()
    // would move the whole buffer each line.
    const size_t kTrimToBytes = kMaxBufferBytes / 4 * 3;
}

void Log::Init()
{
    if (g_ready)
        return;

    InitializeCriticalSection(&g_lock);

    // Log beside the game exe (Bin64), which is always writable enough for a text file.
    GetModuleFileNameA(nullptr, g_logPath, MAX_PATH);
    char* lastSlash = strrchr(g_logPath, '\\');
    if (lastSlash)
        *(lastSlash + 1) = '\0';
    strcat_s(g_logPath, MAX_PATH, "rF2SkrOverlayPublic.log");

    // Keep ONE previous generation before truncating. The log is otherwise
    // wiped on every game start, which loses exactly the run you wanted to
    // read whenever a problem only shows up once and you restart to try
    // again -- the diagnosis is gone before it can be looked at. MoveFileEx
    // with REPLACE_EXISTING overwrites any older .prev; failure is ignored,
    // since a missing backup must never stop the real log opening.
    char prevPath[MAX_PATH];
    strcpy_s(prevPath, MAX_PATH, g_logPath);
    char* dot = strrchr(prevPath, '.');
    if (dot)
        *dot = '\0';
    strcat_s(prevPath, MAX_PATH, ".prev.log");
    MoveFileExA(g_logPath, prevPath, MOVEFILE_REPLACE_EXISTING);

    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "w") == 0 && f)
    {
        fputs("=== rF2SkrOverlayPublic log ===\n", f);
        fclose(f);
    }

    g_ready = true;
}

void Log::Write(const char* fmt, ...)
{
    if (!g_ready)
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[512];
    int prefixLen = sprintf_s(line, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (prefixLen < 0) prefixLen = 0;

    va_list args;
    va_start(args, fmt);
    // _TRUNCATE, not plain vsprintf_s -- a formatted line longer than this
    // buffer must NOT reach the CRT's default invalid-parameter handler,
    // which __fastfails the whole process rather than just this one line
    // (confirmed the hard way in an earlier build). A clipped log line is a
    // fine trade for that.
    _vsnprintf_s(line + prefixLen, sizeof(line) - static_cast<size_t>(prefixLen), _TRUNCATE, fmt, args);
    va_end(args);

    EnterCriticalSection(&g_lock);
    g_buffer += line;
    g_buffer += '\n';
    if (g_buffer.size() > kMaxBufferBytes)
    {
        // Cut at a line boundary so the file never starts mid-line.
        size_t cut = g_buffer.find('\n', g_buffer.size() - kTrimToBytes);
        cut = (cut == std::string::npos) ? g_buffer.size() - kTrimToBytes : cut + 1;
        g_buffer.erase(0, cut);
    }
    LeaveCriticalSection(&g_lock);

    // Driving: buffer only, no disk touch -- see FlushBuffered(). Anywhere
    // else (menu, garage, session monitor) there's no frame-time budget to
    // protect, so this keeps the exact same immediacy Write() always had.
    if (!g_inRealtime)
        FlushBuffered();
}

void Log::FlushBuffered()
{
    if (!g_ready)
        return;

    EnterCriticalSection(&g_lock);
    std::string toWrite;
    toWrite.swap(g_buffer);
    LeaveCriticalSection(&g_lock);

    if (toWrite.empty())
        return;

    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "a") == 0 && f)
    {
        fwrite(toWrite.data(), 1, toWrite.size(), f);
        fclose(f);
    }
}
