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
std::string SunBlock(int slot) {
    return Fmt("cbuffer Sun : register(b%d) { float4 sunRead[4]; float4 sunWrite[4]; float4 clipToWorld[4]; float4 sunDirection; float4 sunSettings; float4 sunFlags; float4 sunControl; };\n", slot);
}
std::string SunPixelCode() {
    return SunBlock(1) +
        // Shadows are applied after the world pass (sun.cpp resolve). In-pass work is only the
        // optional replacement lighting ([Lighting] ReplaceOriginal=1); otherwise colour is untouched.
        "float4 ApplySun(float4 color, VSOut i) {\n"
        "  if (sunFlags.x < 0.5 || sunFlags.y > 0.5 || sunControl.x < 0.5) return color;\n"
        "  if (sunControl.y == 1) return color;\n"
        "  float3 rawN = cross(ddy(i.world), ddx(i.world));\n"
        "  if (dot(i.normal,i.normal) > 1e-8) rawN = i.normal;\n"
        "  float nlen = dot(rawN, rawN); if (nlen < 1e-12) return color;\n"
        "  float3 N = rawN * rsqrt(nlen);\n"
        "  if (sunFlags.w == 2 && N.y > 0) N = -N;\n" // JPOG is Y-down: terrain normals point to -Y
        "  if (sunControl.y == 2) return float4(N*0.5+0.5,color.a);\n"
        "  float ndl = saturate(dot(N, sunDirection.xyz));\n"
        "  if (sunFlags.w == 4) ndl = abs(dot(N,sunDirection.xyz));\n"
        "  float sky = saturate(-N.y*0.5+0.5);\n"
        "  float3 ambient = sunSettings.x * lerp(float3(0.38,0.34,0.27),float3(0.78,0.88,1.0),sky);\n"
        "  float3 light = ambient + sunSettings.y*ndl*float3(1.0,0.95,0.84);\n"
        "  float3 linearColor = pow(max(color.rgb,0),2.2);\n"
        "  return float4(pow(max(linearColor*light,0),1.0/2.2),color.a);\n"
        "}\n";
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
            if (in.op == OP_M4x4 && RegTypeOf(p[2]) == RT_CONST && RegNumOf(p[2]) == 0)
                s += "  worldPosition = " + S(1) + ";\n";
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
    s += "  o.normal = 0;\n";
    if (WorldShaderKind(decl) == 1) {
        // r6 is the skinned normal in the coordinate system consumed by c0.
        // Transform its inverse transpose into the shared terrain world space.
        s += "  if (sunControl.x > 0.5 && sunFlags.x > 0.5) {\n";
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
    s += "  o.d0 = saturate(od0);\n  o.d1 = saturate(od1);\n";
    s += "  o.t0 = ot0; o.t1 = ot1; o.t2 = ot2; o.t3 = ot3; o.t4 = ot4; o.t5 = ot5; o.t6 = ot6; o.t7 = ot7;\n";
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

std::string AlphaTest(BYTE func)
{
    // D3DCMPFUNC: 1 NEVER, 2 LESS, 3 EQUAL, 4 LESSEQUAL, 5 GREATER, 6 NOTEQUAL, 7 GREATEREQUAL, 8 ALWAYS.
    // Compared at 8-bit precision like the hardware does.
    static const char* cmp[9] = { nullptr, "false", "a < r", "a == r", "a <= r", "a > r", "a != r", "a >= r", nullptr };
    if (func == 0 || func >= 8) return "";
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
    s += v.shadowOnly ? "void main(VSOut i) {\n" : "float4 main(VSOut i) : SV_Target {\n";
    s += "  float4 cp[8] = c;\n";
    for (const Instr& in : code)
        if (in.op == OP_DEF)
            s += Fmt("  cp[%d] = float4(%.9g, %.9g, %.9g, %.9g);\n", RegNumOf(in.p[0]), AsFloat(in.p[1]),
                     AsFloat(in.p[2]), AsFloat(in.p[3]), AsFloat(in.p[4]));
    s += "  float4 v0 = i.d0, v1 = i.d1;\n";
    s += "  bool relight = sunControl.x > 0.5 && sunFlags.x > 0.5 && sunFlags.y < 0.5;\n";
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
                // Captured grass shader multiplies its blade texture by a
                // terrain colour/light texture. Retain that texture's alpha
                // but replace its RGB contribution with our own illumination.
                if (n == 1) s += "  if (relight && sunFlags.w == 4) t1.rgb = 1;\n";
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
    s += AlphaTest(v.alphaFunc);
    if (v.shadowOnly) s += "  return;\n}\n";
    else {
        s += "  color = ApplySun(color,i);\n";
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
    s += AlphaTest(k.alphaFunc);
    s += k.shadowOnly ? "  return;\n}\n" : "  return ApplySun(color,i);\n}\n";
    return s;
}

// ---- compilation --------------------------------------------------------------------------------

typedef HRESULT(WINAPI* D3DCompile_t)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*,
                                       ID3DInclude*, const char*, const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);

ComPtr<ID3DBlob> CompileHLSL(const std::string& source, const char* profile, const std::string& debugName)
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
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        Log("ERROR compiling %s (%s):\n%s\n--- source ---\n%s", debugName.c_str(), profile,
            errors ? (const char*)errors->GetBufferPointer() : "?", source.c_str());
        return nullptr;
    }
    return code;
}
