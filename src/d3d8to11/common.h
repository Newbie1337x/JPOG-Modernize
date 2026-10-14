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
    float relightSun = 1.35f;       // [Lighting] RelightSun: scale of the game's own sun colour
    float relightAmbient = 0.85f;   // [Lighting] RelightAmbient: scale of the game's own ambient colour
    float relightSkyTint = 0.65f;   // [Lighting] RelightSkyTint: blue sky above, warm bounce below
    float relightExposure = 0.95f;  // [Lighting] RelightExposure
    bool proceduralSky = true;      // [Lighting] ProceduralSky: replace the game's sky dome (with ReplaceOriginal)
    bool heightfieldLighting = true; // [Lighting] HeightfieldLighting: terrain normals from the native height field
    float bakedTerrainDetail = 1.0f; // [Lighting] BakedTerrainDetail: keep the game's painted terrain darkening, 0..1
    float horizonShadows = 1.0f;    // [Lighting] HorizonShadows: hills shade the terrain behind them, 0..1
    float grassSaturation = 0.8f;   // [Lighting] GrassSaturation (replacement lighting)
    float grassBrightness = 0.92f;  // [Lighting] GrassBrightness (replacement lighting)
    float rimLight = 0.5f;          // [Lighting] RimLight: sunlit silhouettes (replacement lighting), 0..2
    // [TimeOfDay] look of the time-of-day preview (Insert panel, "Time of day tuning")
    float moonColor[3] = { 0.70f, 0.80f, 1.0f };
    float moonIntensity = 1.0f;
    float moonRim = 2.5f;
    float moonSheen = 1.0f;         // silver highlight of the moon on leaves and skin
    float grassMoon = 1.0f;         // moonlight on grass blade tips
    float nightSurface = 0.2f;      // share of the moon on surfaces without normals (grass, water)
    float nightAmbient[3] = { 0.08f, 0.12f, 0.24f };
    float duskAmbient[3] = { 0.62f, 0.68f, 0.88f };
    float lowSunColor[3] = { 1.0f, 0.55f, 0.24f };
    float maxExposure = 1.5f;
    float nightSky = 1.0f;          // brightness of the procedural night sky
    float nightDesaturate = 0.4f;   // colours fade under the moon
    float stars = 1.0f;
    bool timeOfDay = false;         // Insert panel preview: drive the sun from an hour of the day
    float timeOfDayHour = 12.0f;
    bool relightTonemap = true;     // [Lighting] RelightTonemap: soft shoulder for values above 0.8
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
    // [Tessellation] PN-triangle smoothing of low-poly models (hull/domain shaders)
    bool tessellation = true;       // [Tessellation] Enabled
    bool tessDinosOnly = true;      // [Tessellation] DinosOnly: only textures from dinos_* libraries
    float tessMaxFactor = 8.0f;     // [Tessellation] MaxFactor: subdivisions per edge, 1..16
    float tessPixels = 10.0f;       // [Tessellation] PixelsPerEdge: target screen length of a sub-edge
    float tessCurvature = 1.0f;     // [Tessellation] Curvature: 0 flat .. 1 full PN curvature
    bool tessFlipWinding = false;   // [Tessellation] FlipWinding: if tessellated models look inside out
    float tessDisplacement = 0.03f; // [Tessellation] Displacement: relief height in world units, 0 = off
    float tessDetailMip = 0.0f;     // [Tessellation] DetailMip: texture mip the relief reads (higher = softer)
    bool tessInvertRelief = false;  // [Tessellation] InvertRelief: dark raised instead of bright
    bool reliefEnabled = true;      // [Relief] Enabled: parallax + self-shadowing on dinosaur skins
    float reliefDepth = 3.0f;       // [Relief] Depth: height range in texels
    float reliefParallax = 0.5f;    // [Relief] Parallax: 0 = shadows only .. 1 = full view-dependent shift
    float reliefShadow = 0.6f;      // [Relief] SelfShadow: 0 off .. 1 full
    float reliefSoftness = 1.0f;    // [Relief] Softness: penumbra width in texels
    bool reliefInvert = false;      // [Relief] Invert: dark raised instead of bright
    bool tessSeamSafe = true;
    bool tessWeld = true;           // [Tessellation] WeldSeams: split vertices share one curve (no cracks)       // [Tessellation] SeamSafe: fade relief at the original edges (no cracks)
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
