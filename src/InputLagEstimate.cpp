#include "InputLagEstimate.h"
#include "InternalsPlugin.hpp"
#include "PerfStats.h"
#include "FrameRate.h"
#include <cmath>

namespace InputLagEstimate
{
    namespace
    {
        // mDeltaTime is the telemetry callback interval (~10 ms), not the
        // physics step, so it no longer feeds the estimate (review F180);
        // it only tells that telemetry is flowing.
        bool s_haveTelemetry = false;
        // rF2 runs its physics at a fixed 400 Hz.
        const double kHalfPhysicsStepMs = 0.5 * 1000.0 / 400.0;
    }

    void OnTelemetry(const ::TelemInfoV01& info)
    {
        const double dtMs = info.mDeltaTime * 1000.0;
        if (std::isfinite(dtMs) && dtMs > 0.0 && dtMs <= 100.0)
            s_haveTelemetry = true;
    }

    bool GetEstimateMs(double displayLatencyMs, double& outMs)
    {
        if (!s_haveTelemetry)
            return false;
        // Average wait for the next physics step, plus the render thread's
        // own CPU work on the frame, plus on average half a frame before
        // the frame that shows the input starts, plus Present-to-display.
        double est = kHalfPhysicsStepMs + displayLatencyMs;
        double cpuMs = 0.0;
        if (PerfStats::GetCpuFrameTimeMs(cpuMs) && cpuMs > 0.0)
            est += cpuMs;
        double fps = 0.0;
        if (FrameRate::GetFps(fps) && fps > 1.0)
            est += 0.5 * 1000.0 / fps;
        outMs = est;
        return true;
    }
}
