#pragma once

class Renderer;

// Tyre-wear target panel, the tyre counterpart of FuelSave: a target maximum
// wear (e.g. 70%) to reach no sooner than a target time on the current set
// (e.g. 100 minutes). At every lap start each tyre's required average wear
// per lap is re-worked from its wear so far and the laps left in that time.
// One small box per corner shows last lap's wear (green = under that lap's
// requirement, red = over) and the current requirement; the box itself is
// tinted live through the lap by wear so far against the requirement's share
// for the distance covered, using a learned per-tyre wear profile.
// The set's clock restarts whenever fresh tyres are detected.
namespace TyreSave
{
    bool Draw(Renderer& renderer, int screenWidth, int screenHeight);
}
