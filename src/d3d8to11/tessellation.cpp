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
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m_ctx->Map(m_tessCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return false;
    memcpy(map.pData, &c, sizeof(c));
    m_ctx->Unmap(m_tessCB.Get(), 0);
    m_ctx->HSSetShader(m_tessHS.Get(), nullptr, 0);
    m_ctx->DSSetShader(m_tessDS.Get(), nullptr, 0);
    m_ctx->HSSetConstantBuffers(5, 1, m_tessCB.GetAddressOf());
    m_ctx->DSSetConstantBuffers(5, 1, m_tessCB.GetAddressOf());
    topo = D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
    ++m_tessDraws;
    LogOnce("tess-first", "tessellation active (first draw VS 0x%lx PS 0x%lx)", m_vsHandle, m_psHandle);
    return true;
}

void Device::EndTessellation()
{
    m_ctx->HSSetShader(nullptr, nullptr, 0);
    m_ctx->DSSetShader(nullptr, nullptr, 0);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}
