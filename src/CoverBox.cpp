#include "CoverBox.h"
#include "RenderThrottle.h"
#include "Renderer.h"
#include "Config.h"
#include "Layout.h"
#include <cstdio>

namespace
{
    // INI keys per box. Layout keeps these pointers, so they are static.
    const char* const kXKey[CoverBoxConfig::kMaxBoxes] = { "Box1PosXFraction", "Box2PosXFraction", "Box3PosXFraction", "Box4PosXFraction" };
    const char* const kYKey[CoverBoxConfig::kMaxBoxes] = { "Box1PosYFraction", "Box2PosYFraction", "Box3PosYFraction", "Box4PosYFraction" };
    const char* const kWKey[CoverBoxConfig::kMaxBoxes] = { "Box1WidthPixels", "Box2WidthPixels", "Box3WidthPixels", "Box4WidthPixels" };
    const char* const kHKey[CoverBoxConfig::kMaxBoxes] = { "Box1HeightPixels", "Box2HeightPixels", "Box3HeightPixels", "Box4HeightPixels" };
    const char* const kName[CoverBoxConfig::kMaxBoxes] = { "Cover box 1", "Cover box 2", "Cover box 3", "Cover box 4" };
}

void CoverBox::Draw(Renderer& renderer, int screenWidth, int screenHeight)
{
    CoverBoxConfig& cfg = g_coverBoxConfig;
    // 1 Hz rebuild, replayed in between (2026-11-24 throttle pass): they
    // only change in layout mode, which bypasses the throttle.
    static RenderThrottle s_throttle(1.0);
    if (!Layout::Active() && s_throttle.TryReplay(renderer))
        return;
    RenderThrottle::Capture throttleCapture(s_throttle, renderer);
    // Sizes are kept at 2160p, like every other panel's pixel values.
    const float px = static_cast<float>(screenHeight) / 2160.0f;
    for (int i = 0; i < cfg.count && i < CoverBoxConfig::kMaxBoxes; ++i)
    {
        const float halfW = cfg.widthPx[i] * px * 0.5f;
        const float halfH = cfg.heightPx[i] * px * 0.5f;
        const float centerX = screenWidth * cfg.posXFraction[i] + halfW;
        const float centerY = screenHeight * cfg.posYFraction[i] + halfH;

        Layout::RegisterBox(kName[i], "CoverBox", kXKey[i], kYKey[i],
                            &cfg.posXFraction[i], &cfg.posYFraction[i],
                            centerX, centerY, halfW, halfH,
                            &cfg.widthPx[i], kWKey[i], &cfg.heightPx[i], kHKey[i]);

        renderer.AddFilledRect(centerX, centerY, halfW, halfH, 0.0f,
                               cfg.grey, cfg.grey, cfg.grey, cfg.a);   // square corners
    }
}
