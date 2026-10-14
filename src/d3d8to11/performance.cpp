#include "performance.h"
#include "device.h"
#include <psapi.h>
#include <dxgi1_4.h>
#include <share.h>
#pragma comment(lib,"psapi.lib")

void Performance::Poll(ID3D11DeviceContext* c)
{
    for(auto& s:slots) if(s.pending) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dis={};
        if(c->GetData(s.disjoint.Get(),&dis,sizeof(dis),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)continue;
        UINT64 starts[Events]={},ends[Events]={}; bool ready=true;
        for(int i=0;i<s.used;i++) {
            ready &= c->GetData(s.events[i].begin.Get(),&starts[i],sizeof(UINT64),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK;
            ready &= c->GetData(s.events[i].end.Get(),&ends[i],sizeof(UINT64),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK;
        }
        if(!ready)continue;
        valid=!dis.Disjoint && dis.Frequency && !s.overflow;
        if(valid) {
            gpuTick=GetTickCount64();
            double times[Count]={};
            for(int i=0;i<s.used;i++)times[s.events[i].stage]+=(ends[i]-starts[i])*1000.0/dis.Frequency;
            for(int i=0;i<Count;i++)gpu[i]=times[i];
        } else ++skipped;
        s.pending=false;
    }
}
void Performance::Begin(ID3D11Device* d,ID3D11DeviceContext* c)
{
    if(attempted)return;
    attempted=true;
    Poll(c);
    if(!enabled || failed || serial%30)return;
    for(int i=0;i<Slots;i++)if(!slots[i].pending){current=i;break;}
    if(current<0){++skipped;return;}
    auto& s=slots[current];
    if(!s.disjoint){D3D11_QUERY_DESC desc={D3D11_QUERY_TIMESTAMP_DISJOINT,0};
        if(FAILED(d->CreateQuery(&desc,&s.disjoint))){failed=true;current=-1;return;}}
    s.used=0;s.overflow=false;active=true;c->Begin(s.disjoint.Get());
    Start(d,c,Frame);
}
int Performance::Start(ID3D11Device* d,ID3D11DeviceContext* c,int stage)
{
    if(!active)return -1;
    auto& s=slots[current];
    if(s.used==Events){s.overflow=true;return -1;}
    auto& e=s.events[s.used];
    D3D11_QUERY_DESC desc={D3D11_QUERY_TIMESTAMP,0};
    if(!e.begin && FAILED(d->CreateQuery(&desc,&e.begin))){s.overflow=true;return -1;}
    if(!e.end && FAILED(d->CreateQuery(&desc,&e.end))){s.overflow=true;return -1;}
    e.stage=stage;c->End(e.begin.Get());return s.used++;
}
void Performance::Stop(ID3D11DeviceContext* c,int e)
{if(active && e>=0)c->End(slots[current].events[e].end.Get());}
void Performance::Finish(ID3D11DeviceContext* c)
{
    attempted=false;++serial;
    if(!active)return;
    auto& s=slots[current];
    if(s.used && s.events[0].stage==Frame)Stop(c,0);
    c->End(s.disjoint.Get());s.pending=true;active=false;current=-1;
}
void Performance::Tick(double frequency)
{
    LARGE_INTEGER now;QueryPerformanceCounter(&now);
    if(lastFrame.QuadPart){frameMs=(now.QuadPart-lastFrame.QuadPart)*1000.0/frequency;
        windowMs+=frameMs;++windowFrames;
        if(frameMs>windowPeak)windowPeak=frameMs;
        if(frameMs>peakMs)peakMs=frameMs;
        history[historyAt++%180]=(float)frameMs;}
    lastFrame=now;
}

void Device::RecordPerformance()
{
    auto& p=m_perf;const ULONGLONG now=GetTickCount64();
    if(p.cpuMeasured){for(int i=0;i<Performance::CpuCount;++i){p.cpuShown[i]=p.cpu[i];p.cpuWindow[i]+=p.cpu[i];}++p.cpuSamples;}
    for(double& value:p.cpu)value=0;
    p.cpuMeasured=false;
    // The first Present has no frame-to-frame interval yet; do not mix its
    // draw/wait counts into the following interval's averages.
    if(!p.historyAt){p.csvOrigin=p.csvTick=now;return;}
    p.windowPresent+=p.presentMs;p.windowLimiter+=p.limiterMs;
    p.windowDraws+=m_perfDraws;p.windowWater+=m_perfWaterDraws;p.windowShadows+=m_perfShadowDraws;
    if(!g_config.performanceLog){
        if(p.csv){fclose(p.csv);p.csv=nullptr;++p.csvPart;}
        p.csvTick=now;p.windowFrames=0;p.windowMs=p.windowPeak=p.windowPresent=p.windowLimiter=0;
        p.windowDraws=p.windowWater=p.windowShadows=0;p.cpuSamples=0;
        for(double& value:p.cpuWindow)value=0;
        return;
    }
    if(p.csvFailed)return;
    if(!p.csvOrigin)p.csvOrigin=now;
    if(!p.csvTick)p.csvTick=now;
    if(now-p.csvTick<1000)return;
    if(!p.csv){
        SYSTEMTIME t;GetLocalTime(&t);char name[160];
        sprintf_s(name,"d3d8to11_performance_%04u%02u%02u_%02u%02u%02u_%lu_%u.csv",
            t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,GetCurrentProcessId(),p.csvPart);
        p.csvPath=g_gameDir+name;p.csv=_fsopen(p.csvPath.c_str(),"w",_SH_DENYNO);
        if(!p.csv){p.csvFailed=true;Log("Performance CSV creation failed: %s",p.csvPath.c_str());return;}
        fputs("elapsed_s,frames,frame_avg_ms,frame_peak_ms,present_avg_ms,limiter_avg_ms,gpu_valid,gpu_age_ms,gpu_total_ms,gpu_ao_shafts_ms,gpu_composition_ms,gpu_water_copy_ms,gpu_taa_ms,gpu_fsr1_ms,private_mib,resident_mib,private_delta_mib,gpu_budget_valid,gpu_local_mib,gpu_budget_mib,draws_avg,water_draws_avg,shadow_draws_avg,surfaces,textures,volumes,vertex_buffers,index_buffers,blend_cache,depth_cache,raster_cache,sampler_cache,vs_live,ps_live,ps_variants,ff_shaders,dynamic_buffers_mib,msaa,effects,shadows,ao,shafts,normals,grass,water,interaction,ssr,ssr_steps,gpu_skipped,taa,fsr1,water_surface_mode,water_region_ready,water_ocean_cells,water_pond_cells,water_map_cpu_ms,fsr31_requested,fsr31_active,fsr31_failed,fg_requested,fg_presented,fg_failed,generated_presents_total,cpu_prepare_sample_avg_ms,cpu_tess_sample_avg_ms,cpu_shadow_sample_avg_ms,cpu_sample_frames,tess_cache_hits_total,tess_cache_misses_total,tess_cache_entries,tess_cache_mib,effective_fps_limit,effective_vsync,effective_anisotropy,cpu_setup_sample_avg_ms,cpu_scene_sample_avg_ms,cpu_diagnostics_sample_avg_ms,cpu_vertex_sample_avg_ms,cpu_constants_sample_avg_ms,cpu_resources_sample_avg_ms,cpu_pipeline_sample_avg_ms,cpu_geometry_upload_sample_avg_ms,duplicate_constant_writes_total,vs_constant_uploads_total,ps_constant_uploads_total,geometry_uploads_total,geometry_upload_bytes_total,redundant_render_states_total,redundant_texture_states_total,sampler_selection_hits_total,sampler_selection_lookups_total,redundant_binds_total,vs_constant_bytes_total,constant_ring\n",p.csv);
        Log("Performance CSV (1 sample/sec): %s",p.csvPath.c_str());p.csvRows=0;
    }
    unsigned vs=0,ps=0,variants=0;
    for(auto* s:m_vertexShaders)if(s)++vs;
    for(auto* s:m_pixelShaders)if(s){++ps;variants+=unsigned(s->variants.size());}
    const double mib=1048576.0,frames=p.windowFrames ? p.windowFrames:1;
    const bool gpuValid=p.enabled && p.valid && p.gpuTick && now-p.gpuTick<3000;
    fprintf(p.csv,"%.3f,%u,%.3f,%.3f,%.3f,%.3f,%d,%lld,",
        (now-p.csvOrigin)*.001,p.windowFrames,p.windowMs/frames,p.windowPeak,
        p.windowPresent/frames,p.windowLimiter/frames,gpuValid,p.gpuTick ? (long long)(now-p.gpuTick):-1LL);
    for(int i=0;i<Performance::Count;i++){if(gpuValid)fprintf(p.csv,"%.4f,",p.gpu[i]);else fputs("NA,",p.csv);}
    fprintf(p.csv,"%.2f,%.2f,%.2f,%d,%.2f,%.2f,%.2f,%.2f,%.2f,",
        p.privateBytes/mib,p.workingSet/mib,(double(p.privateBytes)-double(p.basePrivate))/mib,
        p.budgetValid,p.gpuUsage/mib,p.gpuBudget/mib,p.windowDraws/frames,p.windowWater/frames,p.windowShadows/frames);
    for(auto& n:ResourceCount::live)fprintf(p.csv,"%u,",n.load());
    fprintf(p.csv,"%u,%u,%u,%u,%u,%u,%u,%u,%.3f,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%d,%d,%d,%d,%u,%u,%.3f,%d,%d,%d,%d,%d,%d,%llu,",
        unsigned(m_blendStates.size()),unsigned(m_depthStates.size()),unsigned(m_rasterStates.size()),unsigned(m_samplers.size()),
        vs,ps,variants,unsigned(m_ffShaders.size()),(double(m_dynVBSize)+m_dynIBSize)/mib,m_msaaApplied,
        g_config.effectsEnabled,g_config.sunLighting,g_config.aoEnabled,g_config.shafts,g_config.normalMaps,
        g_config.grassPhysics,g_config.waterEnabled,g_config.waterInteraction,g_config.waterSSR,g_config.waterSSRSteps,p.skipped,g_config.taa,g_config.fsr1,
        g_config.waterSurfaceMode,m_waterRegionReady,m_waterRegions.oceanCells,m_waterRegions.pondCells,m_waterRegionCpuMs,
        g_config.fsr31,g_config.effectsEnabled && g_config.fsr31 && !m_fsr31Failed && m_fsr31Frame==m_frame,m_fsr31Failed,
        g_config.frameGeneration,m_generatedPresentFrame==m_frame,m_frameGenFailed,m_generatedPresents);
    if(p.cpuSamples) fprintf(p.csv,"%.4f,%.4f,%.4f,",p.cpuWindow[0]/p.cpuSamples,p.cpuWindow[1]/p.cpuSamples,p.cpuWindow[2]/p.cpuSamples);
    else fputs("NA,NA,NA,",p.csv);
    fprintf(p.csv,"%u,%llu,%llu,%u,%.3f,%d,%d,%d",p.cpuSamples,m_tessCacheHits,m_tessCacheMisses,unsigned(m_tessCache.size()),m_tessCacheBytes/1048576.0,
        g_config.EffectiveFpsLimit(),g_config.EffectiveVSync(),g_config.EffectiveAnisotropy());
    for(int i=Performance::CpuSetup;i<Performance::CpuCount;++i) {
        if(p.cpuSamples)fprintf(p.csv,",%.4f",p.cpuWindow[i]/p.cpuSamples);
        else fputs(",NA",p.csv);
    }
    fprintf(p.csv,",%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d\n",p.duplicateConstants,p.vsUploads,p.psUploads,p.geometryUploads,p.geometryUploadBytes,
        p.redundantRenderStates,p.redundantTextureStates,p.samplerSelectionHits,p.samplerSelectionLookups,
        p.redundantBinds,p.constantBytes,g_config.constantRing?1:0);
    if(fflush(p.csv)!=0 || ferror(p.csv)){p.csvFailed=true;Log("Performance CSV write failed");fclose(p.csv);p.csv=nullptr;}
    else if(++p.csvRows>=36000){fclose(p.csv);p.csv=nullptr;++p.csvPart;}
    p.csvTick=now;p.windowFrames=0;p.windowMs=p.windowPeak=p.windowPresent=p.windowLimiter=0;
    p.windowDraws=p.windowWater=p.windowShadows=0;
    p.cpuSamples=0;for(double& value:p.cpuWindow)value=0;
}
void Performance::SampleMemory(ID3D11Device* d)
{
    DWORD now=GetTickCount();if(memoryValid && now-memoryTick<1000)return;memoryTick=now;
    PROCESS_MEMORY_COUNTERS_EX mem={};mem.cb=sizeof(mem);
    if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem),sizeof(mem))){
        privateBytes=mem.PrivateUsage;workingSet=mem.WorkingSetSize;
        if(!memoryValid)basePrivate=privateBytes;
        if(privateBytes>peakPrivate)peakPrivate=privateBytes;
        memoryValid=true;
    }
    ComPtr<IDXGIDevice> dx;ComPtr<IDXGIAdapter> a;ComPtr<IDXGIAdapter3> a3;
    DXGI_QUERY_VIDEO_MEMORY_INFO info={};budgetValid=false;
    if(SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&dx))) && SUCCEEDED(dx->GetAdapter(&a)) &&
       SUCCEEDED(a.As(&a3)) && SUCCEEDED(a3->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&info))){
        gpuUsage=info.CurrentUsage;gpuBudget=info.Budget;budgetValid=true;
    }
}

double TscMilliseconds()
{
    LARGE_INTEGER f, q0, q1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    const unsigned __int64 t0 = __rdtsc();
    do QueryPerformanceCounter(&q1); while (q1.QuadPart - q0.QuadPart < f.QuadPart / 50); // 20 ms
    const unsigned __int64 t1 = __rdtsc();
    return (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / f.QuadPart / (double)(t1 - t0);
}
