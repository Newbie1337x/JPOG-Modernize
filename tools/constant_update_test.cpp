#include "../src/d3d8to11/constant_update.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
int main() {
    uint32_t data[4]={0,0x7fc00001,2,3}, same[4]={0,0x7fc00001,2,3};
    assert(!CopyConstantsIfChanged(data,same,sizeof(data))); // same NaN payload
    assert(!CopyConstantsIfChanged(data,same,0));
    same[0]=0x80000000;
    assert(CopyConstantsIfChanged(data,same,4)); // negative zero is different
    assert(data[0]==same[0] && data[1]==0x7fc00001 && data[3]==3);
    same[1]=0x7fc00002;
    bool dirty=true;
    if(CopyConstantsIfChanged(data,same,sizeof(data)))dirty=true;
    assert(dirty && data[1]==same[1]);
    if(CopyConstantsIfChanged(data,same,sizeof(data)))dirty=true;
    assert(dirty); // duplicate never discards a pending upload
    puts("PASS: bit-exact duplicate detection, partial writes and pending dirty state.");
}
