// device.cpp - IDirect3DDevice8 on D3D11: state tracking, draws, clears and presentation.
#include "device.h"
#include "constant_update.h"
#include "hitch_sampler.h"
#include <math.h>
#include <d3d11sdklayers.h>
#include <dxgi1_5.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

const char* kHelperVS =
    "cbuffer Helper : register(b0) { float4 color; float4 misc; };\n"
    "struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "O main(uint id : SV_VertexID) {\n"
    "  O o; o.uv = float2((id << 1) & 2, id & 2);\n"
    "  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), misc.x, 1);\n"
    "  return o; }\n";

const char* kClearPS =
    "cbuffer Helper : register(b0) { float4 color; float4 misc; };\n"
    "float4 main(float4 pos : SV_Position) : SV_Target { return color; }\n";

const char* kPresentPS =
    "cbuffer Helper : register(b0) { float4 color; float4 misc; };\n"
    "Texture2D src : register(t0); Texture1D lut : register(t1);\n"
    "SamplerState smp : register(s0); SamplerState pt : register(s1);\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "  float4 c = src.Sample(smp, uv);\n"
    "  if (misc.y > 0.5) {\n"
    "    float3 t = saturate(c.rgb) * (255.0 / 256.0) + 0.5 / 256.0;\n"
    "    c.r = lut.Sample(pt, t.r).r; c.g = lut.Sample(pt, t.g).g; c.b = lut.Sample(pt, t.b).b;\n"
    "  }\n"
    "  return float4(c.rgb, 1.0); }\n";

struct HelperConstants { float color[4]; float misc[4]; };

D3D11_BLEND MapBlend(DWORD b, bool alpha)
{
    switch (b) {
    case 1: return D3D11_BLEND_ZERO;
    case 2: return D3D11_BLEND_ONE;
    case 3: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 4: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 5: return D3D11_BLEND_SRC_ALPHA;
    case 6: return D3D11_BLEND_INV_SRC_ALPHA;
    case 7: return D3D11_BLEND_DEST_ALPHA;
    case 8: return D3D11_BLEND_INV_DEST_ALPHA;
    case 9: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 10: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 11: return D3D11_BLEND_SRC_ALPHA_SAT;
    default: return D3D11_BLEND_ONE;
    }
}

D3D11_COMPARISON_FUNC MapCmp(DWORD f) { return (f >= 1 && f <= 8) ? (D3D11_COMPARISON_FUNC)f : D3D11_COMPARISON_ALWAYS; }

D3D11_TEXTURE_ADDRESS_MODE MapAddress(DWORD a) { return (a >= 1 && a <= 5) ? (D3D11_TEXTURE_ADDRESS_MODE)a : D3D11_TEXTURE_ADDRESS_WRAP; }

DXGI_FORMAT DeclFormat(int type)
{
    switch (type) {
    case 0: return DXGI_FORMAT_R32_FLOAT;
    case 1: return DXGI_FORMAT_R32G32_FLOAT;
    case 2: return DXGI_FORMAT_R32G32B32_FLOAT;
    case 3: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 4: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case 5: return DXGI_FORMAT_R8G8B8A8_UINT;
    case 6: return DXGI_FORMAT_R16G16_SINT;
    case 7: return DXGI_FORMAT_R16G16B16A16_SINT;
    default: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    }
}

template <class T> void SetRef(T*& slot, T* value)
{
    if (slot == value) return;
    if (value) value->AddRef();
    if (slot) slot->Release();
    slot = value;
}

} // namespace

// ---- construction --------------------------------------------------------------------------------

Device::Device(Direct3D8* d3d, HWND focus, DWORD flags, D3D8PresentParameters* pp)
    : m_d3d(d3d), m_focus(focus), m_window(nullptr), m_flags(flags), m_pp(*pp)
{
    d3d->AddRef();
    m_window = m_pp.hDeviceWindow ? m_pp.hDeviceWindow : focus;
    memset(m_rs, 0, sizeof(m_rs));
    memset(m_tss, 0, sizeof(m_tss));
    memset(m_vsConst, 0, sizeof(m_vsConst));
    memset(m_transforms, 0, sizeof(m_transforms));
    for (auto& m : m_transforms) m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
    for (int i = 0; i < 256; i++) m_gamma.red[i] = m_gamma.green[i] = m_gamma.blue[i] = (WORD)(i * 257);

    // D3D8 default render states.
    m_rs[RS_ZENABLE] = m_pp.EnableAutoDepthStencil ? 1 : 0;
    m_rs[RS_FILLMODE] = 3;
    m_rs[RS_SHADEMODE] = 2;
    m_rs[RS_ZWRITEENABLE] = 1;
    m_rs[RS_SRCBLEND] = 2;
    m_rs[RS_DESTBLEND] = 1;
    m_rs[RS_CULLMODE] = 3;
    m_rs[RS_ZFUNC] = 4;
    m_rs[RS_ALPHAFUNC] = 8;
    m_rs[RS_FOGEND] = 0x3f800000;
    m_rs[RS_FOGDENSITY] = 0x3f800000;
    m_rs[RS_STENCILFAIL] = m_rs[RS_STENCILZFAIL] = m_rs[RS_STENCILPASS] = 1;
    m_rs[RS_STENCILFUNC] = 8;
    m_rs[RS_STENCILMASK] = m_rs[RS_STENCILWRITEMASK] = 0xFFFFFFFF;
    m_rs[RS_TEXTUREFACTOR] = 0xFFFFFFFF;
    m_rs[RS_LIGHTING] = 1;
    m_rs[RS_COLORWRITEENABLE] = 0xF;
    m_rs[RS_BLENDOP] = 1;
    for (int s = 0; s < kMaxStages; s++) {
        DWORD* t = m_tss[s];
        t[TSS_COLOROP] = s == 0 ? 4 : 1;
        t[TSS_COLORARG1] = 2;
        t[TSS_COLORARG2] = 1;
        t[TSS_ALPHAOP] = s == 0 ? 2 : 1;
        t[TSS_ALPHAARG1] = 2;
        t[TSS_ALPHAARG2] = 1;
        t[TSS_TEXCOORDINDEX] = s;
        t[TSS_ADDRESSU] = t[TSS_ADDRESSV] = t[TSS_ADDRESSW] = 1;
        t[TSS_MAGFILTER] = t[TSS_MINFILTER] = 1;
        t[TSS_MIPFILTER] = 0;
        t[TSS_MAXANISOTROPY] = 1;
        t[TSS_COLORARG0] = t[TSS_ALPHAARG0] = 1;
    }
    for (int i = 0; i < 4; i++) m_psConst.textureFactor[i] = 1.0f;
    m_psConst.alphaRef[0] = 0.0f;
    QueryPerformanceFrequency(&m_qpcFreq);
    QueryPerformanceCounter(&m_lastFrame);
}

Device::~Device()
{
    ReleaseFsr31();
    ShutdownOverlay();
    for (auto& t : m_textures) SetRef(t, (ID8BaseTexture*)nullptr);
    for (auto& s : m_streams) SetRef(s.vb, (VertexBuffer*)nullptr);
    SetRef(m_indices, (IndexBuffer*)nullptr);
    SetRef(m_rt, (Surface*)nullptr);
    SetRef(m_ds, (Surface*)nullptr);
    if (m_backBuffer) { m_backBuffer->m_holdsDeviceRef = false; m_backBuffer->Release(); }
    if (m_autoDepth) { m_autoDepth->m_holdsDeviceRef = false; m_autoDepth->Release(); }
    for (auto* v : m_vertexShaders) delete v;
    for (auto* p : m_pixelShaders) delete p;
    m_d3d->Release();
}

bool Device::Init()
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (g_config.debugLayer) flags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                                   D3D11_SDK_VERSION, &m_device, &got, &m_ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels + 1, 1,
                               D3D11_SDK_VERSION, &m_device, &got, &m_ctx);
    }
    if (FAILED(hr)) { Log("D3D11CreateDevice failed 0x%08lx", hr); return false; }
    m_ctx.As(&m_ctx1);
    Log("D3D11 device created (feature level 0x%x)", got);
    if (!CreatePresentResources()) return false;
    if (!CreateSunResources()) { Log("Sun resource creation failed"); return false; }
    if (!CreateWaterResources()) { Log("Water resource creation failed"); return false; }
    if (!CreateSwapChainAndBackBuffer()) return false;
    PrewarmFF();
    return true;
}

bool Device::CreatePresentResources()
{
    auto vs = CompileHLSL(kHelperVS, "vs_4_0", "helper_vs");
    auto cps = CompileHLSL(kClearPS, "ps_4_0", "clear_ps");
    auto pps = CompileHLSL(kPresentPS, "ps_4_0", "present_ps");
    if (!vs || !cps || !pps) return false;
    m_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_fullscreenVS);
    m_device->CreatePixelShader(cps->GetBufferPointer(), cps->GetBufferSize(), nullptr, &m_clearPS);
    m_device->CreatePixelShader(pps->GetBufferPointer(), pps->GetBufferSize(), nullptr, &m_presentPS);

    auto makeCB = [&](UINT size, ComPtr<ID3D11Buffer>& out) {
        D3D11_BUFFER_DESC bd = { (size + 15) & ~15u, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                                 D3D11_CPU_ACCESS_WRITE, 0, 0 };
        return SUCCEEDED(m_device->CreateBuffer(&bd, nullptr, &out));
    };
    {
        D3D11_FEATURE_DATA_D3D11_OPTIONS opt = {};
        if (m_ctx1 && SUCCEEDED(m_device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof(opt))) &&
            opt.ConstantBufferOffsetting && opt.MapNoOverwriteOnDynamicConstantBuffer && !makeCB(kVsRingBytes, m_vsRing))
            m_vsRing.Reset();
        Log("Vertex constants: %s", m_vsRing ? "ring buffer (no-overwrite, offset binding)" : "one buffer per draw (discard)");
    }
    if (!makeCB(sizeof(m_vsConst), m_vsCB) || !makeCB(sizeof(VSExtraConstants), m_vsExtraCB) ||
        !makeCB(sizeof(PSConstants), m_psCB) || !makeCB(sizeof(HelperConstants), m_clearCB))
        return false;

    D3D11_SAMPLER_DESC sd = {};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    m_device->CreateSamplerState(&sd, &m_linearClamp);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    m_device->CreateSamplerState(&sd, &m_pointClamp);

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_device->CreateBlendState(&bd, &m_noBlend);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    m_device->CreateDepthStencilState(&dd, &m_noDepth);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    m_device->CreateRasterizerState(&rd, &m_noCull);
    rd.ScissorEnable = TRUE;
    m_device->CreateRasterizerState(&rd, &m_noCullScissor);

    D3D11_TEXTURE1D_DESC gd = { 256, 1, 1, DXGI_FORMAT_R16G16B16A16_UNORM, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    if (FAILED(m_device->CreateTexture1D(&gd, nullptr, &m_gammaTex))) return false;
    m_device->CreateShaderResourceView(m_gammaTex.Get(), nullptr, &m_gammaSRV);
    return true;
}

bool Device::CreateSwapChainAndBackBuffer()
{
    m_sunStarted = m_sunResolved = false;
    m_shadowDraws = 0;
    RECT rc = {};
    GetClientRect(m_window, &rc);
    UINT w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (!m_pp.BackBufferWidth || !m_pp.BackBufferHeight) { m_pp.BackBufferWidth = w; m_pp.BackBufferHeight = h; }
    if (!w || !h) { w = m_pp.BackBufferWidth; h = m_pp.BackBufferHeight; }
    if (m_pp.BackBufferFormat == D8FMT_UNKNOWN) m_pp.BackBufferFormat = D8FMT_X8R8G8B8;

    if (!m_swap) {
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        m_device.As(&dxgiDev);
        dxgiDev->GetAdapter(&adapter);
        adapter->GetParent(IID_PPV_ARGS(&factory));
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = w;
        sd.Height = h;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        // Without tearing support a windowed flip-model swap chain is held to the monitor's
        // refresh rate by the compositor even with VSync off.
        ComPtr<IDXGIFactory5> factory5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory.As(&factory5)) &&
            SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) && tearing)
            sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        m_swapFlags = sd.Flags;
        Log("Swap chain: tearing %s", tearing ? "supported (uncapped with VSync off)" : "not supported");
        HRESULT hr = factory->CreateSwapChainForHwnd(m_device.Get(), m_window, &sd, nullptr, nullptr, &m_swap);
        if (FAILED(hr)) { Log("CreateSwapChainForHwnd failed 0x%08lx", hr); return false; }
        factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        m_swapW = w;
        m_swapH = h;
    }

    if (m_backBuffer) { m_backBuffer->m_holdsDeviceRef = false; m_backBuffer->Release(); m_backBuffer = nullptr; }
    if (m_autoDepth) { m_autoDepth->m_holdsDeviceRef = false; m_autoDepth->Release(); m_autoDepth = nullptr; }
    m_msaaApplied = SupportedSamples(g_config.EffectiveMsaa());
    m_taaValid=false;ReleaseFsr31();
    m_waterSnapshotReady = false;
    m_waterInteraction.Clear();
    m_backBuffer = new Surface(this, nullptr, 0, m_pp.BackBufferWidth, m_pp.BackBufferHeight, m_pp.BackBufferFormat,
                               D8USAGE_RENDERTARGET, D8POOL_DEFAULT);
    m_backBuffer->m_samples = m_msaaApplied;
    if (!m_backBuffer->CreateStandalone()) { Log("back buffer creation failed"); return false; }
    if (m_pp.EnableAutoDepthStencil) {
        m_autoDepth = new Surface(this, nullptr, 0, m_pp.BackBufferWidth, m_pp.BackBufferHeight,
                                  m_pp.AutoDepthStencilFormat, D8USAGE_DEPTHSTENCIL, D8POOL_DEFAULT);
        m_autoDepth->m_samples = m_msaaApplied;
        if (!m_autoDepth->CreateStandalone()) { Log("depth buffer creation failed"); return false; }
    }
    Log("MSAA %ux", m_msaaApplied);
    SetRef(m_rt, m_backBuffer);
    SetRef(m_ds, m_autoDepth);
    m_rtDirty = true;
    m_viewport = { 0, 0, m_pp.BackBufferWidth, m_pp.BackBufferHeight, 0.0f, 1.0f };
    m_viewportDirty = true;
    Log("back buffer %ux%u fmt %lu, depth %s, window %ux%u", m_pp.BackBufferWidth, m_pp.BackBufferHeight,
        m_pp.BackBufferFormat, m_autoDepth ? "yes" : "no", w, h);
    return true;
}

HRESULT Device::QueryInterface(REFIID, void** out)
{
    if (!out) return E_POINTER;
    *out = this;
    AddRef();
    return S_OK;
}

ULONG Device::Release()
{
    ULONG r = InterlockedDecrement(&m_ref);
    if (r == 0) delete this;
    return r;
}

HRESULT Device::GetDirect3D(ID8Direct3D** out)
{
    if (!out) return D8ERR_INVALIDCALL;
    *out = m_d3d;
    m_d3d->AddRef();
    return S_OK;
}

HRESULT Device::GetDeviceCaps(void* caps)
{
    if (!caps) return D8ERR_INVALIDCALL;
    Direct3D8::FillCaps(caps);
    return S_OK;
}

HRESULT Device::GetDisplayMode(D3D8DisplayMode* mode)
{
    return m_d3d->GetAdapterDisplayMode(0, mode);
}

HRESULT Device::GetCreationParameters(void* params)
{
    struct P { UINT adapter; DWORD type; HWND focus; DWORD flags; };
    if (!params) return D8ERR_INVALIDCALL;
    *(P*)params = { 0, 1, m_focus, m_flags };
    return S_OK;
}

HRESULT Device::Reset(D3D8PresentParameters* pp)
{
    FlushTerrainBatch();
    InvalidateBindings();
    m_tessCache.clear();m_tessCacheBytes=0;
    if (!pp) return D8ERR_INVALIDCALL;
    m_pp = *pp;
    if (pp->hDeviceWindow) m_window = pp->hDeviceWindow;
    Log("Reset %ux%u", pp->BackBufferWidth, pp->BackBufferHeight);
    return CreateSwapChainAndBackBuffer() ? S_OK : D8ERR_INVALIDCALL;
}

HRESULT Device::GetBackBuffer(UINT, DWORD, ID8Surface** out)
{
    if (!out || !m_backBuffer) return D8ERR_INVALIDCALL;
    *out = m_backBuffer;
    m_backBuffer->AddRef();
    return S_OK;
}

HRESULT Device::GetRasterStatus(D3D8RasterStatus* st)
{
    if (!st) return D8ERR_INVALIDCALL;
    st->InVBlank = FALSE;
    st->ScanLine = 0;
    return S_OK;
}

void Device::SetGammaRamp(DWORD, const D3D8GammaRamp* ramp)
{
    if (!ramp || memcmp(ramp, &m_gamma, sizeof(m_gamma)) == 0) return;
    m_gamma = *ramp;
    m_gammaDirty = true;
    m_gammaIdentity = true;
    for (int i = 0; i < 256 && m_gammaIdentity; i++)
        if (m_gamma.red[i] != i * 257 || m_gamma.green[i] != i * 257 || m_gamma.blue[i] != i * 257) m_gammaIdentity = false;
}

// ---- resource creation ---------------------------------------------------------------------------

HRESULT Device::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, DWORD fmt, DWORD pool, ID8Texture** out)
{
    if (!out || !w || !h) return D8ERR_INVALIDCALL;
    Texture* t = new Texture(this, w, h, levels, usage, fmt, pool);
    if (!t->Create()) { t->Release(); return D8ERR_INVALIDCALL; }
    *out = t;
    return S_OK;
}

HRESULT Device::CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, DWORD fmt, DWORD pool, ID8VolumeTexture** out)
{
    if (!out || !w || !h || !d) return D8ERR_INVALIDCALL;
    VolumeTexture* t = new VolumeTexture(this, w, h, d, levels, usage, fmt, pool);
    if (!t->Create()) { t->Release(); return D8ERR_INVALIDCALL; }
    *out = t;
    return S_OK;
}

HRESULT Device::CreateCubeTexture(UINT, UINT, DWORD, DWORD fmt, DWORD, void**)
{
    LogOnce("cube", "CreateCubeTexture (fmt %lu) not supported", fmt);
    return D8ERR_NOTAVAILABLE;
}

HRESULT Device::CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, DWORD pool, ID8VertexBuffer** out)
{
    if (!out) return D8ERR_INVALIDCALL;
    VertexBuffer* vb = new VertexBuffer(this, fvf);
    AddRef();
    if (!vb->m_impl.Create(this, len, usage, pool, D3D11_BIND_VERTEX_BUFFER)) { vb->Release(); return D8ERR_OUTOFVIDEOMEMORY; }
    *out = vb;
    return S_OK;
}

HRESULT Device::CreateIndexBuffer(UINT len, DWORD usage, DWORD fmt, DWORD pool, ID8IndexBuffer** out)
{
    if (!out) return D8ERR_INVALIDCALL;
    IndexBuffer* ib = new IndexBuffer(this, fmt);
    AddRef();
    if (!ib->m_impl.Create(this, len, usage, pool, D3D11_BIND_INDEX_BUFFER)) { ib->Release(); return D8ERR_OUTOFVIDEOMEMORY; }
    *out = ib;
    return S_OK;
}

HRESULT Device::CreateRenderTarget(UINT w, UINT h, DWORD fmt, DWORD, BOOL, ID8Surface** out)
{
    if (!out) return D8ERR_INVALIDCALL;
    Surface* s = new Surface(this, nullptr, 0, w, h, fmt, D8USAGE_RENDERTARGET, D8POOL_DEFAULT);
    AddRef();
    s->m_holdsDeviceRef = true;
    if (!s->CreateStandalone()) { s->Release(); return D8ERR_INVALIDCALL; }
    *out = s;
    return S_OK;
}

HRESULT Device::CreateDepthStencilSurface(UINT w, UINT h, DWORD fmt, DWORD, ID8Surface** out)
{
    if (!out) return D8ERR_INVALIDCALL;
    Surface* s = new Surface(this, nullptr, 0, w, h, fmt, D8USAGE_DEPTHSTENCIL, D8POOL_DEFAULT);
    AddRef();
    s->m_holdsDeviceRef = true;
    if (!s->CreateStandalone()) { s->Release(); return D8ERR_INVALIDCALL; }
    *out = s;
    return S_OK;
}

HRESULT Device::CreateImageSurface(UINT w, UINT h, DWORD fmt, ID8Surface** out)
{
    if (!out) return D8ERR_INVALIDCALL;
    Surface* s = new Surface(this, nullptr, 0, w, h, fmt, 0, D8POOL_SYSTEMMEM);
    AddRef();
    s->m_holdsDeviceRef = true;
    if (!s->CreateStandalone()) { s->Release(); return D8ERR_INVALIDCALL; }
    *out = s;
    return S_OK;
}

HRESULT Device::CopyRects(ID8Surface* src, const RECT* rects, UINT count, ID8Surface* dst, const POINT* pts)
{
    Surface* s = static_cast<Surface*>(src);
    Surface* d = static_cast<Surface*>(dst);
    if (!s || !d) return D8ERR_INVALIDCALL;
    FlushTerrainBatch();
    if (s->Resource() && d->Resource() && !count) {
        if (d->m_samples > 1) { LogOnce("copyrects-ms", "CopyRects into the MSAA back buffer skipped"); return S_OK; }
        ID3D11Texture2D* source = s->m_samples > 1 ? s->ResolvedTexture() : s->Resource();
        m_ctx->CopySubresourceRegion(d->Resource(), d->Subresource(), 0, 0, 0, source, s->Subresource(), nullptr);
        return S_OK;
    }
    LogOnce("copyrects", "CopyRects: only whole GPU surface copies are supported");
    return S_OK;
}

HRESULT Device::UpdateTexture(ID8BaseTexture*, ID8BaseTexture*)
{
    LogOnce("updatetexture", "UpdateTexture not supported");
    return S_OK;
}

void Device::OnTextureReleased(ID8BaseTexture*) {}
void Device::OnSurfaceReleased(Surface*) {}

// ---- render targets / viewport / clears ----------------------------------------------------------

HRESULT Device::SetRenderTarget(ID8Surface* rt, ID8Surface* ds)
{
    FlushTerrainBatch();
    if (rt) SetRef(m_rt, static_cast<Surface*>(rt));
    SetRef(m_ds, static_cast<Surface*>(ds));
    TraceSun("SetRenderTarget rt %s %ux%u ds %s", m_rt == m_backBuffer ? "back" : "other",
             m_rt ? m_rt->m_width : 0, m_rt ? m_rt->m_height : 0, !m_ds ? "none" : (m_ds == m_autoDepth ? "auto" : "other"));
    m_rtDirty = true;
    if (m_rt) {
        m_viewport = { 0, 0, m_rt->m_width, m_rt->m_height, 0.0f, 1.0f };
        m_viewportDirty = true;
    }
    return S_OK;
}

HRESULT Device::GetRenderTarget(ID8Surface** out)
{
    if (!out || !m_rt) return D8ERR_INVALIDCALL;
    *out = m_rt;
    m_rt->AddRef();
    return S_OK;
}

HRESULT Device::GetDepthStencilSurface(ID8Surface** out)
{
    if (!out) return D8ERR_INVALIDCALL;
    *out = m_ds;
    if (!m_ds) return D8ERR_NOTAVAILABLE;
    m_ds->AddRef();
    return S_OK;
}

// Highest sample count <= requested that the GPU supports for both back buffer formats.
UINT Device::SupportedSamples(int requested)
{
    UINT n = requested >= 8 ? 8 : requested >= 4 ? 4 : requested >= 2 ? 2 : 1;
    for (; n > 1; n /= 2) {
        UINT q1 = 0, q2 = 0;
        m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, n, &q1);
        m_device->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, n, &q2);
        if (q1 && q2) break;
    }
    return n;
}

// [Renderer] MSAA changed (overlay): rebuild the back buffer and depth behind the same Surfaces.
void Device::ApplyMsaa()
{
    FlushTerrainBatch();
    const UINT want = SupportedSamples(g_config.EffectiveMsaa());
    if (want == m_msaaApplied || !m_backBuffer) return;
    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    m_backBuffer->m_samples = want;
    bool ok = m_backBuffer->CreateStandalone();
    if (m_autoDepth) {
        m_autoDepth->m_samples = want;
        ok = m_autoDepth->CreateStandalone() && ok;
        if (ok) m_ctx->ClearDepthStencilView(m_autoDepth->Dsv(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
    }
    m_msaaApplied = want;
    m_waterSnapshotReady = false;
    m_rtDirty = true;
    Log("MSAA changed to %ux%s", want, ok ? "" : " (surface creation failed)");
}

void Device::ApplyRenderTargets()
{
    if (!m_rtDirty) return;
    ID3D11RenderTargetView* rtv = m_rt ? m_rt->Rtv() : nullptr;
    ID3D11DepthStencilView* dsv = m_ds ? m_ds->Dsv() : nullptr;
    if (rtv && dsv && m_rt->m_samples != m_ds->m_samples) {
        // Only the back buffer and its auto depth are multisampled.
        LogOnce("rt-ds-samples", "render target and depth sample counts differ (%u/%u): depth unbound",
                m_rt->m_samples, m_ds->m_samples);
        dsv = nullptr;
    }
    // A texture bound for sampling cannot also be a render target: drop all texture bindings
    // so PrepareDraw rebinds them.
    ID3D11ShaderResourceView* nulls[kMaxStages] = {};
    BindPSSrv(0, kMaxStages, nulls);
    m_ctx->OMSetRenderTargets(1, &rtv, dsv);
    m_rtDirty = false;
}

HRESULT Device::SetViewport(const D3D8Viewport* vp)
{
    if (!vp) return D8ERR_INVALIDCALL;
    if (m_recordingStateBlock) return S_OK;
    if (memcmp(&m_viewport, vp, sizeof(*vp))) FlushTerrainBatch();
    m_viewport = *vp;
    m_viewportDirty = true;
    return S_OK;
}

HRESULT Device::Clear(DWORD count, const D3D8Rect* rects, DWORD flags, DWORD color, float z, DWORD stencil)
{
    FlushTerrainBatch();
    if((flags & D8CLEAR_ZBUFFER) && m_ds==m_autoDepth) m_waterSnapshotReady=false;
    TraceSun("Clear flags %lu z %.3f rt %s ds %s viewport %ux%u", flags, z, m_rt == m_backBuffer ? "back" : "other",
             m_ds == m_autoDepth ? "auto" : "other", m_viewport.Width, m_viewport.Height);
    if (m_sunStarted && (flags & D8CLEAR_ZBUFFER) && m_ds == m_autoDepth) {
        // JPOG draws the world in more than one depth segment per frame, each with its own
        // camera. Finish this one; the next terrain draw starts a new one.
        ResolveSun("clear");
        InvalidateBindings();
        m_sunStarted = m_sunResolved = false;
    }
    ApplyRenderTargets();
    const bool fullViewport = m_rt && m_viewport.X == 0 && m_viewport.Y == 0 &&
                              m_viewport.Width >= m_rt->m_width && m_viewport.Height >= m_rt->m_height;
    if (count == 0 && fullViewport) {
        if ((flags & D8CLEAR_TARGET) && m_rt && m_rt->Rtv()) {
            float c[4];
            ColorToFloat4(color, c);
            m_ctx->ClearRenderTargetView(m_rt->Rtv(), c);
        }
        if ((flags & (D8CLEAR_ZBUFFER | D8CLEAR_STENCIL)) && m_ds && m_ds->Dsv()) {
            UINT f = ((flags & D8CLEAR_ZBUFFER) ? D3D11_CLEAR_DEPTH : 0) | ((flags & D8CLEAR_STENCIL) ? D3D11_CLEAR_STENCIL : 0);
            m_ctx->ClearDepthStencilView(m_ds->Dsv(), f, z, (UINT8)stencil);
        }
        return S_OK;
    }
    ClearWithQuad(count, rects, flags, color, z);
    return S_OK;
}

void Device::ClearWithQuad(DWORD count, const D3D8Rect* rects, DWORD flags, DWORD color, float z)
{
    HelperConstants hc = {};
    ColorToFloat4(color, hc.color);
    hc.misc[0] = z;
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(m_ctx->Map(m_clearCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, &hc, sizeof(hc));
        m_ctx->Unmap(m_clearCB.Get(), 0);
    }
    static ComPtr<ID3D11BlendState> blendColor, blendNone;
    static ComPtr<ID3D11DepthStencilState> depthWrite;
    if (!blendColor) {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        m_device->CreateBlendState(&bd, &blendColor);
        bd.RenderTarget[0].RenderTargetWriteMask = 0;
        m_device->CreateBlendState(&bd, &blendNone);
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        m_device->CreateDepthStencilState(&dd, &depthWrite);
    }
    D3D11_VIEWPORT vp = { (float)m_viewport.X, (float)m_viewport.Y, (float)m_viewport.Width, (float)m_viewport.Height, 0.0f, 1.0f };
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
    m_ctx->VSSetConstantBuffers(0, 1, m_clearCB.GetAddressOf());
    m_ctx->PSSetShader(m_clearPS.Get(), nullptr, 0);
    m_ctx->PSSetConstantBuffers(0, 1, m_clearCB.GetAddressOf());
    m_ctx->OMSetBlendState((flags & D8CLEAR_TARGET) ? blendColor.Get() : blendNone.Get(), nullptr, 0xFFFFFFFF);
    m_ctx->OMSetDepthStencilState((flags & D8CLEAR_ZBUFFER) && m_ds ? depthWrite.Get() : m_noDepth.Get(), 0);
    if (count && rects) {
        m_ctx->RSSetState(m_noCullScissor.Get());
        for (DWORD i = 0; i < count; i++) {
            D3D11_RECT r = { rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2 };
            m_ctx->RSSetScissorRects(1, &r);
            m_ctx->Draw(3, 0);
        }
    } else {
        m_ctx->RSSetState(m_noCull.Get());
        m_ctx->Draw(3, 0);
    }
    // Force PrepareDraw to rebind everything we touched.
    InvalidateBindings();
    m_viewportDirty = true;
    m_vsConstDirty = m_psConstDirty = true;
    m_lastVS = nullptr; m_lastPS = nullptr; m_lastLayout = nullptr;
    m_lastBlend = nullptr; m_lastDepth = nullptr; m_lastRaster = nullptr;
}

// ---- transforms / lights (stored only: the game uses vertex shaders) ------------------------------

HRESULT Device::SetTransform(DWORD state, const D3D8Matrix* m)
{
    if (!m || state >= 512) return D8ERR_INVALIDCALL;
    m_transforms[state] = *m;
    return S_OK;
}

HRESULT Device::GetTransform(DWORD state, D3D8Matrix* m)
{
    if (!m || state >= 512) return D8ERR_INVALIDCALL;
    *m = m_transforms[state];
    return S_OK;
}

HRESULT Device::MultiplyTransform(DWORD state, const D3D8Matrix* m)
{
    if (!m || state >= 512) return D8ERR_INVALIDCALL;
    D3D8Matrix r = {};
    const D3D8Matrix& a = m_transforms[state];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * m->m[k][j];
    m_transforms[state] = r;
    return S_OK;
}

HRESULT Device::SetLight(DWORD index, const D3D8Light* l)
{
    if (!l) return D8ERR_INVALIDCALL;
    m_lights[index] = *l;
    return S_OK;
}

HRESULT Device::GetLight(DWORD index, D3D8Light* l)
{
    auto it = m_lights.find(index);
    if (!l || it == m_lights.end()) return D8ERR_INVALIDCALL;
    *l = it->second;
    return S_OK;
}

HRESULT Device::LightEnable(DWORD index, BOOL enable)
{
    m_lightEnabled[index] = enable;
    return S_OK;
}

HRESULT Device::GetLightEnable(DWORD index, BOOL* enable)
{
    if (!enable) return D8ERR_INVALIDCALL;
    auto it = m_lightEnabled.find(index);
    *enable = it != m_lightEnabled.end() ? it->second : FALSE;
    return S_OK;
}

// ---- render / texture stage states ---------------------------------------------------------------

HRESULT Device::SetRenderState(DWORD state, DWORD value)
{
    if (state >= RS_COUNT) return D8ERR_INVALIDCALL;
    if (m_recordingStateBlock) return S_OK;
    if (m_rs[state] == value) { ++m_perf.redundantRenderStates; return S_OK; }
    FlushTerrainBatch();
    m_rs[state] = value;
    switch (state) {
    case RS_TEXTUREFACTOR: ColorToFloat4(value, m_psConst.textureFactor); m_psConstDirty = true; break;
    case RS_ALPHAREF: m_psConst.alphaRef[0] = (float)(value & 0xFF); m_psConstDirty = true; break;
    case RS_FOGCOLOR: ColorToFloat4(value, m_psConst.fogColor); m_psConstDirty = true; break;
    case RS_FOGENABLE:
        if (value) LogOnce("fog", "fog enabled (table mode %lu) - not implemented", m_rs[RS_FOGTABLEMODE]);
        break;
    case RS_STENCILENABLE:
        if (value) LogOnce("stencil", "stencil enabled");
        break;
    default: break;
    }
    return S_OK;
}

HRESULT Device::GetRenderState(DWORD state, DWORD* value)
{
    if (state >= RS_COUNT || !value) return D8ERR_INVALIDCALL;
    *value = m_rs[state];
    return S_OK;
}

HRESULT Device::BeginStateBlock() { m_recordingStateBlock++; return S_OK; }

HRESULT Device::EndStateBlock(DWORD* token)
{
    if (m_recordingStateBlock) m_recordingStateBlock--;
    if (token) *token = 1;
    return S_OK;
}

HRESULT Device::ApplyStateBlock(DWORD)
{
    LogOnce("applystateblock", "ApplyStateBlock is not implemented");
    return S_OK;
}

HRESULT Device::CreateStateBlock(DWORD, DWORD* token)
{
    if (token) *token = 1;
    return S_OK;
}

HRESULT Device::GetTexture(DWORD stage, ID8BaseTexture** out)
{
    if (stage >= kMaxStages || !out) return D8ERR_INVALIDCALL;
    *out = m_textures[stage];
    if (*out) (*out)->AddRef();
    return S_OK;
}

HRESULT Device::SetTexture(DWORD stage, ID8BaseTexture* tex)
{
    if (stage >= kMaxStages) return D8ERR_INVALIDCALL;
    if (m_recordingStateBlock) return S_OK;
    if (m_textures[stage] != tex) FlushTerrainBatch();
    SetRef(m_textures[stage], tex);
    return S_OK;
}

HRESULT Device::GetTextureStageState(DWORD stage, DWORD type, DWORD* value)
{
    if (stage >= kMaxStages || type >= TSS_COUNT || !value) return D8ERR_INVALIDCALL;
    *value = m_tss[stage][type];
    return S_OK;
}

HRESULT Device::SetTextureStageState(DWORD stage, DWORD type, DWORD value)
{
    if (stage >= kMaxStages || type >= TSS_COUNT) return D8ERR_INVALIDCALL;
    if (m_recordingStateBlock) return S_OK;
    if (m_tss[stage][type] == value) { ++m_perf.redundantTextureStates; return S_OK; }
    FlushTerrainBatch();
    m_tss[stage][type] = value;
    switch (type) {
    case TSS_ADDRESSU: case TSS_ADDRESSV: case TSS_ADDRESSW: case TSS_MAGFILTER: case TSS_MINFILTER: case TSS_MIPFILTER:
    case TSS_MIPMAPLODBIAS: case TSS_MAXMIPLEVEL: case TSS_MAXANISOTROPY: case TSS_BORDERCOLOR:
        m_samplerDirty |= 1u << stage; break;
    default: break;
    }
    switch (type) {
    case TSS_BUMPENVMAT00: m_psConst.bumpMat[stage][0] = AsFloat(value); m_psConstDirty = true; break;
    case TSS_BUMPENVMAT01: m_psConst.bumpMat[stage][1] = AsFloat(value); m_psConstDirty = true; break;
    case TSS_BUMPENVMAT10: m_psConst.bumpMat[stage][2] = AsFloat(value); m_psConstDirty = true; break;
    case TSS_BUMPENVMAT11: m_psConst.bumpMat[stage][3] = AsFloat(value); m_psConstDirty = true; break;
    case TSS_BUMPENVLSCALE: m_psConst.bumpLum[stage][0] = AsFloat(value); m_psConstDirty = true; break;
    case TSS_BUMPENVLOFFSET: m_psConst.bumpLum[stage][1] = AsFloat(value); m_psConstDirty = true; break;
    default: break;
    }
    return S_OK;
}

// ---- shaders -----------------------------------------------------------------------------------------

HRESULT Device::CreateVertexShader(const DWORD* decl, const DWORD* func, DWORD* handle, DWORD)
{
    if (!decl || !handle) return D8ERR_INVALIDCALL;
    VertexShader8* vs = new VertexShader8();
    size_t n = ParseVSDecl(decl, vs->parsed);
    vs->worldKind=WorldShaderKind(vs->parsed);
    vs->decl.assign(decl, decl + n);
    if (func) vs->func.assign(func, func + ShaderTokenCount(func));
    m_vertexShaders.push_back(vs);
    *handle = (DWORD)((m_vertexShaders.size() - 1) << 1) | 1;
    if (!BuildVertexShader(*vs, *handle)) vs->failed = true;
    return S_OK;
}

bool Device::BuildVertexShader(VertexShader8& vs, DWORD handle)
{
    if (vs.func.empty()) { Log("vertex shader 0x%lx has no function (fixed-function T&L) - unsupported", handle); return false; }
    std::string error;
    vs.constRegs = VSConstantRegisters(vs.func.data());
    std::string hlsl = TranslateVS(vs.func.data(), vs.parsed, error);
    if (hlsl.empty()) { Log("vertex shader 0x%lx translation failed: %s", handle, error.c_str()); return false; }
    char name[64];
    sprintf_s(name, "vs_%04lx_%08x", handle, (unsigned)std::hash<std::string>()(hlsl));
    // vs_5_0 so the output can feed the hull shader of tessellated draws.
    ComPtr<ID3DBlob> blob = CompileHLSL(hlsl, m_device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? "vs_5_0" : "vs_4_0", name);
    if (!blob) return false;
    if (FAILED(m_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs.vs))) return false;
    std::vector<D3D11_INPUT_ELEMENT_DESC> elems;
    for (const VSInput& in : vs.parsed.inputs)
        elems.push_back({ "TEXCOORD", (UINT)in.reg, DeclFormat(in.type), (UINT)in.stream, in.offset,
                          D3D11_INPUT_PER_VERTEX_DATA, 0 });
    if (!elems.empty() &&
        FAILED(m_device->CreateInputLayout(elems.data(), (UINT)elems.size(), blob->GetBufferPointer(), blob->GetBufferSize(), &vs.layout))) {
        Log("vertex shader 0x%lx: CreateInputLayout failed", handle);
        return false;
    }
    Log("vertex shader 0x%lx ready (%zu inputs)", handle, vs.parsed.inputs.size());
    return true;
}

HRESULT Device::SetVertexShader(DWORD handle)
{
    if (m_recordingStateBlock) return S_OK;
    if (m_vsHandle != handle) FlushTerrainBatch();
    m_vsHandle = handle;
    if (handle & 1) {
        size_t idx = handle >> 1;
        if (idx < m_vertexShaders.size() && m_vertexShaders[idx]) {
            // D3D8 loads the constants embedded in the declaration when the shader is set.
            for (auto& c : m_vertexShaders[idx]->parsed.constants) {
                UINT count = (UINT)c.second.size() / 4;
                if (c.first + count <= kMaxVSConst &&
                    CopyConstantsIfChanged(m_vsConst[c.first], c.second.data(), count * 16)) {
                    m_vsConstDirty = true;
                    FlushTerrainBatch();
                }
                if (c.first + count <= kMaxVSConst && c.first + count > m_vsConstUsed) { m_vsConstUsed = c.first + count; m_vsConstDirty = true; }
            }
        }
    }
    return S_OK;
}

HRESULT Device::DeleteVertexShader(DWORD handle)
{
    FlushTerrainBatch();
    size_t idx = handle >> 1;
    if (!(handle & 1) || idx >= m_vertexShaders.size() || !m_vertexShaders[idx]) return D8ERR_INVALIDCALL;
    delete m_vertexShaders[idx];
    m_vertexShaders[idx] = nullptr;
    return S_OK;
}

HRESULT Device::SetVertexShaderConstant(DWORD reg, const void* data, DWORD count)
{
    if (!data || reg + count > kMaxVSConst) return D8ERR_INVALIDCALL;
    if (m_terrainBatchOpen) {
        // The recorded terrain sections differ only in c4/c5 (kept with each section).
        for (DWORD r = reg; r < reg + count; r++)
            if ((r < 4 || r > 5) && memcmp(m_vsConst[r], (const BYTE*)data + (r - reg) * 16, 16)) { FlushTerrainBatch(); break; }
    }
    if (CopyConstantsIfChanged(m_vsConst[reg], data, count * 16)) m_vsConstDirty = true;
    else ++m_perf.duplicateConstants;
    if (reg + count > m_vsConstUsed) { m_vsConstUsed = reg + count; m_vsConstDirty = true; }
    if (reg < m_vsWriteLo) m_vsWriteLo = reg;
    if (reg + count > m_vsWriteHi) m_vsWriteHi = reg + count;
    return S_OK;
}

HRESULT Device::GetVertexShaderConstant(DWORD reg, void* data, DWORD count)
{
    if (!data || reg + count > kMaxVSConst) return D8ERR_INVALIDCALL;
    memcpy(data, m_vsConst[reg], count * 16);
    return S_OK;
}

HRESULT Device::GetVertexShaderDeclaration(DWORD handle, void* data, DWORD* size)
{
    size_t idx = handle >> 1;
    if (!size || !(handle & 1) || idx >= m_vertexShaders.size() || !m_vertexShaders[idx]) return D8ERR_INVALIDCALL;
    const auto& d = m_vertexShaders[idx]->decl;
    DWORD bytes = (DWORD)(d.size() * 4);
    if (data) { if (*size < bytes) return D8ERR_INVALIDCALL; memcpy(data, d.data(), bytes); }
    *size = bytes;
    return S_OK;
}

HRESULT Device::GetVertexShaderFunction(DWORD handle, void* data, DWORD* size)
{
    size_t idx = handle >> 1;
    if (!size || !(handle & 1) || idx >= m_vertexShaders.size() || !m_vertexShaders[idx]) return D8ERR_INVALIDCALL;
    const auto& f = m_vertexShaders[idx]->func;
    DWORD bytes = (DWORD)(f.size() * 4);
    if (data) { if (*size < bytes) return D8ERR_INVALIDCALL; memcpy(data, f.data(), bytes); }
    *size = bytes;
    return S_OK;
}

HRESULT Device::CreatePixelShader(const DWORD* func, DWORD* handle)
{
    if (!func || !handle) return D8ERR_INVALIDCALL;
    PixelShader8* ps = new PixelShader8();
    ps->func.assign(func, func + ShaderTokenCount(func));
    ps->samplesStage1=PixelShaderSamplesStage(func,1);
    ps->water=IsWaterPixelShader(func);
    ps->hash = shader_jobs::FunctionHash(ps->func);
    // The variants drawn with this shader in earlier sessions start compiling now, during the load.
    if (g_config.asyncShaders && g_config.prewarmShaders) {
        for (const PSVariant& v : shader_jobs::KnownVariants(ps->hash)) {
            std::string error;
            std::string hlsl = TranslatePS(ps->func.data(), v, error);
            if (hlsl.empty()) continue;
            char name[64];
            sprintf_s(name, "ps_%04zx_%08x", m_pixelShaders.size() + 1, (unsigned)std::hash<std::string>()(hlsl));
            ps->variants.push_back({ v, nullptr, shader_jobs::Submit(m_device.Get(), std::move(hlsl), name, true) });
        }
    }
    m_pixelShaders.push_back(ps);
    *handle = (DWORD)m_pixelShaders.size();
    return S_OK;
}

HRESULT Device::SetPixelShader(DWORD handle)
{
    if (m_recordingStateBlock) return S_OK;
    if (m_psHandle != handle) FlushTerrainBatch();
    m_psHandle = handle;
    return S_OK;
}

HRESULT Device::DeletePixelShader(DWORD handle)
{
    FlushTerrainBatch();
    if (handle == 0 || handle > m_pixelShaders.size() || !m_pixelShaders[handle - 1]) return D8ERR_INVALIDCALL;
    delete m_pixelShaders[handle - 1];
    m_pixelShaders[handle - 1] = nullptr;
    return S_OK;
}

HRESULT Device::SetPixelShaderConstant(DWORD reg, const void* data, DWORD count)
{
    if (!data || reg + count > kMaxPSConst) return D8ERR_INVALIDCALL;
    if (m_terrainBatchOpen && memcmp(m_psConst.c[reg], data, count * 16)) FlushTerrainBatch();
    if (CopyConstantsIfChanged(m_psConst.c[reg], data, count * 16)) m_psConstDirty = true;
    else ++m_perf.duplicateConstants;
    return S_OK;
}

HRESULT Device::GetPixelShaderConstant(DWORD reg, void* data, DWORD count)
{
    if (!data || reg + count > kMaxPSConst) return D8ERR_INVALIDCALL;
    memcpy(data, m_psConst.c[reg], count * 16);
    return S_OK;
}

HRESULT Device::GetPixelShaderFunction(DWORD handle, void* data, DWORD* size)
{
    if (!size || handle == 0 || handle > m_pixelShaders.size() || !m_pixelShaders[handle - 1]) return D8ERR_INVALIDCALL;
    const auto& f = m_pixelShaders[handle - 1]->func;
    DWORD bytes = (DWORD)(f.size() * 4);
    if (data) { if (*size < bytes) return D8ERR_INVALIDCALL; memcpy(data, f.data(), bytes); }
    *size = bytes;
    return S_OK;
}

// ---- streams ----------------------------------------------------------------------------------------

HRESULT Device::SetStreamSource(UINT stream, ID8VertexBuffer* vb, UINT stride)
{
    if (stream >= kMaxStreams) return D8ERR_INVALIDCALL;
    if (m_recordingStateBlock) return S_OK;
    // Stream 0 is per terrain section (recorded with it); the others belong to the whole batch.
    if (stream && (m_streams[stream].vb != vb || m_streams[stream].stride != stride)) FlushTerrainBatch();
    SetRef(m_streams[stream].vb, static_cast<VertexBuffer*>(vb));
    m_streams[stream].stride = stride;
    return S_OK;
}

HRESULT Device::GetStreamSource(UINT stream, ID8VertexBuffer** vb, UINT* stride)
{
    if (stream >= kMaxStreams || !vb || !stride) return D8ERR_INVALIDCALL;
    *vb = m_streams[stream].vb;
    if (*vb) (*vb)->AddRef();
    *stride = m_streams[stream].stride;
    return S_OK;
}

HRESULT Device::SetIndices(ID8IndexBuffer* ib, UINT baseVertex)
{
    if (m_recordingStateBlock) return S_OK;
    SetRef(m_indices, static_cast<IndexBuffer*>(ib));
    m_baseVertex = baseVertex;
    return S_OK;
}

HRESULT Device::GetIndices(ID8IndexBuffer** ib, UINT* baseVertex)
{
    if (!ib || !baseVertex) return D8ERR_INVALIDCALL;
    *ib = m_indices;
    if (*ib) (*ib)->AddRef();
    *baseVertex = m_baseVertex;
    return S_OK;
}

// ---- pipeline state ---------------------------------------------------------------------------------

ID3D11BlendState* Device::GetBlendState()
{
    const bool enable = m_rs[RS_ALPHABLENDENABLE] != 0;
    DWORD src = m_rs[RS_SRCBLEND], dst = m_rs[RS_DESTBLEND];
    if (src == 12) { src = 5; dst = 6; }       // BOTHSRCALPHA
    else if (src == 13) { src = 6; dst = 5; }  // BOTHINVSRCALPHA
    const uint64_t key = (enable ? 1ull : 0ull) | ((uint64_t)src << 1) | ((uint64_t)dst << 9) |
                         ((uint64_t)(m_rs[RS_BLENDOP] & 7) << 17) | ((uint64_t)(m_rs[RS_COLORWRITEENABLE] & 0xF) << 20) |
                         ((uint64_t)(AlphaToCoverageActive() ? 1 : 0) << 24);
    if (key == m_blendKey) return m_blendKeyState;
    auto it = m_blendStates.find(key);
    if (it != m_blendStates.end()) { m_blendKey = key; return m_blendKeyState = it->second.Get(); }
    D3D11_BLEND_DESC bd = {};
    D3D11_RENDER_TARGET_BLEND_DESC& r = bd.RenderTarget[0];
    r.BlendEnable = enable;
    r.SrcBlend = MapBlend(src, false);
    r.DestBlend = MapBlend(dst, false);
    r.BlendOp = (m_rs[RS_BLENDOP] >= 1 && m_rs[RS_BLENDOP] <= 5) ? (D3D11_BLEND_OP)m_rs[RS_BLENDOP] : D3D11_BLEND_OP_ADD;
    r.SrcBlendAlpha = MapBlend(src, true);
    r.DestBlendAlpha = MapBlend(dst, true);
    r.BlendOpAlpha = r.BlendOp;
    r.RenderTargetWriteMask = (UINT8)(m_rs[RS_COLORWRITEENABLE] & 0xF);
    bd.AlphaToCoverageEnable = AlphaToCoverageActive();
    ComPtr<ID3D11BlendState> st;
    m_device->CreateBlendState(&bd, &st);
    m_blendStates[key] = st;
    m_blendKey = key;
    return m_blendKeyState = st.Get();
}

ID3D11DepthStencilState* Device::GetDepthState()
{
    const bool z = m_rs[RS_ZENABLE] != 0;
    const bool stencil = m_rs[RS_STENCILENABLE] != 0;
    uint64_t key = (z ? 1ull : 0ull) | ((m_rs[RS_ZWRITEENABLE] ? 1ull : 0ull) << 1) | ((uint64_t)(m_rs[RS_ZFUNC] & 15) << 2);
    if (stencil)
        key |= (1ull << 6) | ((uint64_t)(m_rs[RS_STENCILFUNC] & 15) << 7) | ((uint64_t)(m_rs[RS_STENCILFAIL] & 15) << 11) |
               ((uint64_t)(m_rs[RS_STENCILZFAIL] & 15) << 15) | ((uint64_t)(m_rs[RS_STENCILPASS] & 15) << 19) |
               ((uint64_t)(m_rs[RS_STENCILMASK] & 0xFF) << 23) | ((uint64_t)(m_rs[RS_STENCILWRITEMASK] & 0xFF) << 31);
    if (key == m_depthKey) return m_depthKeyState;
    auto it = m_depthStates.find(key);
    if (it != m_depthStates.end()) { m_depthKey = key; return m_depthKeyState = it->second.Get(); }
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = z;
    dd.DepthWriteMask = m_rs[RS_ZWRITEENABLE] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = MapCmp(m_rs[RS_ZFUNC]);
    if (stencil) {
        dd.StencilEnable = TRUE;
        dd.StencilReadMask = (UINT8)m_rs[RS_STENCILMASK];
        dd.StencilWriteMask = (UINT8)m_rs[RS_STENCILWRITEMASK];
        D3D11_DEPTH_STENCILOP_DESC op = { (D3D11_STENCIL_OP)m_rs[RS_STENCILFAIL], (D3D11_STENCIL_OP)m_rs[RS_STENCILZFAIL],
                                          (D3D11_STENCIL_OP)m_rs[RS_STENCILPASS], MapCmp(m_rs[RS_STENCILFUNC]) };
        dd.FrontFace = dd.BackFace = op;
    }
    ComPtr<ID3D11DepthStencilState> st;
    m_device->CreateDepthStencilState(&dd, &st);
    m_depthStates[key] = st;
    m_depthKey = key;
    return m_depthKeyState = st.Get();
}

ID3D11RasterizerState* Device::GetRasterState()
{
    const uint64_t key = (m_rs[RS_CULLMODE] & 3) | ((uint64_t)(m_rs[RS_FILLMODE] & 3) << 2) | ((uint64_t)(m_rs[RS_ZBIAS] & 0xFF) << 4);
    if (key == m_rasterKey) return m_rasterKeyState;
    auto it = m_rasterStates.find(key);
    if (it != m_rasterStates.end()) { m_rasterKey = key; return m_rasterKeyState = it->second.Get(); }
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = m_rs[RS_FILLMODE] == 2 ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    rd.CullMode = m_rs[RS_CULLMODE] == 2 ? D3D11_CULL_FRONT : (m_rs[RS_CULLMODE] == 3 ? D3D11_CULL_BACK : D3D11_CULL_NONE);
    rd.DepthBias = -(INT)(m_rs[RS_ZBIAS] * 32);
    rd.SlopeScaledDepthBias = -(float)m_rs[RS_ZBIAS] * 0.25f;
    rd.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> st;
    m_device->CreateRasterizerState(&rd, &st);
    m_rasterStates[key] = st;
    m_rasterKey = key;
    return m_rasterKeyState = st.Get();
}

ID3D11SamplerState* Device::GetSampler(int stage)
{
    auto& selected = m_samplerSelections[stage];
    const int effectiveAniso = g_config.EffectiveAnisotropy();
    if (!(m_samplerDirty & (1u << stage)) && m_samplerAniso[stage] == effectiveAniso && selected.state) {
        ++m_perf.samplerSelectionHits;
        return selected.state;
    }
    m_samplerDirty &= ~(1u << stage);
    m_samplerAniso[stage] = effectiveAniso;
    const DWORD* t = m_tss[stage];
    DWORD mag = t[TSS_MAGFILTER], min = t[TSS_MINFILTER], mip = t[TSS_MIPFILTER];
    DWORD aniso = t[TSS_MAXANISOTROPY];
    if (effectiveAniso > 1 && min >= 2 && mip >= 1) { min = 3; aniso = (DWORD)effectiveAniso; }
    DWORD key[10] = { t[TSS_ADDRESSU], t[TSS_ADDRESSV], t[TSS_ADDRESSW], mag, min, mip, t[TSS_MIPMAPLODBIAS],
                      t[TSS_MAXMIPLEVEL], aniso, t[TSS_BORDERCOLOR] };
    if (selected.Matches(key)) { ++m_perf.samplerSelectionHits; return selected.state; }
    ++m_perf.samplerSelectionLookups;
    std::string k((const char*)key, sizeof(key));
    auto it = m_samplers.find(k);
    if (it != m_samplers.end()) { selected.Remember(key,it->second.Get()); return it->second.Get(); }
    D3D11_SAMPLER_DESC sd = {};
    if (mag == 3 || min == 3) {
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = aniso < 1 ? 1 : (aniso > 16 ? 16 : aniso);
    } else {
        bool minL = min == 2, magL = mag == 2, mipL = mip == 2;
        sd.Filter = (D3D11_FILTER)((minL ? 0x10 : 0) | (magL ? 0x4 : 0) | (mipL ? 0x1 : 0));
        sd.MaxAnisotropy = 1;
    }
    sd.AddressU = MapAddress(t[TSS_ADDRESSU]);
    sd.AddressV = MapAddress(t[TSS_ADDRESSV]);
    sd.AddressW = MapAddress(t[TSS_ADDRESSW]);
    sd.MipLODBias = AsFloat(t[TSS_MIPMAPLODBIAS]);
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ColorToFloat4(t[TSS_BORDERCOLOR], sd.BorderColor);
    sd.MinLOD = (float)t[TSS_MAXMIPLEVEL];
    sd.MaxLOD = mip == 0 ? sd.MinLOD : D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> st;
    m_device->CreateSamplerState(&sd, &st);
    m_samplers[k] = st;
    selected.Remember(key,st.Get()); // lifetime is owned by m_samplers, including across Reset
    return st.Get();
}

static BYTE TextureDim(ID8BaseTexture* t)
{
    if (!t) return 0;
    DWORD type = t->GetType();
    return type == D8RTYPE_VOLUMETEXTURE ? 3 : (type == D8RTYPE_TEXTURE ? 2 : 0);
}

ID3D11PixelShader* Device::GetPixelShader()
{
    const BYTE alphaFunc = m_rs[RS_ALPHATESTENABLE] ? (BYTE)m_rs[RS_ALPHAFUNC] : 0;
    if (m_psHandle) {
        if (m_psHandle > m_pixelShaders.size() || !m_pixelShaders[m_psHandle - 1]) {
            LogOnce(std::string("ps-invalid-") + std::to_string(m_psHandle), "draw rejected: invalid pixel shader handle 0x%lx", m_psHandle);
            return nullptr;
        }
        PixelShader8* ps = m_pixelShaders[m_psHandle - 1];
        PSVariant v;
        for (int s = 0; s < kMaxStages; s++) v.texDim[s] = m_drawTexDim[s]; // PrepareDraw, this draw
        v.alphaFunc = alphaFunc;
        v.shadowOnly = m_sun.flags[1] > 0.5f ? 1 : 0;
        v.a2c = AlphaToCoverageActive() ? 1 : 0;
        v.normalMap = ActiveNormalMap() ? 1 : 0;
        v.material = ActiveMaterial() ? BYTE(MaterialTextureStage()+1):0;
        v.relief = m_reliefActive ? 1 : 0;
        v.water = m_waterDraw ? 1 : 0;
        v.emissive = ActiveEmissive() ? 1 : 0;
        v.roughMap = ActiveRoughness() ? 1 : 0;
        v.waterSamples = m_waterDraw && m_waterSamples>1 ? (BYTE)m_waterSamples : 0;
        if (v.normalMap && m_drawDiagnostics.First(m_vsHandle,m_psHandle,DrawDiagnostics::NormalMap)) Log(
                                 "normal map draw: VS 0x%lx PS 0x%lx kind %d", m_vsHandle, m_psHandle, m_worldKind);
        if (ps->lastShader && ps->lastVariant == v) return ps->lastShader;
        for (auto& e : ps->variants) {
            if (!(e.key == v)) continue;
            ID3D11PixelShader* shader = ShaderOf(e.shader, e.job);
            if (shader) { ps->lastVariant = v; ps->lastShader = shader; }
            return shader;
        }
        shader_jobs::RecordVariant(ps->hash, v);
        std::string error;
        std::string hlsl = TranslatePS(ps->func.data(), v, error);
        ps->variants.push_back({ v, nullptr, nullptr });
        PixelShader8::Variant& e = ps->variants.back();
        if (hlsl.empty()) {
            Log("pixel shader %lu translation failed: %s", m_psHandle, error.c_str());
            return nullptr;
        }
        char name[64];
        sprintf_s(name, "ps_%04lx_%08x", m_psHandle, (unsigned)std::hash<std::string>()(hlsl));
        ComPtr<ID3DBlob> blob = g_config.asyncShaders ? CachedHLSL(hlsl, "ps_4_0") : CompileHLSL(hlsl, "ps_4_0", name);
        if (!blob && g_config.asyncShaders) {
            e.job = shader_jobs::Submit(m_device.Get(), std::move(hlsl), name, false);
            m_psPending = true;
            return nullptr;
        }
        if (blob) {
            HRESULT hr = m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &e.shader);
            if (FAILED(hr)) Log("pixel shader %lu CreatePixelShader failed 0x%08lx", m_psHandle, hr);
        }
        return e.shader.Get();
    }
    FFKey k = {};
    for (int s = 0; s < kMaxStages; s++) {
        const DWORD* t = m_tss[s];
        FFStageKey& g = k.stages[s];
        g.colorOp = (BYTE)t[TSS_COLOROP];
        if (g.colorOp <= 1) break;
        g.colorArg1 = (BYTE)t[TSS_COLORARG1]; g.colorArg2 = (BYTE)t[TSS_COLORARG2]; g.colorArg0 = (BYTE)t[TSS_COLORARG0];
        g.alphaOp = (BYTE)t[TSS_ALPHAOP];
        g.alphaArg1 = (BYTE)t[TSS_ALPHAARG1]; g.alphaArg2 = (BYTE)t[TSS_ALPHAARG2]; g.alphaArg0 = (BYTE)t[TSS_ALPHAARG0];
        g.texCoordIndex = (BYTE)(t[TSS_TEXCOORDINDEX] & 7);
        g.texDim = TextureDim(m_textures[s]);
        g.projected = (t[TSS_TEXTURETRANSFORMFLAGS] & 256) ? 1 : 0;
    }
    k.alphaFunc = alphaFunc;
    k.specular = m_rs[RS_SPECULARENABLE] ? 1 : 0;
    k.shadowOnly = m_sun.flags[1] > 0.5f ? 1 : 0;
    k.a2c = AlphaToCoverageActive() ? 1 : 0;
    k.normalMap = ActiveNormalMap() ? 1 : 0;
    k.material = ActiveMaterial() ? BYTE(MaterialTextureStage()+1):0;
    std::string key((const char*)&k, sizeof(k));
    auto it = m_ffShaders.find(key);
    if (it != m_ffShaders.end()) return ShaderOf(it->second.shader, it->second.job);
    shader_jobs::RecordFF(k);
    std::string hlsl = GenerateFFPS(k);
    char name[64];
    sprintf_s(name, "ff_%08x", (unsigned)std::hash<std::string>()(hlsl));
    FFShader& e = m_ffShaders[key];
    ComPtr<ID3DBlob> blob = g_config.asyncShaders ? CachedHLSL(hlsl, "ps_4_0") : CompileHLSL(hlsl, "ps_4_0", name);
    if (!blob && g_config.asyncShaders) {
        e.job = shader_jobs::Submit(m_device.Get(), std::move(hlsl), name, false);
        m_psPending = true;
        return nullptr;
    }
    if (blob) m_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &e.shader);
    return e.shader.Get();
}

// The shader of a variant entry; null while its background job is still running (the draw is skipped).
ID3D11PixelShader* Device::ShaderOf(ComPtr<ID3D11PixelShader>& shader, std::shared_ptr<shader_jobs::Job>& job)
{
    if (job) {
        if (!job->done.load(std::memory_order_acquire)) {
            if (job->prewarm) shader_jobs::Promote(job); // no-op once it is running
            m_psPending = true;
            return nullptr;
        }
        shader = job->shader;
        job.reset();
    }
    return shader.Get();
}

void Device::PrewarmFF()
{
    if (!g_config.asyncShaders || !g_config.prewarmShaders) return;
    for (const FFKey& k : shader_jobs::KnownFF()) {
        std::string hlsl = GenerateFFPS(k);
        char name[64];
        sprintf_s(name, "ff_%08x", (unsigned)std::hash<std::string>()(hlsl));
        m_ffShaders[std::string((const char*)&k, sizeof(k))] = { nullptr, shader_jobs::Submit(m_device.Get(), std::move(hlsl), name, true) };
    }
}

namespace {
// True when the slots already hold these objects; otherwise records them.
template<class T> __forceinline bool SameBinding(T** cache, UINT capacity, UINT slot, UINT n, T* const* v)
{
    if (slot + n > capacity) return false;
    bool same = true;
    for (UINT i = 0; i < n; i++)
        if (cache[slot + i] != v[i]) { cache[slot + i] = v[i]; same = false; }
    return same;
}
}
void Device::InvalidateBindings() { memset(&m_bind, 0xFF, sizeof(m_bind)); m_vsRingBound = false; }
void Device::BindPSSrv(UINT s, UINT n, ID3D11ShaderResourceView* const* v)
{ if (SameBinding(m_bind.psSrv, BindingCache::Srv, s, n, v)) ++m_perf.redundantBinds; else m_ctx->PSSetShaderResources(s, n, v); }
void Device::BindVSSrv(UINT s, UINT n, ID3D11ShaderResourceView* const* v)
{ if (SameBinding(m_bind.vsSrv, BindingCache::Srv, s, n, v)) ++m_perf.redundantBinds; else m_ctx->VSSetShaderResources(s, n, v); }
void Device::BindPSSamplers(UINT s, UINT n, ID3D11SamplerState* const* v)
{ if (SameBinding(m_bind.psSamp, BindingCache::Samp, s, n, v)) ++m_perf.redundantBinds; else m_ctx->PSSetSamplers(s, n, v); }
void Device::BindVSSamplers(UINT s, UINT n, ID3D11SamplerState* const* v)
{ if (SameBinding(m_bind.vsSamp, BindingCache::Samp, s, n, v)) ++m_perf.redundantBinds; else m_ctx->VSSetSamplers(s, n, v); }
void Device::BindVSCB(UINT s, UINT n, ID3D11Buffer* const* v)
{ if (SameBinding(m_bind.vsCb, BindingCache::Cb, s, n, v)) ++m_perf.redundantBinds; else m_ctx->VSSetConstantBuffers(s, n, v); }
void Device::BindPSCB(UINT s, UINT n, ID3D11Buffer* const* v)
{ if (SameBinding(m_bind.psCb, BindingCache::Cb, s, n, v)) ++m_perf.redundantBinds; else m_ctx->PSSetConstantBuffers(s, n, v); }

bool Device::PrepareDraw()
{
    FlushTerrainBatch(); // before this draw binds anything
    m_perf.enabled=g_config.performanceGPU;
    CpuPerformanceScope cpu(m_perf,0);
    CpuPerformanceScope detail(m_perf,Performance::CpuSetup);
    if(m_materialStatsFrame!=m_frame){m_materialStatsFrame=m_frame;memset(m_materialDraws,0,sizeof(m_materialDraws));}
    m_perf.enabled=g_config.performanceGPU;
    m_perf.Begin(m_device.Get(),m_ctx.Get());
    ++m_perfDraws;
    // Recorded shadow casters read the current constant ring window: draw them before it wraps,
    // and before this draw binds anything (the flush leaves its own state behind).
    if (!m_shadowBatch.empty() && m_vsRingPos + sizeof(m_vsConst) > kVsRingBytes) FlushShadowBatch();
    // The first depth-less draw to the back buffer after the world is the HUD: apply the
    // sun shadows before it so they never darken the interface.
    if (m_sunStarted && !m_sunResolved && m_rt == m_backBuffer && !m_rs[RS_ZENABLE]) { ResolveSun("hud"); InvalidateBindings(); }
    ApplyRenderTargets();
    if (!m_rt) return false;

    if (m_viewportDirty) {
        D3D11_VIEWPORT vp = { (float)m_viewport.X, (float)m_viewport.Y, (float)m_viewport.Width,
                              (float)m_viewport.Height, m_viewport.MinZ, m_viewport.MaxZ };
        m_ctx->RSSetViewports(1, &vp);
        VSExtraConstants vx = {};
        vx.pixelOffset[0] = m_viewport.Width ? -1.0f / m_viewport.Width : 0.0f;
        vx.pixelOffset[1] = m_viewport.Height ? 1.0f / m_viewport.Height : 0.0f;
        if(EnsureTemporal())TemporalJitter(vx.pixelOffset[2],vx.pixelOffset[3]);
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(m_ctx->Map(m_vsExtraCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            memcpy(map.pData, &vx, sizeof(vx));
            m_ctx->Unmap(m_vsExtraCB.Get(), 0);
        }
        m_viewportDirty = false;
    }

    detail.Next(Performance::CpuScene);
    // Vertex shader + input layout + streams
    if (!(m_vsHandle & 1)) {
        LogOnce(std::string("fvf") + std::to_string(m_vsHandle), "draw with FVF 0x%lx (fixed-function T&L) skipped", m_vsHandle);
        return false;
    }
    size_t idx = m_vsHandle >> 1;
    if (idx >= m_vertexShaders.size() || !m_vertexShaders[idx] || m_vertexShaders[idx]->failed) {
        LogOnce(std::string("vs-invalid-") + std::to_string(m_vsHandle), "draw rejected: invalid vertex shader handle 0x%lx", m_vsHandle);
        return false;
    }
    VertexShader8* vs = m_vertexShaders[idx];
    m_worldKind = vs->worldKind;
    bool waterShader=m_psHandle>0 && m_psHandle<=m_pixelShaders.size() && m_pixelShaders[m_psHandle-1] &&
        m_pixelShaders[m_psHandle-1]->water;
    m_waterShaderDraw = waterShader;
    // The sky dome: the first depth-writing kind-0 draw to the back buffer before the terrain.
    // With the procedural sky it is not drawn; the shadow resolve paints sky pixels (depth 1).
    if (SceneFx() && ProceduralSkyActive() && m_worldKind == 0 && !waterShader && !m_sunStarted && !m_sunResolved &&
        m_rt == m_backBuffer && m_ds && m_rs[RS_ZENABLE] &&
        m_viewport.Width == m_backBuffer->m_width && m_viewport.Height == m_backBuffer->m_height) {
        LogOnce("sky-skip", "procedural sky: the game's sky draw is skipped (VS 0x%lx PS 0x%lx)", m_vsHandle, m_psHandle);
        return false;
    }
    m_worldDraw = SceneFx() && m_worldKind != 0 && !waterShader &&
        (m_psHandle != 0 || m_rs[RS_ZWRITEENABLE]) &&
        m_rt == m_backBuffer && m_ds && m_rs[RS_ZENABLE] &&
        (m_worldKind == 2 || !m_rs[RS_ALPHABLENDENABLE] || m_rs[RS_ALPHATESTENABLE]) &&
        m_viewport.Width == m_backBuffer->m_width && m_viewport.Height == m_backBuffer->m_height;
    m_worldCaster = g_config.sunLighting && m_worldDraw && m_rs[RS_ZWRITEENABLE] && (m_worldKind != 4 || g_config.grassShadows) &&
        (m_worldKind != 2 || g_config.terrainShadows);
    if (SceneFx() && m_rt == m_backBuffer && m_worldKind != 0 &&
        m_drawDiagnostics.First(m_vsHandle,m_psHandle,DrawDiagnostics::SunMaterial)) {
        Log(
            "Sun material VS %lx PS %lx kind %d receiver %d caster %d z %lu write %lu blend %lu alpha %lu",
            m_vsHandle,m_psHandle,m_worldKind,m_worldDraw,m_worldCaster,m_rs[RS_ZENABLE],
            m_rs[RS_ZWRITEENABLE],m_rs[RS_ALPHABLENDENABLE],m_rs[RS_ALPHATESTENABLE]);
    }
    if (g_config.effectsEnabled) TrackSkinTexture();
    CollectDumpTextures();
    m_tessCandidate = TessellationCandidate();
    UpdateSun(false);
    if (g_config.effectsEnabled) PrepareWaterDraw();
    else {
        m_waterDraw = false; m_reliefActive = false;
        // Water displacement is in every translated VS. A null CB reads zero,
        // preventing stale swell constants without uploading WaterConstants per draw.
        ID3D11Buffer* off = nullptr;
        BindVSCB(4, 1, &off);
        BindPSCB(2, 1, &off);
    }
    if(m_waterDraw)++m_perfWaterDraws;
    detail.Next(Performance::CpuDiagnostics);
    if (m_trace && g_config.effectsEnabled) {
        DWORD cam = 0;
        if (m_worldKind == 2) for (int i = 0; i < 16; i++) cam = cam * 31 + *(const DWORD*)&m_vsConst[i / 4][i % 4];
        TraceSun("Draw kind %d world %d caster %d z %lu zwrite %lu blend %lu atest %lu rt %s vp %ux%u terrain-cam %08lx",
                 m_worldKind, m_worldDraw, m_worldCaster && m_sunStarted && !m_sunResolved, m_rs[RS_ZENABLE],
                 m_rs[RS_ZWRITEENABLE], m_rs[RS_ALPHABLENDENABLE], m_rs[RS_ALPHATESTENABLE],
                 m_rt == m_backBuffer ? "back" : "other", m_viewport.Width, m_viewport.Height, cam);
    }
    if(m_drawDiagnostics.First(m_vsHandle,m_psHandle,DrawDiagnostics::Path))
    Log("draw path: VS 0x%lx PS 0x%lx streams s0=%p/%u s1=%p/%u tex0=%u tex1=%u tex2=%u viewport=%ux%u",
            m_vsHandle, m_psHandle, (void*)m_streams[0].vb, m_streams[0].stride,
            (void*)m_streams[1].vb, m_streams[1].stride, TextureDim(m_textures[0]),
            TextureDim(m_textures[1]), TextureDim(m_textures[2]), m_viewport.Width, m_viewport.Height);
    detail.Next(Performance::CpuVertex);
    if (m_lastVS != vs->vs.Get()) { m_ctx->VSSetShader(vs->vs.Get(), nullptr, 0); m_lastVS = vs->vs.Get(); }
    if (m_lastLayout != vs->layout.Get()) { m_ctx->IASetInputLayout(vs->layout.Get()); m_lastLayout = vs->layout.Get(); }
    for (int s = 0; s < kMaxStreams; s++) {
        if (!(vs->parsed.streamMask & (1u << s))) continue;
        ID3D11Buffer* buf = m_streams[s].vb ? m_streams[s].vb->m_impl.m_buf.Get() : nullptr;
        UINT stride = m_streams[s].stride, offset = 0;
        if (m_lastVB[s] != buf || m_lastStride[s] != stride) {
            m_ctx->IASetVertexBuffers(s, 1, &buf, &stride, &offset);
            m_lastVB[s] = buf;
            m_lastStride[s] = stride;
        }
    }

    detail.Next(Performance::CpuConstants);
    // Constants
    const size_t vsBytes = (size_t)(m_vsConstUsed ? m_vsConstUsed : kMaxVSConst) * 16;
    const bool ring = m_vsRing && g_config.constantRing;
    if (ring != m_vsRingWasOn) { m_vsRingWasOn = ring; m_vsConstDirty = true; m_vsRingBound = false; } // switched: upload to the other buffer
    // Registers this draw's shader reads; the ring window holds m_vsRingValid of them.
    const size_t ringBytes = (std::min)(vsBytes, (size_t)vs->constRegs * 16);
    if (ring && ringBytes > m_vsRingValid) m_vsConstDirty = true;
    if (ring && m_vsConstDirty) {
        // The bound window is always the full 256 registers; registers past vsBytes hold other
        // draws' data, never read (as with the single buffer, where they are unwritten).
        D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (m_vsRingPos + sizeof(m_vsConst) > kVsRingBytes) { mode = D3D11_MAP_WRITE_DISCARD; m_vsRingPos = 0; }
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(m_ctx->Map(m_vsRing.Get(), 0, mode, 0, &map))) {
            memcpy((BYTE*)map.pData + m_vsRingPos, m_vsConst, ringBytes);
            m_ctx->Unmap(m_vsRing.Get(), 0);
            m_vsRingFirst = m_vsRingPos / 16;
            m_vsRingPos += (UINT)((ringBytes + 255) & ~(size_t)255);
            m_vsRingValid = ringBytes;
            m_perf.constantBytes += ringBytes;
            ++m_perf.vsUploads;
            m_vsRingBound = false;
        }
        m_vsConstDirty = false;
    }
    if (ring) {
        if (!m_vsRingBound) {
            const UINT num = kMaxVSConst;
            m_ctx1->VSSetConstantBuffers1(0, 1, m_vsRing.GetAddressOf(), &m_vsRingFirst, &num);
            m_vsRingBound = true;
            m_bind.vsCb[0] = m_vsRing.Get();
        }
    } else if (m_vsConstDirty) {
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(m_ctx->Map(m_vsCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            // Registers never written are not read by the game's shaders (zero in m_vsConst).
            memcpy(map.pData, m_vsConst, vsBytes);
            m_perf.constantBytes += vsBytes;
            m_ctx->Unmap(m_vsCB.Get(), 0);
            ++m_perf.vsUploads;
        }
        m_vsConstDirty = false;
    }
    if (m_psConstDirty) {
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(m_ctx->Map(m_psCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            memcpy(map.pData, &m_psConst, sizeof(m_psConst));
            m_ctx->Unmap(m_psCB.Get(), 0);
            ++m_perf.psUploads;
        }
        m_psConstDirty = false;
    }
    if (ring) BindVSCB(1, 1, m_vsExtraCB.GetAddressOf());
    else {
        ID3D11Buffer* vcbs[2] = { m_vsCB.Get(), m_vsExtraCB.Get() };
        BindVSCB(0, 2, vcbs);
    }
    BindPSCB(0, 1, m_psCB.GetAddressOf());

    detail.Next(Performance::CpuResources);
    // Textures and samplers
    ID3D11ShaderResourceView* srvs[kMaxStages] = {};
    ID3D11SamplerState* samplers[kMaxStages] = {};
    for (int s = 0; s < kMaxStages; s++) {
        ID8BaseTexture* t = m_textures[s];
        m_drawTexDim[s] = 0;
        if (!t) continue;
        const DWORD type = t->GetType();
        m_drawTexDim[s] = type == D8RTYPE_VOLUMETEXTURE ? 3 : (type == D8RTYPE_TEXTURE ? 2 : 0);
        if (type == D8RTYPE_TEXTURE) {
            Texture* tex = static_cast<Texture*>(static_cast<ID8Texture*>(t));
            if (m_rt && m_rt->m_container == tex) continue; // sampling the current render target
            srvs[s] = tex->Srv();
        } else if (type == D8RTYPE_VOLUMETEXTURE) {
            srvs[s] = static_cast<VolumeTexture*>(static_cast<ID8VolumeTexture*>(t))->Srv();
        }
        samplers[s] = GetSampler(s);
    }
    BindPSSrv(0, kMaxStages, srvs);
    ID3D11ShaderResourceView* extra[2] = { ActiveEmissive(), ActiveNormalMap() }; // t8, t9
    BindPSSrv(8, 2, extra);
    ID3D11ShaderResourceView* rough = ActiveRoughness();
    BindPSSrv(16, 1, &rough);
    if (g_config.effectsEnabled) {
        BindPointLights();
        if(!PrepareMaterial())return false;
        PrepareRelief();
        BindTerrainHeights();
    }
    BindPSSamplers(0, kMaxStages, samplers);

    detail.Next(Performance::CpuPipeline);
    m_psPending = false;
    ID3D11PixelShader* ps = GetPixelShader();
    if (!ps) {
        if (m_psPending) { ++m_perf.pendingShaderDraws; return false; }
        LogOnce(std::string("ps-null-") + std::to_string(m_psHandle) + "-vs-" + std::to_string(m_vsHandle),
                "draw rejected: no pixel shader for VS 0x%lx PS 0x%lx", m_vsHandle, m_psHandle);
        return false;
    }
    if (m_lastPS != ps) { m_ctx->PSSetShader(ps, nullptr, 0); m_lastPS = ps; }

    // Water already composites the opaque snapshot, so alpha blending would
    // count transmission twice. Other game materials retain their blend state.
    ID3D11BlendState* blend = m_waterDraw ? m_noBlend.Get() : GetBlendState();
    if (m_lastBlend != blend) { m_ctx->OMSetBlendState(blend, nullptr, 0xFFFFFFFF); m_lastBlend = blend; }
    ID3D11DepthStencilState* depth = GetDepthState();
    if (m_lastDepth != depth || m_lastStencilRef != m_rs[RS_STENCILREF]) {
        m_ctx->OMSetDepthStencilState(depth, m_rs[RS_STENCILREF]);
        m_lastDepth = depth;
        m_lastStencilRef = m_rs[RS_STENCILREF];
    }
    ID3D11RasterizerState* raster = GetRasterState();
    if (m_lastRaster != raster) { m_ctx->RSSetState(raster); m_lastRaster = raster; }
    m_vsWriteLo = kMaxVSConst; m_vsWriteHi = 0; // constants written before the next draw
    return true;
}

// ---- draws ---------------------------------------------------------------------------------------------

bool Device::DrawWithTopology(DWORD type, UINT primCount, UINT& count, D3D11_PRIMITIVE_TOPOLOGY& topo)
{
    switch (type) {
    case D8PT_POINTLIST: topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; count = primCount; break;
    case D8PT_LINELIST: topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; count = primCount * 2; break;
    case D8PT_LINESTRIP: topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; count = primCount + 1; break;
    case D8PT_TRIANGLELIST: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; count = primCount * 3; break;
    case D8PT_TRIANGLESTRIP: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; count = primCount + 2; break;
    case D8PT_TRIANGLEFAN: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; count = primCount * 3; break;
    default: return false;
    }
    return true;
}

void Device::UploadDynamicIndices(const void* data, UINT bytes, DXGI_FORMAT fmt)
{
    CpuPerformanceScope cpu(m_perf,Performance::CpuGeometryUpload);
    if (bytes > m_dynIBSize) {
        m_dynIB.Reset();
        m_dynIBSize = max(bytes, 65536u);
        D3D11_BUFFER_DESC bd = { m_dynIBSize, D3D11_USAGE_DYNAMIC, D3D11_BIND_INDEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        m_device->CreateBuffer(&bd, nullptr, &m_dynIB);
    }
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(m_ctx->Map(m_dynIB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, data, bytes);
        m_ctx->Unmap(m_dynIB.Get(), 0);
        ++m_perf.geometryUploads; m_perf.geometryUploadBytes+=bytes;
    }
    m_ctx->IASetIndexBuffer(m_dynIB.Get(), fmt, 0);
    m_lastIB = m_dynIB.Get();
}

void Device::UploadDynamicVertices(const void* data, UINT bytes, UINT stride)
{
    CpuPerformanceScope cpu(m_perf,Performance::CpuGeometryUpload);
    if (bytes > m_dynVBSize) {
        m_dynVB.Reset();
        m_dynVBSize = max(bytes, 262144u);
        D3D11_BUFFER_DESC bd = { m_dynVBSize, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        m_device->CreateBuffer(&bd, nullptr, &m_dynVB);
    }
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(m_ctx->Map(m_dynVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, data, bytes);
        m_ctx->Unmap(m_dynVB.Get(), 0);
        ++m_perf.geometryUploads; m_perf.geometryUploadBytes+=bytes;
    }
    UINT offset = 0;
    ID3D11Buffer* buf = m_dynVB.Get();
    m_ctx->IASetVertexBuffers(0, 1, &buf, &stride, &offset);
    m_lastVB[0] = buf;
    m_lastStride[0] = stride;
}

// Builds triangle-list indices for a strip, keeping its winding (odd triangles swap their first
// two vertices) and dropping the degenerate triangles strips use as joints.
static std::vector<UINT> StripToList(const std::vector<UINT>& strip)
{
    std::vector<UINT> out;
    out.reserve(strip.size() * 3);
    for (size_t i = 0; i + 2 < strip.size(); i++) {
        UINT a = strip[i], b = strip[i + 1], c = strip[i + 2];
        if (a == b || b == c || a == c) continue;
        if (i & 1) std::swap(a, b);
        out.push_back(a); out.push_back(b); out.push_back(c);
    }
    return out;
}

// Builds triangle-list indices for a fan: (first, i + 1, i + 2).
static std::vector<UINT> FanToList(const std::vector<UINT>& fan)
{
    std::vector<UINT> out;
    for (size_t i = 1; i + 1 < fan.size(); i++) { out.push_back(fan[0]); out.push_back(fan[i]); out.push_back(fan[i + 1]); }
    return out;
}

HRESULT Device::DrawPrimitive(DWORD type, UINT start, UINT primCount)
{
    m_drawFirstVertex=start;
    if (!primCount || !PrepareDraw()) return S_OK;
    UINT count;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    if (!DrawWithTopology(type, primCount, count, topo)) return D8ERR_INVALIDCALL;
    if (GrassCaptureActive()) CapturePusher(start, type == D8PT_TRIANGLEFAN ? primCount + 2 : count);
    CaptureEmitters(type, 0, start, primCount, false);
    const bool tess = BeginTessellation(type, topo);
    m_ctx->IASetPrimitiveTopology(topo);
    if (type == D8PT_TRIANGLEFAN) {
        std::vector<WORD> idx;
        idx.reserve(count);
        for (UINT i = 0; i < primCount; i++) { idx.push_back(0); idx.push_back((WORD)(i + 1)); idx.push_back((WORD)(i + 2)); }
        UploadDynamicIndices(idx.data(), (UINT)idx.size() * 2, DXGI_FORMAT_R16_UINT);
        m_ctx->DrawIndexed(count, 0, (INT)start);
        DrawSunShadow(true, count, 0, (INT)start);
        return S_OK;
    }
    if (tess) {
        // Patches are drawn from an index list (strips expanded, see DrawTessellated).
        std::vector<UINT> seq(count);
        for (UINT i = 0; i < count; i++) seq[i] = i;
        DrawTessellated(type == D8PT_TRIANGLESTRIP ? StripToList(seq) : seq, (INT)start);
        EndTessellation();
        if (type == D8PT_TRIANGLESTRIP) m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        DrawSunShadow(false, count, start, 0);
        return S_OK;
    }
    m_ctx->Draw(count, start);
    DrawSunShadow(false, count, start, 0);
    return S_OK;
}

HRESULT Device::DrawIndexedPrimitive(DWORD type, UINT minIndex, UINT numVertices, UINT startIndex, UINT primCount)
{
    m_drawFirstVertex=m_baseVertex+minIndex;
    if (m_terrainBatchOpen) {
        if (primCount && m_indices && AppendTerrainBatch(type, startIndex, primCount)) return S_OK;
        FlushTerrainBatch();
    }
    if (!primCount || !m_indices || !PrepareDraw()) return S_OK;
    if (m_trace && m_worldKind == 2) {
        // What changes from one terrain section to the next (for merging their draws).
        DWORD consts = 0;
        for (int r = 4; r < 96; r++) for (int k = 0; k < 4; k++) consts = consts * 31 + *(const DWORD*)&m_vsConst[r][k];
        TraceSun("  terrain draw: type %lu vb %p/%u ib %p base %u min %u verts %u start %u prims %u ps %lx tex %p %p %p %p consts %08lx",
                 type, (void*)m_streams[0].vb, m_streams[0].stride, (void*)m_indices, m_baseVertex, minIndex, numVertices,
                 startIndex, primCount, m_psHandle, (void*)m_textures[0], (void*)m_textures[1], (void*)m_textures[2],
                 (void*)m_textures[3], consts);
    }
    if (StartTerrainBatch(type, startIndex, primCount)) return S_OK;
    if (GrassCaptureActive()) CapturePusher(m_baseVertex + minIndex, numVertices);
    CaptureEmitters(type, m_baseVertex, startIndex, primCount, true);
    UINT count;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    if (!DrawWithTopology(type, primCount, count, topo)) return D8ERR_INVALIDCALL;
    const bool tess = BeginTessellation(type, topo);
    m_ctx->IASetPrimitiveTopology(topo);
    const DXGI_FORMAT fmt = m_indices->m_format == D8FMT_INDEX32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    if (type == D8PT_TRIANGLEFAN) {
        const UINT isz = fmt == DXGI_FORMAT_R32_UINT ? 4 : 2;
        std::vector<UINT> fan(primCount + 2);
        const BYTE* src = m_indices->m_impl.m_shadow.data() + (size_t)startIndex * isz;
        for (UINT i = 0; i < primCount + 2; i++) fan[i] = isz == 4 ? ((const UINT*)src)[i] : ((const WORD*)src)[i];
        std::vector<UINT> list = FanToList(fan);
        UploadDynamicIndices(list.data(), (UINT)list.size() * 4, DXGI_FORMAT_R32_UINT);
        m_ctx->DrawIndexed(count, 0, (INT)m_baseVertex);
        DrawSunShadow(true, count, 0, (INT)m_baseVertex);
        return S_OK;
    }
    ID3D11Buffer* ib = m_indices->m_impl.m_buf.Get();
    if (tess) {
        // Patches are drawn from an index list read from the CPU copy of the index buffer.
        const UINT isz = fmt == DXGI_FORMAT_R32_UINT ? 4 : 2;
        std::vector<UINT> idx(count);
        const BYTE* src = m_indices->m_impl.m_shadow.data() + (size_t)startIndex * isz;
        if (m_indices->m_impl.m_shadow.size() >= ((size_t)startIndex + idx.size()) * isz) {
            for (UINT i = 0; i < idx.size(); i++) idx[i] = isz == 4 ? ((const UINT*)src)[i] : ((const WORD*)src)[i];
            DrawTessellated(type == D8PT_TRIANGLESTRIP ? StripToList(idx) : idx, (INT)m_baseVertex);
        }
        EndTessellation();
        // The shadow redraw uses the game's own indices and topology.
        if (type == D8PT_TRIANGLESTRIP) m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        m_ctx->IASetIndexBuffer(ib, fmt, 0);
        m_lastIB = ib;
        m_lastIBFormat = fmt;
        DrawSunShadow(true, count, startIndex, (INT)m_baseVertex);
        return S_OK;
    }
    if (m_lastIB != ib || m_lastIBFormat != fmt) {
        m_ctx->IASetIndexBuffer(ib, fmt, 0);
        m_lastIB = ib;
        m_lastIBFormat = fmt;
    }
    m_ctx->DrawIndexed(count, startIndex, (INT)m_baseVertex);
    DrawSunShadow(true, count, startIndex, (INT)m_baseVertex);
    return S_OK;
}

HRESULT Device::DrawPrimitiveUP(DWORD type, UINT primCount, const void* data, UINT stride)
{
    FlushTerrainBatch();
    m_drawFirstVertex=0;
    if (!data || !primCount) return D8ERR_INVALIDCALL;
    UINT count;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    if (!DrawWithTopology(type, primCount, count, topo)) return D8ERR_INVALIDCALL;
    UINT vertices = type == D8PT_TRIANGLEFAN ? primCount + 2 : count;
    // D3D8 resets stream 0 after an UP draw.
    SetRef(m_streams[0].vb, (VertexBuffer*)nullptr);
    m_streams[0].stride = stride;
    if (!PrepareDraw()) return S_OK;
    UploadDynamicVertices(data, vertices * stride, stride);
    m_ctx->IASetPrimitiveTopology(topo);
    if (type == D8PT_TRIANGLEFAN) {
        std::vector<WORD> idx;
        for (UINT i = 0; i < primCount; i++) { idx.push_back(0); idx.push_back((WORD)(i + 1)); idx.push_back((WORD)(i + 2)); }
        UploadDynamicIndices(idx.data(), (UINT)idx.size() * 2, DXGI_FORMAT_R16_UINT);
        m_ctx->DrawIndexed(count, 0, 0);
    } else {
        m_ctx->Draw(count, 0);
    }
    m_lastVB[0] = nullptr;
    return S_OK;
}

HRESULT Device::DrawIndexedPrimitiveUP(DWORD type, UINT minIndex, UINT numVertices, UINT primCount,
                                       const void* indices, DWORD indexFmt, const void* data, UINT stride)
{
    FlushTerrainBatch();
    m_drawFirstVertex=0;
    if (!data || !indices || !primCount) return D8ERR_INVALIDCALL;
    UINT count;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    if (!DrawWithTopology(type, primCount, count, topo)) return D8ERR_INVALIDCALL;
    SetRef(m_streams[0].vb, (VertexBuffer*)nullptr);
    SetRef(m_indices, (IndexBuffer*)nullptr);
    m_streams[0].stride = stride;
    if (!PrepareDraw()) return S_OK;
    UploadDynamicVertices(data, (minIndex + numVertices) * stride, stride);
    m_ctx->IASetPrimitiveTopology(topo);
    const UINT isz = indexFmt == D8FMT_INDEX32 ? 4 : 2;
    const UINT idxCount = type == D8PT_TRIANGLEFAN ? primCount + 2 : count;
    if (type == D8PT_TRIANGLEFAN) {
        std::vector<UINT> fan(idxCount);
        for (UINT i = 0; i < idxCount; i++) fan[i] = isz == 4 ? ((const UINT*)indices)[i] : ((const WORD*)indices)[i];
        std::vector<UINT> list = FanToList(fan);
        UploadDynamicIndices(list.data(), (UINT)list.size() * 4, DXGI_FORMAT_R32_UINT);
    } else {
        UploadDynamicIndices(indices, idxCount * isz, isz == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT);
    }
    m_ctx->DrawIndexed(count, 0, 0);
    m_lastVB[0] = nullptr;
    m_lastIB = nullptr;
    return S_OK;
}

// ---- presentation ----------------------------------------------------------------------------------

void Device::UpdateSwapChainSize()
{
    RECT rc = {};
    GetClientRect(m_window, &rc);
    UINT w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (!w || !h || (w == m_swapW && h == m_swapH)) return;
    m_swapRTV.Reset();
    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    HRESULT hr = m_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, m_swapFlags);
    if (SUCCEEDED(hr)) { m_swapW = w; m_swapH = h; Log("swap chain resized to %ux%u", w, h); }
    m_rtDirty = true;
}

void Device::LimitFrameRate()
{
    const int limit = g_config.EffectiveFpsLimit();
    if (limit <= 0) return;
    const LONGLONG frameTicks = m_qpcFreq.QuadPart / limit;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG target = m_lastFrame.QuadPart + frameTicks;
    while (now.QuadPart < target) {
        LONGLONG remainingMs = (target - now.QuadPart) * 1000 / m_qpcFreq.QuadPart;
        if (remainingMs > 2) Sleep((DWORD)(remainingMs - 2)); else YieldProcessor();
        QueryPerformanceCounter(&now);
    }
    m_lastFrame.QuadPart = (now.QuadPart - target < frameTicks) ? target : now.QuadPart;
}

HRESULT Device::Present(const RECT*, const RECT*, HWND, const RGNDATA*)
{
    if (!m_csmt.OnWorker()) hitch_sampler::FramePresented(); // threaded: the game thread reports in csmt_device.cpp
    FlushTerrainBatch();
    m_perf.enabled=g_config.performanceGPU;
    m_perf.Begin(m_device.Get(),m_ctx.Get());
    m_perf.Tick((double)m_qpcFreq.QuadPart);
    m_perf.SampleMemory(m_device.Get());
    if (g_config.debugLayer) {
        ComPtr<ID3D11InfoQueue> queue;
        if (SUCCEEDED(m_device.As(&queue))) {
            const UINT64 count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
            for (UINT64 i = 0; i < count; ++i) {
                SIZE_T size = 0;
                queue->GetMessage(i, nullptr, &size);
                std::vector<BYTE> storage(size);
                auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
                if (SUCCEEDED(queue->GetMessage(i, message, &size))) {
                    LogOnce(std::string("d3d11-") + message->pDescription,
                            "D3D11 diagnostic %u severity %u: %s", message->ID,
                            message->Severity, message->pDescription);
                }
            }
            queue->ClearStoredMessages();
        }
    }
    EndSunFrame();
    UpdateSwapChainSize();
    if (!m_swapRTV) {
        ComPtr<ID3D11Texture2D> buf;
        if (FAILED(m_swap->GetBuffer(0, IID_PPV_ARGS(&buf)))) return D8ERR_INVALIDCALL;
        m_device->CreateRenderTargetView(buf.Get(), nullptr, &m_swapRTV);
    }
    if (m_gammaDirty) {
        WORD lut[256][4];
        for (int i = 0; i < 256; i++) { lut[i][0] = m_gamma.red[i]; lut[i][1] = m_gamma.green[i]; lut[i][2] = m_gamma.blue[i]; lut[i][3] = 0xFFFF; }
        m_ctx->UpdateSubresource(m_gammaTex.Get(), 0, nullptr, lut, sizeof(lut), sizeof(lut));
        m_gammaDirty = false;
    }
    HelperConstants hc = {};
    hc.misc[1] = (g_config.applyGamma && !m_gammaIdentity) ? 1.0f : 0.0f;
    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(m_ctx->Map(m_clearCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, &hc, sizeof(hc));
        m_ctx->Unmap(m_clearCB.Get(), 0);
    }
    ID3D11RenderTargetView* rtv = m_swapRTV.Get();
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = { 0, 0, (float)m_swapW, (float)m_swapH, 0, 1 };
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->RSSetState(m_noCull.Get());
    m_ctx->OMSetBlendState(m_noBlend.Get(), nullptr, 0xFFFFFFFF);
    m_ctx->OMSetDepthStencilState(m_noDepth.Get(), 0);
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_fullscreenVS.Get(), nullptr, 0);
    m_ctx->VSSetConstantBuffers(0, 1, m_clearCB.GetAddressOf());
    m_ctx->PSSetShader(m_presentPS.Get(), nullptr, 0);
    m_ctx->PSSetConstantBuffers(0, 1, m_clearCB.GetAddressOf());
    ID3D11ShaderResourceView* finalScene=PrepareFsr(m_backBuffer->ResolvedSrv());
    // The upscaler helper changes shader/targets/viewport/CB state. Restore presentation.
    m_ctx->OMSetRenderTargets(1,&rtv,nullptr);m_ctx->RSSetViewports(1,&vp);
    m_ctx->VSSetShader(m_fullscreenVS.Get(),nullptr,0);m_ctx->PSSetShader(m_presentPS.Get(),nullptr,0);
    m_ctx->VSSetConstantBuffers(0,1,m_clearCB.GetAddressOf());m_ctx->PSSetConstantBuffers(0,1,m_clearCB.GetAddressOf());
    ID3D11ShaderResourceView* srvs[2] = { finalScene, m_gammaSRV.Get() };
    m_ctx->PSSetShaderResources(0, 2, srvs);
    const bool sameSize = m_swapW == m_backBuffer->m_width && m_swapH == m_backBuffer->m_height;
    ID3D11SamplerState* smp[2] = { sameSize ? m_pointClamp.Get() : m_linearClamp.Get(), m_pointClamp.Get() };
    m_ctx->PSSetSamplers(0, 2, smp);
    m_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[2] = {};
    m_ctx->PSSetShaderResources(0, 2, nulls);
    PollAutomation(); // ini hot reload and file-requested captures, before the panel is drawn
    DrawOverlay();
    m_perf.Finish(m_ctx.Get());

    LARGE_INTEGER perfStart,perfEnd;QueryPerformanceCounter(&perfStart);
    if(PresentGeneratedFrame()){
        // The generated image was shown at the previous VBlank. Restore the
        // already-rendered real image and replay existing GUI draw data, without
        // running input/simulation or building a second ImGui frame.
        m_ctx->OMSetRenderTargets(1,&rtv,nullptr);m_ctx->RSSetViewports(1,&vp);
        m_ctx->RSSetState(m_noCull.Get());m_ctx->OMSetBlendState(m_noBlend.Get(),nullptr,~0u);
        m_ctx->OMSetDepthStencilState(m_noDepth.Get(),0);m_ctx->IASetInputLayout(nullptr);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->VSSetShader(m_fullscreenVS.Get(),nullptr,0);m_ctx->PSSetShader(m_presentPS.Get(),nullptr,0);
        m_ctx->VSSetConstantBuffers(0,1,m_clearCB.GetAddressOf());m_ctx->PSSetConstantBuffers(0,1,m_clearCB.GetAddressOf());
        m_ctx->PSSetShaderResources(0,2,srvs);m_ctx->PSSetSamplers(0,2,smp);m_ctx->Draw(3,0);
        m_ctx->PSSetShaderResources(0,2,nulls);ReplayOverlay();
    }
    const UINT sync = g_config.EffectiveVSync();
    HRESULT hr = m_swap->Present(sync, !sync && (m_swapFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? DXGI_PRESENT_ALLOW_TEARING : 0);
    QueryPerformanceCounter(&perfEnd);
    m_perf.presentMs=(perfEnd.QuadPart-perfStart.QuadPart)*1000.0/m_qpcFreq.QuadPart;
    if (FAILED(hr)) LogOnce("presentfail", "Present failed 0x%08lx", hr);
    LimitFrameRate();
    QueryPerformanceCounter(&perfStart);
    m_perf.limiterMs=(perfStart.QuadPart-perfEnd.QuadPart)*1000.0/m_qpcFreq.QuadPart;
    RecordPerformance();
    {
        // Spike log, render side: a frame of this thread over 15 ms (the game side is in csmt_device.cpp).
        static LARGE_INTEGER last = {};
        static UINT logged = 0;
        const double ms = last.QuadPart ? (perfStart.QuadPart - last.QuadPart) * 1000.0 / m_qpcFreq.QuadPart : 0;
        last = perfStart;
        if (ms >= 15 && ms < 200 && logged < 300) {
            logged++;
            Log("Render frame %.1f ms: present %.1f ms, limiter %.1f ms, %u draws, %u shadow draws", ms, m_perf.presentMs, m_perf.limiterMs,
                m_perfDraws, m_perfShadowDraws);
        }
    }
    m_perfDraws=m_perfWaterDraws=m_perfShadowDraws=0;
    m_tessDrawsShown=m_tessDraws; m_tessDraws=0;
    m_reliefDrawsShown=m_reliefDraws; m_reliefDraws=0;
    ApplyMsaa(); // after the frame is shown: the rebuilt back buffer starts empty
    m_frame++;
    BuildPointLights();
    m_pointLightsDirty = true;

    // Everything bound above has to be rebound by the next draw.
    InvalidateBindings();
    m_rtDirty = m_viewportDirty = true;
    m_vsConstDirty = m_psConstDirty = true;
    m_lastVS = nullptr; m_lastPS = nullptr; m_lastLayout = nullptr;
    m_lastBlend = nullptr; m_lastDepth = nullptr; m_lastRaster = nullptr;
    return S_OK;
}
