#include "device.h"
#include "water_position.h"
#include "native_terrain.h"
#include <cmath>

namespace {
using NativeBridge = native_terrain::Bridge;
using NativeInfo = native_terrain::Info;
bool NativeInfoSafe(const NativeBridge& b, NativeInfo& i) { return native_terrain::InfoSafe(b, i); }
bool NativeHeightsSafe(const NativeBridge& b, const NativeInfo& i, float* out, unsigned first, unsigned count)
{ return native_terrain::HeightsSafe(b, i, out, first, count); }
}

bool Device::WaterSurfaceHeight(float& height) const
{
    if(!(m_vsHandle&1))return false;
    size_t shader=m_vsHandle>>1;
    if(shader>=m_vertexShaders.size() || !m_vertexShaders[shader])return false;
    for(const auto& in:m_vertexShaders[shader]->parsed.inputs){
        if(in.reg!=0 || in.type<0 || in.type>3 || in.stream<0 || in.stream>=kMaxStreams)continue;
        auto* vb=m_streams[in.stream].vb;
        size_t offset=size_t(m_drawFirstVertex)*m_streams[in.stream].stride+in.offset;
        size_t bytes=size_t(in.type+1)*sizeof(float);
        if(!vb || offset+bytes>vb->m_impl.m_shadow.size())return false;
        float p[4]={0,0,0,1};memcpy(p,vb->m_impl.m_shadow.data()+offset,bytes);
        float clip[4]={},world[4]={};
        const auto& code=m_vertexShaders[shader]->func;
        if(!WaterPositionClip(code.data(),code.size(),p,m_vsConst,kMaxVSConst,clip))return false;
        for(int r=0;r<4;r++)for(int c=0;c<4;c++)world[r]+=m_sun.clipToWorld[r][c]*clip[c];
        if(std::fabs(world[3])<1e-6f)return false;
        height=-world[1]/world[3]; // verified native height = negative renderer Y
        return std::isfinite(height) && std::fabs(height)<10000;
    }
    return false;
}

void Device::UpdateWaterRegions()
{
    if(g_config.waterSurfaceMode!=0 || m_waterNativeFault || m_waterRegionFrame==m_frame)return;
    m_waterRegionFrame=m_frame;
    NativeBridge bridge;
    m_waterNativeAvailable=bridge.Resolve();
    if(!m_waterNativeAvailable){LogOnce("water-region-no-native","Automatic ocean/pond separation unavailable: native SimJP terrain exports missing; original ocean profile retained (manual lagoon/ocean override available)");return;}
    NativeInfo info={};float level=0;
    if(!NativeInfoSafe(bridge,info)){
        m_waterRegionReady=false;m_waterRegionNext=0;m_waterRegionRefresh=0;return;
    }
    if(!WaterSurfaceHeight(level)){
        LogOnce("water-region-no-plane","Native water map waiting for a supported water position prefix/vertex stream (VS 0x%lx); original ocean profile retained",m_vsHandle);return;
    }
    LARGE_INTEGER start,end;QueryPerformanceCounter(&start);
    ULONGLONG now=GetTickCount64();
    bool changed=info.terrain!=m_waterRegionTerrain || info.field!=m_waterRegionHeightField ||
        info.w!=m_waterRegionW || info.h!=m_waterRegionH;
    if(changed){
        m_waterRegionTerrain=info.terrain;m_waterRegionHeightField=info.field;
        m_waterRegionW=info.w;m_waterRegionH=info.h;
        m_waterRegionHeights.assign(size_t(info.w)*info.h,0);m_waterRegionNext=0;
        m_waterRegionReady=false;m_waterRegionRefresh=0;m_waterRegionLevel=level;
        m_waterRegionSRV.Reset();m_waterRegionTexture.Reset();
        m_waterTerrainSRV.Reset();m_waterTerrainTexture.Reset();m_taaValid=false;
        m_waterShoreSRV.Reset();m_waterShoreTexture.Reset();
        Log("Native water region map: %ux%u world samples, water height %.3f; incremental read-only terrain scan",info.w,info.h,level);
    }
    unsigned total=info.w*info.h;
    if(m_waterRegionNext==total && now-m_waterRegionRefresh<2000 && std::fabs(level-m_waterRegionLevel)<.02f)return;
    if(m_waterRegionNext==total){m_waterRegionNext=0;m_waterRegionLevel=level;}
    // Heights do not depend on the water plane. Camera round-off must not keep
    // restarting the scan; classify against the latest measured plane instead.
    m_waterRegionLevel=level;
    unsigned count=(std::min)(8192u,total-m_waterRegionNext);
    if(!NativeHeightsSafe(bridge,info,m_waterRegionHeights.data(),m_waterRegionNext,count)){
        m_waterNativeFault=true;m_waterRegionReady=false;
        Log("Native water region scan stopped: invalid terrain read; no engine state modified");return;
    }
    m_waterRegionNext+=count;
    if(m_waterRegionNext==total && m_waterRegions.Build(info.w,info.h,m_waterRegionHeights,m_waterRegionLevel)){
        ID3D11ShaderResourceView* none[3]={};
        BindPSSrv(12,3,none);BindVSSrv(12,3,none);
        if(!m_waterRegionTexture){
            D3D11_TEXTURE2D_DESC td={info.w,info.h,1,1,DXGI_FORMAT_R8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};
            if(FAILED(m_device->CreateTexture2D(&td,nullptr,&m_waterRegionTexture)) ||
               FAILED(m_device->CreateShaderResourceView(m_waterRegionTexture.Get(),nullptr,&m_waterRegionSRV))){
                m_waterNativeFault=true;m_waterRegionReady=false;Log("Water region GPU mask creation failed");return;
            }
        }
        if(!m_waterTerrainTexture){
            D3D11_TEXTURE2D_DESC td={info.w,info.h,1,1,DXGI_FORMAT_R32_FLOAT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};
            if(FAILED(m_device->CreateTexture2D(&td,nullptr,&m_waterTerrainTexture)) ||
               FAILED(m_device->CreateShaderResourceView(m_waterTerrainTexture.Get(),nullptr,&m_waterTerrainSRV))){
                m_waterNativeFault=true;m_waterRegionReady=false;Log("Water terrain GPU height map creation failed");return;
            }
        }
        m_ctx->UpdateSubresource(m_waterRegionTexture.Get(),0,nullptr,m_waterRegions.ocean.data(),info.w,0);
        m_ctx->UpdateSubresource(m_waterTerrainTexture.Get(),0,nullptr,m_waterRegionHeights.data(),info.w*sizeof(float),0);
        if(!m_waterShoreTexture){
            D3D11_TEXTURE2D_DESC td={info.w,info.h,1,1,DXGI_FORMAT_R32_FLOAT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};
            if(FAILED(m_device->CreateTexture2D(&td,nullptr,&m_waterShoreTexture)) ||
               FAILED(m_device->CreateShaderResourceView(m_waterShoreTexture.Get(),nullptr,&m_waterShoreSRV))){
                m_waterNativeFault=true;m_waterRegionReady=false;Log("Water shore distance GPU map creation failed");return;
            }
        }
        m_ctx->UpdateSubresource(m_waterShoreTexture.Get(),0,nullptr,m_waterRegions.shoreDistance.data(),info.w*sizeof(float),0);
        if(!m_waterRegionReady){m_taaValid=false;Log("Ocean/pond connectivity active: %u ocean, %u enclosed-water samples",m_waterRegions.oceanCells,m_waterRegions.pondCells);}
        m_waterRegionReady=true;m_waterRegionRefresh=now;
    }
    QueryPerformanceCounter(&end);m_waterRegionCpuMs=float((end.QuadPart-start.QuadPart)*1000.0/m_qpcFreq.QuadPart);
}
