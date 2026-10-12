// d3d8_profiler.h - Direct3D 8 usage profiler (dx11 branch)
//
// Goal: know exactly which parts of the API the game uses before writing the D3D8 -> D3D11 layer.
// The IDirect3DDevice8 vtable is hooked:
//  - Every method (0..96) goes through a thunk that counts calls.
//  - Some methods have a detailed hook that records arguments (render states, texture stage
//    states, formats, FVFs, primitive types, shaders...).
//  - Created vertex/pixel shaders are dumped to disk (declaration + bytecode).
// <game>\d3d8_profile\profile.txt is rewritten every ~300 frames. Enabled with [Debug] ProfileD3D8=1.
#pragma once
#include <windows.h>
#include <stdio.h>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace prof {

static const int kMethods = 97;
static const char* kNames[kMethods] = {
    "QueryInterface", "AddRef", "Release", "TestCooperativeLevel", "GetAvailableTextureMem",
    "ResourceManagerDiscardBytes", "GetDirect3D", "GetDeviceCaps", "GetDisplayMode", "GetCreationParameters",
    "SetCursorProperties", "SetCursorPosition", "ShowCursor", "CreateAdditionalSwapChain", "Reset",
    "Present", "GetBackBuffer", "GetRasterStatus", "SetGammaRamp", "GetGammaRamp",
    "CreateTexture", "CreateVolumeTexture", "CreateCubeTexture", "CreateVertexBuffer", "CreateIndexBuffer",
    "CreateRenderTarget", "CreateDepthStencilSurface", "CreateImageSurface", "CopyRects", "UpdateTexture",
    "GetFrontBuffer", "SetRenderTarget", "GetRenderTarget", "GetDepthStencilSurface", "BeginScene",
    "EndScene", "Clear", "SetTransform", "GetTransform", "MultiplyTransform",
    "SetViewport", "GetViewport", "SetMaterial", "GetMaterial", "SetLight",
    "GetLight", "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane",
    "SetRenderState", "GetRenderState", "BeginStateBlock", "EndStateBlock", "ApplyStateBlock",
    "CaptureStateBlock", "DeleteStateBlock", "CreateStateBlock", "SetClipStatus", "GetClipStatus",
    "GetTexture", "SetTexture", "GetTextureStageState", "SetTextureStageState", "ValidateDevice",
    "GetInfo", "SetPaletteEntries", "GetPaletteEntries", "SetCurrentTexturePalette", "GetCurrentTexturePalette",
    "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "ProcessVertices",
    "CreateVertexShader", "SetVertexShader", "GetVertexShader", "DeleteVertexShader", "SetVertexShaderConstant",
    "GetVertexShaderConstant", "GetVertexShaderDeclaration", "GetVertexShaderFunction", "SetStreamSource", "GetStreamSource",
    "SetIndices", "GetIndices", "CreatePixelShader", "SetPixelShader", "GetPixelShader",
    "DeletePixelShader", "SetPixelShaderConstant", "GetPixelShaderConstant", "GetPixelShaderFunction", "DrawRectPatch",
    "DrawTriPatch", "DeletePatch",
};

static volatile LONG g_calls[kMethods];
static void* g_orig[kMethods];
static bool g_installed = false;
static std::string g_dir;

// Detailed records
static std::map<DWORD, std::set<DWORD>> g_rs;                  // render state -> values
static std::map<DWORD, std::set<DWORD>> g_tss;                 // (stage<<16|type) -> values
static std::map<std::string, int> g_misc;                      // description -> count
static std::map<DWORD, int> g_drawsByVS;                       // active handle/FVF -> draws
static std::map<DWORD, int> g_vsConstRegs;                     // start register -> max count
static DWORD g_curVS = 0, g_curPS = 0;
static int g_presents = 0, g_vsDumped = 0, g_psDumped = 0;

static void Count(const char* fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_misc[buf]++;
}

static void AddValue(std::map<DWORD, std::set<DWORD>>& m, DWORD key, DWORD v)
{
    auto& s = m[key];
    if (s.size() < 24) s.insert(v);
}

static void Dump()
{
    std::string path = g_dir + "profile.txt";
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "w") != 0 || !f) return;
    fprintf(f, "Presents: %d\n\n== Calls per method ==\n", g_presents);
    for (int i = 0; i < kMethods; i++)
        if (g_calls[i]) fprintf(f, "%3d %-28s %ld\n", i, kNames[i], g_calls[i]);
    fprintf(f, "\n== Render states (state: values) ==\n");
    for (auto& kv : g_rs) {
        fprintf(f, "RS %3lu:", kv.first);
        for (DWORD v : kv.second) fprintf(f, " 0x%lx", v);
        fprintf(f, "\n");
    }
    fprintf(f, "\n== Texture stage states (stage.type: values) ==\n");
    for (auto& kv : g_tss) {
        fprintf(f, "TSS %lu.%-3lu:", kv.first >> 16, kv.first & 0xffff);
        for (DWORD v : kv.second) fprintf(f, " 0x%lx", v);
        fprintf(f, "\n");
    }
    fprintf(f, "\n== Draws per active vertex shader / FVF ==\n");
    for (auto& kv : g_drawsByVS) fprintf(f, "VS 0x%08lx: %d\n", kv.first, kv.second);
    fprintf(f, "\n== Vertex shader constants (start register: max count) ==\n");
    for (auto& kv : g_vsConstRegs) fprintf(f, "c%lu: %d\n", kv.first, kv.second);
    fprintf(f, "\n== Other ==\n");
    for (auto& kv : g_misc) fprintf(f, "%-60s %d\n", kv.first.c_str(), kv.second);
    fclose(f);
}

static void WriteBlob(const char* name, const void* data, size_t bytes)
{
    FILE* f = nullptr;
    if (fopen_s(&f, (g_dir + name).c_str(), "wb") != 0 || !f) return;
    fwrite(data, 1, bytes, f);
    fclose(f);
}

static size_t TokenStreamBytes(const DWORD* p, DWORD endToken, size_t maxTokens)
{
    size_t n = 0;
    while (n < maxTokens && p[n] != endToken) n++;
    return (n + 1) * 4;
}

// ---- detailed hooks (stdcall, this first) ----
#define ORIG(idx, type) ((type)g_orig[idx])

static HRESULT WINAPI H_Present(void* d, const RECT* a, const RECT* b, HWND c, const void* e)
{
    InterlockedIncrement(&g_calls[15]);
    if (++g_presents % 300 == 0) Dump();
    return ORIG(15, HRESULT(WINAPI*)(void*, const RECT*, const RECT*, HWND, const void*))(d, a, b, c, e);
}

static HRESULT WINAPI H_CreateTexture(void* d, UINT w, UINT h, UINT lv, DWORD usage, DWORD fmt, DWORD pool, void** out)
{
    InterlockedIncrement(&g_calls[20]);
    Count("CreateTexture fmt=%lu pool=%lu usage=0x%lx mips=%s", fmt, pool, usage, lv == 1 ? "1" : "many");
    return ORIG(20, HRESULT(WINAPI*)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**))(d, w, h, lv, usage, fmt, pool, out);
}

static HRESULT WINAPI H_CreateCubeTexture(void* d, UINT e, UINT lv, DWORD usage, DWORD fmt, DWORD pool, void** out)
{
    InterlockedIncrement(&g_calls[22]);
    Count("CreateCubeTexture fmt=%lu pool=%lu usage=0x%lx", fmt, pool, usage);
    return ORIG(22, HRESULT(WINAPI*)(void*, UINT, UINT, DWORD, DWORD, DWORD, void**))(d, e, lv, usage, fmt, pool, out);
}

static HRESULT WINAPI H_CreateVB(void* d, UINT len, DWORD usage, DWORD fvf, DWORD pool, void** out)
{
    InterlockedIncrement(&g_calls[23]);
    Count("CreateVertexBuffer usage=0x%lx fvf=0x%lx pool=%lu", usage, fvf, pool);
    return ORIG(23, HRESULT(WINAPI*)(void*, UINT, DWORD, DWORD, DWORD, void**))(d, len, usage, fvf, pool, out);
}

static HRESULT WINAPI H_CreateIB(void* d, UINT len, DWORD usage, DWORD fmt, DWORD pool, void** out)
{
    InterlockedIncrement(&g_calls[24]);
    Count("CreateIndexBuffer usage=0x%lx fmt=%lu pool=%lu", usage, fmt, pool);
    return ORIG(24, HRESULT(WINAPI*)(void*, UINT, DWORD, DWORD, DWORD, void**))(d, len, usage, fmt, pool, out);
}

static HRESULT WINAPI H_CreateRT(void* d, UINT w, UINT h, DWORD fmt, DWORD ms, BOOL lockable, void** out)
{
    InterlockedIncrement(&g_calls[25]);
    Count("CreateRenderTarget %ux%u fmt=%lu ms=%lu", w, h, fmt, ms);
    return ORIG(25, HRESULT(WINAPI*)(void*, UINT, UINT, DWORD, DWORD, BOOL, void**))(d, w, h, fmt, ms, lockable, out);
}

static HRESULT WINAPI H_SetTransform(void* d, DWORD st, const void* m)
{
    InterlockedIncrement(&g_calls[37]);
    Count("SetTransform %lu", st);
    return ORIG(37, HRESULT(WINAPI*)(void*, DWORD, const void*))(d, st, m);
}

static HRESULT WINAPI H_SetLight(void* d, DWORD idx, const DWORD* light)
{
    InterlockedIncrement(&g_calls[44]);
    Count("SetLight idx=%lu type=%lu", idx, light ? light[0] : 0);
    return ORIG(44, HRESULT(WINAPI*)(void*, DWORD, const DWORD*))(d, idx, light);
}

static HRESULT WINAPI H_SetRenderState(void* d, DWORD st, DWORD v)
{
    InterlockedIncrement(&g_calls[50]);
    AddValue(g_rs, st, v);
    return ORIG(50, HRESULT(WINAPI*)(void*, DWORD, DWORD))(d, st, v);
}

static HRESULT WINAPI H_SetTexture(void* d, DWORD stage, void* tex)
{
    InterlockedIncrement(&g_calls[61]);
    Count("SetTexture stage=%lu %s", stage, tex ? "tex" : "NULL");
    return ORIG(61, HRESULT(WINAPI*)(void*, DWORD, void*))(d, stage, tex);
}

static HRESULT WINAPI H_SetTSS(void* d, DWORD stage, DWORD type, DWORD v)
{
    InterlockedIncrement(&g_calls[63]);
    AddValue(g_tss, (stage << 16) | type, v);
    return ORIG(63, HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD))(d, stage, type, v);
}

static HRESULT WINAPI H_DrawPrimitive(void* d, DWORD pt, UINT start, UINT count)
{
    InterlockedIncrement(&g_calls[70]);
    Count("DrawPrimitive type=%lu", pt);
    g_drawsByVS[g_curVS]++;
    return ORIG(70, HRESULT(WINAPI*)(void*, DWORD, UINT, UINT))(d, pt, start, count);
}

static HRESULT WINAPI H_DrawIndexed(void* d, DWORD pt, UINT mi, UINT nv, UINT si, UINT pc)
{
    InterlockedIncrement(&g_calls[71]);
    Count("DrawIndexedPrimitive type=%lu", pt);
    g_drawsByVS[g_curVS]++;
    return ORIG(71, HRESULT(WINAPI*)(void*, DWORD, UINT, UINT, UINT, UINT))(d, pt, mi, nv, si, pc);
}

static HRESULT WINAPI H_DrawUP(void* d, DWORD pt, UINT pc, const void* data, UINT stride)
{
    InterlockedIncrement(&g_calls[72]);
    Count("DrawPrimitiveUP type=%lu stride=%u", pt, stride);
    g_drawsByVS[g_curVS]++;
    return ORIG(72, HRESULT(WINAPI*)(void*, DWORD, UINT, const void*, UINT))(d, pt, pc, data, stride);
}

static HRESULT WINAPI H_DrawIndexedUP(void* d, DWORD pt, UINT mi, UINT nv, UINT pc, const void* idx, DWORD fmt,
                                      const void* data, UINT stride)
{
    InterlockedIncrement(&g_calls[73]);
    Count("DrawIndexedPrimitiveUP type=%lu ifmt=%lu stride=%u", pt, fmt, stride);
    g_drawsByVS[g_curVS]++;
    return ORIG(73, HRESULT(WINAPI*)(void*, DWORD, UINT, UINT, UINT, const void*, DWORD, const void*, UINT))(
        d, pt, mi, nv, pc, idx, fmt, data, stride);
}

static HRESULT WINAPI H_CreateVS(void* d, const DWORD* decl, const DWORD* func, DWORD* handle, DWORD usage)
{
    InterlockedIncrement(&g_calls[75]);
    HRESULT hr = ORIG(75, HRESULT(WINAPI*)(void*, const DWORD*, const DWORD*, DWORD*, DWORD))(d, decl, func, handle, usage);
    if (hr >= 0 && handle && g_vsDumped < 256) {
        char name[64];
        sprintf_s(name, "vs_%03d_h%08lx_decl.bin", g_vsDumped, *handle);
        if (decl) WriteBlob(name, decl, TokenStreamBytes(decl, 0xFFFFFFFF, 4096));
        if (func) {
            sprintf_s(name, "vs_%03d_h%08lx_func.bin", g_vsDumped, *handle);
            WriteBlob(name, func, TokenStreamBytes(func, 0x0000FFFF, 65536));
        }
        Count("CreateVertexShader %s usage=0x%lx", func ? "programmable" : "declaration only (FF)", usage);
        g_vsDumped++;
    }
    return hr;
}

static HRESULT WINAPI H_SetVS(void* d, DWORD h)
{
    InterlockedIncrement(&g_calls[76]);
    g_curVS = h;
    return ORIG(76, HRESULT(WINAPI*)(void*, DWORD))(d, h);
}

static HRESULT WINAPI H_SetVSConst(void* d, DWORD reg, const void* data, DWORD count)
{
    InterlockedIncrement(&g_calls[79]);
    int& m = g_vsConstRegs[reg];
    if ((int)count > m) m = (int)count;
    return ORIG(79, HRESULT(WINAPI*)(void*, DWORD, const void*, DWORD))(d, reg, data, count);
}

static HRESULT WINAPI H_SetStreamSource(void* d, UINT n, void* vb, UINT stride)
{
    InterlockedIncrement(&g_calls[83]);
    Count("SetStreamSource stream=%u stride=%u", n, stride);
    return ORIG(83, HRESULT(WINAPI*)(void*, UINT, void*, UINT))(d, n, vb, stride);
}

static HRESULT WINAPI H_CreatePS(void* d, const DWORD* func, DWORD* handle)
{
    InterlockedIncrement(&g_calls[87]);
    HRESULT hr = ORIG(87, HRESULT(WINAPI*)(void*, const DWORD*, DWORD*))(d, func, handle);
    if (hr >= 0 && handle && func && g_psDumped < 256) {
        char name[64];
        sprintf_s(name, "ps_%03d_h%08lx.bin", g_psDumped, *handle);
        WriteBlob(name, func, TokenStreamBytes(func, 0x0000FFFF, 65536));
        Count("CreatePixelShader version=0x%08lx", func[0]);
        g_psDumped++;
    }
    return hr;
}

static HRESULT WINAPI H_SetPS(void* d, DWORD h)
{
    InterlockedIncrement(&g_calls[88]);
    g_curPS = h;
    Count("SetPixelShader %s", h ? "shader" : "NULL (FF)");
    return ORIG(88, HRESULT(WINAPI*)(void*, DWORD))(d, h);
}

// Generic thunk: inc dword [g_calls+i*4]; jmp dword [g_orig+i*4]
static void* MakeCountThunk(int i)
{
    static BYTE* page = nullptr;
    static int used = 0;
    if (!page) page = (BYTE*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!page || used + 12 > 4096) return nullptr;
    BYTE* t = page + used;
    used += 12;
    t[0] = 0xFF; t[1] = 0x05; *(DWORD*)(t + 2) = (DWORD)&g_calls[i];
    t[6] = 0xFF; t[7] = 0x25; *(DWORD*)(t + 8) = (DWORD)&g_orig[i];
    return t;
}

static void Install(void* device, const std::string& outDir)
{
    if (g_installed || !device) return;
    g_installed = true;
    g_dir = outDir;
    CreateDirectoryA(g_dir.c_str(), nullptr);
    void** vt = *(void***)device;
    struct { int idx; void* fn; } detail[] = {
        { 15, (void*)&H_Present }, { 20, (void*)&H_CreateTexture }, { 22, (void*)&H_CreateCubeTexture },
        { 23, (void*)&H_CreateVB }, { 24, (void*)&H_CreateIB }, { 25, (void*)&H_CreateRT },
        { 37, (void*)&H_SetTransform }, { 44, (void*)&H_SetLight }, { 50, (void*)&H_SetRenderState },
        { 61, (void*)&H_SetTexture }, { 63, (void*)&H_SetTSS }, { 70, (void*)&H_DrawPrimitive },
        { 71, (void*)&H_DrawIndexed }, { 72, (void*)&H_DrawUP }, { 73, (void*)&H_DrawIndexedUP },
        { 75, (void*)&H_CreateVS }, { 76, (void*)&H_SetVS }, { 79, (void*)&H_SetVSConst },
        { 83, (void*)&H_SetStreamSource }, { 87, (void*)&H_CreatePS }, { 88, (void*)&H_SetPS },
    };
    DWORD old;
    VirtualProtect(vt, kMethods * sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    for (int i = 0; i < kMethods; i++) {
        g_orig[i] = vt[i];
        void* repl = nullptr;
        for (auto& dh : detail) if (dh.idx == i) repl = dh.fn;
        if (!repl) repl = MakeCountThunk(i);
        if (repl) vt[i] = repl;
    }
    VirtualProtect(vt, kMethods * sizeof(void*), old, &old);
}

} // namespace prof
