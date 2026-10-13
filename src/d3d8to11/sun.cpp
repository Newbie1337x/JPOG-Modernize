// Sun shadows, same-frame and screen-space:
//  1. The first terrain draw of a frame gives the camera (its c0..c3 is the world-to-clip
//     matrix). A light-space orthographic box is fitted to the visible ground and the shadow
//     map is cleared.
//  2. Every opaque world draw is rendered a second time into the shadow map. The translated
//     vertex shader rebuilds the world position from its own clip position through the
//     inverse camera matrix, so skinned, rigid and terrain geometry all work the same way.
//  3. When the world is finished (first HUD draw, a depth clear, or Present) a full-screen
//     pass reads the scene depth, reconstructs world positions and normals, samples the shadow
//     map and multiplies the back buffer. The game's own lighting is never replaced.
#include "device.h"
#include <DirectXMath.h>
#include <cmath>
#include <stdarg.h>
#include <stdio.h>
#include <share.h>
using namespace DirectX;

namespace {

struct ResolveConstants {
    float clipToWorld[4][4];
    float lightRows[4][4];
    float sunDir[4];
    float params[4]; // strength, 1/map size, depth bias (light depth units), normal offset (world)
    float screen[4]; // 1/W, 1/H, half-pixel offset applied by the vertex shaders (x, y)
    float misc[4];   // debug view, shadow distance, light depth range, shadows on
    float grade0[4]; // exposure, contrast, saturation, warmth
    float grade1[4]; // sun boost, shadow cool, post on, -
};

const char* kResolveVS =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n";

// Compiled twice: MSAA 0 (Texture2D depth) and MSAA 1 (Texture2DMS depth, sample 0).
const char* kResolvePS =
    "cbuffer Resolve : register(b0) { float4 clipToWorld[4]; float4 lightRows[4]; float4 sunDir;\n"
    "  float4 params; float4 screen; float4 misc; float4 grade0; float4 grade1; };\n"
    "#if MSAA\n"
    "Texture2DMS<float> sceneDepth : register(t0);\n"
    "float LoadDepth(int2 p) { return sceneDepth.Load(p, 0); }\n"
    "#else\n"
    "Texture2D<float> sceneDepth : register(t0);\n"
    "float LoadDepth(int2 p) { return sceneDepth.Load(int3(p, 0)); }\n"
    "#endif\n"
    "Texture2D<float> shadowMap : register(t1);\n"
    "Texture2D<float4> sceneColor : register(t2);\n"
    "SamplerComparisonState cmp : register(s0);\n"
    "static const float2 kTaps[12] = { float2(-0.326,-0.406), float2(-0.840,-0.074), float2(-0.696,0.457),\n"
    "  float2(-0.203,0.621), float2(0.962,-0.195), float2(0.473,-0.480), float2(0.519,0.767),\n"
    "  float2(0.185,-0.893), float2(0.507,0.064), float2(0.896,0.412), float2(-0.322,-0.933),\n"
    "  float2(-0.792,-0.598) };\n"
    "float3 Unproject(float2 px, float d) {\n"
    "  float2 ndc = float2(px.x * screen.x * 2 - 1, 1 - px.y * screen.y * 2) - screen.zw;\n"
    "  float4 c = float4(ndc, d, 1);\n"
    "  float4 w = float4(dot(c, clipToWorld[0]), dot(c, clipToWorld[1]), dot(c, clipToWorld[2]), dot(c, clipToWorld[3]));\n"
    "  return w.xyz / w.w; }\n"
    "float3 WorldAt(int2 p) { return Unproject(p + 0.5, LoadDepth(p)); }\n"
    // Soft shoulder above 0.8 so boosted highlights roll off instead of clipping.
    "float3 Shoulder(float3 x) { return x <= 0.8 ? x : 0.8 + 0.2 * (1 - exp(-(x - 0.8) / 0.2)); }\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  int2 p = int2(pos.xy);\n"
    "  float3 color = sceneColor.Load(int3(p, 0)).rgb;\n"
    "  if (misc.x == 5 && misc.w > 0.5 && pos.x < 540 && pos.y < 540) {\n"
    "    uint mw, mh; shadowMap.GetDimensions(mw, mh);\n"
    "    float v = shadowMap.Load(int3(pos.xy / 540 * mw, 0));\n"
    "    return v >= 1 ? float4(0.2, 0.2, 0.6, 1) : float4(frac(v * misc.z / 2).xxx, 1); }\n"
    "  float d = LoadDepth(p);\n"
    "  bool sky = d >= 0.999999;\n"
    "  float shadow = 0, vis = 1;\n"
    "  if (sky && misc.x >= 3) return float4(1, 0.6, 1, 1);\n"
    "  if (!sky && misc.w > 0.5) {\n"
    "    float3 P = Unproject(pos.xy, d);\n"
    "    float3 V = Unproject(pos.xy, 0) - P;\n"
    "    float dist = length(V);\n"
    // Normal from depth: per axis take the neighbour on the same surface (smaller step).
    "    float3 L = WorldAt(p - int2(1, 0)), R = WorldAt(p + int2(1, 0));\n"
    "    float3 U = WorldAt(p - int2(0, 1)), D = WorldAt(p + int2(0, 1));\n"
    "    float3 dx = dot(R - P, R - P) < dot(P - L, P - L) ? R - P : P - L;\n"
    "    float3 dy = dot(D - P, D - P) < dot(P - U, P - U) ? D - P : P - U;\n"
    "    float3 N = cross(dy, dx); float nl = dot(N, N);\n"
    "    N = nl > 1e-20 ? N * rsqrt(nl) : V / max(dist, 1e-6);\n"
    "    if (dot(N, V) < 0) N = -N;\n"
    "    if (misc.x == 4) return float4(N * 0.5 + 0.5, 1);\n"
    "    if (misc.x == 6) { float2 g = frac(P.xz / 8); return float4(g.x < 0.08 || g.y < 0.08 ? float3(1, 0, 0) : frac(P.yyy / 4), 1); }\n"
    "    float ndl = dot(N, sunDir.xyz);\n"
    // Offset toward the sun: robust where depth normals are noisy (grass, foliage).
    "    float3 Q = P + sunDir.xyz * params.w * 3;\n"
    "    float4 Q4 = float4(Q, 1);\n"
    "    float3 q = float3(dot(Q4, lightRows[0]), dot(Q4, lightRows[1]), dot(Q4, lightRows[2]));\n"
    "    float2 uv = q.xy * float2(0.5, -0.5) + 0.5;\n"
    "    float edge = saturate(min(min(uv.x, 1 - uv.x), min(uv.y, 1 - uv.y)) * 12);\n"
    "    if (edge > 0 && q.z > 0 && q.z < 1) {\n"
    "      float z = q.z - params.z, sum = 0;\n"
    "      [unroll] for (int k = 0; k < 12; k++) sum += shadowMap.SampleCmpLevelZero(cmp, uv + kTaps[k] * params.y * 1.6, z);\n"
    "      vis = sum / 12;\n"
    "    }\n"
    "    if (misc.x == 7) return float4(vis.xxx, 1);\n"
    // Faces turned away from the sun are already dark in the game's own lighting.
    "    float facing = lerp(0.6, 1, saturate(ndl * 4));\n"
    "    float fade = saturate((misc.y - dist) / (misc.y * 0.2));\n"
    "    shadow = (1 - vis) * edge * facing * fade;\n"
    "  }\n"
    "  float3 shade = (1 - params.x * shadow) * lerp(float3(1, 1, 1), float3(0.93, 0.96, 1.0), shadow);\n"
    "  if (misc.x == 3) return float4(shade, 1);\n"
    "  float3 c = color * shade;\n"
    "  if (grade1.z > 0.5) {\n"
    // Sunlight: warm lift where the sun reaches (everything outside the shadow), cool shadows.
    "    float lit = sky ? 0 : 1 - shadow;\n"
    "    c *= 1 + grade1.x * lit * float3(1.0, 0.93, 0.78);\n"
    "    c = lerp(c, c * float3(0.88, 0.94, 1.1), grade1.y * saturate(shadow * 2));\n"
    "    c *= grade0.x;\n"
    "    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));\n"
    "    c = lerp(luma.xxx, c, grade0.z);\n"
    "    c *= float3(1 + 0.08 * grade0.w, 1, 1 - 0.08 * grade0.w);\n"
    "    c = (c - 0.5) * grade0.y + 0.5;\n"
    "    c = Shoulder(max(c, 0));\n"
    "  }\n"
    "  return float4(c, 1); }\n";
void LoadRows(const float (&rows)[4][4], XMMATRIX& out)
{
    // rows[i] are dot-product rows (clip_i = rows[i] . p); as a DirectXMath row-vector matrix
    // that is the transpose.
    XMFLOAT4X4 m;
    memcpy(&m, rows, sizeof(m));
    out = XMMatrixTranspose(XMLoadFloat4x4(&m));
}

void StoreRows(FXMMATRIX m, float (&rows)[4][4])
{
    XMFLOAT4X4 f;
    XMStoreFloat4x4(&f, XMMatrixTranspose(m));
    memcpy(rows, &f, sizeof(f));
}

} // namespace

bool Device::CreateSunResources()
{
    D3D11_BUFFER_DESC cb = { sizeof(SunConstants), D3D11_USAGE_DYNAMIC,
        D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    if (FAILED(m_device->CreateBuffer(&cb, nullptr, &m_sunCB))) return false;
    // Everything but the map itself is small and created up front, so shadows can be switched
    // on from the overlay; the map follows [Lighting] ShadowResolution (EnsureShadowMap).
    cb.ByteWidth = (sizeof(ResolveConstants) + 15) & ~15u;
    if (FAILED(m_device->CreateBuffer(&cb, nullptr, &m_resolveCB))) return false;
    cb.ByteWidth = sizeof(float) * 4 * 33; // Grass: pushers[32], grassParams
    if (FAILED(m_device->CreateBuffer(&cb, nullptr, &m_grassCB))) return false;

    D3D11_SAMPLER_DESC sm = {};
    sm.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sm.BorderColor[0] = sm.BorderColor[1] = sm.BorderColor[2] = sm.BorderColor[3] = 1;
    sm.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL; sm.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = TRUE; depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = FALSE; // casters above the light box still cast
    raster.SlopeScaledDepthBias = 1.5f; raster.DepthBiasClamp = 0.002f;
    D3D11_BLEND_DESC blend = {};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
        D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (FAILED(m_device->CreateSamplerState(&sm, &m_sunSampler)) ||
        FAILED(m_device->CreateDepthStencilState(&depth, &m_sunDepth)) ||
        FAILED(m_device->CreateRasterizerState(&raster, &m_sunRaster)) ||
        FAILED(m_device->CreateBlendState(&blend, &m_multiplyBlend))) return false;

    auto vs = CompileHLSL(kResolveVS, "vs_4_0", "sun_resolve_vs");
    auto ps = CompileHLSL(std::string("#define MSAA 0\n") + kResolvePS, "ps_4_1", "sun_resolve_ps");
    auto psMS = CompileHLSL(std::string("#define MSAA 1\n") + kResolvePS, "ps_4_1", "sun_resolve_ps_ms");
    if (!vs || !ps || !psMS) return false;
    if (FAILED(m_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_resolveVS)) ||
        FAILED(m_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_resolvePS)) ||
        FAILED(m_device->CreatePixelShader(psMS->GetBufferPointer(), psMS->GetBufferSize(), nullptr, &m_resolvePSMS)))
        return false;
    Log("Sun shadows: enabled %d, %d map, distance %.0f, strength %.2f, sun %s, grass %d, replacement lighting %d, debug %d",
        g_config.sunLighting, g_config.shadowResolution, g_config.shadowDistance, g_config.shadowStrength, g_config.sunFromGame ? "from game" : "fixed",
        g_config.grassShadows, g_config.replaceLighting, g_config.lightingDebug);
    return true;
}

// Called on the first full-screen terrain draw of a frame: c0..c3 is the camera's world-to-clip.
bool Device::EnsureShadowMap()
{
    const UINT size = (UINT)g_config.shadowResolution;
    if (m_sunMap && m_sunMapSize == size) return true;
    m_sunSRV.Reset(); m_sunDSV.Reset(); m_sunMap.Reset();
    m_sunMapSize = 0;
    D3D11_TEXTURE2D_DESC td = { size, size, 1, 1, DXGI_FORMAT_R32_TYPELESS, { 1, 0 }, D3D11_USAGE_DEFAULT,
        D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
    dd.Format = DXGI_FORMAT_D32_FLOAT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_sunMap)) ||
        FAILED(m_device->CreateDepthStencilView(m_sunMap.Get(), &dd, &m_sunDSV)) ||
        FAILED(m_device->CreateShaderResourceView(m_sunMap.Get(), &sd, &m_sunSRV))) {
        LogOnce("sun-map-fail", "Shadow map %u creation failed", size);
        m_sunSRV.Reset(); m_sunDSV.Reset(); m_sunMap.Reset();
        return false;
    }
    m_sunMapSize = size;
    Log("Shadow map %ux%u created", size, size);
    return true;
}

void Device::StartSunFrame()
{
    if (g_config.sunLighting && !EnsureShadowMap()) return;
    XMMATRIX vp;
    float rows[4][4];
    memcpy(rows, m_vsConst, sizeof(rows));
    LoadRows(rows, vp);
    XMVECTOR det;
    XMMATRIX inv = XMMatrixInverse(&det, vp);
    if (std::fabs(XMVectorGetX(det)) < 1e-20f) return;
    StoreRows(inv, m_sun.clipToWorld);

    // JPOG's world is Y-down: "up" is -Y (camera above the park at y = -55, palm crowns at
    // y = -6 over ground at -1). SunX/Y/Z in the ini use Y-up, so Y is negated here.
    XMVECTOR sun = g_config.sunFromGame && m_gameSunValid
        ? XMVectorSet(m_gameSun[0], m_gameSun[1], m_gameSun[2], 0)
        : XMVectorSet(g_config.sunDirection[0], -g_config.sunDirection[1], g_config.sunDirection[2], 0);
    sun = XMVector3Normalize(sun);
    float elevation = -XMVectorGetY(sun);
    if (elevation < g_config.sunMinElevation) {
        // Keep the azimuth, raise the sun: grazing light gives endless, unstable shadows.
        XMVECTOR flat = XMVectorSet(XMVectorGetX(sun), 0, XMVectorGetZ(sun), 0);
        float fl = XMVectorGetX(XMVector3Length(flat));
        flat = fl > 1e-4f ? flat / fl : XMVectorSet(1, 0, 0, 0);
        float h = g_config.sunMinElevation;
        sun = XMVectorSetY(flat * std::sqrt(1 - h * h), -h);
    }
    XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(m_sun.direction), sun);

    // The light box covers the whole view frustum cut at the shadow distance. Guessing the ground
    // height from the terrain chunk offset put the box beside the view when zoomed in (terrain
    // heights are added on top of that offset).
    const float groundY = m_vsConst[4][1];
    const float maxDist = g_config.shadowDistance;
    XMMATRIX lightView = XMMatrixLookToLH(XMVectorZero(), -sun,
        std::fabs(XMVectorGetY(sun)) > 0.99f ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0));
    XMVECTOR lo = XMVectorReplicate(1e30f), hi = XMVectorReplicate(-1e30f);
    for (int iy = -1; iy <= 1; iy += 2) {
        for (int ix = -1; ix <= 1; ix += 2) {
            XMVECTOR n = XMVector3TransformCoord(XMVectorSet((float)ix, (float)iy, 0, 1), inv);
            XMVECTOR f = XMVector3TransformCoord(XMVectorSet((float)ix, (float)iy, 1, 1), inv);
            XMVECTOR ray = f - n;
            float len = XMVectorGetX(XMVector3Length(ray));
            if (!(len > 1e-6f)) continue;
            XMVECTOR farPt = n + ray * ((len < maxDist ? len : maxDist) / len);
            XMVECTOR a = XMVector3TransformCoord(n, lightView), b = XMVector3TransformCoord(farPt, lightView);
            lo = XMVectorMin(lo, XMVectorMin(a, b));
            hi = XMVectorMax(hi, XMVectorMax(a, b));
        }
    }
    XMFLOAT3 l, h;
    XMStoreFloat3(&l, lo); XMStoreFloat3(&h, hi);
    if (!(l.x < h.x) || !std::isfinite(l.x + l.y + l.z + h.x + h.y + h.z)) return;
    const float res = (float)(m_sunMapSize ? m_sunMapSize : 4096);
    float size = (h.x - l.x > h.y - l.y ? h.x - l.x : h.y - l.y) * 1.15f + 16.0f;
    size = std::ceil(size / 32.0f) * 32.0f; // stable texel size while the camera pans
    const float texel = size / res;
    float cx = std::floor((l.x + h.x) * 0.5f / texel) * texel;
    float cy = std::floor((l.y + h.y) * 0.5f / texel) * texel;
    // Casters toward the sun (trees, buildings, hills off screen) sit well in front of the ground.
    float zNear = l.z - maxDist * 0.5f - 200.0f, zFar = h.z + 100.0f;
    XMMATRIX proj = XMMatrixOrthographicOffCenterLH(cx - size / 2, cx + size / 2, cy - size / 2, cy + size / 2, zNear, zFar);
    StoreRows(lightView * proj, m_sun.writeRows);
    memcpy(m_sun.readRows, m_sun.writeRows, sizeof(m_sun.readRows));
    m_sunDepthRange = zFar - zNear;
    m_sunTexelWorld = texel;
    m_sun.settings[0] = g_config.sunAmbient; m_sun.settings[1] = g_config.sunDirect;
    m_sun.settings[2] = g_config.shadowStrength; m_sun.settings[3] = 1.0f / res;
    if (g_config.sunLighting && m_sunDSV) m_ctx->ClearDepthStencilView(m_sunDSV.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    m_sunStarted = true;
    m_pusherCount = 0;
    m_grassUploaded = false;
    LogOnce("sun-viewport", "Sun camera viewport %lu,%lu %lux%lu depth %.3f..%.3f; camera rows z %g %g %g %g w %g %g %g %g",
            m_viewport.X, m_viewport.Y, m_viewport.Width, m_viewport.Height, m_viewport.MinZ, m_viewport.MaxZ,
            rows[2][0], rows[2][1], rows[2][2], rows[2][3], rows[3][0], rows[3][1], rows[3][2], rows[3][3]);
    m_segmentCasters = 0;
    TraceSun("Sun segment start: ground %.1f, box %.0f, camera z row %g %g %g %g, w row %g %g %g %g", groundY, size,
             rows[2][0], rows[2][1], rows[2][2], rows[2][3], rows[3][0], rows[3][1], rows[3][2], rows[3][3]);
    LogOnce("sun-start", "Sun frame: camera ground y %.1f, light box %.0f units (%.3f per texel), sun %.2f %.2f %.2f (%s)",
            groundY, size, texel, XMVectorGetX(sun), XMVectorGetY(sun), XMVectorGetZ(sun),
            g_config.sunFromGame && m_gameSunValid ? "game" : "fixed");
}

// Skinned shaders light with N . -c89, where N is in the space that c0..c3 maps to clip.
// Mapping -c89 through c0 and the inverse camera gives the game's sun in world space.
void Device::CaptureGameSun()
{
    XMVECTOR l = XMVectorSet(-m_vsConst[89][0], -m_vsConst[89][1], -m_vsConst[89][2], 0);
    if (XMVectorGetX(XMVector3LengthSq(l)) < 1e-8f) return;
    float c0[4][4];
    memcpy(c0, m_vsConst, sizeof(c0));
    XMMATRIX modelToClip, clipToWorld;
    LoadRows(c0, modelToClip);
    LoadRows(m_sun.clipToWorld, clipToWorld);
    XMMATRIX model = modelToClip * clipToWorld;
    XMVECTOR w = XMVector3TransformNormal(l, model);
    float len = XMVectorGetX(XMVector3Length(w));
    if (!(len > 1e-6f) || !std::isfinite(len)) return;
    w = w / len;
    // In-game this comes out as e.g. (0.47 -0.74 0.47): negative Y is up in JPOG, so the sun
    // is above the island. Should it ever point below the ground, mirror it up.
    if (XMVectorGetY(w) > 0) w = -w;
    XMFLOAT4X4 mf;
    XMStoreFloat4x4(&mf, model);
    LogOnce("sun-game", "Game sun (skin c89) %.3f %.3f %.3f; model matrix w column %.4f %.4f %.4f %.4f; terrain c22 %.3f %.3f %.3f",
            XMVectorGetX(w), XMVectorGetY(w), XMVectorGetZ(w), mf._14, mf._24, mf._34, mf._44,
            m_vsConst[22][0], m_vsConst[22][1], m_vsConst[22][2]);
    if (!m_gameSunValid) {
        XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(m_gameSun), w);
        m_gameSunValid = true;
        return;
    }
    XMVECTOR prev = XMLoadFloat3(reinterpret_cast<XMFLOAT3*>(m_gameSun));
    XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(m_gameSun), XMVector3Normalize(XMVectorLerp(prev, w, 0.05f)));
}

// Grass pushers: a sample of the draw's own vertices is skinned on the CPU exactly like the
// skinned vertex shader does (bone rows at c[5 + a0], a0 = c4 * index, weighted by v2), then
// mapped to world through c0..c3 and the inverse camera. The game uploads the whole 30-bone
// palette for every skinned draw, so the creature test is the number of *distinct* bones the
// sampled vertices use: props and buildings use one or a few, animals many.
void Device::CapturePusher(UINT firstVertex, UINT numVertices)
{
    if (m_pusherCount >= 32 || !numVertices) return;
    const size_t idx = m_vsHandle >> 1;
    if (idx >= m_vertexShaders.size() || !m_vertexShaders[idx]) return;
    const VSDecl& decl = m_vertexShaders[idx]->parsed;
    const VSInput *pos = nullptr, *weights = nullptr, *indices = nullptr;
    for (const VSInput& in : decl.inputs) {
        if (in.stream != 0) continue;
        if (in.reg == 0 && in.type == 2) pos = &in;
        if (in.reg == 2 && in.type == 4) weights = &in;
        if (in.reg == 3 && in.type == 4) indices = &in;
    }
    VertexBuffer* vb = m_streams[0].vb;
    const UINT stride = m_streams[0].stride;
    if (!pos || !weights || !indices || !vb || !stride) return;
    const std::vector<BYTE>& data = vb->m_impl.m_shadow;
    if ((size_t)(firstVertex + numVertices) * stride > data.size()) return;

    float c0[4][4];
    memcpy(c0, m_vsConst, sizeof(c0));
    XMMATRIX modelToClip, clipToWorld;
    LoadRows(c0, modelToClip);
    LoadRows(m_sun.clipToWorld, clipToWorld);
    const XMMATRIX model = modelToClip * clipToWorld;
    auto color = [](const BYTE* b) { // D3DCOLOR as the shader sees it: (R, G, B, A)
        return XMFLOAT4(b[2] / 255.0f, b[1] / 255.0f, b[0] / 255.0f, b[3] / 255.0f);
    };
    BYTE used[256] = {};
    UINT distinct = 0;
    XMVECTOR lo = XMVectorReplicate(1e30f), hi = XMVectorReplicate(-1e30f);
    const UINT samples = numVertices < 48 ? numVertices : 48;
    for (UINT k = 0; k < samples; k++) {
        const BYTE* v = data.data() + (size_t)(firstVertex + (UINT)((uint64_t)k * numVertices / samples)) * stride;
        float p[4];
        memcpy(p, v + pos->offset, 12);
        p[3] = 1;
        XMFLOAT4 w = color(v + weights->offset), i = color(v + indices->offset);
        const float wv[4] = { w.x, w.y, w.z, w.w }, iv[4] = { i.x, i.y, i.z, i.w };
        float out[3] = {};
        for (int j = 0; j < 4; j++) {
            if (wv[j] <= 0) continue;
            const int a0 = (int)std::floor(m_vsConst[4][j] * iv[j] + 0.001f);
            if (a0 < 0 || 5 + a0 + 2 >= kMaxVSConst) continue;
            if (!used[a0 & 255]) { used[a0 & 255] = 1; distinct++; }
            for (int r = 0; r < 3; r++) {
                const float* row = m_vsConst[5 + a0 + r];
                out[r] += wv[j] * (row[0] * p[0] + row[1] * p[1] + row[2] * p[2] + row[3] * p[3]);
            }
        }
        XMVECTOR wp = XMVector3TransformCoord(XMVectorSet(out[0], out[1], out[2], 1), model);
        lo = XMVectorMin(lo, wp);
        hi = XMVectorMax(hi, wp);
    }
    XMFLOAT3 l, h;
    XMStoreFloat3(&l, lo); XMStoreFloat3(&h, hi);
    if (!std::isfinite(l.x + l.z + h.x + h.z) || h.x - l.x > 200 || h.z - l.z > 200) return;
    TraceSun("Skinned draw: %u distinct bones, box x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f",
             distinct, l.x, h.x, l.y, h.y, l.z, h.z);
    if (distinct < (UINT)g_config.grassMinBones) return;
    // Body parts of one animal come as separate draws: merge overlapping footprints.
    for (UINT k = 0; k < m_pusherCount; k++) {
        float* b = m_pushBoxes[k];
        if (l.x <= b[2] && h.x >= b[0] && l.z <= b[3] && h.z >= b[1]) {
            b[0] = b[0] < l.x ? b[0] : l.x; b[1] = b[1] < l.z ? b[1] : l.z;
            b[2] = b[2] > h.x ? b[2] : h.x; b[3] = b[3] > h.z ? b[3] : h.z;
            m_grassUploaded = false;
            return;
        }
    }
    float* b = m_pushBoxes[m_pusherCount];
    b[0] = l.x; b[1] = l.z; b[2] = h.x; b[3] = h.z;
    m_pushers[m_pusherCount][1] = h.y; // Y-down: the larger y is the lowest point (feet)
    m_pusherCount++;
    m_grassUploaded = false;
    LogOnce("grass-first", "Grass pusher found: %u distinct bones, box %.1f x %.1f", distinct, h.x - l.x, h.z - l.z);
}
void Device::UploadGrass()
{
    if (!m_grassCB) return;
    if (!m_grassUploaded) {
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(m_ctx->Map(m_grassCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            float* f = (float*)map.pData;
            // Footprint circle: box centre, 0.4 of the longer side (animals are long, not round).
            for (UINT k = 0; k < m_pusherCount; k++) {
                const float* b = m_pushBoxes[k];
                float r = 0.4f * ((b[2] - b[0]) > (b[3] - b[1]) ? (b[2] - b[0]) : (b[3] - b[1]));
                r = (r < 0.8f ? 0.8f : (r > 20.0f ? 20.0f : r)) * g_config.grassRadius;
                m_pushers[k][0] = (b[0] + b[2]) * 0.5f; m_pushers[k][2] = (b[1] + b[3]) * 0.5f; m_pushers[k][3] = r;
            }
            memcpy(f, m_pushers, sizeof(m_pushers));
            // Only the main view's grass reacts (the 256x256 offscreen pass has its own camera).
            const bool on = g_config.grassPhysics && m_worldDraw;
            f[128] = on ? (float)m_pusherCount : 0.0f;
            f[129] = g_config.grassPush;
            f[130] = g_config.grassFlatten;
            f[131] = 0;
            m_ctx->Unmap(m_grassCB.Get(), 0);
            m_grassUploaded = on;
        }
    }
    m_ctx->VSSetConstantBuffers(3, 1, m_grassCB.GetAddressOf());
}
void Device::UpdateSun(bool shadowPass)
{
    if (!SceneFx()) {
        m_worldDraw = false;
    } else {
        if (m_worldDraw && m_worldKind == 2 && !m_sunStarted && !m_sunResolved) StartSunFrame();
        if (!m_sunStarted) {
            if (m_worldCaster) m_earlyCasters++;
            m_worldDraw = false;
        } else if (m_sunResolved && m_worldCaster) {
            m_lateWorldDraws++;
        }
        if (m_worldDraw && m_worldKind == 1 && !m_sunResolved && !shadowPass) {
            CaptureGameSun();
        }
    }
    m_sun.flags[0] = m_worldDraw ? 1.0f : 0.0f;
    m_sun.flags[1] = shadowPass ? 1.0f : 0.0f;
    m_sun.flags[2] = 0;
    m_sun.flags[3] = (float)m_worldKind;
    m_sun.control[0] = g_config.replaceLighting ? 1.0f : 0.0f;
    m_sun.control[1] = (float)g_config.lightingDebug;
    if (!m_sunUploadedValid || memcmp(&m_sunUploaded, &m_sun, sizeof(m_sun))) {
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(m_ctx->Map(m_sunCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            memcpy(map.pData, &m_sun, sizeof(m_sun)); m_ctx->Unmap(m_sunCB.Get(), 0);
            m_sunUploaded = m_sun; m_sunUploadedValid = true;
        }
    }
    m_ctx->VSSetConstantBuffers(2, 1, m_sunCB.GetAddressOf());
    if (m_worldKind == 4) UploadGrass();
    m_ctx->PSSetConstantBuffers(1, 1, m_sunCB.GetAddressOf());
}

void Device::DrawSunShadow(bool indexed, UINT count, UINT start, INT base)
{
    if (!g_config.sunLighting || !m_worldCaster || !m_sunStarted || m_sunResolved || !m_sunDSV) return;
    UpdateSun(true);
    ID3D11PixelShader* depthPixel = GetPixelShader();
    if (!depthPixel) { UpdateSun(false); return; }
    m_ctx->PSSetShader(depthPixel, nullptr, 0);
    m_lastPS = nullptr;
    m_ctx->OMSetRenderTargets(0, nullptr, m_sunDSV.Get());
    D3D11_VIEWPORT vp = { 0, 0, (float)m_sunMapSize, (float)m_sunMapSize, 0, 1 };
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->RSSetState(m_sunRaster.Get());
    m_ctx->OMSetDepthStencilState(m_sunDepth.Get(), 0);
    m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xffffffff);
    if (indexed) m_ctx->DrawIndexed(count, start, base); else m_ctx->Draw(count, start);
    ++m_shadowDraws;
    ++m_segmentCasters;
    // Vertex/pixel shaders, streams and constants remain valid. PrepareDraw restores targets,
    // viewport and the sun-pass flag before the next game draw.
    m_rtDirty = m_viewportDirty = true;
    m_lastBlend = nullptr; m_lastDepth = nullptr; m_lastRaster = nullptr;
}

// End of the world pass: copies the scene and redraws it with sun shadows and the [Post]
// sunlight/colour grade applied, before the HUD is drawn on top.
void Device::ResolveSun(const char* reason)
{
    if (!m_sunStarted || m_sunResolved) return;
    m_sunResolved = true;
    TraceSun("Resolve at %s: %u casters in this segment", reason, m_segmentCasters);
    const bool shadows = g_config.sunLighting && m_segmentCasters && m_sunSRV;
    if (!shadows && !g_config.postEnabled) return;
    if (!m_resolvePS || !m_backBuffer || !m_autoDepth || !m_autoDepth->m_srv || !EnsureSceneCopy()) return;
    if (shadows && g_config.lightingDebug == 8 && !m_dumped) {
        if (!m_dumpAt) m_dumpAt = m_frame + 600;
        else if (m_frame >= m_dumpAt) { DumpSunFrame(); m_dumped = true; }
    }

    ResolveConstants rc = {};
    memcpy(rc.clipToWorld, m_sun.clipToWorld, sizeof(rc.clipToWorld));
    memcpy(rc.lightRows, m_sun.writeRows, sizeof(rc.lightRows));
    memcpy(rc.sunDir, m_sun.direction, sizeof(rc.sunDir));
    rc.params[0] = g_config.shadowStrength;
    rc.params[1] = m_sunMapSize ? 1.0f / m_sunMapSize : 0.0f;
    rc.params[2] = 1.5f * m_sunTexelWorld / m_sunDepthRange;
    rc.params[3] = 1.5f * m_sunTexelWorld;
    const float w = (float)m_backBuffer->m_width, h = (float)m_backBuffer->m_height;
    rc.screen[0] = 1.0f / w; rc.screen[1] = 1.0f / h;
    rc.screen[2] = -1.0f / w; rc.screen[3] = 1.0f / h; // VSExtraConstants::pixelOffset
    rc.misc[0] = (float)g_config.lightingDebug;
    rc.misc[1] = g_config.shadowDistance;
    rc.misc[2] = m_sunDepthRange;
    rc.misc[3] = shadows ? 1.0f : 0.0f;
    rc.grade0[0] = g_config.exposure; rc.grade0[1] = g_config.contrast;
    rc.grade0[2] = g_config.saturation; rc.grade0[3] = g_config.warmth;
    rc.grade1[0] = g_config.sunBoost; rc.grade1[1] = g_config.shadowCool;
    rc.grade1[2] = g_config.postEnabled ? 1.0f : 0.0f;
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m_ctx->Map(m_resolveCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    memcpy(map.pData, &rc, sizeof(rc));
    m_ctx->Unmap(m_resolveCB.Get(), 0);

    CopySceneColor();
    ID3D11RenderTargetView* rtv = m_backBuffer->Rtv();
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, w, h, 0, 1 };
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->RSSetState(m_noCull.Get());
    m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xffffffff);
    m_ctx->OMSetDepthStencilState(m_noDepth.Get(), 0);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_resolveVS.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_autoDepth->m_samples > 1 ? m_resolvePSMS.Get() : m_resolvePS.Get(), nullptr, 0);
    m_ctx->PSSetConstantBuffers(0, 1, m_resolveCB.GetAddressOf());
    ID3D11ShaderResourceView* srvs[3] = { m_autoDepth->m_srv.Get(), shadows ? m_sunSRV.Get() : nullptr, m_sceneCopySRV.Get() };
    m_ctx->PSSetShaderResources(0, 3, srvs);
    m_ctx->PSSetSamplers(0, 1, m_sunSampler.GetAddressOf());
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[3] = {};
    m_ctx->PSSetShaderResources(0, 3, nulls);

    // Everything above has to be rebound by the next game draw.
    m_rtDirty = m_viewportDirty = true;
    m_lastVS = nullptr; m_lastPS = nullptr; m_lastLayout = nullptr;
    m_lastBlend = nullptr; m_lastDepth = nullptr; m_lastRaster = nullptr;
    LogOnce(std::string("sun-resolve-") + reason, "Scene pass at %s: shadows %d (%u casters), post %d",
            reason, shadows, m_segmentCasters, g_config.postEnabled);
}

bool Device::EnsureSceneCopy()
{
    const UINT w = m_backBuffer->m_width, h = m_backBuffer->m_height;
    if (m_sceneCopy && m_sceneCopyW == w && m_sceneCopyH == h) return true;
    m_sceneCopySRV.Reset(); m_sceneCopy.Reset();
    D3D11_TEXTURE2D_DESC td = { w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_sceneCopy)) ||
        FAILED(m_device->CreateShaderResourceView(m_sceneCopy.Get(), nullptr, &m_sceneCopySRV))) {
        m_sceneCopy.Reset();
        return false;
    }
    m_sceneCopyW = w; m_sceneCopyH = h;
    return true;
}

// Back buffer -> m_sceneCopy (resolving MSAA samples when the back buffer is multisampled).
void Device::CopySceneColor()
{
    if (m_backBuffer->m_samples > 1)
        m_ctx->ResolveSubresource(m_sceneCopy.Get(), 0, m_backBuffer->m_tex.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    else
        m_ctx->CopyResource(m_sceneCopy.Get(), m_backBuffer->m_tex.Get());
}
// DebugView=8: writes the shadow map, scene depth, back buffer and matrices once to
// d3d8to11_dump\ for offline analysis (tools/sundump.py).
void Device::DumpSunFrame()
{
    if (m_autoDepth->m_samples > 1) { Log("Sun frame dump needs MSAA off"); return; }
    const std::string dir = g_gameDir + "d3d8to11_dump\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    auto save = [&](ID3D11Texture2D* tex, const char* name, UINT bytesPerPixel) {
        if (!tex) return;
        D3D11_TEXTURE2D_DESC td;
        tex->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &staging))) { Log("dump: staging %s failed", name); return; }
        m_ctx->CopyResource(staging.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(m_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return;
        if (FILE* f = _fsopen((dir + name).c_str(), "wb", _SH_DENYNO)) {
            for (UINT y = 0; y < td.Height; y++) fwrite((BYTE*)map.pData + (size_t)y * map.RowPitch, bytesPerPixel, td.Width, f);
            fclose(f);
        }
        m_ctx->Unmap(staging.Get(), 0);
    };
    save(m_sunMap.Get(), "sunmap.f32", 4);
    save(m_autoDepth->m_tex.Get(), "depth.d24s8", 4);
    save(m_backBuffer->m_tex.Get(), "color.bgra", 4);
    if (FILE* f = _fsopen((dir + "meta.txt").c_str(), "w", _SH_DENYNO)) {
        auto rows = [&](const char* n, const float (&m)[4][4]) {
            fprintf(f, "%s", n);
            for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) fprintf(f, " %.9g", m[r][c]);
            fprintf(f, "\n");
        };
        fprintf(f, "screen %u %u\nmap %u\n", m_backBuffer->m_width, m_backBuffer->m_height, m_sunMapSize);
        rows("clipToWorld", m_sun.clipToWorld);
        rows("lightRows", m_sun.writeRows);
        fprintf(f, "sun %.6f %.6f %.6f\ntexel %.6f\ndepthRange %.6f\ncasters %u\n", m_sun.direction[0], m_sun.direction[1],
                m_sun.direction[2], m_sunTexelWorld, m_sunDepthRange, m_segmentCasters);
        fclose(f);
    }
    Log("Sun frame dumped to d3d8to11_dump (frame %llu)", m_frame);
}

void Device::TraceSun(const char* fmt, ...)
{
    if (!m_trace) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (m_traceLast == line) { m_traceRepeat++; return; }
    TraceFlush();
    m_traceLast = line;
    m_traceRepeat = 1;
}

void Device::TraceFlush()
{
    if (m_trace && m_traceRepeat) {
        char head[16];
        sprintf_s(head, "%4ux ", m_traceRepeat);
        m_traceBuf += head + m_traceLast + "\n";
    }
    m_traceRepeat = 0;
    m_traceLast.clear();
}

// Present: finish a frame that never reached a HUD draw, then reset per-frame state.
void Device::EndSunFrame()
{
    ResolveSun("present");
    if (m_trace) {
        TraceFlush();
        // Keep only a frame that actually rendered the park with shadows.
        if (m_shadowDraws) {
            if (FILE* f = _fsopen((g_gameDir + "d3d8to11_frame.log").c_str(), "w", _SH_DENYNO)) {
                fputs(m_traceBuf.c_str(), f);
                fclose(f);
            }
            m_trace = false;
            m_traceDone = true;
            Log("Frame trace written to d3d8to11_frame.log (frame %llu)", m_frame);
        }
        m_traceBuf.clear();
    }
    if (g_config.sunLighting && !m_traceDone && !m_trace) {
        if (!m_traceAt && m_shadowDraws) m_traceAt = m_frame + 1000;
        if (m_traceAt && m_frame + 1 >= m_traceAt) m_trace = true;
    }
    if (m_sunStarted && (m_earlyCasters || m_lateWorldDraws))
        LogOnce("sun-order", "Sun frame order: %u caster draws before the terrain, %u world draws after the resolve",
                m_earlyCasters, m_lateWorldDraws);
    m_lastCasters = m_shadowDraws;
    m_sunStarted = m_sunResolved = false;
    m_shadowDraws = m_earlyCasters = m_lateWorldDraws = 0;
}
