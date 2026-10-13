#include "device.h"
#include <fstream>
#include <iterator>
#include <cmath>

namespace {
struct TemporalConstants {
    float inverse[4][4],previous[4][4],screen[4],previousOffset[4],options[4];
};
float Halton(unsigned i,unsigned base){float f=1,r=0;while(i){f/=base;r+=f*(i%base);i/=base;}return r;}
bool Target(ID3D11Device* d,UINT w,UINT h,DXGI_FORMAT f,ComPtr<ID3D11Texture2D>& t,
            ComPtr<ID3D11RenderTargetView>& r,ComPtr<ID3D11ShaderResourceView>& s){
    s.Reset();r.Reset();t.Reset();
    D3D11_TEXTURE2D_DESC desc={w,h,1,1,f,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};
    return SUCCEEDED(d->CreateTexture2D(&desc,nullptr,&t)) && SUCCEEDED(d->CreateRenderTargetView(t.Get(),nullptr,&r)) && SUCCEEDED(d->CreateShaderResourceView(t.Get(),nullptr,&s));
}
bool ConstantBuffer(ID3D11Device* d,UINT size,ComPtr<ID3D11Buffer>& b){
    D3D11_BUFFER_DESC desc={size,D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0};
    return SUCCEEDED(d->CreateBuffer(&desc,nullptr,&b));
}
std::string Read(const std::string& path){std::ifstream f(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(f),{});}
const char* CopyPS="Texture2D<float4> image:register(t0);SamplerState smp:register(s0);float4 main(float4 p:SV_Position):SV_Target{uint w,h;image.GetDimensions(w,h);return image.SampleLevel(smp,p.xy/float2(w,h),0);}";
const char* TemporalPS=R"(
cbuffer AA:register(b0){float4 inverse[4];float4 previous[4];float4 screen;float4 prevOffset;float4 options;};
Texture2D<float4> currentColor:register(t0);Texture2D<float4> historyColor:register(t1);
Texture2D<float> historyDepth:register(t3);SamplerState smp:register(s0);
struct Out{float4 color:SV_Target0;float depth:SV_Target1;};
Out main(float4 pos:SV_Position){
    int2 size=int2(1/screen.xy);
    float2 shift=float2(options.z*.5,-options.w*.5);
    float2 uv=pos.xy*screen.xy,currentUV=uv+shift;
    int2 pixel=clamp(int2(currentUV*size),int2(0,0),size-1);
    float d=LoadDepth(pixel);float3 now=currentColor.SampleLevel(smp,currentUV,0).rgb;
    float3 lo=now,hi=now;
    [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++){
        float3 c=currentColor.SampleLevel(smp,currentUV+float2(x,y)*screen.xy,0).rgb;lo=min(lo,c);hi=max(hi,c);
    }
    float2 ndc=float2(currentUV.x*2-1,1-currentUV.y*2)-screen.zw;
    float4 q=float4(ndc,d,1);
    float4 world=float4(dot(q,inverse[0]),dot(q,inverse[1]),dot(q,inverse[2]),dot(q,inverse[3]));
    world/=abs(world.w)>1e-8?world.w:1e-8;
    float4 old=float4(dot(world,previous[0]),dot(world,previous[1]),dot(world,previous[2]),dot(world,previous[3]));
    float2 oldUV=uv;float weight=0;
    if(options.y>.5 && old.w>1e-8 && d<.999999){
        float3 n=old.xyz/old.w;float2 xy=n.xy+prevOffset.xy;
        oldUV=float2(xy.x*.5+.5,.5-xy.y*.5);
        if(all(oldUV>screen.xy*.5) && all(oldUV<1-screen.xy*.5)){
            float previousD=historyDepth.SampleLevel(smp,oldUV,0);
            if(abs(previousD-n.z)<.0015 && previousD<.999999){
                float motion=length((oldUV-uv)/screen.xy);
                weight=options.x/(1+motion*.06);
            }
        }
    }
    float3 history=historyColor.SampleLevel(smp,oldUV,0).rgb;
    history=clamp(history,lo,hi);
    float difference=length(history-now);
    weight*=1-smoothstep(.15,.6,difference);
    Out o;o.color=float4(lerp(now,history,weight),1);o.depth=d;return o;
})";
}

void Device::FullscreenPass(ID3D11PixelShader* ps,ID3D11RenderTargetView* rtv,UINT w,UINT h)
{
    m_ctx->OMSetRenderTargets(1,&rtv,nullptr);
    D3D11_VIEWPORT vp={0,0,(float)w,(float)h,0,1};m_ctx->RSSetViewports(1,&vp);
    m_ctx->RSSetState(m_noCull.Get());m_ctx->OMSetBlendState(m_noBlend.Get(),nullptr,~0u);
    m_ctx->OMSetDepthStencilState(m_noDepth.Get(),0);m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_resolveVS.Get(),nullptr,0);m_ctx->PSSetShader(ps,nullptr,0);
    m_ctx->PSSetSamplers(0,1,m_linearClamp.GetAddressOf());
}
bool Device::EnsureTemporal()
{
    if(EnsureFsr31())return true;
    if(!g_config.effectsEnabled || !g_config.taa){m_taaValid=false;return false;}
    if(m_taaFailed || !m_backBuffer || !m_autoDepth || !m_autoDepth->m_srv)return false;
    const UINT w=m_backBuffer->m_width,h=m_backBuffer->m_height,samples=m_autoDepth->m_samples;
    if(m_taaPS && m_taaW==w && m_taaH==h && m_taaSamples==samples)return true;
    m_taaValid=false;
    std::string depth=samples>1 ?
        "Texture2DMS<float,"+std::to_string(samples)+"> sceneDepth:register(t2);float LoadDepth(int2 p){float d=1;[unroll]for(int i=0;i<"+std::to_string(samples)+";i++)d=min(d,sceneDepth.Load(p,i));return d;}\n":
        "Texture2D<float> sceneDepth:register(t2);float LoadDepth(int2 p){return sceneDepth.Load(int3(p,0));}\n";
    auto ps=CompileHLSL(depth+TemporalPS,"ps_4_0","temporal_aa");auto copy=CompileHLSL(CopyPS,"ps_4_0","aa_copy");
    bool ok=ps && copy && SUCCEEDED(m_device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&m_taaPS)) &&
        SUCCEEDED(m_device->CreatePixelShader(copy->GetBufferPointer(),copy->GetBufferSize(),nullptr,&m_aaCopyPS)) &&
        ConstantBuffer(m_device.Get(),sizeof(TemporalConstants),m_taaCB);
    for(int i=0;i<2 && ok;i++)ok=Target(m_device.Get(),w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,m_taaColor[i],m_taaColorRTV[i],m_taaColorSRV[i]) &&
        Target(m_device.Get(),w,h,DXGI_FORMAT_R32_FLOAT,m_taaDepth[i],m_taaDepthRTV[i],m_taaDepthSRV[i]);
    if(!ok){m_taaFailed=true;Log("TAA unavailable: resource/shader creation failed; jitter disabled");return false;}
    // Initialize both histories: avoids undefined reads even when history weight is zero.
    float clear[4]={};for(int i=0;i<2;i++){
        m_ctx->ClearRenderTargetView(m_taaColorRTV[i].Get(),clear);
        float farDepth[4]={1,1,1,1};m_ctx->ClearRenderTargetView(m_taaDepthRTV[i].Get(),farDepth);
    }
    m_taaW=w;m_taaH=h;m_taaSamples=samples;
    Log("TAA resources ready: %ux%u, depth samples %u, jitter + camera reprojection (no object motion vectors)",w,h,samples);
    return true;
}
void Device::TemporalJitter(float& x,float& y) const
{
    x=y=0;
    if(g_config.effectsEnabled && g_config.fsr31 && m_fsr31 && !m_fsr31Failed){Fsr31Jitter(x,y);return;}
    if(!g_config.effectsEnabled || !g_config.taa || m_taaFailed || !m_taaPS || !m_taaW || !m_taaH)return;
    unsigned i=unsigned(m_frame%8)+1;x=(Halton(i,2)*2-1)/m_taaW;y=(Halton(i,3)*2-1)/m_taaH;
}
void Device::ResolveTemporal()
{
    if(ResolveFsr31())return;
    if(!EnsureTemporal() || !EnsureSceneCopy())return;
    PerformanceScope scope(m_perf,m_device.Get(),m_ctx.Get(),Performance::Temporal);
    if(m_taaFrame+1!=m_frame)m_taaValid=false;
    float cameraChange=0;for(int r=0;r<4;r++)for(int c=0;c<4;c++)cameraChange+=std::fabs(m_waterViewProjection[r][c]-m_taaPrevVP[r][c]);
    if(cameraChange>.5f)m_taaValid=false;
    float jitterX=0,jitterY=0;TemporalJitter(jitterX,jitterY);
    TemporalConstants cb={};memcpy(cb.inverse,m_sun.clipToWorld,sizeof(cb.inverse));memcpy(cb.previous,m_taaPrevVP,sizeof(cb.previous));
    cb.screen[0]=1.f/m_taaW;cb.screen[1]=1.f/m_taaH;
    cb.screen[2]=-cb.screen[0]+jitterX;cb.screen[3]=cb.screen[1]+jitterY;
    memcpy(cb.previousOffset,m_taaPrevOffset,sizeof(cb.previousOffset));
    cb.options[0]=g_config.taaHistory;cb.options[1]=m_taaValid?1.f:0.f;cb.options[2]=jitterX;cb.options[3]=jitterY;
    D3D11_MAPPED_SUBRESOURCE map;
    if(FAILED(m_ctx->Map(m_taaCB.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map))){m_taaValid=false;m_taaFailed=true;return;}
    memcpy(map.pData,&cb,sizeof(cb));m_ctx->Unmap(m_taaCB.Get(),0);
    ID3D11ShaderResourceView* nulls[4]={};m_ctx->PSSetShaderResources(0,4,nulls);m_ctx->OMSetRenderTargets(0,nullptr,nullptr);
    CopySceneColor();
    int next=1-m_taaIndex;
    FullscreenPass(m_taaPS.Get(),m_taaColorRTV[next].Get(),m_taaW,m_taaH);
    ID3D11RenderTargetView* targets[2]={m_taaColorRTV[next].Get(),m_taaDepthRTV[next].Get()};m_ctx->OMSetRenderTargets(2,targets,nullptr);
    ID3D11ShaderResourceView* inputs[4]={m_sceneCopySRV.Get(),m_taaColorSRV[m_taaIndex].Get(),m_autoDepth->m_srv.Get(),m_taaDepthSRV[m_taaIndex].Get()};
    m_ctx->PSSetShaderResources(0,4,inputs);m_ctx->PSSetConstantBuffers(0,1,m_taaCB.GetAddressOf());m_ctx->Draw(3,0);
    m_ctx->PSSetShaderResources(0,4,nulls);
    FullscreenPass(m_aaCopyPS.Get(),m_backBuffer->Rtv(),m_taaW,m_taaH);
    ID3D11ShaderResourceView* result=m_taaColorSRV[next].Get();m_ctx->PSSetShaderResources(0,1,&result);m_ctx->Draw(3,0);m_ctx->PSSetShaderResources(0,4,nulls);
    m_taaIndex=next;m_taaValid=true;m_taaFrame=m_frame;
    memcpy(m_taaPrevVP,m_waterViewProjection,sizeof(m_taaPrevVP));m_taaPrevOffset[0]=-cb.screen[0];m_taaPrevOffset[1]=cb.screen[1];
    m_rtDirty=m_viewportDirty=true;m_lastVS=nullptr;m_lastPS=nullptr;m_lastLayout=nullptr;
    m_lastBlend=nullptr;m_lastDepth=nullptr;m_lastRaster=nullptr;
}

ID3D11ShaderResourceView* Device::PrepareFsr(ID3D11ShaderResourceView* source)
{
    if(g_config.effectsEnabled && g_config.fsr31 && !m_fsr31Failed && m_fsr31Frame==m_frame)return source;
    if(!g_config.effectsEnabled || !g_config.fsr1 || m_fsrFailed || !source)return source;
    if(!m_fsrEasuPS || !m_fsrRcasPS){
        std::string a=Read(g_gameDir+"shaders\\fsr1\\ffx_a.h"),fsr=Read(g_gameDir+"shaders\\fsr1\\ffx_fsr1.h");
        if(a.empty() || fsr.empty()){m_fsrFailed=true;Log("FSR1 unavailable: missing AMD shader headers");return source;}
        std::string base="#define A_GPU 1\n#define A_HLSL 1\n"+a+"\ncbuffer Fsr:register(b0){float4 dimensions;float4 options;}\nTexture2D<float4> image:register(t0);SamplerState smp:register(s0);\n";
        std::string easu=base+"\n#define FSR_EASU_F 1\nfloat4 FsrEasuRF(float2 p){return image.GatherRed(smp,p);}\nfloat4 FsrEasuGF(float2 p){return image.GatherGreen(smp,p);}\nfloat4 FsrEasuBF(float2 p){return image.GatherBlue(smp,p);}\n"+fsr+R"(
float4 main(float4 p:SV_Position):SV_Target{uint4 c0,c1,c2,c3;
FsrEasuCon(c0,c1,c2,c3,dimensions.x,dimensions.y,dimensions.x,dimensions.y,dimensions.z,dimensions.w);
float3 color;FsrEasuF(color,uint2(p.xy),c0,c1,c2,c3);return float4(saturate(color),1);})";
        std::string rcas=base+"\n#define FSR_RCAS_F 1\n#define FSR_RCAS_DENOISE 1\nfloat4 FsrRcasLoadF(int2 p){return image.Load(int3(clamp(p,int2(0,0),int2(dimensions.zw)-1),0));}\nvoid FsrRcasInputF(inout float r,inout float g,inout float b){}\n"+fsr+R"(
float4 main(float4 p:SV_Position):SV_Target{uint4 con;FsrRcasCon(con,options.x);float r,g,b;FsrRcasF(r,g,b,uint2(p.xy),con);return float4(saturate(float3(r,g,b)),1);})";
        auto e=CompileHLSL(easu,"ps_5_0","fsr1_easu"),r=CompileHLSL(rcas,"ps_5_0","fsr1_rcas");
        if(!e || !r || FAILED(m_device->CreatePixelShader(e->GetBufferPointer(),e->GetBufferSize(),nullptr,&m_fsrEasuPS)) ||
           FAILED(m_device->CreatePixelShader(r->GetBufferPointer(),r->GetBufferSize(),nullptr,&m_fsrRcasPS)) || !ConstantBuffer(m_device.Get(),32,m_fsrCB)){
            m_fsrFailed=true;Log("FSR1 unavailable: compile/create failed; original presentation retained");return source;
        }
        Log("AMD FSR1 EASU + RCAS ready (official GPUOpen shader source, 32-bit float path)");
    }
    if(m_fsrW!=m_swapW || m_fsrH!=m_swapH){
        for(int i=0;i<2;i++)if(!Target(m_device.Get(),m_swapW,m_swapH,DXGI_FORMAT_B8G8R8A8_UNORM,m_fsrTex[i],m_fsrRTV[i],m_fsrSRV[i])){m_fsrFailed=true;return source;}
        m_fsrW=m_swapW;m_fsrH=m_swapH;
    }
    const UINT inW=m_backBuffer->m_width,inH=m_backBuffer->m_height;
    // EASU is for upscaling, not arbitrary aspect changes or downsampling.
    bool upscale=inW<m_fsrW && inH<m_fsrH && std::fabs(float(inW)/inH-float(m_fsrW)/m_fsrH)<.02f;
    if(!upscale && (inW!=m_fsrW || inH!=m_fsrH))return source;
    PerformanceScope scope(m_perf,m_device.Get(),m_ctx.Get(),Performance::Upscale);
    float cb[8]={(float)inW,(float)inH,(float)m_fsrW,(float)m_fsrH,2.f*(1-g_config.fsrSharpness),0,0,0};
    D3D11_MAPPED_SUBRESOURCE map;if(FAILED(m_ctx->Map(m_fsrCB.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&map)))return source;
    memcpy(map.pData,cb,sizeof(cb));m_ctx->Unmap(m_fsrCB.Get(),0);m_ctx->PSSetConstantBuffers(0,1,m_fsrCB.GetAddressOf());
    ID3D11ShaderResourceView* null=nullptr;
    if(upscale){FullscreenPass(m_fsrEasuPS.Get(),m_fsrRTV[0].Get(),m_fsrW,m_fsrH);m_ctx->PSSetShaderResources(0,1,&source);m_ctx->Draw(3,0);m_ctx->PSSetShaderResources(0,1,&null);source=m_fsrSRV[0].Get();}
    FullscreenPass(m_fsrRcasPS.Get(),m_fsrRTV[1].Get(),m_fsrW,m_fsrH);m_ctx->PSSetShaderResources(0,1,&source);m_ctx->Draw(3,0);m_ctx->PSSetShaderResources(0,1,&null);
    return m_fsrSRV[1].Get();
}
