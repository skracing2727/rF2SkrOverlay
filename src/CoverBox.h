#pragma once

// Plain rounded boxes, placed and sized in layout mode, for blanking out
// parts of the car's own in-car display that compete with this overlay. See
// CoverBoxConfig. Drawn first in the frame so every other panel sits on top.
class Renderer;

namespace CoverBox
{
    void Draw(Renderer& renderer, int screenWidth, int screenHeight);
}
