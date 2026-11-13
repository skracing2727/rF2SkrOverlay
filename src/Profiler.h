#pragma once
#include <Windows.h>

// What the DLL itself costs, split by module.
//
// Each piece of plugin work is wrapped in PROF("Module name"): a scope timer
// that adds its QueryPerformanceCounter ticks to that module's running total.
// The same name used in several places (a module's scoring update, its
// telemetry update and its Draw) lands in one slot, so a module is one entry
// however many places it runs from. Timed scopes must not nest, or the inner
// time would be counted twice -- wrap leaf calls, not callers of them.
//
// Totals are lock-free atomics, safe from the game's callback threads and the
// render thread alike. Profiler::Draw (render thread) snapshots them once a
// second and shows the last 10 s: the DLL's total CPU time per second, how
// much of that lands on the render thread per frame, and a bar split by
// module with only the big shares labelled.
class Renderer;

namespace Profiler
{
    // Returns the slot for a module name, registering it on first use. Call
    // once per call site (PROF does it through a function-local static).
    int Slot(const char* name);

    // Render-thread frames, for the per-frame figure.
    void FrameTick();

    void Draw(Renderer& renderer, int screenWidth, int screenHeight);

    struct Scope
    {
        explicit Scope(int slot);
        ~Scope();
        // Ends the timing early (for a span that doesn't match a C++ scope).
        void Stop();
        int m_slot;
        LARGE_INTEGER m_start;
    };
}

#define PROF_CONCAT2(a, b) a##b
#define PROF_CONCAT(a, b) PROF_CONCAT2(a, b)
// Times the rest of the enclosing scope against the named module.
#define PROF(name) \
    static const int PROF_CONCAT(prof_slot_, __LINE__) = Profiler::Slot(name); \
    Profiler::Scope PROF_CONCAT(prof_scope_, __LINE__)(PROF_CONCAT(prof_slot_, __LINE__))
