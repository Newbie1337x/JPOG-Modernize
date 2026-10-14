// Standalone, no game launch or plugin loading. Tests the actual proxy hooks
// against synthetic DirectInput reads and the same math used by the plugin.
#include "../src/proxy/dinput8_proxy.cpp"
#include "../src/plugin/camera_math.h"
#include <stdio.h>
#include <assert.h>

static int feeds = 0;
static BOOL WINAPI NoOverlay(LONG, LONG, LONG, DWORD) { return FALSE; }
static BOOL WINAPI Drag(LONG x, LONG y, LONG z, DWORD buttons)
{
    ++feeds;
    assert(x == 15 && y == -7 && z == 120 && (buttons & 4));
    return TRUE;
}
static HRESULT STDMETHODCALLTYPE ReadState(void*, DWORD size, void* data)
{
    memset(data, 0, size);
    LONG* axes = (LONG*)data;
    axes[0] = 15; axes[1] = -7; axes[2] = 120;
    BYTE* buttons = (BYTE*)data + 12;
    buttons[0] = buttons[1] = buttons[2] = 0x80;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ReadData(void*, DWORD size, void* items, DWORD* count, DWORD)
{
    const DWORD offsets[] = { 0, 4, 8, 12, 13, 14 };
    const DWORD values[] = { 15, (DWORD)-7, 120, 0x80, 0x80, 0x80 };
    assert(*count >= 6);
    for (DWORD i = 0; i < 6; ++i) {
        DWORD* item = (DWORD*)((BYTE*)items + i * size);
        item[0] = offsets[i]; item[1] = values[i];
    }
    *count = 6;
    return S_OK;
}
int main()
{
    int mouse = 0;
    g_mice[0] = &mouse;
    g_origGetState = ReadState;
    g_origGetData = ReadData;
    g_mouseInput = NoOverlay;
    g_cameraMouseInput = Drag;
    BYTE state[20];
    assert(SUCCEEDED(Hook_GetDeviceState(&mouse, sizeof(state), state)));
    assert(((LONG*)state)[0] == 0 && ((LONG*)state)[1] == 0);
    assert(((LONG*)state)[2] == 120 && state[12] == 0x80 && state[13] == 0x80 && state[14] == 0);
    DWORD items[6][4] = {}, count = 6;
    assert(SUCCEEDED(Hook_GetDeviceData(&mouse, sizeof(items[0]), items, &count, 0)));
    assert(count == 3 && items[0][0] == 8 && items[1][0] == 12 && items[2][0] == 13);
    const int beforePeek = feeds;
    count = 6;
    assert(SUCCEEDED(Hook_GetDeviceData(&mouse, sizeof(items[0]), items, &count, 1)));
    assert(count == 6 && feeds == beforePeek);
    const int rates[] = { 30, 60, 144, 240 };
    for (int fps : rates) {
        float response = 1;
        for (int i = 0; i < fps; ++i) response *= CameraMath::ResponseDamping(1.0f / fps, 0.12f);
        assert(fabsf(response - expf(-1.0f / 0.12f)) < 0.000001f);
        float velocity = 0, travelled = 0;
        for (int i = 0; i < fps; ++i)
            travelled += CameraMath::IntegrateDisplacement(10.0f / fps, 1.0f / fps, 0.12f, velocity);
        assert(fabsf(travelled - (10.0f - 1.2f * (1.0f - expf(-1.0f / 0.12f)))) < 0.0001f);
        for (int i = 0; i < fps; ++i)
            travelled += CameraMath::IntegrateDisplacement(0, 1.0f / fps, 0.12f, velocity);
        assert(fabsf(travelled - 10.0f) < 0.001f);
    }
    assert(CameraMath::ResponseDamping(0, 0.12f) == 1);
    assert(CameraMath::ResponseDamping(-1, 0.12f) == 1);
    assert(CameraMath::ResponseDamping(10, 0.12f) == CameraMath::ResponseDamping(0.25f, 0.12f));
    assert(CameraMath::ZoomRatio(12) == 0.35f && CameraMath::ZoomRatio(45) == 1 && CameraMath::ZoomRatio(200) == 2);
    CameraMath::OrbitPose orbit;
    orbit.pitch = -0.4f; orbit.yaw = 0.5f;
    float zoomOnly[6] = { 0, 0, 0, 0, -1, 0 }, movement[3];
    for (int i = 0; i < 120; ++i)
        CameraMath::StepOrbit(orbit, zoomOnly, 1.0f/120, 0.12f, -1.48f, 1.48f, 2, 250, movement);
    assert(orbit.pitch == -0.4f && orbit.yaw == 0.5f && orbit.distance == 2);
    assert(orbit.velocity[4] == 0); // no stored momentum past zoom limit
    orbit.velocity[4] = 0;
    float heightOnly[6] = { 0, 0, 0, 0, 0, 1 };
    CameraMath::StepOrbit(orbit, heightOnly, 1.0f/60, 0.12f, -1.48f, 1.48f, 2, 250, movement);
    assert(movement[2] > 0 && orbit.distance == 2 && orbit.pitch == -0.4f && orbit.yaw == 0.5f);
    float pitchOnly[6] = { 0, 0, 0, -100, 0, 0 };
    CameraMath::StepOrbit(orbit, pitchOnly, 1.0f/30, 0.12f, -1.48f, 1.48f, 2, 250, movement);
    assert(orbit.pitch == -1.48f && orbit.velocity[3] == 0);
    puts("PASS: input preservation, response/integrated movement 30-240 FPS, braking and zoom bounds");
    puts("PASS: zoom/height do not change angles; pitch and distance stop at bounds");
}
