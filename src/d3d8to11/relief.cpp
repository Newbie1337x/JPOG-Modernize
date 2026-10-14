// relief.cpp - parallax and self-shadowing on dinosaur skins (docs/RELIEF.md). Decides the
// PSVariant::relief bit for the draw and uploads its constants to pixel shader cbuffer b4.
#include "device.h"

void Device::PrepareRelief()
{
    m_reliefActive = false;
    if (!g_config.effectsEnabled || !g_config.reliefEnabled || !m_worldDraw || m_worldKind != 1 ||
        m_sun.flags[1] > 0.5f || !m_textures[0] || m_textures[0]->GetType() != D8RTYPE_TEXTURE)
        return;
    // Translated pixel shaders only (the dinosaur skin shader); fixed-function draws are skipped.
    if (!m_psHandle) return;
    const std::string& name = static_cast<Texture*>(static_cast<ID8Texture*>(m_textures[0]))->m_name;
    if (name.compare(0, 6, "dinos_") != 0) return;
    if (!m_reliefCB) {
        D3D11_BUFFER_DESC d = { sizeof(ReliefConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        if (FAILED(m_device->CreateBuffer(&d, nullptr, &m_reliefCB))) { g_config.reliefEnabled = false; Log("relief: constant buffer failed, disabled"); return; }
    }
    ReliefConstants c = {};
    c.params[0] = g_config.reliefDepth * (g_config.reliefInvert ? -1.0f : 1.0f);
    c.params[1] = g_config.reliefParallax;
    c.params[2] = g_config.reliefShadow;
    c.params[3] = g_config.reliefSoftness;
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(m_ctx->Map(m_reliefCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    memcpy(map.pData, &c, sizeof(c));
    m_ctx->Unmap(m_reliefCB.Get(), 0);
    BindPSCB(4, 1, m_reliefCB.GetAddressOf());
    m_reliefActive = true;
    ++m_reliefDraws;
}
