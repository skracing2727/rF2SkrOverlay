#include "InputLagEstimate.h"
#include "InternalsPlugin.hpp"
#include <cmath>

namespace InputLagEstimate
{
    namespace
    {
        // Simple EMA of the physics tick period -- smooths out the odd
        // single-tick hitch without lagging behind a genuine change (e.g.
        // a physics-rate setting) for long.
        double s_avgDeltaTimeMs = -1.0;
        const double kEmaAlpha = 0.05;
    }

    void OnTelemetry(const ::TelemInfoV01& info)
    {
        const double dtMs = info.mDeltaTime * 1000.0;
        // Guard against implausible values (a session-load transient, or a
        // paused/replay tick with a huge or zero delta) -- those would skew
        // the average far more than any real physics-rate change could.
        if (!std::isfinite(dtMs) || dtMs <= 0.0 || dtMs > 100.0)
            return;

        if (s_avgDeltaTimeMs < 0.0)
            s_avgDeltaTimeMs = dtMs;
        else
            s_avgDeltaTimeMs = s_avgDeltaTimeMs + kEmaAlpha * (dtMs - s_avgDeltaTimeMs);
    }

    bool GetEstimateMs(double displayLatencyMs, double& outMs)
    {
        if (s_avgDeltaTimeMs < 0.0)
            return false;
        outMs = s_avgDeltaTimeMs * 0.5 + displayLatencyMs;
        return true;
    }
}
