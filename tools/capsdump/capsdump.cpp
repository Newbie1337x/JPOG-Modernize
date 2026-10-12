// capsdump.exe - dumps what a d3d8.dll reports (caps, adapter identifier, display modes).
// Used to make the D3D11 layer report exactly what the game saw with dgVoodoo.
// Usage: capsdump.exe <path\to\d3d8.dll> <out.txt>
#include <windows.h>
#include <stdio.h>

typedef void*(WINAPI* Create8_t)(UINT);

struct ModeInfo { UINT Width, Height, RefreshRate; DWORD Format; };

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: capsdump <d3d8.dll> <out.txt>\n"); return 1; }
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) { printf("cannot load %s\n", argv[1]); return 1; }
    auto create = (Create8_t)GetProcAddress(dll, "Direct3DCreate8");
    void* d3d = create ? create(220) : nullptr;
    if (!d3d) { printf("Direct3DCreate8 failed\n"); return 1; }
    void** vt = *(void***)d3d;
    FILE* f = nullptr;
    fopen_s(&f, argv[2], "w");
    if (!f) return 1;

    // IDirect3D8: 4 GetAdapterCount, 5 GetAdapterIdentifier, 6 GetAdapterModeCount, 7 EnumAdapterModes,
    // 8 GetAdapterDisplayMode, 13 GetDeviceCaps
    UINT count = ((UINT(WINAPI*)(void*))vt[4])(d3d);
    fprintf(f, "adapters %u\n", count);

    BYTE ident[1200] = {};
    HRESULT hr = ((HRESULT(WINAPI*)(void*, UINT, DWORD, void*))vt[5])(d3d, 0, 0, ident);
    fprintf(f, "identifier_hr 0x%08lx\nidentifier", hr);
    for (int i = 0; i < 1120; i++) fprintf(f, " %02x", ident[i]);
    fprintf(f, "\n");

    BYTE caps[512] = {};
    hr = ((HRESULT(WINAPI*)(void*, UINT, DWORD, void*))vt[13])(d3d, 0, 1 /*HAL*/, caps);
    fprintf(f, "caps_hr 0x%08lx\ncaps", hr);
    for (int i = 0; i < 212; i++) fprintf(f, " %02x", caps[i]);
    fprintf(f, "\n");

    ModeInfo cur = {};
    hr = ((HRESULT(WINAPI*)(void*, UINT, ModeInfo*))vt[8])(d3d, 0, &cur);
    fprintf(f, "display %u %u %u %lu\n", cur.Width, cur.Height, cur.RefreshRate, cur.Format);

    UINT modes = ((UINT(WINAPI*)(void*, UINT))vt[6])(d3d, 0);
    fprintf(f, "modes %u\n", modes);
    for (UINT i = 0; i < modes; i++) {
        ModeInfo m = {};
        if (((HRESULT(WINAPI*)(void*, UINT, UINT, ModeInfo*))vt[7])(d3d, 0, i, &m) >= 0)
            fprintf(f, "mode %u %u %u %lu\n", m.Width, m.Height, m.RefreshRate, m.Format);
    }
    fclose(f);
    printf("ok\n");
    return 0;
}
