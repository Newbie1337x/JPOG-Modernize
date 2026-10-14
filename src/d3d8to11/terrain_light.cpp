// terrain_light.cpp - the game's terrain height field as a GPU texture for the replacement
// lighting (docs/LIGHTING_REPLACEMENT.md): smooth terrain normals for any sun direction and
// horizon shadows from hills and mountains. Heights are read incrementally through the exported
// native accessors (native_terrain.h) and rescanned every few seconds, so terraforming shows up.
#include "device.h"
#include "native_terrain.h"
#include <algorithm>

void Device::UpdateTerrainHeights()
{
    if (!g_config.replaceLighting || !g_config.heightfieldLighting || m_terrainFault || m_terrainFrame == m_frame) return;
    m_terrainFrame = m_frame;
    native_terrain::Bridge bridge;
    if (!bridge.Resolve()) { LogOnce("terrain-light-no-native", "Terrain height field unavailable (native exports missing): terrain keeps the game's baked sun factor"); return; }
    native_terrain::Info info = {};
    if (!native_terrain::InfoSafe(bridge, info)) { m_terrainReady = false; m_terrainNext = 0; return; }
    if (info.field != m_terrainField || info.w != m_terrainW || info.h != m_terrainH) {
        m_terrainField = info.field; m_terrainW = info.w; m_terrainH = info.h;
        m_terrainHeights.assign(size_t(info.w) * info.h, 0);
        m_terrainNext = 0; m_terrainReady = false;
        m_terrainHeightSRV.Reset(); m_terrainHeightTex.Reset();
    }
    const unsigned total = info.w * info.h;
    const ULONGLONG now = GetTickCount64();
    if (m_terrainNext == total) {
        if (now - m_terrainRefresh < 3000) return;
        m_terrainNext = 0; // rescan: the texture keeps the previous heights until it completes
    }
    const unsigned count = (std::min)(16384u, total - m_terrainNext);
    if (!native_terrain::HeightsSafe(bridge, info, m_terrainHeights.data(), m_terrainNext, count)) {
        m_terrainFault = true; m_terrainReady = false;
        Log("Terrain height field scan stopped: invalid terrain read; no engine state modified");
        return;
    }
    m_terrainNext += count;
    if (m_terrainNext < total) return;
    if (!m_terrainHeightTex) {
        D3D11_TEXTURE2D_DESC td = { info.w, info.h, 1, 1, DXGI_FORMAT_R32_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_terrainHeightTex)) ||
            FAILED(m_device->CreateShaderResourceView(m_terrainHeightTex.Get(), nullptr, &m_terrainHeightSRV))) {
            m_terrainFault = true; Log("Terrain height field texture creation failed"); return;
        }
    }
    if (!m_terrainSampler) {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(m_device->CreateSamplerState(&sd, &m_terrainSampler))) { m_terrainFault = true; return; }
    }
    m_ctx->UpdateSubresource(m_terrainHeightTex.Get(), 0, nullptr, m_terrainHeights.data(), info.w * sizeof(float), 0);
    if (!m_terrainReady) Log("Terrain height field for lighting: %ux%u samples", info.w, info.h);
    m_terrainReady = true;
    m_terrainRefresh = now;
}

// Terrain and paths sample the height field at t15 / s11 (ApplySun in shaders.cpp).
void Device::BindTerrainHeights()
{
    // Bound on every eligible draw: other passes (FSR) clear the whole SRV range.
    if (!m_worldDraw || !g_config.replaceLighting || (m_worldKind != 2 && m_worldKind != 3) || !TerrainHeightsActive()) return;
    ID3D11ShaderResourceView* srv = m_terrainHeightSRV.Get();
    m_ctx->PSSetShaderResources(15, 1, &srv);
    m_ctx->PSSetSamplers(11, 1, m_terrainSampler.GetAddressOf());
}
