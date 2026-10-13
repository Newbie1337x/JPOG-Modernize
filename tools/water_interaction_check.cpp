#include "../src/d3d8to11/water_interaction.h"
#include <cstdio>
int main(){
    WaterInteractionState state;WaterContact c={0,.5f,0,1};
    state.Update(0,&c,1);
    for(int i=1;i<=20;i++)state.Update(i*.1f,&c,1);
    if(state.ActiveRipples(2)!=1){puts("Stationary emitter generated extra pulses");return 1;}
    for(int i=21;i<=40;i++)state.Update(i*.1f,&c,1);
    if(state.ActiveRipples(4)!=0){puts("Initial pulse did not fade");return 1;}
    state.Clear();c={0,.5f,0,1};
    for(int i=0;i<=20;i++){c.x=i*.15f;state.Update(i*.1f,&c,1);}
    if(state.ActiveRipples(2)<5){puts("Moving emitter did not leave a trail");return 1;}
    unsigned old=state.ActiveRipples(2);
    c.x=100;state.Update(2.1f,&c,1);
    float maxNewAmplitude=0;
    for(const auto& p:state.Ripples())if(p.active && p.birth==2.1f)maxNewAmplitude=p.amplitude;
    if(maxNewAmplitude>.02f){puts("Teleport generated a high-speed wake");return 1;}
    state.Update(6,nullptr,0);
    if(state.ActiveRipples(6)!=0){puts("Disappeared emitter left permanent waves");return 1;}
    c.x=0;c.feetY=-5;state.Update(7,&c,1);
    bool savedDryHeight=false;
    for(const auto& p:state.Ripples())if(p.active && p.contact.feetY==-5)savedDryHeight=true;
    if(!savedDryHeight){puts("Dry height lost before GPU surface filtering");return 1;}
    state.Clear();
    if(state.ActiveRipples(7)){puts("Reset retained pulses");return 1;}
    printf("PASS: stationary, moving trail (%u pulses), teleport, decay, contact height, reset\n",old);
}
