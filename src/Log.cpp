#include "Log.h"
#include "Persist.h"
#include "RealtimeState.h"
#include <Windows.h>
#include <cstdio>
#include <share.h>
#include <cstdarg>
#include <cstring>
#include <iterator>
#include <memory>
#include <vector>

namespace
{
    char g_logPath[MAX_PATH] = {};
    CRITICAL_SECTION g_lock;
    // Serialises the file writers (Persist thread, render-hook crash handler)
    // so their chunks never interleave and a requeue keeps the order.
    CRITICAL_SECTION g_writeLock;
    bool g_ready = false;

    // The buffer is a list of fixed-size chunks rather than one growing
    // string: a string has to be copied whole every time it outgrows its
    // allocation, and at tens of MB that copy -- under the lock, on whatever
    // thread happened to log -- was a frame-sized stall. Appending to a chunk
    // is one short copy; a full chunk just starts a new one.
    //
    // A line never straddles two chunks, so dropping the oldest chunk (below)
    // always leaves the buffer starting on a whole line.
    const size_t kChunkBytes = 256 * 1024;
    struct Chunk
    {
        size_t used = 0;
        char data[kChunkBytes];
    };
    std::vector<std::unique_ptr<Chunk>> g_chunks;

    // Cap against unbounded growth across an extremely long continuous
    // realtime stint -- drops the OLDEST buffered text rather than refusing
    // new lines, since the newest diagnostic context is the more useful half
    // to keep. 256 KB used to hold only ~7 minutes with the old BalanceLog and
    // LogRaceGaps on (~37 KB/min), losing most of a race; 64 MB is ~29 hours
    // at that rate, so in practice nothing is ever dropped.
    const size_t kMaxChunks = 64 * 1024 * 1024 / kChunkBytes;

    // Caller holds g_lock. 'len' is at most one formatted line (~512 bytes).
    void Append(const char* text, size_t len)
    {
        if (g_chunks.empty() || g_chunks.back()->used + len > kChunkBytes)
        {
            if (g_chunks.size() >= kMaxChunks)
                g_chunks.erase(g_chunks.begin());   // a pointer move per chunk, not a text copy
            g_chunks.push_back(std::unique_ptr<Chunk>(new Chunk));
        }
        Chunk& c = *g_chunks.back();
        memcpy(c.data + c.used, text, len);
        c.used += len;
    }

    // Shared open (_SH_DENYNO), not fopen_s's exclusive one: a log viewer
    // tailing the file made every flush fail and the buffered text was
    // thrown away (review F280). False if the file could not be opened.
    bool WriteChunks(const std::vector<std::unique_ptr<Chunk>>& chunks)
    {
        if (chunks.empty())
            return true;
        FILE* f = _fsopen(g_logPath, "a", _SH_DENYNO);
        if (!f)
            return false;
        for (const std::unique_ptr<Chunk>& c : chunks)
            fwrite(c->data, 1, c->used, f);
        fclose(f);
        return true;
    }
}

void Log::Init()
{
    if (g_ready)
        return;

    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_writeLock);

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
    // fine trade for that. One byte is kept back for the newline.
    _vsnprintf_s(line + prefixLen, sizeof(line) - 1 - static_cast<size_t>(prefixLen), _TRUNCATE, fmt, args);
    va_end(args);
    size_t len = strlen(line);
    line[len++] = '\n';

    EnterCriticalSection(&g_lock);
    Append(line, len);
    LeaveCriticalSection(&g_lock);

    // Never any disk access here (the disk rule, Persist.cpp). Out of the
    // car, ask the Persist thread for a pass so the line still reaches the
    // file within about a second; while driving it waits for the car to stop.
    if (!g_inRealtime)
        Persist::RequestAll();
}

void Log::FlushBuffered()
{
    if (!g_ready)
        return;

    // Swap the chunk list out under the lock (a few pointers), write with
    // the lock released, so a thread logging meanwhile never waits for the
    // disk. Only the Persist thread and the render hook's crash handler call
    // this; g_writeLock keeps the two from writing at once.
    EnterCriticalSection(&g_writeLock);
    std::vector<std::unique_ptr<Chunk>> toWrite;
    EnterCriticalSection(&g_lock);
    toWrite.swap(g_chunks);
    LeaveCriticalSection(&g_lock);

    if (!WriteChunks(toWrite))
    {
        // Could not open the file: put the text back in front of anything
        // logged meanwhile, so the next pass tries again (oldest dropped
        // first if that goes over the cap).
        EnterCriticalSection(&g_lock);
        g_chunks.insert(g_chunks.begin(), std::make_move_iterator(toWrite.begin()),
                        std::make_move_iterator(toWrite.end()));
        if (g_chunks.size() > kMaxChunks)
            g_chunks.erase(g_chunks.begin(), g_chunks.begin() + (g_chunks.size() - kMaxChunks));
        LeaveCriticalSection(&g_lock);
    }
    LeaveCriticalSection(&g_writeLock);
}
