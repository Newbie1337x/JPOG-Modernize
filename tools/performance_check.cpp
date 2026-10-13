#include "../src/d3d8to11/performance.h"
#include "../src/d3d8to11/objects.h"
#include <cstdio>
#include <cmath>
#pragma comment(lib,"d3d11.lib")
int main()
{
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;
    if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&c)))return 1;
    Performance p;p.enabled=true;
    p.Begin(d.Get(),c.Get());
    int first=p.current;
    p.Begin(d.Get(),c.Get());if(p.current!=first || p.slots[first].used!=1)return 2;
    {PerformanceScope s(p,d.Get(),c.Get(),Performance::Effects);}
    p.enabled=false; // turning off mid-frame must still close queries
    p.Finish(c.Get());if(p.active || p.attempted || !p.slots[first].pending)return 3;
    c->Flush(); // test only: renderer never flushes for profiling
    for(int i=0;i<200 && !p.valid;i++){p.Poll(c.Get());Sleep(1);}
    if(!p.valid || !std::isfinite(p.gpu[0]) || p.gpu[0]<p.gpu[1])return 4;
    // Discard overfull samples instead of presenting partial totals.
    p.enabled=true;p.serial=30;p.Begin(d.Get(),c.Get());
    for(int i=0;i<40;i++){int e=p.Start(d.Get(),c.Get(),Performance::WaterCopy);p.Stop(c.Get(),e);}
    p.Finish(c.Get());c->Flush();
    for(int i=0;i<200 && !p.skipped;i++){p.Poll(c.Get());Sleep(1);}
    if(!p.skipped || p.valid)return 5;
    p.SampleMemory(d.Get());if(!p.memoryValid || !p.privateBytes)return 6;
    unsigned before=ResourceCount::live[1].load();
    {ResourceCount count(1);if(ResourceCount::live[1].load()!=before+1)return 7;}
    if(ResourceCount::live[1].load()!=before)return 8;
    std::puts("PASS: async timestamps, mid-frame disable, overflow discard, RAM, lifetime counts");
    return 0;
}
