#include "../src/d3d8to11/d3d8_types.h"
#include <vector>
#include <fstream>
#include <stdio.h>
std::vector<DWORD> read(const char* p){std::ifstream f(p,std::ios::binary|std::ios::ate);auto n=f.tellg();if(n<=0)exit(2);std::vector<DWORD>a((size_t)n/4);f.seekg(0);f.read((char*)a.data(),n);return a;}
int main(){
 HWND w=CreateWindowA("STATIC","suncheck",WS_OVERLAPPEDWINDOW,0,0,128,128,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
 auto lib=LoadLibraryA("out\\d3d8.dll");auto fn=(ID8Direct3D*(WINAPI*)(UINT))GetProcAddress(lib,"Direct3DCreate8");auto d=fn(120);
 D3D8PresentParameters pp={};pp.BackBufferWidth=pp.BackBufferHeight=64;pp.BackBufferFormat=22;pp.BackBufferCount=1;pp.SwapEffect=1;pp.hDeviceWindow=w;pp.Windowed=1;pp.EnableAutoDepthStencil=1;pp.AutoDepthStencilFormat=71;
 ID8Device* dev=nullptr;if(FAILED(d->CreateDevice(0,1,w,0x40,&pp,&dev)))return 3;
 auto decl=read("profile\\vs_002_h00000007_decl.bin"),func=read("profile\\vs_002_h00000007_func.bin"),ps=read("profile\\ps_009_h0000000a.bin");DWORD vs=0,pixel=0;dev->CreateVertexShader(decl.data(),func.data(),&vs,0);dev->CreatePixelShader(ps.data(),&pixel);dev->SetVertexShader(vs);dev->SetPixelShader(pixel);
 float constants[20]={1.f/32,0,0,0, 0,0,1.f/32,0, 0,-.01f,0,.5f, 0,0,0,1, 1,1,1,1};dev->SetVertexShaderConstant(0,constants,5);
 struct V{float x,y,z;DWORD color;float u,v;};
 V verts[8]={{-30,0,30,~0u,0,0},{30,0,30,~0u,1,0},{30,0,-30,~0u,1,1},{-30,0,-30,~0u,0,1},{-4,12,4,~0u,0,0},{4,12,4,~0u,1,0},{4,12,-4,~0u,1,1},{-4,12,-4,~0u,0,1}};
 ID8VertexBuffer* vb=nullptr;dev->CreateVertexBuffer(sizeof(verts),0,0,1,&vb);BYTE* data=nullptr;vb->Lock(0,0,&data,0);memcpy(data,verts,sizeof(verts));vb->Unlock();dev->SetStreamSource(0,vb,sizeof(V));
 ID8Texture* tex=nullptr;dev->CreateTexture(4,4,1,0,21,1,&tex);D3D8LockedRect lr={};tex->LockRect(0,&lr,nullptr,0);for(int y=0;y<4;y++)for(int x=0;x<4;x++)((DWORD*)((BYTE*)lr.pBits+y*lr.Pitch))[x]=0xff808080;tex->UnlockRect(0);dev->SetTexture(0,tex);dev->SetRenderState(RS_CULLMODE,1);
 for(int frame=0;frame<2;frame++){
  auto td=read("profile\\vs_005_h0000000d_decl.bin"),tf=read("profile\\vs_005_h0000000d_func.bin");
  DWORD terrain=0;dev->CreateVertexShader(td.data(),tf.data(),&terrain,0);
  DWORD heights[4]={}; float grid[8]={-30,30,30,30,30,-30,-30,-30};
  ID8VertexBuffer *heightVB=nullptr,*gridVB=nullptr;
  dev->CreateVertexBuffer(sizeof(heights),0,0,1,&heightVB);heightVB->Lock(0,0,&data,0);memcpy(data,heights,sizeof(heights));heightVB->Unlock();
  dev->CreateVertexBuffer(sizeof(grid),0,0,1,&gridVB);gridVB->Lock(0,0,&data,0);memcpy(data,grid,sizeof(grid));gridVB->Unlock();
  dev->Clear(0,nullptr,3,0xff000000,1,0);
  float zero[4]={};constants[3]=0;dev->SetVertexShaderConstant(0,constants,4);
  dev->SetVertexShader(terrain);dev->SetVertexShaderConstant(4,zero,1);dev->SetVertexShaderConstant(5,zero,1);
  dev->SetRenderState(RS_ZWRITEENABLE,0);dev->SetRenderState(RS_ALPHABLENDENABLE,1);
  dev->SetRenderState(RS_SRCBLEND,2);dev->SetRenderState(RS_DESTBLEND,1);
  dev->SetStreamSource(0,heightVB,4);dev->SetStreamSource(1,gridVB,8);dev->DrawPrimitive(6,0,2);
  // The model has its own translation in c0: local vertices are not world positions.
  constants[3]=.25f;dev->SetVertexShader(vs);dev->SetVertexShaderConstant(0,constants,5);
  dev->SetRenderState(RS_ZWRITEENABLE,1);dev->SetRenderState(RS_ALPHABLENDENABLE,0);
  dev->SetStreamSource(0,vb,sizeof(V));dev->DrawPrimitive(6,4,2);
  ID8Surface* bb=nullptr;dev->GetBackBuffer(0,0,&bb);if(FAILED(bb->LockRect(&lr,nullptr,16)))return 4;
  auto pixelAt=[&](int x,int y){return ((DWORD*)((BYTE*)lr.pBits+y*lr.Pitch))[x]&255;};
  int lit=pixelAt(12,12),shadow=pixelAt(45,25);printf("frame %d lit=%d projectedShadow=%d\n",frame,lit,shadow);
  bool fail=(lit!=128 || (frame==0 && shadow!=128) || (frame==1 && !(shadow<lit-15)));bb->UnlockRect();bb->Release();dev->Present(nullptr,nullptr,nullptr,nullptr);if(fail)return 5;
 }
 return 0;
}
