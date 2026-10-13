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
    float misc[4];   // debug view, shadow distance, -, -
};

const char* kResolveVS =
    "float4 main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 uv = float2((id << 1) & 2, id & 2);\n"
    "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n";

const char* kResolvePS =
    "cbuffer Resolve : register(b0) { float4 clipToWorld[4]; float4 lightRows[4]; float4 sunDir;\n"
    "  float4 params; float4 screen; float4 misc; };\n"
    "Texture2D<float> sceneDepth : register(t0);\n"
    "Texture2D<float> shadowMap : register(t1);\n"
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
    "float3 WorldAt(int2 p) { return Unproject(p + 0.5, sceneDepth.Load(int3(p, 0))); }\n"
    "float4 main(float4 pos : SV_Position) : SV_Target {\n"
    "  int2 p = int2(pos.xy);\n"
    "  if (misc.x == 5 && pos.x < 540 && pos.y < 540) {\n"
    "    uint mw, mh; shadowMap.GetDimensions(mw, mh);\n"
    "    float v = shadowMap.Load(int3(pos.xy / 540 * mw, 0));\n"
    "    return v >= 1 ? float4(0.2, 0.2, 0.6, 1) : float4(frac(v * misc.z / 2).xxx, 1); }\n"
    "  float d = sceneDepth.Load(int3(p, 0));\n"
    "  if (d >= 0.999999) { if (misc.x >= 3) return float4(1, 0.6, 1, 1); discard; }\n"
    "  float3 P = Unproject(pos.xy, d);\n"
    "  float3 V = Unproject(pos.xy, 0) - P;\n"
    "  float dist = length(V);\n"
    // Normal from depth: per axis take the neighbour on the same surface (smaller step).
    "  float3 L = WorldAt(p - int2(1, 0)), R = WorldAt(p + int2(1, 0));\n"
    "  float3 U = WorldAt(p - int2(0, 1)), D = WorldAt(p + int2(0, 1));\n"
    "  float3 dx = dot(R - P, R - P) < dot(P - L, P - L) ? R - P : P - L;\n"
    "  float3 dy = dot(D - P, D - P) < dot(P - U, P - U) ? D - P : P - U;\n"
    "  float3 N = cross(dy, dx); float nl = dot(N, N);\n"
    "  N = nl > 1e-20 ? N * rsqrt(nl) : V / max(dist, 1e-6);\n"
    "  if (dot(N, V) < 0) N = -N;\n"
    "  if (misc.x == 4) return float4(N * 0.5 + 0.5, 1);\n"
    "  if (misc.x == 6) { float2 g = frac(P.xz / 8); return float4(g.x < 0.08 || g.y < 0.08 ? float3(1, 0, 0) : frac(P.yyy / 4), 1); }\n"
    "  float ndl = dot(N, sunDir.xyz);\n"
    // Offset toward the sun: robust where depth normals are noisy (grass, foliage).
    "  float3 Q = P + sunDir.xyz * params.w * 3;\n"
    "  float4 Q4 = float4(Q, 1);\n"
    "  float3 q = float3(dot(Q4, lightRows[0]), dot(Q4, lightRows[1]), dot(Q4, lightRows[2]));\n"
    "  float2 uv = q.xy * float2(0.5, -0.5) + 0.5;\n"
    "  float edge = saturate(min(min(uv.x, 1 - uv.x), min(uv.y, 1 - uv.y)) * 12);\n"
    "  float vis = 1;\n"
    "  if (edge > 0 && q.z > 0 && q.z < 1) {\n"
    "    float z = q.z - params.z, sum = 0;\n"
    "    [unroll] for (int k = 0; k < 12; k++) sum += shadowMap.SampleCmpLevelZero(cmp, uv + kTaps[k] * params.y * 1.6, z);\n"
    "    vis = sum / 12;\n"
    "  }\n"
    // Faces turned away from the sun are already dark in the game's own lighting.
    "  float facing = lerp(0.6, 1, saturate(ndl * 4));\n"
    "  float fade = saturate((misc.y - dist) / (misc.y * 0.2));\n"
    "  if (misc.x == 7) return float4(vis.xxx, 1);\n"
    "  float shadow = (1 - vis) * edge * facing * fade;\n"
    "  float3 shade = (1 - params.x * shadow) * lerp(float3(1, 1, 1), float3(0.93, 0.96, 1.0), shadow);\n"
    "  return float4(shade, 1); }\n";

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
    if (!g_config.sunLighting) return true;
    cb.ByteWidth = (sizeof(ResolveConstants) + 15) & ~15u;
    if (FAILED(m_device->CreateBuffer(&cb, nullptr, &m_resolveCB))) return false;

    const UINT size = (UINT)g_config.shadowResolution;
    D3D11_TEXTURE2D_DESC td = { size, size, 1, 1, DXGI_FORMAT_R32_TYPELESS, { 1, 0 }, D3D11_USAGE_DEFAULT,
        D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
    dd.Format = DXGI_FORMAT_D32_FLOAT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_sunMap)) ||
        FAILED(m_device->CreateDepthStencilView(m_sunMap.Get(), &dd, &m_sunDSV)) ||
        FAILED(m_device->CreateShaderResourceView(m_sunMap.Get(), &sd, &m_sunSRV))) return false;

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
    auto ps = CompileHLSL(kResolvePS, "ps_4_0", "sun_resolve_ps");
    if (!vs || !ps) return false;
    if (FAILED(m_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_resolveVS)) ||
        FAILED(m_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_resolvePS)))
        return false;
    Log("Sun shadows: %u map, distance %.0f, strength %.2f, sun %s, grass %d, replacement lighting %d, debug %d",
        size, g_config.shadowDistance, g_config.shadowStrength, g_config.sunFromGame ? "from game" : "fixed",
        g_config.grassShadows, g_config.replaceLighting, g_config.lightingDebug);
    return true;
}

// Called on the first full-screen terrain draw of a frame: c0..c3 is the camera's world-to-clip.
void Device::StartSunFrame()
{
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
    const float res = (float)g_config.shadowResolution;
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
    m_ctx->ClearDepthStencilView(m_sunDSV.Get(), D3D11_CLEAR_DEPTH, 1, 0);
    m_sunStarted = true;
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

void Device::UpdateSun(bool shadowPass)
{
    if (!g_config.sunLighting) {
        m_worldDraw = false;
    } else {
        if (m_worldDraw && m_worldKind == 2 && !m_sunStarted && !m_sunResolved) StartSunFrame();
        if (!m_sunStarted) {
            if (m_worldCaster) m_earlyCasters++;
            m_worldDraw = false;
        } else if (m_sunResolved && m_worldCaster) {
            m_lateWorldDraws++;
        }
        if (m_worldDraw && m_worldKind == 1 && !m_sunResolved && !shadowPass) CaptureGameSun();
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
    m_ctx->PSSetConstantBuffers(1, 1, m_sunCB.GetAddressOf());
}

void Device::DrawSunShadow(bool indexed, UINT count, UINT start, INT base)
{
    if (!m_worldCaster || !m_sunStarted || m_sunResolved || !m_sunDSV) return;
    UpdateSun(true);
    ID3D11PixelShader* depthPixel = GetPixelShader();
    if (!depthPixel) { UpdateSun(false); return; }
    m_ctx->PSSetShader(depthPixel, nullptr, 0);
    m_lastPS = nullptr;
    m_ctx->OMSetRenderTargets(0, nullptr, m_sunDSV.Get());
    D3D11_VIEWPORT vp = { 0, 0, (float)g_config.shadowResolution, (float)g_config.shadowResolution, 0, 1 };
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

void Device::ResolveSun(const char* reason)
{
    if (!m_sunStarted || m_sunResolved) return;
    m_sunResolved = true;
    TraceSun("Resolve at %s: %u casters in this segment", reason, m_segmentCasters);
    if (!m_segmentCasters || !m_resolvePS || !m_backBuffer || !m_autoDepth || !m_autoDepth->m_srv) return;
    if (g_config.lightingDebug == 8 && !m_dumped) {
        if (!m_dumpAt) m_dumpAt = m_frame + 600;
        else if (m_frame >= m_dumpAt) { DumpSunFrame(); m_dumped = true; }
    }

    ResolveConstants rc = {};
    memcpy(rc.clipToWorld, m_sun.clipToWorld, sizeof(rc.clipToWorld));
    memcpy(rc.lightRows, m_sun.writeRows, sizeof(rc.lightRows));
    memcpy(rc.sunDir, m_sun.direction, sizeof(rc.sunDir));
    rc.params[0] = g_config.shadowStrength;
    rc.params[1] = 1.0f / g_config.shadowResolution;
    rc.params[2] = 1.5f * m_sunTexelWorld / m_sunDepthRange;
    rc.params[3] = 1.5f * m_sunTexelWorld;
    const float w = (float)m_backBuffer->m_width, h = (float)m_backBuffer->m_height;
    rc.screen[0] = 1.0f / w; rc.screen[1] = 1.0f / h;
    rc.screen[2] = -1.0f / w; rc.screen[3] = 1.0f / h; // VSExtraConstants::pixelOffset
    rc.misc[0] = (float)g_config.lightingDebug;
    rc.misc[1] = g_config.shadowDistance;
    rc.misc[2] = m_sunDepthRange;
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m_ctx->Map(m_resolveCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    memcpy(map.pData, &rc, sizeof(rc));
    m_ctx->Unmap(m_resolveCB.Get(), 0);

    ID3D11RenderTargetView* rtv = m_backBuffer->Rtv();
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, w, h, 0, 1 };
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->RSSetState(m_noCull.Get());
    m_ctx->OMSetBlendState(g_config.lightingDebug >= 3 ? m_noBlend.Get() : m_multiplyBlend.Get(), nullptr, 0xffffffff);
    m_ctx->OMSetDepthStencilState(m_noDepth.Get(), 0);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_resolveVS.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_resolvePS.Get(), nullptr, 0);
    m_ctx->PSSetConstantBuffers(0, 1, m_resolveCB.GetAddressOf());
    ID3D11ShaderResourceView* srvs[2] = { m_autoDepth->m_srv.Get(), m_sunSRV.Get() };
    m_ctx->PSSetShaderResources(0, 2, srvs);
    m_ctx->PSSetSamplers(0, 1, m_sunSampler.GetAddressOf());
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[2] = {};
    m_ctx->PSSetShaderResources(0, 2, nulls);

    // Everything above has to be rebound by the next game draw.
    m_rtDirty = m_viewportDirty = true;
    m_lastVS = nullptr; m_lastPS = nullptr; m_lastLayout = nullptr;
    m_lastBlend = nullptr; m_lastDepth = nullptr; m_lastRaster = nullptr;
    LogOnce(std::string("sun-resolve-") + reason, "Sun shadows resolved at %s: %u caster draws, VS 0x%lx PS 0x%lx",
            reason, m_shadowDraws, m_vsHandle, m_psHandle);
}

// DebugView=8: writes the shadow map, scene depth, back buffer and matrices once to
// d3d8to11_dump\ for offline analysis (tools/sundump.py).
void Device::DumpSunFrame()
{
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
        fprintf(f, "screen %u %u\nmap %d\n", m_backBuffer->m_width, m_backBuffer->m_height, g_config.shadowResolution);
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
    m_sunStarted = m_sunResolved = false;
    m_shadowDraws = m_earlyCasters = m_lateWorldDraws = 0;
}
