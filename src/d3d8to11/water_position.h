#pragma once
#include <cmath>
#include <cstddef>

// Evaluate only the supported position prefix of a VS 1.1, not its lighting.
// Water uses FLOAT2 -> (x,0,z,1), scaling and offsets before projection.
// Fail closed on unfamiliar instructions; never guess c0*v0 or shader handles.
template<class Token>
bool WaterPositionClip(const Token* code, size_t count, const float input[4],
                       const float constants[][4], size_t constantCount, float clip[4])
{
    if(count<2 || code[0]!=0xfffe0101u)return false;
    float temp[12][4]={}; unsigned written=0;
    auto source=[&](Token token,int row,float out[4])->bool {
        if(token&0x2000u)return false; // relative constants unsupported
        unsigned type=(token>>28)&7,reg=(token&0x7ff)+row;
        const float* p=nullptr;
        if(type==0 && reg<12)p=temp[reg];
        else if(type==1 && reg==0)p=input;
        else if(type==2 && reg<constantCount)p=constants[reg];
        else return false;
        unsigned modifier=(token>>24)&15;
        if(modifier>1)return false;
        for(int c=0;c<4;c++)out[c]=p[(token>>(16+c*2))&3]*(modifier==1?-1.f:1.f);
        return true;
    };
    for(size_t at=1;at<count;){
        Token tok=code[at++];unsigned op=tok&0xffff;
        if(op==0xffff)return false;
        if(op==0xfffe){size_t n=(tok>>16)&0x7fff;if(n>count-at)return false;at+=n;continue;}
        if(op==0)continue;
        size_t n=op==1?2:((op==2 || op==3 || op==5 || op==20)?3:0);
        if(!n || n>count-at)return false;
        const Token* p=code+at;at+=n;
        float a[4],b[4],value[4]={};
        if(!source(p[1],0,a))return false;
        if(op==20){
            for(int r=0;r<4;r++){
                if(!source(p[2],r,b))return false;
                for(int c=0;c<4;c++)value[r]+=a[c]*b[c];
            }
        }else{
            if(n==3 && !source(p[2],0,b))return false;
            for(int c=0;c<4;c++)value[c]=op==1?a[c]:op==2?a[c]+b[c]:op==3?a[c]-b[c]:a[c]*b[c];
        }
        unsigned type=(p[0]>>28)&7,reg=p[0]&0x7ff,mask=(p[0]>>16)&15;
        if(p[0]&0x0ff00000u)return false; // unsupported destination modifiers
        if(mask==0)mask=15;
        float* target=nullptr;
        if(type==0 && reg<12)target=temp[reg];
        else if(type==4 && reg==0){target=clip;written|=mask;}
        else if(type!=5 && type!=6)return false;
        if(target)for(int c=0;c<4;c++)if(mask&(1u<<c))target[c]=value[c];
        if(written==15){for(int c=0;c<4;c++)if(!std::isfinite(clip[c]))return false;return true;}
    }
    return false;
}
