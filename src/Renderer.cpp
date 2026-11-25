#include "Renderer.h"
#include "FontResources.h"
#include "Log.h"
#include "Profiler.h"
#include <d3dcompiler.h>
#include <Windows.h>
#include <cmath>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

const char* const Renderer::kFontNames[Renderer::kFontCount] = {
    "Auto", "Consolas", "DejaVu Sans Mono", "Lucida Console", "Courier New" };
const char* const Renderer::kWeightNames[Renderer::kWeightCount] = { "Light", "Normal", "Bold" };

namespace
{
    // Registers the fonts built into this DLL (Fonts.rc) with GDI, private to
    // the game process, so CreateFont can find them by name. Once only.
    void LoadEmbeddedFonts()
    {
        static bool s_done = false;
        if (s_done) return;
        s_done = true;
        HMODULE self = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(&LoadEmbeddedFonts), &self))
            return;
        const int ids[] = { IDR_FONT_DEJAVU_MONO, IDR_FONT_DEJAVU_MONO_BOLD };
        for (int id : ids)
        {
            HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(id), MAKEINTRESOURCEA(10) /* RT_RCDATA */);
            HGLOBAL mem = res ? LoadResource(self, res) : nullptr;
            void* data = mem ? LockResource(mem) : nullptr;
            const DWORD size = res ? SizeofResource(self, res) : 0;
            DWORD added = 0;
            if (!data || !size || !AddFontMemResourceEx(data, size, nullptr, &added))
                Log::Write("Renderer: could not load built-in font %d", id);
        }
    }

    // A font of exactly this face, or null if GDI would substitute another
    // (it never fails outright -- Wine/Proton without Consolas silently
    // hands back some other font, which is what made text unreadable there).
    HFONT TryFont(HDC dc, const char* face, int height, int weight)
    {
        HFONT font = CreateFontA(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, face);
        if (!font) return nullptr;
        HFONT old = static_cast<HFONT>(SelectObject(dc, font));
        char got[LF_FACESIZE] = "";
        GetTextFaceA(dc, LF_FACESIZE, got);
        SelectObject(dc, old);
        if (_stricmp(got, face) != 0)
        {
            DeleteObject(font);
            return nullptr;
        }
        return font;
    }

    // Compiled shader bytecode, kept for the life of the process. It does not
    // depend on the device, so a rebind to a new device creates the shader
    // objects straight from it instead of running the HLSL compiler again
    // (a resize no longer rebuilds the renderer at all -- see Overlay.cpp's
    // BindTo). Never released: four small blobs, and releasing them from a
    // static destructor at unload would call into d3dcompiler at a moment
    // that is not guaranteed safe. Render thread only.
    enum ShaderId { kShapeVS, kShapePS, kTextVS, kTextPS, kShaderCount };
    ID3DBlob* g_shaderBlobs[kShaderCount] = {};

    // Borrowed pointer (the cache keeps it), or null if compiling failed --
    // a failure is not cached, so the next Init tries again.
    ID3DBlob* CompiledShader(ShaderId id, const char* src, const char* entry, const char* target)
    {
        if (!g_shaderBlobs[id])
        {
            ID3DBlob* blob = nullptr;
            ID3DBlob* errBlob = nullptr;
            const HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, 0, 0, &blob, &errBlob);
            if (errBlob) errBlob->Release();
            if (FAILED(hr))
            {
                if (blob) blob->Release();
                return nullptr;
            }
            g_shaderBlobs[id] = blob;
        }
        return g_shaderBlobs[id];
    }

    // AddFilledRoundedRect's arc points: 5 per corner over 90 degrees, at
    // the same fixed angles every call, so their cos/sin are worked out once
    // (with exactly the expressions the draw used to evaluate each time --
    // 20 sinf/cosf pairs per box, and every text plate is one).
    const int kArcPerCorner = 5;
    const int kArcPoints = kArcPerCorner * 4;
    struct ArcTable
    {
        float cosA[kArcPoints];
        float sinA[kArcPoints];
        ArcTable()
        {
            // Start angle for each corner, sweeping 90 degrees each.
            const float start[4] = { -1.57079633f, 0.0f, 1.57079633f, 3.14159265f };
            int n = 0;
            for (int c = 0; c < 4; ++c)
            {
                for (int i = 0; i < kArcPerCorner; ++i)
                {
                    const float t = static_cast<float>(i) / static_cast<float>(kArcPerCorner - 1);
                    const float ang = start[c] + t * 1.57079633f;
                    cosA[n] = cosf(ang);
                    sinA[n] = sinf(ang);
                    ++n;
                }
            }
        }
    };
    const ArcTable kArc;
}

namespace
{
    // Game strings (driver/track names) arrive as UTF-8 or Latin-1, but the
    // atlas only has ASCII 32..126, so accented letters were dropped. Fold
    // them to plain ASCII, keeping case (review F150). Static table, no
    // allocation; MeasureText and AddText share it so width and draw agree.
    // Index = code point - 0xC0 (U+00C0..U+00FF); "" = drop.
    const char* const kLatin1Fold[64] = {
        "A","A","A","A","A","A","AE","C", "E","E","E","E","I","I","I","I",
        "D","N","O","O","O","O","O","x", "O","U","U","U","U","Y","TH","ss",
        "a","a","a","a","a","a","ae","c", "e","e","e","e","i","i","i","i",
        "d","n","o","o","o","o","o","",  "o","u","u","u","u","y","th","y" };

    // Reads one character at p, advances p past it, and returns its ASCII
    // fold ("" if it has none). Valid UTF-8 is decoded first; any other
    // high byte is taken as Latin-1. Never reads past the terminator.
    const char* NextFolded(const char*& p, char (&one)[2])
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x80) { ++p; one[0] = static_cast<char>(c); one[1] = 0; return one; }
        int len = (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3
                : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        for (int i = 1; i < len; ++i)
            if ((static_cast<unsigned char>(p[i]) & 0xC0) != 0x80) { len = 0; break; }
        if (len == 2)
        {
            const unsigned cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(p[1]) & 0x3Fu);
            p += 2;
            return cp >= 0xC0 ? kLatin1Fold[cp - 0xC0] : "";
        }
        if (len > 2) { p += len; return ""; }   // other scripts: no glyph
        ++p;                                    // lone byte: Latin-1
        return c >= 0xC0 ? kLatin1Fold[c - 0xC0] : "";
    }
}

namespace
{
    // float -> IEEE half, round to nearest even, with infinities, NaN and
    // subnormals handled (the well-known branch-light conversion; FP16C
    // instructions would need a CPU check). The GPU widens halves back to
    // floats exactly.
    UINT64 FloatToHalf(float value)
    {
        UINT32 x;
        memcpy(&x, &value, sizeof(x));
        const UINT32 sign = x & 0x80000000u;
        x ^= sign;
        UINT32 h;
        if (x >= (127u + 16u) << 23)                    // too big for a half: Inf, or NaN
        {
            h = (x > 0x7F800000u) ? 0x7E00u : 0x7C00u;
        }
        else if (x < (113u << 23))                      // half subnormal or zero
        {
            const UINT32 magicBits = ((127u - 15u) + (23u - 10u) + 1u) << 23;
            float magic, f;
            memcpy(&magic, &magicBits, sizeof(magic));
            memcpy(&f, &x, sizeof(f));
            f += magic;                                 // the FPU's own rounding does the work
            UINT32 bits;
            memcpy(&bits, &f, sizeof(bits));
            h = bits - magicBits;
        }
        else
        {
            const UINT32 mantOdd = (x >> 13) & 1u;
            x += (static_cast<UINT32>(15 - 127) << 23) + 0xFFFu;
            x += mantOdd;
            h = x >> 13;
        }
        return static_cast<UINT64>((h | (sign >> 16)) & 0xFFFFu);
    }
}

// Each channel as a half float (R16G16B16A16_FLOAT). Not 8-bit UNORM, which
// would be smaller still: callers do pass values above 1 (the side bars'
// corner alphas with SideBarOpacityMultiplier above 1), and those must reach
// the GPU unclamped -- the blend clamps per PIXEL, after interpolation, which
// is what gives an over-1 gradient its solid band. Halves keep that, and
// their ~11 bits are finer than the 8-bit back buffer.
UINT64 Renderer::PackColor(float r, float g, float b, float a)
{
    return FloatToHalf(r) | (FloatToHalf(g) << 16) | (FloatToHalf(b) << 32) | (FloatToHalf(a) << 48);
}

void Renderer::SetFont(int font, int weight)
{
    if (font < 0 || font >= kFontCount) font = 0;
    if (weight < 0 || weight >= kWeightCount) weight = 0;
    if (font == m_fontChoice && weight == m_fontWeight) return;
    m_fontChoice = font;
    m_fontWeight = weight;
    m_fontReloadPending = true;
}

static const char* kTextShaderSrc = R"(
struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR; };
struct PSIn  { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR; };

PSIn VSMain(VSIn input)
{
    PSIn output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    output.col = input.col;
    return output;
}

Texture2D fontTex : register(t0);
SamplerState fontSampler : register(s0);

float4 PSMain(PSIn input) : SV_TARGET
{
    float a = fontTex.Sample(fontSampler, input.uv).r;
    return float4(input.col.rgb, input.col.a * a);
}
)";

static const char* kShaderSrc = R"(
struct VSIn  { float2 pos : POSITION; float4 col : COLOR; };
struct PSIn  { float4 pos : SV_POSITION; float4 col : COLOR; };

PSIn VSMain(VSIn input)
{
    PSIn output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.col = input.col;
    return output;
}

float4 PSMain(PSIn input) : SV_TARGET
{
    return input.col;
}
)";

bool Renderer::Init(ID3D11Device* device)
{
    // A re-Init without Shutdown (after a caught fault) would otherwise leak
    // every object below and keep stale static batches (review F155).
    Shutdown();
    m_device = device;

    // Borrowed from the process-wide cache (see CompiledShader) -- not released here.
    ID3DBlob* vsBlob = CompiledShader(kShapeVS, kShaderSrc, "VSMain", "vs_4_0");
    if (!vsBlob)
        return false;
    ID3DBlob* psBlob = CompiledShader(kShapePS, kShaderSrc, "PSMain", "ps_4_0");
    if (!psBlob)
        return false;

    // Every one of these HRESULTs used to go unchecked -- if any failed
    // (plausible right when the device is in flux from a resolution/mode
    // change, which is exactly when this whole function gets re-run, since
    // OnResize forces a fresh rebind), the corresponding pointer stayed
    // null while Init() still reported success. The very next EndFrame()
    // then called Map() on a null m_vertexBuffer -- a guaranteed access
    // violation, and a very plausible source of "crashes on resolution/
    // window-mode change" reports. Bail out and release whatever did
    // succeed (via Shutdown(), which already null-checks each member) the
    // moment anything fails, so BindTo() sees this as a clean failure and
    // skips drawing that frame instead of running with null resources.
    bool ok = true;
    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vs)))
        ok = false;
    if (FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_ps)))
        ok = false;

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout)))
        ok = false;

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.ByteWidth = sizeof(Vertex) * kMaxVertices;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&vbDesc, nullptr, &m_vertexBuffer)))
        ok = false;

    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blendDesc, &m_blendState)))
        ok = false;

    D3D11_RASTERIZER_DESC rastDesc = {};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;
    rastDesc.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rastDesc, &m_rasterState)))
        ok = false;

    D3D11_DEPTH_STENCIL_DESC depthDesc = {};
    depthDesc.DepthEnable = FALSE;
    depthDesc.StencilEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&depthDesc, &m_depthState)))
        ok = false;

    if (!ok)
    {
        Shutdown();
        return false;
    }

    m_vertices.reserve(kMaxVertices);

    if (!InitText())
    {
        Shutdown();
        return false;
    }

    return true;
}

void Renderer::Shutdown()
{
    ShutdownText();
    for (StaticBatch* b : m_batches)
    {
        if (b->buf) { b->buf->Release(); b->buf = nullptr; }
        b->capacity = 0;
        b->count = 0;
        b->valid = false;
    }
    m_batches.clear();
    m_vbPos = 0;
    m_textVbPos = 0;
    m_inserts.clear();

    // Null after release -- otherwise a second Shutdown (or Init-after-Shutdown)
    // double-releases and crashes.
    if (m_depthState) { m_depthState->Release(); m_depthState = nullptr; }
    if (m_rasterState) { m_rasterState->Release(); m_rasterState = nullptr; }
    if (m_blendState) { m_blendState->Release(); m_blendState = nullptr; }
    if (m_vertexBuffer) { m_vertexBuffer->Release(); m_vertexBuffer = nullptr; }
    if (m_inputLayout) { m_inputLayout->Release(); m_inputLayout = nullptr; }
    if (m_ps) { m_ps->Release(); m_ps = nullptr; }
    if (m_vs) { m_vs->Release(); m_vs = nullptr; }
    m_device = nullptr;
    m_context = nullptr;   // not used below: it may belong to a dead device
    RestoreGameSlots();    // so this only drops the saved references
    ++m_generation;        // throttle caches don't outlive a rebuild
}

void Renderer::BeginTopLayer()
{
    // Capped so a runaway caller can't grow it; extra calls merge into the top layer.
    if (m_layers.size() < 32)
        m_layers.push_back({ m_vertices.size(), m_textVertices.size() });
}

void Renderer::RestoreGameSlots()
{
    if (m_context)
    {
        m_context->PSSetShaderResources(0, 1, &m_savedSRV);
        m_context->PSSetSamplers(0, 1, &m_savedSampler);
        m_context->HSSetShader(m_savedHS, nullptr, 0);
        m_context->DSSetShader(m_savedDS, nullptr, 0);
    }
    if (m_savedSRV) { m_savedSRV->Release(); m_savedSRV = nullptr; }
    if (m_savedSampler) { m_savedSampler->Release(); m_savedSampler = nullptr; }
    if (m_savedHS) { m_savedHS->Release(); m_savedHS = nullptr; }
    if (m_savedDS) { m_savedDS->Release(); m_savedDS = nullptr; }
}

void Renderer::BeginFrame(ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv, int screenWidth, int screenHeight)
{
    m_context = context;
    const int width = screenWidth > 0 ? screenWidth : 1;
    const int height = screenHeight > 0 ? screenHeight : 1;
    if (width != m_screenWidth || height != m_screenHeight)
    {
        // Static batches hold NDC positions, only right at the size they were
        // built for. A resize used to rebuild the whole renderer, which
        // dropped them; it no longer does, so they are marked stale here and
        // their widgets rebuild them (into the same GPU buffers) this frame.
        for (StaticBatch* b : m_batches)
            b->valid = false;
        ++m_generation;   // throttle caches hold old-size positions too (F374)
    }
    m_screenWidth = width;
    m_screenHeight = height;
    m_ndcScaleX = 2.0f / static_cast<float>(m_screenWidth);
    m_ndcScaleY = 2.0f / static_cast<float>(m_screenHeight);
    m_vertices.clear();
    m_textVertices.clear();
    m_inserts.clear();
    m_layers.clear();

    // A font change from the menu: rebuild the glyph atlas before anything
    // is drawn with it. Keeps the old atlas if the new one fails.
    if (m_fontReloadPending && m_fontSRV)
    {
        m_fontReloadPending = false;
        if (!BuildFontAtlas())
            Log::Write("Renderer: font change failed, keeping %s", m_activeFace);
        else
            ++m_generation;   // cached text holds the old atlas's UVs (F375)
    }

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(m_screenWidth);
    vp.Height = static_cast<float>(m_screenHeight);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);

    context->OMSetRenderTargets(1, &rtv, nullptr);

    float blendFactor[4] = { 0, 0, 0, 0 };
    context->OMSetBlendState(m_blendState, blendFactor, 0xffffffff);
    context->RSSetState(m_rasterState);
    context->OMSetDepthStencilState(m_depthState, 0);
    context->IASetInputLayout(m_inputLayout);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(m_vs, nullptr, 0);
    context->PSSetShader(m_ps, nullptr, 0);

    // Review F152: the text pass binds PS t0/s0, so keep the game's to put
    // back in EndFrame; and a leftover hull/domain/geometry shader would make
    // our draws vanish, so clear them (Overlay's SaveState restores GS).
    // References left over from a frame that faulted before EndFrame are dropped.
    if (m_savedSRV) { m_savedSRV->Release(); m_savedSRV = nullptr; }
    if (m_savedSampler) { m_savedSampler->Release(); m_savedSampler = nullptr; }
    if (m_savedHS) { m_savedHS->Release(); m_savedHS = nullptr; }
    if (m_savedDS) { m_savedDS->Release(); m_savedDS = nullptr; }
    context->PSGetShaderResources(0, 1, &m_savedSRV);
    context->PSGetSamplers(0, 1, &m_savedSampler);
    context->HSGetShader(&m_savedHS, nullptr, nullptr);
    context->DSGetShader(&m_savedDS, nullptr, nullptr);
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
}

void Renderer::EndFrame()
{
    if (m_droppedVertices > 0)
    {
        // At most every 10 s, so a sustained overflow can't flood the log.
        static ULONGLONG s_lastLog = 0;
        const ULONGLONG nowMs = GetTickCount64();
        if (nowMs - s_lastLog > 10000)
        {
            s_lastLog = nowMs;
            Log::Write("Renderer: vertex buffer full (%u used) -- %u vertices dropped this frame",
                       static_cast<UINT>(m_vertices.size()), m_droppedVertices);
        }
        m_droppedVertices = 0;
    }
    if (m_droppedText > 0)
    {
        static ULONGLONG s_lastTextLog = 0;
        const ULONGLONG nowMs = GetTickCount64();
        if (nowMs - s_lastTextLog > 10000)
        {
            s_lastTextLog = nowMs;
            Log::Write("Renderer: text buffer full (%u used) -- %u text vertices dropped this frame",
                       static_cast<UINT>(m_textVertices.size()), m_droppedText);
        }
        m_droppedText = 0;
    }
    // Text or static batches alone still draw (review F333).
    if (m_vertices.empty() && m_textVertices.empty() && m_inserts.empty())
    {
        RestoreGameSlots();
        return;
    }

    UINT count = static_cast<UINT>(m_vertices.size());
    if (count > kMaxVertices)
        count = kMaxVertices;

    // How much geometry a frame carries, for reading the DLL-cost panel's
    // "Renderer upload"/"Renderer draw" entries. Every 30 s at most.
    {
        static ULONGLONG s_lastSizeLog = 0;
        const ULONGLONG nowMs = GetTickCount64();
        if (nowMs - s_lastSizeLog > 30000)
        {
            s_lastSizeLog = nowMs;
            Log::Write("Renderer: %u shape vertices (%u KB), %u text vertices (%u KB) this frame",
                       count, static_cast<UINT>(count * sizeof(Vertex) / 1024),
                       static_cast<UINT>(m_textVertices.size()),
                       static_cast<UINT>(m_textVertices.size() * sizeof(TextVertex) / 1024));
        }
    }

    // Timed in two parts for the DLL-cost panel (Profiler.h): handing the
    // vertices to the driver, and the draw calls.
    D3D11_MAPPED_SUBRESOURCE mapped;
    bool textReady = false;
    UINT textCount = 0;
    UINT base = 0, textBase = 0;   // where this frame's vertices start in each ring
    bool shapesReady = false;
    {
        PROF("Renderer upload");
        if (count > 0)
        {
            const bool wrap = m_vbPos + count > kMaxVertices;
            if (wrap) m_vbPos = 0;
            if (SUCCEEDED(m_context->Map(m_vertexBuffer, 0, wrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped)))
            {
                memcpy(static_cast<Vertex*>(mapped.pData) + m_vbPos, m_vertices.data(), count * sizeof(Vertex));
                m_context->Unmap(m_vertexBuffer, 0);
                base = m_vbPos;
                m_vbPos += count;
                shapesReady = true;
            }
        }
        if (!m_textVertices.empty() && m_fontSRV)
        {
            textCount = static_cast<UINT>(m_textVertices.size());
            if (textCount > kMaxTextVertices)
                textCount = kMaxTextVertices;

            D3D11_MAPPED_SUBRESOURCE textMapped;
            const bool wrap = m_textVbPos + textCount > kMaxTextVertices;
            if (wrap) m_textVbPos = 0;
            if (SUCCEEDED(m_context->Map(m_textVertexBuffer, 0, wrap ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &textMapped)))
            {
                memcpy(static_cast<TextVertex*>(textMapped.pData) + m_textVbPos, m_textVertices.data(), textCount * sizeof(TextVertex));
                m_context->Unmap(m_textVertexBuffer, 0);
                textBase = m_textVbPos;
                m_textVbPos += textCount;
                textReady = true;
            }
        }
    }

    PROF("Renderer draw");
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    // Shapes [from, to) in order, with any static batches drawn at the
    // point they were added.
    auto drawShapes = [&](UINT from, UINT to, bool upper)
    {
        if (upper)
        {
            m_context->IASetInputLayout(m_inputLayout);
            m_context->VSSetShader(m_vs, nullptr, 0);
            m_context->PSSetShader(m_ps, nullptr, 0);
        }
        m_context->IASetVertexBuffers(0, 1, &m_vertexBuffer, &stride, &offset);
        UINT start = from;
        for (const StaticInsert& ins : m_inserts)
        {
            const UINT at = ins.at > count ? count : static_cast<UINT>(ins.at);
            // A batch added exactly at the split was added before
            // BeginTopLayer: it belongs to the lower range.
            if (at > to || (upper && at <= from)) continue;
            if (at > start && shapesReady) m_context->Draw(at - start, base + start);
            start = at > start ? at : start;
            StaticBatch* b = ins.batch;
            if (b && b->valid && b->buf && b->count)
            {
                m_context->IASetVertexBuffers(0, 1, &b->buf, &stride, &offset);
                m_context->Draw(b->count, 0);
                m_context->IASetVertexBuffers(0, 1, &m_vertexBuffer, &stride, &offset);
            }
        }
        if (to > start && shapesReady) m_context->Draw(to - start, base + start);
    };
    // Text [from, to): same blend/raster/depth state (still alpha blending,
    // no depth, no culling), different shaders/input-layout/texture.
    auto drawText = [&](UINT from, UINT to)
    {
        if (!textReady || to <= from)
            return;
        UINT textStride = sizeof(TextVertex);
        UINT textOffset = 0;
        m_context->IASetInputLayout(m_textInputLayout);
        m_context->IASetVertexBuffers(0, 1, &m_textVertexBuffer, &textStride, &textOffset);
        m_context->VSSetShader(m_textVs, nullptr, 0);
        m_context->PSSetShader(m_textPs, nullptr, 0);
        m_context->PSSetShaderResources(0, 1, &m_fontSRV);
        m_context->PSSetSamplers(0, 1, &m_fontSampler);
        m_context->Draw(to - from, textBase + from);
    };
    // Layers in order, each one's shapes then its text (BeginTopLayer).
    UINT shapeFrom = 0, textFrom = 0;
    for (size_t li = 0; li <= m_layers.size(); ++li)
    {
        UINT shapeTo = count, textTo = textCount;
        if (li < m_layers.size())
        {
            shapeTo = m_layers[li].shape < count ? static_cast<UINT>(m_layers[li].shape) : count;
            textTo = m_layers[li].text < textCount ? static_cast<UINT>(m_layers[li].text) : textCount;
            if (shapeTo < shapeFrom) shapeTo = shapeFrom;
            if (textTo < textFrom) textTo = textFrom;
        }
        drawShapes(shapeFrom, shapeTo, li > 0);
        drawText(textFrom, textTo);
        shapeFrom = shapeTo;
        textFrom = textTo;
    }
    RestoreGameSlots();
}

void Renderer::PushTriangle(const Vertex& a, const Vertex& b, const Vertex& c)
{
    if (m_vertices.size() + 3 > kMaxVertices)
    {
        m_droppedVertices += 3;
        m_droppedTotal += 3;
        return;
    }
    m_vertices.push_back(a);
    m_vertices.push_back(b);
    m_vertices.push_back(c);
}

void Renderer::EndStatic(size_t mark, StaticBatch& b)
{
    if (mark > m_vertices.size() || !m_device || !m_context) { b.valid = false; return; }
    // Cut short by an overflow: don't keep a batch with gaps for the rest of
    // the session. Its vertices stay in this frame's stream; the widget
    // rebuilds it next frame (review F156).
    if (m_droppedTotal != m_staticDropMark) { b.valid = false; return; }
    const UINT n = static_cast<UINT>(m_vertices.size() - mark);
    if (!b.buf || n > b.capacity)
    {
        if (b.buf) { b.buf->Release(); b.buf = nullptr; }
        const UINT cap = (n + n / 4) > 1024 ? (n + n / 4) : 1024;
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = static_cast<UINT>(sizeof(Vertex)) * cap;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        if (FAILED(m_device->CreateBuffer(&d, nullptr, &b.buf)))
        {
            // Leave the vertices in the normal per-frame stream instead.
            b.buf = nullptr;
            b.capacity = 0;
            b.valid = false;
            return;
        }
        b.capacity = cap;
        bool known = false;
        for (StaticBatch* x : m_batches) if (x == &b) known = true;
        if (!known) m_batches.push_back(&b);
    }
    if (n)
    {
        D3D11_BOX box = { 0, 0, 0, n * static_cast<UINT>(sizeof(Vertex)), 1, 1 };
        m_context->UpdateSubresource(b.buf, 0, &box, m_vertices.data() + mark, 0, 0);
    }
    b.count = n;
    b.valid = true;
    m_vertices.resize(mark);
    m_inserts.push_back({ mark, &b });
}

void Renderer::DrawStatic(StaticBatch& b)
{
    if (b.valid && b.buf)
        m_inserts.push_back({ m_vertices.size(), &b });
}

Renderer::CaptureMark Renderer::BeginCapture() const
{
    return { m_vertices.size(), m_textVertices.size(), m_inserts.size() };
}

void Renderer::EndCapture(const CaptureMark& mark, Cache& outCache) const
{
    // Defensive against a mark from a stale/mismatched frame (buffers
    // cleared since) rather than assuming the mark is always still valid.
    if (mark.shapeCount > m_vertices.size() || mark.textCount > m_textVertices.size()
        || mark.insertCount > m_inserts.size())
    {
        outCache.valid = false;
        return;
    }
    outCache.shapeVerts.assign(m_vertices.begin() + mark.shapeCount, m_vertices.end());
    outCache.textVerts.assign(m_textVertices.begin() + mark.textCount, m_textVertices.end());
    outCache.inserts.clear();
    for (size_t i = mark.insertCount; i < m_inserts.size(); ++i)
        if (m_inserts[i].at >= mark.shapeCount)
            outCache.inserts.push_back({ m_inserts[i].at - mark.shapeCount, m_inserts[i].batch });
    outCache.generation = m_generation;
    outCache.valid = true;
}

void Renderer::Replay(const Cache& cache)
{
    if (!CacheCurrent(cache))
        return;
    // All or nothing (review F407): inserts left behind by dropped shapes
    // would draw the batch in the middle of a later widget.
    if (m_vertices.size() + cache.shapeVerts.size() > kMaxVertices
        || m_textVertices.size() + cache.textVerts.size() > kMaxTextVertices)
    {
        m_droppedVertices += static_cast<UINT>(cache.shapeVerts.size());
        m_droppedTotal += cache.shapeVerts.size();
        m_droppedText += static_cast<UINT>(cache.textVerts.size());
        return;
    }
    const size_t base = m_vertices.size();
    for (const StaticInsert& ins : cache.inserts)
        m_inserts.push_back({ base + ins.at, ins.batch });
    m_vertices.insert(m_vertices.end(), cache.shapeVerts.begin(), cache.shapeVerts.end());
    m_textVertices.insert(m_textVertices.end(), cache.textVerts.begin(), cache.textVerts.end());
}

void Renderer::AddFilledCircle(float cx, float cy, float radius, float r, float g, float b, float a, int segments)
{
    float ndcCx, ndcCy;
    PixelToNDC(cx, cy, ndcCx, ndcCy);

    const UINT64 rgba = PackColor(r, g, b, a);
    Vertex center{ ndcCx, ndcCy, rgba };

    float prevX, prevY;
    PixelToNDC(cx + radius, cy, prevX, prevY);

    for (int i = 1; i <= segments; ++i)
    {
        float angle = (2.0f * 3.14159265f * i) / segments;
        float px = cx + radius * cosf(angle);
        float py = cy + radius * sinf(angle);
        float ndcX, ndcY;
        PixelToNDC(px, py, ndcX, ndcY);

        Vertex v1{ prevX, prevY, rgba };
        Vertex v2{ ndcX, ndcY, rgba };
        PushTriangle(center, v1, v2);

        prevX = ndcX;
        prevY = ndcY;
    }
}

void Renderer::AddGradientQuad(float leftTop, float rightTop,
                               float leftBottom, float rightBottom,
                               float top, float bottom,
                               float r, float g, float b,
                               float aTL, float aTR, float aBL, float aBR)
{
    // Upright (no rotation), but the top and bottom edges may have different
    // widths, so this covers a plain rect and a taper with one primitive.
    //
    // Alpha is given per CORNER, which is a bilinear field -- and a bilinear
    // field is NOT reproducible by two Gouraud-shaded triangles: they'd only
    // agree along the shared diagonal, leaving a visible crease across the
    // face. Splitting into vertical strips makes each strip's own diagonal
    // error vanishingly small (it shrinks with the square of the strip
    // width), so the result reads as one smooth 2D gradient. Corner alphas
    // that vary in only one direction have no bilinear term at all and come
    // out exact either way.
    const int kStrips = 12;
    const UINT64 rgb = PackColor(r, g, b, 0.0f) & 0x0000FFFFFFFFFFFFull;   // alpha added per corner
    auto withAlpha = [rgb](float alpha) { return rgb | (FloatToHalf(alpha) << 48); };

    for (int i = 0; i < kStrips; ++i)
    {
        const float u0 = static_cast<float>(i) / kStrips;
        const float u1 = static_cast<float>(i + 1) / kStrips;

        const float xT0 = leftTop + (rightTop - leftTop) * u0;
        const float xT1 = leftTop + (rightTop - leftTop) * u1;
        const float xB0 = leftBottom + (rightBottom - leftBottom) * u0;
        const float xB1 = leftBottom + (rightBottom - leftBottom) * u1;

        const float aT0 = aTL + (aTR - aTL) * u0;
        const float aT1 = aTL + (aTR - aTL) * u1;
        const float aB0 = aBL + (aBR - aBL) * u0;
        const float aB1 = aBL + (aBR - aBL) * u1;

        float nxT0, nyT, nxT1, nyT1, nxB0, nyB, nxB1, nyB1;
        PixelToNDC(xT0, top, nxT0, nyT);
        PixelToNDC(xT1, top, nxT1, nyT1);
        PixelToNDC(xB0, bottom, nxB0, nyB);
        PixelToNDC(xB1, bottom, nxB1, nyB1);

        const Vertex vTL{ nxT0, nyT, withAlpha(aT0) };
        const Vertex vTR{ nxT1, nyT, withAlpha(aT1) };
        const Vertex vBR{ nxB1, nyB, withAlpha(aB1) };
        const Vertex vBL{ nxB0, nyB, withAlpha(aB0) };

        PushTriangle(vTL, vTR, vBR);
        PushTriangle(vTL, vBR, vBL);
    }
}

void Renderer::AddFilledRect(float cx, float cy, float halfWidth, float halfHeight, float rotation, float r, float g, float b, float a)
{
    // Most boxes are upright; cosf(0)/sinf(0) are exactly 1 and 0 anyway.
    const float cosR = (rotation == 0.0f) ? 1.0f : cosf(rotation);
    const float sinR = (rotation == 0.0f) ? 0.0f : sinf(rotation);
    const UINT64 rgba = PackColor(r, g, b, a);

    // Local corners: (-w,-h) (+w,-h) (+w,+h) (-w,+h)
    const float lx[4] = { -halfWidth,  halfWidth, halfWidth, -halfWidth };
    const float ly[4] = { -halfHeight, -halfHeight, halfHeight, halfHeight };

    Vertex v[4];
    for (int i = 0; i < 4; ++i)
    {
        const float px = cx + lx[i] * cosR - ly[i] * sinR;
        const float py = cy + lx[i] * sinR + ly[i] * cosR;
        float ndcX, ndcY;
        PixelToNDC(px, py, ndcX, ndcY);
        v[i] = Vertex{ ndcX, ndcY, rgba };
    }

    PushTriangle(v[0], v[1], v[2]);
    PushTriangle(v[0], v[2], v[3]);
}

void Renderer::AddRectOutline(float cx, float cy, float halfWidth, float halfHeight, float thickness, float r, float g, float b, float a)
{
    const float ht = thickness * 0.5f;
    // Top and bottom bars span the full width; left/right bars fill the
    // remaining height between them so the four don't overlap at the corners.
    AddFilledRect(cx, cy - halfHeight + ht, halfWidth, ht, 0.0f, r, g, b, a);
    AddFilledRect(cx, cy + halfHeight - ht, halfWidth, ht, 0.0f, r, g, b, a);
    AddFilledRect(cx - halfWidth + ht, cy, ht, halfHeight - thickness, 0.0f, r, g, b, a);
    AddFilledRect(cx + halfWidth - ht, cy, ht, halfHeight - thickness, 0.0f, r, g, b, a);
}

void Renderer::AddFilledRoundedRect(float cx, float cy, float halfWidth, float halfHeight, float cornerRadius, float rotation, float r, float g, float b, float a)
{
    // Clamp the corner radius so it can never exceed the shape.
    float cr = cornerRadius;
    const float maxR = (halfWidth < halfHeight ? halfWidth : halfHeight);
    if (cr > maxR) cr = maxR;
    if (cr <= 0.0f)
    {
        AddFilledRect(cx, cy, halfWidth, halfHeight, rotation, r, g, b, a);
        return;
    }

    const int kPerCorner = kArcPerCorner;     // arc points per corner (angles in kArc)
    const int kTotal = kArcPoints;
    float px[kTotal];
    float py[kTotal];

    // Corner arc centres, in local space (x right, y down).
    const float ix = halfWidth - cr;
    const float iy = halfHeight - cr;
    const float cxs[4] = {  ix,  ix, -ix, -ix };
    const float cys[4] = { -iy,  iy,  iy, -iy };

    int n = 0;
    for (int c = 0; c < 4; ++c)
    {
        for (int i = 0; i < kPerCorner; ++i)
        {
            px[n] = cxs[c] + cr * kArc.cosA[n];
            py[n] = cys[c] + cr * kArc.sinA[n];
            ++n;
        }
    }

    // Text plates and panels are upright; cosf(0)/sinf(0) are exactly 1 and 0.
    const float cosR = (rotation == 0.0f) ? 1.0f : cosf(rotation);
    const float sinR = (rotation == 0.0f) ? 0.0f : sinf(rotation);
    const UINT64 rgba = PackColor(r, g, b, a);

    // Centre vertex for the triangle fan.
    float ndcCx, ndcCy;
    PixelToNDC(cx, cy, ndcCx, ndcCy);
    const Vertex center{ ndcCx, ndcCy, rgba };

    Vertex prev;
    {
        const float wx = cx + px[0] * cosR - py[0] * sinR;
        const float wy = cy + px[0] * sinR + py[0] * cosR;
        float nx, ny;
        PixelToNDC(wx, wy, nx, ny);
        prev = Vertex{ nx, ny, rgba };
    }

    for (int i = 1; i <= kTotal; ++i)
    {
        const int idx = (i == kTotal) ? 0 : i;   // close the loop
        const float wx = cx + px[idx] * cosR - py[idx] * sinR;
        const float wy = cy + px[idx] * sinR + py[idx] * cosR;
        float nx, ny;
        PixelToNDC(wx, wy, nx, ny);
        const Vertex cur{ nx, ny, rgba };

        PushTriangle(center, prev, cur);
        prev = cur;
    }
}

void Renderer::AddRing(float cx, float cy, float innerRadius, float outerRadius, float r, float g, float b, float a, int segments)
{
    const UINT64 rgba = PackColor(r, g, b, a);
    float prevInX, prevInY, prevOutX, prevOutY;
    {
        float ndcX, ndcY;
        PixelToNDC(cx + innerRadius, cy, ndcX, ndcY);
        prevInX = ndcX; prevInY = ndcY;
        PixelToNDC(cx + outerRadius, cy, ndcX, ndcY);
        prevOutX = ndcX; prevOutY = ndcY;
    }

    for (int i = 1; i <= segments; ++i)
    {
        float angle = (2.0f * 3.14159265f * i) / segments;
        float cosA = cosf(angle);
        float sinA = sinf(angle);

        float ndcInX, ndcInY, ndcOutX, ndcOutY;
        PixelToNDC(cx + innerRadius * cosA, cy + innerRadius * sinA, ndcInX, ndcInY);
        PixelToNDC(cx + outerRadius * cosA, cy + outerRadius * sinA, ndcOutX, ndcOutY);

        Vertex a0{ prevInX, prevInY, rgba };
        Vertex a1{ prevOutX, prevOutY, rgba };
        Vertex a2{ ndcInX, ndcInY, rgba };
        Vertex a3{ ndcOutX, ndcOutY, rgba };

        PushTriangle(a0, a1, a2);
        PushTriangle(a1, a3, a2);

        prevInX = ndcInX; prevInY = ndcInY;
        prevOutX = ndcOutX; prevOutY = ndcOutY;
    }
}

void Renderer::AddLine(float x1, float y1, float x2, float y2, float thickness, float r, float g, float b, float a)
{
    float dx = x2 - x1;
    float dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f)
        return;

    float nx = -dy / len * (thickness * 0.5f);
    float ny = dx / len * (thickness * 0.5f);

    float ndc1x, ndc1y, ndc2x, ndc2y, ndc3x, ndc3y, ndc4x, ndc4y;
    PixelToNDC(x1 + nx, y1 + ny, ndc1x, ndc1y);
    PixelToNDC(x1 - nx, y1 - ny, ndc2x, ndc2y);
    PixelToNDC(x2 + nx, y2 + ny, ndc3x, ndc3y);
    PixelToNDC(x2 - nx, y2 - ny, ndc4x, ndc4y);

    const UINT64 rgba = PackColor(r, g, b, a);
    Vertex v1{ ndc1x, ndc1y, rgba };
    Vertex v2{ ndc2x, ndc2y, rgba };
    Vertex v3{ ndc3x, ndc3y, rgba };
    Vertex v4{ ndc4x, ndc4y, rgba };

    PushTriangle(v1, v2, v3);
    PushTriangle(v2, v4, v3);
}

// =============================== Text ===============================
//
// A font atlas is rasterized once via GDI (white text on black, grayscale
// anti-aliased -- no ClearType colour fringing) into a grid, one cell per
// ASCII character 32..126. The grid is read back as a single-channel
// (R8_UNORM) texture, since only glyph coverage/alpha is needed -- colour
// tinting happens per-draw via vertex colour, same as every other primitive.

bool Renderer::BuildFontAtlas()
{
    // Three sets of the same glyphs, rasterized at three sizes, all in one
    // texture: AddText draws each string from the smallest set at least as
    // tall as the text (see SetFor), so text is only ever shrunk a little
    // and never enlarged. One 160px set alone meant small text (~20px) was
    // a 7x minification, which only looks right when the driver generates
    // mipmaps -- under Proton/DXVK thin strokes dropped out between samples.
    //   set 2: 160px cells, at the top (gear/speed readouts, up to ~165px)
    //   set 1:  64px cells, below it on the left
    //   set 0:  32px cells, beside set 1
    const int kColumns = 10;
    const int kRows = 10; // 100 slots, 95 used
    struct SetLayout { int cellW, cellH, originX, originY; };
    const SetLayout sets[kFontSets] = {
        { 20, 32, 440, 1600 },    // small
        { 40, 64, 0, 1600 },      // medium
        { 96, 160, 0, 0 },        // large
    };
    const int atlasW = kColumns * 96;              // 960
    const int atlasH = 1600 + kRows * 64;          // 2240

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = atlasW;
    bmi.bmiHeader.biHeight = -atlasH; // top-down, so row 0 is the top row
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* dibPixels = nullptr;
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP dib = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &dibPixels, nullptr, 0);
    ReleaseDC(nullptr, screenDC);

    if (!dib || !dibPixels)
    {
        if (dib) DeleteObject(dib);
        DeleteDC(memDC);
        return false;
    }

    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, dib));

    // Fill black (the "no coverage" background the glyphs sit on).
    RECT full = { 0, 0, atlasW, atlasH };
    HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    FillRect(memDC, &full, black);

    // Light is the default weight -- at the large sizes these numbers are
    // actually drawn at (up to ~165px), Consolas's regular weight reads as
    // "fat"; a lighter stroke stays legible while looking cleaner. Fonts
    // with no light face (the built-in DejaVu) just draw their regular one.
    LoadEmbeddedFonts();
    static const int kWeights[kWeightCount] = { FW_LIGHT, FW_NORMAL, FW_BOLD };
    const int weight = kWeights[m_fontWeight];
    const int height = sets[kFontSets - 1].cellH - 16;   // the face is chosen at the large size
    const char* face = m_fontChoice == 0 ? "Consolas" : kFontNames[m_fontChoice];
    HFONT font = TryFont(memDC, face, height, weight);
    if (!font)
    {
        // Picked font not installed (Auto on a system with no Consolas
        // lands here too): the built-in one is always there.
        Log::Write("Renderer: font '%s' not installed -- using the built-in DejaVu Sans Mono", face);
        face = "DejaVu Sans Mono";
        font = TryFont(memDC, face, height, weight);
    }
    if (!font)
    {
        // Last resort, the old behaviour: whatever GDI gives for Consolas.
        face = "Consolas";
        font = CreateFontA(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, face);
    }
    HFONT oldFont = static_cast<HFONT>(SelectObject(memDC, font));
    Glyph newGlyphs[kFontSets][kGlyphCount];

    SetTextColor(memDC, RGB(255, 255, 255));
    SetBkMode(memDC, TRANSPARENT);

    for (int s = 0; s < kFontSets; ++s)
    {
    const SetLayout& L = sets[s];
    m_setHeight[s] = static_cast<float>(L.cellH);
    // Same proportions at every size: glyph height 0.9 of the cell, 6/160
    // of it as top padding. The large set reuses the font chosen above; the
    // smaller ones get the same face rasterized natively at their size, so
    // GDI's hinting lines their strokes up with whole pixels.
    HFONT setFont = font;
    if (s != kFontSets - 1)
    {
        setFont = CreateFontA(L.cellH * 9 / 10, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, face);
        SelectObject(memDC, setFont);
    }
    const int padTop = (L.cellH * 6 + 80) / 160;

    for (int i = 0; i < kGlyphCount; ++i)
    {
        char ch = static_cast<char>(kGlyphFirst + i);
        const int col = i % kColumns;
        const int row = i / kColumns;
        const int cellX = L.originX + col * L.cellW;
        const int cellY = L.originY + row * L.cellH;

        SIZE extent = {};
        GetTextExtentPoint32A(memDC, &ch, 1, &extent);

        // Centre horizontally in the cell; a little padding from the top.
        const int drawX = cellX + (L.cellW - extent.cx) / 2;
        const int drawY = cellY + padTop;
        TextOutA(memDC, drawX, drawY, &ch, 1);

        // UV bounds are the TIGHT glyph width (drawX..drawX+extent.cx), not
        // the full padded cell -- AddText draws each quad at a width derived
        // from this UV rect's aspect ratio, so if it spanned the whole cell
        // (wider than the glyph's actual advance whenever the cell has any
        // horizontal padding, which it does here) every quad would be drawn
        // wider than the cursor advances between characters, overlapping the
        // next glyph. Matching the two exactly makes drawn width == advance.
        Glyph& g = newGlyphs[s][i];
        g.u0 = static_cast<float>(drawX) / atlasW;
        g.v0 = static_cast<float>(cellY) / atlasH;
        g.u1 = static_cast<float>(drawX + extent.cx) / atlasW;
        g.v1 = static_cast<float>(cellY + L.cellH) / atlasH;
        g.advancePx = static_cast<float>(extent.cx);
    }
    if (setFont != font)
    {
        SelectObject(memDC, font);
        DeleteObject(setFont);
    }
    }

    // Expand to RGBA8, replicating the coverage byte into every channel
    // (the shader only reads .r). R8_UNORM was tried first, but
    // D3D11_RESOURCE_MISC_GENERATE_MIPS silently no-ops on hardware/drivers
    // that don't list D3D11_FORMAT_SUPPORT_MIP_AUTOGEN for a given format --
    // when that happens every mip level past 0 is left as UNINITIALIZED
    // GPU memory, which samples as garbage and looks far worse than no
    // mipmaps at all (this is what actually happened: text got worse, not
    // better). R8G8B8A8_UNORM's auto-mipmap support is essentially
    // universal, so this sidesteps the per-format uncertainty entirely.
    std::vector<unsigned char> rgbaData(static_cast<size_t>(atlasW) * atlasH * 4);
    const unsigned char* src = static_cast<const unsigned char*>(dibPixels);
    for (int y = 0; y < atlasH; ++y)
    {
        for (int x = 0; x < atlasW; ++x)
        {
            const unsigned char coverage = src[(static_cast<size_t>(y) * atlasW + x) * 4]; // B channel
            unsigned char* out = &rgbaData[(static_cast<size_t>(y) * atlasW + x) * 4];
            out[0] = out[1] = out[2] = out[3] = coverage;
        }
    }

    SelectObject(memDC, oldFont);
    SelectObject(memDC, oldBmp);
    DeleteObject(font);
    DeleteObject(dib);
    DeleteDC(memDC);

    // Only ask for a mip chain if this device actually supports generating
    // one for this format -- otherwise fall back to the single-level path
    // rather than risk the garbage-mip failure mode described above.
    UINT formatSupport = 0;
    m_device->CheckFormatSupport(DXGI_FORMAT_R8G8B8A8_UNORM, &formatSupport);
    const bool canAutoMip = (formatSupport & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) != 0;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = atlasW;
    texDesc.Height = atlasH;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;

    ID3D11Texture2D* fontTexture = nullptr;
    ID3D11ShaderResourceView* newSRV = nullptr;
    HRESULT hr;

    if (canAutoMip)
    {
        // This atlas gets sampled at wildly different scales (28px readout
        // text up to 165px gear digits, all from one ~144pt source); a
        // single mip level has no way to properly filter the large
        // minification at the small end, so mipmapping lets the GPU pick a
        // pre-downsampled level appropriate to each draw size.
        texDesc.MipLevels = 0; // full auto chain
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

        // GENERATE_MIPS resources can't take initial data at creation time --
        // upload the top level via UpdateSubresource afterward instead.
        if (FAILED(m_device->CreateTexture2D(&texDesc, nullptr, &fontTexture)))
            return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = static_cast<UINT>(-1); // all levels

        hr = m_device->CreateShaderResourceView(fontTexture, &srvDesc, &newSRV);
        if (SUCCEEDED(hr))
        {
            ID3D11DeviceContext* immediateCtx = nullptr;
            m_device->GetImmediateContext(&immediateCtx);
            if (immediateCtx)
            {
                immediateCtx->UpdateSubresource(fontTexture, 0, nullptr, rgbaData.data(), atlasW * 4, 0);
                immediateCtx->GenerateMips(newSRV);
                immediateCtx->Release();
            }
        }
    }
    else
    {
        Log::Write("Renderer::InitText: device reports no MIP_AUTOGEN support for R8G8B8A8_UNORM -- using a single mip level");
        texDesc.MipLevels = 1;
        texDesc.Usage = D3D11_USAGE_IMMUTABLE;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = rgbaData.data();
        initData.SysMemPitch = atlasW * 4;

        if (FAILED(m_device->CreateTexture2D(&texDesc, &initData, &fontTexture)))
            return false;
        hr = m_device->CreateShaderResourceView(fontTexture, nullptr, &newSRV);
    }

    fontTexture->Release(); // SRV holds its own reference
    if (FAILED(hr))
    {
        if (newSRV) newSRV->Release();
        return false;
    }

    // Swap in the new atlas only once it is complete.
    if (m_fontSRV) m_fontSRV->Release();
    m_fontSRV = newSRV;
    memcpy(m_glyphs, newGlyphs, sizeof(m_glyphs));
    strcpy_s(m_activeFace, face);
    Log::Write("Renderer: text font %s (%s)", m_activeFace, kWeightNames[m_fontWeight]);
    return true;
}

bool Renderer::InitText()
{
    if (!BuildFontAtlas())
        return false;
    m_fontReloadPending = false;

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (FAILED(m_device->CreateSamplerState(&sampDesc, &m_fontSampler)))
        return false;

    // Shaders + input layout for the textured quad pipeline (bytecode
    // borrowed from the process-wide cache, see CompiledShader).
    ID3DBlob* vsBlob = CompiledShader(kTextVS, kTextShaderSrc, "VSMain", "vs_4_0");
    if (!vsBlob)
        return false;
    ID3DBlob* psBlob = CompiledShader(kTextPS, kTextShaderSrc, "PSMain", "ps_4_0");
    if (!psBlob)
        return false;

    // Same "don't trust these to succeed" fix as the shape pipeline in
    // Init() -- a null m_textVertexBuffer in particular would crash
    // EndFrame()'s Map() call on it.
    bool ok = true;
    if (FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_textVs)))
        ok = false;
    if (FAILED(m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_textPs)))
        ok = false;

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    if (FAILED(m_device->CreateInputLayout(layout, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_textInputLayout)))
        ok = false;

    if (!ok)
        return false;

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.ByteWidth = sizeof(TextVertex) * kMaxTextVertices;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_device->CreateBuffer(&vbDesc, nullptr, &m_textVertexBuffer)))
        return false;

    m_textVertices.reserve(kMaxTextVertices);
    return true;
}

void Renderer::ShutdownText()
{
    if (m_fontSampler) { m_fontSampler->Release(); m_fontSampler = nullptr; }
    if (m_fontSRV) { m_fontSRV->Release(); m_fontSRV = nullptr; }
    if (m_textVertexBuffer) { m_textVertexBuffer->Release(); m_textVertexBuffer = nullptr; }
    if (m_textInputLayout) { m_textInputLayout->Release(); m_textInputLayout = nullptr; }
    if (m_textPs) { m_textPs->Release(); m_textPs = nullptr; }
    if (m_textVs) { m_textVs->Release(); m_textVs = nullptr; }
}

void Renderer::PushGlyphQuad(float x, float y, float w, float h, const Glyph& g, UINT64 rgba)
{
    if (m_textVertices.size() + 6 > kMaxTextVertices)
    {
        m_droppedText += 6;
        return;
    }

    float ndcX0, ndcY0, ndcX1, ndcY1;
    PixelToNDC(x, y, ndcX0, ndcY0);
    PixelToNDC(x + w, y + h, ndcX1, ndcY1);

    TextVertex v0{ ndcX0, ndcY0, g.u0, g.v0, rgba };
    TextVertex v1{ ndcX1, ndcY0, g.u1, g.v0, rgba };
    TextVertex v2{ ndcX0, ndcY1, g.u0, g.v1, rgba };
    TextVertex v3{ ndcX1, ndcY1, g.u1, g.v1, rgba };

    m_textVertices.push_back(v0);
    m_textVertices.push_back(v1);
    m_textVertices.push_back(v2);
    m_textVertices.push_back(v1);
    m_textVertices.push_back(v3);
    m_textVertices.push_back(v2);
}

// One glyph turned 90 degrees anticlockwise: its point (lx along the text,
// ly down the glyph) lands at screen (x + ly, yBottom - lx).
void Renderer::PushGlyphQuadRotated(float x, float yBottom, float along, float w, float h, const Glyph& g, UINT64 rgba)
{
    if (m_textVertices.size() + 6 > kMaxTextVertices)
    {
        m_droppedText += 6;
        return;
    }
    float ax, ay, bx, by, cx, cy, dx, dy;
    PixelToNDC(x,     yBottom - along,       ax, ay);   // glyph top-left
    PixelToNDC(x,     yBottom - along - w,   bx, by);   // top-right
    PixelToNDC(x + h, yBottom - along,       cx, cy);   // bottom-left
    PixelToNDC(x + h, yBottom - along - w,   dx, dy);   // bottom-right
    TextVertex v0{ ax, ay, g.u0, g.v0, rgba };
    TextVertex v1{ bx, by, g.u1, g.v0, rgba };
    TextVertex v2{ cx, cy, g.u0, g.v1, rgba };
    TextVertex v3{ dx, dy, g.u1, g.v1, rgba };
    m_textVertices.push_back(v0);
    m_textVertices.push_back(v1);
    m_textVertices.push_back(v2);
    m_textVertices.push_back(v1);
    m_textVertices.push_back(v3);
    m_textVertices.push_back(v2);
}

void Renderer::AddTextRotated(float x, float y, const char* text, float pixelHeight, float r, float g, float b, float a)
{
    if (!text || !m_fontSRV) return;
    const int set = SetFor(pixelHeight);
    const float scale = pixelHeight / m_setHeight[set];
    const UINT64 rgba = PackColor(r, g, b, a);
    float along = 0.0f;
    char one[2];
    for (const char* p = text; *p; )
    for (const char* q = NextFolded(p, one); *q; ++q)
    {
        const int idx = static_cast<int>(*q) - kGlyphFirst;
        if (idx < 0 || idx >= kGlyphCount)
            continue;
        const Glyph& gl = m_glyphs[set][idx];
        const float adv = gl.advancePx * scale;
        if (*q != ' ')
            PushGlyphQuadRotated(x, y, along, adv, pixelHeight, gl, rgba);
        along += adv;
    }
}

// The smallest glyph set at least as tall as the text, so it is shrunk by
// at most ~2.5x and never enlarged (past the large set's 160px).
int Renderer::SetFor(float pixelHeight) const
{
    for (int s = 0; s < kFontSets - 1; ++s)
        if (pixelHeight <= m_setHeight[s]) return s;
    return kFontSets - 1;
}

float Renderer::MeasureText(const char* text, float pixelHeight) const
{
    if (!text) return 0.0f;
    const int set = SetFor(pixelHeight);
    const float scale = pixelHeight / m_setHeight[set];
    float width = 0.0f;
    char one[2];
    for (const char* p = text; *p; )
    for (const char* q = NextFolded(p, one); *q; ++q)
    {
        const int idx = static_cast<int>(*q) - kGlyphFirst;
        if (idx < 0 || idx >= kGlyphCount) continue;
        width += m_glyphs[set][idx].advancePx * scale;
    }
    return width;
}

void Renderer::SetTextBackgroundStyle(float r, float g, float b, float a, float paddingPx, float cornerRadiusPx)
{
    m_textBgR = r;
    m_textBgG = g;
    m_textBgB = b;
    m_textBgA = a;
    m_textBgPaddingPx = paddingPx;
    m_textBgCornerRadiusPx = cornerRadiusPx;
}

void Renderer::AddLineSmooth(float x1, float y1, float x2, float y2, float thickness, float r, float g, float b, float a)
{
    if (thickness <= 0.0f || a <= 0.0f)
        return;

    // Halo first so the core sits on top of it. Roughly one pixel of feather
    // each side (the halo is 2px wider overall) at a third of the alpha --
    // wider than that reads as a blur rather than a smooth edge, and a higher
    // alpha makes the line look thicker instead of softer.
    AddLine(x1, y1, x2, y2, thickness + 2.0f, r, g, b, a * 0.33f);
    AddLine(x1, y1, x2, y2, thickness, r, g, b, a);
}

void Renderer::AddPolylineSmooth(const float* xy, int count, bool closed, float thickness, float r, float g, float b, float a,
                                 const float* rgbPerPoint)
{
    if (!xy || count < 2 || thickness <= 0.0f || a <= 0.0f)
        return;

    const float half = thickness * 0.5f;
    const float feather = 1.0f;
    // A miter grows without bound as a bend tightens; past this multiple of
    // the half-width it is clamped, so a hairpin squeezed into a few pixels
    // gives a slightly blunt outside corner rather than a long spike.
    const float kMaxMiter = 2.5f;

    // Unit direction of segment i -> i+1 (wrapping when closed).
    auto segDir = [&](int i, float& dx, float& dy)
    {
        const int j = (i + 1) % count;
        dx = xy[j * 2] - xy[i * 2];
        dy = xy[j * 2 + 1] - xy[i * 2 + 1];
        const float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.0001f) { dx = 1.0f; dy = 0.0f; return; }
        dx /= len; dy /= len;
    };

    // Per point: the 4 ribbon edges across the line (outer feather, core,
    // core, outer feather), already in NDC. Computed for a point, then
    // reused as the start of the next segment.
    struct Cross { Vertex v[4]; };
    auto crossAt = [&](int i, Cross& out)
    {
        float inX, inY, outX, outY;
        const bool hasPrev = closed || i > 0;
        const bool hasNext = closed || i < count - 1;
        if (hasNext) segDir(i, outX, outY);
        if (hasPrev) segDir((i - 1 + count) % count, inX, inY);
        if (!hasNext) { outX = inX; outY = inY; }
        if (!hasPrev) { inX = outX; inY = outY; }

        // Normals of the two segments, and their average as the miter.
        const float n0x = -inY, n0y = inX;
        const float n1x = -outY, n1y = outX;
        float mx = n0x + n1x, my = n0y + n1y;
        const float mlen = sqrtf(mx * mx + my * my);
        float scale = 1.0f;
        if (mlen < 0.0001f)
        {
            // Full reversal: no meaningful miter, fall back to one normal.
            mx = n1x; my = n1y;
        }
        else
        {
            mx /= mlen; my /= mlen;
            const float cosHalf = mx * n1x + my * n1y;
            scale = (cosHalf > 1.0f / kMaxMiter) ? 1.0f / cosHalf : kMaxMiter;
        }

        const float px = xy[i * 2], py = xy[i * 2 + 1];
        const float offs[4] = { -(half + feather), -half, half, half + feather };
        const float alpha[4] = { 0.0f, a, a, 0.0f };
        const float cr = rgbPerPoint ? rgbPerPoint[i * 3] : r;
        const float cg = rgbPerPoint ? rgbPerPoint[i * 3 + 1] : g;
        const float cb = rgbPerPoint ? rgbPerPoint[i * 3 + 2] : b;
        const UINT64 edge = PackColor(cr, cg, cb, alpha[0]);
        const UINT64 core = PackColor(cr, cg, cb, alpha[1]);
        const UINT64 colours[4] = { edge, core, core, edge };
        for (int k = 0; k < 4; ++k)
        {
            float nx, ny;
            PixelToNDC(px + mx * offs[k] * scale, py + my * offs[k] * scale, nx, ny);
            out.v[k] = Vertex{ nx, ny, colours[k] };
        }
    };

    const int segments = closed ? count : count - 1;
    Cross first, cur, next;
    crossAt(0, first);
    cur = first;
    for (int s = 0; s < segments; ++s)
    {
        const int j = s + 1;
        if (j == count) next = first;   // closing segment reuses point 0
        else crossAt(j, next);

        // Three strips across the ribbon: feather, core, feather.
        for (int k = 0; k < 3; ++k)
        {
            PushTriangle(cur.v[k], cur.v[k + 1], next.v[k]);
            PushTriangle(cur.v[k + 1], next.v[k + 1], next.v[k]);
        }
        cur = next;
    }
}

void Renderer::AddText(float x, float y, const char* text, float pixelHeight, float r, float g, float b, float a, HAlign align)
{
    if (!text || !m_fontSRV) return;

    const float totalWidthForBg = MeasureText(text, pixelHeight);

    float startX = x;
    if (align != HAlign::Left)
    {
        startX = (align == HAlign::Center) ? (x - totalWidthForBg * 0.5f) : (x - totalWidthForBg);
    }

    // Plate first, so it is behind this string's own glyphs. Scaled by the
    // text size rather than fixed, so it stays proportionate at any
    // resolution; skipped entirely for text that is itself fading out, which
    // would otherwise leave the plate hanging behind invisible glyphs.
    if (m_textBackground && m_textBgA > 0.004f && a > 0.004f && totalWidthForBg > 0.0f)
    {
        const float pad = m_textBgPaddingPx * (pixelHeight / 40.0f);
        const float halfW = totalWidthForBg * 0.5f + pad;
        const float halfH = pixelHeight * 0.5f + pad;
        AddFilledRoundedRect(startX + totalWidthForBg * 0.5f, y + pixelHeight * 0.5f,
                             halfW, halfH,
                             m_textBgCornerRadiusPx * (pixelHeight / 40.0f), 0.0f,
                             m_textBgR, m_textBgG, m_textBgB, m_textBgA * a);
    }

    const int set = SetFor(pixelHeight);
    const float scale = pixelHeight / m_setHeight[set];
    const UINT64 rgba = PackColor(r, g, b, a);

    float cursor = startX;
    char one[2];
    for (const char* p = text; *p; )
    for (const char* q = NextFolded(p, one); *q; ++q)   // review F150
    {
        const int idx = static_cast<int>(*q) - kGlyphFirst;
        if (idx < 0 || idx >= kGlyphCount)
            continue;

        const Glyph& gl = m_glyphs[set][idx];
        // A space has no ink: advance only, no quad (review F149).
        if (*q == ' ')
        {
            cursor += gl.advancePx * scale;
            continue;
        }
        // Quad width from advancePx directly (same formula as the cursor
        // step below and as MeasureText), NOT from the UV rect's aspect
        // ratio: (u1-u0)/(v1-v0) is a ratio of two deltas each normalized by
        // a DIFFERENT denominator (atlasW vs atlasH), so it only equals the
        // true pixel aspect ratio when atlasW==atlasH. That held by accident
        // for the old full-cell UV (a square 10x10 glyph grid made the
        // mismatch cancel out), but broke once the UV was cropped to each
        // glyph's tight ink width -- silently inflating every quad by
        // atlasH/atlasW and causing exactly the overlap this was meant to fix.
        const float quadH = pixelHeight;
        const float quadW = gl.advancePx * scale;

        PushGlyphQuad(cursor, y, quadW, quadH, gl, rgba);
        cursor += gl.advancePx * scale;
    }
}
