#pragma once

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
    void Reset();
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight);
}
