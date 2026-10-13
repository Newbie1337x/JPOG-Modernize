// Hidden GPU test with captured JPOG shaders; never launches SimJP.
// Usage: watercheck <profile directory> <built d3d8.dll> <debug 0..3/-1 off/4 grass/5 seams> <MSAA>
#include "../src/d3d8to11/d3d8_types.h"
#include <vector>
#include <string>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#define CHECK(x) do{if(FAILED(x)){printf("Failed line %d\n",__LINE__);return 3;}}while(0)
int main(int argc,char** argv) {
    if(argc<5) return 2;
    int debug=atoi(argv[3]),msaa=atoi(argv[4]);
    bool temporalTest=debug>=17 && debug<=19,fgTest=debug==18 || debug==19;
    bool materialTest=debug>=20 && debug<=25;
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr,exe,MAX_PATH);
    char* sep=strrchr(exe,'\\'); if(!sep) return 2; sep[1]=0;
    std::string ini=std::string(exe)+"d3d8to11.ini";
    // These are test fixtures next to this executable, never the game's configuration.
    auto put=[&](const char* section,const char* key,const char* value){
        WritePrivateProfileStringA(section,key,value,ini.c_str());
    };
    put("Renderer","Backend","d3d11");put("Renderer","MSAA",argv[4]);
    put("Renderer","EffectsEnabled",debug==8?"0":"1");
    put("Renderer","ApplyGammaRamp","0");put("Renderer","FPSLimit","0");
    put("Antialiasing","TAA",debug==11?"1":"0");put("Antialiasing","FSR1",debug==11?"1":"0");
    put("Antialiasing","FSR31",temporalTest?"1":"0");
    put("Antialiasing","FrameGeneration",fgTest?"1":"0");
    put("Antialiasing","FrameGenerationDebug",debug==19?"1":"0");
    put("Renderer","VSync",fgTest?"1":"0");
    if(temporalTest){put("Antialiasing","TAA","1");put("Antialiasing","FSR1","1");put("Performance","LogCSV","1");}
    put("Lighting","Enabled","0");put("Lighting","ReplaceOriginal","0");
    if(debug==4)put("Lighting","ReplaceOriginal","1");
    put("Post","Enabled","0");put("Effects","AO","0");put("Effects","LightShafts","0");
    put("Grass","Enabled","0");put("NormalMaps","Enabled","0");
    put("Materials","Enabled",debug==21 || debug==23 || debug==25?"0":"1");
    if(materialTest){put("NormalMaps","Enabled","1");put("Materials","SkinRoughness","0.58");put("Materials","BuildingRoughness","0.78");}
    put("Water","Enabled",debug<0?"0":"1");put("Water","DebugView",debug<0 || debug==4?"0":argv[3]);
    put("Water","SurfaceMode","0");
    put("Water","SwellAmplitude",debug==14 || debug==15?"0.65":"0");
    put("Water","SwellLength","1");put("Water","SwellCoastDepth","3");put("Water","CrestFoam","0.35");
    if(debug==14 || debug==15){put("Water","DebugView","7");put("Water","SurfaceMode",debug==14?"1":"2");}
    if(debug==5)put("Water","DebugView","2");
    if(debug==6 || debug==7)put("Water","DebugView","4");
    if(debug==11)put("Water","DebugView","0");
    if(temporalTest)put("Water","DebugView","0");
    if(debug==12 || debug==13){put("Water","DebugView","3");put("Water","SurfaceMode",debug==12?"1":"2");put("Water","LagoonFoam","0.03");}
    if(debug==9 || debug==10){
        put("Water","DebugView","5");put("Water","SceneReflections",debug==9?"1":"0");
        put("Water","SceneReflectionStrength","1");put("Water","SceneReflectionDistance","80");
        put("Water","SceneReflectionSteps","24");put("Performance","LogCSV","1");
    }
    put("Water","Ripples","1");put("Water","Speed","0");
    put("Water","Interaction","1");put("Water","InteractionStrength","1");
    put("Water","DepthScale","6");put("Water","Distortion","0");put("Water","Foam",debug==3 || debug==12 || debug==13?"0.6":"0");
    put("Debug","D3D11DebugLayer","1");
    if(debug==8){
        put("Lighting","Enabled","1");put("Lighting","ReplaceOriginal","1");
        put("Post","Enabled","1");put("Effects","AO","1");put("Effects","LightShafts","1");
        put("Grass","Enabled","1");put("NormalMaps","Enabled","1");put("Water","DebugView","0");
    }
    HWND w=CreateWindowA("STATIC","watercheck",debug==11 || fgTest?WS_POPUP:WS_OVERLAPPEDWINDOW,0,0,fgTest?64:128,fgTest?64:128,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
    HMODULE lib=LoadLibraryA(argv[2]);if(!lib)return 2;
    auto fn=(ID8Direct3D*(WINAPI*)(UINT))GetProcAddress(lib,"Direct3DCreate8");if(!fn)return 2;
    auto d=fn(120);ID8Device* dev=nullptr;
    D3D8PresentParameters pp={};pp.BackBufferWidth=pp.BackBufferHeight=64;pp.BackBufferFormat=22;
    pp.BackBufferCount=1;pp.SwapEffect=1;pp.hDeviceWindow=w;pp.Windowed=1;
    pp.EnableAutoDepthStencil=1;pp.AutoDepthStencilFormat=71;CHECK(d->CreateDevice(0,1,w,0x40,&pp,&dev));
    auto read=[&](const char* name){
        std::ifstream f(std::string(argv[1])+"\\"+name,std::ios::binary|std::ios::ate);
        auto n=f.tellg();if(n<=0){printf("missing %s\n",name);exit(2);}
        std::vector<DWORD> a((size_t)n/4);f.seekg(0);f.read((char*)a.data(),n);return a;
    };
    auto makeVS=[&](const char* dd,const char* ff){
        auto decl=read(dd),func=read(ff);DWORD h=0;
        if(FAILED(dev->CreateVertexShader(decl.data(),func.data(),&h,0)))exit(3);return h;
    };
    auto makePS=[&](const char* ff){auto f=read(ff);DWORD h=0;if(FAILED(dev->CreatePixelShader(f.data(),&h)))exit(3);return h;};
    DWORD terrain=makeVS("vs_005_h0000000d_decl.bin","vs_005_h0000000d_func.bin");
    DWORD rigid=makeVS("vs_002_h00000007_decl.bin","vs_002_h00000007_func.bin");
    DWORD flat=makePS("ps_009_h0000000a.bin"),water=makePS("ps_008_h00000009.bin");
    DWORD materialBlend=debug>=24?makePS("ps_001_h00000002.bin"):0;
    DWORD grass=makeVS("vs_010_h00000017_decl.bin","vs_010_h00000017_func.bin"),grassPS=makePS("ps_014_h0000000f.bin");
    DWORD skin=makeVS("vs_001_h00000005_decl.bin","vs_001_h00000005_func.bin");
    auto buffer=[&](const void* bytes,UINT n){
        ID8VertexBuffer* b=nullptr;BYTE* p=nullptr;
        if(FAILED(dev->CreateVertexBuffer(n,0,0,1,&b)) || FAILED(b->Lock(0,0,&p,0)))exit(3);
        memcpy(p,bytes,n);b->Unlock();return b;
    };
    DWORD heights[4]={};float grid[]={-40,40,40,40,40,-40,-40,-40};
    auto hv=buffer(heights,sizeof(heights)),gv=buffer(grid,sizeof(grid));
    struct V{float x,y,z;DWORD color;float u,v;};
    V quad[]={{-30,0,30,~0u,0,0},{30,0,30,~0u,1,0},{30,0,-30,~0u,1,1},{-30,0,-30,~0u,0,1}};
    auto qv=buffer(quad,sizeof(quad));
    V wall[]={{-12,-12,15,~0u,0,0},{12,-12,15,~0u,1,0},{12,0,15,~0u,1,1},{-12,0,15,~0u,0,1}};
    auto wallVB=buffer(wall,sizeof(wall));
    // Same surface, split into batches with deliberately discontinuous UVs and
    // hostile legacy simulation fields. The world-space Fresnel must not change.
    V tiles[]={{-30,0,30,~0u,9,9},{0,0,30,~0u,15,9},{0,0,-30,~0u,15,15},{-30,0,-30,~0u,9,15},
               {0,0,30,~0u,-4,-4},{30,0,30,~0u,-9,-4},{30,0,-30,~0u,-9,-9},{0,0,-30,~0u,-4,-9}};
    auto tileVB=buffer(tiles,sizeof(tiles));
    struct S{float x,y,z,nx,ny,nz;DWORD weight,index;float u,v;};
    S creature[8]={};
    for(int k=0;k<8;k++){
        creature[k].x=.5f*std::cos(k*6.2831853f/8);
        creature[k].z=.5f*std::sin(k*6.2831853f/8);
        creature[k].y=debug==7?-5.f:.5f;creature[k].ny=-1;
        if(fgTest){creature[k].x*=6;creature[k].z*=6;creature[k].y=-2;}
        creature[k].weight=0x00ff0000;creature[k].index=(k%6)<<16;
    }
    auto creatureVB=buffer(creature,sizeof(creature));
    struct G{float x,y,z,u,v;};
    G blades[]={{-30,0,30,0,0},{30,0,30,1,0},{30,0,-30,1,1},{-30,0,-30,0,1}};
    float bladeHeights[4]={};
    auto grassVB=buffer(blades,sizeof(blades)),bladeVB=buffer(bladeHeights,sizeof(bladeHeights));
    auto texture=[&](DWORD rgba){
        ID8Texture* t=nullptr;D3D8LockedRect r={};
        if(FAILED(dev->CreateTexture(4,4,1,0,21,1,&t)) || FAILED(t->LockRect(0,&r,nullptr,0)))exit(3);
        for(int y=0;y<4;y++)for(int x=0;x<4;x++)((DWORD*)((BYTE*)r.pBits+y*r.Pitch))[x]=rgba;
        t->UnlockRect(0);return t;
    };
    auto ground=texture(0xff20a030),wave=texture(0xff8080ff),reflection=texture(0xffffffff),hud=texture(0xff00ff00);
    auto badWave0=texture(0xffff00ff),badWave1=texture(0xff00ffff);
    if(materialTest){
        auto setNormal=(BOOL(WINAPI*)(void*,const char*))GetProcAddress(lib,"D3D8to11_SetNormalMap");
        const char* path=debug<=21 || debug>=24?"C:\\Games\\Jurassic Park Operation Genesis\\mods\\textures\\dinos\\dinos_hellcreek_b__Tricera_n.dds":
            "C:\\Games\\Jurassic Park Operation Genesis\\mods\\textures\\buildings\\Structures_Hi__Hamburger_n.dds";
        if(!setNormal || !setNormal(reflection,path)){printf("Material fixture normal attachment failed\n");return 5;}
    }
    float camera[]={1.f/32,0,0,0, 0,0,1.f/32,0, 0,.01f,0,.5f, 0,0,0,1};
    if(debug==5 || debug==9 || debug==10) {
        camera[5]=-.95f/32;camera[6]=.30f/32;
        camera[9]=.30f*.001f;camera[10]=.95f*.001f;
    }
    float white[]={1,1,1,1},zero[4]={};
    dev->SetRenderState(RS_CULLMODE,1);
    DWORD samples[10]={};float depths[]={.2f,2.f,8.f,.2f};
    DWORD baseline[48]={};int seamErrors=0;
    int maxInteraction[10]={};
    bool generatedDiffers=false;
    int frames=(debug==6 || debug==7)?10:4;
    for(int frame=0;frame<frames;frame++){
        if(temporalTest){camera[3]=frame*.01f;}
        dev->Clear(0,nullptr,3,0,1,0);dev->SetRenderState(RS_ZENABLE,1);dev->SetRenderState(RS_ZWRITEENABLE,1);
        dev->SetRenderState(RS_ALPHABLENDENABLE,0);
        dev->SetVertexShader(terrain);dev->SetPixelShader(flat);dev->SetTexture(0,ground);
        dev->SetVertexShaderConstant(0,camera,4);float offset[]={0,frames==10 || fgTest?4.f:depths[frame],0,0};
        dev->SetVertexShaderConstant(4,offset,1);dev->SetVertexShaderConstant(5,zero,1);
        dev->SetStreamSource(0,hv,4);dev->SetStreamSource(1,gv,8);CHECK(dev->DrawPrimitive(6,0,2));
        if(debug==9 || debug==10){
            dev->SetVertexShader(rigid);dev->SetPixelShader(flat);dev->SetVertexShaderConstant(4,white,1);
            dev->SetStreamSource(0,wallVB,sizeof(V));CHECK(dev->DrawPrimitive(6,0,2));
        }
        if(frames==10 || temporalTest){
            float bones[18][4]={};
            for(int k=0;k<6;k++){bones[k*3][0]=1;bones[k*3+1][1]=1;bones[k*3+2][2]=1;}
            float paletteIndex[]={765,765,765,765},actorCamera[16];memcpy(actorCamera,camera,sizeof(camera));
            actorCamera[3]=frame*(fgTest?3.f:.12f)/32;
            dev->SetVertexShader(skin);dev->SetStreamSource(0,creatureVB,sizeof(S));
            dev->SetVertexShaderConstant(0,actorCamera,4);dev->SetVertexShaderConstant(4,paletteIndex,1);
            dev->SetVertexShaderConstant(5,bones,18);CHECK(dev->DrawPrimitive(6,0,6));
            dev->SetVertexShaderConstant(0,camera,4);
        }
        dev->SetVertexShader(rigid);dev->SetPixelShader(water);dev->SetVertexShaderConstant(4,white,1);
        dev->SetStreamSource(0,qv,sizeof(V));dev->SetTexture(0,wave);dev->SetTexture(2,reflection);
        dev->SetRenderState(RS_ZWRITEENABLE,0);dev->SetRenderState(RS_ALPHABLENDENABLE,1);
        dev->SetRenderState(RS_SRCBLEND,5);dev->SetRenderState(RS_DESTBLEND,6);
        if(debug==4) {
            dev->SetVertexShader(grass);dev->SetPixelShader(grassPS);
            dev->SetVertexShaderConstant(4,zero,1);dev->SetVertexShaderConstant(5,white,1);
            dev->SetVertexShaderConstant(6,white,1);dev->SetVertexShaderConstant(7,zero,1);
            dev->SetVertexShaderConstant(8,zero,1);dev->SetVertexShaderConstant(9,zero,1);dev->SetVertexShaderConstant(10,zero,1);
            dev->SetTexture(0,reflection);dev->SetTexture(1,ground);
            dev->SetStreamSource(0,grassVB,sizeof(G));dev->SetStreamSource(1,bladeVB,4);
            dev->SetRenderState(RS_ALPHABLENDENABLE,0);dev->SetRenderState(RS_ALPHATESTENABLE,1);
            dev->SetRenderState(RS_ALPHAFUNC,7);dev->SetRenderState(RS_ALPHAREF,1);
        }
        if(materialTest){
            if(debug>=24){dev->SetPixelShader(materialBlend);dev->SetTexture(0,ground);dev->SetTexture(1,reflection);float c[]={0,0,0,0};dev->SetPixelShaderConstant(1,c,1);}
            else{dev->SetPixelShader(flat);dev->SetTexture(0,reflection);}
            dev->SetRenderState(RS_ALPHABLENDENABLE,0);
        }
        if(debug==5 && frame>0) {
            dev->SetStreamSource(0,tileVB,sizeof(V));dev->SetTexture(0,badWave0);
            CHECK(dev->DrawPrimitive(6,0,2));
            dev->SetTexture(0,badWave1);CHECK(dev->DrawPrimitive(6,4,2));
        } else CHECK(dev->DrawPrimitive(6,0,2));
        auto sample=[&](){
            ID8Surface* b=nullptr;D3D8LockedRect r={};
            if(FAILED(dev->GetBackBuffer(0,0,&b)) || FAILED(b->LockRect(&r,nullptr,D8LOCK_READONLY)))exit(3);
            DWORD c=((DWORD*)((BYTE*)r.pBits+32*r.Pitch))[32]&0xffffff;b->UnlockRect();b->Release();return c;
        };
        samples[frame]=sample();printf("MSAA %d debug %d frame %d RGB %06lx\n",msaa,debug,frame,samples[frame]);
        if(frames==10) {
            ID8Surface* b=nullptr;D3D8LockedRect r={};
            CHECK(dev->GetBackBuffer(0,0,&b));CHECK(b->LockRect(&r,nullptr,D8LOCK_READONLY));
            for(int y=6;y<58;y++)for(int x=6;x<58;x++){
                if(x>=28 && x<=36 && y>=30 && y<=34)continue; // exclude foreground dry animal
                int v=((DWORD*)((BYTE*)r.pBits+y*r.Pitch))[x]&255;
                if(v>maxInteraction[frame])maxInteraction[frame]=v;
            }
            b->UnlockRect();b->Release();
            printf("interaction outside body: %d\n",maxInteraction[frame]);
        }
        if(debug==5) {
            ID8Surface* b=nullptr;D3D8LockedRect r={};
            CHECK(dev->GetBackBuffer(0,0,&b));CHECK(b->LockRect(&r,nullptr,D8LOCK_READONLY));
            for(int x=8;x<56;x++){
                DWORD p=((DWORD*)((BYTE*)r.pBits+32*r.Pitch))[x]&0xffffff;
                if(frame==0)baseline[x-8]=p;
                else for(int shift=0;shift<=16;shift+=8)
                    if(abs((int)((p>>shift)&255)-(int)((baseline[x-8]>>shift)&255))>1)seamErrors++;
            }
            b->UnlockRect();b->Release();
        }
        // The actual game ends the main scene with a depth clear before HUD.
        // Previously this dropped m_sunStarted before interaction capture.
        if(frames==10)CHECK(dev->Clear(0,nullptr,D8CLEAR_ZBUFFER,0,1,0));
        if(temporalTest){
            CHECK(dev->Clear(0,nullptr,D8CLEAR_ZBUFFER,0,1,0));
            DWORD result=sample();printf("FSR31 post-resolve frame %d RGB %06lx\n",frame,result);
            if(frame>0 && result!=samples[frame])generatedDiffers=true;
            if(!result || (!fgTest && abs(int((result>>8)&255)-int((samples[frame]>>8)&255))>40)){printf("FSR31 OUTPUT FAILED\n");return 5;}
        }
        dev->SetRenderState(RS_ZENABLE,0);dev->SetRenderState(RS_ALPHABLENDENABLE,0);
        dev->SetVertexShader(rigid);dev->SetVertexShaderConstant(4,white,1);dev->SetStreamSource(0,qv,sizeof(V));
        dev->SetRenderState(RS_ALPHATESTENABLE,0);
        dev->SetPixelShader(flat);dev->SetTexture(0,hud);CHECK(dev->DrawPrimitive(6,0,2));
        if(sample()!=0x00ff00){printf("HUD FAILED\n");return 5;}
        dev->Present(nullptr,nullptr,nullptr,nullptr);
        if(frames==10)Sleep(100);
        if(debug==9)Sleep(350);
        if(debug==11)Sleep(350);
        if(temporalTest)Sleep(350);
    }
    auto green=[](DWORD c){return(int)((c>>8)&255);};
    bool ok=true;
    if(debug==1)ok=abs(green(samples[0])-9)<=3 && abs(green(samples[1])-85)<=3 && green(samples[2])>=250;
    if(debug==0)ok=green(samples[0])>green(samples[2])+30;
    if(debug==3)ok=green(samples[0])>15 && green(samples[1])==0 && green(samples[2])==0;
    if(debug<0)ok=samples[0]==samples[1] && samples[1]==samples[2];
    if(debug==8)ok=samples[0]==0xcccccc && samples[1]==0xcccccc && samples[2]==0xcccccc;
    if(debug==9)ok=green(samples[0])>10;
    if(debug==10)ok=green(samples[0])==0;
    if(debug==4)ok=green(samples[0])>2*(int)((samples[0]>>16)&255)+30;
    if(debug==5){ok=seamErrors==0;printf("Split UV/simulation batch seam errors: %d\n",seamErrors);}
    if(debug==6){ok=maxInteraction[0]==0 && maxInteraction[9]>5;}
    if(debug==7){for(int k=0;k<10;k++)ok &= maxInteraction[k]==0;}
    if(debug!=3 && debug!=5 && debug!=11 && frames!=10)ok &= samples[0]==samples[3]; // snapshot must refresh correctly across frames
    if(debug==11)ok=samples[0]!=0 && samples[3]!=0; // HUD was checked separately, history must survive scene transitions
    if(debug==12)ok=green(samples[0])<=5 && green(samples[1])==0;
    if(debug==13)ok=green(samples[0])>15 && green(samples[1])==0;
    if(debug==14)ok=green(samples[0])==0 && green(samples[1])==0;
    if(debug==15)ok=green(samples[0])>5;
    if(debug==17)ok=samples[0]!=0 && samples[3]!=0;
    if(debug==18)ok=samples[0]!=0 && samples[3]!=0;
    if(debug==19)ok=samples[0]!=0 && samples[3]!=0 && generatedDiffers;
    if(materialTest)ok=samples[0]!=0 && samples[3]!=0;
    printf("Depth / shading / HUD / frame reset: %s\n",ok?"PASS":"FAIL");
    return ok?0:5;
}
