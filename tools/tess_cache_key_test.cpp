#include "../src/d3d8to11/device.h"
#include <cassert>
#include <cstdio>
struct TessCacheKeyTest {
    static void Run() {
        Device::TessCacheEntry e;
        e.identity=1;e.revision=3;e.base=-2;e.stride=32;e.offset=4;e.weld=true;e.list={0,1,2};
        assert(e.Matches(1,3,-2,32,4,true,{0,1,2}));
        assert(!e.Matches(2,3,-2,32,4,true,{0,1,2}));
        assert(!e.Matches(1,4,-2,32,4,true,{0,1,2}));
        assert(!e.Matches(1,3,0,32,4,true,{0,1,2}));
        assert(!e.Matches(1,3,-2,24,4,true,{0,1,2}));
        assert(!e.Matches(1,3,-2,32,0,true,{0,1,2}));
        assert(!e.Matches(1,3,-2,32,4,false,{0,1,2}));
        assert(!e.Matches(1,3,-2,32,4,true,{0,2,1}));
        assert(!e.Matches(1,3,-2,32,4,true,{0,1,2,0,2,3}));
    }
};
int main(){TessCacheKeyTest::Run();puts("PASS: exact cache key and invalidation dimensions; no game or device launched");}
