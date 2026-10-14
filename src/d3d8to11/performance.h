#pragma once
#include "common.h"
#include <cstdio>
#include <intrin.h>

// Fixed query ring. No Flush, spins or blocking GPU readbacks. Times are inclusive.
struct Performance {
    enum Stage { Frame, Effects, Resolve, WaterCopy, Temporal, Upscale, Count };
    enum CpuStage { CpuPrepare, CpuTess, CpuShadow, CpuSetup, CpuScene,
        CpuDiagnostics, CpuVertex, CpuConstants, CpuResources, CpuPipeline, CpuGeometryUpload, CpuCount };
    static constexpr int Slots=4, Events=32;
    struct Event { ComPtr<ID3D11Query> begin,end; int stage=0; };
    struct Slot { ComPtr<ID3D11Query> disjoint; Event events[Events]; int used=0; bool pending=false, overflow=false; };
    Slot slots[Slots];
    bool enabled=false, active=false, failed=false, valid=false, attempted=false;
    unsigned serial=0;
    int current=-1;
    unsigned skipped=0;
    double gpu[Count]={}, frameMs=0, peakMs=0, presentMs=0, limiterMs=0;
    float history[180]={}; unsigned historyAt=0;
    SIZE_T privateBytes=0, workingSet=0, basePrivate=0, peakPrivate=0;
    UINT64 gpuUsage=0,gpuBudget=0;
    bool memoryValid=false, budgetValid=false;
    DWORD memoryTick=0;
    LARGE_INTEGER lastFrame={};
    ULONGLONG gpuTick=0, csvTick=0, csvOrigin=0;
    FILE* csv=nullptr;
    bool csvFailed=false;
    std::string csvPath;
    unsigned csvPart=0,csvRows=0,windowFrames=0;
    double windowMs=0,windowPeak=0,windowPresent=0,windowLimiter=0;
    UINT64 windowDraws=0,windowWater=0,windowShadows=0;
    double cpu[CpuCount]={},cpuShown[CpuCount]={},cpuWindow[CpuCount]={};
    UINT64 duplicateConstants=0, vsUploads=0, psUploads=0, geometryUploads=0, geometryUploadBytes=0;
    UINT64 redundantRenderStates=0,redundantTextureStates=0,samplerSelectionHits=0,samplerSelectionLookups=0;
    UINT64 redundantBinds=0,constantBytes=0;
    bool cpuMeasured=false; unsigned cpuSamples=0;
    ~Performance(){if(csv)fclose(csv);}
    void Begin(ID3D11Device*,ID3D11DeviceContext*);
    int Start(ID3D11Device*,ID3D11DeviceContext*,int);
    void Stop(ID3D11DeviceContext*,int);
    void Finish(ID3D11DeviceContext*);
    void Poll(ID3D11DeviceContext*);
    void SampleMemory(ID3D11Device*);
    void Tick(double frequency);
};
// Milliseconds per time-stamp counter tick, measured once against QueryPerformanceCounter.
// The stage clock is read about ten times per draw on sampled frames: rdtsc costs a few
// nanoseconds where QueryPerformanceCounter costs about 25.
double TscMilliseconds();
struct CpuPerformanceScope {
    Performance& p; int stage; unsigned __int64 start=0; bool enabled;
    __forceinline CpuPerformanceScope(Performance& perf,int s):p(perf),stage(s),enabled(perf.enabled && perf.serial%30==0) {
        if(enabled){p.cpuMeasured=true;start=__rdtsc();}
    }
    // Unsampled frames (29 of 30) only store the stage: no call, no clock read.
    __forceinline void Next(int nextStage){if(enabled)Measure();stage=nextStage;}
    __declspec(noinline) void Measure(){const unsigned __int64 end=__rdtsc();
        static const double scale=TscMilliseconds();
        p.cpu[stage]+=(double)(end-start)*scale;start=end;}
    __forceinline ~CpuPerformanceScope(){if(enabled)Measure();}
};
struct PerformanceScope {
    Performance& p; ID3D11DeviceContext* ctx; int event;
    PerformanceScope(Performance& p,ID3D11Device* d,ID3D11DeviceContext* c,int stage):p(p),ctx(c),event(p.Start(d,c,stage)){}
    ~PerformanceScope(){p.Stop(ctx,event);}
};
