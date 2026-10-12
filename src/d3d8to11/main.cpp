// main.cpp - d3d8.dll entry points and the IDirect3D8 object.
//
// [Renderer] Backend=dgvoodoo in d3d8to11.ini forwards Direct3DCreate8 to D3D8_dgVoodoo.dll
// (the original dgVoodoo d3d8.dll renamed), so both renderers can be switched without moving files.
#include "device.h"
#include "caps_data.h"
#include <algorithm>

static HMODULE g_forward = nullptr;

// ---- Direct3D8 ------------------------------------------------------------------------------------

Direct3D8::Direct3D8()
{
    // Display modes and adapter identity come from DXGI.
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
        DXGI_ADAPTER_DESC1 ad;
        adapter->GetDesc1(&ad);
        WideCharToMultiByte(CP_ACP, 0, ad.Description, -1, m_ident.Description, sizeof(m_ident.Description), nullptr, nullptr);
        m_ident.VendorId = ad.VendorId;
        m_ident.DeviceId = ad.DeviceId;
        m_ident.SubSysId = ad.SubSysId;
        m_ident.Revision = ad.Revision;
        if (SUCCEEDED(adapter->EnumOutputs(0, &output))) {
            UINT n = 0;
            output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &n, nullptr);
            std::vector<DXGI_MODE_DESC> modes(n);
            if (n) output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &n, modes.data());
            for (DWORD fmt : { (DWORD)D8FMT_X8R8G8B8, (DWORD)D8FMT_R5G6B5 }) {
                for (const DXGI_MODE_DESC& m : modes) {
                    UINT hz = m.RefreshRate.Denominator ? (m.RefreshRate.Numerator + m.RefreshRate.Denominator / 2) / m.RefreshRate.Denominator : 60;
                    D3D8DisplayMode dm = { m.Width, m.Height, hz, fmt };
                    bool dup = false;
                    for (auto& e : m_modes) if (!memcmp(&e, &dm, sizeof(dm))) { dup = true; break; }
                    if (!dup) m_modes.push_back(dm);
                }
            }
        }
    }
    strcpy_s(m_ident.Driver, "d3d8to11.dll");
    m_ident.DriverVersion.QuadPart = 0x0006000E000A0000LL; // 6.14.10.0
    m_ident.WHQLLevel = 1;
    if (m_modes.empty()) m_modes.push_back({ 1920, 1080, 60, D8FMT_X8R8G8B8 });
    Log("adapter: %s, %zu display modes", m_ident.Description, m_modes.size());
}

HRESULT Direct3D8::QueryInterface(REFIID, void** out)
{
    if (!out) return E_POINTER;
    *out = this;
    AddRef();
    return S_OK;
}

ULONG Direct3D8::Release()
{
    ULONG r = InterlockedDecrement(&m_ref);
    if (r == 0) delete this;
    return r;
}

HRESULT Direct3D8::GetAdapterIdentifier(UINT adapter, DWORD, D3D8AdapterIdentifier* id)
{
    if (adapter != 0 || !id) return D8ERR_INVALIDCALL;
    *id = m_ident;
    return S_OK;
}

UINT Direct3D8::GetAdapterModeCount(UINT adapter)
{
    return adapter == 0 ? (UINT)m_modes.size() : 0;
}

HRESULT Direct3D8::EnumAdapterModes(UINT adapter, UINT mode, D3D8DisplayMode* out)
{
    if (adapter != 0 || mode >= m_modes.size() || !out) return D8ERR_INVALIDCALL;
    *out = m_modes[mode];
    return S_OK;
}

HRESULT Direct3D8::GetAdapterDisplayMode(UINT adapter, D3D8DisplayMode* out)
{
    if (adapter != 0 || !out) return D8ERR_INVALIDCALL;
    DEVMODEA dm = { };
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm);
    *out = { dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency, D8FMT_X8R8G8B8 };
    return S_OK;
}

HRESULT Direct3D8::CheckDeviceType(UINT adapter, DWORD, DWORD displayFmt, DWORD bbFmt, BOOL)
{
    if (adapter != 0) return D8ERR_INVALIDCALL;
    auto ok = [](DWORD f) { return f == D8FMT_X8R8G8B8 || f == D8FMT_A8R8G8B8 || f == D8FMT_R5G6B5 || f == D8FMT_X1R5G5B5 || f == D8FMT_UNKNOWN; };
    return ok(displayFmt) && ok(bbFmt) ? S_OK : D8ERR_NOTAVAILABLE;
}

HRESULT Direct3D8::CheckDeviceFormat(UINT adapter, DWORD, DWORD, DWORD usage, DWORD rtype, DWORD fmt)
{
    if (adapter != 0) return D8ERR_INVALIDCALL;
    FormatInfo i = GetFormatInfo(fmt);
    if (i.dxgi == DXGI_FORMAT_UNKNOWN) return D8ERR_NOTAVAILABLE;
    if (usage & D8USAGE_DEPTHSTENCIL) return i.depth ? S_OK : D8ERR_NOTAVAILABLE;
    if (i.depth) return D8ERR_NOTAVAILABLE;
    if (usage & D8USAGE_RENDERTARGET)
        return (fmt == D8FMT_A8R8G8B8 || fmt == D8FMT_X8R8G8B8 || fmt == D8FMT_R5G6B5) ? S_OK : D8ERR_NOTAVAILABLE;
    if (rtype == D8RTYPE_CUBETEXTURE) return D8ERR_NOTAVAILABLE;
    return S_OK;
}

HRESULT Direct3D8::CheckDeviceMultiSampleType(UINT, DWORD, DWORD, BOOL, DWORD ms)
{
    return ms == 0 ? S_OK : D8ERR_NOTAVAILABLE;
}

void Direct3D8::FillCaps(void* caps)
{
    memcpy(caps, kCaps8, kCapsSize);
}

HRESULT Direct3D8::GetDeviceCaps(UINT adapter, DWORD, void* caps)
{
    if (adapter != 0 || !caps) return D8ERR_INVALIDCALL;
    FillCaps(caps);
    return S_OK;
}

HMONITOR Direct3D8::GetAdapterMonitor(UINT)
{
    POINT p = { 0, 0 };
    return MonitorFromPoint(p, MONITOR_DEFAULTTOPRIMARY);
}

HRESULT Direct3D8::CreateDevice(UINT adapter, DWORD, HWND focus, DWORD flags, D3D8PresentParameters* pp, ID8Device** out)
{
    if (adapter != 0 || !pp || !out) return D8ERR_INVALIDCALL;
    Log("CreateDevice: %ux%u fmt %lu windowed %d depth %d/%lu flags 0x%lx", pp->BackBufferWidth, pp->BackBufferHeight,
        pp->BackBufferFormat, pp->Windowed, pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat, flags);
    Device* dev = new Device(this, focus, flags, pp);
    if (!dev->Init()) {
        Log("CreateDevice failed");
        dev->Release();
        *out = nullptr;
        return D8ERR_NOTAVAILABLE;
    }
    *out = dev;
    return S_OK;
}

// ---- exports ----------------------------------------------------------------------------------------

extern "C" void* WINAPI Direct3DCreate8(UINT sdk)
{
    if (g_config.useDgVoodoo) {
        if (!g_forward) g_forward = LoadLibraryA((g_gameDir + "D3D8_dgVoodoo.dll").c_str());
        auto fn = g_forward ? (void*(WINAPI*)(UINT))GetProcAddress(g_forward, "Direct3DCreate8") : nullptr;
        Log("Direct3DCreate8(%u) -> dgVoodoo %s", sdk, fn ? "ok" : "NOT FOUND");
        return fn ? fn(sdk) : nullptr;
    }
    Log("Direct3DCreate8(%u) -> D3D11 layer", sdk);
    return static_cast<ID8Direct3D*>(new Direct3D8());
}

extern "C" HRESULT WINAPI ValidatePixelShader(const DWORD*, const void*, BOOL, char** errors)
{
    if (errors) *errors = nullptr;
    return S_OK;
}

extern "C" HRESULT WINAPI ValidateVertexShader(const DWORD*, const DWORD*, const void*, BOOL, char** errors)
{
    if (errors) *errors = nullptr;
    return S_OK;
}

extern "C" void WINAPI DebugSetMute() {}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        LoadConfig();
        LogInit(g_gameDir + "d3d8to11.log");
        Log("d3d8to11 loaded (backend: %s)", g_config.useDgVoodoo ? "dgVoodoo" : "D3D11");
    }
    return TRUE;
}
