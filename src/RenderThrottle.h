#pragma once
#include <Windows.h>
#include "Renderer.h"

// Shared by widgets throttled below full frame rate: most on-screen numbers
// (gear/speed text, delta rows, gaps to other cars, fuel/tyre wear, the
// clock) don't need to be recomputed at native frame rate to look smooth --
// nobody can read text changing 139 times a second -- but the renderer is
// immediate-mode, so something still has to be submitted every frame. This
// lets a widget regenerate its geometry only every N seconds and replay the
// last real frame's output on the frames in between, skipping the
// sprintf/MeasureText/tessellation work on those skipped frames.
//
// Radar, the side proximity bars and the pedal trace are deliberately NOT
// throttled -- they track fast-changing positions/timing where a lower rate
// would be visible.
//
// One instance per widget, held as a function-local static in its Draw().
class RenderThrottle
{
public:
    explicit RenderThrottle(double intervalSeconds) : m_intervalSeconds(intervalSeconds) {}

    // Call first, before anything else in Draw(). Returns true if this frame
    // is not due for a real update -- the last captured geometry has already
    // been replayed into 'renderer' and the caller should return immediately
    // without doing any of its own work. Returns false if it's due: proceed
    // with normal draw logic, and construct a Capture guard right after so
    // whatever gets added is captured regardless of which of Draw()'s return
    // points ends up being taken (same idea as this codebase's existing
    // TextBgGuard pattern for multi-return functions).
    bool TryReplay(Renderer& renderer)
    {
        if (IsDue())
            return false;
        renderer.Replay(m_cache);
        return true;
    }

    class Capture
    {
    public:
        Capture(RenderThrottle& owner, Renderer& renderer)
            : m_owner(owner), m_renderer(renderer), m_mark(renderer.BeginCapture()) {}
        ~Capture() { m_renderer.EndCapture(m_mark, m_owner.m_cache); }
    private:
        RenderThrottle& m_owner;
        Renderer& m_renderer;
        Renderer::CaptureMark m_mark;
    };

private:
    bool IsDue()
    {
        static LARGE_INTEGER s_freq = {};
        if (s_freq.QuadPart == 0)
            QueryPerformanceFrequency(&s_freq);

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        if (s_freq.QuadPart == 0 || m_lastUpdate.QuadPart == 0)
        {
            m_lastUpdate = now;
            return true;
        }
        const double elapsed = static_cast<double>(now.QuadPart - m_lastUpdate.QuadPart)
                             / static_cast<double>(s_freq.QuadPart);
        if (elapsed < m_intervalSeconds)
            return false;
        m_lastUpdate = now;
        return true;
    }

    double m_intervalSeconds;
    LARGE_INTEGER m_lastUpdate = {};
    Renderer::Cache m_cache;
};
