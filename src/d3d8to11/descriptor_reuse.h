#pragma once
#include <cstring>

// Cache object selection, not GPU bindings. Helper passes may freely rebind state.
template<class Word, class State>
struct DescriptorReuse {
    Word key[10]={};
    State state={};
    bool valid=false;
    bool Matches(const Word (&next)[10]) const {
        return valid && std::memcmp(key,next,sizeof(key))==0;
    }
    void Remember(const Word (&next)[10], State selected) {
        std::memcpy(key,next,sizeof(key));state=selected;valid=true;
    }
};
