#pragma once

// Top-left "data" HUD panel: fuel time-left plus per-tyre degradation, per
// the layout convention that top-right is timing metrics (RelativeTiming,
// LapDelta) and top-left is car data (this, FuelStore.cpp, TyreWear.cpp).
class Renderer;

namespace FuelTyrePanel
{
    // Draws into the current frame. Returns false if there is nothing to show.
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight);

    // Screen Y of the panel's own bottom edge as it was LAST DRAWN, padding
    // included -- so anything stacked underneath follows the panel's real
    // height instead of guessing at a fixed fraction. The panel's height
    // varies with how many rows apply (race vs practice, tyre data available
    // or not), which is exactly why a fixed fraction gets overlapped.
    // Falls back to the panel's top anchor when it has not drawn this
    // session, so a caller never lands off the top of the screen.
    float BottomEdgeY(int screenWidth, int screenHeight);
}
