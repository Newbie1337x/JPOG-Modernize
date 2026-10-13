#pragma once
#include <vector>
#include <cstdint>
#include <cmath>

// Native terrain heights are Y-up; renderer water positions are Y-down.
// Flood only genuinely submerged samples. Viewport and depth snapshots play no role.
struct WaterRegionMap {
    unsigned width=0,height=0,oceanCells=0,pondCells=0;
    std::vector<uint8_t> ocean;
    std::vector<float> shoreDistance;
    bool Build(unsigned w,unsigned h,const std::vector<float>& terrain,float waterHeight){
        if(!w || !h || size_t(w)*h!=terrain.size() || terrain.size()>1048576 || !std::isfinite(waterHeight))return false;
        std::vector<uint8_t> wet(terrain.size(),0),visited(terrain.size(),0);
        std::vector<unsigned> queue;queue.reserve(terrain.size());
        for(size_t i=0;i<terrain.size();i++){
            if(!std::isfinite(terrain[i]))return false;
            wet[i]=terrain[i]<waterHeight-.005f;
        }
        auto seed=[&](unsigned i){if(wet[i] && !visited[i]){visited[i]=1;queue.push_back(i);}};
        for(unsigned x=0;x<w;x++){seed(x);seed((h-1)*w+x);}
        for(unsigned z=0;z<h;z++){seed(z*w);seed(z*w+w-1);}
        for(size_t head=0;head<queue.size();head++){
            unsigned i=queue[head],x=i%w,z=i/w;
            if(x)seed(i-1);if(x+1<w)seed(i+1);if(z)seed(i-w);if(z+1<h)seed(i+w);
        }
        ocean.assign(terrain.size(),0);oceanCells=pondCells=0;
        for(size_t i=0;i<wet.size();i++)if(wet[i]){
            if(visited[i]){ocean[i]=255;++oceanCells;}else ++pondCells;
        }
        // Extend the sea label one dry sample into its banks, without changing
        // connectivity or assigning ocean to any enclosed wet sample.
        for(unsigned i:queue){unsigned x=i%w,z=i/w;
            if(x && !wet[i-1])ocean[i-1]=255;if(x+1<w && !wet[i+1])ocean[i+1]=255;
            if(z && !wet[i-w])ocean[i-w]=255;if(z+1<h && !wet[i+w])ocean[i+w]=255;
        }
        // Two-pass 8-neighbour chamfer distance to dry land (world-unit grid).
        // A scalar phase field makes wave fronts follow the coast continuously;
        // dot(position, a varying local direction) would create phase seams.
        shoreDistance.resize(terrain.size());
        for(size_t i=0;i<wet.size();i++)shoreDistance[i]=wet[i]?8192.f:0.f;
        auto relax=[&](unsigned i,unsigned j,float cost){
            float candidate=shoreDistance[j]+cost;
            if(candidate<shoreDistance[i])shoreDistance[i]=candidate;
        };
        for(unsigned z=0;z<h;z++)for(unsigned x=0;x<w;x++){
            unsigned i=z*w+x;
            if(x)relax(i,i-1,1);if(z)relax(i,i-w,1);
            if(x && z)relax(i,i-w-1,1.41421356f);
            if(x+1<w && z)relax(i,i-w+1,1.41421356f);
        }
        for(unsigned z=h;z-->0;)for(unsigned x=w;x-->0;){
            unsigned i=z*w+x;
            if(x+1<w)relax(i,i+1,1);if(z+1<h)relax(i,i+w,1);
            if(x && z+1<h)relax(i,i+w-1,1.41421356f);
            if(x+1<w && z+1<h)relax(i,i+w+1,1.41421356f);
        }
        width=w;height=h;return true;
    }
};
