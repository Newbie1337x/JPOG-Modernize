// First sun-lighting backend: ping-pong shadow maps for visible world geometry.
// The completed previous frame is sampled while this frame builds the next map.
#include "device.h"
#include <DirectXMath.h>
#include <cmath>
using namespace DirectX;

bool Device::CreateSunResources()
{
    D3D11_BUFFER_DESC cb = { sizeof(SunConstants), D3D11_USAGE_DYNAMIC,
        D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    if (FAILED(m_device->CreateBuffer(&cb, nullptr, &m_sunCB))) return false;
    if (!g_config.sunLighting) return true;
    const UINT size = (UINT)g_config.shadowResolution;
    for (int i = 0; i < 2; ++i) {
        D3D11_TEXTURE2D_DESC td = { size,size,1,1,DXGI_FORMAT_R32_TYPELESS,{1,0},D3D11_USAGE_DEFAULT,
            D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE,0,0 };
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
        dd.Format = DXGI_FORMAT_D32_FLOAT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(m_device->CreateTexture2D(&td,nullptr,&m_sunMaps[i])) ||
            FAILED(m_device->CreateDepthStencilView(m_sunMaps[i].Get(),&dd,&m_sunDSV[i])) ||
            FAILED(m_device->CreateShaderResourceView(m_sunMaps[i].Get(),&sd,&m_sunSRV[i]))) return false;
        m_ctx->ClearDepthStencilView(m_sunDSV[i].Get(),D3D11_CLEAR_DEPTH,1,0);
    }
    D3D11_SAMPLER_DESC sm = {};
    sm.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sm.BorderColor[0]=sm.BorderColor[1]=sm.BorderColor[2]=sm.BorderColor[3]=1;
    sm.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL; sm.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable=TRUE; depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
    D3D11_RASTERIZER_DESC raster = {};
    raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE;
    raster.DepthClipEnable=TRUE; raster.DepthBias=64; raster.SlopeScaledDepthBias=1.2f;
    if (FAILED(m_device->CreateSamplerState(&sm,&m_sunSampler)) ||
        FAILED(m_device->CreateDepthStencilState(&depth,&m_sunDepth)) ||
        FAILED(m_device->CreateRasterizerState(&raster,&m_sunRaster))) return false;
    Log("Sun lighting: %u shadow map, extent %.0f, ambient %.2f, direct %.2f",size,
        g_config.shadowExtent,g_config.sunAmbient,g_config.sunDirect);
    return true;
}

void Device::UpdateSun(bool shadowPass)
{
    if (m_worldDraw && !m_sunStarted && m_worldKind == 2) {
        // c0..c3 are dot-product rows of the world-to-clip matrix in recognized shaders.
        XMFLOAT4X4 rows; memcpy(&rows,m_vsConst,sizeof(rows));
        XMMATRIX vp=XMMatrixTranspose(XMLoadFloat4x4(&rows));
        XMVECTOR determinant;
        XMMATRIX inverse=XMMatrixInverse(&determinant,vp);
        if (std::fabs(XMVectorGetX(determinant)) < 1e-12f) m_worldDraw=false;
        else {
            XMFLOAT4X4 inverseRows;
            XMStoreFloat4x4(&inverseRows,XMMatrixTranspose(inverse));
            memcpy(m_sun.clipToWorld,&inverseRows,sizeof(inverseRows));
            XMVECTOR nearPoint=XMVector3TransformCoord(XMVectorSet(0,0,0,1),inverse);
            XMVECTOR farPoint=XMVector3TransformCoord(XMVectorSet(0,0,0.9f,1),inverse);
            XMVECTOR ray=farPoint-nearPoint;
            float rayY=XMVectorGetY(ray), ground=m_worldKind==2 ? m_vsConst[4][1] : 0;
            XMVECTOR center=nearPoint;
            if (std::fabs(rayY)>1e-5f) center=nearPoint+ray*((ground-XMVectorGetY(nearPoint))/rayY);
            XMFLOAT3 target; XMStoreFloat3(&target,center);
            if (!std::isfinite(target.x)||!std::isfinite(target.y)||!std::isfinite(target.z)) m_worldDraw=false;
            else {
                float step=g_config.shadowExtent/g_config.shadowResolution;
                target.x=std::floor(target.x/step)*step; target.z=std::floor(target.z/step)*step;
                center=XMLoadFloat3(&target);
                XMVECTOR light=XMVector3Normalize(XMVectorSet(g_config.sunDirection[0],g_config.sunDirection[1],g_config.sunDirection[2],0));
                XMMATRIX view=XMMatrixLookAtLH(center+light*g_config.shadowExtent,center,XMVectorSet(0,1,0,0));
                XMMATRIX proj=XMMatrixOrthographicLH(g_config.shadowExtent,g_config.shadowExtent,1,g_config.shadowExtent*2);
                XMStoreFloat4x4(&rows,XMMatrixTranspose(view*proj));
                memcpy(m_sun.writeRows,&rows,sizeof(rows));
                XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(m_sun.direction),light);
                m_sun.settings[0]=g_config.sunAmbient; m_sun.settings[1]=g_config.sunDirect;
                m_sun.settings[2]=g_config.shadowStrength; m_sun.settings[3]=1.0f/g_config.shadowResolution;
                ID3D11ShaderResourceView* nullView=nullptr;
                m_ctx->PSSetShaderResources(8,1,&nullView);
                m_ctx->ClearDepthStencilView(m_sunDSV[m_sunWrite].Get(),D3D11_CLEAR_DEPTH,1,0);
                m_sunStarted=true;
                LogOnce("sun-camera", "Sun shadow center %.1f %.1f %.1f; first geometry kind %d",target.x,target.y,target.z,m_worldKind);
            }
        }
    }
    // A skinned draw can use a model-to-clip matrix. It cannot establish the
    // shared world camera: wait for the terrain and reconstruct every model
    // from its original clip position through that camera's inverse.
    if (!m_sunStarted) m_worldDraw=false;
    m_sun.flags[0]=m_worldDraw ? 1.0f : 0.0f;
    m_sun.flags[1]=shadowPass ? 1.0f : 0.0f;
    m_sun.flags[2]=m_sunReady ? 1.0f : 0.0f;
    m_sun.flags[3]=(float)m_worldKind;
    if (!m_sunUploadedValid || memcmp(&m_sunUploaded,&m_sun,sizeof(m_sun))) {
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(m_ctx->Map(m_sunCB.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map))) {
            memcpy(map.pData,&m_sun,sizeof(m_sun)); m_ctx->Unmap(m_sunCB.Get(),0);
            m_sunUploaded=m_sun; m_sunUploadedValid=true;
        }
    }
    m_ctx->VSSetConstantBuffers(2,1,m_sunCB.GetAddressOf());
    m_ctx->PSSetConstantBuffers(1,1,m_sunCB.GetAddressOf());
    ID3D11ShaderResourceView* srv=m_sunReady ? m_sunSRV[m_sunRead].Get() : nullptr;
    m_ctx->PSSetShaderResources(8,1,&srv);
    m_ctx->PSSetSamplers(8,1,m_sunSampler.GetAddressOf());
}

void Device::DrawSunShadow(bool indexed, UINT count, UINT start, INT base)
{
    if (!m_worldDraw || !m_worldCaster || !m_sunStarted) return;
    UpdateSun(true);
    ID3D11PixelShader* depthPixel=GetPixelShader();
    if (!depthPixel) { UpdateSun(false); return; }
    m_ctx->PSSetShader(depthPixel,nullptr,0);
    m_lastPS=nullptr;
    m_ctx->OMSetRenderTargets(0,nullptr,m_sunDSV[m_sunWrite].Get());
    D3D11_VIEWPORT vp={0,0,(float)g_config.shadowResolution,(float)g_config.shadowResolution,0,1};
    m_ctx->RSSetViewports(1,&vp);
    m_ctx->RSSetState(m_sunRaster.Get());
    m_ctx->OMSetDepthStencilState(m_sunDepth.Get(),0);
    m_ctx->OMSetBlendState(m_noBlend.Get(),nullptr,0xffffffff);
    if (indexed) m_ctx->DrawIndexed(count,start,base); else m_ctx->Draw(count,start);
    ++m_shadowDraws;
    m_rtDirty=m_viewportDirty=true;
    m_lastBlend=nullptr; m_lastDepth=nullptr; m_lastRaster=nullptr;
    // Vertex/pixel shaders, streams and their constants remain valid. PrepareDraw
    // restores targets, viewport and the sun-pass flag before the next game draw.
}
