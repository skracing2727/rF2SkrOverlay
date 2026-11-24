#pragma once
#include <Windows.h>
#include <atomic>

// Frequency content of the force feedback rF2 sends to the wheel, laid out
// on the same points as Moza Pit House's FFB equalizer (0, 5, 10, 15, 25,
// 30, 40, 50, 60, 80, 100 Hz) so it can be read straight across: a lot of
// force at a point = that is where the EQ would act.
//
// The signal is the value rF2 passes to the plugin's ForceFeedback() hook
// (~400 calls/s, -1..+1). Reading it works; editing it does not -- the game
// discards the plugin's value (see memory/project-rf2-ffb-plugin-edits-
// ignored), so the hook only ever records and returns false. What the wheel
// base then does with the signal (Pit House filters, its own EQ) is not
// visible from here.
class FfbSpectrumStore
{
public:
    void Init();
    // From the ForceFeedback() callback.
    void OnForce(double value);
    void Clear();

    // Copies the newest 'count' samples (oldest first) and their arrival
    // times in seconds. Returns how many were actually available, and the
    // running total of samples ever received.
    int CopyLatest(float* values, double* times, int count, long long& totalOut);
    // Just the running total CopyLatest would report, for deciding whether
    // a copy is worth making yet.
    long long Total();

    // The most recent force value (-1..+1, rF2's own sign), lock-free.
    float Latest() const { return m_latest.load(std::memory_order_relaxed); }

    // From the player's telemetry: lap number (for the lap-average line)
    // and speed (time stopped in the pits is left out of that average).
    void OnTelemetry(long lapNumber, float speedMs)
    {
        m_lap.store(lapNumber, std::memory_order_relaxed);
        m_speed.store(speedMs, std::memory_order_relaxed);
    }
    long Lap() const { return m_lap.load(std::memory_order_relaxed); }
    float SpeedMs() const { return m_speed.load(std::memory_order_relaxed); }

    static const int kRing = 4096; // > 10 s at 400 Hz

private:
    std::atomic<long> m_lap{ -1 };
    std::atomic<float> m_speed{ 0.0f };
    CRITICAL_SECTION m_lock;
    bool m_ready = false;
    float m_values[kRing] = {};
    double m_times[kRing] = {};
    long long m_total = 0;
    bool m_collecting = false; // force callback thread only: false while the graph is hidden
    LARGE_INTEGER m_freq = {};
    std::atomic<float> m_latest{ 0.0f };
};

extern FfbSpectrumStore g_ffbSpectrum;

class Renderer;

namespace FfbGraph
{
    // Returns false if nothing was drawn.
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight);
}
