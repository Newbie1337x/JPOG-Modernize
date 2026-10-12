// GPU check of the sun shadow pass with captured JPOG shaders (hidden 64x64 window, not SimJP).
// A top-down terrain quad and a floating caster quad are drawn in one frame; after Present the
// back buffer must show the caster's shadow displaced away from the sun, in that same frame,
// and nothing drawn without depth (HUD) may be darkened.
// Run from a folder holding out\d3d8.dll, profile\ (captured shaders) and a d3d8to11.ini with
// [Lighting] Enabled=1, SunSource=fixed, SunX=0.6, SunY=0.8, SunZ=0.
#include "../src/d3d8to11/d3d8_types.h"
#include <vector>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
static std::vector<DWORD> read(const char* p)
{
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    auto n = f.tellg();
    if (n <= 0) { printf("missing %s\n", p); exit(2); }
    std::vector<DWORD> a((size_t)n / 4);
    f.seekg(0);
    f.read((char*)a.data(), n);
    return a;
}
int main()
{
    HWND w = CreateWindowA("STATIC", "suncheck", WS_OVERLAPPEDWINDOW, 0, 0, 128, 128, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    auto lib = LoadLibraryA("out\\d3d8.dll");
    if (!lib) return 2;
    auto fn = (ID8Direct3D*(WINAPI*)(UINT))GetProcAddress(lib, "Direct3DCreate8");
    auto d = fn(120);
    D3D8PresentParameters pp = {};
    pp.BackBufferWidth = pp.BackBufferHeight = 64; pp.BackBufferFormat = 22; pp.BackBufferCount = 1;
    pp.SwapEffect = 1; pp.hDeviceWindow = w; pp.Windowed = 1; pp.EnableAutoDepthStencil = 1; pp.AutoDepthStencilFormat = 71;
    ID8Device* dev = nullptr;
    if (FAILED(d->CreateDevice(0, 1, w, 0x40, &pp, &dev))) return 3;

    auto decl = read("profile\\vs_002_h00000007_decl.bin"), func = read("profile\\vs_002_h00000007_func.bin");
    auto psCode = read("profile\\ps_009_h0000000a.bin");
    auto td = read("profile\\vs_005_h0000000d_decl.bin"), tf = read("profile\\vs_005_h0000000d_func.bin");
    DWORD rigid = 0, terrain = 0, pixel = 0;
    dev->CreateVertexShader(decl.data(), func.data(), &rigid, 0);
    dev->CreateVertexShader(td.data(), tf.data(), &terrain, 0);
    dev->CreatePixelShader(psCode.data(), &pixel);
    dev->SetPixelShader(pixel);

    // Top-down orthographic camera: clip = (x/32, z/32, 0.5 - 0.01 y, 1).
    float camera[16] = { 1.f / 32, 0, 0, 0,  0, 0, 1.f / 32, 0,  0, -.01f, 0, .5f,  0, 0, 0, 1 };
    float white[4] = { 1, 1, 1, 1 }, zero[4] = {};

    struct V { float x, y, z; DWORD color; float u, v; };
    V caster[4] = { { -4, 12, 4, ~0u, 0, 0 }, { 4, 12, 4, ~0u, 1, 0 }, { 4, 12, -4, ~0u, 1, 1 }, { -4, 12, -4, ~0u, 0, 1 } };
    // HUD strip across the top rows, drawn without depth after the world.
    V hud[4] = { { -14, 0, 2, ~0u, 0, 0 }, { -10, 0, 2, ~0u, 1, 0 }, { -10, 0, -2, ~0u, 1, 1 }, { -14, 0, -2, ~0u, 0, 1 } };
    BYTE* data = nullptr;
    ID8VertexBuffer *casterVB = nullptr, *hudVB = nullptr, *heightVB = nullptr, *gridVB = nullptr;
    dev->CreateVertexBuffer(sizeof(caster), 0, 0, 1, &casterVB);
    casterVB->Lock(0, 0, &data, 0); memcpy(data, caster, sizeof(caster)); casterVB->Unlock();
    dev->CreateVertexBuffer(sizeof(hud), 0, 0, 1, &hudVB);
    hudVB->Lock(0, 0, &data, 0); memcpy(data, hud, sizeof(hud)); hudVB->Unlock();
    DWORD heights[4] = {};
    float grid[8] = { -40, 40, 40, 40, 40, -40, -40, -40 };
    dev->CreateVertexBuffer(sizeof(heights), 0, 0, 1, &heightVB);
    heightVB->Lock(0, 0, &data, 0); memcpy(data, heights, sizeof(heights)); heightVB->Unlock();
    dev->CreateVertexBuffer(sizeof(grid), 0, 0, 1, &gridVB);
    gridVB->Lock(0, 0, &data, 0); memcpy(data, grid, sizeof(grid)); gridVB->Unlock();

    ID8Texture* tex = nullptr;
    dev->CreateTexture(4, 4, 1, 0, 21, 1, &tex);
    D3D8LockedRect lr = {};
    tex->LockRect(0, &lr, nullptr, 0);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ((DWORD*)((BYTE*)lr.pBits + y * lr.Pitch))[x] = 0xff808080;
    tex->UnlockRect(0);
    ID8Texture* hudTex = nullptr;
    dev->CreateTexture(4, 4, 1, 0, 21, 1, &hudTex);
    hudTex->LockRect(0, &lr, nullptr, 0);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ((DWORD*)((BYTE*)lr.pBits + y * lr.Pitch))[x] = 0xffc8c8c8;
    hudTex->UnlockRect(0);
    dev->SetRenderState(RS_CULLMODE, 1);

    int failures = 0;
    for (int frame = 0; frame < 3; frame++) {
        dev->Clear(0, nullptr, 3, 0xff000000, 1, 0);
        dev->SetRenderState(RS_ZENABLE, 1);
        dev->SetRenderState(RS_ZWRITEENABLE, 1);
        dev->SetRenderState(RS_ALPHABLENDENABLE, 0);
        dev->SetTexture(0, tex);
        // Terrain: writes depth, establishes the camera.
        dev->SetVertexShader(terrain);
        dev->SetVertexShaderConstant(0, camera, 4);
        dev->SetVertexShaderConstant(4, zero, 1);
        dev->SetVertexShaderConstant(5, zero, 1);
        dev->SetStreamSource(0, heightVB, 4);
        dev->SetStreamSource(1, gridVB, 8);
        dev->DrawPrimitive(6, 0, 2);
        // Caster 12 units above the ground; the model is moved through its own c0..c3 (frame 2).
        float model[16];
        memcpy(model, camera, sizeof(model));
        if (frame == 2) model[3] += 4.0f / 32; // shift 4 units in +x
        dev->SetVertexShader(rigid);
        dev->SetVertexShaderConstant(0, model, 4);
        dev->SetVertexShaderConstant(4, white, 1);
        dev->SetStreamSource(0, casterVB, sizeof(V));
        dev->DrawPrimitive(6, 0, 2);
        // HUD
        dev->SetRenderState(RS_ZENABLE, 0);
        dev->SetTexture(0, hudTex);
        dev->SetVertexShaderConstant(0, camera, 4);
        dev->SetStreamSource(0, hudVB, sizeof(V));
        dev->DrawPrimitive(6, 0, 2);
        dev->Present(nullptr, nullptr, nullptr, nullptr);

        ID8Surface* bb = nullptr;
        dev->GetBackBuffer(0, 0, &bb);
        if (FAILED(bb->LockRect(&lr, nullptr, 16))) return 4;
        auto at = [&](int x, int y) { return (int)(((DWORD*)((BYTE*)lr.pBits + y * lr.Pitch))[x] & 255); };
        // world x -> pixel (x/32 + 1) * 32. Sun toward +x at 0.6/0.8: shadow 9 units toward -x.
        const int dx = frame == 2 ? 4 : 0;
        int px = (int)((-9 + dx) / 32.0f * 32 + 32);
        int lit = at(52, 32), shadow = at(px, 32), casterTop = at(32 + dx, 32), hudPx = at(20, 32);
        printf("frame %d lit=%d shadow(x=%d)=%d caster=%d hud=%d\n", frame, lit, px, shadow, casterTop, hudPx);
        bool ok = lit == 128 && shadow < lit - 25 && casterTop == 128 && hudPx == 200;
        if (!ok) failures++;
        bb->UnlockRect();
        bb->Release();
    }
    printf(failures ? "FAILED\n" : "OK\n");
    return failures ? 5 : 0;
}
