#pragma once
#include <windows.h>

// Efficiency mode (EcoQoS) for the plugin's own background threads (user,
// 2026-11-24): Windows 11 then prefers E-cores for them on hybrid CPUs and
// runs them at lower clocks elsewhere, leaving the P-cores to rF2. None of
// these threads is time-critical (button presses, saving, lookups, polling).
// Not used for the sound threads (brake beep timing) and impossible for the
// render thread and the game's callback threads, which belong to rF2.
// SetThreadInformation is looked up at run time: absent before Windows 8,
// and the throttling class is ignored before Windows 10 1709.
inline void MakeThreadEfficient(HANDLE thread)
{
    if (!thread) return;
    typedef BOOL (WINAPI *SetThreadInformationFn)(HANDLE, int, LPVOID, DWORD);
    static SetThreadInformationFn fn = reinterpret_cast<SetThreadInformationFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadInformation"));
    if (!fn) return;
    struct { ULONG Version, ControlMask, StateMask; } state = { 1, 0x1, 0x1 };   // THREAD_POWER_THROTTLING_EXECUTION_SPEED on
    fn(thread, 3 /* ThreadPowerThrottling */, &state, sizeof(state));
}