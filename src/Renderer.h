#pragma once
#include <d3d11.h>
#include <vector>

// Minimal immediate-mode 2D shape renderer, drawn directly into the game's own
// back buffer. No text, no images -- just colored triangles. Positions are
// given in screen pixels and converted to NDC internally.
class Renderer
{
public:
    bool Init(ID3D11Device* device);
    void Shutdown();

    void BeginFrame(ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, int screenWidth, int screenHeight);
    void EndFrame();

    void AddFilledCircle(float cx, float cy, float radius, float r, float g, float b, float a, int segments = 32);
    // Rectangle centred on (cx,cy), rotated clockwise by 'rotation' radians.
    void AddFilledRect(float cx, float cy, float halfWidth, float halfHeight, float rotation, float r, float g, float b, float a);
    // Upright quad with a true per-pixel alpha gradient in BOTH directions,
    // given as one alpha per corner (GPU-interpolated, so no banding however
    // large it is). The top and bottom edges take independent x extents, so
    // this draws a plain rect or a taper; only the colour is fixed.
    void AddGradientQuad(float leftTop, float rightTop,
                         float leftBottom, float rightBottom,
                         float top, float bottom,
                         float r, float g, float b,
                         float aTL, float aTR, float aBL, float aBR);
    // Same, but with rounded corners of the given radius (clamped to fit).
    void AddFilledRoundedRect(float cx, float cy, float halfWidth, float halfHeight, float cornerRadius, float rotation, float r, float g, float b, float a);
    void AddRing(float cx, float cy, float innerRadius, float outerRadius, float r, float g, float b, float a, int segments = 48);
    // Rectangular border only (4 thin bars), not filled.
    void AddRectOutline(float cx, float cy, float halfWidth, float halfHeight, float thickness, float r, float g, float b, float a);
    void AddLine(float x1, float y1, float x2, float y2, float thickness, float r, float g, float b, float a);
    // Same line, with a soft edge. The back buffer has no MSAA and this
    // renderer has no shader-side coverage term, so a diagonal thin line
    // otherwise shows hard stair-stepping. Drawn as a faint wider pass with
    // the solid core on top: the halo covers the pixels a true coverage
    // calculation would have partially lit, which reads as a smooth edge at
    // normal viewing distance for two extra quads.
    void AddLineSmooth(float x1, float y1, float x2, float y2, float thickness, float r, float g, float b, float a);
    // A thick line through 'count' points (xy pairs, screen pixels) drawn as
    // one continuous ribbon: neighbouring segments share mitred corner
    // vertices, so bends have no notches on the outside or overlaps on the
    // inside, as a run of separate AddLine quads would. Edges are feathered
    // by ~1px of alpha falloff for anti-aliasing. 'closed' joins the last
    // point back to the first. Points closer than a pixel apart should be
    // thinned out by the caller first. 'rgbPerPoint' (3 floats per point),
    // when given, overrides r/g/b point by point, blending along each segment.
    void AddPolylineSmooth(const float* xy, int count, bool closed, float thickness, float r, float g, float b, float a,
                           const float* rgbPerPoint = nullptr);

    enum class HAlign { Left, Center, Right };

    // Text is a separate textured batch (see EndFrame) -- shapes and text can
    // be freely interleaved in the same frame. 'pixelHeight' is the rendered
    // glyph height in screen pixels. ASCII 32..126 only.
    float MeasureText(const char* text, float pixelHeight) const;
    void AddText(float x, float y, const char* text, float pixelHeight, float r, float g, float b, float a, HAlign align = HAlign::Left);

    // Text font, chosen from kFontNames (0 = Auto: Consolas when installed,
    // else the built-in DejaVu Sans Mono) and kWeightNames. Call before
    // Init(), or any time after: the glyph atlas is rebuilt at the start of
    // the next frame. ActiveFontFace() is the face actually in use.
    static const int kFontCount = 5;
    static const char* const kFontNames[kFontCount];
    static const int kWeightCount = 3;
    static const char* const kWeightNames[kWeightCount];
    void SetFont(int font, int weight);
    const char* ActiveFontFace() const { return m_activeFace; }

    // Translucent plate drawn behind each AddText call, so text stays legible
    // over bright track, sky or a white car. Handled here rather than at each
    // call site so text added later cannot silently arrive without one.
    // Lands in the SHAPE batch, which EndFrame draws before the text batch,
    // so it is always underneath -- including under text from other widgets
    // drawn earlier in the same frame, which is why widgets that already
    // paint their own background panel turn this off around their text
    // instead of stacking two plates.
    void SetTextBackground(bool enabled) { m_textBackground = enabled; }
    bool TextBackgroundEnabled() const { return m_textBackground; }

    // Vertices queued so far this frame, for per-panel accounting.
    size_t ShapeVertexCount() const { return m_vertices.size(); }
    size_t TextVertexCount() const { return m_textVertices.size(); }
    void SetTextBackgroundStyle(float r, float g, float b, float a, float paddingPx, float cornerRadiusPx);

private:
    struct Vertex { float x, y; float r, g, b, a; };

    void PixelToNDC(float px, float py, float& ndcX, float& ndcY) const;
    void PushTriangle(const Vertex& a, const Vertex& b, const Vertex& c);

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    ID3D11VertexShader* m_vs = nullptr;
    ID3D11PixelShader* m_ps = nullptr;
    ID3D11InputLayout* m_inputLayout = nullptr;
    ID3D11Buffer* m_vertexBuffer = nullptr;
    ID3D11BlendState* m_blendState = nullptr;
    ID3D11RasterizerState* m_rasterState = nullptr;
    ID3D11DepthStencilState* m_depthState = nullptr;

    std::vector<Vertex> m_vertices;
    int m_screenWidth = 1;
    int m_screenHeight = 1;
    // Generous: at a 10s / 144Hz pedal trace, worst case is ~53k vertices
    // (4 lines x 1439 segments x 6 verts, plus slip strips). At 24 bytes each
    // this buffer is still only ~2.4MB.
    // Raised to 200k with the FFB spectrum panel: past the cap, the rest of
    // the frame's shapes (and a throttled widget's whole replay) are dropped,
    // which shows as panels flashing off. Overflow is now logged.
    static const UINT kMaxVertices = 200000;
    UINT m_droppedVertices = 0;

    // --- Text: separate pipeline, textured alpha-only glyph atlas ---
    struct TextVertex { float x, y; float u, v; float r, g, b, a; };
    struct Glyph { float u0, v0, u1, v1; float advancePx; };

    bool InitText();
    bool BuildFontAtlas();
    int m_fontChoice = 0;
    int m_fontWeight = 0;
    bool m_fontReloadPending = false;
    char m_activeFace[64] = "";
    void ShutdownText();
    void PushGlyphQuad(float x, float y, float w, float h, const Glyph& g, float r, float gg, float b, float a);

    ID3D11VertexShader* m_textVs = nullptr;
    ID3D11PixelShader* m_textPs = nullptr;
    ID3D11InputLayout* m_textInputLayout = nullptr;
    ID3D11Buffer* m_textVertexBuffer = nullptr;
    ID3D11ShaderResourceView* m_fontSRV = nullptr;
    ID3D11SamplerState* m_fontSampler = nullptr;
    // Reuses m_blendState (already alpha blending) -- no separate state needed.

    bool m_textBackground = false;
    float m_textBgR = 0.04f, m_textBgG = 0.04f, m_textBgB = 0.04f, m_textBgA = 0.55f;
    float m_textBgPaddingPx = 6.0f;
    float m_textBgCornerRadiusPx = 5.0f;

    std::vector<TextVertex> m_textVertices;
    // 6 vertices/character. With every text widget now enabled at once (perf
    // overlay, relative-timing table at its full 9 rows, fuel/tyre panel,
    // gear/speed dashboard incl. the PIT box and shift stats, lap delta incl.
    // the DRS indicator) a realistic worst-case frame is a few thousand
    // characters short of this -- comfortable today, but tight enough that a
    // widget added later could start silently dropping glyphs past the cap
    // (PushGlyphQuad just stops appending, no error). Sized with real
    // headroom instead of exactly fitting current usage; 32 bytes/vertex
    // makes even this a rounding error in GPU memory.
    static const UINT kMaxTextVertices = 20000;

    static const int kGlyphFirst = 32;   // ' '
    static const int kGlyphCount = 95;   // through '~' (126)
    // Three glyph sets (small / medium / large), one texture -- see
    // BuildFontAtlas. m_setHeight is each set's cell height in atlas pixels.
    static const int kFontSets = 3;
    Glyph m_glyphs[kFontSets][kGlyphCount];
    float m_setHeight[kFontSets] = { 32.0f, 64.0f, 160.0f };
    int SetFor(float pixelHeight) const;

public:
    // Lets a widget throttled below full frame rate (see the tier notes on
    // each widget's own Draw()) skip regenerating its geometry on frames it
    // isn't due to update, by replaying what it drew last time instead. The
    // widget itself decides WHEN to use these (its own elapsed-time check);
    // this is just the copy-out/copy-back plumbing.
    struct Cache
    {
        std::vector<Vertex> shapeVerts;
        std::vector<TextVertex> textVerts;
        bool valid = false;
    };
    struct CaptureMark { size_t shapeCount; size_t textCount; };

    // Call right before running normal draw logic...
    CaptureMark BeginCapture() const;
    // ...then this right after, to copy out everything added in between.
    void EndCapture(const CaptureMark& mark, Cache& outCache) const;
    // On a throttled-out frame, call this INSTEAD of the normal draw logic
    // to re-append the last captured geometry unchanged.
    void Replay(const Cache& cache);
};
