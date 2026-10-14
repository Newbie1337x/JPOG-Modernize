// Executes the real controller against a fake native camera; no game is loaded.
#include <windows.h>
static HWND WINAPI TestForeground() { return (HWND)1; }
static HMODULE WINAPI TestModule(LPCSTR) { return nullptr; }
#define GetForegroundWindow TestForeground
#define GetModuleHandleA TestModule
#include "../src/plugin/JPOGMouseFix.cpp"
#include <assert.h>
#include <stdio.h>

alignas(16) static BYTE cameraMemory[0x400] = {};
static int table, manager;
static float renderedPitch;
static BYTE* __stdcall Manager() { return (BYTE*)&manager; }
static BYTE* __fastcall Active(BYTE*, void*) { return cameraMemory; }
static float __fastcall Pitch(BYTE* c, void*) { return *(float*)(c + 0x100); }
static float __fastcall Yaw(BYTE* c, void*) { return *(float*)(c + 0x104); }
static float __fastcall Distance(BYTE* c, void*) { return *(float*)(c + 0x24); }
static void __fastcall SetPitch(BYTE* c, void*, float v, bool) { *(float*)(c + 0x100) = v; }
static void __fastcall SetYaw(BYTE* c, void*, float v, bool, bool) { *(float*)(c + 0x104) = v; }
static void __fastcall SetDistance(BYTE* c, void*, float v, bool) { *(float*)(c + 0x24) = v; }
static void __fastcall Move(BYTE*, void*, float) {}
static void __fastcall Fov(BYTE* c, void*, float v) { *(float*)(c + 0x134) = v; }
static bool __fastcall BaseUpdate(BYTE* c, void*, float)
{
    assert((*(DWORD*)(c + 0x58) & (1u << 8)) != 0); // terrain protection retained
    // Dynamic native max-pitch used to depend on distance; invoke the actual
    // replacement limit bridge, rather than testing only pose arithmetic.
    assert(NativeCameraLimit(c, 3) == 0);
    renderedPitch = *(float*)(c + 0x100);
    return true;
}
int main()
{
    *(void**)cameraMemory = &table;
    *(DWORD*)(cameraMemory + 0x58) = (1u << 6) | (1u << 8) | (1u << 0xe) | (1u << 0x15) | (1u << 0x16);
    *(float*)(cameraMemory + 0x100) = -0.4f;
    *(float*)(cameraMemory + 0x104) = 0.2f;
    *(float*)(cameraMemory + 0x24) = 45;
    g_gameWnd = (HWND)1;
    g_worldCameraVtable = &table;
    g_cameraLimitsReady = true;
    g_worldUpdateTramp = (void*)1; // installed-hook marker; legacy path is never called
    g_getCameraManager = Manager;
    g_getActiveCamera = (ActiveCamera_t)Active;
    g_getCameraPitch = (CameraDistance_t)Pitch;
    g_getCameraRotation = (CameraDistance_t)Yaw;
    g_getCameraDistance = (CameraDistance_t)Distance;
    g_setCameraPitch = (CameraSetFloat_t)SetPitch;
    g_setCameraRotation = (CameraSetRotation_t)SetYaw;
    g_setCameraDistance = (CameraSetFloat_t)SetDistance;
    g_moveCameraForward = g_moveCameraRight = g_moveCameraUp = (CameraInput_t)Move;
    g_setCameraFovDegrees = (CameraInput_t)Fov;
    g_baseCameraUpdate = (CameraUpdate_t)BaseUpdate;
    for (int i = 0; i < 240; ++i) {
        *(float*)(cameraMemory + 0x394) = 10; // zoom out to maximum and keep scrolling
        assert(WorldCameraUpdate_Hook(cameraMemory, nullptr, 1.0f/120));
        assert(renderedPitch == -0.4f);
    }
    assert(*(float*)(cameraMemory + 0x24) == g_cameraMaxDistance);
    for (int i = 0; i < 120; ++i) {
        *(float*)(cameraMemory + 0x390) = 0.1f; // try tilting above horizon
        WorldCameraUpdate_Hook(cameraMemory, nullptr, 1.0f/120);
        assert(renderedPitch <= 0);
    }
    assert(renderedPitch == 0);
    g_followCameraVtable = &manager;
    g_followLimitsReady = true;
    *(void**)cameraMemory = &manager;
    g_focusMinDistance = 1; g_focusMaxDistance = 800;
    assert(NativeCameraLimit(cameraMemory, 0) == 800 && NativeCameraLimit(cameraMemory, 1) == 1);
    for (int i = 0; i < 240; ++i) {
        *(float*)(cameraMemory + 0x394) = 20;
        WorldCameraUpdate_Hook(cameraMemory, nullptr, 1.0f/120);
    }
    assert(*(float*)(cameraMemory + 0x24) == 800);
    JPOGCameraSettings settings = {}; settings.size = sizeof(settings); settings.version = 1;
    assert(JPOGCamera_GetSettings(&settings) && settings.controllerReady && settings.followReady);
    settings.maxDistance = 1200; settings.focusMaxDistance = 5000;
    assert(JPOGCamera_SetSettings(&settings, FALSE));
    assert(g_cameraMaxDistance == 1200 && g_focusMaxDistance == 2000);
    settings.maxDistance = nanf("");
    assert(!JPOGCamera_SetSettings(&settings, FALSE));
    assert(g_cameraMaxDistance == 1200);
    puts("PASS: real camera bridge keeps terrain safety, max zoom angle, and horizon limit");
    puts("PASS: independent selected-unit zoom limits and live GUI settings API");
}
