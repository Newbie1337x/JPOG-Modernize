// JPOGMouseFix.asi - Jurassic Park: Operation Genesis (PC)
//
// Main features (each section below documents its hook site):
//  - Cursor: removes pointer acceleration (Toshi::TGUIScreen::ApplyPointerAccel doubles/quadruples
//    the delta above a threshold) and replaces it with a configurable linear scale with sub-pixel
//    accumulation.
//  - First-person mouse sensitivity slider: lower minimum.
//  - Mouse wheel zoom, camera damping and edge scrolling made framerate independent.
//  - Widescreen: Hor+ FOV, unstretched HUD (4:3 centered or 16:9 anchored) with matching input.
//  - Forced borderless window through Direct3D 8, non-exclusive keyboard, Alt+F4.
//  - Texture replacement from mods\textures.
//
// Config: JPOGMouseFix.ini next to the .asi.

#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <initializer_list>
#include <string>
#include <vector>
#include <unordered_map>
#include <stdarg.h>
#include <xmmintrin.h>
#include "d3d8_profiler.h"
#include "camera_math.h"
#include "camera_settings.h"

static float g_cursorSpeed = 0.5f;
static bool  g_disableAccel = true;
static bool  g_cursorAspectFix = true;
static float g_sliderMin = 0.05f;
static float g_wheelDt = 1.0f / 30.0f;
static bool  g_wheelFix = true;
static bool  g_horPlus = true;
static bool  g_freeKeyboard = true;
static float g_remX = 0.0f, g_remY = 0.0f;
static HWND g_gameWnd = nullptr;
static bool g_modernCamera = true;
static bool g_gameFastPaths = true;
static float g_cameraResponse = 0.12f;
static float g_dragPanSensitivity = 0.0015f;
static float g_dragRotateSensitivity = 0.0018f;
static bool g_rightDragPan = true;              // right button drag pans; a click without movement reaches the game
static bool g_keyboardPan = true;               // W/A/S/D move the park camera like the arrow keys
static float g_keyboardPanSpeed = 1.0f, g_keyboardFast = 2.5f, g_keyboardSlow = 0.35f;
static bool g_proportionalZoom = true;
static bool g_independentCamera = true;
static float g_cameraMinDistance = 2.0f, g_cameraMaxDistance = 250.0f;
static float g_cameraMinPitch = -85.0f, g_cameraMaxPitch = 0.0f;
static float g_cameraFov = 50.0f;
static float g_focusMinDistance = 2.0f, g_focusMaxDistance = 250.0f;
static char g_cameraConfigPath[MAX_PATH] = {};
static bool g_cameraLimitsReady = false, g_followLimitsReady = false;
static void* g_followCameraVtable = nullptr;
static void* g_worldUpdateTramp = nullptr;
static void* g_worldMinDistanceTramp = nullptr;
typedef bool(__thiscall* CameraUpdate_t)(BYTE*, float);
typedef void(__thiscall* CameraSetFloat_t)(BYTE*, float, bool);
typedef void(__thiscall* CameraSetRotation_t)(BYTE*, float, bool, bool);
static CameraUpdate_t g_baseCameraUpdate = nullptr;
static CameraSetFloat_t g_setCameraPitch = nullptr, g_setCameraDistance = nullptr;
static CameraSetRotation_t g_setCameraRotation = nullptr;
typedef BYTE*(__stdcall* CameraManager_t)();
typedef BYTE*(__thiscall* ActiveCamera_t)(BYTE*);
typedef void(__thiscall* CameraInput_t)(BYTE*, float);
typedef float(__thiscall* CameraDistance_t)(BYTE*);
typedef BOOL(WINAPI* CameraInputBlocked_t)();
static CameraInput_t g_moveCameraForward = nullptr, g_moveCameraRight = nullptr, g_moveCameraUp = nullptr;
static CameraInput_t g_setCameraFovDegrees = nullptr;
static CameraDistance_t g_getCameraPitch = nullptr, g_getCameraRotation = nullptr;
struct OwnedCamera {
    BYTE* camera = nullptr;
    CameraMath::OrbitPose pose;
    float oldMinPitch = 0, oldMaxPitch = 0, oldMinDistance = 0, oldMaxDistance = 0, oldFov = 0;
    DWORD oldFlags = 0;
};
static OwnedCamera g_ownedCamera;
static float g_cameraHeightImpulse = 0;
static CameraManager_t g_getCameraManager = nullptr;
static ActiveCamera_t g_getActiveCamera = nullptr;
static CameraDistance_t g_getCameraDistance = nullptr;
static CameraInput_t g_cameraPanRight = nullptr, g_cameraPanForward = nullptr;
static CameraInput_t g_cameraRotateRight = nullptr, g_cameraRotateDown = nullptr;
static void* g_worldCameraVtable = nullptr;
static void* g_scrollingUpdateTramp = nullptr;
static BYTE** g_cameraInputMap = nullptr;
static volatile LONG g_dragDx = 0, g_dragDy = 0, g_dragMode = 0;
// Right button: 0 idle, 1 pressed and hidden from the game until it moves or is released,
// 2 dragging (the press and its release never reach the game).
static volatile LONG g_rightState = 0, g_rightTravel = 0;
const LONG kRightDragThreshold = 6; // raw mouse counts before a right press becomes a pan
static volatile LONG g_cameraPanAllowed = 0, g_cameraRotateAllowed = 0;
static DWORD g_cameraControlTick = 0;

static BYTE* ActiveWorldCamera()
{
    if (!g_getCameraManager || !g_getActiveCamera || !g_worldCameraVtable) return nullptr;
    BYTE* manager = g_getCameraManager();
    BYTE* camera = manager ? g_getActiveCamera(manager) : nullptr;
    return camera && (*(void**)camera == g_worldCameraVtable ||
        (g_followLimitsReady && *(void**)camera == g_followCameraVtable)) ? camera : nullptr;
}

static bool IsFollowCamera(BYTE* camera) { return camera && *(void**)camera == g_followCameraVtable; }
static float CameraMinDistance(BYTE* camera) { return IsFollowCamera(camera) ? g_focusMinDistance : g_cameraMinDistance; }
static float CameraMaxDistance(BYTE* camera) { return IsFollowCamera(camera) ? g_focusMaxDistance : g_cameraMaxDistance; }
struct CameraLimitTable {
    void* table = nullptr;
    CameraDistance_t original[4] = {}; // max distance, min distance, min pitch, max pitch
};
static CameraLimitTable g_worldLimits, g_followLimits;
static float NativeCameraLimit(BYTE* camera, int which)
{
    const CameraLimitTable& table = IsFollowCamera(camera) ? g_followLimits : g_worldLimits;
    if (g_independentCamera && g_cameraLimitsReady && g_worldUpdateTramp && camera == ActiveWorldCamera()) {
        if (which == 0) return CameraMaxDistance(camera);
        if (which == 1) return CameraMinDistance(camera);
        return (which == 2 ? g_cameraMinPitch : g_cameraMaxPitch) * (3.14159265f / 180.0f);
    }
    return table.original[which](camera);
}
static float __fastcall CameraMaxDistance_Hook(BYTE* c, void*) { return NativeCameraLimit(c, 0); }
static float __fastcall CameraMinDistance_Hook(BYTE* c, void*) { return NativeCameraLimit(c, 1); }
static float __fastcall CameraMinPitch_Hook(BYTE* c, void*) { return NativeCameraLimit(c, 2); }
static float __fastcall CameraMaxPitch_Hook(BYTE* c, void*) { return NativeCameraLimit(c, 3); }

extern "C" __declspec(dllexport) BOOL WINAPI JPOGCamera_GetSettings(JPOGCameraSettings* out)
{
    if (!out || out->size != sizeof(*out) || out->version != 1) return FALSE;
    *out = { sizeof(*out), 1, g_cameraMinDistance, g_cameraMaxDistance, g_focusMinDistance, g_focusMaxDistance,
        g_cameraMinPitch, g_cameraMaxPitch, g_cameraFov, g_cameraResponse, g_dragPanSensitivity, g_dragRotateSensitivity,
        g_worldUpdateTramp && g_cameraLimitsReady && g_independentCamera, g_followLimitsReady };
    return TRUE;
}
extern "C" __declspec(dllexport) BOOL WINAPI JPOGCamera_SetSettings(const JPOGCameraSettings* in, BOOL save)
{
    if (!in || in->size != sizeof(*in) || in->version != 1) return FALSE;
    const float values[] = { in->minDistance, in->maxDistance, in->focusMinDistance, in->focusMaxDistance,
        in->minPitch, in->maxPitch, in->fixedFov, in->responseSeconds, in->panSensitivity, in->rotateSensitivity };
    for (float value : values) if (!_finite(value)) return FALSE;
    g_cameraMinDistance = CameraMath::Clamp(in->minDistance, 0.5f, 100);
    g_cameraMaxDistance = CameraMath::Clamp(in->maxDistance, g_cameraMinDistance + 1, 2000);
    g_focusMinDistance = CameraMath::Clamp(in->focusMinDistance, 0.5f, 100);
    g_focusMaxDistance = CameraMath::Clamp(in->focusMaxDistance, g_focusMinDistance + 1, 2000);
    g_cameraMinPitch = CameraMath::Clamp(in->minPitch, -89, -5);
    g_cameraMaxPitch = CameraMath::Clamp(in->maxPitch, -10, 0);
    if (g_cameraMaxPitch <= g_cameraMinPitch) g_cameraMaxPitch = g_cameraMinPitch + 1;
    g_cameraFov = CameraMath::Clamp(in->fixedFov, 25, 90);
    g_cameraResponse = CameraMath::Clamp(in->responseSeconds, 0.03f, 0.5f);
    g_dragPanSensitivity = CameraMath::Clamp(in->panSensitivity, 0.0001f, 0.02f);
    g_dragRotateSensitivity = CameraMath::Clamp(in->rotateSensitivity, 0.0001f, 0.02f);
    if (!save) return TRUE;
    const char* keys[] = { "MinDistance", "MaxDistance", "FocusMinDistance", "FocusMaxDistance", "MinPitch", "MaxPitch",
        "FixedFOV", "ResponseSeconds", "DragPanSensitivity", "DragRotateSensitivity" };
    const float applied[] = { g_cameraMinDistance, g_cameraMaxDistance, g_focusMinDistance, g_focusMaxDistance,
        g_cameraMinPitch, g_cameraMaxPitch, g_cameraFov, g_cameraResponse, g_dragPanSensitivity, g_dragRotateSensitivity };
    BOOL ok = TRUE;
    for (int i = 0; i < 10; ++i) {
        char value[32]; sprintf_s(value, "%.6g", applied[i]);
        if (!WritePrivateProfileStringA("Camera", keys[i], value, g_cameraConfigPath)) ok = FALSE;
    }
    return ok;
}

static bool CameraControlsBlocked()
{
    if (!g_gameWnd || GetForegroundWindow() != g_gameWnd) return true;
    HMODULE renderer = GetModuleHandleA("d3d8.dll");
    CameraInputBlocked_t blocked = renderer ? (CameraInputBlocked_t)GetProcAddress(renderer, "D3D8to11_InputBlocked") : nullptr;
    return blocked && blocked();
}

static float __fastcall WorldMinDistance_Hook(BYTE* camera, void*)
{
    if (g_independentCamera && g_worldUpdateTramp && camera == ActiveWorldCamera()) return CameraMinDistance(camera);
    return ((CameraDistance_t)g_worldMinDistanceTramp)(camera);
}

static bool CameraGesturesReady()
{
    return g_modernCamera && g_scrollingUpdateTramp && !CameraControlsBlocked() &&
        GetTickCount() - g_cameraControlTick <= 100 && ActiveWorldCamera();
}

// Right button events from the proxy's buffered read. Returns 0 to pass the event to the game,
// 1 to drop it, 2 to deliver it as a click (the proxy turns the release into a press and sends
// the release on the next read): a right click without movement keeps its game meaning.
extern "C" __declspec(dllexport) DWORD WINAPI JPOGCamera_RightButton(BOOL down)
{
    if (down) {
        if (g_rightDragPan && CameraGesturesReady() && g_cameraPanAllowed) {
            InterlockedExchange(&g_rightTravel, 0);
            InterlockedExchange(&g_rightState, 1);
            return 1;
        }
        InterlockedExchange(&g_rightState, 0);
        return 0;
    }
    const LONG state = InterlockedExchange(&g_rightState, 0);
    return state == 2 ? 1 : state == 1 ? 2 : 0;
}

// Raw DirectInput feed from our proxy: no Windows cursor warping or edge clamp.
// Return TRUE only for a camera drag; the proxy then removes the axes and the middle button.
// Middle drag orbits and tilts (Shift + middle pans, Alt + middle changes height); a right
// drag pans.
extern "C" __declspec(dllexport) BOOL WINAPI JPOGCamera_MouseInput(LONG dx, LONG dy, LONG /*wheel*/, DWORD buttons)
{
    if (g_rightState == 1 && (buttons & 2)) {
        const LONG travel = InterlockedExchangeAdd(&g_rightTravel, labs(dx) + labs(dy)) + labs(dx) + labs(dy);
        if (travel >= kRightDragThreshold) InterlockedCompareExchange(&g_rightState, 2, 1);
    }
    const bool rightDrag = g_rightState == 2 && (buttons & 2);
    const bool middle = (buttons & 4) != 0;
    const bool height = middle && g_worldUpdateTramp && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool shiftPan = middle && !height && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool rotate = middle && !height && !shiftPan && !rightDrag;
    const LONG mode = height ? 3 : rotate ? 2 : 1;
    if (!CameraGesturesReady() || !(middle || rightDrag) ||
        !(rotate ? g_cameraRotateAllowed : g_cameraPanAllowed)) {
        InterlockedExchange(&g_dragMode, 0);
        InterlockedExchange(&g_dragDx, 0);
        InterlockedExchange(&g_dragDy, 0);
        return FALSE;
    }
    if (InterlockedExchange(&g_dragMode, mode) != mode) {
        InterlockedExchange(&g_dragDx, 0);
        InterlockedExchange(&g_dragDy, 0);
    }
    // Bound corrupt/stale input, while keeping normal fast drags intact.
    if (dx < -4096) dx = -4096; else if (dx > 4096) dx = 4096;
    if (dy < -4096) dy = -4096; else if (dy > 4096) dy = 4096;
    InterlockedExchangeAdd(&g_dragDx, dx);
    InterlockedExchangeAdd(&g_dragDy, dy);
    return TRUE;
}

static bool __fastcall ScrollingUpdate_Hook(BYTE* state, void*, float dt)
{
    BYTE* camera = ActiveWorldCamera();
    BYTE* inputMap = g_cameraInputMap ? *g_cameraInputMap : nullptr;
    const bool allowed = g_modernCamera && camera && inputMap &&
        (*(DWORD*)(inputMap + 0x78) & 2) && !CameraControlsBlocked() && dt > 0 && dt < 0.25f;
    g_cameraPanAllowed = allowed && state[0x48] != 0;
    g_cameraRotateAllowed = allowed && state[0x4a] != 0;
    g_cameraControlTick = allowed ? GetTickCount() : 0;
    const LONG mode = InterlockedCompareExchange(&g_dragMode, 0, 0);
    const LONG dx = InterlockedExchange(&g_dragDx, 0);
    const LONG dy = InterlockedExchange(&g_dragDy, 0);
    const bool dragging = allowed && ((mode == 1 || mode == 3) ? g_cameraPanAllowed : mode == 2 && g_cameraRotateAllowed);
    const BYTE translate = state[0x48], rotate = state[0x4a];
    if (dragging) { state[0x48] = 0; state[0x4a] = 0; }
    const bool result = ((bool(__thiscall*)(BYTE*, float))g_scrollingUpdateTramp)(state, dt);
    state[0x48] = translate;
    state[0x4a] = rotate;
    // W/A/S/D: same impulse as the game's arrow keys (frame time, scaled by the camera's
    // height-dependent speed); Shift faster, Ctrl slower. Alt is left to the game.
    if (g_keyboardPan && allowed && g_cameraPanAllowed && !(dragging && mode != 2) &&
        !(GetAsyncKeyState(VK_MENU) & 0x8000)) {
        auto held = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
        const float forward = (held('W') ? 1.0f : 0.0f) - (held('S') ? 1.0f : 0.0f);
        const float right = (held('D') ? 1.0f : 0.0f) - (held('A') ? 1.0f : 0.0f);
        if (forward != 0 || right != 0) {
            float speed = dt * g_keyboardPanSpeed;
            if (held(VK_SHIFT)) speed *= g_keyboardFast;
            else if (held(VK_CONTROL)) speed *= g_keyboardSlow;
            if (forward != 0) g_cameraPanForward(camera, forward * speed);
            if (right != 0) g_cameraPanRight(camera, right * speed);
        }
    }
    if (dragging) {
        // Native input methods generate displacement impulses. Mouse deltas are
        // already displacement, so NEVER multiply them by frame dt again.
        if (mode == 1) {
            if (dx) g_cameraPanRight(camera, -dx * g_dragPanSensitivity);
            if (dy) g_cameraPanForward(camera, dy * g_dragPanSensitivity);
        } else if (mode == 2) {
            if (dx) g_cameraRotateRight(camera, dx * g_dragRotateSensitivity);
            if (dy) g_cameraRotateDown(camera, dy * g_dragRotateSensitivity);
        } else {
            g_cameraHeightImpulse += -dy * 0.05f;
        }
    } else InterlockedExchange(&g_dragMode, 0);
    return result;
}

// Replacement controller: native AWorldCamera::OnUpdate is NOT called for the
// active park camera. Only ACamera's matrix/listener/picking bridge is retained.
static bool __fastcall WorldCameraUpdate_Hook(BYTE* camera, void*, float dt)
{
    const DWORD automaticFlags = (1u << 6) | (1u << 0x15) | (1u << 0x16);
    if (!g_independentCamera || !g_cameraLimitsReady || camera != ActiveWorldCamera()) {
        if (g_ownedCamera.camera == camera) {
            *(float*)(camera + 0x3c0) = g_ownedCamera.oldMinPitch;
            *(float*)(camera + 0x3c4) = g_ownedCamera.oldMaxPitch;
            *(float*)(camera + 0x3b8) = g_ownedCamera.oldMinDistance;
            *(float*)(camera + 0x3bc) = g_ownedCamera.oldMaxDistance;
            *(float*)(camera + 0x134) = g_ownedCamera.oldFov;
            DWORD& flags = *(DWORD*)(camera + 0x58);
            flags = (flags & ~automaticFlags) | (g_ownedCamera.oldFlags & automaticFlags);
            g_ownedCamera = OwnedCamera();
        }
        g_cameraHeightImpulse = 0;
        return ((CameraUpdate_t)g_worldUpdateTramp)(camera, dt);
    }
    if (g_ownedCamera.camera != camera) {
        g_ownedCamera = OwnedCamera();
        g_ownedCamera.camera = camera;
        g_ownedCamera.pose.pitch = g_getCameraPitch(camera);
        g_ownedCamera.pose.yaw = g_getCameraRotation(camera);
        g_ownedCamera.pose.distance = g_getCameraDistance(camera);
        g_ownedCamera.oldMinPitch = *(float*)(camera + 0x3c0);
        g_ownedCamera.oldMaxPitch = *(float*)(camera + 0x3c4);
        g_ownedCamera.oldMinDistance = *(float*)(camera + 0x3b8);
        g_ownedCamera.oldMaxDistance = *(float*)(camera + 0x3bc);
        g_ownedCamera.oldFov = *(float*)(camera + 0x134);
        g_ownedCamera.oldFlags = *(DWORD*)(camera + 0x58);
    }
    const float radians = 3.14159265f / 180.0f;
    *(float*)(camera + 0x3c0) = g_cameraMinPitch * radians;
    *(float*)(camera + 0x3c4) = g_cameraMaxPitch * radians;
    const float minDistance = CameraMinDistance(camera), maxDistance = CameraMaxDistance(camera);
    *(float*)(camera + 0x3b8) = minDistance;
    *(float*)(camera + 0x3bc) = maxDistance;
    // Default-pitch lock, focal-ground deflection and altitude-driven FOV off.
    // Keep flag 8: GetPosition still lifts the eye above terrain when necessary.
    *(DWORD*)(camera + 0x58) &= ~automaticFlags;
    float input[6];
    for (int i = 0; i < 5; ++i) {
        input[i] = *(float*)(camera + 0x384 + i * 4);
        *(float*)(camera + 0x384 + i * 4) = 0;
        *(float*)(camera + 0x370 + i * 4) = 0; // old per-frame momentum unused
    }
    input[5] = g_cameraHeightImpulse;
    g_cameraHeightImpulse = 0;
    if (CameraControlsBlocked()) {
        memset(input, 0, sizeof(input));
        memset(g_ownedCamera.pose.velocity, 0, sizeof(g_ownedCamera.pose.velocity));
    }
    float movement[3];
    CameraMath::StepOrbit(g_ownedCamera.pose, input, dt, g_cameraResponse,
        g_cameraMinPitch * radians, g_cameraMaxPitch * radians,
        minDistance, maxDistance, movement);
    // Cancel native distance/orientation interpolation. Focal transitions (e.g.
    // selecting a point of interest) still use the engine's positional bridge.
    *(DWORD*)(camera + 0x58) &= ~((1u << 1) | (1u << 2));
    g_setCameraRotation(camera, g_ownedCamera.pose.yaw, false, false);
    g_setCameraPitch(camera, g_ownedCamera.pose.pitch, false);
    g_setCameraDistance(camera, g_ownedCamera.pose.distance, false);
    if (movement[0]) g_moveCameraForward(camera, movement[0]);
    if (movement[1]) g_moveCameraRight(camera, movement[1]);
    if (movement[2]) g_moveCameraUp(camera, movement[2]);
    g_setCameraFovDegrees(camera, g_cameraFov);
    return g_baseCameraUpdate(camera, dt);
}

static const float g_four3 = 4.0f / 3.0f;
static const float g_half = 0.5f;

static bool PatchBytes(void* addr, const void* data, size_t len)
{
    DWORD old;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(addr, data, len);
    VirtualProtect(addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
    return true;
}

static bool BytesMatch(const void* addr, const unsigned char* expected, size_t len)
{
    __try {
        return memcmp(addr, expected, len) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool InstallCameraLimits(HMODULE exe, void* vtable, const char* className, CameraLimitTable& state)
{
    if (!vtable) return false;
    const int offsets[] = { 0x9c, 0xa0, 0xa8, 0xac };
    const char* methods[] = { "ConstMaxDistance", "ConstMinDistance", "ConstMinPitch", "ConstMaxPitch" };
    void* hooks[] = { (void*)&CameraMaxDistance_Hook, (void*)&CameraMinDistance_Hook,
        (void*)&CameraMinPitch_Hook, (void*)&CameraMaxPitch_Hook };
    state.table = vtable;
    for (int i = 0; i < 4; ++i) {
        char symbol[128]; sprintf_s(symbol, "?%s@%s@@UBEMXZ", methods[i], className);
        void* fn = (void*)GetProcAddress(exe, symbol);
        void** slot = (void**)((BYTE*)vtable + offsets[i]);
        if (!fn || *slot != fn) return false;
        state.original[i] = (CameraDistance_t)fn;
    }
    for (int i = 0; i < 4; ++i) {
        void** slot = (void**)((BYTE*)vtable + offsets[i]);
        if (!PatchBytes(slot, &hooks[i], sizeof(void*))) {
            for (int j = 0; j < i; ++j)
                PatchBytes((BYTE*)vtable + offsets[j], &state.original[j], sizeof(void*));
            return false;
        }
    }
    return true;
}

static void WriteRel(void* from, void* to, unsigned char opcode, size_t totalLen)
{
    unsigned char buf[16];
    memset(buf, 0x90, sizeof(buf));
    buf[0] = opcode; // 0xE9 jmp / 0xE8 call
    *(DWORD*)(buf + 1) = (DWORD)((BYTE*)to - (BYTE*)from - 5);
    PatchBytes(from, buf, totalLen);
}

// ---------------------------------------------------------------------------------------------
// Cursor

static int Scale(int delta, float& remainder, float speed)
{
    float v = delta * speed + remainder;
    int whole = (int)v; // truncates toward zero
    remainder = v - whole;
    return whole;
}

typedef void(__stdcall* GetScreenSize_t)(float* outVec2);
static GetScreenSize_t GetGuiScreenSize = (GetScreenSize_t)0x50bce0;    // ACamera::GetGuiScreenSize
static GetScreenSize_t GetRenderScreenSize = (GetScreenSize_t)0x50bd30; // ACamera::GetRenderScreenSize

// X factor that makes one mouse count move the same number of pixels on X and Y.
static float CursorAspectFactor()
{
    float gui[2] = {}, ren[2] = {};
    __try {
        GetGuiScreenSize(gui);
        GetRenderScreenSize(ren);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1.0f;
    }
    if (gui[0] <= 0 || gui[1] <= 0 || ren[0] <= 0 || ren[1] <= 0)
        return 1.0f;
    float f = (gui[0] / gui[1]) / (ren[0] / ren[1]);
    return (f > 0.25f && f < 4.0f) ? f : 1.0f;
}

// ---------------------------------------------------------------------------------------------
// Centered 4:3 HUD (PGUIRenderer.dll, PGUITRDisplayContext::BeginScene)
//
// The GUI is drawn in virtual 4:3 coordinates with an ortho projection that stretches them to the
// full width. After BeginScene, the base of the matrix stack (this+0x230 + idx*0x40) is identity
// and every Push* composes on top of it. It is replaced with X scale = f and X translation =
// (1-f)/2 * guiWidth, and sent to the render context (this+0x21c, vtable+0x7c) like PushScale does.
static bool g_fixHud = true;
static void* g_beginSceneTramp = nullptr;

// HUD X scale (1 = unchanged). Only applies to screens wider than the GUI (4:3).
static float HudFactor(float* gui, float* ren)
{
    gui[0] = gui[1] = ren[0] = ren[1] = 0.0f;
    if (!g_fixHud) return 1.0f;
    __try {
        GetGuiScreenSize(gui);
        GetRenderScreenSize(ren);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1.0f;
    }
    if (gui[0] <= 0 || gui[1] <= 0 || ren[0] <= 0 || ren[1] <= 0) return 1.0f;
    const float f = (gui[0] / gui[1]) / (ren[0] / ren[1]);
    return (f < 0.999f && f > 0.25f) ? f : 1.0f;
}

// GUI X coordinate (centered HUD) -> X of the GUI "stretched" to the full screen (0..W).
static float GuiToFullX(float x, float f, float w) { return f * x + (1.0f - f) * 0.5f * w; }

// Replacement for TGUIScreen::SetPointerPosition(int x, int y) (TGuiInterface, thiscall, ret 8).
// The original clamps X to [0, W-1]; with the centered HUD the range covers the whole screen.
static bool __fastcall SetPointerPosition_Hook(BYTE* screen, void* /*edx*/, int x, int y)
{
    const int w = *(short*)(screen + 0x70);
    const int h = *(short*)(screen + 0x72);
    int lo = 0, hi = w - 1;
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    if (f < 1.0f) {
        const int ext = (int)((1.0f - f) * 0.5f * w / f);
        lo -= ext;
        hi += ext;
    }
    if (x > hi) x = hi;
    if (x < lo) x = lo;
    if (y > h - 1) y = h - 1;
    if (y < 0) y = 0;
    int* px = (int*)(screen + 0x114);
    int* py = (int*)(screen + 0x118);
    if (*px == x && *py == y) return false;
    *px = x;
    *py = y;
    return true;
}

// Replacement for ACamera::ConvertGuiScreenToRenderScreen(const TVector2& gui, TVector2& out)
// (SimJP 0x50bd60, stdcall, ret 8): same mapping as the centered HUD, so clicks in the 3D world
// land where the cursor is drawn.
static void __stdcall ConvertGuiToRender_Hook(const float* in, float* out)
{
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    if (gui[0] <= 0 || gui[1] <= 0) { out[0] = in[0]; out[1] = in[1]; return; }
    float x = GuiToFullX(in[0], f, gui[0]) * ren[0] / gui[0];
    float y = in[1] * ren[1] / gui[1];
    out[0] = x < 0 ? 0 : (x > ren[0] ? ren[0] : x);
    out[1] = y < 0 ? 0 : (y > ren[1] ? ren[1] : y);
}

// Inverse: ACamera::ConvertRenderScreenToGuiScreen(const TVector2& ren, TVector2& gui) (0x50bdd0).
static void __stdcall ConvertRenderToGui_Hook(const float* in, float* out)
{
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    if (ren[0] <= 0 || ren[1] <= 0) { out[0] = in[0]; out[1] = in[1]; return; }
    const float full = in[0] * gui[0] / ren[0];
    out[0] = f < 1.0f ? (full - (1.0f - f) * 0.5f * gui[0]) / f : full;
    out[1] = in[1] * gui[1] / ren[1];
}

// World -> GUI: ARenderer::ConvertToScreenCoords(TVector3&) (0x477ec0) returns stretched GUI
// coordinates (and scales Y by the width). They are converted to the centered HUD space so icons
// over dinos/buildings and "what is under the cursor" match what is on screen.
static void* g_toScreenTramp = nullptr;
static void __fastcall ToScreenCoords_Hook(void* rnd, void*, float* v)
{
    ((void(__thiscall*)(void*, float*))g_toScreenTramp)(rnd, v);
    if (v[0] == -100.0f && v[1] == -100.0f) return; // behind the camera
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    if (f >= 1.0f || gui[0] <= 0) return;
    v[0] = (v[0] - (1.0f - f) * 0.5f * gui[0]) / f;
    v[1] = v[1] / f;
}

// Edge scrolling (AScrollingState::OnUpdate 0x55cbe5: mov ebx,[edi+0x114]).
// Replaced with "call EdgeXStub": EBX = cursor X mapped to the full-screen 0..W range.
static int __cdecl EdgeX(int x)
{
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    return f < 1.0f ? (int)GuiToFullX((float)x, f, gui[0]) : x;
}

__declspec(naked) static void EdgeXStub()
{
    __asm {
        push eax
        push ecx
        push edx
        push dword ptr [edi + 0x114]
        call EdgeX
        add esp, 4
        mov ebx, eax
        pop edx
        pop ecx
        pop eax
        ret
    }
}

// ---------------------------------------------------------------------------------------------
// HUD anchored to the screen edges (TGUIScreen::Paint, TGuiInterface)
//
// With the HUD centered in 4:3, panels attached to an edge of the 4:3 area (money, counters, build
// menu...) are moved to the real edges of the 16:9 screen by changing their position
// (TGUIWidget +0x6c x, +0x70 width). Cursor and clicks use the same coordinates, so hit testing
// still matches. While a visible full-screen panel exists (menus, Dinopedia...) nothing is
// anchored and positions are restored.
// Children: first node at +0x50, sentinel at +0x44, next at +0xc. Visible: flags +0x88 & 4.
static int g_hudMode = 2; // 0 = stretched, 1 = 4:3 centered, 2 = 16:9 anchored
static bool g_minimapEnhanced = true;
static bool g_minimapHooksReady = false;
static int g_minimapSize = 128;
static int g_minimapMargin = 16;
static int g_minimapBottom = 56; // space for the native mail button
static void* g_minimapCtorTramp = nullptr;
static void* g_minimapPaintTramp = nullptr;
static int HudExtent(int W);

// AMinimap is a native HUD element, NOT a TGUIWidget. REA verified its
// unsigned x/y (3a/3c), texture width/height (26/28) and terrain sampling shift
// (2a). SetSize is a no-op. Resize BEFORE Init allocates the CPU/GPU texture;
// never modify dimensions on a live map (Refresh uses them as buffer strides).
static BYTE* __fastcall MinimapCtor_Hook(BYTE* self, void*)
{
    BYTE* result = ((BYTE*(__thiscall*)(BYTE*))g_minimapCtorTramp)(self);
    const unsigned short shift = g_minimapSize == 256 ? 2 : g_minimapSize == 128 ? 3 : 4;
    *(unsigned short*)(result + 0x26) = (unsigned short)g_minimapSize;
    *(unsigned short*)(result + 0x28) = (unsigned short)g_minimapSize;
    *(unsigned short*)(result + 0x2a) = shift; // same 1024-cell coverage, finer samples
    return result;
}

static void PlaceNativeMinimap(BYTE* map, int W, int H)
{
    if (W < 320 || H < 240) return;
    const int width = *(unsigned short*)(map + 0x26);
    const int height = *(unsigned short*)(map + 0x28);
    // Unsigned native coordinates cannot represent the negative left extension.
    // The right extension is positive and uses the existing widescreen transform.
    int x = W + HudExtent(W) - width - g_minimapMargin;
    int y = H - height - g_minimapBottom;
    if (x < 0) x = 0;
    if (x > 32767) x = 32767;
    if (y < 0) y = 0;
    *(unsigned short*)(map + 0x3a) = (unsigned short)x;
    *(unsigned short*)(map + 0x3c) = (unsigned short)y;
}

static void __fastcall MinimapPosition_Hook(BYTE* map, void*, int x, int y)
{
    *(unsigned short*)(map + 0x3a) = (unsigned short)x;
    *(unsigned short*)(map + 0x3c) = (unsigned short)y;
    float gui[2] = {};
    GetGuiScreenSize(gui);
    PlaceNativeMinimap(map, (int)gui[0], (int)gui[1]);
}

static bool __stdcall MinimapPaint_Hook(BYTE* map, BYTE* screen, void* event)
{
    PlaceNativeMinimap(map, *(short*)(screen + 0x70), *(short*)(screen + 0x72));
    // Original paint reads these same fields for terrain AND camera marker;
    // GetBBox/GetPosition also use them. Keep native visibility/events intact.
    return ((bool(__stdcall*)(BYTE*, BYTE*, void*))g_minimapPaintTramp)(map, screen, event);
}
static void* g_screenPaintTramp = nullptr;
struct AnchorState { short origX, setX; };
static std::unordered_map<BYTE*, AnchorState>* g_anchors = nullptr;
static const int kEdgeSnap = 48; // a panel within 48 units of a 4:3 edge counts as anchored

// Popup menus (PGUIMenuDialog / PGUIDiscMenuDialog::UpdatePosition, PGUICore): the game places
// them next to the button that opened them and clamps them to [0, W]. With the anchored HUD that
// button can be outside that range, so a wider screen is simulated during the call.
// These panels are excluded from anchoring while they are visible.
static void* g_menuUpdPosTramp = nullptr;
static void* g_discUpdPosTramp = nullptr;
static BYTE* g_lastScreen = nullptr;
static unsigned g_paintFrame = 0;
static std::unordered_map<BYTE*, unsigned>* g_selfPositioned = nullptr;

static int HudExtent(int W)
{
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    return (f < 1.0f && g_hudMode == 2) ? (int)((1.0f - f) * 0.5f * W / f) : 0;
}

static void UpdatePositionWide(void* tramp, BYTE* dlg, int* px, int* py)
{
    typedef void(__thiscall* Fn)(BYTE*, int*, int*);
    BYTE* screen = g_lastScreen;
    const int ext = screen ? HudExtent(*(short*)(screen + 0x70)) : 0;
    if (!g_selfPositioned) g_selfPositioned = new std::unordered_map<BYTE*, unsigned>();
    (*g_selfPositioned)[dlg] = g_paintFrame;
    if (!ext) { ((Fn)tramp)(dlg, px, py); return; }
    short& sw = *(short*)(screen + 0x70);
    const short oldW = sw;
    *px += ext;
    sw = (short)(oldW + 2 * ext);
    ((Fn)tramp)(dlg, px, py);
    sw = oldW;
    *(short*)(dlg + 0x6c) -= (short)ext;
    *px -= ext;
}

static void __fastcall MenuUpdatePosition_Hook(BYTE* dlg, void*, int* px, int* py)
{
    UpdatePositionWide(g_menuUpdPosTramp, dlg, px, py);
}

static void __fastcall DiscUpdatePosition_Hook(BYTE* dlg, void*, int* px, int* py)
{
    UpdatePositionWide(g_discUpdPosTramp, dlg, px, py);
}

static bool IsSelfPositioned(BYTE* w)
{
    if (!g_selfPositioned) return false;
    auto it = g_selfPositioned->find(w);
    if (it == g_selfPositioned->end()) return false;
    // While visible the game positions it; once hidden it is forgotten (the memory may be reused).
    if (!(*(DWORD*)(w + 0x88) & 4) && g_paintFrame - it->second > 2) { g_selfPositioned->erase(it); return false; }
    return true;
}

static void AnchorHud(BYTE* screen)
{
    g_lastScreen = screen;
    g_paintFrame++;
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    const int W = *(short*)(screen + 0x70), H = *(short*)(screen + 0x72);
    if (!g_anchors) g_anchors = new std::unordered_map<BYTE*, AnchorState>();
    const int ext = HudExtent(W);

    BYTE* sentinel = screen + 0x44;
    bool fullscreen = false;
    int guard = 0;
    for (BYTE* w = *(BYTE**)(screen + 0x50); w && w != sentinel && guard < 512; w = *(BYTE**)(w + 0xc), guard++) {
        if (!(*(DWORD*)(w + 0x88) & 4)) continue;
        if (*(short*)(w + 0x70) >= W - 40 && *(short*)(w + 0x72) >= H - 80) { fullscreen = true; break; }
    }
    guard = 0;
    for (BYTE* w = *(BYTE**)(screen + 0x50); w && w != sentinel && guard < 512; w = *(BYTE**)(w + 0xc), guard++) {
        if (IsSelfPositioned(w)) { g_anchors->erase(w); continue; }
        short& x = *(short*)(w + 0x6c);
        const short width = *(short*)(w + 0x70);
        auto it = g_anchors->find(w);
        if (it == g_anchors->end() || x != it->second.setX) {
            AnchorState s = { x, x };
            it = g_anchors->insert_or_assign(w, s).first;
        }
        AnchorState& s = it->second;
        short target = s.origX;
        // Only small panels: wide ones (e.g. the message bar) stay with the centered content.
        if (ext && !fullscreen && width <= W / 2 && s.origX >= 0 && s.origX + width <= W) {
            const bool nearLeft = s.origX <= kEdgeSnap;
            const bool nearRight = s.origX + width >= W - kEdgeSnap;
            if (nearLeft && !nearRight) target = (short)(s.origX - ext);
            else if (nearRight && !nearLeft) target = (short)(s.origX + ext);
        }
        x = target;
        s.setX = target;
    }
}

static void __fastcall ScreenPaint_Hook(BYTE* screen, void* /*edx*/, void* widget)
{
    __try {
        AnchorHud(screen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    ((void(__thiscall*)(BYTE*, void*))g_screenPaintTramp)(screen, widget);
}

// 3D views inside the GUI (building preview, Dinopedia...):
// AGUIRenderObject::SetViewRectangle (SimJP 0x678a90) converts the GUI rectangle to pixels with
// the stretched scale. It is given the rectangle already mapped to the centered/anchored HUD.
static void* g_setViewRectTramp = nullptr;

static void __fastcall SetViewRectangle_Hook(BYTE* obj, void*, const int* rect)
{
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    int r[4] = { rect[0], rect[1], rect[2], rect[3] };
    if (f < 1.0f && gui[0] > 0) {
        r[0] = (int)floorf(GuiToFullX((float)rect[0], f, gui[0]) + 0.5f);
        r[2] = (int)floorf(rect[2] * f + 0.5f);
    }
    ((void(__thiscall*)(BYTE*, const int*))g_setViewRectTramp)(obj, r);
}

static void __fastcall BeginScene_Hook(BYTE* ctx, void* /*edx*/)
{
    ((void(__thiscall*)(BYTE*))g_beginSceneTramp)(ctx);
    float gui[2], ren[2];
    const float f = HudFactor(gui, ren);
    if (f >= 1.0f) return;
    const int idx = *(int*)(ctx + 0x22c);
    float* m = (float*)(ctx + 0x230 + idx * 0x40);
    m[0] *= f; m[1] *= f; m[2] *= f;          // row 0: X axis
    m[12] += (1.0f - f) * 0.5f * gui[0];      // row 3: X translation
    void* rc = *(void**)(ctx + 0x21c);
    void** vt = *(void***)rc;
    ((void(__thiscall*)(void*, float*))vt[0x7c / 4])(rc, m);
}

// Replacement for: void __thiscall TGUIScreen::ApplyPointerAccel(int& dx, int& dy)  (ret 8)
// __fastcall: ECX = this, EDX = unused, 2 stack args cleaned by the callee -> ret 8.
static void __fastcall ApplyPointerAccel_Hook(void* screen, void* /*edx*/, int* dx, int* dy)
{
    if (!g_disableAccel) {
        // Original behaviour: threshold at +0x138, mode at +0x140 (0 = off, 1 = x2, 2 = x4)
        const int threshold = *(int*)((BYTE*)screen + 0x138);
        const int mode = *(int*)((BYTE*)screen + 0x140);
        for (int* d : { dx, dy }) {
            if (abs(*d) > threshold && mode != 0) *d *= 2;
            if (abs(*d) > threshold && mode == 2) *d *= 2;
        }
    }
    // With the 4:3 centered HUD, GUI units are already square on screen.
    const float fx = (g_cursorAspectFix && !g_fixHud) ? CursorAspectFactor() : 1.0f;
    *dx = Scale(*dx, g_remX, g_cursorSpeed * fx);
    *dy = Scale(*dy, g_remY, g_cursorSpeed);
}

// ---------------------------------------------------------------------------------------------
// Mouse wheel zoom (SimJP.exe, AScrollingState::ProcessInput)
//
// The zoom impulse is dt * 2 per wheel click, so higher FPS means less zoom. Original site (twice,
// zoom in and zoom out):
//   fld dword ptr [esp]     ; frame dt
//   mov edx, [ebx]          ; camera vtable
// Replaced with "call ZoomDtStub". EBP = InputEvent*, whose first field is the doodad. Wheel
// events use a fixed dt.

static const int DOODAD_WHEEL_FORWARD = 0x30009;
static const int DOODAD_WHEEL_BACKWARD = 0x3000a;

static float __cdecl AdjustZoomDt(const int* inputEvent, float dt)
{
    __try {
        const int doodad = inputEvent[0];
        if (doodad == DOODAD_WHEEL_FORWARD || doodad == DOODAD_WHEEL_BACKWARD)
        {
            BYTE* camera = g_modernCamera && g_proportionalZoom ? ActiveWorldCamera() : nullptr;
            if (camera && g_getCameraDistance) {
                const float ratio = CameraMath::ZoomRatio(g_getCameraDistance(camera));
                return g_wheelDt * ratio;
            }
            return g_wheelDt;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return dt;
}

__declspec(naked) static void ZoomDtStub()
{
    __asm {
        mov edx, dword ptr [esp + 4] // caller's [esp] (dt), shifted by the return address
        push edx
        push ebp
        call AdjustZoomDt            // returns the float in ST0, like the original fld
        add esp, 8
        mov edx, dword ptr [ebx]
        ret
    }
}

// ---------------------------------------------------------------------------------------------
// Framerate independent camera smoothing (SimJP.exe, AWorldCamera::OnUpdate)
//
// Per frame: v = v * scm_fDampening + impulse * scm_fOMDampening. The impulse is already dt * k, so
// the final speed does not depend on FPS, but acceleration/braking lasts N frames: the higher the
// FPS, the more abrupt it gets (edge/key panning, rotation and zoom).
// Both loads are replaced by stubs that return, with dt in EDI:
//   D'  = D ^ (dt * RefFPS)
//   OM' = OM * (1 - D') / (1 - D)     (keeps the final speed)

static float* const scm_fDampening = (float*)0xd12c00;
static float* const scm_fOMDampening = (float*)0x8b61f0;
static float g_refFps = 30.0f;
static bool  g_smoothFix = true;

// Edge scrolling speed (AScrollingState::OnUpdate). Constants used only there:
// 0xd1a954 = forward/back (original 2.0), 0xd1a958 = sideways (original 1.0).
static float* const kEdgeVertical = (float*)0xd1a954;
static float* const kEdgeHorizontal = (float*)0xd1a958;
static float g_edgeVertical = 1.5f;
static float g_edgeHorizontal = 1.5f;

static float ClampDt(float dt)
{
    if (!(dt > 0.0f)) return 0.0f;
    return dt > 0.25f ? 0.25f : dt;
}

static float __cdecl DampeningFor(float dt)
{
    const float d = *scm_fDampening;
    if (d <= 0.0f || d >= 1.0f) return d;
    if (g_modernCamera) return CameraMath::ResponseDamping(dt, g_cameraResponse);
    return powf(d, ClampDt(dt) * g_refFps);
}

static float __cdecl OMDampeningFor(float dt)
{
    const float d = *scm_fDampening;
    const float om = *scm_fOMDampening;
    if (d <= 0.0f || d >= 1.0f) return om;
    const float dEff = DampeningFor(dt);
    return om * (1.0f - dEff) / (1.0f - d);
}

// Sites: 0x527d3f fld [scm_fDampening] / 0x527d51 fld [scm_fOMDampening] (6 bytes each).
// EDI = dt (float in an integer register, loaded at the start of OnUpdate).
__declspec(naked) static void DampeningStub()
{
    __asm {
        push edi
        call DampeningFor
        add esp, 4
        ret
    }
}

__declspec(naked) static void OMDampeningStub()
{
    __asm {
        push edi
        call OMDampeningFor
        add esp, 4
        ret
    }
}

// ---------------------------------------------------------------------------------------------
// Hor+ (TRenderInterface.dll, TCameraViewportHandler::OnBeginRenderEvent, horizontal FOV branch)
//
// Original:  fld dword ptr [0.5] ; fmul dword ptr [esi+0x20] (width) ; fdivrp  -> focal = 0.5*W/tan
// The stub loads 0.5*k, where k = min(1, (H*4/3)/W), and the original fmul multiplies it by W,
// giving focal = 0.5*(H*4/3)/tan: same vertical FOV as 4:3, more image on the sides.

__declspec(naked) static void HorPlusStub()
{
    __asm {
        fld dword ptr [esi + 0x24]   // H
        fmul dword ptr [g_four3]     // H*4/3
        fdiv dword ptr [esi + 0x20]  // k = H*4/3 / W
        fld1
        fcomip st, st(1)             // 1 vs k
        jae keep                     // k <= 1 (wide screen): use k
        fstp st(0)
        fld1                         // narrower than 4:3: original behaviour
    keep:
        fmul dword ptr [g_half]
        ret
    }
}

// ---------------------------------------------------------------------------------------------
// Borderless window
//
// A thread finds the process main window and, if it has a title bar/borders, removes them and
// stretches it to its monitor. Re-checked every 500 ms in case the game changes it back.

static bool g_borderless = true;

static BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
        return TRUE;
    RECT rc;
    GetWindowRect(hwnd, &rc);
    if (rc.right - rc.left < 320 || rc.bottom - rc.top < 240)
        return TRUE;
    *(HWND*)lp = hwnd;
    return FALSE;
}

static void MakeBorderless(HWND hwnd)
{
    const LONG style = GetWindowLongA(hwnd, GWL_STYLE);
    if (!(style & (WS_CAPTION | WS_THICKFRAME)) || IsIconic(hwnd))
        return; // already borderless, exclusive fullscreen, or minimized

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoA(mon, &mi))
        return;

    SetWindowLongA(hwnd, GWL_STYLE, (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                                               WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP);
    const LONG ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
    SetWindowLongA(hwnd, GWL_EXSTYLE, ex & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE |
                                             WS_EX_STATICEDGE | WS_EX_WINDOWEDGE));
    const RECT& r = mi.rcMonitor;
    SetWindowPos(hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

static DWORD WINAPI BorderlessThread(LPVOID)
{
    for (;;) {
        HWND hwnd = nullptr;
        EnumWindows(FindGameWindowProc, (LPARAM)&hwnd);
        if (hwnd)
            MakeBorderless(hwnd);
        Sleep(500);
    }
}

// ---------------------------------------------------------------------------------------------
// Forced windowed mode through Direct3D 8
//
// The engine ignores ScreenWindowed in practice, so Direct3DCreate8 is intercepted (IAT of
// TRenderD3DInterface.dll) and then IDirect3D8::CreateDevice / IDirect3DDevice8::Reset always ask
// for windowed mode. The window is made borderless and monitor sized.

struct D3DPP8 {
    UINT BackBufferWidth, BackBufferHeight;
    DWORD BackBufferFormat;
    UINT BackBufferCount;
    DWORD MultiSampleType, SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed, EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat, Flags;
    UINT FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
};
struct D3DDISPLAYMODE8 { UINT Width, Height, RefreshRate; DWORD Format; };

typedef void* (WINAPI* Direct3DCreate8_t)(UINT);
typedef HRESULT(WINAPI* CreateDevice_t)(void*, UINT, DWORD, HWND, DWORD, D3DPP8*, void**);
typedef HRESULT(WINAPI* GetAdapterDisplayMode_t)(void*, UINT, D3DDISPLAYMODE8*);
typedef HRESULT(WINAPI* Reset_t)(void*, D3DPP8*);

static bool g_forceWindowed = true;
static bool g_profileD3D8 = false;
static bool g_altF4 = true;
static bool g_runInBackground = false;
static Direct3DCreate8_t g_origCreate8 = nullptr;
static CreateDevice_t g_origCreateDevice = nullptr;
static Reset_t g_origReset = nullptr;
static UINT g_bbWidth = 0, g_bbHeight = 0;
static DWORD g_desktopFormat = 0; // D3DFMT_UNKNOWN
static WNDPROC g_origWndProc = nullptr;

static void FitWindowToMonitor(HWND hwnd)
{
    if (!hwnd) return;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoA(mon, &mi)) return;
    const LONG style = GetWindowLongA(hwnd, GWL_STYLE);
    SetWindowLongA(hwnd, GWL_STYLE, (style & ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                                               WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP | WS_VISIBLE);
    const LONG ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
    SetWindowLongA(hwnd, GWL_EXSTYLE, ex & ~(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE |
                                             WS_EX_STATICEDGE | WS_EX_WINDOWEDGE));
    const RECT& r = mi.rcMonitor;
    SetWindowPos(hwnd, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_altF4 && msg == WM_SYSKEYDOWN && wp == VK_F4)
        TerminateProcess(GetCurrentProcess(), 0);
    // The game stops rendering when it is deactivated; hide that from it.
    if (g_runInBackground && ((msg == WM_ACTIVATEAPP && !wp) || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE)))
        return 0;
    return IsWindowUnicode(hwnd) ? CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp)
                                 : CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
}

static void SubclassGameWindow(HWND hwnd)
{
    if (!hwnd || g_gameWnd == hwnd) return;
    g_gameWnd = hwnd;
    if (IsWindowUnicode(hwnd))
        g_origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)GameWndProc);
    else
        g_origWndProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)GameWndProc);
}

static void MakeWindowed(D3DPP8* pp, HWND focus)
{
    if (!pp) return;
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->FullScreen_PresentationInterval = 0; // D3DPRESENT_INTERVAL_DEFAULT (required when windowed)
    pp->BackBufferFormat = g_desktopFormat;  // must match the desktop when windowed
    if (pp->SwapEffect == 2)                 // D3DSWAPEFFECT_FLIP
        pp->SwapEffect = 1;                  // D3DSWAPEFFECT_DISCARD
    HWND w = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    FitWindowToMonitor(w);
    SubclassGameWindow(w);
}

static HRESULT WINAPI Reset_Hook(void* dev, D3DPP8* pp)
{
    if (g_forceWindowed) MakeWindowed(pp, g_gameWnd);
    HRESULT hr = g_origReset(dev, pp);
    if (hr >= 0 && pp) { g_bbWidth = pp->BackBufferWidth; g_bbHeight = pp->BackBufferHeight; }
    return hr;
}

static void PatchVtable(void** vtbl, int index, void* hook, void** orig)
{
    if (vtbl[index] == hook) return;
    *orig = vtbl[index];
    PatchBytes(&vtbl[index], &hook, sizeof(void*));
}

// Present: after presenting, the whole back buffer is cleared to black. In menus nothing is drawn
// on the side bars of the 4:3 HUD and, with SWAPEFFECT_DISCARD, old cursor images stayed there
// ("trails").
struct D3DVIEWPORT8 { DWORD X, Y, Width, Height; float MinZ, MaxZ; };
typedef HRESULT(WINAPI* Present_t)(void*, const RECT*, const RECT*, HWND, const void*);
typedef HRESULT(WINAPI* Clear_t)(void*, DWORD, const void*, DWORD, DWORD, float, DWORD);
typedef HRESULT(WINAPI* Viewport_t)(void*, D3DVIEWPORT8*);
static Present_t g_origPresent = nullptr;

static HRESULT WINAPI Present_Hook(void* dev, const RECT* src, const RECT* dst, HWND wnd, const void* rgn)
{
    HRESULT hr = g_origPresent(dev, src, dst, wnd, rgn);
    if (g_fixHud && g_bbWidth && g_bbHeight) {
        void** vt = *(void***)dev;
        D3DVIEWPORT8 old = {};
        if (((Viewport_t)vt[41])(dev, &old) >= 0) {           // GetViewport
            D3DVIEWPORT8 full = { 0, 0, g_bbWidth, g_bbHeight, 0.0f, 1.0f };
            ((Viewport_t)vt[40])(dev, &full);                  // SetViewport
            ((Clear_t)vt[36])(dev, 0, nullptr, 1 /*D3DCLEAR_TARGET*/, 0xFF000000, 1.0f, 0);
            ((Viewport_t)vt[40])(dev, &old);
        }
    }
    return hr;
}

static HRESULT WINAPI CreateDevice_Hook(void* d3d, UINT adapter, DWORD type, HWND focus, DWORD flags,
                                        D3DPP8* pp, void** outDev)
{
    if (g_forceWindowed) {
        D3DDISPLAYMODE8 mode = {};
        void** vtbl = *(void***)d3d;
        if (((GetAdapterDisplayMode_t)vtbl[8])(d3d, adapter, &mode) >= 0)
            g_desktopFormat = mode.Format;
        MakeWindowed(pp, focus);
    } else {
        SubclassGameWindow(pp && pp->hDeviceWindow ? pp->hDeviceWindow : focus);
    }
    HRESULT hr = g_origCreateDevice(d3d, adapter, type, focus, flags, pp, outDev);
    if (hr >= 0 && outDev && *outDev && g_forceWindowed)
        PatchVtable(*(void***)*outDev, 14, (void*)&Reset_Hook, (void**)&g_origReset); // IDirect3DDevice8::Reset
    if (hr >= 0 && outDev && *outDev) {
        PatchVtable(*(void***)*outDev, 15, (void*)&Present_Hook, (void**)&g_origPresent); // Present
        if (pp) { g_bbWidth = pp->BackBufferWidth; g_bbHeight = pp->BackBufferHeight; }
        if (g_profileD3D8) {
            char dir[MAX_PATH];
            GetModuleFileNameA(nullptr, dir, MAX_PATH);
            char* slash = strrchr(dir, '\\');
            if (slash) slash[1] = 0;
            prof::Install(*outDev, std::string(dir) + "d3d8_profile\\");
        }
    }
    return hr;
}

static void* WINAPI Direct3DCreate8_Hook(UINT sdk)
{
    void* d3d = g_origCreate8(sdk);
    if (d3d)
        PatchVtable(*(void***)d3d, 15, (void*)&CreateDevice_Hook, (void**)&g_origCreateDevice); // IDirect3D8::CreateDevice
    return d3d;
}

// Replaces an IAT entry of 'module' (imported from 'dll'!'func').
static bool PatchIAT(HMODULE module, const char* dll, const char* func, void* hook, void** orig)
{
    BYTE* base = (BYTE*)module;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((char*)(base + imp->Name), dll) != 0) continue;
        auto thunk = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        auto orig_thunk = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        for (; orig_thunk->u1.AddressOfData; ++thunk, ++orig_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(orig_thunk->u1.Ordinal)) continue;
            auto byName = (IMAGE_IMPORT_BY_NAME*)(base + orig_thunk->u1.AddressOfData);
            if (strcmp((char*)byName->Name, func) != 0) continue;
            *orig = (void*)thunk->u1.Function;
            void* p = hook;
            return PatchBytes(&thunk->u1.Function, &p, sizeof(p));
        }
    }
    return false;
}

static void InstallD3DHooks()
{
    HMODULE rd3d = GetModuleHandleA("TRenderD3DInterface.dll");
    if (rd3d)
        PatchIAT(rd3d, "d3d8.dll", "Direct3DCreate8", (void*)&Direct3DCreate8_Hook, (void**)&g_origCreate8);
}

// ---------------------------------------------------------------------------------------------
// Texture replacement (mods\textures)
//
// Files: mods\textures\[subfolders\]<library>__<texture>.png|.dds|.jpg|.tga|.bmp
//   e.g. mods\textures\ui\hud\gui_InGame__Menu_Buttons1.png  (library = Data\matlibs\gui_InGame.tml)
// At startup each referenced .tml is read, the texture is located by name and the hash of its
// bytes is stored. The engine creates DDS textures with D3DXCreateTextureFromFileInMemoryEx
// (statically linked in TRenderD3DInterface.dll, RVA 0xf51a); when the bytes match a hash the
// replacement file is passed instead. The ORIGINAL width/height is then restored in
// D3DXIMAGE_INFO, because the GUI computes its coordinates in pixels of the original texture.

struct TexReplacement {
    std::string path;
    UINT origW, origH;
};
static std::unordered_map<unsigned long long, TexReplacement> g_texMods;
// Normal maps: <library>__<texture>_n.dds, handed to the D3D11 renderer (d3d8.dll export
// D3D8to11_SetNormalMap) right after the game creates the texture, replaced or not. Roughness
// maps (<library>__<texture>_r.dds, D3D8to11_SetRoughnessMap) the same way.
static std::unordered_map<unsigned long long, std::string> g_normalMods, g_roughMods;
static bool g_texModsEnabled = true;
static int g_texModsUsed = 0;

static unsigned long long Fnv64(const BYTE* p, size_t n)
{
    unsigned long long h = 1469598103934665603ULL ^ n;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static bool ReadWholeFile(const std::string& path, std::vector<BYTE>& out)
{
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    out.resize(size);
    BOOL ok = size == 0 || ReadFile(f, out.data(), size, &got, nullptr);
    CloseHandle(f);
    return ok && got == size;
}

static std::string GameDir()
{
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;
    return dir;
}

static void LogTexMods(const char* fmt, const char* a, const char* b)
{
    static std::string logPath = GameDir() + "mods\\textures\\_log.txt";
    static bool first = true;
    FILE* f = nullptr;
    if (fopen_s(&f, logPath.c_str(), first ? "w" : "a") != 0 || !f) return;
    first = false;
    fprintf(f, fmt, a, b);
    fputc('\n', f);
    fclose(f);
}

// Finds texture <name> in a .tml; returns pointer/size inside 'tml'.
static bool FindTmlTexture(const std::vector<BYTE>& tml, const char* name, const BYTE** blob, UINT* size,
                           UINT* hdrW, UINT* hdrH)
{
    if (tml.size() < 16 || memcmp(tml.data(), "TML1", 4) != 0) return false;
    const UINT count = *(const UINT*)(tml.data() + 8);
    size_t o = 0xc;
    struct Ent { UINT idx; size_t off; UINT size; UINT w, h; };
    std::vector<Ent> ents;
    for (UINT i = 0; i < count; i++) {
        if (o + 28 > tml.size()) return false;
        Ent e = { *(const UINT*)(tml.data() + o), o + 28, *(const UINT*)(tml.data() + o + 4),
                   *(const unsigned short*)(tml.data() + o + 18), *(const unsigned short*)(tml.data() + o + 20) };
        if (e.off + e.size > tml.size()) return false;
        ents.push_back(e);
        o = e.off + e.size;
    }
    if (o + 4 > tml.size()) return false;
    const UINT nnames = *(const UINT*)(tml.data() + o);
    o += 4;
    for (UINT n = 0; n < nnames && o + 32 * (n + 1) <= tml.size(); n++) {
        char buf[33] = {};
        memcpy(buf, tml.data() + o + 32 * n, 32);
        if (_stricmp(buf, name) != 0) continue;
        for (const Ent& e : ents)
            if (e.idx == n) { *blob = tml.data() + e.off; *size = e.size; *hdrW = e.w; *hdrH = e.h; return true; }
    }
    // Libraries can hold more textures than names (dinosaur libraries: 3 names, 6 textures, the
    // adult and juvenile skins). Texture packs call the unnamed ones "tex<index>".
    unsigned idx = 0;
    char tail = 0;
    if (_strnicmp(name, "tex", 3) == 0 && sscanf_s(name + 3, "%u%c", &idx, &tail, 1) == 1)
        for (const Ent& e : ents)
            if (e.idx == idx) { *blob = tml.data() + e.off; *size = e.size; *hdrW = e.w; *hdrH = e.h; return true; }
    return false;
}

// Walks mods\textures and all its subfolders (ui\menu, dinos, ...). Folders starting with "_"
// (e.g. _backup) are ignored. If the same texture appears twice, the first one wins.
static void ScanTextureModsDir(const std::string& gameDir, const std::string& folder, const std::string& rel,
                               std::unordered_map<std::string, std::vector<BYTE>>& tmls, int depth)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((folder + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string file = fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (file[0] != '.' && file[0] != '_' && depth < 8)
                ScanTextureModsDir(gameDir, folder + file + "\\", rel + file + "\\", tmls, depth + 1);
            continue;
        }
        size_t sep = file.find("__"), dot = file.rfind('.');
        if (sep == std::string::npos || dot == std::string::npos || dot < sep) continue;
        std::string lib = file.substr(0, sep), tex = file.substr(sep + 2, dot - sep - 2);
        const std::string shown = rel + file;
        // Most libraries also ship a 16-bit twin (<library>16.tml, raw 1555/4444) that the game
        // loads instead of the DDS one in some cases (dinosaurs among them). Register both.
        bool found = false;
        for (int twin = 0; twin < 2; twin++) {
            const std::string name = twin ? lib + "16" : lib;
            auto it = tmls.find(name);
            if (it == tmls.end()) {
                std::vector<BYTE> data;
                ReadWholeFile(gameDir + "Data\\matlibs\\" + name + ".tml", data);
                it = tmls.emplace(name, std::move(data)).first;
            }
            const BYTE* blob; UINT size, hw = 0, hh = 0;
            bool normal = false, rough = false;
            if (!FindTmlTexture(it->second, tex.c_str(), &blob, &size, &hw, &hh) || size < 16) {
                const bool nSuffix = tex.size() > 2 && _stricmp(tex.c_str() + tex.size() - 2, "_n") == 0;
                const bool rSuffix = tex.size() > 2 && _stricmp(tex.c_str() + tex.size() - 2, "_r") == 0;
                if ((!nSuffix && !rSuffix) || !FindTmlTexture(it->second, tex.substr(0, tex.size() - 2).c_str(), &blob, &size, &hw, &hh) || size < 16)
                    continue;
                normal = nSuffix; rough = rSuffix;
            }
            found = true;
            const char* tag = twin ? " (16-bit twin)" : "";
            const unsigned long long key = Fnv64(blob, size);
            if (normal) {
                if (!g_normalMods.count(key)) { g_normalMods[key] = folder + file; LogTexMods("normal map: %s%s", shown.c_str(), tag); }
                continue;
            }
            if (rough) {
                if (!g_roughMods.count(key)) { g_roughMods[key] = folder + file; LogTexMods("roughness map: %s%s", shown.c_str(), tag); }
                continue;
            }
            if (g_texMods.count(key)) {
                LogTexMods("DUPLICATE (ignored): %s%s", shown.c_str(), tag);
                continue;
            }
            TexReplacement r;
            r.path = folder + file;
            if (memcmp(blob, "DDS ", 4) == 0) {
                r.origH = *(const UINT*)(blob + 12);
                r.origW = *(const UINT*)(blob + 16);
            } else { // raw texture (1555/4444/8888): size from the .tml entry header
                r.origW = hw;
                r.origH = hh;
            }
            g_texMods[key] = std::move(r);
            LogTexMods("ok: %s%s", shown.c_str(), tag);
        }
        if (!found) LogTexMods("NOT FOUND: %s (%s)", shown.c_str(), "check library/texture name");
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// Hash of every texture in Data\matlibs -> "<library>__<texture>", so the renderer can name the
// textures the game creates (diagnostics only; needs the D3D8to11_SetTextureName export).
static std::unordered_map<unsigned long long, std::string> g_texNames;

static void IndexTextureNames(const std::string& gameDir)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((gameDir + "Data\\matlibs\\*.tml").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::vector<BYTE> tml;
        if (!ReadWholeFile(gameDir + "Data\\matlibs\\" + fd.cFileName, tml)) continue;
        if (tml.size() < 16 || memcmp(tml.data(), "TML1", 4) != 0) continue;
        std::string lib = fd.cFileName;
        lib = lib.substr(0, lib.size() - 4);
        const UINT count = *(const UINT*)(tml.data() + 8);
        size_t o = 0xc;
        std::vector<std::pair<UINT, std::pair<size_t, UINT>>> ents;
        bool ok = true;
        for (UINT i = 0; i < count; i++) {
            if (o + 28 > tml.size()) { ok = false; break; }
            const UINT idx = *(const UINT*)(tml.data() + o), size = *(const UINT*)(tml.data() + o + 4);
            if (o + 28 + size > tml.size()) { ok = false; break; }
            ents.push_back({ idx, { o + 28, size } });
            o += 28 + size;
        }
        if (!ok || o + 4 > tml.size()) continue;
        const UINT nnames = *(const UINT*)(tml.data() + o);
        o += 4;
        for (const auto& e : ents) {
            char name[33] = {};
            if (e.first < nnames && o + 32 * (e.first + 1) <= tml.size()) memcpy(name, tml.data() + o + 32 * e.first, 32);
            else sprintf_s(name, "tex%u", e.first); // unnamed (see FindTmlTexture)
            const unsigned long long key = Fnv64(tml.data() + e.second.first, e.second.second);
            if (!g_texNames.count(key)) g_texNames[key] = lib + "__" + name;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void ScanTextureMods()
{
    const std::string dir = GameDir();
    std::unordered_map<std::string, std::vector<BYTE>> tmls;
    ScanTextureModsDir(dir, dir + "mods\\textures\\", "", tmls, 0);
    IndexTextureNames(dir);
}

typedef HRESULT(WINAPI* D3DXCreateTexMemEx_t)(void* dev, const void* data, UINT size, UINT w, UINT h, UINT mips,
                                              DWORD usage, DWORD fmt, DWORD pool, DWORD filter, DWORD mipFilter,
                                              DWORD colorKey, UINT* srcInfo, void* palette, void** tex);
static D3DXCreateTexMemEx_t g_origCreateTexMem = nullptr;

typedef BOOL(WINAPI* SetNormalMap_t)(void*, const char*);

static void NameTexture(unsigned long long key, void* texture, bool replaced)
{
    if (!texture) return;
    static SetNormalMap_t setName = nullptr; // D3D8to11_SetTextureName has the same signature
    static bool looked = false;
    if (!looked) {
        looked = true;
        HMODULE d3d8 = GetModuleHandleA("d3d8.dll");
        setName = d3d8 ? (SetNormalMap_t)GetProcAddress(d3d8, "D3D8to11_SetTextureName") : nullptr;
    }
    if (!setName) return;
    auto it = g_texNames.find(key);
    char hex[32];
    sprintf_s(hex, "unknown_%016llx", key);
    std::string name = it != g_texNames.end() ? it->second : hex;
    if (replaced) name += " +mod";
    setName(texture, name.c_str());
}

static void AttachNormalMap(unsigned long long key, void* texture)
{
    if ((g_normalMods.empty() && g_roughMods.empty()) || !texture) return;
    static SetNormalMap_t setNormal = nullptr, setRough = nullptr; // same signature
    static bool looked = false;
    if (!looked) {
        looked = true;
        HMODULE d3d8 = GetModuleHandleA("d3d8.dll"); // dgVoodoo or another d3d8.dll: no maps
        setNormal = d3d8 ? (SetNormalMap_t)GetProcAddress(d3d8, "D3D8to11_SetNormalMap") : nullptr;
        setRough = d3d8 ? (SetNormalMap_t)GetProcAddress(d3d8, "D3D8to11_SetRoughnessMap") : nullptr;
    }
    auto it = g_normalMods.find(key);
    if (it != g_normalMods.end() && setNormal && !setNormal(texture, it->second.c_str()))
        LogTexMods("NORMAL MAP ERROR: %s%s", it->second.c_str(), "");
    it = g_roughMods.find(key);
    if (it != g_roughMods.end() && setRough && !setRough(texture, it->second.c_str()))
        LogTexMods("ROUGHNESS MAP ERROR: %s%s", it->second.c_str(), "");
}

static HRESULT WINAPI CreateTexMem_Hook(void* dev, const void* data, UINT size, UINT w, UINT h, UINT mips,
                                        DWORD usage, DWORD fmt, DWORD pool, DWORD filter, DWORD mipFilter,
                                        DWORD colorKey, UINT* srcInfo, void* palette, void** tex)
{
    const unsigned long long key = data && size >= 128 ? Fnv64((const BYTE*)data, size) : 0;
    if (!g_texMods.empty() && key) {
        auto it = g_texMods.find(key);
        if (it != g_texMods.end()) {
            TexReplacement& r = it->second;
            std::vector<BYTE> fileData;
            ReadWholeFile(r.path, fileData);
            if (!fileData.empty()) {
                // The engine passes D3DFMT_UNKNOWN and D3DX_DEFAULT sizes: everything comes from the file.
                HRESULT hr = g_origCreateTexMem(dev, fileData.data(), (UINT)fileData.size(), w, h, mips, usage,
                                                fmt, pool, filter, mipFilter, colorKey, srcInfo, palette, tex);
                if (hr >= 0) {
                    if (srcInfo) { srcInfo[0] = r.origW; srcInfo[1] = r.origH; } // D3DXIMAGE_INFO.Width/Height
                    g_texModsUsed++;
                    if (tex) { AttachNormalMap(key, *tex); NameTexture(key, *tex, true); }
                    return hr;
                }
                LogTexMods("LOAD ERROR: %s%s", r.path.c_str(), "");
            }
        }
    }
    HRESULT hr = g_origCreateTexMem(dev, data, size, w, h, mips, usage, fmt, pool, filter, mipFilter, colorKey,
                                    srcInfo, palette, tex);
    if (hr >= 0 && key && tex) { AttachNormalMap(key, *tex); NameTexture(key, *tex, false); }    return hr;
}

// Raw textures: TTextureResourceHAL::CreateFromMemory5551/4444/8888(w, h, mips, data)
// (thiscall, ret 0x10). When replaced, the texture is created with D3DX from the file into
// this+0x60 and the original size is stored at this+0x64/+0x68, as the engine does.
typedef bool(__thiscall* CreateRaw_t)(BYTE*, UINT, UINT, UINT, const void*);
typedef void*(__thiscall* GetRenderer_t)(void*);
static CreateRaw_t g_origRaw[3] = {};
static GetRenderer_t g_getRenderer = nullptr;

static bool CreateRawReplacement(BYTE* res, UINT w, UINT h, const void* data, UINT bpp)
{
    if (g_texMods.empty() || !data || !g_getRenderer || !g_origCreateTexMem) return false;
    auto it = g_texMods.find(Fnv64((const BYTE*)data, (size_t)w * h * bpp));
    if (it == g_texMods.end()) return false;
    std::vector<BYTE> fileData;
    if (!ReadWholeFile(it->second.path, fileData) || fileData.empty()) return false;
    BYTE* renderer = (BYTE*)g_getRenderer(res);
    if (!renderer) return false;
    void* dev = *(void**)(renderer + 0x178);
    UINT info[5] = {};
    HRESULT hr = g_origCreateTexMem(dev, fileData.data(), (UINT)fileData.size(), (UINT)-1, (UINT)-1, (UINT)-1, 0, 0,
                                    1 /*MANAGED*/, (DWORD)-1, (DWORD)-1, 0, info, nullptr, (void**)(res + 0x60));
    if (hr < 0) { LogTexMods("LOAD ERROR: %s%s", it->second.path.c_str(), ""); return false; }
    *(UINT*)(res + 0x64) = w;
    *(UINT*)(res + 0x68) = h;
    g_texModsUsed++;
    return true;
}

static bool RawCreate(int i, BYTE* r, UINT w, UINT h, UINT m, const void* d, UINT bpp)
{
    const bool replaced = CreateRawReplacement(r, w, h, d, bpp);
    const bool ok = replaced || g_origRaw[i](r, w, h, m, d);
    if (ok && d) {
        const unsigned long long key = Fnv64((const BYTE*)d, (size_t)w * h * bpp);
        AttachNormalMap(key, *(void**)(r + 0x60));
        NameTexture(key, *(void**)(r + 0x60), replaced);
    }
    return ok;
}

static bool __fastcall Raw5551_Hook(BYTE* r, void*, UINT w, UINT h, UINT m, const void* d) { return RawCreate(0, r, w, h, m, d, 2); }
static bool __fastcall Raw4444_Hook(BYTE* r, void*, UINT w, UINT h, UINT m, const void* d) { return RawCreate(1, r, w, h, m, d, 2); }
static bool __fastcall Raw8888_Hook(BYTE* r, void*, UINT w, UINT h, UINT m, const void* d) { return RawCreate(2, r, w, h, m, d, 4); }

static void* MakeTrampoline(BYTE* fn, size_t len, void* hook)
{
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return nullptr;
    memcpy(t, fn, len);
    t[len] = 0xE9;
    *(int*)(t + len + 1) = (int)((fn + len) - (t + len + 5));
    WriteRel(fn, hook, 0xE9, len);
    return t;
}

static void InstallTextureMods()
{
    if (!g_texModsEnabled) return;
    ScanTextureMods();
    if (g_texMods.empty() && g_normalMods.empty() && g_roughMods.empty()) return;
    HMODULE rd3d = GetModuleHandleA("TRenderD3DInterface.dll");
    if (!rd3d) return;
    BYTE* fn = (BYTE*)rd3d + 0xf51a;
    static const unsigned char expect[] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x3C, 0x04, 0x00, 0x00 };
    if (!BytesMatch(fn, expect, sizeof(expect))) {
        LogTexMods("ERROR: unexpected TRenderD3DInterface.dll version%s%s", "", "");
        return;
    }
    BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, fn, 9);
    t[9] = 0xE9;
    *(int*)(t + 10) = (int)((fn + 9) - (t + 14));
    g_origCreateTexMem = (D3DXCreateTexMemEx_t)t;
    WriteRel(fn, (void*)&CreateTexMem_Hook, 0xE9, 9);

    HMODULE ri = GetModuleHandleA("TRenderInterface.dll");
    g_getRenderer = ri ? (GetRenderer_t)GetProcAddress(ri, "?GetRenderer@TResource@Toshi@@QBEPAVTRenderInterface@2@XZ") : nullptr;
    struct { DWORD rva; unsigned char b6; void* hook; } raws[] = {
        { 0xcfd0, 0x1C, (void*)&Raw5551_Hook }, { 0xd1d0, 0x1C, (void*)&Raw4444_Hook }, { 0xd3d0, 0x2C, (void*)&Raw8888_Hook },
    };
    for (int i = 0; i < 3 && g_getRenderer; i++) {
        BYTE* f = (BYTE*)rd3d + raws[i].rva;
        const unsigned char ex[] = { 0x57, 0x56, 0x55, 0x53, 0x83, 0xEC, raws[i].b6 };
        if (BytesMatch(f, ex, sizeof(ex)))
            g_origRaw[i] = (CreateRaw_t)MakeTrampoline(f, 7, raws[i].hook);
    }
}

// ---------------------------------------------------------------------------------------------

static void LoadConfig(HMODULE self)
{
    char path[MAX_PATH];
    GetModuleFileNameA(self, path, MAX_PATH);
    char* dot = strrchr(path, '.');
    if (dot) strcpy_s(dot, path + MAX_PATH - dot, ".ini");
    strcpy_s(g_cameraConfigPath, path);

    char buf[64];
    GetPrivateProfileStringA("Cursor", "Speed", "0.5", buf, sizeof(buf), path);
    g_cursorSpeed = (float)atof(buf);
    if (g_cursorSpeed <= 0.0f) g_cursorSpeed = 0.5f;
    g_disableAccel = GetPrivateProfileIntA("Cursor", "DisableAcceleration", 1, path) != 0;
    g_cursorAspectFix = GetPrivateProfileIntA("Cursor", "FixAspect", 1, path) != 0;

    GetPrivateProfileStringA("FirstPerson", "SliderMin", "0.05", buf, sizeof(buf), path);
    g_sliderMin = (float)atof(buf);
    if (g_sliderMin <= 0.0f || g_sliderMin > 0.25f) g_sliderMin = 0.05f;

    g_wheelFix = GetPrivateProfileIntA("Zoom", "FixWheel", 1, path) != 0;
    GetPrivateProfileStringA("Zoom", "WheelStrength", "1.0", buf, sizeof(buf), path);
    float strength = (float)atof(buf);
    if (strength <= 0.0f) strength = 1.0f;
    g_wheelDt = strength / 30.0f;

    g_horPlus = GetPrivateProfileIntA("Widescreen", "HorPlus", 1, path) != 0;
    g_fixHud = GetPrivateProfileIntA("Widescreen", "FixHUD", 1, path) != 0;
    g_hudMode = GetPrivateProfileIntA("Widescreen", "HUDMode", 2, path);
    if (!g_fixHud) g_hudMode = 0;
    if (g_hudMode < 0 || g_hudMode > 2) g_hudMode = 2;
    g_fixHud = g_hudMode != 0;
    g_minimapEnhanced = GetPrivateProfileIntA("HUD", "EnhancedMinimap", 1, path) != 0;
    g_minimapSize = GetPrivateProfileIntA("HUD", "MinimapSize", 128, path);
    if (g_minimapSize != 64 && g_minimapSize != 128 && g_minimapSize != 256) g_minimapSize = 128;
    g_minimapMargin = GetPrivateProfileIntA("HUD", "MinimapMargin", 16, path);
    if (g_minimapMargin < 0 || g_minimapMargin > 96) g_minimapMargin = 16;
    g_minimapBottom = GetPrivateProfileIntA("HUD", "MinimapBottom", 56, path);
    if (g_minimapBottom < 32 || g_minimapBottom > 160) g_minimapBottom = 56;
    g_texModsEnabled = GetPrivateProfileIntA("Mods", "TextureReplacement", 1, path) != 0;
    g_profileD3D8 = GetPrivateProfileIntA("Debug", "ProfileD3D8", 0, path) != 0;
    g_gameFastPaths = GetPrivateProfileIntA("Performance", "GameFastPaths", 1, path) != 0;
    g_borderless = GetPrivateProfileIntA("Display", "Borderless", 1, path) != 0;
    g_forceWindowed = g_borderless; // borderless = forced windowed + no borders
    g_altF4 = GetPrivateProfileIntA("Display", "AltF4Quit", 1, path) != 0;
    g_runInBackground = GetPrivateProfileIntA("Display", "RunInBackground", 0, path) != 0;
    g_freeKeyboard = GetPrivateProfileIntA("Display", "FreeKeyboard", 1, path) != 0;

    GetPrivateProfileStringA("Camera", "EdgeScrollVertical", "1.5", buf, sizeof(buf), path);
    g_edgeVertical = (float)atof(buf);
    if (g_edgeVertical <= 0.0f) g_edgeVertical = 1.5f;
    GetPrivateProfileStringA("Camera", "EdgeScrollHorizontal", "1.5", buf, sizeof(buf), path);
    g_edgeHorizontal = (float)atof(buf);
    if (g_edgeHorizontal <= 0.0f) g_edgeHorizontal = 1.5f;

    g_smoothFix = GetPrivateProfileIntA("Camera", "FixSmoothing", 1, path) != 0;
    GetPrivateProfileStringA("Camera", "ReferenceFPS", "30", buf, sizeof(buf), path);
    g_refFps = (float)atof(buf);
    if (g_refFps < 10.0f || g_refFps > 300.0f) g_refFps = 30.0f;
    g_modernCamera = GetPrivateProfileIntA("Camera", "ModernControls", 1, path) != 0;
    g_independentCamera = GetPrivateProfileIntA("Camera", "IndependentController", 1, path) != 0;
    struct { const char* key; const char* def; float* value; float low, high; } cameraOptions[] = {
        { "MinDistance", "2", &g_cameraMinDistance, 0.5f, 100 },
        { "MaxDistance", "250", &g_cameraMaxDistance, 2, 2000 },
        { "FocusMinDistance", "2", &g_focusMinDistance, 0.5f, 100 },
        { "FocusMaxDistance", "250", &g_focusMaxDistance, 2, 2000 },
        { "MinPitch", "-89", &g_cameraMinPitch, -89, -5 },
        { "MaxPitch", "-10", &g_cameraMaxPitch, -10, 0 },
        { "FixedFOV", "50", &g_cameraFov, 25, 90 }
    };
    for (const auto& option : cameraOptions) {
        GetPrivateProfileStringA("Camera", option.key, option.def, buf, sizeof(buf), path);
        *option.value = (float)atof(buf);
        if (!(*option.value >= option.low && *option.value <= option.high)) *option.value = (float)atof(option.def);
    }
    if (g_cameraMaxDistance <= g_cameraMinDistance) g_cameraMaxDistance = g_cameraMinDistance + 1;
    if (g_focusMaxDistance <= g_focusMinDistance) g_focusMaxDistance = g_focusMinDistance + 1;
    if (g_cameraMaxPitch <= g_cameraMinPitch) g_cameraMaxPitch = g_cameraMinPitch + 1;
    g_proportionalZoom = GetPrivateProfileIntA("Camera", "ProportionalZoom", 1, path) != 0;
    GetPrivateProfileStringA("Camera", "ResponseSeconds", "0.12", buf, sizeof(buf), path);
    g_cameraResponse = (float)atof(buf);
    if (!(g_cameraResponse >= 0.03f && g_cameraResponse <= 0.5f)) g_cameraResponse = 0.12f;
    GetPrivateProfileStringA("Camera", "DragPanSensitivity", "0.0015", buf, sizeof(buf), path);
    g_dragPanSensitivity = (float)atof(buf);
    if (!(g_dragPanSensitivity > 0 && g_dragPanSensitivity <= 0.02f)) g_dragPanSensitivity = 0.0015f;
    GetPrivateProfileStringA("Camera", "DragRotateSensitivity", "0.0018", buf, sizeof(buf), path);
    g_dragRotateSensitivity = (float)atof(buf);
    if (!(g_dragRotateSensitivity > 0 && g_dragRotateSensitivity <= 0.02f)) g_dragRotateSensitivity = 0.0018f;
    g_rightDragPan = GetPrivateProfileIntA("Camera", "RightDragPan", 1, path) != 0;
    g_keyboardPan = GetPrivateProfileIntA("Camera", "KeyboardPan", 1, path) != 0;
    const struct { const char* key; const char* def; float* value; float low, high; } keyOptions[] = {
        { "KeyboardPanSpeed", "1.0", &g_keyboardPanSpeed, 0.1f, 10 },
        { "KeyboardFast", "2.5", &g_keyboardFast, 1, 10 },
        { "KeyboardSlow", "0.35", &g_keyboardSlow, 0.05f, 1 },
    };
    for (const auto& option : keyOptions) {
        GetPrivateProfileStringA("Camera", option.key, option.def, buf, sizeof(buf), path);
        *option.value = (float)atof(buf);
        if (!(*option.value >= option.low && *option.value <= option.high)) *option.value = (float)atof(option.def);
    }
}

static void ApplyPatches()
{
    if (g_modernCamera) {
        HMODULE exe = GetModuleHandleA(nullptr);
        g_getCameraManager = (CameraManager_t)GetProcAddress(exe, "?GetCameraManager@ACameraManager@@SGPAV1@XZ");
        g_getActiveCamera = (ActiveCamera_t)GetProcAddress(exe, "?GetActiveCamera@ACameraManager@@QBEPAVACamera@@XZ");
        g_getCameraDistance = (CameraDistance_t)GetProcAddress(exe, "?GetDistance@ACamera@@QBEMXZ");
        g_cameraInputMap = (BYTE**)GetProcAddress(exe, "?ms_pSingleton@AInputMap@@1PAV1@A");
        g_worldCameraVtable = (void*)GetProcAddress(exe, "??_7AWorldCamera@@6B@");
        g_cameraPanRight = (CameraInput_t)GetProcAddress(exe, "?InputPushRight@AWorldCamera@@UAEXM@Z");
        g_cameraPanForward = (CameraInput_t)GetProcAddress(exe, "?InputPushForwards@AWorldCamera@@UAEXM@Z");
        g_cameraRotateRight = (CameraInput_t)GetProcAddress(exe, "?InputRotateRight@AWorldCamera@@UAEXM@Z");
        g_cameraRotateDown = (CameraInput_t)GetProcAddress(exe, "?InputRotateDown@AWorldCamera@@UAEXM@Z");
        BYTE* update = (BYTE*)GetProcAddress(exe, "?OnUpdate@AScrollingState@@UAE_NM@Z");
        const unsigned char signature[] = { 0x57, 0x56, 0x55, 0x53, 0x83, 0xec, 0x14 };
        if (g_getCameraManager && g_getActiveCamera && g_getCameraDistance && g_worldCameraVtable && g_cameraInputMap &&
            g_cameraPanRight && g_cameraPanForward && g_cameraRotateRight && g_cameraRotateDown &&
            update && BytesMatch(update, signature, sizeof(signature))) {
            BYTE* trampoline = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (trampoline) {
                memcpy(trampoline, update, sizeof(signature));
                trampoline[7] = 0xe9;
                *(int*)(trampoline + 8) = (int)((update + 7) - (trampoline + 12));
                g_scrollingUpdateTramp = trampoline;
                WriteRel(update, (void*)&ScrollingUpdate_Hook, 0xe9, 7);
                if (*update != 0xe9) { g_scrollingUpdateTramp = nullptr; VirtualFree(trampoline, 0, MEM_RELEASE); }
            }
        }
        OutputDebugStringA(g_scrollingUpdateTramp ? "JPOG: modern park camera installed\n" : "JPOG: camera input hook unavailable\n");
        if (g_scrollingUpdateTramp && g_independentCamera) {
            g_baseCameraUpdate = (CameraUpdate_t)GetProcAddress(exe, "?OnUpdate@ACamera@@UAE_NM@Z");
            g_setCameraPitch = (CameraSetFloat_t)GetProcAddress(exe, "?SetPitch@ACamera@@QAEXM_N@Z");
            g_setCameraDistance = (CameraSetFloat_t)GetProcAddress(exe, "?SetDistance@ACamera@@QAEXM_N@Z");
            g_setCameraRotation = (CameraSetRotation_t)GetProcAddress(exe, "?SetRotation@ACamera@@QAEXM_N0@Z");
            g_getCameraPitch = (CameraDistance_t)GetProcAddress(exe, "?GetPitch@ACamera@@QAEMXZ");
            g_getCameraRotation = (CameraDistance_t)GetProcAddress(exe, "?GetRotation@ACamera@@QAEMXZ");
            g_moveCameraForward = (CameraInput_t)GetProcAddress(exe, "?MoveForwardsHorizontal@ACamera@@IAEXM@Z");
            g_moveCameraRight = (CameraInput_t)GetProcAddress(exe, "?MoveRightHorizontal@ACamera@@IAEXM@Z");
            g_moveCameraUp = (CameraInput_t)GetProcAddress(exe, "?MoveUp@ACamera@@IAEXM@Z");
            g_setCameraFovDegrees = (CameraInput_t)GetProcAddress(exe, "?SetFieldOfViewDegrees@ACamera@@QAEXM@Z");
            BYTE* worldUpdate = (BYTE*)GetProcAddress(exe, "?OnUpdate@AWorldCamera@@UAE_NM@Z");
            BYTE* minDistance = (BYTE*)GetProcAddress(exe, "?ConstMinDistance@AWorldCamera@@UBEMXZ");
            const unsigned char worldSignature[] = { 0x57, 0x55, 0x83, 0xec, 0x3c, 0x8b, 0xe9 };
            const unsigned char minSignature[] = { 0x57, 0x83, 0xec, 0x10, 0x8b, 0xf9 };
            if (g_baseCameraUpdate && g_setCameraPitch && g_setCameraDistance && g_setCameraRotation &&
                g_getCameraPitch && g_getCameraRotation && g_moveCameraForward && g_moveCameraRight &&
                g_moveCameraUp && g_setCameraFovDegrees && worldUpdate && minDistance &&
                BytesMatch(worldUpdate, worldSignature, sizeof(worldSignature)) &&
                BytesMatch(minDistance, minSignature, sizeof(minSignature))) {
                BYTE* wt = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                BYTE* mt = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (wt && mt) {
                    memcpy(wt, worldUpdate, 7); wt[7] = 0xe9;
                    *(int*)(wt + 8) = (int)((worldUpdate + 7) - (wt + 12));
                    memcpy(mt, minDistance, 6); mt[6] = 0xe9;
                    *(int*)(mt + 7) = (int)((minDistance + 6) - (mt + 11));
                    g_worldUpdateTramp = wt; g_worldMinDistanceTramp = mt;
                    WriteRel(minDistance, (void*)&WorldMinDistance_Hook, 0xe9, 6);
                    WriteRel(worldUpdate, (void*)&WorldCameraUpdate_Hook, 0xe9, 7);
                    if (*worldUpdate != 0xe9 || *minDistance != 0xe9) {
                        PatchBytes(worldUpdate, worldSignature, sizeof(worldSignature));
                        PatchBytes(minDistance, minSignature, sizeof(minSignature));
                        g_worldUpdateTramp = g_worldMinDistanceTramp = nullptr;
                        VirtualFree(wt, 0, MEM_RELEASE); VirtualFree(mt, 0, MEM_RELEASE);
                    }
                } else {
                    if (wt) VirtualFree(wt, 0, MEM_RELEASE);
                    if (mt) VirtualFree(mt, 0, MEM_RELEASE);
                }
            }
            OutputDebugStringA(g_worldUpdateTramp ? "JPOG: independent camera controller installed\n" : "JPOG: independent camera unavailable; previous controls retained\n");
            if (g_worldUpdateTramp) {
                g_cameraLimitsReady = InstallCameraLimits(exe, g_worldCameraVtable, "AWorldCamera", g_worldLimits);
                g_followCameraVtable = (void*)GetProcAddress(exe, "??_7AFollowCamera@@6B@");
                if (g_cameraLimitsReady)
                    g_followLimitsReady = InstallCameraLimits(exe, g_followCameraVtable, "AFollowCamera", g_followLimits);
                OutputDebugStringA(g_cameraLimitsReady ? "JPOG: constant camera limits installed\n" : "JPOG: limit bridge failed; original controller retained\n");
            }
        }
    }
    // Install as one group: do not resize if the native paint/position hooks
    // cannot be installed. Export + whole-instruction signatures gate this build.
    if (g_minimapEnhanced) {
        HMODULE exe = GetModuleHandleA(nullptr);
        BYTE* ctor = (BYTE*)GetProcAddress(exe, "??0AMinimap@@QAE@XZ");
        BYTE* paint = (BYTE*)GetProcAddress(exe, "?OnGUIPaint@AMinimap@@CG_NPAV1@PAVTGUIScreen@Toshi@@PAVTGUIScreenPaintEvent@3@@Z");
        BYTE* pos = (BYTE*)GetProcAddress(exe, "?SetPosition@AMinimap@@UAEXHH@Z");
        const unsigned char ctorSig[] = { 0x56, 0x83, 0xec, 0x28, 0x8b, 0xf1 };
        const unsigned char paintSig[] = { 0x57, 0x56, 0x55, 0x53, 0x83, 0xec, 0x44 };
        const unsigned char posSig[] = { 0x8b, 0x44, 0x24, 0x04, 0x8b, 0x54, 0x24, 0x08 };
        if (ctor && paint && pos && BytesMatch(ctor, ctorSig, sizeof(ctorSig)) &&
            BytesMatch(paint, paintSig, sizeof(paintSig)) && BytesMatch(pos, posSig, sizeof(posSig))) {
            BYTE* ct = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            BYTE* pt = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (ct && pt) {
                memcpy(ct, ctor, sizeof(ctorSig));
                ct[6] = 0xe9;
                *(int*)(ct + 7) = (int)((ctor + 6) - (ct + 11));
                memcpy(pt, paint, sizeof(paintSig));
                pt[7] = 0xe9;
                *(int*)(pt + 8) = (int)((paint + 7) - (pt + 12));
                g_minimapCtorTramp = ct;
                g_minimapPaintTramp = pt;
                WriteRel(paint, (void*)&MinimapPaint_Hook, 0xe9, 7);
                WriteRel(pos, (void*)&MinimapPosition_Hook, 0xe9, 8);
                WriteRel(ctor, (void*)&MinimapCtor_Hook, 0xe9, 6);
                g_minimapHooksReady = *paint == 0xe9 && *pos == 0xe9 && *ctor == 0xe9;
                if (!g_minimapHooksReady) {
                    PatchBytes(paint, paintSig, sizeof(paintSig));
                    PatchBytes(pos, posSig, sizeof(posSig));
                    PatchBytes(ctor, ctorSig, sizeof(ctorSig));
                }
            } else {
                if (ct) VirtualFree(ct, 0, MEM_RELEASE);
                if (pt) VirtualFree(pt, 0, MEM_RELEASE);
            }
        }
        OutputDebugStringA(g_minimapHooksReady ? "JPOG: native enhanced minimap installed\n" : "JPOG: minimap signatures/allocation failed; native map unchanged\n");
    }
    // --- Cursor: TGuiInterface.dll ---
    HMODULE gui = GetModuleHandleA("TGuiInterface.dll");
    if (gui) {
        void* fn = (void*)GetProcAddress(gui, "?ApplyPointerAccel@TGUIScreen@Toshi@@IAEXAAH0@Z");
        static const unsigned char expect[] = { 0x57, 0x56, 0x55, 0x53, 0x56, 0x8B, 0xD9 };
        if (fn && BytesMatch(fn, expect, sizeof(expect)))
            WriteRel(fn, (void*)&ApplyPointerAccel_Hook, 0xE9, 5);
    }

    // --- First-person sensitivity slider: SimJP.exe (fixed base 0x400000, no ASLR) ---
    // AWindowsControlsOptionsDialog::AWindowsControlsOptionsDialog
    //   0x57d672: mov dword ptr [esp+4], 0x3e800000        (min = 0.25)
    //   0x57d682: mov dword ptr [ecx+0xa0], 0x3e800000     (min = 0.25)
    static const unsigned char expectA[] = { 0xC7, 0x44, 0x24, 0x04, 0x00, 0x00, 0x80, 0x3E };
    static const unsigned char expectB[] = { 0xC7, 0x81, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3E };
    BYTE* a = (BYTE*)0x57d672;
    BYTE* b = (BYTE*)0x57d682;
    if (BytesMatch(a, expectA, sizeof(expectA)) && BytesMatch(b, expectB, sizeof(expectB))) {
        PatchBytes(a + 4, &g_sliderMin, 4);
        PatchBytes(b + 6, &g_sliderMin, 4);
    }

    // --- Mouse wheel zoom: SimJP.exe ---
    if (g_wheelFix) {
        static const unsigned char expectZoom[] = { 0xD9, 0x04, 0x24, 0x8B, 0x13, 0x57 };
        BYTE* zin = (BYTE*)0x55c5bf;
        BYTE* zout = (BYTE*)0x55c609;
        if (BytesMatch(zin, expectZoom, sizeof(expectZoom)) && BytesMatch(zout, expectZoom, sizeof(expectZoom))) {
            WriteRel(zin, (void*)&ZoomDtStub, 0xE8, 5);
            WriteRel(zout, (void*)&ZoomDtStub, 0xE8, 5);
        }
    }

    // --- Camera smoothing: SimJP.exe ---
    if (g_smoothFix) {
        static const unsigned char expectD[] = { 0xD9, 0x05, 0x00, 0x2C, 0xD1, 0x00 };
        static const unsigned char expectOM[] = { 0xD9, 0x05, 0xF0, 0x61, 0x8B, 0x00 };
        BYTE* sd = (BYTE*)0x527d3f;
        BYTE* som = (BYTE*)0x527d51;
        if (BytesMatch(sd, expectD, sizeof(expectD)) && BytesMatch(som, expectOM, sizeof(expectOM))) {
            WriteRel(sd, (void*)&DampeningStub, 0xE8, 6);
            WriteRel(som, (void*)&OMDampeningStub, 0xE8, 6);
        }
    }

    // --- Edge scrolling speed: SimJP.exe ---
    if (*kEdgeVertical == 2.0f && *kEdgeHorizontal == 1.0f) {
        PatchBytes(kEdgeVertical, &g_edgeVertical, 4);
        PatchBytes(kEdgeHorizontal, &g_edgeHorizontal, 4);
    }

    // --- Non-exclusive keyboard: TInputDXInterface.dll (relocatable base) ---
    // EnumerateDeviceCallback calls SetCooperativeLevel(hwnd, 0x15) = EXCLUSIVE|FOREGROUND|NOWINKEY
    // for the keyboard, which keeps Windows from receiving Print Screen, Alt+F4 and the Windows key.
    // Changed to 0x06 = NONEXCLUSIVE|FOREGROUND.
    if (g_freeKeyboard) {
        HMODULE idx = GetModuleHandleA("TInputDXInterface.dll");
        if (idx) {
            BYTE* site = (BYTE*)idx + 0x39dc;
            static const unsigned char expectKb[] = { 0x6A, 0x15, 0x52, 0x56 };
            if (BytesMatch(site, expectKb, sizeof(expectKb))) {
                const unsigned char flags = 0x06;
                PatchBytes(site + 1, &flags, 1);
            }
        }
    }

    // --- Hor+: TRenderInterface.dll (relocatable base) ---
    if (g_horPlus) {
        HMODULE ri = GetModuleHandleA("TRenderInterface.dll");
        if (ri) {
            BYTE* site = (BYTE*)ri + 0xebf5;
            unsigned char expect[12] = { 0xD9, 0x05, 0, 0, 0, 0, 0xD8, 0x4E, 0x20, 0xDE, 0xF1 };
            *(DWORD*)(expect + 2) = (DWORD)((BYTE*)ri + 0x30fcc); // fld dword ptr [0.5] (relocated)
            if (BytesMatch(site, expect, 11))
                WriteRel(site, (void*)&HorPlusStub, 0xE8, 6);
        }
    }

    // --- Centered 4:3 HUD: PGUIRenderer.dll ---
    // Prologue: push ebp; push ebx; sub esp,1c; mov ebx,ecx  (7 bytes, no relative operands)
    if (g_fixHud) {
        HMODULE pr = GetModuleHandleA("PGUIRenderer.dll");
        BYTE* fn = pr ? (BYTE*)GetProcAddress(pr, "?BeginScene@PGUITRDisplayContext@@UAEXXZ") : nullptr;
        static const unsigned char expectBs[] = { 0x55, 0x53, 0x83, 0xEC, 0x1C, 0x8B, 0xD9 };
        if (fn && BytesMatch(fn, expectBs, sizeof(expectBs))) {
            BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (t) {
                memcpy(t, fn, 7);
                t[7] = 0xE9;
                *(int*)(t + 8) = (int)((fn + 7) - (t + 12));
                g_beginSceneTramp = t;
                WriteRel(fn, (void*)&BeginScene_Hook, 0xE9, 7);
            }
        }

        // Anchored HUD: TGUIScreen::Paint (prologue push edi/esi/ebp/ebx; sub esp,14; mov ebx,ecx)
        if (g_hudMode == 2) {
            HMODULE tgi = GetModuleHandleA("TGuiInterface.dll");
            BYTE* pf = tgi ? (BYTE*)GetProcAddress(tgi, "?Paint@TGUIScreen@Toshi@@IAEXPAVTGUIWidget@2@@Z") : nullptr;
            static const unsigned char expectPf[] = { 0x57, 0x56, 0x55, 0x53, 0x83, 0xEC, 0x14, 0x8B, 0xD9 };
            if (pf && BytesMatch(pf, expectPf, sizeof(expectPf))) {
                BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (t) {
                    memcpy(t, pf, 9);
                    t[9] = 0xE9;
                    *(int*)(t + 10) = (int)((pf + 9) - (t + 14));
                    g_screenPaintTramp = t;
                    WriteRel(pf, (void*)&ScreenPaint_Hook, 0xE9, 9);
                }
            }
        }

        // Popup menus: PGUICore UpdatePosition (prologues without relative jumps)
        HMODULE pc = GetModuleHandleA("PGUICore.dll");
        if (pc && g_hudMode == 2) {
            struct { const char* name; const unsigned char* expect; size_t len; void* hook; void** tramp; } sites[] = {
                { "?UpdatePosition@PGUIMenuDialog@@UAEXAAH0@Z",
                  (const unsigned char*)"\x57\x56\x55\x53\x83\xEC\x24\x89\x4C\x24\x20", 11,
                  (void*)&MenuUpdatePosition_Hook, &g_menuUpdPosTramp },
                { "?UpdatePosition@PGUIDiscMenuDialog@@UAEXAAH0@Z",
                  (const unsigned char*)"\x57\x56\x55\x53\x83\xEC\x14\x8B\xD9", 9,
                  (void*)&DiscUpdatePosition_Hook, &g_discUpdPosTramp },
            };
            for (auto& site : sites) {
                BYTE* fn = (BYTE*)GetProcAddress(pc, site.name);
                if (!fn || !BytesMatch(fn, site.expect, site.len)) continue;
                BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (!t) continue;
                memcpy(t, fn, site.len);
                t[site.len] = 0xE9;
                *(int*)(t + site.len + 1) = (int)((fn + site.len) - (t + site.len + 5));
                *site.tramp = t;
                WriteRel(fn, site.hook, 0xE9, site.len);
            }
        }

        // 3D views in the GUI: AGUIRenderObject::SetViewRectangle
        {
            BYTE* fn = (BYTE*)0x678a90;
            static const unsigned char expectVr[] = { 0x56, 0x55, 0x53, 0x8B, 0xF1, 0x8B, 0x6C, 0x24, 0x10 };
            if (BytesMatch(fn, expectVr, sizeof(expectVr))) {
                BYTE* t = (BYTE*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (t) {
                    memcpy(t, fn, 9);
                    t[9] = 0xE9;
                    *(int*)(t + 10) = (int)((fn + 9) - (t + 14));
                    g_setViewRectTramp = t;
                    WriteRel(fn, (void*)&SetViewRectangle_Hook, 0xE9, 9);
                }
            }
        }

        // Cursor range: TGUIScreen::SetPointerPosition
        HMODULE tg = GetModuleHandleA("TGuiInterface.dll");
        BYTE* sp = tg ? (BYTE*)GetProcAddress(tg, "?SetPointerPosition@TGUIScreen@Toshi@@QAE_NHH@Z") : nullptr;
        static const unsigned char expectSp[] = { 0x55, 0x83, 0xEC, 0x10, 0x0F, 0xBF, 0x41, 0x70 };
        if (sp && BytesMatch(sp, expectSp, sizeof(expectSp)))
            WriteRel(sp, (void*)&SetPointerPosition_Hook, 0xE9, 5);

        // Clicks in the 3D world: ACamera::ConvertGuiScreenToRenderScreen
        static const unsigned char expectCv[] = { 0x55, 0x53, 0x83, 0xEC, 0x14, 0x8B, 0x6C, 0x24, 0x20 };
        BYTE* cv = (BYTE*)0x50bd60;
        if (BytesMatch(cv, expectCv, sizeof(expectCv)))
            WriteRel(cv, (void*)&ConvertGuiToRender_Hook, 0xE9, 5);

        BYTE* cr = (BYTE*)0x50bdd0;
        if (BytesMatch(cr, expectCv, sizeof(expectCv)))
            WriteRel(cr, (void*)&ConvertRenderToGui_Hook, 0xE9, 5);

        // World -> GUI (icons over units, selection)
        static const unsigned char expectTs[] = { 0x57, 0x56, 0x55, 0x83, 0xEC, 0x38, 0x8B, 0xF9, 0x8B, 0x74, 0x24, 0x48 };
        BYTE* ts = (BYTE*)0x477ec0;
        if (BytesMatch(ts, expectTs, sizeof(expectTs)))
            g_toScreenTramp = MakeTrampoline(ts, 12, (void*)&ToScreenCoords_Hook);

        // Edge scrolling
        static const unsigned char expectEx[] = { 0x8B, 0x9F, 0x14, 0x01, 0x00, 0x00 };
        BYTE* ex = (BYTE*)0x55cbe5;
        if (BytesMatch(ex, expectEx, sizeof(expectEx)))
            WriteRel(ex, (void*)&EdgeXStub, 0xE8, 6);
    }
}

// ---------------------------------------------------------------------------------------------
// Game-side fast paths: same results as the game's own code, less CPU on the render thread.

// ARenderer::RenderTerrain sorts the visible terrain sections by distance with a binary search
// and an insertion into an array of {section, distance} pairs; the insertion (SimJP 0x479860,
// thiscall (index, count)) shifts the tail one 8-byte element at a time, reloading the array
// pointer for every element. With ~2000 sections that is the hottest function of the exe.
struct SectionArray { int grow, count, capacity; unsigned __int64* data; };
typedef void(__fastcall* InsertGap_t)(SectionArray*, void*, int, int);
static InsertGap_t g_origInsertGap;

static void __fastcall InsertGap_Hook(SectionArray* a, void*, int index, int n)
{
    if (n < 0 || a->count + n > a->capacity) { g_origInsertGap(a, nullptr, index, n); return; } // growth: game's allocator
    if (index < a->count) memmove(a->data + index + n, a->data + index, (size_t)(a->count - index) * sizeof(*a->data));
    a->count += n;
}

// Toshi::TMatrix44::Multiply(const TMatrix44& a, const TMatrix44& b) (TKernelInterface +0x1bd90):
// this.row[r] = b[r].x * a.row0 + b[r].y * a.row1 + b[r].z * a.row2 + b[r].w * a.row3, in x87.
// The original writes as it reads, so a result that overlaps a or b keeps the original code.
typedef void(__fastcall* MatMul_t)(float*, void*, const float*, const float*);
static MatMul_t g_origMatMul;

static void __fastcall MatMul_Hook(float* r, void*, const float* a, const float* b)
{
    if (r == a || r == b) { g_origMatMul(r, nullptr, a, b); return; }
    const __m128 a0 = _mm_loadu_ps(a), a1 = _mm_loadu_ps(a + 4), a2 = _mm_loadu_ps(a + 8), a3 = _mm_loadu_ps(a + 12);
    for (int i = 0; i < 16; i += 4) {
        __m128 v = _mm_mul_ps(a0, _mm_set1_ps(b[i]));
        v = _mm_add_ps(v, _mm_mul_ps(a1, _mm_set1_ps(b[i + 1])));
        v = _mm_add_ps(v, _mm_mul_ps(a2, _mm_set1_ps(b[i + 2])));
        v = _mm_add_ps(v, _mm_mul_ps(a3, _mm_set1_ps(b[i + 3])));
        _mm_storeu_ps(r + i, v);
    }
}

static void InstallGameFastPaths()
{
    if (!g_gameFastPaths) return;
    BYTE* gap = (BYTE*)0x479860;
    static const unsigned char gapSig[] = { 0x57, 0x56, 0x55, 0x53, 0x83, 0xEC, 0x1C, 0x8B, 0xF9, 0x8B, 0x74, 0x24, 0x30 };
    if (BytesMatch(gap, gapSig, sizeof(gapSig)))
        g_origInsertGap = (InsertGap_t)MakeTrampoline(gap, 7, (void*)&InsertGap_Hook);
    HMODULE kernel = GetModuleHandleA("TKernelInterface.dll");
    BYTE* mul = kernel ? (BYTE*)kernel + 0x1bd90 : nullptr;
    static const unsigned char mulSig[] = { 0x53, 0x8B, 0xD9, 0x8B, 0x4C, 0x24, 0x08, 0x8B, 0x54, 0x24, 0x0C, 0x33, 0xC0, 0xEB, 0x05, 0x90, 0xD9, 0x5C, 0x03 };
    if (mul && BytesMatch(mul, mulSig, sizeof(mulSig)))
        g_origMatMul = (MatMul_t)MakeTrampoline(mul, 7, (void*)&MatMul_Hook);
    char line[96];
    sprintf_s(line, "JPOG: game fast paths: terrain sort %s, matrix multiply %s\n",
        g_origInsertGap ? "on" : "off", g_origMatMul ? "on" : "off");
    OutputDebugStringA(line);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        LoadConfig(inst);
        ApplyPatches();
        InstallD3DHooks();
        InstallTextureMods();
        InstallGameFastPaths();
        if (g_borderless) {
            HANDLE t = CreateThread(nullptr, 0, BorderlessThread, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    return TRUE;
}
