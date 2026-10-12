// common.cpp - logging, configuration and format conversion.
#include "common.h"
#include <stdio.h>
#include <share.h>
#include <stdarg.h>
#include <set>
#include <mutex>

Config g_config;
std::string g_gameDir;

static FILE* g_log = nullptr;
static std::mutex g_logMutex;
static std::set<std::string> g_logOnce;

void LogInit(const std::string& path)
{
    g_log = _fsopen(path.c_str(), "w", _SH_DENYNO);
}

static void LogV(const char* fmt, va_list ap)
{
    if (!g_log) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
}

void Log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

void LogOnce(const std::string& key, const char* fmt, ...)
{
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (!g_logOnce.insert(key).second) return;
    }
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

void LoadConfig()
{
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0;
    g_gameDir = exe;
    const std::string ini = g_gameDir + "d3d8to11.ini";
    char buf[64];
    GetPrivateProfileStringA("Renderer", "Backend", "d3d11", buf, sizeof(buf), ini.c_str());
    g_config.useDgVoodoo = _stricmp(buf, "dgvoodoo") == 0;
    g_config.syncInterval = GetPrivateProfileIntA("Renderer", "VSync", 0, ini.c_str()) ? 1 : 0;
    g_config.fpsLimit = GetPrivateProfileIntA("Renderer", "FPSLimit", 0, ini.c_str());
    g_config.forceAnisotropy = GetPrivateProfileIntA("Renderer", "Anisotropy", 16, ini.c_str());
    g_config.applyGamma = GetPrivateProfileIntA("Renderer", "ApplyGammaRamp", 1, ini.c_str()) != 0;
    g_config.debugLayer = GetPrivateProfileIntA("Debug", "D3D11DebugLayer", 0, ini.c_str()) != 0;
    g_config.dumpShaders = GetPrivateProfileIntA("Debug", "DumpShaders", 0, ini.c_str()) != 0;
    g_config.sunLighting = GetPrivateProfileIntA("Lighting", "Enabled", 0, ini.c_str()) != 0;
    g_config.replaceLighting = GetPrivateProfileIntA("Lighting", "ReplaceOriginal", 0, ini.c_str()) != 0;
    g_config.grassShadows = GetPrivateProfileIntA("Lighting", "GrassShadows", 0, ini.c_str()) != 0;
    g_config.lightingDebug = GetPrivateProfileIntA("Lighting", "DebugView", 0, ini.c_str());
    GetPrivateProfileStringA("Lighting", "SunSource", "game", buf, sizeof(buf), ini.c_str());
    g_config.sunFromGame = _stricmp(buf, "fixed") != 0;
    g_config.shadowResolution = GetPrivateProfileIntA("Lighting", "ShadowResolution", 4096, ini.c_str());
    if (g_config.shadowResolution != 1024 && g_config.shadowResolution != 2048 &&
        g_config.shadowResolution != 4096 && g_config.shadowResolution != 8192)
        g_config.shadowResolution = 4096;
    auto readFloat = [&](const char* key, float fallback, float lo, float hi) {
        char value[64];
        GetPrivateProfileStringA("Lighting", key, "", value, sizeof(value), ini.c_str());
        float f = value[0] ? (float)atof(value) : fallback;
        return f >= lo && f <= hi ? f : fallback;
    };
    g_config.shadowDistance = readFloat("ShadowDistance", 500, 32, 16384);
    g_config.sunAmbient = readFloat("Ambient", 0.60f, 0.1f, 2);
    g_config.sunDirect = readFloat("SunIntensity", 0.55f, 0, 2);
    g_config.shadowStrength = readFloat("ShadowStrength", 0.55f, 0, 1);
    g_config.sunMinElevation = readFloat("SunMinHeight", 0.35f, 0.05f, 1);
    g_config.sunDirection[0] = readFloat("SunX", -0.35f, -1, 1);
    g_config.sunDirection[1] = readFloat("SunY", 0.82f, 0.05f, 1);
    g_config.sunDirection[2] = readFloat("SunZ", -0.45f, -1, 1);
}

// ---- formats -----------------------------------------------------------------------------------

FormatInfo GetFormatInfo(DWORD f)
{
    FormatInfo i;
    switch (f) {
    case D8FMT_A8R8G8B8: i.dxgi = DXGI_FORMAT_B8G8R8A8_UNORM; i.bytesPerPixel = 4; break;
    case D8FMT_X8R8G8B8: i.dxgi = DXGI_FORMAT_B8G8R8A8_UNORM; i.bytesPerPixel = 4; i.convert = true; break;
    case D8FMT_R8G8B8:   i.dxgi = DXGI_FORMAT_B8G8R8A8_UNORM; i.bytesPerPixel = 3; i.convert = true; break;
    case D8FMT_R5G6B5: case D8FMT_X1R5G5B5: case D8FMT_A1R5G5B5: case D8FMT_A4R4G4B4:
    case D8FMT_X4R4G4B4: case D8FMT_A8L8: case D8FMT_A8R3G3B2:
        i.dxgi = DXGI_FORMAT_B8G8R8A8_UNORM; i.bytesPerPixel = 2; i.convert = true; break;
    case D8FMT_L8: case D8FMT_A8: case D8FMT_A4L4: case D8FMT_R3G3B2:
        i.dxgi = DXGI_FORMAT_B8G8R8A8_UNORM; i.bytesPerPixel = 1; i.convert = true; break;
    case D8FMT_V8U8: i.dxgi = DXGI_FORMAT_R8G8_SNORM; i.bytesPerPixel = 2; break;
    case D8FMT_Q8W8V8U8: i.dxgi = DXGI_FORMAT_R8G8B8A8_SNORM; i.bytesPerPixel = 4; break;
    case D8FMT_X8L8V8U8: i.dxgi = DXGI_FORMAT_R8G8B8A8_SNORM; i.bytesPerPixel = 4; i.convert = true; break;
    case D8FMT_DXT1: i.dxgi = DXGI_FORMAT_BC1_UNORM; i.blockBytes = 8; break;
    case D8FMT_DXT2: case D8FMT_DXT3: i.dxgi = DXGI_FORMAT_BC2_UNORM; i.blockBytes = 16; break;
    case D8FMT_DXT4: case D8FMT_DXT5: i.dxgi = DXGI_FORMAT_BC3_UNORM; i.blockBytes = 16; break;
    case D8FMT_D16: case D8FMT_D16_LOCKABLE:
        i.dxgi = DXGI_FORMAT_D24_UNORM_S8_UINT; i.bytesPerPixel = 2; i.depth = true; break;
    case D8FMT_D24S8: case D8FMT_D24X8: case D8FMT_D24X4S4: case D8FMT_D32: case D8FMT_D15S1:
        i.dxgi = DXGI_FORMAT_D24_UNORM_S8_UINT; i.bytesPerPixel = 4; i.depth = true; break;
    default: break;
    }
    return i;
}

bool IsSupportedTextureFormat(DWORD f)
{
    FormatInfo i = GetFormatInfo(f);
    return i.dxgi != DXGI_FORMAT_UNKNOWN && !i.depth;
}

UINT D8RowPitch(DWORD fmt, UINT width)
{
    FormatInfo i = GetFormatInfo(fmt);
    if (i.blockBytes) return ((width + 3) / 4) * i.blockBytes;
    return width * i.bytesPerPixel;
}

UINT D8RowCount(DWORD fmt, UINT height)
{
    return GetFormatInfo(fmt).blockBytes ? (height + 3) / 4 : height;
}

UINT DxgiRowPitch(DXGI_FORMAT f, UINT width)
{
    switch (f) {
    case DXGI_FORMAT_BC1_UNORM: return ((width + 3) / 4) * 8;
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: return ((width + 3) / 4) * 16;
    case DXGI_FORMAT_R8G8_SNORM: return width * 2;
    default: return width * 4;
    }
}

UINT DxgiRowCount(DXGI_FORMAT f, UINT height)
{
    switch (f) {
    case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: return (height + 3) / 4;
    default: return height;
    }
}

static inline BYTE Expand(UINT v, int bits) { return (BYTE)((v * 255 + ((1 << bits) - 1) / 2) / ((1 << bits) - 1)); }

void ConvertRows(DWORD fmt, const BYTE* src, UINT srcPitch, BYTE* dst, UINT dstPitch, UINT w, UINT h)
{
    for (UINT y = 0; y < h; y++) {
        const BYTE* s = src + (size_t)y * srcPitch;
        BYTE* d = dst + (size_t)y * dstPitch; // B, G, R, A
        for (UINT x = 0; x < w; x++, d += 4) {
            UINT v;
            switch (fmt) {
            case D8FMT_X8R8G8B8: d[0] = s[x * 4]; d[1] = s[x * 4 + 1]; d[2] = s[x * 4 + 2]; d[3] = 255; break;
            case D8FMT_R8G8B8: d[0] = s[x * 3]; d[1] = s[x * 3 + 1]; d[2] = s[x * 3 + 2]; d[3] = 255; break;
            case D8FMT_R5G6B5: v = ((const WORD*)s)[x];
                d[2] = Expand(v >> 11, 5); d[1] = Expand((v >> 5) & 63, 6); d[0] = Expand(v & 31, 5); d[3] = 255; break;
            case D8FMT_X1R5G5B5: case D8FMT_A1R5G5B5: v = ((const WORD*)s)[x];
                d[2] = Expand((v >> 10) & 31, 5); d[1] = Expand((v >> 5) & 31, 5); d[0] = Expand(v & 31, 5);
                d[3] = (fmt == D8FMT_A1R5G5B5) ? ((v & 0x8000) ? 255 : 0) : 255; break;
            case D8FMT_A4R4G4B4: case D8FMT_X4R4G4B4: v = ((const WORD*)s)[x];
                d[2] = Expand((v >> 8) & 15, 4); d[1] = Expand((v >> 4) & 15, 4); d[0] = Expand(v & 15, 4);
                d[3] = fmt == D8FMT_A4R4G4B4 ? Expand(v >> 12, 4) : 255; break;
            case D8FMT_A8R3G3B2: v = ((const WORD*)s)[x];
                d[2] = Expand((v >> 5) & 7, 3); d[1] = Expand((v >> 2) & 7, 3); d[0] = Expand(v & 3, 2); d[3] = (BYTE)(v >> 8); break;
            case D8FMT_R3G3B2: v = s[x];
                d[2] = Expand((v >> 5) & 7, 3); d[1] = Expand((v >> 2) & 7, 3); d[0] = Expand(v & 3, 2); d[3] = 255; break;
            case D8FMT_A8L8: v = ((const WORD*)s)[x]; d[0] = d[1] = d[2] = (BYTE)v; d[3] = (BYTE)(v >> 8); break;
            case D8FMT_L8: d[0] = d[1] = d[2] = s[x]; d[3] = 255; break;
            case D8FMT_A8: d[0] = d[1] = d[2] = 0; d[3] = s[x]; break;
            case D8FMT_A4L4: v = s[x]; d[0] = d[1] = d[2] = Expand(v & 15, 4); d[3] = Expand(v >> 4, 4); break;
            case D8FMT_X8L8V8U8: // U, V, L, X -> R8G8B8A8_SNORM (u, v, l, 1); L is unsigned in D3D8
                d[0] = s[x * 4]; d[1] = s[x * 4 + 1]; d[2] = (BYTE)(s[x * 4 + 2] >> 1); d[3] = 127; break;
            default: d[0] = d[1] = d[2] = d[3] = 255; break;
            }
        }
    }
}
