// shaders.h - D3D8 shader translation (vs_1_1 / ps_1_x bytecode -> HLSL) and the fixed-function
// pixel pipeline generator.
#pragma once
#include "common.h"

// One vertex input register declared by a D3D8 vertex declaration.
struct VSInput {
    int reg;       // v#
    int type;      // D3DVSDT_* (0 FLOAT1 .. 3 FLOAT4, 4 D3DCOLOR, 5 UBYTE4, 6 SHORT2, 7 SHORT4)
    int stream;
    UINT offset;   // byte offset inside the stream's vertex
};

struct VSDecl {
    std::vector<VSInput> inputs;
    std::vector<std::pair<UINT, std::vector<float>>> constants; // D3DVSD_CONST blocks: start register, values
    UINT streamMask = 0;
};

// Parses a D3D8 vertex declaration. Returns the number of DWORD tokens including the end token.
size_t ParseVSDecl(const DWORD* decl, VSDecl& out);
// Number of DWORD tokens of a shader function including the end token (0x0000FFFF).
size_t ShaderTokenCount(const DWORD* func);

// Common interpolator layout between every generated VS and PS.
extern const char* kVSOutputStruct;

std::string TranslateVS(const DWORD* func, const VSDecl& decl, std::string& error);
// Recognized main-world geometry; screen-space/UI shaders remain unlit.
int WorldShaderKind(const VSDecl& decl);

// Pixel shader variant: texture dimension per stage (0 = none, 2 = 2D, 3 = volume) and alpha test.
struct PSVariant {
    BYTE texDim[kMaxStages] = {};
    BYTE alphaFunc = 0; // 0 = alpha test off, otherwise D3DCMPFUNC
    BYTE shadowOnly = 0;
    bool operator==(const PSVariant& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};
std::string TranslatePS(const DWORD* func, const PSVariant& variant, std::string& error);

// Fixed-function texture stage pipeline key.
struct FFStageKey {
    BYTE colorOp, colorArg1, colorArg2, colorArg0;
    BYTE alphaOp, alphaArg1, alphaArg2, alphaArg0;
    BYTE texCoordIndex, texDim, projected, pad;
};
struct FFKey {
    FFStageKey stages[kMaxStages];
    BYTE alphaFunc;
    BYTE specular;
    BYTE shadowOnly;
    BYTE pad;
};
std::string GenerateFFPS(const FFKey& key);

// HLSL compilation (d3dcompiler_47.dll is loaded dynamically).
ComPtr<ID3DBlob> CompileHLSL(const std::string& source, const char* profile, const std::string& debugName);

// Pixel shader constant buffer layout (b0), shared by translated and fixed-function shaders.
struct PSConstants {
    float c[kMaxPSConst][4];
    float textureFactor[4];
    float alphaRef[4];
    float bumpMat[kMaxStages][4];   // m00, m01, m10, m11
    float bumpLum[kMaxStages][4];   // scale, offset
    float fogColor[4];
    float fogParams[4];             // start, end, 1/(end-start), enabled
};

struct VSExtraConstants {
    float pixelOffset[4];           // xy: D3D9-style half pixel correction in clip space
};

struct SunConstants {
    float readRows[4][4];
    float writeRows[4][4];
    float clipToWorld[4][4]; // inverse of the terrain's shared view-projection
    float direction[4];
    float settings[4]; // ambient, direct, shadow strength, inverse map size
    float flags[4];    // lighting enabled, shadow pass, previous map ready, normal kind
};
