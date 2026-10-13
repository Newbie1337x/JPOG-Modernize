// Renderer-independent footprint tracking. Approximate body contacts, not named feet.
#pragma once
#include <array>
#include <cmath>
#include <algorithm>

struct WaterContact { float x=0,feetY=0,z=0,radius=1; };
struct WaterRipple {
    WaterContact contact;
    float birth=0,amplitude=0,radius=0,speed=0,lifetime=3.2f;
    bool active=false;
};
class WaterInteractionState {
public:
    static constexpr unsigned MaxTracks=32,MaxRipples=64;
    struct Track {
        WaterContact contact;
        float lastSeen=0,lastEmit=0,vx=0,vz=0,emitX=0,emitZ=0;
        bool active=false;
    };
    void Clear(){tracks={};ripples={};nextRipple=0;}
    void Update(float now,const WaterContact* contacts,unsigned count) {
        if(!std::isfinite(now))return;
        bool used[MaxTracks]={};
        for(auto& t:tracks)if(t.active && (now-t.lastSeen>.8f || now<t.lastSeen))t.active=false;
        for(unsigned n=0;n<count && n<MaxTracks;n++){
            const auto& c=contacts[n];
            if(!std::isfinite(c.x+c.feetY+c.z+c.radius) || c.radius<=0)continue;
            int best=-1;float bestDist=1e30f;
            for(unsigned k=0;k<MaxTracks;k++){
                const auto& t=tracks[k];if(!t.active || used[k])continue;
                float dt=(std::max)(0.f,now-t.lastSeen);
                float px=t.contact.x+t.vx*dt,pz=t.contact.z+t.vz*dt;
                float dx=c.x-px,dz=c.z-pz,d2=dx*dx+dz*dz;
                float range=(std::max)(1.5f,(std::min)(12.f,(c.radius+t.contact.radius)*.7f+dt*8));
                if(d2<range*range && d2<bestDist && std::abs(c.feetY-t.contact.feetY)<6){
                    best=(int)k;bestDist=d2;
                }
            }
            if(best<0){
                for(unsigned k=0;k<MaxTracks;k++)if(!tracks[k].active){best=(int)k;break;}
                if(best<0)continue;
                auto& t=tracks[best];t={};t.contact=c;t.lastSeen=t.lastEmit=now;
                t.emitX=c.x;t.emitZ=c.z;t.active=true;used[best]=true;
                Emit(c,now,0);continue;
            }
            auto& t=tracks[best];used[best]=true;float dt=now-t.lastSeen;
            if(dt>1e-4f){
                float vx=(c.x-t.contact.x)/dt,vz=(c.z-t.contact.z)/dt;
                float speed=std::sqrt(vx*vx+vz*vz);
                if(speed>20){vx*=20/speed;vz*=20/speed;}
                float blend=1-std::exp(-dt*6);
                t.vx+=(vx-t.vx)*blend;t.vz+=(vz-t.vz)*blend;
                speed=std::sqrt(t.vx*t.vx+t.vz*t.vz);
                float dx=c.x-t.emitX,dz=c.z-t.emitZ;
                float stride=(std::max)(.15f,c.radius*.08f);
                if(speed>.12f && now-t.lastEmit>=.16f && dx*dx+dz*dz>=stride*stride){
                    Emit(c,now,speed);t.lastEmit=now;t.emitX=c.x;t.emitZ=c.z;
                }
            }
            t.contact=c;t.lastSeen=now;
        }
        for(auto& r:ripples)if(r.active && (now-r.birth>r.lifetime || now<r.birth))r.active=false;
    }
    unsigned ActiveRipples(float now)const {
        unsigned n=0;for(const auto& r:ripples)if(r.active && now>=r.birth && now-r.birth<=r.lifetime)n++;return n;
    }
    const std::array<WaterRipple,MaxRipples>& Ripples()const{return ripples;}
private:
    void Emit(const WaterContact& c,float now,float velocity){
        auto& r=ripples[nextRipple++%MaxRipples];r={};r.contact=c;r.birth=now;r.active=true;
        r.amplitude=(.009f+.008f*(std::min)(velocity,4.f))*std::sqrt((std::max)(.5f,c.radius*.5f));
        r.radius=.2f+(std::min)(c.radius,5.f)*.12f;r.speed=1.7f+.2f*(std::min)(velocity,5.f);
    }
    std::array<Track,MaxTracks> tracks={};
    std::array<WaterRipple,MaxRipples> ripples={};
    unsigned nextRipple=0;
};
