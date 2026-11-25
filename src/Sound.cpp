#include "Sound.h"
#include "Log.h"
#include <Windows.h>
#include <mmsystem.h>
#include <cstring>
#pragma comment(lib, "winmm.lib")

namespace
{
    // Everything below is guarded by g_lock. It is only ever held for a few
    // assignments (and once, for CreateThread), so a caller can never be
    // made to wait on the sound thread's PlaySound call.
    SRWLOCK g_lock = SRWLOCK_INIT;
    HANDLE g_thread = nullptr;
    HANDLE g_wake = nullptr;           // auto-reset: a request (or Shutdown) is waiting
    const void* g_request = nullptr;   // newest cue not yet handed to PlaySound
    bool g_stop = false;               // Shutdown: the thread should exit
    bool g_shutDown = false;           // no new thread after Shutdown
    bool g_startFailed = false;        // could not create the thread: play directly instead

    const int kSilenceRate = 44100;
    const int kSilenceSamples = kSilenceRate / 100;    // 10 ms
    unsigned char g_silence[44 + kSilenceSamples * 2];

    // Played once, synchronously, when the thread starts: the first PlaySound
    // of the process is the slow one (it loads and opens the audio stack), so
    // it is spent here on something nobody can hear.
    void WarmUp()
    {
        unsigned char* p = g_silence;
        auto writeU32 = [&](DWORD v) { memcpy(p, &v, 4); p += 4; };
        auto writeU16 = [&](WORD v) { memcpy(p, &v, 2); p += 2; };
        const DWORD dataBytes = kSilenceSamples * 2;
        memcpy(p, "RIFF", 4); p += 4;
        writeU32(36 + dataBytes);
        memcpy(p, "WAVE", 4); p += 4;
        memcpy(p, "fmt ", 4); p += 4;
        writeU32(16);
        writeU16(1);                   // PCM
        writeU16(1);                   // mono
        writeU32(kSilenceRate);
        writeU32(kSilenceRate * 2);
        writeU16(2);
        writeU16(16);
        memcpy(p, "data", 4); p += 4;
        writeU32(dataBytes);
        memset(p, 0, dataBytes);
        PlaySoundA(reinterpret_cast<LPCSTR>(g_silence), nullptr, SND_MEMORY | SND_SYNC | SND_NODEFAULT);
    }

    // An exception must not leave a thread: std::terminate would take the
    // game down (review F271). The real body is ThreadProcBody below.
    DWORD ThreadProcBody(LPVOID param);
    DWORD WINAPI ThreadProc(LPVOID param)
    {
        try { return ThreadProcBody(param); }
        catch (...) { return 0; }
    }

    DWORD ThreadProcBody(LPVOID)
    {
        // A beep is only useful on time (the brake cue especially), and this
        // thread sleeps between them, so it may as well be first in line
        // when it does wake.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        WarmUp();
        for (;;)
        {
            WaitForSingleObject(g_wake, INFINITE);
            AcquireSRWLockExclusive(&g_lock);
            const void* wav = g_request;
            g_request = nullptr;
            const bool stop = g_stop;
            ReleaseSRWLockExclusive(&g_lock);
            if (stop)
                break;
            // SND_ASYNC exactly as before: a new cue cuts off one still
            // playing rather than queueing behind it.
            if (wav)
                PlaySoundA(static_cast<LPCSTR>(wav), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        }
        // Nothing may still be reading a cue buffer once the plugin is gone.
        PlaySoundA(nullptr, nullptr, 0);
        return 0;
    }

    // Caller holds g_lock exclusively.
    void StartLocked()
    {
        if (g_thread || g_shutDown || g_startFailed)
            return;
        if (!g_wake)
            g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_wake)
            g_thread = CreateThread(nullptr, 0, ThreadProc, nullptr, 0, nullptr);
        if (!g_thread)
        {
            g_startFailed = true;
            Log::Write("Sound: could not start the sound thread (%lu) -- cues play directly", GetLastError());
        }
    }
}

void Sound::Prepare()
{
    AcquireSRWLockExclusive(&g_lock);
    StartLocked();
    ReleaseSRWLockExclusive(&g_lock);
}

void Sound::PlayMemory(const void* wav)
{
    if (!wav)
        return;
    AcquireSRWLockExclusive(&g_lock);
    StartLocked();
    const bool queued = g_thread != nullptr && !g_stop;
    const bool direct = !queued && g_startFailed && !g_shutDown;
    if (queued)
        g_request = wav;   // latest wins: an unplayed older cue is simply replaced
    HANDLE wake = g_wake;
    ReleaseSRWLockExclusive(&g_lock);

    if (queued)
        SetEvent(wake);
    else if (direct)
        PlaySoundA(static_cast<LPCSTR>(wav), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void Sound::Shutdown()
{
    AcquireSRWLockExclusive(&g_lock);
    g_shutDown = true;
    g_stop = true;
    g_request = nullptr;
    HANDLE thread = g_thread;
    g_thread = nullptr;
    HANDLE wake = g_wake;
    ReleaseSRWLockExclusive(&g_lock);

    if (!thread)
    {
        if (g_startFailed)
            PlaySoundA(nullptr, nullptr, 0);
        return;
    }
    SetEvent(wake);
    // Stop any cue now, so a thread inside an async PlaySound returns sooner.
    PlaySoundA(nullptr, nullptr, 0);
    // Normally immediate. Bounded so a hung audio service cannot hang the
    // game's exit; the handle is closed either way (that does not stop it).
    if (WaitForSingleObject(thread, 2000) != WAIT_OBJECT_0)
    {
        // The thread may still run our code: pin the DLL so unloading it
        // cannot pull that code out from under it (review F417).
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&ThreadProc), &self);
        Log::Write("Sound: sound thread still busy at shutdown -- DLL pinned");
    }
    CloseHandle(thread);
    // g_wake is left open on purpose: a PlayMemory that raced this Shutdown
    // may still be about to signal it.
}
