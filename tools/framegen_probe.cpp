#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <wrl/client.h>
#include <memory>
#include <cstdio>
#include <cstdlib>
using Microsoft::WRL::ComPtr;
static decltype(FfxInterface::fpCreatePipeline) createPipeline;
static unsigned failures=0,computeFailures=0;
static decltype(FfxInterface::fpCreateResource) createResource;
static FfxErrorCode TraceResource(FfxInterface* api,const FfxCreateResourceDescription* desc,FfxUInt32 id,FfxResourceInternal* out){
    std::printf("Resource create %ls: %u x %u, format %u\n",desc->name,desc->resourceDescription.width,desc->resourceDescription.height,unsigned(desc->resourceDescription.format));
    auto result=createResource(api,desc,id,out);std::printf("Resource result 0x%x\n",unsigned(result));return result;
}
static FfxErrorCode TracePipeline(FfxInterface* api,FfxEffect effect,FfxPass pass,uint32_t permutation,
 const FfxPipelineDescription* desc,FfxUInt32 id,FfxPipelineState* out){
    auto result=createPipeline(api,effect,pass,permutation,desc,id,out);
    std::printf("Pipeline effect=%u pass=%u stage=%u result=0x%x\n",unsigned(effect),unsigned(pass),unsigned(desc->stage),unsigned(result));
    if(result!=FFX_OK){++failures;if(desc->stage==FFX_BIND_COMPUTE_SHADER_STAGE)++computeFailures;}
    return result;
}
int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    std::printf("Pointer size %zu bits\n",sizeof(void*)*8);
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;
    D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1};
    if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,1,D3D11_SDK_VERSION,&d,nullptr,&c)))return 2;
    size_t bytes=ffxGetScratchMemorySizeDX11(2);void* scratch=calloc(1,bytes);if(!scratch)return 2;
    FfxInterface api={};if(ffxGetInterfaceDX11(&api,ffxGetDeviceDX11_Fsr31(d.Get()),scratch,bytes,2)!=FFX_OK)return 2;
    createPipeline=api.fpCreatePipeline;api.fpCreatePipeline=TracePipeline;
    createResource=api.fpCreateResource;api.fpCreateResource=TraceResource;
    auto of=std::make_unique<FfxOpticalflowContext>();auto fi=std::make_unique<FfxFrameInterpolationContext>();
    FfxOpticalflowContextDescription od={};od.backendInterface=api;od.resolution={128,128};
    auto oe=ffxOpticalflowContextCreate(of.get(),&od);std::printf("Optical flow create: 0x%x\n",unsigned(oe));
    FfxFrameInterpolationContextDescription fd={};fd.backendInterface=api;fd.maxRenderSize=fd.displaySize={128,128};fd.backBufferFormat=FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    auto fe=ffxFrameInterpolationContextCreate(fi.get(),&fd);std::printf("Interpolation create: 0x%x\n",unsigned(fe));
    std::printf("Pipeline failures: %u (compute %u)\n",failures,computeFailures);
    if(fe==FFX_OK)ffxFrameInterpolationContextDestroy(fi.get());
    if(oe==FFX_OK)ffxOpticalflowContextDestroy(of.get());free(scratch);
    return oe!=FFX_OK || fe!=FFX_OK || computeFailures?1:0;
}
