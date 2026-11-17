#pragma once

class Renderer;

// Small FFB strength box: the force rF2 is sending the wheel right now (as a
// % of full scale, peak-held briefly so it is readable), this lap's maximum
// so far, and the previous lap's maximum plus how much of that lap was at
// 100% (clipping). Fed from the ForceFeedback() hook's -1..+1 value.
namespace FfbLevel
{
    void OnForce(double value);   // FFB thread, ~400 Hz
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight);
}
