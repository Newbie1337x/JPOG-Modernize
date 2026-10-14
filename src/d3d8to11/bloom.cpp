// bloom.cpp - glow around bright pixels (night lights, the moon on water, sunlit highlights).
//
// Runs after the scene resolve, before the HUD: the finished world picture is copied, its bright
// part is extracted at quarter resolution, blurred twice (separable, the second pass twice as
// wide) and added back. Strength follows the time of day ([Lighting] Bloom, BloomStrength,
// BloomThreshold, BloomDay); without the time-of-day preview the day strength applies.
#include "device.h"

namespace {
struct BloomConstants {
    float source[4]; // 1/source width, 1/source height, threshold, knee
    float blur[4];   // strength, blur direction x, y (texels), night
};

const char* kBloomCode =
    "cbuffer Bloom : register(b0) { float4 source; float4 blur; };\n"
    "Texture2D<float4> src : register(t0); SamplerState lin : register(s0);\n"
    "float2 Uv(float4 pos, float2 size) { return pos.xy / size; }\n"
    // Bright pass + 4x downsample: four bilinear taps cover the 4x4 source block.
    "float4 BrightPS(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 uv = pos.xy * 4 * source.xy;\n"
    "  float3 c = 0;\n"
    "  [unroll] for (int k = 0; k < 4; k++) {\n"
    "    float2 o = float2((k & 1) ? 1 : -1, (k & 2) ? 1 : -1) * source.xy;\n"
    "    c += src.SampleLevel(lin, uv + o, 0).rgb;\n"
    "  }\n"
    "  c *= 0.25;\n"
    "  float l = max(c.r, max(c.g, c.b));\n"
    "  float w = saturate((l - source.z + source.w) / (2 * source.w));\n"
    "  w = l > source.z + source.w ? 1 : w * w;\n"
    // At night (blur.w) the world is graded toward grey: only coloured light (lamps) blooms.
    "  float chroma = (l - min(c.r, min(c.g, c.b))) / max(l, 1e-3);\n"
    "  w *= lerp(1, saturate((chroma - 0.15) / 0.25), blur.w);\n"
    "  return float4(c * w, 1); }\n"
    // 9-tap gaussian along blur.yz (in this texture's texels).
    "float4 BlurPS(float4 pos : SV_Position) : SV_Target {\n"
    "  uint w, h; src.GetDimensions(w, h);\n"
    "  float2 uv = pos.xy / float2(w, h), d = blur.yz / float2(w, h);\n"
    "  const float k[5] = { 0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162 };\n"
    "  float3 c = src.SampleLevel(lin, uv, 0).rgb * k[0];\n"
    "  [unroll] for (int i = 1; i < 5; i++) c += (src.SampleLevel(lin, uv + d * i, 0).rgb + src.SampleLevel(lin, uv - d * i, 0).rgb) * k[i];\n"
    "  return float4(c, 1); }\n"
    // Upsample (bilinear) and add, through an additive blend.
    "float4 CompositePS(float4 pos : SV_Position) : SV_Target {\n"
    "  float2 uv = pos.xy * source.xy;\n"
    "  return float4(src.SampleLevel(lin, uv, 0).rgb * blur.x, 1); }\n";
}

bool Device::EnsureBloom()
{
    if (m_bloomFailed) return false;
    const UINT w = (m_backBuffer->m_width + 3) / 4, h = (m_backBuffer->m_height + 3) / 4;
    if (!m_bloomBright) {
        auto make = [&](const char* entry, ComPtr<ID3D11PixelShader>& out) {
            const std::string src = std::string(kBloomCode) +
                "float4 main(float4 pos : SV_Position) : SV_Target { return " + entry + "(pos); }\n";
            auto blob = CompileHLSL(src, "ps_4_0", std::string("bloom_") + entry);
            return blob && SUCCEEDED(m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out));
        };
        D3D11_BUFFER_DESC cb = { sizeof(BloomConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (!make("BrightPS", m_bloomBright) || !make("BlurPS", m_bloomBlur) || !make("CompositePS", m_bloomComposite) ||
            FAILED(m_device->CreateBuffer(&cb, nullptr, &m_bloomCB)) ||
            FAILED(m_device->CreateSamplerState(&sd, &m_bloomSampler)) ||
            FAILED(m_device->CreateBlendState(&bd, &m_bloomAdd))) {
            Log("Bloom: shader or state creation failed; bloom disabled");
            m_bloomFailed = true;
            return false;
        }
    }
    if (m_bloomTex[0] && m_bloomW == w && m_bloomH == h) return true;
    for (int i = 0; i < 2; i++) { m_bloomSRV[i].Reset(); m_bloomRTV[i].Reset(); m_bloomTex[i].Reset(); }
    D3D11_TEXTURE2D_DESC td = { w, h, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    for (int i = 0; i < 2; i++) {
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_bloomTex[i])) ||
            FAILED(m_device->CreateRenderTargetView(m_bloomTex[i].Get(), nullptr, &m_bloomRTV[i])) ||
            FAILED(m_device->CreateShaderResourceView(m_bloomTex[i].Get(), nullptr, &m_bloomSRV[i]))) {
            Log("Bloom: target creation failed; bloom disabled");
            m_bloomFailed = true;
            return false;
        }
    }
    m_bloomW = w; m_bloomH = h;
    return true;
}

void Device::ApplyBloom()
{
    if (!g_config.bloom || !g_config.replaceLighting || !m_backBuffer || !EnsureSceneCopy() || !EnsureBloom()) return;
    const float night = m_tod.active ? 1 - m_tod.dayW : 0.0f;
    const float strength = g_config.bloomStrength * (g_config.bloomDay + (1 - g_config.bloomDay) * night);
    if (strength <= 0.001f) return;
    CopySceneColor();

    BloomConstants c = {};
    c.source[0] = 1.0f / m_backBuffer->m_width; c.source[1] = 1.0f / m_backBuffer->m_height;
    // Lower threshold at night: lamps and moonlit water are dim in absolute terms.
    c.source[2] = g_config.bloomThreshold * (1 - 0.35f * night); c.source[3] = 0.12f;
    c.blur[3] = night;
    auto upload = [&](float s, float dx, float dy) {
        c.blur[0] = s; c.blur[1] = dx; c.blur[2] = dy;
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(m_ctx->Map(m_bloomCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { memcpy(map.pData, &c, sizeof(c)); m_ctx->Unmap(m_bloomCB.Get(), 0); }
    };
    ID3D11ShaderResourceView* none[1] = {};
    auto pass = [&](ID3D11PixelShader* ps, ID3D11ShaderResourceView* in, ID3D11RenderTargetView* out, UINT w, UINT h) {
        m_ctx->PSSetShaderResources(0, 1, none);
        m_ctx->OMSetRenderTargets(1, &out, nullptr);
        D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
        m_ctx->RSSetViewports(1, &vp);
        m_ctx->PSSetShader(ps, nullptr, 0);
        m_ctx->PSSetShaderResources(0, 1, &in);
        m_ctx->Draw(3, 0);
    };
    m_ctx->RSSetState(m_noCull.Get());
    m_ctx->OMSetDepthStencilState(m_noDepth.Get(), 0);
    m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xffffffff);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_resolveVS.Get(), nullptr, 0);
    m_ctx->PSSetConstantBuffers(0, 1, m_bloomCB.GetAddressOf());
    m_ctx->PSSetSamplers(0, 1, m_bloomSampler.GetAddressOf());

    upload(strength, 0, 0);
    pass(m_bloomBright.Get(), m_sceneCopySRV.Get(), m_bloomRTV[0].Get(), m_bloomW, m_bloomH);
    upload(strength, 1, 0); pass(m_bloomBlur.Get(), m_bloomSRV[0].Get(), m_bloomRTV[1].Get(), m_bloomW, m_bloomH);
    upload(strength, 0, 1); pass(m_bloomBlur.Get(), m_bloomSRV[1].Get(), m_bloomRTV[0].Get(), m_bloomW, m_bloomH);
    upload(strength, 2, 0); pass(m_bloomBlur.Get(), m_bloomSRV[0].Get(), m_bloomRTV[1].Get(), m_bloomW, m_bloomH);
    upload(strength, 0, 2); pass(m_bloomBlur.Get(), m_bloomSRV[1].Get(), m_bloomRTV[0].Get(), m_bloomW, m_bloomH);

    upload(strength, 0, 0);
    m_ctx->OMSetBlendState(m_bloomAdd.Get(), nullptr, 0xffffffff);
    pass(m_bloomComposite.Get(), m_bloomSRV[0].Get(), m_backBuffer->Rtv(), m_backBuffer->m_width, m_backBuffer->m_height);
    m_ctx->PSSetShaderResources(0, 1, none);
    m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xffffffff);
}
