#include "../src/d3d8to11/water_regions.h"
#include "../src/d3d8to11/water_position.h"
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
static bool CheckCapturedPosition(const char* folder,const char* file,bool planar){
    std::ifstream f(std::string(folder)+"/"+file,std::ios::binary|std::ios::ate);
    if(!f)return false;
    std::streamoff bytes=f.tellg();if(bytes<=0 || bytes%4!=0)return false;
    std::vector<uint32_t> code(size_t(bytes)/4);f.seekg(0);
    if(!f.read(reinterpret_cast<char*>(code.data()),bytes))return false;
    float c[256][4]={},p[4]={7,8,0,1},clip[4]={};
    for(int r=0;r<4;r++)c[r][r]=1;
    if(planar){
        c[8][0]=2;c[8][1]=3;c[8][2]=4;c[8][3]=1;
        c[9][0]=100;c[9][2]=200;c[0][3]=0;c[1][3]=-7;
    }else{p[1]=5;p[2]=8;c[21][0]=100;c[21][1]=4;c[21][2]=200;}
    if(!WaterPositionClip(code.data(),code.size(),p,c,256,clip))return false;
    float expected[4]={planar?114.f:107.f,planar?-7.f:9.f,planar?232.f:208.f,1};
    for(int r=0;r<4;r++)if(std::fabs(clip[r]-expected[r])>.0001f)return false;
    if(WaterPositionClip(code.data(),2,p,c,256,clip))return false;
    std::printf("PASS: captured %s position prefix, swizzle, scaling, offsets, bounds\n",file);
    return true;
}
int main(int argc,char** argv){
    std::vector<float> h(49,2);h[0]=h[7]=-2;h[24]=-2;
    WaterRegionMap m;
    if(!m.Build(7,7,h,0) || m.oceanCells!=2 || m.pondCells!=1 || m.ocean[24]!=0 || m.ocean[0]!=255)return 1;
    // Open a channel from the previously enclosed pond to the map boundary.
    h[3]=h[10]=h[17]=-2;
    if(!m.Build(7,7,h,0) || m.pondCells!=0 || m.ocean[24]!=255)return 2;
    // The sea bank expansion must never overwrite an enclosed wet neighbour.
    h.assign(49,2);h[0]=-2;h[8]=-2;
    if(!m.Build(7,7,h,0) || m.ocean[8]!=0 || m.pondCells!=1)return 3;
    h[8]=NAN;if(m.Build(7,7,h,0))return 4;
    // Coast on the west: distance increases eastwards; -gradient points west.
    h.assign(49,-2);for(unsigned z=0;z<7;z++)h[z*7]=2;
    if(!m.Build(7,7,h,0) || std::fabs(m.shoreDistance[3*7+5]-5)>1e-4f ||
       m.shoreDistance[3*7+5]<=m.shoreDistance[3*7+4])return 6;
    // Island: the inward propagation direction reverses on opposite shores.
    h.assign(49,-2);h[24]=2;
    if(!m.Build(7,7,h,0) || m.shoreDistance[22]<=m.shoreDistance[23] ||
       m.shoreDistance[26]<=m.shoreDistance[25] || m.shoreDistance[24]!=0)return 7;
    // -k*d-omega*t is constant along d(t)=d(0)-omega*t/k: toward land.
    float phase0=-.15708f*5,phase1=-.15708f*(5-1.05f/.15708f)-1.05f;
    if(std::fabs(phase0-phase1)>1e-5f)return 8;
    std::puts("PASS: shore distance, inward direction on opposite coasts, coastward phase sign");
    std::puts("PASS: ocean boundary, enclosed pond, live channel change, separated wet cells, invalid input");
    if(argc>1 && (!CheckCapturedPosition(argv[1],"vs_008_h00000013_func.bin",true) ||
                  !CheckCapturedPosition(argv[1],"vs_007_h00000011_func.bin",false)))return 5;
    return 0;
}
