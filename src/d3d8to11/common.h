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
    bool useDgVoodoo = false;   // [Renderer] Backend=dgvoodoo forwards everything to D3D8_dgVoodoo.dll
    int syncInterval = 0;       // [Renderer] VSync
    int fpsLimit = 0;           // [Renderer] FPSLimit (0 = off)
    int forceAnisotropy = 16;   // [Renderer] Anisotropy (0/1 = use game setting)
    bool applyGamma = true;     // [Renderer] ApplyGammaRamp
    bool debugLayer = false;    // [Debug] D3D11DebugLayer
    bool dumpShaders = false;   // [Debug] DumpShaders (writes generated HLSL to d3d8to11_shaders\)
    bool sunLighting = false;
    int shadowResolution = 2048;
    float shadowExtent = 384.0f;
    float sunAmbient = 0.60f, sunDirect = 0.55f, shadowStrength = 0.45f;
    float sunDirection[3] = { -0.35f, 0.82f, -0.45f };
};
extern Config g_config;
extern std::string g_gameDir;
void LoadConfig();

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
