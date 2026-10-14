// shaders.cpp - vs_1_1 / ps_1_x bytecode to HLSL (shader model 4), plus the fixed-function
// texture stage pipeline expressed as HLSL.
//
// Token format reference (identical in D3D8 and D3D9 for 1.x shaders):
//   instruction token: opcode in bits 0-15, co-issue flag bit 30 (ps). 1.x tokens carry no length,
//                      so parameter counts come from the opcode table below.
//   register token:    number in bits 0-10, type in bits 28-30.
//   destination:       write mask bits 16-19, saturate bit 20, shift bits 24-27 (signed nibble).
//   source:            swizzle bits 16-23 (2 bits per component), modifier bits 24-27,
//                      vs relative addressing (c[a0.x + n]) bit 13.
#include "shaders.h"
#include <stdio.h>
#include <stdarg.h>

namespace {

enum Opcode {
    OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX,
    OP_SLT, OP_SGE, OP_EXP, OP_LOG, OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4x4, OP_M4x3, OP_M3x4, OP_M3x3,
    OP_M3x2,
    OP_TEXCOORD = 64, OP_TEXKILL, OP_TEX, OP_TEXBEM, OP_TEXBEML, OP_TEXREG2AR, OP_TEXREG2GB,
    OP_TEXM3x2PAD, OP_TEXM3x2TEX, OP_TEXM3x3PAD, OP_TEXM3x3TEX, OP_TEXM3x3DIFF, OP_TEXM3x3SPEC,
    OP_TEXM3x3VSPEC, OP_EXPP, OP_LOGP, OP_CND, OP_DEF, OP_TEXREG2RGB, OP_TEXDP3TEX, OP_TEXM3x2DEPTH,
    OP_TEXDP3, OP_TEXM3x3, OP_TEXDEPTH, OP_CMP, OP_BEM,
    OP_PHASE = 0xFFFD, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF,
};

enum RegType { RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_ADDR_TEX = 3, RT_RASTOUT = 4, RT_ATTROUT = 5,
               RT_TEXCRDOUT = 6 };

int ParamCount(DWORD op, bool ps, DWORD minor)
{
    switch (op) {
    case OP_NOP: case OP_PHASE: return 0;
    case OP_MOV: case OP_RCP: case OP_RSQ: case OP_EXP: case OP_LOG: case OP_LIT: case OP_FRC:
    case OP_EXPP: case OP_LOGP: return 2;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DP3: case OP_DP4: case OP_MIN: case OP_MAX:
    case OP_SLT: case OP_SGE: case OP_DST: case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3:
    case OP_M3x2: case OP_BEM: return 3;
    case OP_MAD: case OP_LRP: case OP_CND: case OP_CMP: return 4;
    case OP_DEF: return 5;
    case OP_TEXCOORD: case OP_TEX: return (ps && minor >= 4) ? 2 : 1;
    case OP_TEXKILL: case OP_TEXDEPTH: return 1;
    case OP_TEXM3x3SPEC: return 3;
    case OP_TEXBEM: case OP_TEXBEML: case OP_TEXREG2AR: case OP_TEXREG2GB: case OP_TEXM3x2PAD:
    case OP_TEXM3x2TEX: case OP_TEXM3x3PAD: case OP_TEXM3x3TEX: case OP_TEXM3x3DIFF: case OP_TEXM3x3VSPEC:
    case OP_TEXREG2RGB: case OP_TEXDP3TEX: case OP_TEXM3x2DEPTH: case OP_TEXDP3: case OP_TEXM3x3: return 2;
    default: return -1;
    }
}

std::string Fmt(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

inline int RegTypeOf(DWORD t) { return (t >> 28) & 7; }
inline int RegNumOf(DWORD t) { return t & 0x7FF; }

std::string SwizzleOf(DWORD t)
{
    DWORD s = (t >> 16) & 0xFF;
    if (s == 0xE4) return "";
    static const char comp[] = "xyzw";
    std::string r = ".";
    for (int i = 0; i < 4; i++) r += comp[(s >> (i * 2)) & 3];
    return r;
}

std::string MaskOf(DWORD t)
{
    DWORD m = (t >> 16) & 0xF;
    if (m == 0xF || m == 0) return "";
    std::string r = ".";
    if (m & 1) r += 'x';
    if (m & 2) r += 'y';
    if (m & 4) r += 'z';
    if (m & 8) r += 'w';
    return r;
}

std::string ApplySourceModifier(const std::string& v, DWORD t)
{
    switch ((t >> 24) & 0xF) {
    case 0: return v;
    case 1: return "(-" + v + ")";
    case 2: return "(" + v + " - 0.5)";
    case 3: return "(0.5 - " + v + ")";
    case 4: return "(2.0 * " + v + " - 1.0)";
    case 5: return "(1.0 - 2.0 * " + v + ")";
    case 6: return "(1.0 - " + v + ")";
    case 7: return "(2.0 * " + v + ")";
    case 8: return "(-2.0 * " + v + ")";
    default: return v; // _dz/_dw (ps 1.4 projective) handled by texld only
    }
}

// Splits a shader into instructions: {opcode token, params...}.
struct Instr {
    DWORD op;
    bool coissue;
    std::vector<DWORD> p;
};

bool Decode(const DWORD* func, bool ps, std::vector<Instr>& out, DWORD& minor, std::string& error)
{
    DWORD ver = func[0];
    minor = ver & 0xFF;
    const DWORD* t = func + 1;
    for (int guard = 0; guard < 65536; guard++) {
        DWORD tok = *t++;
        DWORD op = tok & 0xFFFF;
        if (tok == 0x0000FFFF) return true;
        if (op == OP_COMMENT) { t += (tok >> 16) & 0x7FFF; continue; }
        int n = ParamCount(op, ps, minor);
        if (n < 0) { error = Fmt("unknown opcode %lu", op); return false; }
        Instr in;
        in.op = op;
        in.coissue = (tok & 0x40000000) != 0;
        in.p.assign(t, t + n);
        t += n;
        out.push_back(in);
    }
    error = "missing end token";
    return false;
}

const char* kHelpers =
    "static const float FLT_BIG = 3.402823466e+38;\n"
    "float4 rcp_d3d(float x) { return (x == 0.0 ? FLT_BIG : 1.0 / x).xxxx; }\n"
    "float4 rsq_d3d(float x) { x = abs(x); return (x == 0.0 ? FLT_BIG : rsqrt(x)).xxxx; }\n"
    "float4 log_d3d(float x) { x = abs(x); return (x == 0.0 ? -FLT_BIG : log2(x)).xxxx; }\n"
    "float4 expp_d3d(float x) { float f = floor(x); return float4(exp2(f), x - f, exp2(x), 1.0); }\n"
    "float4 logp_d3d(float x) { x = abs(x); if (x == 0.0) return float4(-FLT_BIG, 1.0, -FLT_BIG, 1.0);\n"
    "  float e = floor(log2(x)); return float4(e, x / exp2(e), log2(x), 1.0); }\n"
    "float4 lit_d3d(float4 s) { float4 r = float4(1.0, 0.0, 0.0, 1.0);\n"
    "  if (s.x > 0.0) { r.y = s.x; float p = clamp(s.w, -128.0, 128.0);\n"
    "    r.z = (s.y > 0.0) ? pow(s.y, p) : (p == 0.0 ? 1.0 : 0.0); } return r; }\n";

} // namespace

const char* kVSOutputStruct =
    "struct VSOut {\n"
    "  float4 pos : SV_Position;\n"
    "  float4 d0 : COLOR0;\n"
    "  float4 d1 : COLOR1;\n"
    "  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
    "  float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7;\n"
    "  float fog : FOG;\n"
    "  float3 world : TEXCOORD8; float3 normal : TEXCOORD9;\n"
    "};\n";

namespace {
std::string WaterBlock(int slot) {
    return Fmt("cbuffer Water : register(b%d) { float4 waterClip[4]; float4 waterProject[4]; float4 waterReflection; float4 waterRegions; float4 waterProfiles; float4 waterSwell; float4 waterScreen; float4 waterParams; float4 waterOptical; float4 waterMisc; float4 waterMotion; float4 waterContacts[64]; float4 waterRings[64]; };\n",slot);
}
// Shared VS/PS field: identical phase in world space across all water batches.
std::string WaterSurfaceCode(){
    return
        "Texture2D<float> waterOceanMask : register(t12); Texture2D<float> waterTerrain : register(t13);\n"
        "Texture2D<float> waterShore : register(t14);\n"
        "SamplerState waterClamp : register(s10);\n"
        "float WaterShoreDistance(float2 p){\n"
        " if(waterRegions.x<.5)return 8192;\n"
        " float2 edge=clamp(p,float2(0,0),1/waterRegions.zw-1);\n"
        " return waterShore.SampleLevel(waterClamp,(edge+.5)*waterRegions.zw,0)+length(p-edge);}\n"
        "float2 WaterShoreGradient(float2 p){\n"
        " return float2(WaterShoreDistance(p+float2(2,0))-WaterShoreDistance(p-float2(2,0)),\n"
        " WaterShoreDistance(p+float2(0,2))-WaterShoreDistance(p-float2(0,2)))*.25;}\n"
        "float WaterOceanAt(float3 p){\n"
        " if(waterRegions.x<.5)return waterRegions.y;\n"
        " float2 uv=(p.xz+.5)*waterRegions.zw;\n"
        " if(any(p.xz<0) || any(uv>1))return 1;\n"
        " return waterOceanMask.SampleLevel(waterClamp,uv,0);}\n"
        "float WaterCoastFade(float3 p){\n"
        " if(waterRegions.x<.5)return 1;\n"
        " float2 uv=(p.xz+.5)*waterRegions.zw;\n"
        " if(any(p.xz<0) || any(uv>1))return 1;\n"
        " float depth=waterProfiles.w-waterTerrain.SampleLevel(waterClamp,uv,0);\n"
        " return smoothstep(.15,max(.3,waterSwell.z),depth);}\n"
        // Height is positive UP; renderer Y is positive DOWN. Wavelengths are
        // deliberately long for the legacy mesh. This is analytic, not fluid simulation.
        "float4 WaterSwellAt(float3 p){\n"
        " float a=waterSwell.x*WaterOceanAt(p)*WaterCoastFade(p);if(a<=0)return 0;\n"
        " float scale=1/max(.5,waterSwell.y),t=waterOptical.w;\n"
        " float2 d0=float2(.93969,.34202),d1=float2(.81915,.57358),d2=float2(.57358,.81915);\n"
        " float k0=.15708*scale,k1=.29920*scale,k2=.48332*scale;\n"
        " float q0=dot(p.xz,d0)*k0-t*1.05,q1=dot(p.xz,d1)*k1-t*1.38+1.1,q2=dot(p.xz,d2)*k2-t*1.74+2.0;\n"
        " float coastDist=WaterShoreDistance(p.xz);\n"
        " if(coastDist<4096){\n"
        "   float2 g=-WaterShoreGradient(p.xz);d0=g;d1=g;d2=g;\n"
        "   q0=-coastDist*k0-t*1.05;q1=-coastDist*k1-t*1.38+1.1;q2=-coastDist*k2-t*1.74+2.0;}\n"
        // Dominant travelling ridges, not crossing-wave isolated maxima. A mild
        // second harmonic sharpens the profile; its derivative is kept in sync.
        " float h=.80*(sin(q0)+.16*sin(2*q0))/1.16+.15*sin(q1)+.05*sin(q2);\n"
        " float2 gradient=.80*k0*(cos(q0)+.32*cos(2*q0))/1.16*d0+.15*k1*cos(q1)*d1+.05*k2*cos(q2)*d2;\n"
        // A narrow moving leading crest, not foam wherever total height is high.
        " float crest=smoothstep(.92,.99,sin(q0))*(1-smoothstep(-.04,.22,cos(q0)));\n"
        " return float4(h*a,gradient*a,crest*WaterCoastFade(p)*WaterOceanAt(p));}\n";
}
std::string WaterPixelCode(int samples) {
    std::string s = WaterBlock(2);
    s += WaterSurfaceCode();
    s += "Texture2D<float4> waterScene : register(t11);\n"
         "float WaterOcean(VSOut i){return WaterOceanAt(i.world);}\n"
         "float FoamHash(float2 p){return frac(sin(dot(p,float2(127.1,311.7)))*43758.5453);}\n"
         "float FoamNoise(float2 p){float2 cell=floor(p),f=frac(p);f=f*f*(3-2*f);\n"
         " return lerp(lerp(FoamHash(cell),FoamHash(cell+float2(1,0)),f.x),lerp(FoamHash(cell+float2(0,1)),FoamHash(cell+1),f.x),f.y);}\n";
    if (samples > 1) {
        s += Fmt("Texture2DMS<float,%d> waterDepth : register(t10);\n",samples);
        s += Fmt("float WaterDepthAt(int2 p) { float d=1; [unroll] for(int j=0;j<%d;j++) d=min(d,waterDepth.Load(p,j)); return d; }\n", samples);
    } else s += "Texture2D<float> waterDepth : register(t10); float WaterDepthAt(int2 p) { return waterDepth.Load(int3(p,0)); }\n";
    s +=
        "float2 WaterInteractionSlope(VSOut i) {\n"
        "  float2 slope=0;\n"
        "  [loop] for(int k=0;k<(int)waterMotion.x;k++) {\n"
        "    float4 contact=waterContacts[k], ring=waterRings[k];\n"
        "    float age=waterMotion.z-contact.w;\n"
        // Feet above the local surface are on dry land. This is evaluated against
        // each water pixel, so events never turn a land-only contact into a wake.
        "    if(age<0 || age>ring.w || contact.y<i.world.y-0.10) continue;\n"
        "    float2 delta=i.world.xz-contact.xz; float distance=length(delta);\n"
        "    float front=ring.y+age*ring.z, width=0.40+age*0.12;\n"
        "    float d=distance-front; if(abs(d)>width*3 || distance<0.001) continue;\n"
        "    float envelope=exp(-d*d/(width*width)-age*1.25)*(1-smoothstep(ring.w-.5,ring.w,age));\n"
        "    float gradient=ring.x*envelope*(5*cos(d*5)-2*d/(width*width)*sin(d*5));\n"
        "    slope+=delta/distance*gradient;\n"
        "  }\n"
        "  return clamp(slope*waterMotion.y,-0.30,0.30); }\n"
        // The legacy t0 is a simulation/lookup field, not a validated world
        // normal map. Generate one continuous gradient field across all UV tiles.
        "float4 WaterWave(VSOut i) {\n"
        "  float2 d0=float2(0.93969,0.34202),d1=float2(-0.52992,0.84805),d2=float2(0.25882,-0.96593);\n"
        "  float ocean=WaterOcean(i);float2 p=i.world.xz; float t=waterOptical.w*lerp(.55,1,ocean);\n"
        "  float2 slope=d0*(0.035*0.9*cos(dot(p,d0)*0.9-t*0.8))\n"
        "              +d1*(0.016*1.7*cos(dot(p,d1)*1.7+t*1.1))\n"
        "              +d2*(0.006*2.8*cos(dot(p,d2)*2.8-t*1.35));\n"
        "  slope*=waterMisc.y*lerp(waterProfiles.y,waterProfiles.z,ocean);\n"
        "  slope+=WaterSwellAt(i.world).yz;\n"
        "  slope+=WaterInteractionSlope(i);\n"
        "  float3 n=normalize(float3(slope,1));\n"
        "  return float4(n*0.5+0.5,1); }\n"
        "float3 WaterUnproject(float2 pixel,float d) {\n"
        "  float2 ndc=float2(pixel.x*waterScreen.x*2-1,1-pixel.y*waterScreen.y*2)-waterScreen.zw;\n"
        "  float4 q=float4(ndc,d,1);\n"
        "  float4 p=float4(dot(q,waterClip[0]),dot(q,waterClip[1]),dot(q,waterClip[2]),dot(q,waterClip[3]));\n"
        "  return p.xyz / (abs(p.w)>1e-8 ? p.w : 1e-8); }\n"
        // No history buffers: current-camera snapshot only, avoiding stale reflection ghosts.
        "bool WaterRayProbe(float3 P,out float2 uv,out float3 sceneP,out float gap) {\n"
        "  float4 q=float4(P,1); float4 clip=float4(dot(q,waterProject[0]),dot(q,waterProject[1]),dot(q,waterProject[2]),dot(q,waterProject[3]));\n"
        "  uv=0;sceneP=0;gap=-1; if(clip.w<=0.00001)return false;\n"
        "  float3 ndc=clip.xyz/clip.w; ndc.xy+=waterScreen.zw;\n"
        "  uv=float2(ndc.x*.5+.5,.5-ndc.y*.5);\n"
        "  if(any(uv<waterScreen.xy) || any(uv>1-waterScreen.xy) || ndc.z<0 || ndc.z>1)return false;\n"
        "  int2 size=int2(1/waterScreen.xy),pixel=clamp(int2(uv*size),int2(0,0),size-1);\n"
        "  float d=WaterDepthAt(pixel);if(d>=.999999)return true;\n"
        "  sceneP=WaterUnproject(pixel+.5,d);\n"
        "  float3 eye=P-sunCamera.xyz;float len=length(eye);\n"
        "  gap=dot(P-sceneP,eye/max(len,.0001));return true; }\n"
        "float4 WaterSceneReflection(VSOut i,float3 N,float3 V) {\n"
        "  if(waterReflection.x<=0)return 0;\n"
        "  float3 R=normalize(reflect(-V,N));\n"
        "  if(R.y>=-.005)return 0;\n"
        "  float3 origin=i.world+N*.06;float lastT=.08,lastGap=-1;\n"
        "  int steps=(int)waterReflection.z;\n"
        "  [loop] for(int k=1;k<=steps;k++) {\n"
        "    float fraction=(float)k/steps,t=.08+waterReflection.y*fraction*fraction;\n"
        "    float2 uv;float3 sceneP;float gap;\n"
        "    if(!WaterRayProbe(origin+R*t,uv,sceneP,gap))break;\n"
        "    if(lastGap<0 && gap>=0) {\n"
        "      float lo=lastT,hi=t;\n"
        "      [unroll] for(int j=0;j<5;j++){float mid=(lo+hi)*.5;float2 mu;float3 mp;float mg;\n"
        "        if(WaterRayProbe(origin+R*mid,mu,mp,mg) && mg>=0)hi=mid;else lo=mid;}\n"
        "      if(WaterRayProbe(origin+R*hi,uv,sceneP,gap) && gap>=0 && gap<.6 && sceneP.y<i.world.y-.04) {\n"
        "        float edge=min(min(uv.x,uv.y),min(1-uv.x,1-uv.y));\n"
        "        float confidence=smoothstep(0,.055,edge)*(1-smoothstep(waterReflection.y*.75,waterReflection.y,hi));\n"
        "        float2 blur=waterScreen.xy*1.25;\n"
        "        float3 color=waterScene.SampleLevel(waterClamp,uv,0).rgb*.5;\n"
        "        color+=(waterScene.SampleLevel(waterClamp,uv+float2(blur.x,0),0).rgb+waterScene.SampleLevel(waterClamp,uv-float2(blur.x,0),0).rgb\n"
        "          +waterScene.SampleLevel(waterClamp,uv+float2(0,blur.y),0).rgb+waterScene.SampleLevel(waterClamp,uv-float2(0,blur.y),0).rgb)*.125;\n"
        "        return float4(color,confidence*waterReflection.x);\n"
        "      }\n"
        "    }\n"
        "    lastT=t;lastGap=gap;\n"
        "  }return 0; }\n"
        "float4 ApplyWater(float4 original,VSOut i,float4 waveSample,float3 reflected) {\n"
        "  if(waterParams.x<0.5) return original;\n"
        "  int2 size=int2(1/waterScreen.xy), pixel=clamp(int2(i.pos.xy),int2(0,0),size-1);\n"
        "  float d=WaterDepthAt(pixel);\n"
        "  float3 floorP=WaterUnproject(pixel+0.5,d);\n"
        // JPOG world Y points downward; only water behind the opaque depth is valid.
        "  float depth=d>=0.999999 ? waterParams.y*8 : clamp(floorP.y-i.world.y,0,waterParams.y*8);\n"
        "  float2 wave=waveSample.rg*2-1;\n"
        "  float ocean=WaterOcean(i);\n"
        "  float3 N=normalize(float3(wave.x,-(waveSample.b*2-1),wave.y));\n"
        "  float3 eye=sunCamera.xyz-i.world;\n"
        "  float3 V=dot(eye,eye)>1e-8 ? normalize(eye) : float3(0,-1,0);\n"
        "  float4 sceneReflection=WaterSceneReflection(i,N,V);\n"
        "  reflected=lerp(reflected,sceneReflection.rgb,sceneReflection.a);\n"
        "  float fresnel=0.02+0.98*pow(1-saturate(abs(dot(N,V))),5);\n"
        "  float2 uv=(pixel+0.5)*waterScreen.xy;\n"
        "  float2 warped=clamp(uv+wave*waterOptical.z*lerp(.45,1,ocean)*waterScreen.xy,waterScreen.xy*0.5,1-waterScreen.xy*0.5);\n"
        "  int2 wp=clamp(int2(warped*size),int2(0,0),size-1);\n"
        "  if(WaterDepthAt(wp)<i.pos.z-0.00001) warped=uv;\n"
        "  float3 behind=waterScene.SampleLevel(waterClamp,warped,0).rgb;\n"
        "  float amount=depth/max(waterParams.y,0.01);\n"
        "  float3 transmission=exp(-amount*float3(1.6,0.65,0.40));\n"
        "  float3 deepTint=lerp(float3(.045,.145,.095),float3(.025,.11,.14),ocean);\n"
        "  float3 tint=lerp(float3(0.16,0.32,0.24),deepTint,saturate(amount));\n"
        "  float clarity=lerp(1,waterOptical.y,saturate(amount*2));\n"
        "  float3 through=lerp(tint,behind,transmission*clarity);\n"
        "  float3 surface=lerp(through,reflected,saturate(fresnel*waterOptical.x));\n"
        // Surface slope must remain readable from above, where Fresnel is small.
        // Bounded art-directed body-light contrast and a sun glint; no camera-space
        // phase/noise and no claim of a complete physically based water model.
        "  float3 L=dot(sunDirection.xyz,sunDirection.xyz)>.5 ? normalize(sunDirection.xyz):normalize(float3(.4,-.8,.35));\n"
        "  float ndl=saturate(dot(N,L)),flatNdl=saturate(-L.y);\n"
        "  float bodyLight=clamp(1+2.5*(ndl-flatNdl),.70,1.30);\n"
        "  surface*=lerp(1,bodyLight,ocean);\n"
        "  float3 halfVector=L+V;float3 H=halfVector/max(length(halfVector),.001);\n"
        "  float glint=pow(saturate(dot(N,H)),96)*(.08+.24*fresnel)*waterOptical.x*ocean;\n"
        "  surface+=glint*float3(1,.96,.86);\n"
        // Advancing/receding depth-space fronts plus drifting porous foam, no
        // permanent uniform white outline. Footprint waves remain independent.
        "  float phase=waterOptical.w*1.4+FoamNoise(i.world.xz*.09)*2;\n"
        "  float width=max(.02,waterParams.z)*(.8+.35*sin(phase));\n"
        "  float band=(1-smoothstep(0,width,depth))*smoothstep(.008,.06,depth);\n"
        "  float2 drift=waterRegions.x>.5 ? WaterShoreGradient(i.world.xz)*.16:float2(-.11,-.06);\n"
        "  float2 fp=i.world.xz*1.8+drift*waterOptical.w;\n"
        "  float cells=FoamNoise(fp)*.65+FoamNoise(fp*2.3+7)*.35;\n"
        "  float porous=.40+.60*smoothstep(.22,.7,cells);\n"
        "  float front=.65+.35*sin(depth/max(width,.02)*9+phase*2);\n"
        "  float shore=band*front*porous*waterParams.w*lerp(waterProfiles.x,1,ocean);\n"
        "  float crestBreak=smoothstep(.40,.68,FoamNoise(i.world.xz*.28-float2(.06,.02)*waterOptical.w));\n"
        "  float crest=WaterSwellAt(i.world).w*waterSwell.w*.32*crestBreak*porous;\n"
        "  float foam=saturate(shore+crest);\n"
        "  surface=lerp(surface,float3(0.80,0.86,0.78),foam);\n"
        "  if(waterMisc.x==1) return float4(saturate(amount).xxx,1);\n"
        "  if(waterMisc.x==2) return float4(fresnel.xxx,1);\n"
        "  if(waterMisc.x==3) return float4(foam.xxx,1);\n"
        "  if(waterMisc.x==4) {float disturbance=saturate(length(WaterInteractionSlope(i))*20); return float4(disturbance.xxx,1);}\n"
        "  if(waterMisc.x==5) return float4(sceneReflection.a.xxx,1);\n"
        "  if(waterMisc.x==6) return float4(lerp(float3(.1,.8,.2),float3(.05,.3,1),ocean),1);\n"
        "  if(waterMisc.x==7) return float4(saturate(abs(WaterSwellAt(i.world).x)/max(.001,waterSwell.x)).xxx,1);\n"
        "  return float4(surface,1); }\n";
    return s;
}
std::string SunBlock(int slot) {
    return Fmt("cbuffer Sun : register(b%d) { float4 sunRead[4]; float4 sunWrite[4]; float4 clipToWorld[4]; float4 sunDirection; float4 sunSettings; float4 sunFlags; float4 sunControl; float4 sunCamera;"
               " float4 gameAmbient; float4 gameSun; float4 relightParams; float4 terrainMap; float4 todSun; float4 todAmbient; float4 todNight; float4 rimColor; };\n", slot);
}
// Normal maps (stage 0, t9): relief relative to the sun on top of the game's own lighting. No
// tangents in JPOG's meshes, so the frame comes from screen-space derivatives of the world position
// and texture coordinate (Schueler). sunCamera.w = strength, negative = flip green.
std::string NormalMapCode() {
    return
        "Texture2D normalTex : register(t9);\n"
        "float4 ApplyNormalMap(float4 color, VSOut i) {\n"
        "  if (sunFlags.x < 0.5 || sunFlags.y > 0.5) return color;\n"
        "  float3 n = normalTex.Sample(smp0, i.t0.xy).xyz * 2 - 1;\n"
        "  if (sunCamera.w < 0) n.y = -n.y;\n"
        "  n.z = sqrt(saturate(1 - dot(n.xy, n.xy)));\n"
        "  float3 dp1 = ddx(i.world), dp2 = ddy(i.world);\n"
        "  float2 duv1 = ddx(i.t0.xy), duv2 = ddy(i.t0.xy);\n"
        // Tangent frame from the derivative-consistent normal cross(dp1, dp2); only the normal
        // component is turned toward the camera (back faces of foliage).
        "  float3 N0 = cross(dp1, dp2); float gl = dot(N0, N0);\n"
        "  if (gl < 1e-20) return color;\n"
        "  N0 *= rsqrt(gl);\n"
        "  float3 Ng = dot(N0, sunCamera.xyz - i.world) < 0 ? -N0 : N0;\n"
        "  float3 dp2perp = cross(dp2, N0), dp1perp = cross(N0, dp1);\n"
        "  float3 T = dp2perp * duv1.x + dp1perp * duv2.x, B = dp2perp * duv1.y + dp1perp * duv2.y;\n"
        "  float tl = max(dot(T, T), dot(B, B));\n"
        "  if (tl < 1e-20) return color;\n"
        "  float inv = rsqrt(tl);\n"
        "  float3 Nw = normalize(T * inv * n.x + B * inv * n.y + Ng * n.z);\n"
        "  if (sunControl.y == 11) return float4(Nw * 0.5 + 0.5, 1);\n"
        "  float k = abs(sunCamera.w);\n"
        // Brighter where the detail faces the sun, darker where it turns away; slight cavity term.
        "  float relief = 1 + k * (saturate(dot(Nw, sunDirection.xyz)) - saturate(dot(Ng, sunDirection.xyz)));\n"
        "  relief *= lerp(1, n.z, k * 0.35);\n"
        "  return float4(color.rgb * clamp(relief, 0.35, 1.7), color.a); }\n";
}

// Skin relief (stage 0): height from the colour texture's local luminance (bright = raised, the
// same estimate as the normal maps and the tessellation displacement). ReliefApply runs first in
// main: it shifts i.t0 by offset-limited parallax and marches the height field toward the sun for
// self-shadowing. The tangent frame comes from screen derivatives like the normal maps; steps are
// taken in texels of the mip being displayed so the result does not shimmer with distance.
std::string ReliefCode() {
    return
        "cbuffer Relief : register(b4) { float4 reliefParams; };\n"
        "float ReliefHeight(float2 uv, float lod) {\n"
        "  const float3 luma = float3(0.299, 0.587, 0.114);\n"
        "  float fine = dot(tex0.SampleLevel(smp0, uv, lod).rgb, luma);\n"
        "  float coarse = dot(tex0.SampleLevel(smp0, uv, lod + 3).rgb, luma);\n"
        "  return clamp((fine - coarse) * 3, -1, 1); }\n"
        "float ReliefApply(inout VSOut i) {\n"
        "  float3 dp1 = ddx(i.world), dp2 = ddy(i.world);\n"
        "  float2 uv1 = ddx(i.t0.xy), uv2 = ddy(i.t0.xy);\n"
        "  float3 N = cross(dp1, dp2); float gl = dot(N, N);\n"
        "  if (gl < 1e-20 || sunFlags.y > 0.5) return 1;\n"
        "  N *= rsqrt(gl);\n"
        "  float3 eye = sunCamera.xyz - i.world;\n"
        "  if (dot(N, eye) < 0) N = -N;\n"
        "  float3 a = cross(dp2, N), b = cross(N, dp1);\n"
        "  float3 T = a * uv1.x + b * uv2.x, B = a * uv1.y + b * uv2.y;\n"
        "  if (dot(T, T) < 1e-20 || dot(B, B) < 1e-20) return 1;\n"
        "  T = normalize(T); B = normalize(B);\n"
        "  uint w, h; tex0.GetDimensions(w, h);\n"
        "  float2 size = float2(max(w, 1), max(h, 1));\n"
        // Displayed mip from the derivatives (CalculateLevelOfDetail needs ps_4_1).
        "  float lod = max(0.5 * log2(max(dot(uv1 * size, uv1 * size), dot(uv2 * size, uv2 * size))), 0);\n"
        "  float2 texel = exp2(lod) / size;\n"
        "  float depth = reliefParams.x;\n" // texels per unit height, sign = invert
        // Offset-limited parallax, three refinements: shift toward the viewer by the height.
        "  if (reliefParams.y > 0) {\n"
        "    float3 V = normalize(eye);\n"
        "    float2 dir = float2(dot(V, T), dot(V, B)) / max(dot(V, N), 0.35);\n"
        "    float2 uv = i.t0.xy;\n"
        "    [unroll] for (int k = 0; k < 3; k++)\n"
        "      uv = i.t0.xy + dir * (ReliefHeight(uv, lod) * depth * reliefParams.y * 0.5) * texel;\n"
        "    i.t0.xy = uv;\n"
        "  }\n"
        // Self-shadowing: march toward the sun; occluded when the field rises above the ray.
        "  if (reliefParams.z <= 0) return 1;\n"
        "  float3 L = normalize(sunDirection.xyz);\n"
        "  float3 Lt = float3(dot(L, T), dot(L, B), dot(L, N));\n"
        "  if (Lt.z <= 0.02) return 1;\n" // facing away: the game's own shading already handles it
        "  float horiz = length(Lt.xy);\n"
        "  if (horiz < 1e-3) return 1;\n"
        "  float2 dir = Lt.xy / horiz; float slope = Lt.z / horiz;\n"
        "  float h0 = ReliefHeight(i.t0.xy, lod) * depth;\n"
        "  float occl = 0;\n"
        "  [unroll] for (int s = 1; s <= 8; s++) {\n"
        "    float dist = s * 0.75;\n"
        "    float hs = ReliefHeight(i.t0.xy + dir * dist * texel, lod) * depth;\n"
        "    occl = max(occl, saturate((hs - (h0 + dist * slope)) / reliefParams.w) * (1 - s / 9.0));\n"
        "  }\n"
        "  return 1 - occl * reliefParams.z * saturate(Lt.z * 6); }\n";
}

std::string MaterialCode(bool normalMap, int stage){
    std::string s=
        "cbuffer Material:register(b3){float4 materialSurface;float4 materialOptics;};\n"
        "float3 MaterialNormal(VSOut i){\n"
        " float3 raw=cross(ddy(i.world),ddx(i.world));\n"
        " if(dot(i.normal,i.normal)>1e-8)raw=i.normal;\n"
        " if(dot(raw,raw)<1e-12)return float3(0,-1,0);\n"
        " float3 N=normalize(raw);if(dot(N,sunCamera.xyz-i.world)<0)N=-N;\n";
    if(normalMap)s+=
        " float3 n=normalTex.Sample(smp0,i.t0.xy).xyz*2-1;\n"
        " n.xy*=materialSurface.w;if(sunCamera.w<0)n.y=-n.y;n.z=sqrt(saturate(1-dot(n.xy,n.xy)));\n"
        " float3 dp1=ddx(i.world),dp2=ddy(i.world);float2 uv1=ddx(i.t0.xy),uv2=ddy(i.t0.xy);\n"
        " float3 a=cross(dp2,N),b=cross(N,dp1);float3 T=a*uv1.x+b*uv2.x,B=a*uv1.y+b*uv2.y;\n"
        " float scale=max(dot(T,T),dot(B,B));\n"
        " if(scale>1e-12)N=normalize((T*n.x+B*n.y)*rsqrt(scale)+N*n.z);\n";
    s+=
        " return N;}\n"
        "float4 ApplyMaterial(float4 color,VSOut i){\n"
        " if(materialOptics.y>.5){float t=materialOptics.z;\n"
        "  return float4(t>.5?float3(1,0,0):materialOptics.w<1.5?float3(1,0,1):float3(0,1,1),color.a);}\n"
        " float3 N=MaterialNormal(i),eye=sunCamera.xyz-i.world;\n"
        " float3 V=dot(eye,eye)>1e-8?normalize(eye):N,L=normalize(sunDirection.xyz);\n"
        " float3 halfVector=V+L,H=halfVector/max(length(halfVector),.001);\n"
        " float nl=saturate(dot(N,L)),nv=max(.001,saturate(dot(N,V))),nh=saturate(dot(N,H)),vh=saturate(dot(V,H));\n"
        " float rough=clamp(materialSurface.x,.1,1),alpha=rough*rough,a2=alpha*alpha;\n"
        " float denom=nh*nh*(a2-1)+1,D=a2/(3.14159265*max(denom*denom,.000001));\n"
        " float k=(rough+1)*(rough+1)/8;float G=nv/(nv*(1-k)+k)*nl/max(nl*(1-k)+k,.001);\n"
        " float3 F0=materialOptics.x.xxx,F=F0+(1-F0)*pow(1-vh,5);\n"
        " float3 albedo=pow(max(color.rgb,0),2.2);\n"
        " float3 spec=D*G*F/max(4*nv*nl,.001)*materialSurface.y;\n"
        " bool game=gameSun.w>.5;\n"
        " float3 sunRad=game?GameSun()*3.14159265:sunSettings.y*3.14159265*float3(1,.95,.84);\n"
        " float3 direct=((1-F)*albedo/3.14159265+spec)*nl*sunRad;\n"
        " float sky=saturate(-N.y*.5+.5);\n"
        " float3 ambient=game?albedo*GameAmbient(N):albedo*sunSettings.x*lerp(float3(.38,.34,.27),float3(.78,.88,1),sky);\n"
        // Analytic environment approximation: no cubemap probes or GI claim.
        " float3 R=reflect(-V,N);float horizon=saturate(-R.y*.5+.5);\n"
        " float3 environment=lerp(float3(.10,.07,.04),float3(.24,.34,.50),horizon);\n"
        " float3 envF=F0+(max((1-rough).xxx,F0)-F0)*pow(1-nv,5);\n"
        " float3 indirectSpec=environment*envF*materialSurface.z*(1-.5*rough);\n"
        " float3 lit=ambient+direct+indirectSpec;\n"
        " if(game){float3 rim=RimLight(N,i.world);lit+=albedo*rim+rim*.08;}\n"
        " return float4(game?LinearToOutput(lit):pow(max(lit,0),1/2.2),color.a);}\n";
    if(stage==1){size_t p=0;while((p=s.find("i.t0.xy",p))!=std::string::npos){s.replace(p,7,"i.t1.xy");p+=7;}}
    return s;
}
std::string SunPixelCode() {
    return SunBlock(1) +
        // Shadows are applied after the world pass (sun.cpp resolve). In-pass work is only the
        // optional replacement lighting ([Lighting] ReplaceOriginal=1); otherwise colour is untouched.
        // Replacement starts from the game's own light colours (gameAmbient/gameSun, read per draw
        // from the vertex shader constants: docs/LIGHTING_REPLACEMENT.md) and evaluates them in linear
        // light: full sun and full shade reproduce the original colours exactly, the rest is
        // physically summed and gets a sky/ground tinted ambient. Output goes through exposure and
        // a highlight-only shoulder so values the game kept below 1 are not changed.
        "float3 ToLinear(float3 c) { return pow(max(c, 0), 2.2); }\n"
        "float3 Shoulder(float3 c) {\n"
        "  const float start = 0.8; float peak = max(c.r, max(c.g, c.b));\n"
        "  if (peak <= start) return c;\n"
        "  const float d = 1 - start; float np = 1 - d * d / (peak + d - start);\n"
        "  c *= np / peak;\n"
        "  float g = 1 - 1 / (0.15 * (peak - np) + 1);\n"
        "  return lerp(c, np.xxx, g); }\n"
        "float3 LinearToOutput(float3 c) {\n"
        "  c *= relightParams.w;\n"
        "  if (gameAmbient.w > 0.5) c = Shoulder(c);\n"
        "  return pow(saturate(c), 1 / 2.2); }\n"
        "float3 GameAmbient(float3 N) {\n"
        "  float up = saturate(-N.y * 0.5 + 0.5);\n" // JPOG is Y-down
        "  float3 tint = lerp(float3(0.95, 0.85, 0.65), float3(0.92, 1.02, 1.25), up);\n"
        "  return ToLinear(saturate(gameAmbient.rgb)) * relightParams.y * lerp(1, tint, relightParams.z) * todAmbient.rgb; }\n"
        // The game's vertex colour output is clamped to 1: its full-sun light is saturate(A + S).
        "float3 GameSun() { return max(ToLinear(saturate(gameAmbient.rgb + gameSun.rgb)) - ToLinear(saturate(gameAmbient.rgb)), 0) * relightParams.x * todSun.rgb; }\n"
        // Native terrain height field (terrain_light.cpp): one sample per world unit, height up =
        // -renderer Y. Smooth normals from central differences; horizon shadows by marching
        // toward the sun with growing steps up to ~300 units.
        "Texture2D<float> terrainHeight : register(t15); SamplerState terrainSmp : register(s11);\n"
        "float TerrainH(float2 p) { return terrainHeight.SampleLevel(terrainSmp, (p + 0.5) * terrainMap.yz, 0); }\n"
        "bool TerrainInside(float2 p) { float2 uv = (p + 0.5) * terrainMap.yz; return all(uv > 0) && all(uv < 1); }\n"
        "float3 TerrainNormal(float2 p) {\n"
        "  float hx = TerrainH(p + float2(1, 0)) - TerrainH(p - float2(1, 0));\n"
        "  float hz = TerrainH(p + float2(0, 1)) - TerrainH(p - float2(0, 1));\n"
        "  return normalize(float3(-0.5 * hx, -1, -0.5 * hz)); }\n"
        "float HorizonShadow(float3 w, float3 L) {\n"
        "  float len = length(L.xz); if (len < 1e-3) return 1;\n"
        "  float2 d = L.xz / len; float rise = -L.y / len;\n" // height gained per unit toward the sun
        "  if (rise <= 0) return 0;\n"
        "  float h0 = -w.y + 0.25, vis = 1, t = 1.5;\n"
        "  [loop] for (int k = 0; k < 24; k++) {\n"
        "    float2 q = w.xz + d * t; if (!TerrainInside(q)) break;\n"
        "    vis = min(vis, 1 - saturate((TerrainH(q) - (h0 + t * rise)) / (0.4 + 0.03 * t)));\n"
        "    t *= 1.27;\n"
        "  }\n"
        "  return vis; }\n"
        // Rim light (todAmbient.w, colour rimColor): the sun or the moon outlines silhouettes, most
        // strongly when it is behind the model.
        "float3 RimLight(float3 N, float3 world) {\n"
        "  if (todAmbient.w <= 0) return 0;\n"
        "  float3 V = sunCamera.xyz - world; float vl = dot(V, V); if (vl < 1e-8) return 0;\n"
        "  V *= rsqrt(vl); if (dot(N, V) < 0) N = -N;\n"
        "  float3 L = normalize(sunDirection.xyz);\n"
        "  float edge = pow(1 - saturate(dot(N, V)), 3);\n"
        "  float facing = saturate(dot(N, L) * 0.7 + 0.3), back = saturate(dot(-V, L) * 0.5 + 0.5);\n"
        "  return rimColor.rgb * edge * facing * lerp(0.5, 1.5, back) * todAmbient.w; }\n"
        "float4 RelightSurface(float4 color, VSOut i) {\n"
        "  if (sunFlags.x < 0.5 || sunFlags.y > 0.5 || sunControl.x < 0.5) return color;\n"
        "  bool rigid = sunFlags.w == 3 && sunFlags.z > 0.5;\n" // paths, relit with the terrain's light
        "  if (sunFlags.w != 1 && sunFlags.w != 2 && !rigid) return color;\n" // grass keeps its baked light
        "  if (sunControl.y == 1) return color;\n"
        "  float3 rawN = cross(ddy(i.world), ddx(i.world));\n"
        "  if (sunFlags.w == 1 && dot(i.normal,i.normal) > 1e-8) rawN = i.normal;\n"
        "  float nlen = dot(rawN, rawN); if (nlen < 1e-12) return color;\n"
        "  float3 N = rawN * rsqrt(nlen);\n"
        "  if (sunFlags.w != 1 && N.y > 0) N = -N;\n" // terrain and paths face up (-Y)
        "  bool field = sunFlags.w != 1 && terrainMap.x > 0.5 && TerrainInside(i.world.xz);\n"
        "  if (field) N = TerrainNormal(i.world.xz);\n"
        "  if (sunControl.y == 2) return float4(N*0.5+0.5,color.a);\n"
        // The game's baked factor also holds painted darkening (under trees, hollows): its ratio
        // to what the field normal gives under the same sun is kept as occlusion (sunControl.w),
        // so the game's sun reproduces the baked look and a moved sun keeps that detail.
        "  float fieldNdl = 1, fieldShadow = 1;\n"
        "  if (field) {\n"
        "    float3 L = normalize(sunDirection.xyz);\n"
        "    fieldNdl = saturate(dot(N, L));\n"
        "    fieldShadow = lerp(1, HorizonShadow(i.world, L), terrainMap.w);\n"
        "  }\n"
        // Paths: their vertex colour is the game's light saturate(A + S * f) times a tint. Recover
        // the sun factor f from its brightness and keep the rest as tint.
        "  if (rigid) {\n"
        "    const float3 luma = float3(0.299, 0.587, 0.114);\n"
        "    float3 A = saturate(gameAmbient.rgb), S = gameSun.rgb, baked = saturate(i.d0.rgb);\n"
        "    float f = saturate((dot(baked, luma) - dot(A, luma)) / max(dot(S, luma), 1e-3));\n"
        "    float3 tint = clamp(baked / max(saturate(A + S * f), 1e-3), 0, 2);\n"
        "    float sunF = f;\n"
        "    if (field) sunF = fieldNdl * min(fieldShadow, lerp(1, fieldNdl > 0.05 ? saturate(f / fieldNdl) : 1, sunControl.w));\n"
        "    float3 light = GameAmbient(N) + GameSun() * sunF;\n"
        "    return float4(LinearToOutput(ToLinear(color.rgb * tint) * light), color.a);\n"
        "  }\n"
        // Terrain: the game's per-vertex sun factor (v0.w, passed in t7.x) keeps its smooth slopes.
        "  float ndl = saturate(dot(N, sunDirection.xyz));\n"
        "  if (sunFlags.w == 2) {\n"
        "    float baked = saturate(i.t7.x);\n"
        "    ndl = field ? fieldNdl * min(fieldShadow, lerp(1, fieldNdl > 0.05 ? saturate(baked / fieldNdl) : 1, sunControl.w)) : baked;\n"
        "  }\n"
        "  float3 light = GameAmbient(N) + GameSun() * ndl;\n"
        "  float3 rim = sunFlags.w == 1 ? RimLight(N, i.world) : 0;\n"
        "  light += rim;\n"
        // The terrain shader fades its lighting out under distant haze; keep that.
        "  if (sunFlags.w == 2) light = lerp(light, 1, saturate(i.d1.w));\n"
        "  return float4(LinearToOutput(ToLinear(color.rgb) * light + rim * 0.08), color.a);\n"
        "}\n"
        "float4 ApplySun(float4 color, VSOut i) {\n"
        "  if (sunControl.y == 13 && sunFlags.y < 0.5) {\n"
        "    float k = sunFlags.x > 0.5 ? sunFlags.w : -1;\n"
        "    float3 c = k == 1 ? float3(1, 0.2, 0.2) : k == 2 ? float3(0.2, 1, 0.2) : k == 3 ? float3(0.2, 0.4, 1) : k == 4 ? float3(1, 1, 0.2) : k == 0 ? float3(1, 0.3, 1) : float3(0.5, 0.5, 0.5);\n"
        "    return float4(lerp(color.rgb, c, 0.7), color.a); }\n"
        "  bool relit = sunFlags.x > 0.5 && sunFlags.y < 0.5 && sunControl.x > 0.5 &&\n"
        "    (sunFlags.w == 1 || sunFlags.w == 2 || (sunFlags.w == 3 && sunFlags.z > 0.5));\n"
        // Called unconditionally (it returns the colour unchanged when not relit): fxc overflows
        // its stack on this function inlined inside a branch.
        "  float4 lit = RelightSurface(color, i);\n"
        "  if (!relit && todNight.w > 0.5) lit.rgb *= todNight.rgb;\n" // grass, water, other world surfaces
        "  return lit; }\n";
}
}

int WorldShaderKind(const VSDecl& decl) {
    if (decl.inputs.size() == 5 && decl.inputs[0].type == 2 && decl.inputs[1].type == 2) return 1;
    if (decl.inputs.size() == 2 && decl.inputs[0].type == 4 && decl.inputs[1].stream == 1) return 2;
    if (decl.inputs.size() == 3 && decl.inputs[0].type == 2 && decl.inputs[1].type == 4) return 3;
    if (decl.inputs.size() == 3 && decl.inputs[0].type == 2 && decl.inputs[2].stream == 1) return 4;
    return 0;
}

size_t ShaderTokenCount(const DWORD* func)
{
    bool ps = (func[0] & 0xFFFF0000) == 0xFFFF0000;
    DWORD minor = func[0] & 0xFF;
    const DWORD* t = func + 1;
    for (int guard = 0; guard < 65536; guard++) {
        DWORD tok = *t++;
        if (tok == 0x0000FFFF) break;
        DWORD op = tok & 0xFFFF;
        if (op == OP_COMMENT) { t += (tok >> 16) & 0x7FFF; continue; }
        int n = ParamCount(op, ps, minor);
        if (n < 0) break;
        t += n;
    }
    return t - func;
}

size_t ParseVSDecl(const DWORD* decl, VSDecl& out)
{
    const DWORD* t = decl;
    int stream = 0;
    UINT offset = 0;
    static const UINT typeSize[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };
    for (int guard = 0; guard < 4096; guard++) {
        DWORD tok = *t++;
        if (tok == 0xFFFFFFFF) break;
        switch (tok >> 29) {
        case 1: // D3DVSD_TOKEN_STREAM
            stream = tok & 0xF;
            offset = 0;
            out.streamMask |= 1u << stream;
            break;
        case 2: // D3DVSD_TOKEN_STREAMDATA
            if (tok & 0x10000000) { offset += ((tok >> 16) & 0xF) * 4; break; } // skip dwords
            {
                VSInput in;
                in.reg = tok & 0x1F;
                in.type = (tok >> 16) & 0xF;
                in.stream = stream;
                in.offset = offset;
                offset += in.type < 8 ? typeSize[in.type] : 4;
                out.inputs.push_back(in);
            }
            break;
        case 4: { // D3DVSD_TOKEN_CONSTMEM: count in bits 25-28 (float4 registers), address bits 0-7
            UINT count = (tok >> 25) & 0xF, addr = tok & 0xFF;
            std::vector<float> v(count * 4);
            memcpy(v.data(), t, count * 16);
            t += count * 4;
            out.constants.push_back({ addr, v });
            break;
        }
        case 3: // tessellator - not supported
        case 5: // extension
            if ((tok >> 29) == 5) t += (tok >> 24) & 0x1F;
            Log("vertex declaration: unsupported token 0x%08lx", tok);
            break;
        default:
            break;
        }
    }
    return t - decl;
}

// ---- vertex shaders --------------------------------------------------------------------------

namespace {

std::string VSRegister(DWORD t, int offset)
{
    int type = RegTypeOf(t), n = RegNumOf(t) + offset;
    switch (type) {
    case RT_TEMP: return Fmt("r%d", n);
    case RT_INPUT: return Fmt("v%d", n);
    case RT_CONST:
        if (t & (1 << 13)) return Fmt("C(a0 + %d)", n);
        return Fmt("c[%d]", n);
    case RT_ADDR_TEX: return "float4(a0, a0, a0, a0)";
    case RT_RASTOUT: return n == 0 ? "oPos" : (n == 1 ? "oFog" : "oPts");
    case RT_ATTROUT: return Fmt("od%d", n);
    case RT_TEXCRDOUT: return Fmt("ot%d", n);
    default: return "float4(0, 0, 0, 0)";
    }
}

std::string VSSource(DWORD t, int offset = 0)
{
    return ApplySourceModifier(VSRegister(t, offset) + SwizzleOf(t), t);
}

} // namespace

std::string TranslateVS(const DWORD* func, const VSDecl& decl, std::string& error)
{
    if ((func[0] & 0xFFFF0000) != 0xFFFE0000) { error = "not a vertex shader"; return ""; }
    std::vector<Instr> code;
    DWORD minor;
    if (!Decode(func, false, code, minor, error)) return "";

    std::string s;
    s += "cbuffer VSConstants : register(b0) { float4 c[256]; };\n";
    s += "cbuffer VSExtra : register(b1) { float4 pixelOffset; };\n";
    s += SunBlock(2);
    s += WaterBlock(4); // b3 belongs to interactive grass
    s += WaterSurfaceCode();
    const bool grass = WorldShaderKind(decl) == 4;
    if (grass) {
        // Interactive grass: creatures near a blade bend it away and flatten it, scaled by the
        // blade-local height (v0.y, which the game also uses for its wind sway). Pushers are
        // filled per frame in sun.cpp (CapturePusher).
        s += "cbuffer Grass : register(b3) { float4 pushers[32]; float4 grassParams; };\n"
             "float4 GrassPush(float4 p, float h) {\n"
             "  float hh = abs(h);\n"
             "  [loop] for (int k = 0; k < (int)grassParams.x; k++) {\n"
             "    float2 d = p.xz - pushers[k].xz; float dist = length(d); float r = pushers[k].w;\n"
             "    if (dist >= r) continue;\n"
             "    float f = 1 - dist / r; f = f * f * (3 - 2 * f);\n"
             "    float2 dir = dist > 1e-3 ? d / dist : float2(1, 0);\n"
             "    p.xz += dir * (f * hh * grassParams.y);\n"
             "    p.y -= h * f * grassParams.z;\n"
             "  }\n"
             "  return p; }\n";
    }
    s += "float4 C(int i) { return (i >= 0 && i < 256) ? c[i] : float4(0, 0, 0, 0); }\n";
    s += kHelpers;
    s += kVSOutputStruct;
    s += "struct VSIn {\n";
    static const char* inTypes[8] = { "float", "float2", "float3", "float4", "float4", "uint4", "int2", "int4" };
    for (const VSInput& in : decl.inputs)
        s += Fmt("  %s v%d : TEXCOORD%d;\n", inTypes[in.type & 7], in.reg, in.reg);
    s += "};\n";
    s += "VSOut main(VSIn i) {\n";
    s += "  float4 worldPosition = 0;\n";
    for (int r = 0; r < 16; r++) {
        bool declared = false;
        for (const VSInput& in : decl.inputs) {
            if (in.reg != r) continue;
            declared = true;
            switch (in.type) {
            case 0: s += Fmt("  float4 v%d = float4(i.v%d, 0, 0, 1);\n", r, r); break;
            case 1: s += Fmt("  float4 v%d = float4(i.v%d, 0, 1);\n", r, r); break;
            case 2: s += Fmt("  float4 v%d = float4(i.v%d, 1);\n", r, r); break;
            case 6: s += Fmt("  float4 v%d = float4(i.v%d, 0, 1);\n", r, r); break;
            default: s += Fmt("  float4 v%d = float4(i.v%d);\n", r, r); break;
            }
        }
        if (!declared) s += Fmt("  float4 v%d = float4(0, 0, 0, 1);\n", r);
    }
    s += "  float4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0;\n";
    s += "  int a0 = 0;\n";
    s += "  float4 oPos = 0, oFog = 0, oPts = 0, od0 = 1, od1 = 0;\n";
    s += "  float4 ot0 = 0, ot1 = 0, ot2 = 0, ot3 = 0, ot4 = 0, ot5 = 0, ot6 = 0, ot7 = 0;\n";

    for (const Instr& in : code) {
        const std::vector<DWORD>& p = in.p;
        if (in.op == OP_NOP) continue;
        std::string expr;
        auto S = [&](int i, int off = 0) { return VSSource(p[i], off); };
        switch (in.op) {
        case OP_MOV:
            if (RegTypeOf(p[0]) == RT_ADDR_TEX) { s += "  a0 = (int)floor((" + S(1) + ").x);\n"; continue; }
            expr = S(1); break;
        case OP_ADD: expr = S(1) + " + " + S(2); break;
        case OP_SUB: expr = S(1) + " - " + S(2); break;
        case OP_MUL: expr = S(1) + " * " + S(2); break;
        case OP_MAD: expr = S(1) + " * " + S(2) + " + " + S(3); break;
        case OP_DP3: expr = "dot((" + S(1) + ").xyz, (" + S(2) + ").xyz).xxxx"; break;
        case OP_DP4: expr = "dot(" + S(1) + ", " + S(2) + ").xxxx"; break;
        case OP_MIN: expr = "min(" + S(1) + ", " + S(2) + ")"; break;
        case OP_MAX: expr = "max(" + S(1) + ", " + S(2) + ")"; break;
        case OP_SLT: expr = "float4((" + S(1) + ") < (" + S(2) + "))"; break;
        case OP_SGE: expr = "float4((" + S(1) + ") >= (" + S(2) + "))"; break;
        case OP_RCP: expr = "rcp_d3d((" + S(1) + ").x)"; break;
        case OP_RSQ: expr = "rsq_d3d((" + S(1) + ").x)"; break;
        case OP_EXP: expr = "exp2((" + S(1) + ").x).xxxx"; break;
        case OP_LOG: expr = "log_d3d((" + S(1) + ").x)"; break;
        case OP_EXPP: expr = "expp_d3d((" + S(1) + ").x)"; break;
        case OP_LOGP: expr = "logp_d3d((" + S(1) + ").x)"; break;
        case OP_LIT: expr = "lit_d3d(" + S(1) + ")"; break;
        case OP_DST: expr = "float4(1.0, (" + S(1) + ").y * (" + S(2) + ").y, (" + S(1) + ").z, (" + S(2) + ").w)"; break;
        case OP_FRC: expr = "frac(" + S(1) + ")"; break;
        case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: {
            if (in.op == OP_M4x4 && RegTypeOf(p[2]) == RT_CONST && RegNumOf(p[2]) == 0) {
                if (grass && RegTypeOf(p[1]) == RT_TEMP) {
                    const std::string reg = VSRegister(p[1], 0);
                    s += "  if (grassParams.x > 0) " + reg + " = GrassPush(" + reg + ", v0.y);\n";
                }
                s += "  worldPosition = " + S(1) + ";\n";
            }
            int rows = (in.op == OP_M4x4 || in.op == OP_M3x4) ? 4 : (in.op == OP_M3x2 ? 2 : 3);
            bool dp4 = in.op == OP_M4x4 || in.op == OP_M4x3;
            expr = "float4(";
            for (int r = 0; r < 4; r++) {
                if (r) expr += ", ";
                if (r >= rows) { expr += "0"; continue; }
                expr += dp4 ? "dot(" + S(1) + ", " + S(2, r) + ")"
                            : "dot((" + S(1) + ").xyz, (" + S(2, r) + ").xyz)";
            }
            expr += ")";
            break;
        }
        default:
            error = Fmt("unsupported vs opcode %lu", in.op);
            return "";
        }
        std::string dst = VSRegister(p[0], 0);
        std::string mask = MaskOf(p[0]);
        s += "  " + dst + mask + " = (" + expr + ")" + (mask.empty() ? "" : mask) + ";\n";
    }

    s += "  VSOut o;\n";
    s += "  o.pos = oPos;\n";
    s += "  if (sunFlags.x > 0.5 || sunFlags.y > 0.5) {\n";
    s += "    float4 worldH = float4(dot(oPos,clipToWorld[0]),dot(oPos,clipToWorld[1]),dot(oPos,clipToWorld[2]),dot(oPos,clipToWorld[3]));\n";
    s += "    if (abs(worldH.w) > 1e-6) worldPosition = float4(worldH.xyz/worldH.w,1);\n";
    s += "  }\n";
    s += "  o.world = worldPosition.xyz;\n";
    s += "  if(waterParams.x>0.5) {\n";
    s += "    float4 p=float4(dot(oPos,waterClip[0]),dot(oPos,waterClip[1]),dot(oPos,waterClip[2]),dot(oPos,waterClip[3]));\n";
    s += "    if(abs(p.w)>1e-8) o.world=p.xyz/p.w;\n";
    s += "    float4 swell=WaterSwellAt(o.world);o.world.y-=swell.x;\n";
    s += "    if(waterSwell.x>0) {float4 q=float4(o.world,1);\n";
    s += "      o.pos=float4(dot(q,waterProject[0]),dot(q,waterProject[1]),dot(q,waterProject[2]),dot(q,waterProject[3]));oPos=o.pos;}\n";
    s += "  }\n";
    s += "  o.normal = 0;\n";
    if (WorldShaderKind(decl) == 1) {
        // r6 is the skinned normal in the coordinate system consumed by c0.
        // Transform its inverse transpose into the shared terrain world space.
        // Also needed by the tessellation stages (sunControl.z) to curve the surface.
        s += "  if ((sunControl.x > 0.5 || sunControl.z > 0.5) && sunFlags.x > 0.5) {\n";
        s += "    float3 basis[3];\n";
        s += "    [unroll] for(int a=0;a<3;a++) {\n";
        s += "      float4 clipAxis=float4(c[0][a],c[1][a],c[2][a],c[3][a]);\n";
        s += "      basis[a]=float3(dot(clipAxis,clipToWorld[0]),dot(clipAxis,clipToWorld[1]),dot(clipAxis,clipToWorld[2]));\n";
        s += "    }\n";
        s += "    float det=dot(basis[0],cross(basis[1],basis[2]));\n";
        s += "    if(abs(det)>1e-8) o.normal=(r6.x*cross(basis[1],basis[2])+r6.y*cross(basis[2],basis[0])+r6.z*cross(basis[0],basis[1]))/det;\n";
        s += "  }\n";
    }
    s += "  if (sunFlags.y > 0.5) o.pos = float4(dot(worldPosition,sunWrite[0]),dot(worldPosition,sunWrite[1]),dot(worldPosition,sunWrite[2]),dot(worldPosition,sunWrite[3]));\n";
    s += "  if (sunFlags.y < 0.5) o.pos.xy += pixelOffset.xy * oPos.w;\n";
    s += "  if(sunFlags.y<0.5 && (sunFlags.x>0.5 || waterParams.x>0.5)) o.pos.xy+=pixelOffset.zw*oPos.w;\n";
    s += "  o.d0 = saturate(od0);\n  o.d1 = saturate(od1);\n";
    s += "  o.t0 = ot0; o.t1 = ot1; o.t2 = ot2; o.t3 = ot3; o.t4 = ot4; o.t5 = ot5; o.t6 = ot6; o.t7 = ot7;\n";
    // Terrain: r9 = v0.w * c7 + c6 is the game's light; v0.w is its baked sun factor (relighting).
    if (WorldShaderKind(decl) == 2) s += "  o.t7 = float4(v0.w, 0, 0, 1);\n";
    s += "  o.fog = oFog.x;\n";
    s += "  return o;\n}\n";
    return s;
}

// ---- pixel shaders ----------------------------------------------------------------------------

namespace {

const char* kPSCommon =
    "cbuffer PSConstants : register(b0) {\n"
    "  float4 c[8];\n"
    "  float4 textureFactor;\n"
    "  float4 alphaRef;\n"
    "  float4 bumpMat[8];\n"
    "  float4 bumpLum[8];\n"
    "  float4 fogColor;\n"
    "  float4 fogParams;\n"
    "};\n";

std::string TextureDecls(const BYTE* dims)
{
    std::string s;
    for (int i = 0; i < kMaxStages; i++) {
        if (dims[i] == 2) s += Fmt("Texture2D tex%d : register(t%d);\n", i, i);
        else if (dims[i] == 3) s += Fmt("Texture3D tex%d : register(t%d);\n", i, i);
        if (dims[i]) s += Fmt("SamplerState smp%d : register(s%d);\n", i, i);
    }
    return s;
}

std::string SampleExpr(int stage, const BYTE* dims, const std::string& coord)
{
    if (dims[stage] == 2) return Fmt("tex%d.Sample(smp%d, (", stage, stage) + coord + ").xy)";
    if (dims[stage] == 3) return Fmt("tex%d.Sample(smp%d, (", stage, stage) + coord + ").xyz)";
    return "float4(0, 0, 0, 1)";
}

std::string AlphaTest(BYTE func, BYTE a2c = 0)
{
    // D3DCMPFUNC: 1 NEVER, 2 LESS, 3 EQUAL, 4 LESSEQUAL, 5 GREATER, 6 NOTEQUAL, 7 GREATEREQUAL, 8 ALWAYS.
    // Compared at 8-bit precision like the hardware does.
    static const char* cmp[9] = { nullptr, "false", "a < r", "a == r", "a <= r", "a > r", "a != r", "a >= r", nullptr };
    if (func == 0 || func >= 8) return "";
    if (a2c && (func == 5 || func == 7))
        // Alpha-to-coverage: turn the test threshold into a one-pixel-wide ramp so MSAA coverage
        // gives foliage soft edges; fully opaque inside, nothing below the threshold.
        return "  { float ref = alphaRef.x / 255.0; float w = max(fwidth(color.a), 1e-4);\n"
               "    color.a = saturate((color.a - ref) / w + 0.5); if (color.a <= 0) discard; }\n";
    return std::string("  { int a = (int)round(saturate(color.a) * 255.0); int r = (int)alphaRef.x;\n")
         + "    if (!(" + cmp[func] + ")) discard; }\n";
}

std::string PSRegister(DWORD t)
{
    int type = RegTypeOf(t), n = RegNumOf(t);
    switch (type) {
    case RT_TEMP: return Fmt("r%d", n);
    case RT_INPUT: return Fmt("v%d", n);
    case RT_CONST: return Fmt("cp[%d]", n);
    case RT_ADDR_TEX: return Fmt("t%d", n);
    default: return "float4(0, 0, 0, 0)";
    }
}

std::string PSSource(DWORD t) { return ApplySourceModifier(PSRegister(t) + SwizzleOf(t), t); }

std::string DestModifiers(DWORD t, const std::string& v)
{
    std::string r = v;
    switch ((t >> 24) & 0xF) {
    case 1: r = "(" + r + " * 2.0)"; break;
    case 2: r = "(" + r + " * 4.0)"; break;
    case 3: r = "(" + r + " * 8.0)"; break;
    case 15: r = "(" + r + " * 0.5)"; break;
    case 14: r = "(" + r + " * 0.25)"; break;
    case 13: r = "(" + r + " * 0.125)"; break;
    default: break;
    }
    if ((t >> 20) & 1) r = "saturate(" + r + ")";
    return r;
}

} // namespace

bool IsWaterPixelShader(const DWORD* func)
{
    std::vector<Instr> code;
    DWORD minor; std::string error;
    if (!func || (func[0]&0xffff0000)!=0xffff0000 || !Decode(func,true,code,minor,error)) return false;
    bool pad=false, reflect=false, bump=false;
    for(const auto& i:code) {
        if(i.op==OP_TEX && RegNumOf(i.p[0])==0) bump=true;
        if(i.op==OP_TEXM3x2PAD && RegNumOf(i.p[0])==1) pad=true;
        if(i.op==OP_TEXM3x2TEX && RegNumOf(i.p[0])==2) reflect=true;
    }
    return bump && pad && reflect;
}

bool PixelShaderSamplesStage(const DWORD* func, int stage)
{
    std::vector<Instr> code;DWORD minor;std::string error;
    if(!func || !Decode(func,true,code,minor,error))return false;
    for(const auto& i:code)if(i.op==OP_TEX && !i.p.empty() && RegNumOf(i.p[0])==stage)return true;
    return false;
}

std::string TranslatePS(const DWORD* func, const PSVariant& v, std::string& error)
{
    if ((func[0] & 0xFFFF0000) != 0xFFFF0000) { error = "not a pixel shader"; return ""; }
    std::vector<Instr> code;
    DWORD minor;
    if (!Decode(func, true, code, minor, error)) return "";
    if (minor >= 4) { error = "ps_1_4 is not supported yet"; return ""; }

    std::string s;
    s += kPSCommon;
    s += TextureDecls(v.texDim);
    s += kVSOutputStruct;
    s += SunPixelCode();
    if (v.water) s += WaterPixelCode(v.waterSamples);
    if (v.normalMap && v.texDim[v.material?v.material-1:0] == 2) s += NormalMapCode();
    if(v.material)s+=MaterialCode(v.normalMap && v.texDim[v.material-1]==2,v.material-1);
    const bool relief = v.relief && !v.shadowOnly && v.texDim[0] == 2;
    if (relief) s += ReliefCode();
    s += v.shadowOnly ? "void main(VSOut i) {\n" : "float4 main(VSOut i) : SV_Target {\n";
    s += relief ? "  float reliefShadow = ReliefApply(i);\n" : "";
    s += "  float4 cp[8] = c;\n";
    for (const Instr& in : code)
        if (in.op == OP_DEF)
            s += Fmt("  cp[%d] = float4(%.9g, %.9g, %.9g, %.9g);\n", RegNumOf(in.p[0]), AsFloat(in.p[1]),
                     AsFloat(in.p[2]), AsFloat(in.p[3]), AsFloat(in.p[4]));
    s += "  float4 v0 = i.d0, v1 = i.d1;\n";
    s += v.material ? "  bool relight = sunControl.x > 0.5 && sunFlags.x > 0.5 && sunFlags.y < 0.5;\n"
                    : "  bool relight = sunControl.x > 0.5 && sunFlags.x > 0.5 && sunFlags.y < 0.5 && (sunFlags.w == 1 || sunFlags.w == 2 || (sunFlags.w == 3 && sunFlags.z > 0.5));\n";
    s += "  if (relight) { v0.rgb = 1; v1.rgb = 0; }\n";
    s += "  float4 r0 = 0, r1 = 0, t0 = 0, t1 = 0, t2 = 0, t3 = 0;\n";
    s += "  float m0 = 0, m1 = 0;\n";

    // Results of a co-issued pair must be computed from the registers before either write.
    std::vector<std::string> pendingWrites;
    int tmp = 0;
    auto flush = [&]() { for (auto& w : pendingWrites) s += w; pendingWrites.clear(); };
    int padCount = 0;
    bool deferredFog = false;

    for (size_t k = 0; k < code.size(); k++) {
        const Instr& in = code[k];
        const std::vector<DWORD>& p = in.p;
        if (!in.coissue) flush();
        if (in.op == OP_DEF || in.op == OP_NOP || in.op == OP_PHASE) continue;

        if (in.op >= OP_TEXCOORD && in.op <= OP_TEXDEPTH && in.op != OP_EXPP && in.op != OP_LOGP &&
            in.op != OP_CND && in.op != OP_DEF) {
            int n = RegNumOf(p[0]);
            std::string coord = Fmt("i.t%d", n);
            switch (in.op) {
            case OP_TEX:
                s += Fmt("  t%d = ", n) + SampleExpr(n, v.texDim, coord) + ";\n";
                // Also use the same continuous wave field for the original
                // texm3x2 reflection lookup, not just our Fresnel/refraction.
                if(v.water && n==0) s += "  t0=WaterWave(i);\n";
                // Grass t1 contains terrain material colour as well as legacy
                // lighting. Whitening it removes the green tint of the blades.
                // Preserve it until those two components can be separated.
                break;
            case OP_TEXCOORD: s += Fmt("  t%d = float4(saturate(i.t%d.xyz), 1.0);\n", n, n); break;
            case OP_TEXKILL: s += "  if (any(" + coord + ".xyz < 0.0)) discard;\n"; break;
            case OP_TEXBEM: case OP_TEXBEML: {
                std::string src = PSSource(p[1]);
                s += Fmt("  { float4 b = %s; float2 uv = i.t%d.xy + float2(bumpMat[%d].x * b.x + bumpMat[%d].z * b.y,"
                         " bumpMat[%d].y * b.x + bumpMat[%d].w * b.y);\n", src.c_str(), n, n, n, n, n);
                s += Fmt("    t%d = ", n) + SampleExpr(n, v.texDim, "float4(uv, 0, 0)") + ";\n";
                if (in.op == OP_TEXBEML)
                    s += Fmt("    t%d.rgb *= saturate(b.z * bumpLum[%d].x + bumpLum[%d].y);\n", n, n, n);
                s += "  }\n";
                break;
            }
            case OP_TEXREG2AR: s += Fmt("  t%d = ", n) + SampleExpr(n, v.texDim, "float4((" + PSSource(p[1]) + ").wx, 0, 0)") + ";\n"; break;
            case OP_TEXREG2GB: s += Fmt("  t%d = ", n) + SampleExpr(n, v.texDim, "float4((" + PSSource(p[1]) + ").yz, 0, 0)") + ";\n"; break;
            case OP_TEXREG2RGB: s += Fmt("  t%d = ", n) + SampleExpr(n, v.texDim, "float4((" + PSSource(p[1]) + ").xyz, 0)") + ";\n"; break;
            case OP_TEXM3x2PAD: case OP_TEXM3x3PAD:
                s += Fmt("  m%d = dot(i.t%d.xyz, (", padCount & 1, n) + PSSource(p[1]) + ").xyz);\n";
                padCount++;
                break;
            case OP_TEXM3x2TEX:
                s += Fmt("  t%d = ", n) + SampleExpr(n, v.texDim, Fmt("float4(m0, dot(i.t%d.xyz, (", n) + PSSource(p[1]) + ").xyz), 0, 0)") + ";\n";
                padCount = 0;
                break;
            case OP_TEXM3x2DEPTH:
                LogOnce("texm3x2depth", "ps: texm3x2depth not supported");
                padCount = 0;
                break;
            case OP_TEXM3x3TEX: case OP_TEXM3x3: case OP_TEXM3x3SPEC: case OP_TEXM3x3VSPEC: {
                std::string nrm = Fmt("float3(m0, m1, dot(i.t%d.xyz, (", n) + PSSource(p[1]) + ").xyz))";
                if (in.op == OP_TEXM3x3) { s += Fmt("  t%d = float4(", n) + nrm + ", 1.0);\n"; }
                else if (in.op == OP_TEXM3x3TEX) { s += Fmt("  t%d = ", n) + SampleExpr(n, v.texDim, "float4(" + nrm + ", 0)") + ";\n"; }
                else {
                    std::string eye = in.op == OP_TEXM3x3SPEC ? "(" + PSSource(p[2]) + ").xyz"
                                                             : Fmt("float3(i.t%d.w, i.t%d.w, i.t%d.w)", n - 2, n - 1, n);
                    s += "  { float3 N = " + nrm + "; float3 E = " + eye + ";\n";
                    s += "    float3 R = 2.0 * N * dot(N, E) / max(dot(N, N), 1e-8) - E;\n";
                    s += Fmt("    t%d = ", n) + SampleExpr(n, v.texDim, "float4(R, 0)") + "; }\n";
                }
                padCount = 0;
                break;
            }
            case OP_TEXDP3TEX: s += Fmt("  t%d = ", n) + SampleExpr(n, v.texDim, Fmt("float4(dot(i.t%d.xyz, (", n) + PSSource(p[1]) + ").xyz), 0, 0, 0)") + ";\n"; break;
            case OP_TEXDP3: s += Fmt("  t%d = dot(i.t%d.xyz, (", n, n) + PSSource(p[1]) + ").xyz).xxxx;\n"; break;
            default:
                LogOnce(Fmt("psop%lu", in.op), "ps: unsupported texture opcode %lu", in.op);
                break;
            }
            continue;
        }

        std::string expr;
        auto S = [&](int i) { return PSSource(p[i]); };
        switch (in.op) {
        case OP_MOV: expr = S(1); break;
        case OP_ADD: expr = S(1) + " + " + S(2); break;
        case OP_SUB: expr = S(1) + " - " + S(2); break;
        case OP_MUL: expr = S(1) + " * " + S(2); break;
        case OP_MAD: expr = S(1) + " * " + S(2) + " + " + S(3); break;
        case OP_LRP:
            expr = "lerp(" + S(3) + ", " + S(2) + ", " + S(1) + ")";
            // Captured world shaders use lrp r0.xyz, v1.w, v1, r0 for fog.
            // Keep it out of the albedo; apply it after our lighting instead.
            if (RegTypeOf(p[1]) == RT_INPUT && RegNumOf(p[1]) == 1 &&
                ((p[1] >> 16) & 255) == 255 && ((p[1] >> 24) & 15) == 0 &&
                RegTypeOf(p[2]) == RT_INPUT && RegNumOf(p[2]) == 1 &&
                MaskOf(p[0]) == ".xyz") {
                deferredFog = true;
                expr = "(relight ? " + S(3) + " : " + expr + ")";
            }
            break;
        case OP_DP3: expr = "dot((" + S(1) + ").xyz, (" + S(2) + ").xyz).xxxx"; break;
        case OP_DP4: expr = "dot(" + S(1) + ", " + S(2) + ").xxxx"; break;
        case OP_CND: expr = "((" + S(1) + ") > 0.5 ? (" + S(2) + ") : (" + S(3) + "))"; break;
        case OP_CMP: expr = "((" + S(1) + ") >= 0.0 ? (" + S(2) + ") : (" + S(3) + "))"; break;
        default:
            error = Fmt("unsupported ps opcode %lu", in.op);
            return "";
        }
        std::string name = Fmt("q%d", tmp++);
        s += "  float4 " + name + " = " + DestModifiers(p[0], "(" + expr + ")") + ";\n";
        std::string mask = MaskOf(p[0]);
        pendingWrites.push_back("  " + PSRegister(p[0]) + mask + " = " + name + mask + ";\n");
    }
    flush();
    s += "  float4 color = saturate(r0);\n";
    s += AlphaTest(v.alphaFunc, v.a2c);
    if (v.shadowOnly) s += "  return;\n}\n";
    else {
        if (v.water) s += "  color = ApplyWater(color,i,t0,t2.rgb);\n";
        if(v.material)s+="  color = ApplyMaterial(color,i);\n";
        else{
            if (v.normalMap && v.texDim[0] == 2) s += "  color = ApplyNormalMap(color,i);\n";
            s += "  color = ApplySun(color,i);\n";
        }
        if (relief) s += "  color.rgb *= reliefShadow;\n"
                         "  if (sunControl.y == 12) color.rgb = reliefShadow;\n";
        if (deferredFog)
            s += "  if (relight && sunControl.y == 0) color.rgb = lerp(color.rgb,i.d1.rgb,saturate(i.d1.w));\n";
        s += "  return color;\n}\n";
    }
    return s;
}

// ---- fixed-function texture stages -----------------------------------------------------------

namespace {

std::string FFArg(BYTE arg)
{
    std::string v;
    switch (arg & 0xF) {
    case 0: v = "diffuse"; break;  // D3DTA_DIFFUSE
    case 1: v = "cur"; break;      // D3DTA_CURRENT
    case 2: v = "tex"; break;      // D3DTA_TEXTURE
    case 3: v = "textureFactor"; break;
    case 4: v = "specular"; break;
    case 5: v = "temp"; break;
    default: v = "cur"; break;
    }
    if (arg & 0x20) v = v + ".aaaa";      // D3DTA_ALPHAREPLICATE
    if (arg & 0x10) v = "(1.0 - " + v + ")"; // D3DTA_COMPLEMENT
    return v;
}

std::string FFOp(BYTE op, BYTE a1, BYTE a2, BYTE a0)
{
    std::string x = FFArg(a1), y = FFArg(a2), z = FFArg(a0);
    switch (op) {
    case 2: return x;                                         // SELECTARG1
    case 3: return y;                                         // SELECTARG2
    case 4: return x + " * " + y;                             // MODULATE
    case 5: return "(" + x + " * " + y + ") * 2.0";           // MODULATE2X
    case 6: return "(" + x + " * " + y + ") * 4.0";           // MODULATE4X
    case 7: return x + " + " + y;                             // ADD
    case 8: return x + " + " + y + " - 0.5";                  // ADDSIGNED
    case 9: return "(" + x + " + " + y + " - 0.5) * 2.0";     // ADDSIGNED2X
    case 10: return x + " - " + y;                            // SUBTRACT
    case 11: return x + " + " + y + " - " + x + " * " + y;    // ADDSMOOTH
    case 12: return "lerp(" + y + ", " + x + ", diffuse.a)";  // BLENDDIFFUSEALPHA
    case 13: return "lerp(" + y + ", " + x + ", tex.a)";      // BLENDTEXTUREALPHA
    case 14: return "lerp(" + y + ", " + x + ", textureFactor.a)"; // BLENDFACTORALPHA
    case 15: return x + " + " + y + " * (1.0 - tex.a)";       // BLENDTEXTUREALPHAPM
    case 16: return "lerp(" + y + ", " + x + ", cur.a)";      // BLENDCURRENTALPHA
    case 18: return x + " + " + x + ".aaaa * " + y;           // MODULATEALPHA_ADDCOLOR
    case 19: return x + " * " + y + " + " + x + ".aaaa";      // MODULATECOLOR_ADDALPHA
    case 20: return x + " + (1.0 - " + x + ".aaaa) * " + y;   // MODULATEINVALPHA_ADDCOLOR
    case 21: return "(1.0 - " + x + ") * " + y + " + " + x + ".aaaa"; // MODULATEINVCOLOR_ADDALPHA
    case 24: return "saturate(dot(" + x + ".rgb * 2.0 - 1.0, " + y + ".rgb * 2.0 - 1.0)).xxxx"; // DOTPRODUCT3
    case 25: return z + " + " + x + " * " + y;                // MULTIPLYADD
    case 26: return "lerp(" + z + ", " + y + ", " + x + ")";  // LERP: arg1 * arg2 + (1 - arg1) * arg0
    default: return "cur";
    }
}

} // namespace

std::string GenerateFFPS(const FFKey& k)
{
    BYTE dims[kMaxStages] = {};
    for (int i = 0; i < kMaxStages; i++) dims[i] = k.stages[i].texDim;
    std::string s;
    s += kPSCommon;
    s += TextureDecls(dims);
    s += kVSOutputStruct;
    s += SunPixelCode();
    if (k.normalMap && dims[k.material?k.material-1:0] == 2) s += NormalMapCode();
    if(k.material)s+=MaterialCode(k.normalMap && dims[k.material-1]==2,k.material-1);
    s += k.shadowOnly ? "void main(VSOut i) {\n" : "float4 main(VSOut i) : SV_Target {\n";
    s += "  float4 diffuse = i.d0, specular = i.d1, cur = i.d0, temp = 0, tex = 1;\n";
    s += "  if (sunControl.x > 0.5 && sunFlags.x > 0.5 && sunFlags.y < 0.5) { diffuse.rgb=1; specular.rgb=0; cur.rgb=1; }\n";
    s += "  float2 bump = 0; float bumpL = 1.0;\n";
    s += "  float4 tc[8] = { i.t0, i.t1, i.t2, i.t3, i.t4, i.t5, i.t6, i.t7 };\n";
    for (int st = 0; st < kMaxStages; st++) {
        const FFStageKey& g = k.stages[st];
        if (g.colorOp == 1 || g.colorOp == 0) break; // D3DTOP_DISABLE ends the cascade
        s += Fmt("  // stage %d\n  {\n", st);
        std::string coord = Fmt("tc[%d]", g.texCoordIndex & 7);
        if (g.projected) coord = "(" + coord + " / " + coord + ".w)";
        if (st > 0 && (k.stages[st - 1].colorOp == 22 || k.stages[st - 1].colorOp == 23))
            coord = "float4(" + coord + ".xy + bump, " + coord + ".zw)";
        if (g.texDim) s += "    tex = " + SampleExpr(st, dims, coord) + ";\n";
        else s += "    tex = float4(1, 1, 1, 1);\n";
        if (st > 0 && k.stages[st - 1].colorOp == 23) s += "    tex.rgb *= bumpL;\n";
        if (g.colorOp == 22 || g.colorOp == 23) {
            // BUMPENVMAP(LUMINANCE): this stage perturbs the next stage's coordinates; colour passes through.
            s += Fmt("    bump = float2(bumpMat[%d].x * tex.x + bumpMat[%d].z * tex.y, bumpMat[%d].y * tex.x + bumpMat[%d].w * tex.y);\n", st, st, st, st);
            s += Fmt("    bumpL = saturate(tex.z * bumpLum[%d].x + bumpLum[%d].y);\n", st, st);
            s += "  }\n";
            continue;
        }
        s += "    float3 rgb = (" + FFOp(g.colorOp, g.colorArg1, g.colorArg2, g.colorArg0) + ").rgb;\n";
        if (g.alphaOp == 1 || g.alphaOp == 0) s += "    float a = cur.a;\n";
        else s += "    float a = (" + FFOp(g.alphaOp, g.alphaArg1, g.alphaArg2, g.alphaArg0) + ").a;\n";
        s += "    cur = saturate(float4(rgb, a));\n  }\n";
    }
    s += "  float4 color = cur;\n";
    if (k.specular) s += "  color.rgb = saturate(color.rgb + specular.rgb);\n";
    s += AlphaTest(k.alphaFunc, k.a2c);
    if (!k.shadowOnly && !k.material && k.normalMap && k.stages[0].texDim == 2) s += "  color = ApplyNormalMap(color,i);\n";
    s += k.shadowOnly ? "  return;\n}\n" : k.material ? "  return ApplyMaterial(color,i);\n}\n":"  return ApplySun(color,i);\n}\n";
    return s;
}

// ---- tessellation -------------------------------------------------------------------------------
// PN triangles (Vlachos et al. 2001) between the translated VS and PS. The VS already outputs the
// shared world position and, with sunControl.z set, the skinned world normal. Edge factors come
// from the edge's own screen length, so neighbouring patches always agree. The clip position is
// interpolated from the corners (keeping the game's projection and jitter) plus the curvature
// offset taken through the camera view-projection.

namespace {
const char* kTessCommon =
    "cbuffer Tess : register(b5) { float4 tessViewProj[4]; float4 tessParams; float4 tessScreen; float4 tessDisplace; };\n"
    "struct PatchConst {\n"
    "  float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor;\n"
    "  float3 b210 : B210; float3 b120 : B120; float3 b021 : B021;\n"
    "  float3 b012 : B012; float3 b102 : B102; float3 b201 : B201; float3 b111 : B111;\n"
    "  float3 n110 : N110; float3 n011 : N011; float3 n101 : N101;\n"
    "};\n";
}

std::string TessellationHS(bool flipWinding)
{
    std::string s = kVSOutputStruct;
    s += kTessCommon;
    s +=
        "float3 SafeNormal(float3 n) { float l = dot(n, n); return l > 1e-12 ? n * rsqrt(l) : 0; }\n"
        // Control point near pi on edge pi-pj, projected onto the tangent plane at pi.
        "float3 EdgePoint(float3 pi, float3 pj, float3 ni) { return (2 * pi + pj - dot(pj - pi, ni) * tessParams.z * ni) / 3; }\n"
        "float2 ToPixels(float4 p) { return p.xy / p.w * tessScreen.xy * 0.5; }\n"
        // Mid-edge normal of the quadratic normal patch, reflected across the edge's mid-plane.
        "float3 EdgeNormal(float3 pi, float3 pj, float3 ni, float3 nj) {\n"
        "  float3 d = pj - pi; float dd = dot(d, d);\n"
        "  float k = dd > 1e-12 ? 2 * dot(d, ni + nj) / dd : 0;\n"
        "  return SafeNormal(ni + nj - k * d); }\n"
        "float EdgeFactor(float4 a, float4 b) {\n"
        "  if (a.w <= 1e-4 || b.w <= 1e-4) return 1;\n"
        "  return clamp(length(ToPixels(a) - ToPixels(b)) / tessParams.y, 1, tessParams.x); }\n"
        // Patches carry 6 points: the triangle and, for each corner, the first vertex of the draw at
        // the same bind-pose position (tessellation.cpp). Split vertices (UV seams, hard edges) then
        // curve shared edges from the same data on both sides, so no cracks open between them.
        "VSOut Weld(InputPatch<VSOut, 6> p, uint i) {\n"
        "  VSOut v = p[i];\n"
        "  float3 d = p[i + 3].world - v.world;\n"
        "  if (tessDisplace.w > 0.5 && dot(d, d) < 1e-4) {\n"
        "    v.world = p[i + 3].world; v.normal = p[i + 3].normal;\n"
        "    v.pos += float4(dot(d, tessViewProj[0].xyz), dot(d, tessViewProj[1].xyz), dot(d, tessViewProj[2].xyz), dot(d, tessViewProj[3].xyz));\n"
        "  }\n"
        "  return v; }\n"
        "PatchConst Constants(InputPatch<VSOut, 6> ip) {\n"
        "  PatchConst o;\n"
        "  VSOut p[3] = { Weld(ip, 0), Weld(ip, 1), Weld(ip, 2) };\n"
        // Locals, not o.edges[], for the inside factor: reading the outputs back trips fxc.
        "  float e0 = EdgeFactor(p[1].pos, p[2].pos), e1 = EdgeFactor(p[2].pos, p[0].pos), e2 = EdgeFactor(p[0].pos, p[1].pos);\n"
        "  o.edges[0] = e0; o.edges[1] = e1; o.edges[2] = e2;\n"
        "  o.inside = (e0 + e1 + e2) / 3;\n"
        "  float3 P0 = p[0].world, P1 = p[1].world, P2 = p[2].world;\n"
        "  float3 N0 = SafeNormal(p[0].normal), N1 = SafeNormal(p[1].normal), N2 = SafeNormal(p[2].normal);\n"
        "  o.b210 = EdgePoint(P0, P1, N0); o.b120 = EdgePoint(P1, P0, N1);\n"
        "  o.b021 = EdgePoint(P1, P2, N1); o.b012 = EdgePoint(P2, P1, N2);\n"
        "  o.b102 = EdgePoint(P2, P0, N2); o.b201 = EdgePoint(P0, P2, N0);\n"
        "  float3 E = (o.b210 + o.b120 + o.b021 + o.b012 + o.b102 + o.b201) / 6, V = (P0 + P1 + P2) / 3;\n"
        "  o.b111 = E + (E - V) / 2;\n"
        "  o.n110 = EdgeNormal(P0, P1, N0, N1); o.n011 = EdgeNormal(P1, P2, N1, N2); o.n101 = EdgeNormal(P2, P0, N2, N0);\n"
        "  return o; }\n";
    s += std::string("[domain(\"tri\")] [partitioning(\"fractional_odd\")] [outputtopology(\"") +
         (flipWinding ? "triangle_ccw" : "triangle_cw") + "\")]\n";
    s += "[outputcontrolpoints(3)] [patchconstantfunc(\"Constants\")] [maxtessfactor(16.0)]\n"
         "VSOut main(InputPatch<VSOut, 6> p, uint i : SV_OutputControlPointID) { return Weld(p, i); }\n";
    return s;
}

std::string TessellationDS()
{
    std::string s = kVSOutputStruct;
    s += kTessCommon;
    s +=
        "Texture2D heightTex : register(t0); SamplerState heightSmp : register(s0);\n"
        "float3 SafeNormalDS(float3 n) { float l = dot(n, n); return l > 1e-12 ? n * rsqrt(l) : 0; }\n"
        "[domain(\"tri\")]\n"
        "VSOut main(PatchConst c, float3 b : SV_DomainLocation, const OutputPatch<VSOut, 3> p) {\n"
        "  float u = b.x, v = b.y, w = b.z;\n"
        "  VSOut o;\n"
        "#define BARY(f) (p[0].f * u + p[1].f * v + p[2].f * w)\n"
        "  o.pos = BARY(pos); o.d0 = BARY(d0); o.d1 = BARY(d1);\n"
        "  o.t0 = BARY(t0); o.t1 = BARY(t1); o.t2 = BARY(t2); o.t3 = BARY(t3);\n"
        "  o.t4 = BARY(t4); o.t5 = BARY(t5); o.t6 = BARY(t6); o.t7 = BARY(t7);\n"
        "  o.fog = BARY(fog); o.normal = BARY(normal); o.world = BARY(world);\n"
        "  float3 pn = p[0].world * u * u * u + p[1].world * v * v * v + p[2].world * w * w * w\n"
        "    + c.b210 * (3 * u * u * v) + c.b120 * (3 * u * v * v) + c.b201 * (3 * u * u * w)\n"
        "    + c.b021 * (3 * v * v * w) + c.b102 * (3 * u * w * w) + c.b012 * (3 * v * w * w)\n"
        "    + c.b111 * (6 * u * v * w);\n"
        "  float3 n = SafeNormalDS(SafeNormalDS(p[0].normal) * u * u + SafeNormalDS(p[1].normal) * v * v + SafeNormalDS(p[2].normal) * w * w\n"
        "    + c.n110 * u * v + c.n011 * v * w + c.n101 * u * w);\n"
        "  if (dot(n, n) > 0) o.normal = n;\n"
        // tessDisplace: x world height scale (signed, 0 = off), y detail mip, z seam fade.
        "  if (tessDisplace.x != 0 && dot(n, n) > 0) {\n"
        "    const float3 luma = float3(0.299, 0.587, 0.114);\n"
        "    float fine = dot(heightTex.SampleLevel(heightSmp, o.t0.xy, tessDisplace.y).rgb, luma);\n"
        "    float coarse = dot(heightTex.SampleLevel(heightSmp, o.t0.xy, tessDisplace.y + 4).rgb, luma);\n"
        "    float h = clamp((fine - coarse) * 2, -1, 1);\n"
        // Triangles meeting at a UV seam read different heights: keep the original edges on the
        // PN surface so no cracks open there.
        "    float seam = tessDisplace.z > 0.5 ? saturate(min(u, min(v, w)) * 6) : 1;\n"
        "    pn += n * (h * tessDisplace.x * seam);\n"
        "  }\n"
        "  float4 d = float4(pn - o.world, 0);\n"
        "  o.world = pn;\n"
        "  o.pos += float4(dot(d, tessViewProj[0]), dot(d, tessViewProj[1]), dot(d, tessViewProj[2]), dot(d, tessViewProj[3]));\n"
        "  return o; }\n";
    return s;
}

// ---- compilation --------------------------------------------------------------------------------

typedef HRESULT(WINAPI* D3DCompile_t)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*,
                                       ID3DInclude*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);

ComPtr<ID3DBlob> CompileHLSL(const std::string& source, const char* profile, const std::string& debugName, bool optimize)
{
    static D3DCompile_t compile = nullptr;
    if (!compile) {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        if (m) compile = (D3DCompile_t)GetProcAddress(m, "D3DCompile");
        if (!compile) { Log("ERROR: d3dcompiler_47.dll not available"); return nullptr; }
    }
    if (g_config.dumpShaders) {
        std::string dir = g_gameDir + "d3d8to11_shaders\\";
        CreateDirectoryA(dir.c_str(), nullptr);
        FILE* f = nullptr;
        if (fopen_s(&f, (dir + debugName + ".hlsl").c_str(), "w") == 0 && f) { fputs(source.c_str(), f); fclose(f); }
    }
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = compile(source.data(), source.size(), debugName.c_str(), nullptr, nullptr, "main", profile,
                         optimize ? D3DCOMPILE_OPTIMIZATION_LEVEL3 : D3DCOMPILE_SKIP_OPTIMIZATION, 0, &code, &errors);
    if (FAILED(hr)) {
        Log("ERROR compiling %s (%s):\n%s\n--- source ---\n%s", debugName.c_str(), profile,
            errors ? (const char*)errors->GetBufferPointer() : "?", source.c_str());
        return nullptr;
    }
    return code;
}
