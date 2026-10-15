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

// pulled: the batched terrain variant (terrain_batch.cpp). Vertices are read from the batch pools
// by SV_VertexID/SV_InstanceID instead of the input assembler, and c4/c5 come from the section.
std::string TranslateVS(const DWORD* func, const VSDecl& decl, std::string& error, bool pulled = false);
// Constant registers c0..n-1 the translated shader can read: every directly addressed source
// plus three more rows (matrix instructions read up to four consecutive registers), at least
// c0..c3 (our clip-axis code reads them). 256 when the shader uses relative addressing.
UINT VSConstantRegisters(const DWORD* func);
// Recognized main-world geometry; screen-space/UI shaders remain unlit.
int WorldShaderKind(const VSDecl& decl);
bool IsWaterPixelShader(const DWORD* func); // captured bump-reflection path, not handles
bool PixelShaderSamplesStage(const DWORD* func, int stage);

// Pixel shader variant: texture dimension per stage (0 = none, 2 = 2D, 3 = volume) and alpha test.
struct PSVariant {
    BYTE texDim[kMaxStages] = {};
    BYTE alphaFunc = 0; // 0 = alpha test off, otherwise D3DCMPFUNC
    BYTE shadowOnly = 0;
    BYTE a2c = 0;       // alpha-to-coverage instead of the alpha test discard
    BYTE normalMap = 0; // stage 0 texture has a normal map bound at t9
    BYTE water = 0;
    BYTE waterSamples = 0; // copied scene depth: 0 single sample, otherwise 2/4/8
    BYTE material=0;
    BYTE relief = 0;    // stage-0 skin parallax + self-shadowing (cbuffer b4)
    BYTE emissive = 0;  // stage-0 night emission mask at t8 (emissiveGlow)
    BYTE roughMap = 0;  // stage-0 roughness map at t16 (relit dinosaurs)
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
    BYTE a2c;
    BYTE normalMap;
    BYTE material;
    BYTE pad[2];
};
std::string GenerateFFPS(const FFKey& key);

// HLSL compilation (d3dcompiler_47.dll is loaded dynamically).
// PN-triangle hull/domain shaders for VSOut patches (see TessConstants).
std::string TessellationHS(bool flipWinding);
std::string TessellationDS();
struct TessConstants {
    float viewProj[4][4]; // camera world -> clip rows (terrain c0..c3)
    float params[4];      // max factor, target pixels per sub-edge, curvature, unused
    float screen[4];      // viewport width, height
    float displace[4];    // world relief scale (signed, 0 = off), detail mip, seam fade, weld split vertices
};
// optimize = false compiles with D3DCOMPILE_SKIP_OPTIMIZATION (see the tessellation hull shader).
ComPtr<ID3DBlob> CompileHLSL(const std::string& source, const char* profile, const std::string& debugName, bool optimize = true);

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

struct ReliefConstants {
    float params[4];  // depth (texels, signed), parallax 0..1, self-shadow 0..1, softness (texels)
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
    float control[4];  // replace original, debug view, tessellation normals, baked terrain detail
    float camera[4];   // camera world position, w = normal map strength
    float gameAmbient[4]; // the draw's own ambient light colour (gamma), w = highlight shoulder on
    float gameSun[4];     // the draw's own sun light colour (gamma), w = replacement lighting on
    float relightParams[4]; // sun scale, ambient scale, sky tint, exposure
    float terrainMap[4];    // height field ready, 1/width, 1/height, horizon shadow strength
    float todSun[4];        // time of day: sun/moon colour multiplier
    float todAmbient[4];    // time of day: ambient colour multiplier, w = rim light strength
    float todNight[4];      // time of day: tint for surfaces that are not relit, w = apply to this draw
    float rimColor[4];      // rim light colour (sun or moon)
    float fogTint[4];       // time of day: multiplier on the game's haze colour
    float grassGrade[4];    // grass with replacement lighting: saturation, brightness, rim glow, w = on
    float specParams[4];    // replacement specular: roughness, F0; rim: moon thin-line weight, line width
    float gameSunDir[4];    // the game's own sun direction (grass relighting reference)
    float emissiveGlow[4];  // night emission of the bound stage-0 texture (rgb, already scaled)
    float nightGrade[4];    // night colour fade: tint rgb, strength (world draws, before emission)
};

struct WaterConstants {
    float clipToWorld[4][4];
    float worldToClip[4][4];
    float reflection[4]; // SSR strength/enabled, distance, steps, reserved
    float regions[4];    // mask valid, forced/default ocean factor, inverse native grid W/H
    float profiles[4];   // lagoon foam, lagoon ripples, ocean ripples, native water height
    float swell[4];      // amplitude, wavelength scale, coast fade depth, crest foam
    float screen[4];  // inverse resolution, original clip half-pixel correction
    float params[4];  // active, absorption depth scale, foam width, foam strength
    float optical[4]; // reflection, transparency, distortion pixels, time
    float misc[4];    // debug mode, reserved
    float motion[4];  // ripple count, interaction strength, unscaled time, reserved
    float contacts[64][4]; // x, feet Y (downward positive), z, birth time
    float rings[64][4];    // amplitude, initial radius, expansion speed, lifetime
};
