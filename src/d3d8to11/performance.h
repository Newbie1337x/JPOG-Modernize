#pragma once
#include "common.h"
#include <cstdio>

// Fixed query ring. No Flush, spins or blocking GPU readbacks. Times are inclusive.
struct Performance {
    enum Stage { Frame, Effects, Resolve, WaterCopy, Temporal, Upscale, Count };
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
    ~Performance(){if(csv)fclose(csv);}
    void Begin(ID3D11Device*,ID3D11DeviceContext*);
    int Start(ID3D11Device*,ID3D11DeviceContext*,int);
    void Stop(ID3D11DeviceContext*,int);
    void Finish(ID3D11DeviceContext*);
    void Poll(ID3D11DeviceContext*);
    void SampleMemory(ID3D11Device*);
    void Tick(double frequency);
};
struct PerformanceScope {
    Performance& p; ID3D11DeviceContext* ctx; int event;
    PerformanceScope(Performance& p,ID3D11Device* d,ID3D11DeviceContext* c,int stage):p(p),ctx(c),event(p.Start(d,c,stage)){}
    ~PerformanceScope(){p.Stop(ctx,event);}
};
