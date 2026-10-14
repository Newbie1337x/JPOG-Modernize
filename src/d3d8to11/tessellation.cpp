// tessellation.cpp - PN-triangle smoothing of low-poly skinned models (docs/TESSELLATION.md).
// Eligible draws (main view, skinned, triangle lists, dinosaur textures by default) are drawn as
// 3-control-point patches through the hull/domain shaders from shaders.cpp. The stages are bound
// for that single draw only: every other pass keeps the plain VS -> PS pipeline.
#include "device.h"

bool Device::TessellationCandidate() const
{
    if (!g_config.effectsEnabled || !g_config.tessellation || m_tessFailed || !m_worldDraw || m_worldKind != 1)
        return false;
    if (m_device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) return false;
    if (!g_config.tessDinosOnly) return true;
    if (!m_textures[0] || m_textures[0]->GetType() != D8RTYPE_TEXTURE) return false;
    const std::string& name = static_cast<Texture*>(static_cast<ID8Texture*>(m_textures[0]))->m_name;
    return name.compare(0, 6, "dinos_") == 0;
}

bool Device::BeginTessellation(DWORD type, D3D11_PRIMITIVE_TOPOLOGY& topo)
{
    if (!m_tessCandidate) return false;
    // Strips are expanded to lists by the caller (device.cpp StripToList).
    if (type != D8PT_TRIANGLELIST && type != D8PT_TRIANGLESTRIP) {
        LogOnce("tess-topology-" + std::to_string(type), "tessellation: skipped a candidate draw with primitive type %lu", type);
        return false;
    }
    if (!m_tessHS || m_tessBuiltFlip != g_config.tessFlipWinding) {
        // The optimizer emits invalid fork/join phases for the PN patch constants (fxc X8000).
        auto hs = CompileHLSL(TessellationHS(g_config.tessFlipWinding), "hs_5_0", "tess_hs", false);
        auto ds = m_tessDS ? nullptr : CompileHLSL(TessellationDS(), "ds_5_0", "tess_ds");
        m_tessHS.Reset();
        if (!hs || FAILED(m_device->CreateHullShader(hs->GetBufferPointer(), hs->GetBufferSize(), nullptr, &m_tessHS)) ||
            (!m_tessDS && (!ds || FAILED(m_device->CreateDomainShader(ds->GetBufferPointer(), ds->GetBufferSize(), nullptr, &m_tessDS))))) {
            m_tessFailed = true;
            Log("tessellation: shader creation failed, disabled");
            return false;
        }
        m_tessBuiltFlip = g_config.tessFlipWinding;
    }
    if (!m_tessCB) {
        D3D11_BUFFER_DESC d = { sizeof(TessConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        if (FAILED(m_device->CreateBuffer(&d, nullptr, &m_tessCB))) { m_tessFailed = true; Log("tessellation: constant buffer failed, disabled"); return false; }
    }
    TessConstants c = {};
    memcpy(c.viewProj, m_waterViewProjection, sizeof(c.viewProj));
    c.params[0] = g_config.tessMaxFactor;
    c.params[1] = g_config.tessPixels;
    c.params[2] = g_config.tessCurvature;
    c.screen[0] = (float)m_viewport.Width;
    c.screen[1] = (float)m_viewport.Height;
    // Relief from the stage-0 colour texture, bound to the domain shader for this draw.
    ID3D11ShaderResourceView* height = nullptr;
    if (g_config.tessDisplacement > 0 && m_textures[0] && m_textures[0]->GetType() == D8RTYPE_TEXTURE)
        height = static_cast<Texture*>(static_cast<ID8Texture*>(m_textures[0]))->m_srv.Get();
    if (height && !m_tessSampler) {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(m_device->CreateSamplerState(&sd, &m_tessSampler))) height = nullptr;
    }
    c.displace[0] = height ? g_config.tessDisplacement * (g_config.tessInvertRelief ? -1.0f : 1.0f) : 0.0f;
    c.displace[1] = g_config.tessDetailMip;
    c.displace[2] = g_config.tessSeamSafe ? 1.0f : 0.0f;
    c.displace[3] = g_config.tessWeld ? 1.0f : 0.0f;
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m_ctx->Map(m_tessCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return false;
    memcpy(map.pData, &c, sizeof(c));
    m_ctx->Unmap(m_tessCB.Get(), 0);
    m_ctx->HSSetShader(m_tessHS.Get(), nullptr, 0);
    m_ctx->DSSetShader(m_tessDS.Get(), nullptr, 0);
    m_ctx->HSSetConstantBuffers(5, 1, m_tessCB.GetAddressOf());
    m_ctx->DSSetConstantBuffers(5, 1, m_tessCB.GetAddressOf());
    if (height) {
        m_ctx->DSSetShaderResources(0, 1, &height);
        m_ctx->DSSetSamplers(0, 1, m_tessSampler.GetAddressOf());
    }
    topo = D3D11_PRIMITIVE_TOPOLOGY_6_CONTROL_POINT_PATCHLIST; // drawn by DrawTessellated
    ++m_tessDraws;
    LogOnce("tess-first", "tessellation active (first draw VS 0x%lx PS 0x%lx)", m_vsHandle, m_psHandle);
    return true;
}

// Draws a triangle list (indices relative to baseVertex) as 6-point patches: the triangle plus, per
// corner, the first vertex of this draw sharing its exact bind-pose position (welded in the hull
// shader). Only vertices this draw references are candidates, so their bone indices are valid for
// the current palette.
void Device::DrawTessellated(const std::vector<UINT>& list, INT baseVertex)
{
    if (list.empty()) return;
    std::vector<UINT> canon(list);
    const VSInput* posIn = nullptr;
    size_t vsIdx = m_vsHandle >> 1;
    if (g_config.tessWeld && vsIdx < m_vertexShaders.size() && m_vertexShaders[vsIdx])
        for (const VSInput& in : m_vertexShaders[vsIdx]->parsed.inputs)
            if (in.reg == 0 && in.stream == 0 && in.type == 2) posIn = &in;
    const VertexBuffer* vb = m_streams[0].vb;
    const UINT stride = m_streams[0].stride;
    if (posIn && vb && stride && !vb->m_impl.m_shadow.empty()) {
        const std::vector<BYTE>& data = vb->m_impl.m_shadow;
        struct Key { float p[3]; bool operator==(const Key& o) const { return !memcmp(p, o.p, sizeof(p)); } };
        struct Hash { size_t operator()(const Key& k) const {
            const UINT* u = (const UINT*)k.p; return (u[0] * 73856093u) ^ (u[1] * 19349663u) ^ (u[2] * 83492791u); } };
        std::unordered_map<Key, UINT, Hash> first;
        first.reserve(list.size());
        UINT welded = 0;
        for (size_t i = 0; i < list.size(); i++) {
            size_t at = ((size_t)list[i] + baseVertex) * stride + posIn->offset;
            if (at + sizeof(Key) > data.size()) { canon[i] = list[i]; continue; }
            Key k; memcpy(k.p, &data[at], sizeof(k.p));
            auto it = first.emplace(k, list[i]).first;
            canon[i] = it->second;
            welded += canon[i] != list[i];
        }
        LogOnce("tess-weld", "tessellation: first welded draw has %zu corners, %u at a split vertex", list.size(), welded);
    }
    std::vector<UINT> patches;
    patches.reserve(list.size() * 2);
    for (size_t t = 0; t + 2 < list.size(); t += 3) {
        patches.insert(patches.end(), { list[t], list[t + 1], list[t + 2], canon[t], canon[t + 1], canon[t + 2] });
    }
    UploadDynamicIndices(patches.data(), (UINT)patches.size() * 4, DXGI_FORMAT_R32_UINT);
    m_ctx->DrawIndexed((UINT)patches.size(), 0, baseVertex);
}

void Device::EndTessellation()
{
    m_ctx->HSSetShader(nullptr, nullptr, 0);
    m_ctx->DSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* none = nullptr;
    m_ctx->DSSetShaderResources(0, 1, &none);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}
