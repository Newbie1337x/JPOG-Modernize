// dinput8.dll proxy + mini ASI loader.
// Forwards everything to the real system dinput8.dll and, at process start, loads every *.asi in
// the game folder (all modules imported by the game are already mapped at that point).

#include <windows.h>

typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT(WINAPI* DllCanUnloadNow_t)();
typedef HRESULT(WINAPI* DllGetClassObject_t)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT(WINAPI* DllRegisterServer_t)();

static HMODULE g_real = nullptr;
static bool g_asiLoaded = false;

static HMODULE Real()
{
    if (!g_real) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH); // SysWOW64 in a 32-bit process
        strcat_s(path, "\\dinput8.dll");
        g_real = LoadLibraryA(path);
    }
    return g_real;
}

static void LoadAsiPlugins()
{
    if (g_asiLoaded) return;
    g_asiLoaded = true;

    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;

    char pattern[MAX_PATH];
    strcpy_s(pattern, dir);
    strcat_s(pattern, "*.asi");

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char full[MAX_PATH];
        strcpy_s(full, dir);
        strcat_s(full, fd.cFileName);
        LoadLibraryA(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// ---- input hand-off to the d3d8to11 overlay ------------------------------------------------------
// The renderer's settings panel (d3d8.dll, overlay.cpp) needs the mouse and must pause the game's
// input while it is open. IDirectInput8::CreateDevice and the device's GetDeviceState /
// GetDeviceData are patched in their vtables (shared by every instance): mouse reads are reported
// to D3D8to11_MouseInput, and while D3D8to11_InputBlocked is true the game gets empty input.
// Without our d3d8.dll (e.g. dgVoodoo) the exports are missing and nothing changes.

typedef BOOL(WINAPI* MouseInput_t)(LONG, LONG, LONG, DWORD);
typedef BOOL(WINAPI* InputBlocked_t)();
typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(void*, REFGUID, void**, void*);
typedef HRESULT(STDMETHODCALLTYPE* GetDeviceState_t)(void*, DWORD, void*);
typedef HRESULT(STDMETHODCALLTYPE* GetDeviceData_t)(void*, DWORD, void*, DWORD*, DWORD);

static const GUID kSysMouse = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static CreateDevice_t g_origCreateDevice = nullptr;
static GetDeviceState_t g_origGetState = nullptr;
static GetDeviceData_t g_origGetData = nullptr;
static void* g_mice[8] = {};
static DWORD g_mouseButtons = 0;
static MouseInput_t g_mouseInput = nullptr;
static InputBlocked_t g_inputBlocked = nullptr;
static MouseInput_t g_cameraMouseInput = nullptr;

static void FindCameraInput()
{
    if (g_cameraMouseInput) return;
    HMODULE plugin = GetModuleHandleA("JPOGMouseFix.asi");
    if (plugin) {
        g_cameraMouseInput = (MouseInput_t)GetProcAddress(plugin, "JPOGCamera_MouseInput");
        if (!g_cameraMouseInput)
            g_cameraMouseInput = (MouseInput_t)GetProcAddress(plugin, "_JPOGCamera_MouseInput@16");
    }
}

static void FindOverlay()
{
    if (g_mouseInput) return;
    HMODULE d3d8 = GetModuleHandleA("d3d8.dll");
    if (!d3d8) return;
    g_mouseInput = (MouseInput_t)GetProcAddress(d3d8, "D3D8to11_MouseInput");
    g_inputBlocked = (InputBlocked_t)GetProcAddress(d3d8, "D3D8to11_InputBlocked");
}

static bool IsMouse(void* dev)
{
    for (void* m : g_mice) if (m && m == dev) return true;
    return false;
}

static void Patch(void** slot, void* hook, void** original)
{
    if (*slot == hook) return;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
    if (!*original) *original = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
}

static HRESULT STDMETHODCALLTYPE Hook_GetDeviceState(void* self, DWORD size, void* data)
{
    HRESULT hr = g_origGetState(self, size, data);
    if (FAILED(hr) || !data) return hr;
    FindOverlay();
    FindCameraInput();
    if (IsMouse(self) && (size == 16 || size == 20)) {
        // DIMOUSESTATE / DIMOUSESTATE2: lX, lY, lZ, rgbButtons[4 or 8]
        const LONG* axes = (const LONG*)data;
        const BYTE* buttons = (const BYTE*)data + 12;
        DWORD bits = 0;
        for (int i = 0; i < 3; i++) if (buttons[i] & 0x80) bits |= 1u << i;
        const bool blocked = g_mouseInput ? g_mouseInput(axes[0], axes[1], axes[2], bits) != FALSE : false;
        if (blocked) memset(data, 0, size);
        else if (g_cameraMouseInput && g_cameraMouseInput(axes[0], axes[1], axes[2], bits)) {
            ((LONG*)data)[0] = ((LONG*)data)[1] = 0;
            ((BYTE*)data)[14] = 0; // middle only; preserve selection, right click and wheel
        }
    } else if (g_inputBlocked && g_inputBlocked()) {
        memset(data, 0, size);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_GetDeviceData(void* self, DWORD objSize, void* items, DWORD* count, DWORD flags)
{
    HRESULT hr = g_origGetData(self, objSize, items, count, flags);
    if (FAILED(hr) || !count) return hr;
    FindOverlay();
    FindCameraInput();
    if (IsMouse(self) && items && objSize >= 8) {
        // DIDEVICEOBJECTDATA: dwOfs, dwData, ... ; DIMOFS_X/Y/Z = 0/4/8, DIMOFS_BUTTON0.. = 12..
        LONG dx = 0, dy = 0, dz = 0;
        for (DWORD i = 0; i < *count; i++) {
            const DWORD* item = (const DWORD*)((const BYTE*)items + (size_t)i * objSize);
            if (item[0] == 0) dx += (LONG)item[1];
            else if (item[0] == 4) dy += (LONG)item[1];
            else if (item[0] == 8) dz += (LONG)item[1];
            else if (item[0] >= 12 && item[0] < 15) {
                DWORD bit = 1u << (item[0] - 12);
                g_mouseButtons = (item[1] & 0x80) ? (g_mouseButtons | bit) : (g_mouseButtons & ~bit);
            }
        }
        const bool blocked = g_mouseInput ? g_mouseInput(dx, dy, dz, g_mouseButtons) != FALSE : false;
        if (blocked) *count = 0;
        else if (!(flags & 1) && g_cameraMouseInput && g_cameraMouseInput(dx, dy, dz, g_mouseButtons)) {
            // Remove only consumed axes/middle events from the game's batch.
            DWORD kept = 0;
            for (DWORD i = 0; i < *count; ++i) {
                BYTE* item = (BYTE*)items + (size_t)i * objSize;
                const DWORD offset = *(DWORD*)item;
                if (offset == 0 || offset == 4 || offset == 14) continue;
                if (kept != i) memmove((BYTE*)items + (size_t)kept * objSize, item, objSize);
                ++kept;
            }
            *count = kept;
        }
    } else if (g_inputBlocked && g_inputBlocked()) {
        *count = 0;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_CreateDevice(void* self, REFGUID guid, void** out, void* outer)
{
    HRESULT hr = g_origCreateDevice(self, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        void** vtbl = *(void***)*out;
        Patch(&vtbl[9], (void*)Hook_GetDeviceState, (void**)&g_origGetState);
        Patch(&vtbl[10], (void*)Hook_GetDeviceData, (void**)&g_origGetData);
        if (IsEqualGUID(guid, kSysMouse)) {
            for (void*& m : g_mice) if (!m) { m = *out; break; }
        }
    }
    return hr;
}

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    LoadAsiPlugins();
    auto fn = (DirectInput8Create_t)GetProcAddress(Real(), "DirectInput8Create");
    HRESULT hr = fn ? fn(inst, ver, riid, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) {
        void** vtbl = *(void***)*out;
        Patch(&vtbl[3], (void*)Hook_CreateDevice, (void**)&g_origCreateDevice);
    }
    return hr;
}

extern "C" HRESULT WINAPI Proxy_DllCanUnloadNow()
{
    auto fn = (DllCanUnloadNow_t)GetProcAddress(Real(), "DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}

extern "C" HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out)
{
    auto fn = (DllGetClassObject_t)GetProcAddress(Real(), "DllGetClassObject");
    return fn ? fn(clsid, riid, out) : E_FAIL;
}

extern "C" HRESULT WINAPI Proxy_DllRegisterServer()
{
    auto fn = (DllRegisterServer_t)GetProcAddress(Real(), "DllRegisterServer");
    return fn ? fn() : E_FAIL;
}

extern "C" HRESULT WINAPI Proxy_DllUnregisterServer()
{
    auto fn = (DllRegisterServer_t)GetProcAddress(Real(), "DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        // Loaded right away (like Ultimate ASI Loader) so plugins can hook things the game creates
        // before using DirectInput, such as the Direct3D device.
        LoadAsiPlugins();
    }
    return TRUE;
}
