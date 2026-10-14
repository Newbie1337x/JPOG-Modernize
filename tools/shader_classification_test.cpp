#include "../src/d3d8to11/device.h"
#include <cassert>
#include <cstdio>
int main() {
    const DWORD water[]={0xffff0101,66,0xb00f0000,71,0xb00f0001,0xb0e40000,
        72,0xb00f0002,0xb0e40000,0xffff};
    const DWORD ordinary[]={0xffff0101,66,0xb00f0000,0xffff};
    PixelShader8 a,b;
    a.func.assign(water,water+10);b.func.assign(ordinary,ordinary+4);
    a.water=IsWaterPixelShader(a.func.data());b.water=IsWaterPixelShader(b.func.data());
    assert(a.water && !b.water);
    for(int i=0;i<10000;++i)assert(a.water && !b.water); // runtime reads only
    VertexShader8 v;
    v.parsed.inputs.resize(5);v.parsed.inputs[0].type=2;v.parsed.inputs[1].type=2;
    v.worldKind=WorldShaderKind(v.parsed);assert(v.worldKind==1);
    v.parsed.inputs.resize(2);v.parsed.inputs[0].type=4;v.parsed.inputs[1].stream=1;
    v.worldKind=WorldShaderKind(v.parsed);assert(v.worldKind==2);
    VertexShader8 fresh;PixelShader8 freshPS;
    assert(fresh.worldKind==0 && !freshPS.water);
    puts("PASS: cached shader classifications and fresh-object defaults.");
}
