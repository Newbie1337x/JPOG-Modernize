// native_terrain.h - read-only access to the game's terrain height field through exported
// SimJP/Toshi accessors. Shared by the water region map and the lighting height field.
// Only exported, read-only native accessors. No absolute addresses, hooks, engine patching,
// writes to terrain, DLL loading or game launching. POD-only SEH helpers.
#pragma once
#include "common.h"
#include <cmath>

namespace native_terrain {
using TerrainFn=void*(__stdcall*)();
using FieldFn=void*(__thiscall*)(void*);
using SizeFn=unsigned(__thiscall*)(void*);
using HeightFn=float(__thiscall*)(void*,unsigned,unsigned);
struct Bridge {
    TerrainFn terrain=nullptr;FieldFn field=nullptr;SizeFn width=nullptr,height=nullptr;HeightFn sample=nullptr;
    bool Resolve(){
        HMODULE exe=GetModuleHandleA("SimJP.exe"),base=GetModuleHandleA("TTerrainInterface.dll");
        if(!exe || exe!=GetModuleHandleA(nullptr) || !base || !GetModuleHandleA("SimJPTerrain.dll"))return false;
        terrain=reinterpret_cast<TerrainFn>(GetProcAddress(exe,"?GetTerrain@AParkManager@@SGPAVASimJPTerrain@@XZ"));
        field=reinterpret_cast<FieldFn>(GetProcAddress(base,"?GetHeightField@TTerrainInterface@Toshi@@QBEPAVTHeightField@2@XZ"));
        width=reinterpret_cast<SizeFn>(GetProcAddress(base,"?GetSizeX@TTerrainInterface@Toshi@@QBEIXZ"));
        height=reinterpret_cast<SizeFn>(GetProcAddress(base,"?GetSizeZ@TTerrainInterface@Toshi@@QBEIXZ"));
        sample=reinterpret_cast<HeightFn>(GetProcAddress(base,"?GetHeight@THeightField@Toshi@@QBEMII@Z"));
        return terrain && field && width && height && sample;
    }
};
// One sample per world unit: sample (x, z) is at world (x, z); height is positive up (= -renderer Y).
struct Info {void* terrain;void* field;unsigned w,h;};
static bool InfoSafe(const Bridge& b,Info& i){
    __try{
        i.terrain=b.terrain();if(!i.terrain)return false;
        i.field=b.field(i.terrain);if(!i.field)return false;
        i.w=b.width(i.terrain);i.h=b.height(i.terrain);
        return i.w>=2 && i.h>=2 && i.w<=4096 && i.h<=4096 && UINT64(i.w)*i.h<=1048576;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
static bool HeightsSafe(const Bridge& b,const Info& i,float* out,unsigned first,unsigned count){
    __try{
        for(unsigned n=first;n<first+count;n++){
            float h=b.sample(i.field,n%i.w,n/i.w);
            if(!std::isfinite(h) || std::fabs(h)>100000)return false;
            out[n]=h;
        }
        return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
}
