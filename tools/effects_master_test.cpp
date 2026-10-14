#include "../src/d3d8to11/common.h"
#include <cassert>
#include <cstdio>
int main() {
    Config c;
    c.msaa=8; c.fpsLimit=165; c.syncInterval=1; c.forceAnisotropy=16;
    c.effectsEnabled=false;
    assert(c.EffectiveMsaa()==0 && c.EffectiveFpsLimit()==0);
    assert(c.EffectiveVSync()==0 && c.EffectiveAnisotropy()==0);
    assert(c.msaa==8 && c.fpsLimit==165 && c.syncInterval==1 && c.forceAnisotropy==16);
    c.effectsEnabled=true;
    assert(c.EffectiveMsaa()==8 && c.EffectiveFpsLimit()==165);
    assert(c.EffectiveVSync()==1 && c.EffectiveAnisotropy()==16);
    puts("PASS: baseline overrides and saved preferences restored; no game launched.");
}
