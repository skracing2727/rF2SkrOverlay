#include "RealtimeState.h"
#include <Windows.h>
#include <cstring>

std::atomic<bool> g_inRealtime{ false };
std::atomic<int> g_fuelMult{ -1 };
std::atomic<int> g_headlightsOn{ -1 };
std::atomic<bool> g_playerAiControl{ false };

namespace
{
    // Written on the scoring thread, read on the telemetry and render
    // threads. A short fixed buffer under a critical section rather than a
    // std::string, so a reader can never see a half-reallocated string.
    CRITICAL_SECTION g_keyLock;
    bool g_keyLockReady = false;
    char g_vehicleKey[64] = "unknown";
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
    if (g_keyLockReady)
        return;
    InitializeCriticalSection(&g_keyLock);
    g_keyLockReady = true;
}

void VehicleKey::SetFromScoring(const char* vehicleClass, const char* vehicleName)
{
    if (!g_keyLockReady) return;
    // Class first; fall back to the entry name only when the mod leaves the
    // class blank, so the key is never empty.
    const char* src = (vehicleClass && vehicleClass[0]) ? vehicleClass
                    : ((vehicleName && vehicleName[0]) ? vehicleName : "unknown");
    EnterCriticalSection(&g_keyLock);
    strncpy_s(g_vehicleKey, sizeof(g_vehicleKey), src, _TRUNCATE);
    LeaveCriticalSection(&g_keyLock);
}

void VehicleKey::Get(char* out, size_t outSize)
{
    if (!out || outSize == 0) return;
    if (!g_keyLockReady) { strncpy_s(out, outSize, "unknown", _TRUNCATE); return; }
    EnterCriticalSection(&g_keyLock);
    strncpy_s(out, outSize, g_vehicleKey, _TRUNCATE);
    LeaveCriticalSection(&g_keyLock);
}
