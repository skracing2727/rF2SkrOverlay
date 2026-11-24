#pragma once
#include <Windows.h>

// Per-corner tyre temperatures, for judging camber: for the last completed
// corner, the peak SURFACE temperature and the inner-layer ("subsurface")
// temperature at exit, at three points across each of the four tyres.
//
// Corners are found from lateral g alone (no per-track data), and the loaded
// side is taken from the tyre loads rather than from any sign convention.
// Fed from the telemetry callback (player car only).
class TyreTempStore
{
public:
    void Init();
    void OnTelemetry(const struct TelemInfoV01& info);
    void Clear();

    // Position across the tread, after mapping from the SDK's left/centre/right.
    enum Position { Outside = 0, Middle = 1, Inside = 2 };

    struct Wheel
    {
        float surfacePeak[3] = {}; // Celsius, indexed by Position
        float inner[3] = {};       // Celsius, indexed by Position
    };
    struct State
    {
        bool valid = false;
        int cornerCount = 0;
        bool loadedLeft = false;   // the LEFT-side wheels carried the load (a right-hand corner)
        float durationSeconds = 0.0f;
        Wheel wheel[4];            // FL, FR, RL, RR
    };
    State Read();

private:
    enum class Phase { Idle, Pending, InCorner, Tail };

    void ResetAccum();
    void Accumulate(const struct TelemInfoV01& info);
    void Finalize(double drivenSeconds);

    CRITICAL_SECTION m_lock;
    bool m_ready = false;
    State m_state;
    int m_cornerCounter = 0;

    Phase m_phase = Phase::Idle;
    double m_lastET = -1.0;
    double m_pendingStartET = 0.0;
    double m_belowExitStartET = -1.0;
    double m_tailStartET = 0.0;
    double m_lastLogET = -1.0;   // session time of the last corner logged (see Finalize)

    // In the SDK's own left/centre/right order; mapped only when published.
    float m_peak[4][3] = {};
    float m_lastInner[4][3] = {};
    double m_loadSum[4] = {};
    double m_camberSum[4] = {};
    int m_samples = 0;
};

extern TyreTempStore g_tyreTemps;

class Renderer;

namespace TyreTemps
{
    // Returns false if nothing was drawn.
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight);
}
