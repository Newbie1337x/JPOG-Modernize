#include "../src/d3d8to11/descriptor_reuse.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
int main() {
    DescriptorReuse<uint32_t,int> cache;
    uint32_t key[10]={1,1,1,2,3,2,0x80000000,0,16,0xffffffff};
    assert(!cache.Matches(key));
    cache.Remember(key,42);
    assert(cache.Matches(key) && cache.state==42);
    for(int i=0;i<10;++i){key[i]^=1;assert(!cache.Matches(key));key[i]^=1;}
    DescriptorReuse<uint32_t,int> other;
    other.Remember(key,7);assert(other.state==7 && cache.state==42);
    key[8]=1; // anisotropy / master switch changes effective descriptor
    assert(!cache.Matches(key));cache.Remember(key,9);
    assert(cache.Matches(key) && cache.state==9);
    puts("PASS: all descriptor fields, independent stages and configuration changes.");
}
