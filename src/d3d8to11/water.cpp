// Depth-aware water. Never samples an actively bound depth/render target.
// One opaque snapshot is frozen before the first water draw of each scene segment.
#include "device.h"

bool Device::CreateWaterResources()
{
    m_waterTimeOrigin=GetTickCount64();
    D3D11_BUFFER_DESC cb={sizeof(WaterConstants),D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0};
    if(FAILED(m_device->CreateBuffer(&cb,nullptr,&m_waterCB))) return false;
    D3D11_SAMPLER_DESC sd={};
    sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD=D3D11_FLOAT32_MAX;
    return SUCCEEDED(m_device->CreateSamplerState(&sd,&m_waterSampler));
}

bool Device::EnsureWaterSnapshot()
{
    if(!m_backBuffer || !m_autoDepth || !m_autoDepth->m_tex) return false;
    D3D11_TEXTURE2D_DESC depth; m_autoDepth->m_tex->GetDesc(&depth);
    if(!m_waterDepthCopy || m_waterW!=depth.Width || m_waterH!=depth.Height || m_waterSamples!=depth.SampleDesc.Count) {
        m_waterSnapshotReady=false;
        m_waterDepthSRV.Reset(); m_waterColorSRV.Reset();
        m_waterDepthCopy.Reset(); m_waterColorCopy.Reset();
        depth.BindFlags=D3D11_BIND_SHADER_RESOURCE; depth.MiscFlags=0;
        if(FAILED(m_device->CreateTexture2D(&depth,nullptr,&m_waterDepthCopy))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv={};
        sv.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sv.ViewDimension=depth.SampleDesc.Count>1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        if(depth.SampleDesc.Count<=1) sv.Texture2D.MipLevels=1;
        if(FAILED(m_device->CreateShaderResourceView(m_waterDepthCopy.Get(),&sv,&m_waterDepthSRV))) return false;
        D3D11_TEXTURE2D_DESC color={depth.Width,depth.Height,1,1,DXGI_FORMAT_B8G8R8A8_UNORM,{1,0},
            D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};
        if(FAILED(m_device->CreateTexture2D(&color,nullptr,&m_waterColorCopy)) ||
           FAILED(m_device->CreateShaderResourceView(m_waterColorCopy.Get(),nullptr,&m_waterColorSRV))) return false;
        m_waterW=depth.Width; m_waterH=depth.Height; m_waterSamples=depth.SampleDesc.Count;
    }
    if(!m_waterDepthSRV || !m_waterColorSRV) return false;
    if(!m_waterSnapshotReady) {
        PerformanceScope perf(m_perf,m_device.Get(),m_ctx.Get(),Performance::WaterCopy);
        ID3D11ShaderResourceView* nulls[2]={};
        m_ctx->PSSetShaderResources(10,2,nulls);
        m_ctx->OMSetRenderTargets(0,nullptr,nullptr);
        m_ctx->CopyResource(m_waterDepthCopy.Get(),m_autoDepth->m_tex.Get());
        if(m_backBuffer->m_samples>1)
            m_ctx->ResolveSubresource(m_waterColorCopy.Get(),0,m_backBuffer->m_tex.Get(),0,DXGI_FORMAT_B8G8R8A8_UNORM);
        else m_ctx->CopyResource(m_waterColorCopy.Get(),m_backBuffer->m_tex.Get());
        m_waterSnapshotReady=true;
        m_waterFrameTime=WaterTime();
        m_rtDirty=true; ApplyRenderTargets();
        LogOnce("water-snapshot","Water snapshot active: %ux%u, %u depth samples",m_waterW,m_waterH,m_waterSamples);
    }
    return true;
}

void Device::PrepareWaterDraw()
{
    m_waterDraw=false;
    WaterConstants c={};
    bool candidate=g_config.effectsEnabled && g_config.waterEnabled && m_sunStarted && !m_sunResolved &&
        m_rt==m_backBuffer && m_ds==m_autoDepth && m_rs[RS_ZENABLE] &&
        m_viewport.X==0 && m_viewport.Y==0 &&
        m_viewport.Width==m_backBuffer->m_width && m_viewport.Height==m_backBuffer->m_height &&
        m_psHandle>0 && m_psHandle<=m_pixelShaders.size() &&
        m_pixelShaders[m_psHandle-1] && IsWaterPixelShader(m_pixelShaders[m_psHandle-1]->func.data());
    if(candidate && EnsureWaterSnapshot()) {
        m_waterDraw=true;
        UpdateWaterRegions();
        memcpy(c.clipToWorld,m_sun.clipToWorld,sizeof(c.clipToWorld));
        memcpy(c.worldToClip,m_waterViewProjection,sizeof(c.worldToClip));
        c.reflection[0]=g_config.waterSSR ? g_config.waterSSRStrength:0;
        c.reflection[1]=g_config.waterSSRDistance;c.reflection[2]=(float)g_config.waterSSRSteps;
        c.regions[0]=(g_config.waterSurfaceMode==0 && m_waterRegionReady && m_waterRegionSRV) ? 1.f:0.f;
        c.regions[1]=g_config.waterSurfaceMode==1 ? 0.f:1.f;
        c.regions[2]=m_waterRegions.width ? 1.f/m_waterRegions.width:0;
        c.regions[3]=m_waterRegions.height ? 1.f/m_waterRegions.height:0;
        c.profiles[0]=g_config.waterLagoonFoam;c.profiles[1]=g_config.waterLagoonRipples;c.profiles[2]=g_config.waterOceanRipples;
        c.profiles[3]=m_waterRegionLevel;
        // No unclassified ocean displacement during the first automatic scan.
        c.swell[0]=(g_config.waterSurfaceMode!=0 || c.regions[0]>0) ? g_config.waterSwellAmplitude:0;
        c.swell[1]=g_config.waterSwellLength;c.swell[2]=g_config.waterSwellCoastDepth;c.swell[3]=g_config.waterCrestFoam;
        c.screen[0]=1.0f/m_waterW; c.screen[1]=1.0f/m_waterH;
        c.screen[2]=-c.screen[0]; c.screen[3]=c.screen[1];
        float aaX=0,aaY=0;TemporalJitter(aaX,aaY);c.screen[2]+=aaX;c.screen[3]+=aaY;
        c.params[0]=1; c.params[1]=g_config.waterDepthScale;
        c.params[2]=g_config.waterFoamWidth; c.params[3]=g_config.waterFoam;
        c.optical[0]=g_config.waterReflection; c.optical[1]=g_config.waterTransparency;
        c.optical[2]=g_config.waterDistortion; c.optical[3]=m_waterFrameTime*g_config.waterSpeed;
        c.misc[0]=(float)g_config.waterDebug;
        c.misc[1]=g_config.waterRipples;
        c.motion[1]=g_config.waterInteractionStrength;
        c.motion[2]=m_waterFrameTime;
        if(g_config.waterInteraction) {
            unsigned n=0;
            for(const auto& r:m_waterInteraction.Ripples()) {
                float age=m_waterFrameTime-r.birth;
                if(!r.active || age<0 || age>r.lifetime)continue;
                c.contacts[n][0]=r.contact.x;c.contacts[n][1]=r.contact.feetY;
                c.contacts[n][2]=r.contact.z;c.contacts[n][3]=r.birth;
                c.rings[n][0]=r.amplitude;c.rings[n][1]=r.radius;
                c.rings[n][2]=r.speed;c.rings[n][3]=r.lifetime;n++;
            }
            c.motion[0]=(float)n;
        }
        LogOnce("water-shader","Modern water active: captured bump-reflection shader VS %lx PS %lx",m_vsHandle,m_psHandle);
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if(SUCCEEDED(m_ctx->Map(m_waterCB.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) {
        memcpy(mapped.pData,&c,sizeof(c)); m_ctx->Unmap(m_waterCB.Get(),0);
    } else m_waterDraw=false;
    m_ctx->VSSetConstantBuffers(4,1,m_waterCB.GetAddressOf());
    m_ctx->PSSetConstantBuffers(2,1,m_waterCB.GetAddressOf());
    ID3D11ShaderResourceView* views[2]={m_waterDraw ? m_waterDepthSRV.Get():nullptr,m_waterDraw ? m_waterColorSRV.Get():nullptr};
    m_ctx->PSSetShaderResources(10,2,views);
    ID3D11ShaderResourceView* regionViews[3]={m_waterDraw && m_waterRegionReady ? m_waterRegionSRV.Get():nullptr,
        m_waterDraw && m_waterRegionReady ? m_waterTerrainSRV.Get():nullptr,
        m_waterDraw && m_waterRegionReady ? m_waterShoreSRV.Get():nullptr};
    m_ctx->PSSetShaderResources(12,3,regionViews);
    m_ctx->VSSetShaderResources(12,3,regionViews);
    m_ctx->VSSetSamplers(10,1,m_waterSampler.GetAddressOf());
    m_ctx->PSSetSamplers(10,1,m_waterSampler.GetAddressOf());
}

float Device::WaterTime() const {
    return (float)(GetTickCount64()-m_waterTimeOrigin)*0.001f;
}
void Device::UpdateWaterInteractions() {
    if(!g_config.effectsEnabled || !g_config.waterEnabled || !g_config.waterInteraction) {
        m_waterInteraction.Clear();return;
    }
    WaterContact contacts[32]={};unsigned count=m_sunStarted ? m_pusherCount : 0;
    for(unsigned k=0;k<count;k++){
        const float* b=m_pushBoxes[k];
        float r=.4f*((b[2]-b[0])>(b[3]-b[1])?(b[2]-b[0]):(b[3]-b[1]));
        r=r<.8f?.8f:r>20?20:r;
        contacts[k]={(b[0]+b[2])*.5f,m_pushers[k][1],(b[1]+b[3])*.5f,r};
    }
    float now=WaterTime();m_waterInteraction.Update(now,contacts,count);
    if(count)LogOnce("water-contact","Water interaction tracking active: %u approximate creature footprints",count);
}
