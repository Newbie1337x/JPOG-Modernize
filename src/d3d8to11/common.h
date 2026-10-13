// common.h - shared includes, logging, configuration and format helpers for d3d8to11.
#pragma once
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <unordered_map>
#include "d3d8_types.h"

using Microsoft::WRL::ComPtr;

// ---- logging -----------------------------------------------------------------------------------
void LogInit(const std::string& path);
void Log(const char* fmt, ...);
// Logs a message only the first time a given key is seen (for per-draw problems).
void LogOnce(const std::string& key, const char* fmt, ...);

// ---- configuration (d3d8to11.ini next to the game executable) ----------------------------------
struct Config {
    bool effectsEnabled = true; // master switch; individual effect settings are preserved
    bool useDgVoodoo = false;   // [Renderer] Backend=dgvoodoo forwards everything to D3D8_dgVoodoo.dll
    int syncInterval = 0;       // [Renderer] VSync
    int fpsLimit = 0;           // [Renderer] FPSLimit (0 = off)
    int forceAnisotropy = 16;   // [Renderer] Anisotropy (0/1 = use game setting)
    bool applyGamma = true;     // [Renderer] ApplyGammaRamp
    int msaa = 8;               // [Renderer] MSAA: 0/1 off, 2, 4, 8 samples (3D scene and HUD)
    bool alphaToCoverage = false; // [Renderer] AlphaToCoverage: smooth alpha-tested foliage under MSAA
    bool debugLayer = false;    // [Debug] D3D11DebugLayer
    bool dumpShaders = false;   // [Debug] DumpShaders (writes generated HLSL to d3d8to11_shaders\)
    bool sunLighting = true;        // [Lighting] Enabled: sun shadows
    bool replaceLighting = false;   // [Lighting] ReplaceOriginal: experimental per-pixel relighting
    bool sunFromGame = true;        // [Lighting] SunSource=game (else SunX/Y/Z)
    bool grassShadows = false;      // [Lighting] GrassShadows
    bool terrainShadows = false;    // [Lighting] TerrainShadows (hills shadowing the ground; acne-prone)
    int lightingDebug = 0;          // 0 off, 1 albedo, 2 normals (replacement), 3 shadow mask, 4 resolve normals
    int shadowResolution = 4096;
    float shadowDistance = 250.0f;  // farthest point from the camera that receives shadows
    float sunAmbient = 0.60f, sunDirect = 0.55f, shadowStrength = 0.25f;
    float sunMinElevation = 0.35f;  // minimum sun height (sin of elevation), avoids endless shadows
    float sunDirection[3] = { -0.35f, 0.82f, -0.45f };
    // [Post] sunlight and colour, applied to the world before the HUD (sun.cpp resolve pass)
    bool postEnabled = true;        // [Post] Enabled
    float sunBoost = 0.25f;         // [Post] SunBoost: extra light on sunlit surfaces
    float shadowCool = 0.5f;        // [Post] ShadowCool: blue tint in shadows
    float exposure = 1.0f;          // [Post] Exposure
    float contrast = 0.97f;         // [Post] Contrast
    float saturation = 1.09f;        // [Post] Saturation
    float warmth = 0.23f;           // [Post] Warmth (-1 cold .. 1 warm)
    // [Effects] ambient occlusion and volumetric light shafts (scene pass, before the HUD)
    bool aoEnabled = true;          // [Effects] AO
    float aoRadius = 2.0f;          // [Effects] AORadius (world units)
    float aoStrength = 0.7f;        // [Effects] AOStrength
    bool shafts = true;             // [Effects] LightShafts (needs sun shadows)
    float shaftStrength = 0.35f;    // [Effects] ShaftStrength
    float shaftScatter = 0.6f;      // [Effects] ShaftScatter: 0 even haze .. 0.9 tight glow toward the sun
    // [NormalMaps] relief from <texture>_n.dds files (loaded by the texture-replacement plugin)
    bool normalMaps = true;         // [NormalMaps] Enabled
    float normalStrength = 1.5f;    // [NormalMaps] Strength
    bool normalFlipGreen = false;   // [NormalMaps] FlipGreen (OpenGL-style maps)
    bool waterEnabled = true;       // [Water] depth-aware water, original waves/reflections
    float waterDepthScale = 6.0f, waterTransparency = 0.80f;
    float waterReflection = 0.85f, waterFoam = 0.35f, waterFoamWidth = 0.65f;
    float waterDistortion = 2.0f;    // refraction displacement in pixels
    float waterRipples = 1.0f, waterSpeed = 1.0f; // continuous world-space waves
    bool waterInteraction = true;
    bool waterSSR = true; // scene reflections from the frozen opaque snapshot
    float waterSSRStrength=1.0f, waterSSRDistance=80.0f;
    int waterSSRSteps=24;
    int waterSurfaceMode=0; // 0 native connectivity, 1 force lagoon, 2 force ocean
    float waterLagoonFoam=.03f,waterLagoonRipples=.25f,waterOceanRipples=1.4f;
    float waterSwellAmplitude=.65f,waterSwellLength=1.f,waterSwellCoastDepth=3.f,waterCrestFoam=.35f;
    bool performanceLog=true, performanceGPU=true;
    bool taa=true, fsr1=true;
    bool fsr31=true; // experimental native-resolution temporal AA, not frame generation
    bool frameGeneration=false; // synchronous x2 prototype; requires native AA + VSync
    bool frameGenerationDebug=false; // show SDK intermediate instead of extra Present
    bool materialsEnabled=true;
    float materialSkinRoughness=.58f,materialBuildingRoughness=.78f;
    float materialSpecular=.85f,materialEnvironment=.25f,materialNormalStrength=.65f;
    float taaHistory=.72f, fsrSharpness=.5f;
    float waterInteractionStrength = 1.0f;
    int waterDebug = 0;             // 0 shaded, 1 thickness, 2 Fresnel, 3 foam
    // [Grass] creatures bend and flatten the grass around them
    bool grassPhysics = true;       // [Grass] Enabled
    float grassPush = 0.8f;         // [Grass] Push: sideways bend, in blade heights
    float grassFlatten = 0.5f;      // [Grass] Flatten: 0..1 of the blade height pressed down
    float grassRadius = 1.0f;       // [Grass] RadiusScale: multiplies the radius taken from the skeleton
    int grassMinBones = 6;          // [Grass] MinBones: skinned draws with fewer bones (props, trees) do not push
    int overlayKey = VK_INSERT;     // [Overlay] Key: virtual-key code that opens the settings panel
    bool showFps = false;           // [Overlay] ShowFPS: small counter while the panel is closed
    float overlayScale = 1.0f;      // [Overlay] Scale
};
extern Config g_config;
extern std::string g_gameDir;
void LoadConfig();
void SaveConfig(); // writes every setting above back to d3d8to11.ini

// ---- formats -----------------------------------------------------------------------------------
struct FormatInfo {
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN; // storage format on the GPU
    UINT bytesPerPixel = 0;                 // D3D8 format, uncompressed
    UINT blockBytes = 0;                    // D3D8 format, block compressed (4x4 blocks)
    bool convert = false;                   // CPU conversion to B8G8R8A8 needed on upload
    bool depth = false;
};
FormatInfo GetFormatInfo(DWORD d8fmt);
bool IsSupportedTextureFormat(DWORD d8fmt);
UINT D8RowPitch(DWORD fmt, UINT width);   // bytes per row (per block row for DXT)
UINT D8RowCount(DWORD fmt, UINT height);  // rows (block rows for DXT)
// Converts rows of a D3D8 format to B8G8R8A8 (or R8G8_SNORM/R8G8B8A8_SNORM for bump formats).
void ConvertRows(DWORD fmt, const BYTE* src, UINT srcPitch, BYTE* dst, UINT dstPitch, UINT w, UINT h);
UINT DxgiRowPitch(DXGI_FORMAT f, UINT width);
UINT DxgiRowCount(DXGI_FORMAT f, UINT height);

inline UINT MipSize(UINT size, UINT level) { UINT s = size >> level; return s ? s : 1; }
inline UINT FullMipCount(UINT w, UINT h, UINT d = 1)
{
    UINT n = 1, m = w > h ? w : h;
    if (d > m) m = d;
    while (m > 1) { m >>= 1; n++; }
    return n;
}

inline float AsFloat(DWORD v) { float f; memcpy(&f, &v, 4); return f; }
inline void ColorToFloat4(DWORD c, float out[4])
{
    out[0] = ((c >> 16) & 0xff) / 255.0f;
    out[1] = ((c >> 8) & 0xff) / 255.0f;
    out[2] = (c & 0xff) / 255.0f;
    out[3] = ((c >> 24) & 0xff) / 255.0f;
}
