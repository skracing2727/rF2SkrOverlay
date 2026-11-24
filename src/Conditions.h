#pragma once

#include <vector>

struct ScoringInfoV01;
struct TelemInfoV01;
class Renderer;

// Track conditions box for mixed-weather races: track / air temperature,
// rain now, racing-line wetness (driest / average / wettest) and which way
// each is heading, plus a slicks / wets call. The crossover is set by
// [Conditions] WetsAtWetness (default 20% average racing-line wetness).
// Everything comes from ScoringInfoV01 (mTrackTemp, mAmbientTemp, mRaining,
// m*PathWetness); no forecast (that is only on rF2's REST API).
namespace Conditions
{
    void OnScoring(const ScoringInfoV01& info);   // scoring thread, ~5 Hz
    // Player telemetry: counts how many wheel samples are on a surface rF2
    // reports as wet (TelemWheelV01::mSurfaceType 1) vs dry (0), so each
    // stretch between scoring ticks can be marked wet or dry.
    void OnTelemetry(const TelemInfoV01& info);
    // Wet sections as last driven, for the track map: one entry per binM
    // metres of lap distance, -1 = not driven yet, 0 = dry, 1 = wet.
    // False when there is nothing to show.
    bool WetBins(std::vector<signed char>& bins, float& binM);
    void Reset();
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight);
}
