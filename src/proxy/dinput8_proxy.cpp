// dinput8.dll proxy + mini ASI loader.
// Reenvia todo al dinput8.dll real del sistema y, al arrancar el proceso, carga los *.asi de la
// carpeta del juego (todos los modulos importados por el juego ya estan mapeados en ese momento).

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
        GetSystemDirectoryA(path, MAX_PATH); // SysWOW64 en un proceso de 32 bits
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

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    LoadAsiPlugins();
    auto fn = (DirectInput8Create_t)GetProcAddress(Real(), "DirectInput8Create");
    return fn ? fn(inst, ver, riid, out, outer) : E_FAIL;
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
        // Se cargan ya (como Ultimate ASI Loader) para que los plugins puedan enganchar cosas que el
        // juego crea antes de usar DirectInput, como el dispositivo Direct3D.
        LoadAsiPlugins();
    }
    return TRUE;
}
