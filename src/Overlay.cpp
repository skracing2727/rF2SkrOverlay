#include "Overlay.h"
#include <Windows.h>
#include <malloc.h>
#include "Renderer.h"
#include "CarState.h"
#include "PedalTrace.h"
#include "Shortcuts.h"
#include "CarLayout.h"
#include "CoverBox.h"
#include "FrameLatency.h"
#include "FrameRate.h"
#include "PerfStats.h"
#include "AmdGpuPower.h"
#include "GpuEngineUsage.h"
#include "GearDisplay.h"
#include "LapDelta.h"
#include "RelativeTiming.h"
#include "FuelTyrePanel.h"
#include "FuelSave.h"
#include "TyreSave.h"
#include "EngineTemps.h"
#include "FfbLevel.h"
#include "BrakeCue.h"
#include "MiniSector.h"
#include "TyreTemps.h"
#include "FfbGraph.h"
#include "TrackMap.h"
#include "Profiler.h"
#include "ProximityHue.h"
#include "RealtimeState.h"
#include "InputLagEstimate.h"
#include "Config.h"
#include "Version.h"
#include "Layout.h"
#include "Log.h"
#include "RenderThrottle.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>

namespace
{
    // rFactor2 has more than one swapchain (the CEF/Chromium UI layer creates its
    // own, and there is the actual game view). Caching a device from whichever one
    // presented first and then mixing it with another swapchain's back buffer is
    // invalid and will corrupt or crash the process -- so everything below is
    // re-derived per swapchain and rebuilt whenever the swapchain/device changes.
    IDXGISwapChain* g_boundSwapChain = nullptr;
    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_context = nullptr;
    ID3D11RenderTargetView* g_rtv = nullptr;

    Renderer g_renderer;
    bool g_rendererReady = false;
    bool g_loggedFirstPresent = false;
    int g_faultCount = 0;
    bool g_permanentlyDisabled = false;

    // All radar tuning now lives in g_radarConfig (Config.h/.cpp), loaded from
    // rF2RadarOverlay.ini next to the DLL -- edit the INI and restart rather
    // than changing values here.

    // Element visibility is per-widget now (Ctrl+Shift+<letter>, see
    // Shortcuts.h). The old Ctrl+Tab Gray-code cycle that lived here
    // walked a fixed sequence of COMBINED states, which was already eight
    // steps for three toggles and does not extend to eleven elements.

    // Saved device state, so the game's rendering is left exactly as we found it.
    //
    // Releases whatever it's still holding when an instance goes out of
    // scope. RestoreState (the normal path) already releases every field
    // itself and nulls each pointer right after, so the destructor is a
    // no-op there -- it exists for the ABNORMAL path instead: a fault caught
    // by OnPresent's __except between SaveState and RestoreState would
    // otherwise leak every AddRef'd state object SaveState had already
    // captured (all of RSGetState/OMGetBlendState/OMGetRenderTargets/etc.
    // AddRef what they return, same as any D3D11 Get-style call). On x64,
    // __except(EXCEPTION_EXECUTE_HANDLER)'s unwind back to the __try block
    // runs C++ destructors for automatic objects in the frames it unwinds
    // through exactly like a thrown exception would, so this local's
    // destructor still fires even though nothing here ever throws.
    struct D3DStateBackup
    {
        UINT numViewports;
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        ID3D11RasterizerState* rasterState;
        ID3D11BlendState* blendState;
        FLOAT blendFactor[4];
        UINT sampleMask;
        ID3D11DepthStencilState* depthState;
        UINT stencilRef;
        ID3D11RenderTargetView* renderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
        ID3D11DepthStencilView* depthStencilView;
        ID3D11InputLayout* inputLayout;
        D3D11_PRIMITIVE_TOPOLOGY topology;
        ID3D11Buffer* vertexBuffer;
        UINT vertexStride;
        UINT vertexOffset;
        ID3D11VertexShader* vs;
        ID3D11PixelShader* ps;
        ID3D11GeometryShader* gs;

        ~D3DStateBackup()
        {
            if (rasterState) rasterState->Release();
            if (blendState) blendState->Release();
            if (depthState) depthState->Release();
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
                if (renderTargets[i]) renderTargets[i]->Release();
            if (depthStencilView) depthStencilView->Release();
            if (inputLayout) inputLayout->Release();
            if (vertexBuffer) vertexBuffer->Release();
            if (vs) vs->Release();
            if (ps) ps->Release();
            if (gs) gs->Release();
        }
    };

    void SaveState(ID3D11DeviceContext* ctx, D3DStateBackup& s)
    {
        ZeroMemory(&s, sizeof(s));
        s.numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&s.numViewports, s.viewports);
        ctx->RSGetState(&s.rasterState);
        ctx->OMGetBlendState(&s.blendState, s.blendFactor, &s.sampleMask);
        ctx->OMGetDepthStencilState(&s.depthState, &s.stencilRef);
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.renderTargets, &s.depthStencilView);
        ctx->IAGetInputLayout(&s.inputLayout);
        ctx->IAGetPrimitiveTopology(&s.topology);
        ctx->IAGetVertexBuffers(0, 1, &s.vertexBuffer, &s.vertexStride, &s.vertexOffset);
        ctx->VSGetShader(&s.vs, nullptr, nullptr);
        ctx->PSGetShader(&s.ps, nullptr, nullptr);
        ctx->GSGetShader(&s.gs, nullptr, nullptr);
    }

    void RestoreState(ID3D11DeviceContext* ctx, D3DStateBackup& s)
    {
        ctx->RSSetViewports(s.numViewports, s.viewports);
        ctx->RSSetState(s.rasterState);
        ctx->OMSetBlendState(s.blendState, s.blendFactor, s.sampleMask);
        ctx->OMSetDepthStencilState(s.depthState, s.stencilRef);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.renderTargets, s.depthStencilView);
        ctx->IASetInputLayout(s.inputLayout);
        ctx->IASetPrimitiveTopology(s.topology);
        ctx->IASetVertexBuffers(0, 1, &s.vertexBuffer, &s.vertexStride, &s.vertexOffset);
        ctx->VSSetShader(s.vs, nullptr, 0);
        ctx->PSSetShader(s.ps, nullptr, 0);
        ctx->GSSetShader(s.gs, nullptr, 0);

        // Nulled right after releasing -- ~D3DStateBackup() releases whatever
        // it still finds non-null once 's' goes out of scope, so clearing
        // each pointer here (the normal path) makes that a no-op instead of
        // a double-release.
        if (s.rasterState) { s.rasterState->Release(); s.rasterState = nullptr; }
        if (s.blendState) { s.blendState->Release(); s.blendState = nullptr; }
        if (s.depthState) { s.depthState->Release(); s.depthState = nullptr; }
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            if (s.renderTargets[i]) { s.renderTargets[i]->Release(); s.renderTargets[i] = nullptr; }
        if (s.depthStencilView) { s.depthStencilView->Release(); s.depthStencilView = nullptr; }
        if (s.inputLayout) { s.inputLayout->Release(); s.inputLayout = nullptr; }
        if (s.vertexBuffer) { s.vertexBuffer->Release(); s.vertexBuffer = nullptr; }
        if (s.vs) { s.vs->Release(); s.vs = nullptr; }
        if (s.ps) { s.ps->Release(); s.ps = nullptr; }
        if (s.gs) { s.gs->Release(); s.gs = nullptr; }
    }

    void ReleaseBinding()
    {
        if (g_rendererReady)
        {
            g_renderer.Shutdown();
            g_rendererReady = false;
        }
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
        if (g_context) { g_context->Release(); g_context = nullptr; }
        if (g_device) { g_device->Release(); g_device = nullptr; }
        g_boundSwapChain = nullptr;
    }

    // Binds device/context/renderer for this swapchain. The render target view is
    // NOT cached here: with flip-model swapchains buffer 0 rotates every frame, so
    // a cached RTV would point at the wrong texture. It is built per frame instead.
    bool BindTo(IDXGISwapChain* swapChain)
    {
        if (g_boundSwapChain == swapChain && g_rendererReady)
            return true;

        ReleaseBinding();

        ID3D11Device* device = nullptr;
        if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) || !device)
        {
            Log::Write("BindTo: swapchain %p is not a D3D11 device, skipping", swapChain);
            return false;
        }

        device->GetImmediateContext(&g_context);
        g_device = device;
        g_boundSwapChain = swapChain;

        g_renderer.SetFont(g_textFontConfig.font, g_textFontConfig.weight);
        if (!g_renderer.Init(g_device))
        {
            Log::Write("BindTo: Renderer::Init FAILED");
            ReleaseBinding();
            return false;
        }

        g_rendererReady = true;
        PerfStats::Init(swapChain);
        Log::Write("BindTo: bound to swapchain %p (device %p) OK", swapChain, g_device);
        return true;
    }

    // Case-insensitive "does haystack contain needle" -- car class/name
    // strings arrive in whatever case the mod/content author used.
    bool ContainsCI(const char* haystack, const char* needle)
    {
        if (haystack[0] == '\0' || needle[0] == '\0')
            return false;
        const size_t hLen = strlen(haystack);
        const size_t nLen = strlen(needle);
        if (nLen > hLen)
            return false;
        for (size_t i = 0; i + nLen <= hLen; ++i)
        {
            size_t j = 0;
            for (; j < nLen; ++j)
            {
                if (tolower(static_cast<unsigned char>(haystack[i + j])) != tolower(static_cast<unsigned char>(needle[j])))
                    break;
            }
            if (j == nLen)
                return true;
        }
        return false;
    }

    // Typical real footprint for a handful of recognisable categories,
    // matched against the car's own VehicleScoringInfoV01 class and name.
    // The SDK exposes no actual per-car dimension field, so this is not
    // exact for any one specific car -- but it is a genuine improvement over
    // one flat box shared by every car on a mixed grid (a BTCC-style touring
    // car IS narrower and shorter than a GT3 car). Checked in order; the
    // first match wins. A car matching none of these keeps the configured
    // CarHalfWidthMeters/CarHalfLengthMeters/CarCornerRadiusMeters box,
    // exactly as with RadarConfig::useRealCarSize turned off.
    //
    // Figures are half of a real published overall length/width for a
    // representative car in each class (converted from mm), not invented:
    //   GT3   -- Audi R8 LMS GT3 evo: 4600 x 1996mm
    //   GTE   -- Porsche 911 RSR (2017): 4557 x 2045mm (axle-width figure)
    //   LMP   -- Oreca 07 LMP2: 4745 x 1895mm (LMP1/LMP3 regs land within a
    //            few cm of this too -- Toyota TS050 4650x1900, Ligier
    //            JS P320 4605x1900 -- so one shared bucket stays accurate)
    //   NASCAR (Cup) -- Next Gen car: 4912 x 1996mm
    //   INDYCAR -- Dallara IR18, ~5080 x 2000mm (published figures for this
    //            one are inconsistent between sources; used the wider,
    //            more conservative reading)
    // GT4/TCR/TOURING/BTCC/FORMULA/PROTOTYPE/TRUCK are class-typical
    // approximations, not each individually sourced.
    struct CarCategory { const char* keyword; float halfWidthM, halfLengthM, cornerRadiusM; };
    const CarCategory kCarCategories[] =
    {
        { "GTE",       1.02f, 2.28f, 0.45f },  // ~2.05m x 4.56m
        { "GT3",       1.00f, 2.30f, 0.45f },  // ~2.0m x 4.6m
        { "GT4",       0.95f, 2.20f, 0.40f },  // ~1.9m x 4.4m
        { "TCR",       0.90f, 2.15f, 0.40f },  // ~1.8m x 4.3m
        { "TOURING",   0.90f, 2.15f, 0.40f },
        { "BTCC",      0.90f, 2.15f, 0.40f },
        { "TRUCK",     1.00f, 2.45f, 0.45f },  // NASCAR truck -- approximate, no verified source found
        { "NASCAR",    1.00f, 2.45f, 0.45f },  // ~2.0m x 4.9m
        { "DPI",       0.95f, 2.38f, 0.40f },  // ~1.9m x 4.75m
        { "LMP",       0.95f, 2.37f, 0.40f },  // ~1.9m x 4.75m
        { "PROTOTYPE", 0.95f, 2.37f, 0.40f },
        { "INDYCAR",   1.00f, 2.54f, 0.35f },  // ~2.0m x 5.08m
        { "IR18",      1.00f, 2.54f, 0.35f },
        { "FORMULA",   0.90f, 2.50f, 0.30f },  // open-wheel: narrower, longer
    };

    void LookupCarFootprint(const RadarConfig& cfg, const char* vehClass, const char* vehName,
                            float& outHalfWidthM, float& outHalfLengthM, float& outCornerRadiusM)
    {
        outHalfWidthM = cfg.carHalfWidthM;
        outHalfLengthM = cfg.carHalfLengthM;
        outCornerRadiusM = cfg.carCornerRadiusM;
        if (cfg.useRealCarSize)
        {
            for (const CarCategory& cat : kCarCategories)
            {
                if (ContainsCI(vehClass, cat.keyword) || ContainsCI(vehName, cat.keyword))
                {
                    outHalfWidthM = cat.halfWidthM;
                    outHalfLengthM = cat.halfLengthM;
                    outCornerRadiusM = cat.cornerRadiusM;
                    break;
                }
            }
        }
        outHalfWidthM += cfg.footprintBufferMeters;
        outHalfLengthM += cfg.footprintBufferMeters;
    }

    void DrawRadar(int screenWidth, int screenHeight)
    {
        if (!g_radarConfig.enabled)
            return;

        CarPos cars[128];
        double playerHeading = 0.0;
        bool hasPlayer = false;
        const int count = g_carState.Read(cars, 128, playerHeading, hasPlayer);

        if (count == 0 || !hasPlayer)
            return; // not on track -- draw nothing rather than covering menus

        // Locate the player entry for the position origin.
        const CarPos* player = nullptr;
        for (int i = 0; i < count; ++i)
        {
            if (cars[i].isPlayer)
            {
                player = &cars[i];
                break;
            }
        }
        if (!player)
            return;

        const RadarConfig& cfg = g_radarConfig;

        // Size and position derived from the actual screen, so this works at any
        // resolution rather than being hardcoded pixels.
        const float radarRadiusPx = (screenWidth * cfg.widthFraction) * 0.5f * cfg.scale;
        const float centerX = screenWidth * cfg.centerXFraction;
        const float centerY = screenHeight * cfg.centerYFraction;

        // Square grab box around the circular radar -- close enough, and the
        // radar is the largest panel on screen, so it loses any overlap
        // hit-test to whatever sits on top of it anyway.
        Layout::Register("Radar", "Radar", "CenterXFraction", "CenterYFraction",
                         &g_radarConfig.centerXFraction, &g_radarConfig.centerYFraction,
                         centerX, centerY, radarRadiusPx, radarRadiusPx,
                         &g_radarConfig.scale, "Scale");

        // Radar is scaled to the largest configured range so everything fits.
        const float worldRange = (cfg.rangeSideM > cfg.rangeForwardM)
            ? (cfg.rangeSideM > cfg.rangeRearM ? cfg.rangeSideM : cfg.rangeRearM)
            : (cfg.rangeForwardM > cfg.rangeRearM ? cfg.rangeForwardM : cfg.rangeRearM);

        const double cosH = cos(-playerHeading);
        const double sinH = sin(-playerHeading);
        const float scale = radarRadiusPx / worldRange;

        // First pass: work out which cars are actually in range, so the whole
        // radar can stay hidden when nothing is near.
        struct Blip { float x, y, rot, alpha; bool danger; float halfWm, halfLm, cornerRm; };
        Blip blips[128];
        int blipCount = 0;
        float strongestAlpha = 0.0f;

        // Diagnostic: dumps ground-truth numbers once a second so the icon
        // rotation formula can be checked against reality instead of
        // guessed at from a screenshot -- see RadarConfig::logOrientationDebug.
        static LARGE_INTEGER s_lastOrientationLog = {};
        static LARGE_INTEGER s_orientationLogFreq = {};
        bool logThisFrame = false;
        if (cfg.logOrientationDebug)
        {
            if (s_orientationLogFreq.QuadPart == 0)
                QueryPerformanceFrequency(&s_orientationLogFreq);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double sinceLog = (s_lastOrientationLog.QuadPart == 0) ? 1.0e9
                : static_cast<double>(now.QuadPart - s_lastOrientationLog.QuadPart) / static_cast<double>(s_orientationLogFreq.QuadPart);
            if (sinceLog >= 1.0)
            {
                s_lastOrientationLog = now;
                logThisFrame = true;
                Log::Write("OrientDebug: player world pos=(%.1f,%.1f) heading=%.1fdeg",
                           player->x, player->z, playerHeading * 57.29577951308232);
            }
        }

        for (int i = 0; i < count; ++i)
        {
            if (cars[i].isPlayer)
                continue;

            const double dx = cars[i].x - player->x;
            const double dz = cars[i].z - player->z;

            // Rotate world delta into player-forward-up radar space.
            const double relX = dx * cosH - dz * sinH;
            const double relZ = dx * sinH + dz * cosH;

            // Offsets in radar space (negated on both axes -- the first version
            // came out rotated 180 degrees, with cars ahead appearing behind).
            const double lateral = -relX;   // +ve = to the right on the radar
            const double forward = -relZ;   // +ve = ahead of the player

            // Elliptical range test. Equivalent to comparing the car's distance
            // against a limit that eases smoothly from 10m fore/aft out to 20m
            // abeam, with no hard edges in between.
            const double longRange = (forward >= 0.0) ? cfg.rangeForwardM : cfg.rangeRearM;
            const double nx = lateral / cfg.rangeSideM;
            const double nz = forward / longRange;

            // 0 at the player, 1 exactly on the range boundary.
            const double edgeDist = sqrt(nx * nx + nz * nz);
            if (edgeDist > 1.0)
                continue;

            // Fade over the outer band so cars ease in and out of view.
            double alpha = 1.0;
            if (edgeDist > cfg.fadeStartFraction)
            {
                const double t = (edgeDist - cfg.fadeStartFraction) / (1.0 - cfg.fadeStartFraction);
                alpha = 1.0 - t;
                // Smoothstep, so the fade eases at both ends rather than ramping
                // linearly and appearing to "start" abruptly.
                alpha = alpha * alpha * (3.0 - 2.0 * alpha);
            }
            if (alpha <= 0.004)
                continue;

            // Danger classification, in real centre-to-centre metres (not
            // screen-scaled, and not bumper gaps). Both axes must qualify.
            //
            // This is the SAME test ProximityHue.cpp applies for its side
            // indicator, off the same three config values, deliberately --
            // a blip going red and a side bar appearing must always mean
            // exactly the same thing.
            const bool inLongitudinalWindow =
                (forward <= cfg.dangerAheadM) && (forward >= -cfg.dangerBehindM);
            const bool lateralOverlap = fabs(lateral) <= cfg.dangerLateralM;

            blips[blipCount].x = centerX + static_cast<float>(lateral) * scale;
            blips[blipCount].y = centerY - static_cast<float>(forward) * scale;

            // Sign flip vs the plain "carHeading - playerHeading": per user
            // report, a car further into a corner than the player should
            // rotate MORE toward the corner, and a car not yet at the
            // corner should rotate the OTHER way -- that relative logic was
            // already right (it falls straight out of the subtraction), but
            // the reported behaviour was mirrored for BOTH front and back
            // at once, consistent with the render's rotation parameter
            // turning the opposite way on screen from what the raw angle
            // subtraction assumes. Single-variable test: negate the whole
            // thing and see if that resolves it, rather than the deeper
            // (and better-supported, but riskier) transform rewrite noted
            // in OrientDebug's investigation.
            blips[blipCount].rot = static_cast<float>(playerHeading - cars[i].heading);
            blips[blipCount].alpha = static_cast<float>(alpha);
            blips[blipCount].danger = inLongitudinalWindow && lateralOverlap;
            LookupCarFootprint(cfg, cars[i].vehicleClass, cars[i].vehicleName,
                               blips[blipCount].halfWm, blips[blipCount].halfLm, blips[blipCount].cornerRm);

            if (logThisFrame)
            {
                Log::Write("OrientDebug:   car world pos=(%.1f,%.1f) heading=%.1fdeg -> lateral=%.2f forward=%.2f rot=%.1fdeg",
                           cars[i].x, cars[i].z, cars[i].heading * 57.29577951308232,
                           lateral, forward, blips[blipCount].rot * 57.29577951308232);
            }
            if (blips[blipCount].alpha > strongestAlpha)
                strongestAlpha = blips[blipCount].alpha;
            ++blipCount;
        }

        // Nothing close by -> draw nothing at all (fully transparent).
        if (blipCount == 0)
            return;

        // No background disc, no ring -- just the cars.

        bool anyDanger = false;
        for (int i = 0; i < blipCount; ++i)
        {
            if (blips[i].danger) { anyDanger = true; break; }
        }
        if (anyDanger && cfg.dangerFlashEnabled)
        {
            // Wall-clock pulse (not tied to telemetry ticks), same reasoning
            // as the pedal trace's idle fade and gear display's blink -- it
            // keeps animating smoothly regardless of frame/update rate.
            static float s_flashPhase = 0.0f;
            static LARGE_INTEGER s_flashLastTime = {};
            static LARGE_INTEGER s_flashFreq = {};
            if (s_flashFreq.QuadPart == 0)
                QueryPerformanceFrequency(&s_flashFreq);

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double dt = 0.0;
            if (s_flashLastTime.QuadPart != 0 && s_flashFreq.QuadPart != 0)
            {
                dt = static_cast<double>(now.QuadPart - s_flashLastTime.QuadPart) / static_cast<double>(s_flashFreq.QuadPart);
                if (dt < 0.0 || dt > 1.0) dt = 0.0;
            }
            s_flashLastTime = now;

            const double period = (cfg.dangerFlashPeriodSeconds > 0.05) ? cfg.dangerFlashPeriodSeconds : 0.05;
            s_flashPhase = fmodf(s_flashPhase + static_cast<float>(dt / period), 1.0f);

            // Smooth breathing pulse (0..1..0), not a hard on/off blink.
            const float pulse = 0.5f - 0.5f * cosf(s_flashPhase * 6.2831853f);
            const float flashAlpha = cfg.dangerFlashMaxAlpha * pulse;

            if (flashAlpha > 0.002f)
            {
                g_renderer.AddFilledCircle(centerX, centerY, radarRadiusPx,
                                           cfg.dangerFlashR, cfg.dangerFlashG, cfg.dangerFlashB, flashAlpha);
            }
        }

        // Other cars: yellow if clear ahead/behind, angled to match their real
        // heading, faded near the edge. Danger cars drawn in a second pass so
        // they land on top of any overlapping yellow one. Each blip carries
        // its OWN footprint (see LookupCarFootprint) rather than one size
        // shared by the whole grid.
        for (int i = 0; i < blipCount; ++i)
        {
            if (blips[i].danger) continue;
            g_renderer.AddFilledRoundedRect(blips[i].x, blips[i].y,
                                            blips[i].halfWm * scale, blips[i].halfLm * scale, blips[i].cornerRm * scale,
                                            blips[i].rot, cfg.otherR, cfg.otherG, cfg.otherB, blips[i].alpha);
        }
        // Danger: inside the longitudinal window and laterally overlapping -- red.
        for (int i = 0; i < blipCount; ++i)
        {
            if (!blips[i].danger) continue;
            g_renderer.AddFilledRoundedRect(blips[i].x, blips[i].y,
                                            blips[i].halfWm * scale, blips[i].halfLm * scale, blips[i].cornerRm * scale,
                                            blips[i].rot, cfg.dangerR, cfg.dangerG, cfg.dangerB, blips[i].alpha);
        }

        // Player: green, centred. Matches the nearest car's opacity so the
        // whole radar fades up and away as one, instead of the marker popping.
        float playerHalfWm, playerHalfLm, playerCornerRm;
        LookupCarFootprint(cfg, player->vehicleClass, player->vehicleName,
                           playerHalfWm, playerHalfLm, playerCornerRm);
        g_renderer.AddFilledRoundedRect(centerX, centerY,
                                        playerHalfWm * scale, playerHalfLm * scale, playerCornerRm * scale, 0.0f,
                                        cfg.playerR, cfg.playerG, cfg.playerB, strongestAlpha);
    }

    // Rounds to 'sigFigs' significant figures and formats with just enough
    // decimal places for that -- e.g. 8.34 -> "8.3", 12.7 -> "13", 0.85 -> "0.85".
    void FormatSigFigs(double value, int sigFigs, char* out, size_t outSize)
    {
        // Anything not finite, not positive, or too small to show at any
        // sensible precision is just "0". The smallness test is a CRASH
        // GUARD, not cosmetics: decimals below is derived from the value's
        // exponent, so a near-zero input (the CPU/GPU busy EMAs decay toward
        // zero while sitting at the menu and never quite arrive, reaching
        // 1e-20 and beyond) would ask for "%.21f" and write far more than
        // outSize. sprintf_s does not truncate on overflow -- it invokes the
        // invalid parameter handler, which terminates the process outright,
        // with no exception for the event log to record.
        if (!std::isfinite(value) || value < 1e-4)
        {
            sprintf_s(out, outSize, "0");
            return;
        }
        const int exponent = static_cast<int>(std::floor(std::log10(value)));
        const double magnitude = std::pow(10.0, exponent - (sigFigs - 1));
        const double rounded = std::round(value / magnitude) * magnitude;
        // Recompute from the rounded value in case rounding bumped it up a
        // power of ten (e.g. 9.96 -> 10).
        const int roundedExponent = (rounded > 0.0) ? static_cast<int>(std::floor(std::log10(rounded))) : exponent;
        int decimals = sigFigs - 1 - roundedExponent;
        if (decimals < 0) decimals = 0;
        if (decimals > 6) decimals = 6;

        // Belt and braces: _TRUNCATE makes even an unforeseen oversized
        // result truncate rather than kill the game.
        _snprintf_s(out, outSize, _TRUNCATE, "%.*f", decimals, rounded);
    }

    // One "stat | stat | stat" row where each segment can carry its own
    // color (used so CPU Busy / GPU Busy can flash orange/red independently
    // while FPS / Display Latency stay the normal color).
    // widthRef is a fixed worst-case string used ONLY to size/position this
    // segment's slot -- not displayed. Without it, the live text's own
    // varying length (a longer vsync-status phrase, one more digit) would
    // shift every segment after it and resize the whole box every frame.
    // Defaults to text itself when null (segments with genuinely fixed-
    // length content, e.g. the CPU/GPU-bound label, don't need a separate one).
    struct StatSegment { const char* text; float r, g, b; const char* widthRef = nullptr; };

    // Draws one background box plus a single row of color-tagged segments
    // joined by " | ". boxBottomY is the background box's own outer bottom
    // edge (padding included) -- so setting it to screenHeight puts the box
    // flush against the bottom of the screen instead of the padding pushing
    // it upward from wherever the text started. boxX is either that same
    // edge's horizontal CENTRE, or its RIGHT edge when anchorRight is true
    // (used to line the box's right edge up with another widget's, e.g. the
    // pedal trace, rather than centering it independently).
    // Drag hook for the perf stats: drops whatever alignment flag is pinning
    // the panel, so the position fractions (and therefore the drag) take
    // effect. Persisted immediately -- otherwise the panel would snap back to
    // its aligned corner on the next restart, silently undoing the drag.
    void ReleasePerfStatsAlignment()
    {
        FrameLatencyConfig& cfg = g_frameLatencyConfig;
        if (!cfg.alignBottomRightCorner && !cfg.alignTop && !cfg.alignRightToPedalTrace)
            return;
        cfg.alignBottomRightCorner = false;
        cfg.alignTop = false;
        cfg.alignRightToPedalTrace = false;
        Config::WriteBool("FrameLatency", "AlignBottomRightCorner", false);
        Config::WriteBool("FrameLatency", "AlignTop", false);
        Config::WriteBool("FrameLatency", "AlignRightToPedalTrace", false);
        Log::Write("Perf stats: alignment flags cleared -- panel is now free-floating");
    }

    void DrawSegmentedStatLine(const FrameLatencyConfig& cfg, const StatSegment* segments, int count,
                                float boxX, float boxBottomY, float uiScale, bool anchorRight = false, bool alignTop = false,
                                bool registerForLayout = false)
    {
        if (count == 0)
            return;

        const float textHeightPx = cfg.textHeightPx * uiScale;
        // Floored to a fraction of the text size itself rather than trusting
        // the configured pixel value alone -- tall glyphs (parentheses,
        // ascenders/descenders in "stable cap", "%") sit outside the plain
        // text-height box, and a small configured padding (or a bigger font
        // at a higher UI scale) previously clipped them against the edge of
        // the plate. This way the box is always big enough for what's
        // actually drawn, automatically, at any scale.
        const float configuredPaddingPx = cfg.bgPaddingPx * uiScale;
        const float autoPaddingPx = textHeightPx * 0.3f;
        const float bgPaddingPx = (configuredPaddingPx > autoPaddingPx) ? configuredPaddingPx : autoPaddingPx;
        const float bgCornerRadiusPx = cfg.bgCornerRadiusPx * uiScale;

        const char* kSep = " | ";
        const float sepW = g_renderer.MeasureText(kSep, textHeightPx);

        // Sized from each segment's fixed widthRef (falling back to its own
        // text when none is given), never from the live text directly --
        // see StatSegment's comment for why.
        float segW[8] = {};
        float totalW = 0.0f;
        for (int i = 0; i < count && i < 8; ++i)
        {
            segW[i] = g_renderer.MeasureText(segments[i].widthRef ? segments[i].widthRef : segments[i].text, textHeightPx);
            totalW += segW[i];
            if (i + 1 < count) totalW += sepW;
        }

        const float halfW = totalW * 0.5f + bgPaddingPx;
        const float halfH = textHeightPx * 0.5f + bgPaddingPx;
        const float boxCenterX = anchorRight ? (boxX - halfW) : boxX;
        // Top-flush: the box's own TOP edge (centerY - halfH) touches y=0
        // exactly, regardless of boxBottomY -- the fraction-based
        // positioning this overrides only ever anchors the BOTTOM edge, so
        // it can't express "flush with the top" on its own.
        const float centerY = alignTop ? halfH : (boxBottomY - halfH);

        // Registered whichever alignment mode is in force. The three align
        // flags each OVERRIDE the position fractions, so a drag would be
        // silently undone next frame while one is set -- hence the drag hook
        // below, which turns them off (and persists that) the moment the
        // panel is grabbed. Flag and drag are two things fighting over one
        // position; grabbing it settles the argument in favour of the drag.
        if (registerForLayout)
        {
            Layout::Register("Perf stats", "FrameLatency", "CpuGpuPosXFraction", "CpuGpuPosYFraction",
                             &g_frameLatencyConfig.cpuGpuPosXFraction, &g_frameLatencyConfig.cpuGpuPosYFraction,
                             boxCenterX, centerY, halfW, halfH,
                             &g_frameLatencyConfig.scale, "Scale",
                             &ReleasePerfStatsAlignment);
        }

        if (cfg.bgEnabled)
        {
            g_renderer.AddFilledRoundedRect(boxCenterX, centerY, halfW, halfH, bgCornerRadiusPx, 0.0f,
                                            cfg.bgR, cfg.bgG, cfg.bgB, cfg.bgA);
        }

        // This line already sits on ONE box, and it is drawn as several
        // separate AddText calls (one per stat, plus the separators). The
        // renderer's per-string text plate would therefore stamp a differently
        // sized rectangle behind every one of them -- and since each segment
        // is laid out to a fixed widthRef rather than its live text, those
        // rectangles do not even line up with the columns, which reads as a
        // ragged row of mismatched grey boxes inside the real one. Suppress it
        // for the whole line and restore afterwards.
        const bool prevTextBg = g_renderer.TextBackgroundEnabled();
        g_renderer.SetTextBackground(false);

        const float textY = centerY - textHeightPx * 0.5f;
        float x = boxCenterX - totalW * 0.5f;
        for (int i = 0; i < count && i < 8; ++i)
        {
            g_renderer.AddText(x, textY, segments[i].text, textHeightPx,
                               segments[i].r, segments[i].g, segments[i].b, 1.0f, Renderer::HAlign::Left);
            x += segW[i];
            if (i + 1 < count)
            {
                g_renderer.AddText(x, textY, kSep, textHeightPx, cfg.colorR, cfg.colorG, cfg.colorB, 1.0f, Renderer::HAlign::Left);
                x += sepW;
            }
        }

        g_renderer.SetTextBackground(prevTextBg);
    }

    // FPS, Display Latency, and Input Lag, joined into one horizontal line
    // ("stat | stat | stat"), centred along the bottom of the screen.
    //
    // The underlying values are smoothed every frame (see FrameRate.cpp /
    // FrameLatency.cpp), but the ON-SCREEN text only refreshes twice a
    // second -- fast enough to track
    // real changes, slow enough to read without flickering digit-to-digit.
    // Wall-clock time of day, top right. Styled to match the frametime stats
    // (same font colour and translucent plate) so the two read as one family
    // of "information about the machine, not the car".
    void DrawClock(int screenWidth, int screenHeight)
    {
        const ClockConfig& cfg = g_clockConfig;
        if (!cfg.enabled)
            return;
        // Only while actually driving (between EnterRealtime/ExitRealtime) --
        // not in the menus, the session monitor, or spectating, and not even
        // in layout mode: that is used from the cockpit (see Layout.cpp),
        // and out of the car only the perf overlay stays up at all.
        if (!g_inRealtime.load(std::memory_order_relaxed))
            return;

        // Low tier (~5Hz) -- it's a clock, 1Hz would even suffice, but 5Hz
        // is cheap and keeps a shared rule with the rest of the Low tier.
        // Bypassed in layout mode so dragging/resizing stays smooth.
        static RenderThrottle s_throttle(0.2);
        if (!Layout::Active() && s_throttle.TryReplay(g_renderer))
            return;
        RenderThrottle::Capture throttleCapture(s_throttle, g_renderer);

        SYSTEMTIME lt;
        GetLocalTime(&lt);

        char buf[16];
        if (cfg.showSeconds)
            sprintf_s(buf, "%02d:%02d:%02d", lt.wHour, lt.wMinute, lt.wSecond);
        else
            sprintf_s(buf, "%02d:%02d", lt.wHour, lt.wMinute);

        // cfg.scale is the layout-mode corner-handle multiplier. Folded in
        // HERE rather than applied to the drawn box, so every pixel value
        // downstream scales with it in exact proportion.
        const float uiScale = static_cast<float>(screenHeight) / 2160.0f * cfg.scale;
        const float textPx = cfg.textHeightPx * uiScale;

        // Flush into the bottom-LEFT corner. posYFraction is the BOTTOM edge
        // (1.0 = the very bottom), so the glyph height comes off it here --
        // AddText takes the text's TOP y.
        {
            // Built upward from posYFraction the same way the text is, since
            // that fraction names the BOTTOM edge rather than a centre.
            const float clockW = g_renderer.MeasureText(buf, textPx);
            const float leftX = screenWidth * cfg.posXFraction;
            const float bottomY = screenHeight * cfg.posYFraction;
            Layout::Register("Clock", "Clock", "PosXFraction", "PosYFraction",
                             &g_clockConfig.posXFraction, &g_clockConfig.posYFraction,
                             leftX + clockW * 0.5f, bottomY - textPx * 0.5f,
                             clockW * 0.5f, textPx * 0.5f,
                             &g_clockConfig.scale, "Scale");
        }

        // Matches every other readout's grey plate now (this used to
        // suppress it to sit flush in the corner, but it should get the
        // same legibility treatment here as everywhere else -- the plate's
        // own padding pushing it a few pixels off the exact corner is an
        // acceptable trade for that).
        g_renderer.AddText(screenWidth * cfg.posXFraction,
                           screenHeight * cfg.posYFraction - textPx,
                           buf, textPx, cfg.r, cfg.g, cfg.b, 1.0f, Renderer::HAlign::Left);
    }

    void DrawPerfOverlay(int screenWidth, int screenHeight)
    {
        const FrameLatencyConfig& cfg = g_frameLatencyConfig;
        if (!cfg.enabled)
            return;

        static char s_fpsBuf[72] = "";
        static char s_frameBuf[32] = "";
        static char s_latencyBuf[40] = "";
        static char s_inputLagBuf[40] = "";
        static char s_fpsRangeBuf[56] = "";
        static char s_cpuBuf[32] = "";
        static char s_gpuBuf[32] = "";
        static char s_ramBuf[32] = "";
        static char s_vramBuf[48] = "";
        static char s_gpuPowerBuf[32] = "";
        static bool s_haveFps = false, s_haveLatency = false;
        static bool s_haveInputLag = false;
        static bool s_haveFpsRange = false, s_haveCpu = false, s_haveGpu = false, s_haveRam = false, s_haveVram = false;
        static bool s_haveGpuPower = false;
        // Which of the two FPS width references currently applies -- see
        // where it's set below for why this can't just be one fixed string.
        static bool s_fpsIsCapped = false;
        // Same idea for Display Latency: the numeric case ("8.2 ms") and the
        // "(needs fullscreen)" fallback are wildly different lengths, and
        // reserving worst-case space for the rare fallback would leave a
        // large dead gap after the far more common short numeric text.
        static bool s_latencyIsFallback = false;
        static LARGE_INTEGER s_lastRefresh = {};
        static LARGE_INTEGER s_freq = {};

        if (s_freq.QuadPart == 0)
            QueryPerformanceFrequency(&s_freq);

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double sinceRefreshMs = (s_lastRefresh.QuadPart == 0) ? 1.0e9
            : static_cast<double>(now.QuadPart - s_lastRefresh.QuadPart) / static_cast<double>(s_freq.QuadPart) * 1000.0;

        const double refreshIntervalMs = 1000.0 / g_perfUpdateConfig.updateHz;
        if (sinceRefreshMs >= refreshIntervalMs)
        {
            double fps = 0.0, latencyMs = 0.0;
            const bool haveFps = cfg.showFps && FrameRate::GetFps(fps);
            const bool haveLatency = FrameLatency::GetLatencyMs(latencyMs);

            char valBuf[16];

            // Cap detection is fed at this same [Performance] UpdateHz
            // cadence (real-time-windowed internally, see FrameRate.cpp) --
            // see there for why the CPU/GPU busy% budget below is judged
            // against this detected ceiling rather than the live FPS above.
            double capFps = 0.0;
            bool isCapped = false;
            if (haveFps)
            {
                FrameRate::PollCapDetection(fps);
                isCapped = FrameRate::IsCapped(capFps);
            }

            s_haveFps = haveFps;
            if (haveFps)
            {
                sprintf_s(s_fpsBuf, "FPS %.0f", fps);
                sprintf_s(s_frameBuf, "Frame %.1f ms", 1000.0 / fps);
                // "Stable cap" is a rare, meaningful state CHANGE (only
                // flips when the cap is gained/lost), not per-frame noise --
                // so unlike a digit count, it's fine for its width slot to
                // actually change size on the (infrequent) transition,
                // rather than always reserving room for the longer variant
                // and leaving a visible gap the rest of the time.
                s_fpsIsCapped = isCapped;
            }

            // "Display latency" -- the real name for what GetFrameStatistics
            // reports (Present() call to actual scan-out), distinct from a
            // plain frame time and from CapFrameX/PresentMon's own figures.
            //
            // Above 100ms something is already badly wrong (a stall, a load,
            // focus loss) -- showing the exact spiky number just adds noise,
            // so it's capped to ">100" instead.
            s_haveLatency = haveLatency;
            if (haveLatency)
            {
                s_latencyIsFallback = false;
                if (latencyMs > 100.0)
                    sprintf_s(s_latencyBuf, "Display Latency >100 ms");
                else
                {
                    FormatSigFigs(latencyMs, 2, valBuf, sizeof(valBuf));
                    sprintf_s(s_latencyBuf, "Display Latency %s ms", valBuf);
                }
            }
            else if (FrameLatency::GetUnavailableReason() == FrameLatency::UnavailableReason::NotFullscreen)
            {
                // DXGI only reports real frame statistics in EXCLUSIVE
                // fullscreen -- windowed and borderless both fail every call
                // (see FrameLatency.cpp for which HRESULT that actually is),
                // so flag that explicitly instead of just silently omitting
                // the stat (which reads as "broken" rather than "not
                // applicable here").
                s_latencyIsFallback = true;
                sprintf_s(s_latencyBuf, "Display Latency (needs fullscreen)");
                s_haveLatency = true;
            }

            // Input-lag estimate: needs Display Latency as its second term,
            // so it can only be shown once that's available.
            s_haveInputLag = cfg.showInputLagEstimate && haveLatency;
            if (s_haveInputLag)
            {
                double inputLagMs = 0.0;
                s_haveInputLag = InputLagEstimate::GetEstimateMs(latencyMs, inputLagMs);
                if (s_haveInputLag)
                {
                    FormatSigFigs(inputLagMs, 2, valBuf, sizeof(valBuf));
                    sprintf_s(s_inputLagBuf, "Input Lag (est) %s ms", valBuf);
                }
            }

            // Extra diagnostic stats -- polled here at the same refresh
            // cadence as everything else above (see PerfStats.h for why
            // this shouldn't run every frame).
            PerfStats::Poll();
            GpuEngineUsage::Poll();

            char valBuf2[16], valBuf3[16];

            s_haveFpsRange = false;
            if (cfg.showFpsRange)
            {
                double minFps = 0.0, low1Fps = 0.0, maxFps = 0.0;
                if (FrameRate::GetFpsRange(minFps, low1Fps, maxFps))
                {
                    FormatSigFigs(minFps, 3, valBuf, sizeof(valBuf));
                    FormatSigFigs(low1Fps, 3, valBuf2, sizeof(valBuf2));
                    FormatSigFigs(maxFps, 3, valBuf3, sizeof(valBuf3));
                    sprintf_s(s_fpsRangeBuf, "FPS %s-%s (1%%low %s)", valBuf, valBuf3, valBuf2);
                    s_haveFpsRange = true;
                }
            }

            s_haveCpu = false;
            if (cfg.showCpuFrameTime)
            {
                double cpuMs = 0.0;
                if (PerfStats::GetCpuFrameTimeMs(cpuMs))
                {
                    sprintf_s(s_cpuBuf, "CPU %.1f ms", cpuMs);
                    s_haveCpu = true;
                }
            }

            s_haveGpu = false;
            if (cfg.showGpuFrameTime)
            {
                // Real GPU busy% (OS "GPU Engine" perf counters -- see
                // GpuEngineUsage.h) times the actual current frame period,
                // giving a busy-ms figure comparable to CapFrameX/Task
                // Manager's own GPU number -- NOT a D3D11 Present-to-Present
                // timestamp interval, which was tried here first and reads
                // high under a frame cap (includes GPU idle/pacing time).
                double gpuPercent = 0.0;
                double fpsForGpuMs = 0.0;
                if (FrameRate::GetFps(fpsForGpuMs) && fpsForGpuMs > 0.0 && GpuEngineUsage::GetBusyPercent(gpuPercent))
                {
                    const double framePeriodMs = 1000.0 / fpsForGpuMs;
                    const double gpuMs = (gpuPercent / 100.0) * framePeriodMs;
                    sprintf_s(s_gpuBuf, "GPU %.1f ms", gpuMs);
                    s_haveGpu = true;
                }
            }

            s_haveRam = false;
            if (cfg.showProcessRam)
            {
                double ramMB = 0.0;
                if (PerfStats::GetProcessRamMB(ramMB))
                {
                    FormatSigFigs(ramMB, 4, valBuf, sizeof(valBuf));
                    sprintf_s(s_ramBuf, "RAM %s MB", valBuf);
                    s_haveRam = true;
                }
            }

            s_haveVram = false;
            if (cfg.showVram)
            {
                double vramUsedMB = 0.0, vramBudgetMB = 0.0;
                if (PerfStats::GetVramMB(vramUsedMB, vramBudgetMB))
                {
                    FormatSigFigs(vramUsedMB, 4, valBuf, sizeof(valBuf));
                    FormatSigFigs(vramBudgetMB, 4, valBuf2, sizeof(valBuf2));
                    sprintf_s(s_vramBuf, "VRAM %s/%s MB", valBuf, valBuf2);
                    s_haveVram = true;
                }
            }

            s_haveGpuPower = false;
            if (cfg.showGpuPower)
            {
                double watts = 0.0;
                if (AmdGpuPower::GetPowerWatts(watts))
                {
                    FormatSigFigs(watts, 3, valBuf, sizeof(valBuf));
                    sprintf_s(s_gpuPowerBuf, "GPU %s W", valBuf);
                    s_haveGpuPower = true;
                }
            }

            s_lastRefresh = now;
        }

        if (!s_haveFps && !(s_haveLatency && cfg.showDisplayLatency) && !s_haveInputLag
            && !s_haveFpsRange && !s_haveCpu && !s_haveGpu && !s_haveRam && !s_haveVram && !s_haveGpuPower)
            return;

        // Colors (and the flash animation) are recomputed every frame even
        // though the underlying text only refreshes twice a second, so the
        // white<->orange/red flash animates smoothly.
        // Each widthRef is the longest plausible real value for that segment
        // -- sized from these FIXED strings rather than the live text (see
        // StatSegment's comment), so the box's width and every segment's
        // position stay put regardless of which vsync phrase, digit count,
        // etc. actually shows this frame.
        StatSegment segs[10];
        int segCount = 0;
        // Which stats appear is chosen in the INI ([FrameLatency] Show* keys),
        // in this fixed order. Each has a fixed-width slot (widthRef) so the
        // box never resizes or shifts as digits change.
        if (s_haveFps)
        {
            segs[segCount++] = { s_fpsBuf, cfg.colorR, cfg.colorG, cfg.colorB, "FPS 999" };
            if (cfg.showFrameTime)
                segs[segCount++] = { s_frameBuf, cfg.colorR, cfg.colorG, cfg.colorB, "Frame 99.9 ms" };
        }
        if (s_haveFpsRange) segs[segCount++] = { s_fpsRangeBuf, cfg.colorR, cfg.colorG, cfg.colorB, "FPS 999-999 (1%low 999)" };
        if (s_haveCpu) segs[segCount++] = { s_cpuBuf, cfg.colorR, cfg.colorG, cfg.colorB, "CPU 99.9 ms" };
        if (s_haveGpu) segs[segCount++] = { s_gpuBuf, cfg.colorR, cfg.colorG, cfg.colorB, "GPU 99.9 ms" };
        if (s_haveLatency && cfg.showDisplayLatency) segs[segCount++] = { s_latencyBuf, cfg.colorR, cfg.colorG, cfg.colorB,
            s_latencyIsFallback ? nullptr : "Display Latency >100 ms" };
        if (s_haveInputLag) segs[segCount++] = { s_inputLagBuf, cfg.colorR, cfg.colorG, cfg.colorB, "Input Lag (est) 99 ms" };
        if (s_haveRam) segs[segCount++] = { s_ramBuf, cfg.colorR, cfg.colorG, cfg.colorB, "RAM 9999 MB" };
        if (s_haveVram) segs[segCount++] = { s_vramBuf, cfg.colorR, cfg.colorG, cfg.colorB, "VRAM 9999/9999 MB" };
        if (s_haveGpuPower) segs[segCount++] = { s_gpuPowerBuf, cfg.colorR, cfg.colorG, cfg.colorB, "GPU 999 W" };

        // cfg.scale is the layout-mode corner-handle multiplier. Folded in
        // HERE rather than applied to the drawn box, so every pixel value
        // downstream scales with it in exact proportion.
        const float uiScale = static_cast<float>(screenHeight) / 2160.0f * cfg.scale;
        if (cfg.alignBottomRightCorner)
        {
            // Flush against the screen's own bottom-right corner, padding
            // included.
            DrawSegmentedStatLine(cfg, segs, segCount, static_cast<float>(screenWidth), static_cast<float>(screenHeight), uiScale, true, false, true);
        }
        else if (cfg.alignTop)
        {
            // Flush against the top of the screen, horizontally centred.
            DrawSegmentedStatLine(cfg, segs, segCount, screenWidth * cfg.cpuGpuPosXFraction, 0.0f, uiScale, false, true, true);
        }
        else if (cfg.alignRightToPedalTrace)
        {
            // Same right-edge formula PedalTrace::Draw uses internally
            // (centerX + halfWidth) -- kept in sync here rather than having
            // PedalTrace expose it, since it's one line of arithmetic from
            // config already visible to this file.
            const PedalConfig& pedalCfg = g_pedalConfig;
            const float pedalRightEdgeX = screenWidth * pedalCfg.centerXFraction + screenWidth * pedalCfg.widthFraction * 0.5f;
            DrawSegmentedStatLine(cfg, segs, segCount, pedalRightEdgeX, screenHeight * cfg.cpuGpuPosYFraction, uiScale, true, false, true);
        }
        else
        {
            DrawSegmentedStatLine(cfg, segs, segCount,
                                 screenWidth * cfg.cpuGpuPosXFraction, screenHeight * cfg.cpuGpuPosYFraction, uiScale,
                                 false, false, true);
        }
    }

    // Per-panel vertex counts, logged every 30 s: which panel fills the
    // vertex buffer (the cost shows up as "Renderer upload" in the DLL-cost
    // panel, not under the panel that queued the geometry).
    struct PanelVertices { const char* name; size_t shape; size_t text; };
    PanelVertices g_panelVerts[32];
    int g_panelVertCount = 0;

    struct VertexTally
    {
        const char* name;
        size_t shape0, text0;
        VertexTally(const char* n) : name(n), shape0(g_renderer.ShapeVertexCount()), text0(g_renderer.TextVertexCount()) {}
        ~VertexTally()
        {
            if (g_panelVertCount < 32)
                g_panelVerts[g_panelVertCount++] = { name, g_renderer.ShapeVertexCount() - shape0,
                                                     g_renderer.TextVertexCount() - text0 };
        }
    };

    void LogPanelVertices()
    {
        static ULONGLONG s_last = 0;
        const ULONGLONG nowMs = GetTickCount64();
        if (g_inRealtime && nowMs - s_last > 30000)
        {
            s_last = nowMs;
            char line[1024];
            int len = sprintf_s(line, "Renderer: vertices by panel (shape/text):");
            for (int i = 0; i < g_panelVertCount && len > 0 && len < 900; ++i)
                len += sprintf_s(line + len, sizeof(line) - len, " %s %u/%u", g_panelVerts[i].name,
                                 static_cast<UINT>(g_panelVerts[i].shape), static_cast<UINT>(g_panelVerts[i].text));
            Log::Write("%s", line);
        }
        g_panelVertCount = 0;
    }

// A panel's draw: timed (PROF) and its vertices counted (VertexTally).
#define DRAWN(name) PROF(name); VertexTally PROF_CONCAT(prof_verts_, __LINE__)(name)

    void PresentBody(IDXGISwapChain* swapChain)
    {
        DXGI_SWAP_CHAIN_DESC desc;
        if (FAILED(swapChain->GetDesc(&desc)))
            return;

        // Ignore tiny/offscreen swapchains (CEF UI surfaces etc.) -- only draw on
        // something plausibly the main game view.
        if (desc.BufferDesc.Width < 640 || desc.BufferDesc.Height < 480)
            return;

        // DLL-cost panel (Profiler.h): count the frame, and time the D3D
        // setup up to the first widget as its own entry.
        Profiler::FrameTick();
        static const int s_setupSlot = Profiler::Slot("D3D setup");
        Profiler::Scope setupTiming(s_setupSlot);

        // Step logging for the first few frames only, so a fault is pinpointed
        // without spamming the log for the rest of the session.
        static int s_traceFrames = 0;
        const bool trace = (s_traceFrames < 3);
        if (trace) Log::Write("  step 1: desc %ux%u", desc.BufferDesc.Width, desc.BufferDesc.Height);

        if (!BindTo(swapChain))
            return;
        if (trace) Log::Write("  step 2: bound OK");

        // Fresh RTV every frame -- correct for both bitblt and flip swap models.
        ID3D11Texture2D* backBuffer = nullptr;
        if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) || !backBuffer)
            return;
        if (trace) Log::Write("  step 3: got backbuffer");

        g_rtv = nullptr;
        HRESULT hr = g_device->CreateRenderTargetView(backBuffer, nullptr, &g_rtv);
        backBuffer->Release();

        if (FAILED(hr) || !g_rtv)
        {
            if (trace) Log::Write("  step 4: CreateRenderTargetView FAILED hr=0x%08X", hr);
            return;
        }
        if (trace) Log::Write("  step 4: RTV created");

        D3DStateBackup state;
        SaveState(g_context, state);
        if (trace) Log::Write("  step 5: state saved");

        // One frame, both widgets. EndFrame is a no-op if neither drew anything.
        const int sw = static_cast<int>(desc.BufferDesc.Width);
        const int sh = static_cast<int>(desc.BufferDesc.Height);
        g_renderer.BeginFrame(g_context, g_rtv, sw, sh);
        setupTiming.Stop();
        // Driving HUD only makes sense while actually driving -- the session
        // monitor and menus still fire UpdateTelemetry/UpdateScoring with
        // stale or placeholder data (e.g. gear "N", speed 0) that would
        // otherwise show up over the monitor/UI screens.
        {
            PROF("Shortcuts");
            Shortcuts::Poll();
            CarLayout::Poll(); // after Shortcuts::Poll: needs the toggles loaded
            Shortcuts::NoteRealtime(g_inRealtime);
        }

        // Diagnostics are driven from HERE, the one path known to run during
        // a standalone replay, rather than from the scoring callback, which
        // does not fire there at all.

        // Text plate style is pushed once per frame from config, then left
        // on -- widgets that paint their own panel switch it off around their
        // own text and switch it back.
        {
            const TextBackgroundConfig& tb = g_textBackgroundConfig;
            const float uiScale = static_cast<float>(sh) / 2160.0f;
            g_renderer.SetTextBackgroundStyle(tb.r, tb.g, tb.b, tb.alpha,
                                              tb.paddingPx * uiScale,
                                              tb.cornerRadiusPx * uiScale);
            g_renderer.SetTextBackground(tb.enabled);
        }

        // Marks every remembered panel unseen; each panel re-registers as it
        // draws. Deliberately not a clear -- see the note in Layout.h.
        Layout::BeginFrame(sw, sh);

        // Master switch, checked once around the whole overlay rather than
        // per panel. Its own toast still draws (below) -- otherwise the key
        // that turned everything off could never be found again.
        if (Shortcuts::OverlayEnabled())
        {
            // Driving widgets only draw while actually in the car -- layout
            // mode is deliberately used FROM THE COCKPIT (see Layout.cpp),
            // not from the monitor/garage. Out of the car, only the perf
            // overlay below stays up, as the one "the plugin is alive" sign.
            if (g_inRealtime)
            {
                // Each panel timed for the DLL-cost panel (Profiler.h).
                // Cover boxes first: they hide the car's own display, and
                // every panel of ours draws on top of them.
                if (Shortcuts::Visible(Shortcuts::CoverBoxes))
                    { DRAWN("Cover boxes"); CoverBox::Draw(g_renderer, sw, sh); }
                if (Shortcuts::Visible(Shortcuts::Radar))
                    { DRAWN("Radar"); DrawRadar(sw, sh); }
                if (Shortcuts::Visible(Shortcuts::SideBar))
                    { DRAWN("Side bars"); g_proximityHue.Draw(g_renderer, sw, sh, desc.OutputWindow); }
                if (Shortcuts::Visible(Shortcuts::PedalTrace))
                    { DRAWN("Pedal trace"); PedalTrace::Draw(g_renderer, sw, sh); }
                if (Shortcuts::Visible(Shortcuts::Gear))
                    { DRAWN("Gear + speed"); GearDisplay::Draw(g_renderer, sw, sh); }
                // Drawn if EITHER delta row is on -- LapDelta::Draw gates the two
                // rows individually (and the DRS indicator inside it is not tied
                // to either).
                if (Shortcuts::Visible(Shortcuts::LapDelta) || Shortcuts::Visible(Shortcuts::BestDelta))
                    { DRAWN("Lap delta"); LapDelta::Draw(g_renderer, sw, sh); }
                if (Shortcuts::Visible(Shortcuts::MiniSector))
                    { DRAWN("Mini-sectors"); MiniSector::Draw(g_renderer, sw, sh); }
                if (Shortcuts::Visible(Shortcuts::TyreTemps))
                    { DRAWN("Tyre temps"); TyreTemps::Draw(g_renderer, sw, sh); }
                if (Shortcuts::Visible(Shortcuts::FfbGraph))
                    { DRAWN("FFB graph"); FfbGraph::Draw(g_renderer, sw, sh); }
                if (Shortcuts::Visible(Shortcuts::TrackMap))
                {
                    DRAWN("Track map");
                    TrackMap::Draw(g_renderer, sw, sh);
                    TrackMap::DrawLayoutControls(g_renderer, sw, sh, desc.OutputWindow);
                }
                if (Shortcuts::Visible(Shortcuts::RelativeTiming))
                    { DRAWN("Relative"); RelativeTiming::Draw(g_renderer, sw, sh); }
                if (Shortcuts::Visible(Shortcuts::FuelTyre))
                    { DRAWN("Fuel + tyres"); FuelTyrePanel::Draw(g_renderer, sw, sh); }
                { DRAWN("Fuel save"); FuelSave::Draw(g_renderer, sw, sh); }
                { DRAWN("Tyre target"); TyreSave::Draw(g_renderer, sw, sh); }
                { DRAWN("Engine temps"); EngineTemps::Draw(g_renderer, sw, sh); }
                { DRAWN("FFB strength"); FfbLevel::Draw(g_renderer, sw, sh); }
                if (Shortcuts::Visible(Shortcuts::BrakeCue))
                    { DRAWN("Brake cue"); BrakeCue::Draw(g_renderer, sw, sh); }
            }
            // Frametime stats deliberately draw everywhere, menus included.
            if (Shortcuts::Visible(Shortcuts::Perf))
                { DRAWN("Frametime stats"); DrawPerfOverlay(sw, sh); }
            if (Shortcuts::Visible(Shortcuts::Clock))
                { DRAWN("Clock"); DrawClock(sw, sh); }
            // Times itself (as "DLL cost panel").
            if (Shortcuts::Visible(Shortcuts::DllCost))
                Profiler::Draw(g_renderer, sw, sh);
        }
        // After every panel has drawn (and registered), so the hit-test uses
        // this frame's real rectangles rather than the previous frame's.
        {
            PROF("Layout");
            Layout::Poll(desc.OutputWindow, sw, sh);
            Layout::Draw(g_renderer, sw, sh, g_inRealtime);
        }

        { PROF("Shortcuts"); Shortcuts::Draw(g_renderer, sw, sh, desc.OutputWindow); }
        // Every panel above only queues geometry; this is where it is all
        // uploaded and drawn (timed inside, upload and draw separately).
        LogPanelVertices();
        g_renderer.EndFrame();
        if (trace) Log::Write("  step 6: draw done");

        PROF("D3D setup"); // restore + release, to the end of the frame
        RestoreState(g_context, state);
        if (trace) Log::Write("  step 7: state restored");

        g_rtv->Release();
        g_rtv = nullptr;
        if (trace) { Log::Write("  step 8: frame complete"); ++s_traceFrames; }
    }
}

void Overlay::OnPresent(IDXGISwapChain* swapChain)
{
    if (g_permanentlyDisabled)
        return;

    if (!g_loggedFirstPresent)
    {
        g_loggedFirstPresent = true;
        Log::Write("OnPresent: first present seen, swapchain=%p", swapChain);
    }

    // Never let a fault in overlay code take the game down with it.
    __try
    {
        PresentBody(swapChain);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Previously this reset state and retried every frame, which re-created
        // D3D resources forever and eventually killed the process. Give up instead.
        const DWORD exceptionCode = GetExceptionCode();

        // Reset the stack guard page FIRST, before doing anything else --
        // including logging. A stack overflow consumes the guard page, and
        // the thread is left in an unsafe state (unable to detect the NEXT
        // overflow -- silently corrupting memory instead of faulting --
        // and on some CRT versions unable to safely run much further code
        // at all) until it's restored. Calling Log::Write or anything else
        // BEFORE this reset risks running on a stack that has no remaining
        // safety margin at all. This is the game's own render thread -- the
        // same one any other Present hook (RTSS notably) runs its own logic
        // on -- so leaving it in that state could plausibly break more than
        // just this overlay.
        if (exceptionCode == EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();

        ++g_faultCount;
        Log::Write("OnPresent: EXCEPTION #%d (code 0x%08X)", g_faultCount, exceptionCode);
        // Force this out to disk right now regardless of g_inRealtime --
        // losing the one diagnostic line that actually explains a crash to
        // an unflushed buffer would defeat the entire point of logging it.
        Log::FlushBuffered();

        // The normal path releases g_rtv itself once it's done drawing (see
        // the end of PresentBody) -- a fault caught between that RTV being
        // created and that release running would otherwise leak the COM
        // reference every time this handler fires.
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
        g_boundSwapChain = nullptr;
        g_rendererReady = false;

        if (g_faultCount >= 3)
        {
            g_permanentlyDisabled = true;
            Log::Write("OnPresent: 3 faults -- overlay permanently disabled, game left untouched");
        }
    }
}

namespace
{
    void OnResizeBody()
    {
        // RTVs are per-frame now, so nothing of ours holds a buffer reference
        // here. Just drop the cached binding so it re-derives after the resize.
        Log::Write("OnResize: dropping cached binding");
        g_boundSwapChain = nullptr;
    }
}

void Overlay::OnResize()
{
    if (g_permanentlyDisabled)
        return;

    // A resolution/display-mode change is exactly the moment the swapchain's
    // buffers and possibly the device itself are in flux -- unlike OnPresent,
    // this used to run completely unguarded, so any fault here (e.g. a stale
    // device reference caught mid-transition) would take the whole game down
    // with it instead of just disabling the overlay.
    __try
    {
        OnResizeBody();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        const DWORD exceptionCode = GetExceptionCode();

        // Reset the guard page FIRST -- see OnPresent's handler for why
        // this must happen before any other code runs, logging included.
        if (exceptionCode == EXCEPTION_STACK_OVERFLOW)
            _resetstkoflw();

        ++g_faultCount;
        Log::Write("OnResize: EXCEPTION #%d (code 0x%08X)", g_faultCount, exceptionCode);

        g_rtv = nullptr;
        g_boundSwapChain = nullptr;
        g_rendererReady = false;

        if (g_faultCount >= 3)
        {
            g_permanentlyDisabled = true;
            Log::Write("OnResize: 3 faults -- overlay permanently disabled, game left untouched");
        }
    }
}

ID3D11DeviceContext* Overlay::GetContext()
{
    return g_context;
}
