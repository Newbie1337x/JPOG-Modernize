#include "../src/d3d8to11/d3d8_types.h"
#include <vector>
#include <fstream>
#include <stdio.h>
std::vector<DWORD> read(const char* p){std::ifstream f(p,std::ios::binary|std::ios::ate); auto n=f.tellg(); if(n<=0)exit(2); std::vector<DWORD> a((size_t)n/4); f.seekg(0); f.read((char*)a.data(),n); return a;}
int main(){
 HWND w=CreateWindowA("STATIC","hudcheck",WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
 auto lib=LoadLibraryA("out\\d3d8.dll"); auto fn=(ID8Direct3D*(WINAPI*)(UINT))GetProcAddress(lib,"Direct3DCreate8"); auto d=fn(120);
 D3D8PresentParameters pp={}; pp.BackBufferWidth=pp.BackBufferHeight=64; pp.BackBufferFormat=22; pp.BackBufferCount=1; pp.SwapEffect=1; pp.hDeviceWindow=w; pp.Windowed=1;
 ID8Device* dev=nullptr; HRESULT hr=d->CreateDevice(0,1,w,0x40,&pp,&dev); if(FAILED(hr))return 3;
 auto decl=read("profile\\vs_002_h00000007_decl.bin"); auto func=read("profile\\vs_002_h00000007_func.bin"); DWORD vs=0; dev->CreateVertexShader(decl.data(),func.data(),&vs,0); dev->SetVertexShader(vs);
 float constants[20]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1,1,1,1,1}; dev->SetVertexShaderConstant(0,constants,5);
 struct V{float x,y,z; DWORD color; float u,v;}; V verts[4]={{-.75f,.75f,.5f,0xff00ff00,0,0},{.75f,.75f,.5f,0xff00ff00,1,0},{.75f,-.75f,.5f,0xff00ff00,1,1},{-.75f,-.75f,.5f,0xff00ff00,0,1}};
 ID8VertexBuffer* vb=nullptr; dev->CreateVertexBuffer(sizeof(verts),0,0,1,&vb); BYTE* data=nullptr; vb->Lock(0,0,&data,0); memcpy(data,verts,sizeof(verts)); vb->Unlock(); dev->SetStreamSource(0,vb,sizeof(V));
 ID8Texture* tex=nullptr; dev->CreateTexture(4,4,1,0,21,1,&tex); D3D8LockedRect lr={}; tex->LockRect(0,&lr,nullptr,0); memset(lr.pBits,255,lr.Pitch*4); tex->UnlockRect(0); dev->SetTexture(0,tex);
 RECT partial={0,0,1,1}; tex->LockRect(0,&lr,&partial,0); *(DWORD*)lr.pBits=0xffff0000; tex->UnlockRect(0);
 dev->SetRenderState(RS_ZENABLE,0); dev->SetRenderState(RS_CULLMODE,1); dev->Clear(0,nullptr,1,0xff000000,1,0); dev->DrawPrimitive(6,0,2);
 ID8Surface* bb=nullptr; dev->GetBackBuffer(0,0,&bb); hr=bb->LockRect(&lr,nullptr,D8LOCK_READONLY); if(FAILED(hr))return 4;
 auto px=(DWORD*)((BYTE*)lr.pBits+32*lr.Pitch); printf("center=%08lx expected=ff00ff00\n",px[32]); int fail=(px[32]&0xffffff)!=0x00ff00; bb->UnlockRect(); dev->Present(nullptr,nullptr,nullptr,nullptr); return fail;
}
