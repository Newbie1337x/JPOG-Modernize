#include "device.h"
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <memory>
#include <cmath>

namespace {
struct FsrTexture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    bool Create(ID3D11Device* d,UINT w,UINT h,DXGI_FORMAT format){
        D3D11_TEXTURE2D_DESC td={w,h,1,1,format,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,0,0};
        return SUCCEEDED(d->CreateTexture2D(&td,nullptr,&texture)) && SUCCEEDED(d->CreateShaderResourceView(texture.Get(),nullptr,&srv)) &&
            SUCCEEDED(d->CreateUnorderedAccessView(texture.Get(),nullptr,&uav));
    }
    FfxResource Resource() const {return ffxGetResourceDX11_Fsr31(texture.Get(),GetFfxResourceDescriptionDX11(texture.Get()),L"JPOG FSR31");}
};
struct Inputs {float inverse[4][4],previous[4][4],screen[4],flags[4],pushers[32][4];};
const char* InputCS=R"(
cbuffer Inputs:register(b0){float4 inverse[4];float4 previous[4];float4 screen;float4 flags;float4 pushers[32];};
Texture2D<float4> scene:register(t0);Texture2D<float> terrain:register(t2);SamplerState clampSampler:register(s0);
RWTexture2D<float4> linearColor:register(u0);RWTexture2D<float> depthOut:register(u1);
RWTexture2D<float2> motion:register(u2);RWTexture2D<float> reactive:register(u3);
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=(uint2)screen.xy))return;
 int2 p=id.xy;float d=LoadDepth(p);float2 uv=(p+.5)/screen.xy;
 float2 xy=float2(uv.x*2-1,1-uv.y*2)-screen.zw;
 float4 q=float4(xy,d,1),world=float4(dot(q,inverse[0]),dot(q,inverse[1]),dot(q,inverse[2]),dot(q,inverse[3]));
 bool finiteWorld=abs(world.w)>1e-8;world/=finiteWorld?world.w:1;
 float4 old=float4(dot(world,previous[0]),dot(world,previous[1]),dot(world,previous[2]),dot(world,previous[3]));
 float2 mv=0;float r=d>=.999999?1:0;
 if(flags.x>.5 && old.w>1e-8 && finiteWorld && d<.999999){
   float2 oldUV=float2(old.x/old.w*.5+.5,.5-old.y/old.w*.5);
   // Unjittered current UV -> unjittered previous UV, in normalized UV units.
   float2 baseUV=float2(xy.x*.5+.5,.5-xy.y*.5);mv=oldUV-baseUV;
   if(any(abs(mv)>.5) || any(oldUV<0) || any(oldUV>1)){mv=0;r=1;}}
 if(flags.y>.5 && finiteWorld){uint tw,th;terrain.GetDimensions(tw,th);
   float2 tu=(world.xz+.5)/float2(tw,th);
   if(any(tu<0) || any(tu>1))r=1;
   else if(terrain.SampleLevel(clampSampler,tu,0)<flags.z-.005)r=1;}
 [loop]for(int k=0;k<(int)flags.w;k++){
   float radius=max(2,pushers[k].w*1.6);
   if(distance(world.xz,pushers[k].xz)<radius)r=1;}
 float3 c=scene.Load(int3(p,0)).rgb;
 linearColor[p]=float4(pow(max(c,0),2.2),1);depthOut[p]=d;motion[p]=mv;reactive[p]=r;
})";
const char* OutputPS=R"(Texture2D<float4> image:register(t0);
float4 main(float4 p:SV_Position):SV_Target{return float4(pow(saturate(image.Load(int3(int2(p.xy),0)).rgb),1/2.2),1);})";
const char* GeneratedPS=R"(
cbuffer Helper:register(b0){float4 color;float4 misc;};
Texture2D<float4> generated:register(t0);Texture2D<float4> hudless:register(t1);Texture2D<float4> finalFrame:register(t2);
Texture1D<float4> gammaLut:register(t3);
float4 main(float4 p:SV_Position):SV_Target{
 int3 pixel=int3(int2(p.xy),0);float3 a=pow(saturate(generated.Load(pixel).rgb),1/2.2);
 float3 b=pow(saturate(hudless.Load(pixel).rgb),1/2.2),f=finalFrame.Load(pixel).rgb;
 float3 diff=abs(f-b);float mask=smoothstep(.008,.025,max(diff.x,max(diff.y,diff.z)));
 float3 outColor=lerp(a,f,mask);
 if(misc.y>.5)outColor=float3(gammaLut.Load(int2(outColor.r*255+.5,0)).r,gammaLut.Load(int2(outColor.g*255+.5,0)).g,gammaLut.Load(int2(outColor.b*255+.5,0)).b);
 return float4(outColor,1);
})";
struct FrameGenState {
    void* scratch=nullptr;
    std::unique_ptr<FfxOpticalflowContext> optical;
    std::unique_ptr<FfxFrameInterpolationContext> interpolation;
    bool opticalCreated=false,interpolationCreated=false;
    FsrTexture flow,scd,generated;
    ComPtr<ID3D11PixelShader> composite;
    ~FrameGenState(){if(interpolationCreated)ffxFrameInterpolationContextDestroy(interpolation.get());
        if(opticalCreated)ffxOpticalflowContextDestroy(optical.get());free(scratch);}
    bool Create(ID3D11Device* device,UINT w,UINT h){
        size_t bytes=ffxGetScratchMemorySizeDX11(2);scratch=calloc(1,bytes);
        FfxInterface api={};if(!scratch || ffxGetInterfaceDX11(&api,ffxGetDeviceDX11_Fsr31(device),scratch,bytes,2)!=FFX_OK)return false;
        optical=std::make_unique<FfxOpticalflowContext>();interpolation=std::make_unique<FfxFrameInterpolationContext>();
        FfxOpticalflowContextDescription od={};od.backendInterface=api;od.resolution={w,h};
        if(ffxOpticalflowContextCreate(optical.get(),&od)!=FFX_OK)return false;opticalCreated=true;
        FfxFrameInterpolationContextDescription fd={};fd.backendInterface=api;fd.maxRenderSize=fd.displaySize={w,h};
        fd.backBufferFormat=FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
        if(ffxFrameInterpolationContextCreate(interpolation.get(),&fd)!=FFX_OK)return false;interpolationCreated=true;
        FfxOpticalflowSharedResourceDescriptions shared={};
        if(ffxOpticalflowGetSharedResourceDescriptions(optical.get(),&shared)!=FFX_OK)return false;
        const auto& flowDesc=shared.opticalFlowVector.resourceDescription;
        const auto& scdDesc=shared.opticalFlowSCD.resourceDescription;
        if(flowDesc.format!=FFX_SURFACE_FORMAT_R16G16_SINT || scdDesc.format!=FFX_SURFACE_FORMAT_R32_UINT)return false;
        auto ps=CompileHLSL(GeneratedPS,"ps_5_0","framegen_hud_composite");
        return flow.Create(device,flowDesc.width,flowDesc.height,DXGI_FORMAT_R16G16_SINT) &&
            scd.Create(device,scdDesc.width,scdDesc.height,DXGI_FORMAT_R32_UINT) &&
            generated.Create(device,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT) && ps &&
            SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&composite));
    }
    bool Dispatch(const FfxFsr3UpscalerDispatchDescription& aa,UINT64 frameID){
        FfxFrameInterpolationPrepareDescription p={};p.commandList=aa.commandList;p.renderSize=aa.renderSize;
        p.jitterOffset=aa.jitterOffset;p.motionVectorScale=aa.motionVectorScale;p.frameTimeDelta=aa.frameTimeDelta;
        p.cameraNear=aa.cameraNear;p.cameraFar=aa.cameraFar;p.cameraFovAngleVertical=aa.cameraFovAngleVertical;p.viewSpaceToMetersFactor=1;
        p.depth=aa.depth;p.motionVectors=aa.motionVectors;p.frameID=frameID;p.dilatedDepth=aa.dilatedDepth;
        p.dilatedMotionVectors=aa.dilatedMotionVectors;p.reconstructedPrevDepth=aa.reconstructedPrevNearestDepth;
        if(ffxFrameInterpolationPrepare(interpolation.get(),&p)!=FFX_OK)return false;
        FfxOpticalflowDispatchDescription o={};o.commandList=aa.commandList;o.color=aa.output;o.opticalFlowVector=flow.Resource();
        // SDK scRGB normalization divides by (maxLuma-minLuma)/80. Our SDR
        // linear 0..1 input therefore needs 0..80, not 0..1 (which clips luma).
        o.opticalFlowSCD=scd.Resource();o.reset=aa.reset;o.backbufferTransferFunction=FFX_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;o.minMaxLuminance={0,80};
        if(ffxOpticalflowContextDispatch(optical.get(),&o)!=FFX_OK)return false;
        FfxFrameInterpolationDispatchDescription d={};d.commandList=aa.commandList;d.displaySize=d.renderSize=aa.renderSize;
        d.currentBackBuffer=d.currentBackBuffer_HUDLess=aa.output;d.output=generated.Resource();
        d.interpolationRect={0,0,int32_t(aa.renderSize.width),int32_t(aa.renderSize.height)};d.opticalFlowVector=flow.Resource();
        d.opticalFlowSceneChangeDetection=scd.Resource();d.opticalFlowBufferSize={flow.texture?flowDescWidth():0,flowDescHeight()};
        d.opticalFlowScale={1.f/aa.renderSize.width,1.f/aa.renderSize.height};d.opticalFlowBlockSize=8;
        d.cameraNear=aa.cameraNear;d.cameraFar=aa.cameraFar;d.cameraFovAngleVertical=aa.cameraFovAngleVertical;d.viewSpaceToMetersFactor=1;
        d.frameTimeDelta=aa.frameTimeDelta;d.reset=aa.reset;d.backBufferTransferFunction=FFX_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
        d.minMaxLuminance[0]=0;d.minMaxLuminance[1]=80;d.frameID=frameID;
        d.dilatedDepth=aa.dilatedDepth;d.dilatedMotionVectors=aa.dilatedMotionVectors;d.reconstructedPrevDepth=aa.reconstructedPrevNearestDepth;
        return ffxFrameInterpolationDispatch(interpolation.get(),&d)==FFX_OK;
    }
    UINT flowDescWidth()const{D3D11_TEXTURE2D_DESC d;flow.texture->GetDesc(&d);return d.Width;}
    UINT flowDescHeight()const{D3D11_TEXTURE2D_DESC d;flow.texture->GetDesc(&d);return d.Height;}
};
}

struct Device::Fsr31State {
    std::unique_ptr<FfxFsr3UpscalerContext> context;
    void* scratch=nullptr;bool created=false;
    UINT w=0,h=0,samples=0;
    FsrTexture color,depth,motion,reactive,output,dilatedDepth,dilatedMotion,previousDepth;
    ComPtr<ID3D11ComputeShader> inputs;
    ComPtr<ID3D11PixelShader> copy;
    ComPtr<ID3D11Buffer> cb;
    float previousVP[4][4]={};ULONGLONG tick=0;
    std::unique_ptr<FrameGenState> fg;
    ~Fsr31State(){fg.reset();if(created)ffxFsr3UpscalerContextDestroy(context.get());free(scratch);}
};
void Device::ReleaseFsr31(){
    if(m_fsr31 && m_ctx){
        ID3D11ShaderResourceView* srvs[128]={};ID3D11UnorderedAccessView* uavs[64]={};
        ID3D11Buffer* cbs[14]={};ID3D11SamplerState* samplers[16]={};
        m_ctx->CSSetShaderResources(0,128,srvs);m_ctx->CSSetUnorderedAccessViews(0,64,uavs,nullptr);
        m_ctx->CSSetConstantBuffers(0,14,cbs);m_ctx->CSSetSamplers(0,16,samplers);m_ctx->CSSetShader(nullptr,nullptr,0);
    }
    delete m_fsr31;m_fsr31=nullptr;m_fsr31Frame=~UINT64(0);m_taaValid=false;m_viewportDirty=true;m_frameGenReady=false;
}
bool Device::EnsureFsr31(){
    if(!g_config.effectsEnabled || !g_config.fsr31){if(m_fsr31)ReleaseFsr31();return false;}
    if(m_fsr31Failed || !m_backBuffer || !m_autoDepth || !m_autoDepth->m_srv)return false;
    UINT w=m_backBuffer->m_width,h=m_backBuffer->m_height,samples=m_autoDepth->m_samples;
    if(m_fsr31 && m_fsr31->w==w && m_fsr31->h==h && m_fsr31->samples==samples)return true;
    ReleaseFsr31();
    if(m_device->GetFeatureLevel()<D3D_FEATURE_LEVEL_11_1 || UINT64(w)*h>4194304){
        m_fsr31Failed=true;Log("FSR31 unavailable: requires FL11.1 and <=4M pixels in this x86 prototype");return false;}
    try {
        auto s=std::make_unique<Fsr31State>();s->w=w;s->h=h;s->samples=samples;
        size_t bytes=ffxGetScratchMemorySizeDX11(1);s->scratch=calloc(1,bytes);
        s->context=std::make_unique<FfxFsr3UpscalerContext>();
        FfxFsr3UpscalerContextDescription desc={};desc.maxRenderSize={w,h};desc.maxUpscaleSize={w,h};
        if(!s->scratch || ffxGetInterfaceDX11(&desc.backendInterface,ffxGetDeviceDX11_Fsr31(m_device.Get()),s->scratch,bytes,1)!=FFX_OK)
            throw "backend interface";
        FfxErrorCode err=ffxFsr3UpscalerContextCreate(s->context.get(),&desc);
        if(err!=FFX_OK){Log("FSR31 context creation error 0x%x",unsigned(err));throw "context";}s->created=true;
        bool ok=s->color.Create(m_device.Get(),w,h,DXGI_FORMAT_R16G16B16A16_FLOAT) &&
            s->depth.Create(m_device.Get(),w,h,DXGI_FORMAT_R32_FLOAT) && s->motion.Create(m_device.Get(),w,h,DXGI_FORMAT_R16G16_FLOAT) &&
            s->reactive.Create(m_device.Get(),w,h,DXGI_FORMAT_R8_UNORM) && s->output.Create(m_device.Get(),w,h,DXGI_FORMAT_R16G16B16A16_FLOAT) &&
            s->dilatedDepth.Create(m_device.Get(),w,h,DXGI_FORMAT_R32_FLOAT) && s->dilatedMotion.Create(m_device.Get(),w,h,DXGI_FORMAT_R16G16_FLOAT) &&
            s->previousDepth.Create(m_device.Get(),w,h,DXGI_FORMAT_R32_UINT);
        std::string load=samples>1 ? "Texture2DMS<float,"+std::to_string(samples)+"> z:register(t1);float LoadDepth(int2 p){float d=1;[unroll]for(int i=0;i<"+std::to_string(samples)+";i++)d=min(d,z.Load(p,i));return d;}\n":
            "Texture2D<float> z:register(t1);float LoadDepth(int2 p){return z.Load(int3(p,0));}\n";
        auto cs=CompileHLSL(load+InputCS,"cs_5_0","fsr31_inputs");auto ps=CompileHLSL(OutputPS,"ps_5_0","fsr31_copy");
        D3D11_BUFFER_DESC bd={sizeof(Inputs),D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0};
        if(!ok || !cs || !ps || FAILED(m_device->CreateComputeShader(cs->GetBufferPointer(),cs->GetBufferSize(),nullptr,&s->inputs)) ||
            FAILED(m_device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&s->copy)) ||
            FAILED(m_device->CreateBuffer(&bd,nullptr,&s->cb)))throw "input resources/shaders";
        m_fsr31=s.release();Log("FSR31 native temporal ready: %ux%u, MSAA %u; camera motion + conservative dynamic reactive mask; optional FrameGen separately gated",w,h,samples);
        return true;
    }catch(...){m_fsr31Failed=true;Log("FSR31 initialization failed; original TAA/FSR1 retained");return false;}
}
void Device::Fsr31Jitter(float& x,float& y) const {
    x=y=0;if(!g_config.effectsEnabled || !g_config.fsr31 || !m_fsr31 || m_fsr31Failed)return;
    float jx=0,jy=0;int phase=ffxFsr3UpscalerGetJitterPhaseCount(m_fsr31->w,m_fsr31->w);
    ffxFsr3UpscalerGetJitterOffset(&jx,&jy,int(m_frame%phase),phase);
    x=2*jx/m_fsr31->w;y=-2*jy/m_fsr31->h;
}
bool Device::ResolveFsr31(){
    m_frameGenReady=false;
    if(!EnsureFsr31() || !EnsureSceneCopy())return false;
    auto& s=*m_fsr31;PerformanceScope scope(m_perf,m_device.Get(),m_ctx.Get(),Performance::Temporal);
    bool reset=!m_taaValid || m_fsr31Frame+1!=m_frame;
    float change=0;for(int r=0;r<4;r++)for(int c=0;c<4;c++)change+=std::fabs(m_waterViewProjection[r][c]-s.previousVP[r][c]);
    reset|=change>.5f;
    float jx=0,jy=0;Fsr31Jitter(jx,jy);
    Inputs cb={};memcpy(cb.inverse,m_sun.clipToWorld,sizeof(cb.inverse));memcpy(cb.previous,s.previousVP,sizeof(cb.previous));
    cb.screen[0]=float(s.w);cb.screen[1]=float(s.h);cb.screen[2]=-1.f/s.w+jx;cb.screen[3]=1.f/s.h+jy;
    cb.flags[0]=reset?0.f:1.f;cb.flags[1]=m_waterRegionReady && m_waterTerrainSRV?1.f:0.f;
    cb.flags[2]=m_waterRegionLevel;cb.flags[3]=float(m_pusherCount);memcpy(cb.pushers,m_pushers,sizeof(cb.pushers));
    D3D11_MAPPED_SUBRESOURCE map;if(FAILED(m_ctx->Map(s.cb.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map))){
        Log("FSR31 constant-buffer mapping failed; disabling and falling back");m_fsr31Failed=true;ReleaseFsr31();return false;
    }
    memcpy(map.pData,&cb,sizeof(cb));m_ctx->Unmap(s.cb.Get(),0);
    ID3D11ShaderResourceView* none[16]={};m_ctx->PSSetShaderResources(0,16,none);m_ctx->OMSetRenderTargets(0,nullptr,nullptr);CopySceneColor();
    ID3D11ShaderResourceView* inputs[3]={m_sceneCopySRV.Get(),m_autoDepth->m_srv.Get(),cb.flags[1]>0?m_waterTerrainSRV.Get():nullptr};
    ID3D11UnorderedAccessView* outputs[4]={s.color.uav.Get(),s.depth.uav.Get(),s.motion.uav.Get(),s.reactive.uav.Get()};
    m_ctx->CSSetShader(s.inputs.Get(),nullptr,0);m_ctx->CSSetConstantBuffers(0,1,s.cb.GetAddressOf());
    m_ctx->CSSetSamplers(0,1,m_linearClamp.GetAddressOf());m_ctx->CSSetShaderResources(0,3,inputs);m_ctx->CSSetUnorderedAccessViews(0,4,outputs,nullptr);
    m_ctx->Dispatch((s.w+7)/8,(s.h+7)/8,1);
    ID3D11UnorderedAccessView* nullUav[64]={};m_ctx->CSSetUnorderedAccessViews(0,64,nullUav,nullptr);m_ctx->CSSetShaderResources(0,16,none);
    FfxFsr3UpscalerDispatchDescription d={};d.commandList=ffxGetCommandListDX11(m_ctx.Get());
    d.color=s.color.Resource();d.depth=s.depth.Resource();d.motionVectors=s.motion.Resource();d.reactive=s.reactive.Resource();
    d.dilatedDepth=s.dilatedDepth.Resource();d.dilatedMotionVectors=s.dilatedMotion.Resource();d.reconstructedPrevNearestDepth=s.previousDepth.Resource();d.output=s.output.Resource();
    d.renderSize=d.upscaleSize={s.w,s.h};d.motionVectorScale={float(s.w),float(s.h)};d.jitterOffset={jx*s.w*.5f,-jy*s.h*.5f};
    ULONGLONG now=GetTickCount64();d.frameTimeDelta=s.tick?float((std::max)(1ULL,(std::min)(100ULL,now-s.tick))):16.667f;
    d.preExposure=1;d.reset=reset;d.enableSharpening=true;d.sharpness=g_config.fsrSharpness;
    // Derive perspective clip ranges and FOV from the captured inverse VP.
    auto point=[&](float y,float depth,float out[3]){float w=cb.inverse[3][1]*y+cb.inverse[3][2]*depth+cb.inverse[3][3];
        if(std::fabs(w)<1e-8f)return false;for(int r=0;r<3;r++)out[r]=(cb.inverse[r][1]*y+cb.inverse[r][2]*depth+cb.inverse[r][3])/w;return true;};
    float nearP[3],farP[3],top[3];d.cameraNear=.1f;d.cameraFar=10000;d.cameraFovAngleVertical=1;
    if(point(0,0,nearP) && point(0,1,farP) && point(1,0,top)){
        float n=0,f=0,a=0,b=0,dot=0;for(int k=0;k<3;k++){float u=nearP[k]-m_sun.camera[k],v=top[k]-m_sun.camera[k];n+=u*u;f+=(farP[k]-m_sun.camera[k])*(farP[k]-m_sun.camera[k]);a+=u*u;b+=v*v;dot+=u*v;}
        d.cameraNear=(std::max)(.01f,std::sqrt(n));d.cameraFar=(std::max)(d.cameraNear+1,std::sqrt(f));
        if(a*b>1e-8f)d.cameraFovAngleVertical=(std::max)(.1f,(std::min)(3.f,2*std::acos((std::max)(-1.f,(std::min)(1.f,dot/std::sqrt(a*b))))));
    }
    d.viewSpaceToMetersFactor=1;
    FfxErrorCode result=ffxFsr3UpscalerContextDispatch(s.context.get(),&d);
    m_ctx->CSSetUnorderedAccessViews(0,64,nullUav,nullptr);m_ctx->CSSetShaderResources(0,16,none);m_ctx->CSSetShader(nullptr,nullptr,0);
    if(result!=FFX_OK){Log("FSR31 dispatch error 0x%x; disabling and falling back",unsigned(result));m_fsr31Failed=true;ReleaseFsr31();}
    else{
        LogOnce("fsr31-dispatch-active","FSR31 SDK dispatch active: native temporal AA before depth clear/HUD; camera motion only");
        bool fgWanted=g_config.frameGeneration && g_config.syncInterval==1 && !m_frameGenFailed && s.w==m_swapW && s.h==m_swapH;
        if(!fgWanted)s.fg.reset();
        else try{
            bool first=!s.fg;
            if(first){
                s.fg=std::make_unique<FrameGenState>();
                if(!s.fg->Create(m_device.Get(),s.w,s.h))throw "context";
                Log("FrameGen DX11 x86 contexts ready: %ux%u, synchronous VSync prototype",s.w,s.h);
            }
            auto fgParams=d;fgParams.reset|=first;
            if(!s.fg->Dispatch(fgParams,m_frame))throw "dispatch";
            m_frameGenReady=!fgParams.reset;
            LogOnce("framegen-dispatch","FrameGen optical flow + interpolation dispatched; generated presentation requires valid history, native size and VSync");
        }catch(...){m_frameGenFailed=true;s.fg.reset();Log("FrameGen initialization/dispatch failed; real frames and FSR31 retained");}
        m_ctx->CSSetUnorderedAccessViews(0,64,nullUav,nullptr);m_ctx->CSSetShaderResources(0,16,none);m_ctx->CSSetShader(nullptr,nullptr,0);
        FullscreenPass(s.copy.Get(),m_backBuffer->Rtv(),s.w,s.h);
        auto* output=g_config.frameGenerationDebug && m_frameGenReady && s.fg ? s.fg->generated.srv.Get():s.output.srv.Get();
        m_ctx->PSSetShaderResources(0,1,&output);m_ctx->Draw(3,0);m_ctx->PSSetShaderResources(0,16,none);
        memcpy(s.previousVP,m_waterViewProjection,sizeof(s.previousVP));s.tick=now;m_taaValid=true;m_fsr31Frame=m_frame;
    }
    m_rtDirty=m_viewportDirty=true;m_lastVS=nullptr;m_lastPS=nullptr;m_lastLayout=nullptr;
    m_lastBlend=nullptr;m_lastDepth=nullptr;m_lastRaster=nullptr;
    return result==FFX_OK;
}

bool Device::PresentGeneratedFrame(){
    if(!g_config.effectsEnabled || !g_config.frameGeneration || g_config.frameGenerationDebug || !g_config.fsr31 || g_config.syncInterval!=1 ||
       !m_frameGenReady || m_frameGenFailed || !m_fsr31 || !m_fsr31->fg || m_fsr31Frame!=m_frame ||
       m_fsr31->w!=m_swapW || m_fsr31->h!=m_swapH)return false;
    m_frameGenReady=false;
    auto& s=*m_fsr31;
    FullscreenPass(s.fg->composite.Get(),m_swapRTV.Get(),m_swapW,m_swapH);
    m_ctx->PSSetConstantBuffers(0,1,m_clearCB.GetAddressOf());
    ID3D11ShaderResourceView* inputs[4]={s.fg->generated.srv.Get(),s.output.srv.Get(),m_backBuffer->ResolvedSrv(),m_gammaSRV.Get()};
    m_ctx->PSSetShaderResources(0,4,inputs);m_ctx->Draw(3,0);ID3D11ShaderResourceView* none[4]={};m_ctx->PSSetShaderResources(0,4,none);
    ReplayOverlay();
    HRESULT hr=m_swap->Present(1,0);
    // Return true once the swap target was overwritten, even if Present failed:
    // the caller must restore the real image before its normal Present.
    if(FAILED(hr)){m_frameGenFailed=true;Log("Generated Present failed 0x%x",unsigned(hr));return true;}
    ++m_generatedPresents;m_generatedPresentFrame=m_frame;
    LogOnce("framegen-present","Generated frames are being presented before real frames (synchronous VSync x2 prototype)");
    return true;
}
