#include "../src/d3d8to11/draw_diagnostics.h"
#include <cassert>
#include <cstdio>
int main() {
    DrawDiagnostics c;
    assert(c.First(1,2,DrawDiagnostics::Path));
    for(int i=0;i<10000;++i)assert(!c.First(1,2,DrawDiagnostics::Path));
    assert(c.First(1,2,DrawDiagnostics::SunMaterial));
    assert(!c.First(1,2,DrawDiagnostics::SunMaterial));
    assert(c.First(1,3,DrawDiagnostics::Path));
    assert(!c.First(1,2,DrawDiagnostics::Path)); // return to earlier pair
    assert(c.First(2,1,DrawDiagnostics::Path)); // VS/PS order matters
    assert(c.First(1,2,DrawDiagnostics::NormalMap));
    assert(c.First(0xffffffffu,0xffffffffu,DrawDiagnostics::Path));
    DrawDiagnostics bounded;
    for(unsigned i=0;i<4096;++i)assert(bounded.First(i,0,DrawDiagnostics::Path));
    assert(!bounded.First(4096,0,DrawDiagnostics::Path));
    assert(!bounded.First(4096,0,DrawDiagnostics::Path));
    assert(!bounded.First(0,0,DrawDiagnostics::Path));
    assert(bounded.First(0,0,DrawDiagnostics::SunMaterial));
    puts("PASS: repeated pairs, independent events, full-width keys and bounded storage.");
}
