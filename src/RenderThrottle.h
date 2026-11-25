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
//
// Staggered (2026-11-24): each instance starts its schedule at a different
// fraction of its interval (golden-ratio steps), so throttles on the same
// rate do not all rebuild on the same frame.
//
// ReplayCount(): replays so far, all throttles together -- the box-stats
// labels compare it around a panel's draw to tell a rebuild from a replay.
inline unsigned& RenderThrottleReplayCount() { static unsigned s_count = 0; return s_count; }

class RenderThrottle
{
public:
    explicit RenderThrottle(double intervalSeconds) : m_intervalSeconds(intervalSeconds) {}
    // The next TryReplay rebuilds regardless of the schedule (e.g. after a
    // click that changes what the box shows).
    void ForceNext() { m_force = true; }

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
        // A cache from before a resize or font change is stale: redraw now
        // instead of replaying it (review F374/F375).
        if (!renderer.CacheCurrent(m_cache))
        {
            IsDue();   // keep the schedule ticking
            return false;
        }
        if (m_force) { m_force = false; IsDue(); return false; }
        if (IsDue())
            return false;
        renderer.Replay(m_cache);
        ++RenderThrottleReplayCount();
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
            // First call: due now, but the next deadline is moved by this
            // instance's own fraction of the interval (stagger).
            static double s_phase = 0.0;
            s_phase += 0.6180339887;
            s_phase -= static_cast<double>(static_cast<int>(s_phase));
            m_lastUpdate.QuadPart = now.QuadPart - static_cast<LONGLONG>(s_phase * m_intervalSeconds * static_cast<double>(s_freq.QuadPart));
            return true;
        }
        const double elapsed = static_cast<double>(now.QuadPart - m_lastUpdate.QuadPart)
                             / static_cast<double>(s_freq.QuadPart);
        // Review F153: accept a frame up to 2 ms early, and advance the
        // deadline by the interval rather than resetting it to now, so frame
        // jitter doesn't alternate N and N+1 frames. Resync if far behind.
        const double kSlack = 0.002;
        if (elapsed + kSlack < m_intervalSeconds)
            return false;
        const LONGLONG step = static_cast<LONGLONG>(m_intervalSeconds * static_cast<double>(s_freq.QuadPart));
        if (elapsed >= 2.0 * m_intervalSeconds || step <= 0)
            m_lastUpdate = now;
        else
            m_lastUpdate.QuadPart += step;
        return true;
    }

    double m_intervalSeconds;
    LARGE_INTEGER m_lastUpdate = {};
    bool m_force = false;
    Renderer::Cache m_cache;
};
