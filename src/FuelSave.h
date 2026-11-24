#pragma once

class Renderer;

// Fuel-saving delta bar for long races run in fixed-length stints (e.g. a
// 24h race on 50-minute stints). The target litres/lap is whatever makes the
// current tank last until the stint's target length, re-worked out at every
// lap start from the fuel in the tank, the stint time left and the last lap
// time. The bar shows, live through the lap, fuel used so far minus the
// target's share for the distance covered (green = under, red = over); the
// line beneath it gives the target, last lap's usage and the stint length
// the recent laps' average is heading for.
namespace FuelSave
{
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight);

    // Save thread (Persist.cpp), any time: writes the fuel-use profile if a
    // lap was learned since the last call, and reads a new car+track's
    // profile that Draw asked for (Draw picks it up on a later frame).
    void FlushIfDirty();
}
