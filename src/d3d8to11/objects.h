// objects.h - D3D8 object implementations on top of D3D11.
#pragma once
#include "common.h"
#include <atomic>

// CPU wrapper lifetime counts, not an estimate of GPU allocation bytes.
struct ResourceCount {
    inline static std::atomic<unsigned> live[5]={};
    unsigned kind;
    explicit ResourceCount(unsigned k):kind(k){live[k].fetch_add(1,std::memory_order_relaxed);}
    ~ResourceCount(){live[kind].fetch_sub(1,std::memory_order_relaxed);}
    ResourceCount(const ResourceCount&)=delete;
    ResourceCount& operator=(const ResourceCount&)=delete;
};

class Device;

// Shared IUnknown/IDirect3DResource8 boilerplate.
#define D8_PRIVATE_DATA_STUBS                                                                        \
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) override { return S_OK; } \
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override { return D8ERR_NOTAVAILABLE; } \
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return S_OK; }

#define D8_RESOURCE_STUBS                                                                            \
    D8_PRIVATE_DATA_STUBS                                                                            \
    DWORD STDMETHODCALLTYPE SetPriority(DWORD p) override { DWORD o = m_priority; m_priority = p; return o; } \
    DWORD STDMETHODCALLTYPE GetPriority() override { return m_priority; }                            \
    void STDMETHODCALLTYPE PreLoad() override {}

// ---- textures and surfaces ---------------------------------------------------------------------

class Texture;
class VolumeTexture;

// A 2D surface: either a level of a Texture (container != null) or a standalone surface
// (back buffer, render target, depth stencil, image surface).
class Surface : public ID8Surface {
public:
    ResourceCount m_count{0};
    Surface(Device* dev, Texture* container, UINT level, UINT w, UINT h, DWORD fmt, DWORD usage, DWORD pool);
    ~Surface();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDevice(ID8Device** dev) override;
    D8_PRIVATE_DATA_STUBS
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID, void** container) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3D8SurfaceDesc* desc) override;
    HRESULT STDMETHODCALLTYPE LockRect(D3D8LockedRect* lr, const RECT* rect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect() override;

    bool CreateStandalone();       // GPU objects for standalone surfaces
    ID3D11RenderTargetView* Rtv();
    ID3D11DepthStencilView* Dsv() { return m_dsv.Get(); }
    ID3D11Texture2D* Resource();   // backing texture (container texture for levels)
    ID3D11Texture2D* ResolvedTexture();       // single-sample view of a standalone render target
    ID3D11ShaderResourceView* ResolvedSrv();
    UINT Subresource() const { return m_level; }

    Device* m_dev;
    Texture* m_container;
    UINT m_level, m_width, m_height;
    DWORD m_format, m_usage, m_pool;
    LONG m_ref = 1;
    bool m_holdsDeviceRef = false;          // app-created standalone surfaces keep the device alive
    UINT m_samples = 1;                     // MSAA sample count (back buffer / auto depth)
    ComPtr<ID3D11Texture2D> m_tex;          // standalone only
    ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11DepthStencilView> m_dsv;
    ComPtr<ID3D11ShaderResourceView> m_srv; // standalone render targets (back buffer)
    ComPtr<ID3D11Texture2D> m_resolveTex;   // MSAA back buffer: resolved copy
    ComPtr<ID3D11ShaderResourceView> m_resolveSrv;
    std::vector<BYTE> m_lockData;           // read-back / image surface storage
};

class Texture : public ID8Texture {
public:
    ResourceCount m_count{1};
    Texture(Device* dev, UINT w, UINT h, UINT levels, DWORD usage, DWORD fmt, DWORD pool);
    ~Texture();
    bool Create();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDevice(ID8Device** dev) override;
    D8_RESOURCE_STUBS
    DWORD STDMETHODCALLTYPE GetType() override { return D8RTYPE_TEXTURE; }
    DWORD STDMETHODCALLTYPE SetLOD(DWORD lod) override { DWORD o = m_lod; m_lod = lod; return o; }
    DWORD STDMETHODCALLTYPE GetLOD() override { return m_lod; }
    DWORD STDMETHODCALLTYPE GetLevelCount() override { return m_levels; }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3D8SurfaceDesc* desc) override;
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level, ID8Surface** surf) override;
    HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3D8LockedRect* lr, const RECT* rect, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT*) override { return S_OK; }

    void Upload(UINT level);
    ID3D11ShaderResourceView* Srv() { return m_srv.Get(); }

    Device* m_dev;
    UINT m_width, m_height, m_levels;
    DWORD m_usage, m_format, m_pool, m_priority = 0, m_lod = 0;
    LONG m_ref = 1;
    FormatInfo m_info;
    ComPtr<ID3D11Texture2D> m_tex;
    ComPtr<ID3D11ShaderResourceView> m_srv;
    ComPtr<ID3D11ShaderResourceView> m_normalSrv; // optional normal map (D3D8to11_SetNormalMap)
    BYTE m_materialProfile=0; // scoped pilot: 1 Triceratops, 2 hamburger-shop ornament
    std::string m_name;        // <library>__<texture> reported by the plugin, for diagnostics
    unsigned long long m_contentHash = 0; // FNV-1a of the first level-0 upload (identity of unnamed textures)
    bool m_hashValid = false;
    std::string m_normalName;  // basename of the attached normal map, for diagnostics
    std::vector<std::vector<BYTE>> m_shadow; // D3D8-format copy of each level (lazy)
    std::vector<Surface*> m_surfaces;        // one per level, owned
};

class Volume;

class VolumeTexture : public ID8VolumeTexture {
public:
    ResourceCount m_count{2};
    VolumeTexture(Device* dev, UINT w, UINT h, UINT d, UINT levels, DWORD usage, DWORD fmt, DWORD pool);
    ~VolumeTexture();
    bool Create();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDevice(ID8Device** dev) override;
    D8_RESOURCE_STUBS
    DWORD STDMETHODCALLTYPE GetType() override { return D8RTYPE_VOLUMETEXTURE; }
    DWORD STDMETHODCALLTYPE SetLOD(DWORD lod) override { DWORD o = m_lod; m_lod = lod; return o; }
    DWORD STDMETHODCALLTYPE GetLOD() override { return m_lod; }
    DWORD STDMETHODCALLTYPE GetLevelCount() override { return m_levels; }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3D8VolumeDesc* desc) override;
    HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT level, ID8Volume** vol) override;
    HRESULT STDMETHODCALLTYPE LockBox(UINT level, D3D8LockedBox* lb, const D3D8Box* box, DWORD flags) override;
    HRESULT STDMETHODCALLTYPE UnlockBox(UINT level) override;
    HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3D8Box*) override { return S_OK; }

    void Upload(UINT level);
    ID3D11ShaderResourceView* Srv() { return m_srv.Get(); }

    Device* m_dev;
    UINT m_width, m_height, m_depth, m_levels;
    DWORD m_usage, m_format, m_pool, m_priority = 0, m_lod = 0;
    LONG m_ref = 1;
    FormatInfo m_info;
    ComPtr<ID3D11Texture3D> m_tex;
    ComPtr<ID3D11ShaderResourceView> m_srv;
    std::vector<std::vector<BYTE>> m_shadow;
    std::vector<Volume*> m_volumes;
};

class Volume : public ID8Volume {
public:
    Volume(VolumeTexture* container, UINT level) : m_container(container), m_level(level) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return m_container->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return m_container->Release(); }
    HRESULT STDMETHODCALLTYPE GetDevice(ID8Device** dev) override { return m_container->GetDevice(dev); }
    D8_PRIVATE_DATA_STUBS
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID, void** container) override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3D8VolumeDesc* desc) override { return m_container->GetLevelDesc(m_level, desc); }
    HRESULT STDMETHODCALLTYPE LockBox(D3D8LockedBox* lb, const D3D8Box* box, DWORD flags) override
    { return m_container->LockBox(m_level, lb, box, flags); }
    HRESULT STDMETHODCALLTYPE UnlockBox() override { return m_container->UnlockBox(m_level); }

    VolumeTexture* m_container;
    UINT m_level;
};

// ---- buffers ----------------------------------------------------------------------------------

// Vertex and index buffers share the implementation: a full CPU copy ("shadow") is kept so that
// locks without DISCARD/NOOVERWRITE never stall or corrupt in-flight draws.
class BufferImpl {
public:
    bool Create(Device* dev, UINT size, DWORD usage, DWORD pool, UINT bind);
    HRESULT Lock(UINT offset, UINT size, BYTE** data, DWORD flags);
    HRESULT Unlock();

    Device* m_dev = nullptr;
    UINT m_size = 0;
    DWORD m_usage = 0, m_pool = 0;
    bool m_dynamic = false;
    ComPtr<ID3D11Buffer> m_buf;
    std::vector<BYTE> m_shadow;
    UINT m_lockOffset = 0, m_lockSize = 0, m_lockCount = 0;
    DWORD m_lockFlags = 0;
    bool m_everDiscarded = false;
};

class VertexBuffer : public ID8VertexBuffer {
public:
    ResourceCount m_count{3};
    VertexBuffer(Device* dev, DWORD fvf) : m_dev(dev), m_fvf(fvf) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDevice(ID8Device** dev) override;
    D8_RESOURCE_STUBS
    DWORD STDMETHODCALLTYPE GetType() override { return D8RTYPE_VERTEXBUFFER; }
    HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, BYTE** data, DWORD flags) override
    { return m_impl.Lock(offset, size, data, flags); }
    HRESULT STDMETHODCALLTYPE Unlock() override { return m_impl.Unlock(); }
    HRESULT STDMETHODCALLTYPE GetDesc(D3D8VertexBufferDesc* desc) override;

    Device* m_dev;
    DWORD m_fvf, m_priority = 0;
    LONG m_ref = 1;
    BufferImpl m_impl;
};

class IndexBuffer : public ID8IndexBuffer {
public:
    ResourceCount m_count{4};
    IndexBuffer(Device* dev, DWORD fmt) : m_dev(dev), m_format(fmt) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE GetDevice(ID8Device** dev) override;
    D8_RESOURCE_STUBS
    DWORD STDMETHODCALLTYPE GetType() override { return D8RTYPE_INDEXBUFFER; }
    HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT size, BYTE** data, DWORD flags) override
    { return m_impl.Lock(offset, size, data, flags); }
    HRESULT STDMETHODCALLTYPE Unlock() override { return m_impl.Unlock(); }
    HRESULT STDMETHODCALLTYPE GetDesc(D3D8IndexBufferDesc* desc) override;

    Device* m_dev;
    DWORD m_format, m_priority = 0;
    LONG m_ref = 1;
    BufferImpl m_impl;
};

// ---- Direct3D8 object ----------------------------------------------------------------------------

class Direct3D8 : public ID8Direct3D {
public:
    Direct3D8();
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void*) override { return D8ERR_INVALIDCALL; }
    UINT STDMETHODCALLTYPE GetAdapterCount() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter, DWORD flags, D3D8AdapterIdentifier* id) override;
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter, UINT mode, D3D8DisplayMode* out) override;
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter, D3D8DisplayMode* out) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT, DWORD, DWORD displayFmt, DWORD bbFmt, BOOL) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT, DWORD, DWORD, DWORD usage, DWORD rtype, DWORD fmt) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT, DWORD, DWORD, BOOL, DWORD ms) override;
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT, DWORD, DWORD, DWORD, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter, DWORD type, void* caps) override;
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override;
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, DWORD type, HWND focus, DWORD flags,
                                           D3D8PresentParameters* pp, ID8Device** out) override;

    static void FillCaps(void* caps);

    LONG m_ref = 1;
    std::vector<D3D8DisplayMode> m_modes;
    D3D8AdapterIdentifier m_ident = {};
};
