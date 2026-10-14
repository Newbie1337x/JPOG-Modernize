#pragma once
#include <cstdint>
#include <unordered_map>

// Renderer-thread diagnostics only. No string construction or log mutex on hits.
class DrawDiagnostics {
    std::unordered_map<uint64_t, unsigned> seen;
    uint64_t lastKey=0;
    unsigned lastBits=0;
    bool lastValid=false;
public:
    enum { Path=1, SunMaterial=2, NormalMap=4 };
    bool First(uint32_t vs, uint32_t ps, unsigned bit) {
        const uint64_t key=(uint64_t(vs)<<32)|ps;
        if(lastValid && lastKey==key && (lastBits&bit))return false;
        auto it=seen.find(key);
        if(it==seen.end()) {
            // Do not let diagnostic-only state grow indefinitely in long sessions.
            if(seen.size()>=4096)return false;
            it=seen.emplace(key,0).first;
        }
        const bool first=(it->second&bit)==0;
        it->second|=bit;
        lastKey=key;lastBits=it->second;lastValid=true;
        return first;
    }
};
