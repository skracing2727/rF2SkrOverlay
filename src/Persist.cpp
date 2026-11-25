// THE DISK RULE.
//
// The plugin writes to disk at exactly two moments, and always from the one
// thread in this file -- never from the render, telemetry or scoring threads,
// where a file write (open, write, close, sometimes tens of ms behind antivirus
// or a busy drive) shows up as a frame-time spike or a stalled game callback:
//
// 1. The player crosses the line: only learned driving data that changed (best
//    lap, braking points, fuel rate, fuel/tyre profiles, learned pit limit and
//    shift points, recorded track maps). RequestLearned().
// 2. Out of the car (not in realtime): all of the above, plus queued INI
//    writes, the per-car layouts, the per-track pit-lane limits and the
//    log. RequestAll().
//
// Everything else that changes mid-lap (pit entry and exit, leaving layout
// mode, a PB reset) only marks its store dirty and waits for one of those two.
// Only data used between sessions is written at all. The one exception is the
// render hook's crash handler, which writes the log itself so the crash's own
// diagnostics are not lost.
//
// The pass itself lives in Plugin.cpp; this file only decides when it runs.
#include "Persist.h"
#include "EcoThread.h"
#include "Log.h"
#include <Windows.h>
#include <atomic>

namespace
{
    void (*s_pass)(bool all) = nullptr;
    void (*s_loadPass)() = nullptr;
    HANDLE s_thread = nullptr;
    // Created once and never closed: any thread may still call a Request*()
    // during or after Stop(), and signalling a closed (or reused) handle
    // would be worse than keeping two events for the life of the process.
    HANDLE s_wake = nullptr;       // auto-reset: something was requested
    HANDLE s_allDone = nullptr;    // auto-reset: an "all" pass just finished
    std::atomic<DWORD> s_threadId{ 0 };
    std::atomic<bool> s_wantLearned{ false }, s_wantAll{ false }, s_wantLoad{ false };
    std::atomic<bool> s_stop{ false };
    std::atomic<bool> s_inAllPass{ false };
    // FlushAllNow tickets: each call takes the next number; s_flushCovered is
    // the highest ticket that a finished "all" pass is known to have started
    // after. Only this thread writes s_flushCovered.
    std::atomic<long> s_flushIssued{ 0 }, s_flushCovered{ 0 };

    // A short wait before each pass so a burst of requests (the plugin's own
    // line trigger plus the stores that learn something at the same line)
    // lands in one pass rather than several, and a floor between passes: out
    // of the car the scoring callback asks five times a second, and nothing
    // there needs saving more often than about once a second.
    const DWORD kSettleMs = 250;
    const DWORD kMinGapMs = 1000;
    // EndSession / Shutdown wait at most this long for a pass. A whole pass
    // is normally a few ms; this only matters if the disk has gone away.
    const DWORD kFlushTimeoutMs = 10000;
    const DWORD kStopTimeoutMs = 10000;

    bool Urgent()
    {
        return s_stop.load() || s_flushIssued.load() > s_flushCovered.load();
    }

    // Waits up to 'ms', returning early only for FlushAllNow or Stop. An
    // ordinary request arriving meanwhile just joins the coming pass (its
    // flag stays set; only the wake-up is used up here).
    void HoldOff(DWORD ms)
    {
        const ULONGLONG until = GetTickCount64() + ms;
        for (;;)
        {
            if (Urgent())
                return;
            const ULONGLONG now = GetTickCount64();
            if (now >= until)
                return;
            WaitForSingleObject(s_wake, static_cast<DWORD>(until - now));
        }
    }

    bool ThreadAlive()
    {
        return s_thread && WaitForSingleObject(s_thread, 0) == WAIT_TIMEOUT;
    }

    DWORD WINAPI ThreadProc(LPVOID)
    {
        s_threadId.store(GetCurrentThreadId());
        ULONGLONG lastPass = 0;
        for (;;)
        {
            if (!s_wantLearned.load() && !s_wantAll.load() && !s_wantLoad.load() && !Urgent())
                WaitForSingleObject(s_wake, INFINITE);

            if (!Urgent())
            {
                HoldOff(kSettleMs);
                const ULONGLONG since = GetTickCount64() - lastPass;
                if (since < kMinGapMs)
                    HoldOff(static_cast<DWORD>(kMinGapMs - since));
            }

            const bool stopping = s_stop.load();
            // Read BEFORE the pass starts saving anything, so every ticket it
            // claims was issued before any store was looked at.
            const long covers = s_flushIssued.load();
            const bool forced = covers > s_flushCovered.load();
            bool all = s_wantAll.exchange(false) || forced;
            const bool learned = s_wantLearned.exchange(false);
            const bool load = s_wantLoad.exchange(false);
            if (stopping)
                all = true;   // the last pass there will be: catches what was logged after FlushAllNow

            if ((all || learned) && s_pass)
            {
                s_inAllPass.store(all);
                // An exception must not leave the thread (std::terminate
                // would take the game down); skip this pass instead (review F271).
                try { s_pass(all); }
                catch (...) { Log::Write("Persist: the save pass threw an exception -- skipped"); }
                s_inAllPass.store(false);
                lastPass = GetTickCount64();
            }
            else if (load && s_loadPass)
            {
                // Reads only; a full or learned pass above does its loads itself.
                try { s_loadPass(); }
                catch (...) { Log::Write("Persist: the load pass threw an exception -- skipped"); }
            }
            if (all)
            {
                if (covers > s_flushCovered.load())
                    s_flushCovered.store(covers);
                SetEvent(s_allDone);
            }
            if (stopping)
                break;
        }
        return 0;
    }

    void Wake()
    {
        if (s_wake)
            SetEvent(s_wake);
    }
}

void Persist::Start(void (*pass)(bool all), void (*loadPass)())
{
    if (s_thread)
        return;
    s_pass = pass;
    s_loadPass = loadPass;
    if (!s_wake) s_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s_allDone) s_allDone = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s_wake || !s_allDone)
    {
        Log::Write("Persist: could not create events (%lu) -- saving falls back to session end", GetLastError());
        return;
    }
    s_stop.store(false);
    DWORD tid = 0;
    // Normal priority on purpose: the pass briefly takes locks the render
    // thread also takes, and a below-normal thread preempted while holding
    // one would make the frame wait for it.
    s_thread = CreateThread(nullptr, 0, ThreadProc, nullptr, 0, &tid);
    MakeThreadEfficient(s_thread);
    if (s_thread)
        s_threadId.store(tid);
    else
        Log::Write("Persist: could not start the save thread (%lu) -- saving falls back to session end", GetLastError());
}

void Persist::Stop()
{
    if (!s_thread)
        return;
    s_stop.store(true);
    Wake();
    if (WaitForSingleObject(s_thread, kStopTimeoutMs) != WAIT_OBJECT_0)
    {
        // Still inside a pass: leave it be (closing its handle would not stop
        // it), and do not run another one here alongside it.
        Log::Write("Persist: save thread still busy after %lu ms at shutdown", kStopTimeoutMs);
        s_thread = nullptr;
        return;
    }
    CloseHandle(s_thread);
    s_thread = nullptr;
    s_threadId.store(0);
}

void Persist::RequestLearned()
{
    if (!s_wantLearned.exchange(true))
        Wake();
}

void Persist::RequestLoad()
{
    if (!s_wantLoad.exchange(true))
        Wake();
}

void Persist::RequestAll()
{
    // The pass's own log lines (a store saying it saved) would otherwise ask
    // for yet another pass after every pass. Out of the car, an "all" pass
    // writes the log as its very last step, so those lines go out with it.
    if (s_inAllPass.load() && GetCurrentThreadId() == s_threadId.load())
        return;
    if (!s_wantAll.exchange(true))
        Wake();
}

void Persist::FlushAllNow()
{
    if (GetCurrentThreadId() == s_threadId.load())
        return;   // from inside a pass: it would wait for itself

    if (!ThreadAlive())
    {
        // No save thread (never started, stopped, or gone with the process):
        // nothing else can be running a pass, so run it here.
        if (s_pass)
        {
            s_wantAll.store(false);
            s_wantLearned.store(false);
            s_pass(true);
        }
        return;
    }

    const long ticket = s_flushIssued.fetch_add(1) + 1;
    Wake();
    const ULONGLONG deadline = GetTickCount64() + kFlushTimeoutMs;
    HANDLE waitOn[2] = { s_allDone, s_thread };
    while (s_flushCovered.load() < ticket)
    {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
        {
            Log::Write("Persist: FlushAllNow gave up after %lu ms", kFlushTimeoutMs);
            return;
        }
        // Rechecked every 50 ms as well: with two callers waiting, one
        // auto-reset signal only wakes one of them.
        const DWORD left = static_cast<DWORD>(deadline - now);
        const DWORD r = WaitForMultipleObjects(2, waitOn, FALSE, left < 50 ? left : 50);
        if (r == WAIT_OBJECT_0 + 1)
            return;   // the thread is gone
    }
}
