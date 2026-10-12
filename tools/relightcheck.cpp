// GPU regression against captured JPOG skin shaders: own light, camera invariance,
// animated normals, and fog after lighting. Runs a hidden test window, not SimJP.
#include "../src/d3d8to11/d3d8_types.h"
#include <DirectXMath.h>
#include <vector>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cmath>
using namespace DirectX;
static std::vector<DWORD> read(const char* p) {
    std::ifstream f(p,std::ios::binary|std::ios::ate); auto n=f.tellg();
    if(n<=0) { printf("Missing %s\n",p); exit(2); }
    std::vector<DWORD> a((size_t)n/4); f.seekg(0); f.read((char*)a.data(),n); return a;
}
#define CHECK(x) do { if(FAILED(x)) { printf("Failed line %d\n",__LINE__); return 3; } } while(0)
int main() {
    HWND w=CreateWindowA("STATIC","relightcheck",WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
    auto lib=LoadLibraryA("out\\d3d8.dll");
    if(!lib) return 2;
    auto fn=(ID8Direct3D*(WINAPI*)(UINT))GetProcAddress(lib,"Direct3DCreate8");
    auto d=fn(120);
    D3D8PresentParameters pp={}; pp.BackBufferWidth=pp.BackBufferHeight=64; pp.BackBufferFormat=22;
    pp.BackBufferCount=1; pp.SwapEffect=1; pp.hDeviceWindow=w; pp.Windowed=1;
    pp.EnableAutoDepthStencil=1; pp.AutoDepthStencilFormat=71;
    ID8Device* dev=nullptr; CHECK(d->CreateDevice(0,1,w,0x40,&pp,&dev));
    auto makeVS=[&](const char* dp,const char* fp) {
        auto dd=read(dp),ff=read(fp); DWORD h=0;
        if(FAILED(dev->CreateVertexShader(dd.data(),ff.data(),&h,0))) exit(3); return h;
    };
    auto makePS=[&](const char* p) { auto f=read(p); DWORD h=0; if(FAILED(dev->CreatePixelShader(f.data(),&h))) exit(3); return h; };
    DWORD terrain=makeVS("profile\\vs_005_h0000000d_decl.bin","profile\\vs_005_h0000000d_func.bin");
    DWORD skin=makeVS("profile\\vs_001_h00000005_decl.bin","profile\\vs_001_h00000005_func.bin");
    DWORD terrainPS=makePS("profile\\ps_009_h0000000a.bin"),skinPS=makePS("profile\\ps_001_h00000002.bin");
    auto buffer=[&](const void* bytes,UINT size) {
        ID8VertexBuffer* b=nullptr; BYTE* p=nullptr;
        if(FAILED(dev->CreateVertexBuffer(size,0,0,1,&b)) || FAILED(b->Lock(0,0,&p,0))) exit(3);
        memcpy(p,bytes,size); b->Unlock(); return b;
    };
    DWORD heights[4]={}; float grid[]={-30,30,30,30,30,-30,-30,-30};
    auto hv=buffer(heights,sizeof(heights)),gv=buffer(grid,sizeof(grid));
    struct V { float x,y,z,nx,ny,nz; DWORD weight,index; float u,v; };
    V vertices[]={{-8,4,8,0,1,0,0xffff0000,0,0,0},{8,4,8,0,1,0,0xffff0000,0,1,0},
                  {8,4,-8,0,1,0,0xffff0000,0,1,1},{-8,4,-8,0,1,0,0xffff0000,0,0,1}};
    auto vb=buffer(vertices,sizeof(vertices));
    ID8Texture* tex=nullptr; CHECK(dev->CreateTexture(4,4,1,0,21,1,&tex));
    D3D8LockedRect lock={}; CHECK(tex->LockRect(0,&lock,nullptr,0));
    for(int y=0;y<4;y++) for(int x=0;x<4;x++) ((DWORD*)((BYTE*)lock.pBits+y*lock.Pitch))[x]=0xff808080;
    tex->UnlockRect(0); dev->SetTexture(0,tex); dev->SetTexture(1,tex);
    dev->SetRenderState(RS_CULLMODE,1); dev->SetRenderState(RS_ZENABLE,1);
    dev->SetRenderState(RS_ZWRITEENABLE,1); dev->SetRenderState(RS_ALPHABLENDENABLE,0);
    DWORD samples[4]={};
    for(int frame=0;frame<4;frame++) {
        float c[96][4]={};
        XMVECTOR eye=frame==0 ? XMVectorSet(0,50,-30,1) : XMVectorSet(35,50,0,1);
        XMMATRIX vp=XMMatrixLookAtLH(eye,XMVectorSet(0,4,0,1),XMVectorSet(0,1,0,0))*XMMatrixOrthographicLH(40,40,1,200);
        XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(c),XMMatrixTranspose(vp));
        dev->Clear(0,nullptr,3,0,1,0);
        dev->SetVertexShader(terrain); dev->SetPixelShader(terrainPS);
        dev->SetVertexShaderConstant(0,c,96); dev->SetStreamSource(0,hv,4); dev->SetStreamSource(1,gv,8);
        CHECK(dev->DrawPrimitive(6,0,2));
        c[5][0]=c[6][1]=c[7][2]=1; // one identity bone
        c[88][0]=c[88][1]=c[88][2]=frame==0 ? .1f : .9f; c[88][3]=1;
        c[90][0]=c[90][1]=c[90][2]=1; c[92][0]=frame==3 ? -10000.f : 10000.f;
        c[92][1]=frame==3 ? 1.f : 0.f; c[92][2]=1;
        c[93][0]=.2f; c[93][1]=.4f; c[93][2]=.6f;
        for(auto& v:vertices) v.ny=frame==2 ? -1.f : 1.f;
        BYTE* data=nullptr; CHECK(vb->Lock(0,0,&data,0)); memcpy(data,vertices,sizeof(vertices)); vb->Unlock();
        dev->SetVertexShader(skin); dev->SetPixelShader(skinPS);
        dev->SetVertexShaderConstant(0,c,96); dev->SetStreamSource(0,vb,sizeof(V));
        CHECK(dev->DrawPrimitive(6,0,2));
        ID8Surface* bb=nullptr; CHECK(dev->GetBackBuffer(0,0,&bb)); CHECK(bb->LockRect(&lock,nullptr,D8LOCK_READONLY));
        samples[frame]=((DWORD*)((BYTE*)lock.pBits+32*lock.Pitch))[32]&0xffffff;
        printf("frame %d RGB=%06lx\n",frame,samples[frame]);
        bb->UnlockRect(); bb->Release(); dev->Present(nullptr,nullptr,nullptr,nullptr);
    }
    auto channel=[](DWORD p,int shift){return (int)((p>>shift)&255);};
    bool ok=true;
    for(int s=0;s<=16;s+=8) ok &= abs(channel(samples[0],s)-channel(samples[1],s))<=2;
    ok &= channel(samples[0],16)>80 && channel(samples[0],16)-channel(samples[2],16)>20;
    ok &= abs(channel(samples[3],16)-51)<=2 && abs(channel(samples[3],8)-102)<=2 && abs(channel(samples[3],0)-153)<=2;
    printf("Replacement light / camera rotation / original-light removal / skinned normal / fog: %s\n",ok?"PASS":"FAIL");
    return ok?0:5;
}
