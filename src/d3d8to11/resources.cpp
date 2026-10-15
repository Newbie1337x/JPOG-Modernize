// resources.cpp - textures, surfaces, volumes and buffers.
#include "device.h"

// ---- DXT decompression (only for odd-sized DXT textures D3D11 cannot store as BC) -----------------

static void Rgb565(WORD c, BYTE out[3])
{
    out[0] = (BYTE)(((c >> 11) & 31) * 255 / 31);
    out[1] = (BYTE)(((c >> 5) & 63) * 255 / 63);
    out[2] = (BYTE)((c & 31) * 255 / 31);
}

static void DecodeColorBlock(const BYTE* b, BYTE rgba[16][4], bool dxt1)
{
    WORD c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    BYTE pal[4][4];
    Rgb565(c0, pal[0]); Rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    for (int i = 0; i < 3; i++) {
        if (!dxt1 || c0 > c1) {
            pal[2][i] = (BYTE)((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = (BYTE)((pal[0][i] + 2 * pal[1][i]) / 3);
        } else {
            pal[2][i] = (BYTE)((pal[0][i] + pal[1][i]) / 2);
            pal[3][i] = 0;
        }
    }
    pal[2][3] = 255;
    pal[3][3] = (dxt1 && c0 <= c1) ? 0 : 255;
    DWORD idx = b[4] | (b[5] << 8) | (b[6] << 16) | ((DWORD)b[7] << 24);
    for (int p = 0; p < 16; p++) memcpy(rgba[p], pal[(idx >> (p * 2)) & 3], 4);
}

static void DecompressDXT(DWORD fmt, const BYTE* src, UINT srcPitch, UINT w, UINT h, BYTE* dst, UINT dstPitch)
{
    const UINT blockBytes = fmt == D8FMT_DXT1 ? 8 : 16;
    for (UINT by = 0; by < (h + 3) / 4; by++) {
        for (UINT bx = 0; bx < (w + 3) / 4; bx++) {
            const BYTE* b = src + by * srcPitch + bx * blockBytes;
            BYTE px[16][4];
            if (fmt == D8FMT_DXT1) {
                DecodeColorBlock(b, px, true);
            } else {
                DecodeColorBlock(b + 8, px, false);
                if (fmt == D8FMT_DXT2 || fmt == D8FMT_DXT3) {
                    for (int p = 0; p < 16; p++) px[p][3] = (BYTE)(((b[p / 2] >> ((p & 1) * 4)) & 15) * 17);
                } else {
                    BYTE a[8] = { b[0], b[1] };
                    if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = (BYTE)(((7 - i) * a[0] + i * a[1]) / 7);
                    else { for (int i = 1; i < 5; i++) a[i + 1] = (BYTE)(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
                    for (int p = 0; p < 16; p++) px[p][3] = a[(bits >> (3 * p)) & 7];
                }
            }
            for (int p = 0; p < 16; p++) {
                UINT x = bx * 4 + (p & 3), y = by * 4 + (p >> 2);
                if (x >= w || y >= h) continue;
                BYTE* d = dst + y * dstPitch + x * 4; // B, G, R, A
                d[0] = px[p][2]; d[1] = px[p][1]; d[2] = px[p][0]; d[3] = px[p][3];
            }
        }
    }
}

// Writes a level of D3D8-format data into a D3D11 subresource (2D or one slice row-set for 3D).
static void UploadLevel(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub, DWORD fmt, const FormatInfo& info,
                        bool decompress, const BYTE* data, UINT w, UINT h, UINT depth)
{
    const UINT srcPitch = D8RowPitch(fmt, w), srcRows = D8RowCount(fmt, h);
    if (!info.convert && !decompress) {
        ctx->UpdateSubresource(res, sub, nullptr, data, srcPitch, srcPitch * srcRows);
        return;
    }
    const UINT dstPitch = w * 4;
    std::vector<BYTE> tmp((size_t)dstPitch * h * depth);
    for (UINT z = 0; z < depth; z++) {
        const BYTE* s = data + (size_t)z * srcPitch * srcRows;
        BYTE* d = tmp.data() + (size_t)z * dstPitch * h;
        if (decompress) DecompressDXT(fmt, s, srcPitch, w, h, d, dstPitch);
        else ConvertRows(fmt, s, srcPitch, d, dstPitch, w, h);
    }
    ctx->UpdateSubresource(res, sub, nullptr, tmp.data(), dstPitch, dstPitch * h);
}

// ---- Surface ----------------------------------------------------------------------------------------

Surface::Surface(Device* dev, Texture* container, UINT level, UINT w, UINT h, DWORD fmt, DWORD usage, DWORD pool)
    : m_dev(dev), m_container(container), m_level(level), m_width(w), m_height(h), m_format(fmt), m_usage(usage), m_pool(pool)
{
}

Surface::~Surface()
{
    if (m_holdsDeviceRef) m_dev->Release();
}

HRESULT Surface::QueryInterface(REFIID riid, void** out)
{
    if (!out) return E_POINTER;
    *out = this;
    AddRef();
    return S_OK;
}

ULONG Surface::AddRef()
{
    return m_container ? m_container->AddRef() : InterlockedIncrement(&m_ref);
}

ULONG Surface::Release()
{
    if (m_container) return m_container->Release();
    ULONG r = InterlockedDecrement(&m_ref);
    if (r == 0) {
        Surface* self = this;
        auto destroy = [self] { self->m_dev->OnSurfaceReleased(self); delete self; };
        if (m_dev->m_csmt.OffWorker()) m_dev->m_csmt.Post(destroy); else destroy();
    }
    return r;
}

HRESULT Surface::GetDevice(ID8Device** dev)
{
    if (!dev) return D8ERR_INVALIDCALL;
    *dev = m_dev->Facade();
    (*dev)->AddRef();
    return S_OK;
}

HRESULT Surface::GetContainer(REFIID, void** container)
{
    if (!container) return D8ERR_INVALIDCALL;
    if (m_container) { *container = m_container; m_container->AddRef(); return S_OK; }
    *container = m_dev;
    m_dev->AddRef();
    return S_OK;
}

HRESULT Surface::GetDesc(D3D8SurfaceDesc* d)
{
    if (!d) return D8ERR_INVALIDCALL;
    d->Format = m_format;
    d->Type = D8RTYPE_SURFACE;
    d->Usage = m_usage;
    d->Pool = m_pool;
    d->Size = D8RowPitch(m_format, m_width) * D8RowCount(m_format, m_height);
    d->MultiSampleType = 0;
    d->Width = m_width;
    d->Height = m_height;
    return S_OK;
}

bool Surface::CreateStandalone()
{
    FormatInfo info = GetFormatInfo(m_format);
    if (info.dxgi == DXGI_FORMAT_UNKNOWN) { Log("surface: unsupported format %lu", m_format); return false; }
    // Called again (with a new m_samples) when the MSAA level changes: the game keeps its
    // pointer to this Surface, only the D3D11 objects behind it are replaced.
    m_tex.Reset(); m_rtv.Reset(); m_dsv.Reset(); m_srv.Reset(); m_resolveTex.Reset(); m_resolveSrv.Reset();
    const bool ms = m_samples > 1;
    if (m_usage & D8USAGE_DEPTHSTENCIL) {
        // Typeless so the sun shadow resolve can read scene depth through m_srv.
        D3D11_TEXTURE2D_DESC td = { m_width, m_height, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, { m_samples, 0 },
                                    D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        if (FAILED(m_dev->D3D()->CreateTexture2D(&td, nullptr, &m_tex))) return false;
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = {};
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dd.ViewDimension = ms ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(m_dev->D3D()->CreateDepthStencilView(m_tex.Get(), &dd, &m_dsv))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        sd.ViewDimension = ms ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        m_dev->D3D()->CreateShaderResourceView(m_tex.Get(), &sd, &m_srv);
        return true;
    }
    if (m_usage & D8USAGE_RENDERTARGET) {
        D3D11_TEXTURE2D_DESC td = { m_width, m_height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { m_samples, 0 },
                                    D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        if (FAILED(m_dev->D3D()->CreateTexture2D(&td, nullptr, &m_tex))) return false;
        if (FAILED(m_dev->D3D()->CreateRenderTargetView(m_tex.Get(), nullptr, &m_rtv))) return false;
        if (ms) {
            // Single-sample copy used for presenting and read-back (ResolvedTexture).
            td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(m_dev->D3D()->CreateTexture2D(&td, nullptr, &m_resolveTex))) return false;
            return SUCCEEDED(m_dev->D3D()->CreateShaderResourceView(m_resolveTex.Get(), nullptr, &m_resolveSrv));
        }
        return SUCCEEDED(m_dev->D3D()->CreateShaderResourceView(m_tex.Get(), nullptr, &m_srv));
    }
    // Image surface: CPU memory only.
    m_lockData.assign((size_t)D8RowPitch(m_format, m_width) * D8RowCount(m_format, m_height), 0);
    return true;
}

ID3D11Texture2D* Surface::Resource()
{
    return m_container ? m_container->m_tex.Get() : m_tex.Get();
}

ID3D11Texture2D* Surface::ResolvedTexture()
{
    if (m_samples <= 1 || !m_resolveTex) return m_tex.Get();
    m_dev->Ctx()->ResolveSubresource(m_resolveTex.Get(), 0, m_tex.Get(), 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    return m_resolveTex.Get();
}

ID3D11ShaderResourceView* Surface::ResolvedSrv()
{
    if (m_samples <= 1) return m_srv.Get();
    ResolvedTexture();
    return m_resolveSrv.Get();
}

ID3D11RenderTargetView* Surface::Rtv()
{
    if (m_rtv) return m_rtv.Get();
    if (m_container && (m_container->m_usage & D8USAGE_RENDERTARGET)) {
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipSlice = m_level;
        m_dev->D3D()->CreateRenderTargetView(m_container->m_tex.Get(), &rd, &m_rtv);
    }
    return m_rtv.Get();
}

HRESULT Surface::LockRect(D3D8LockedRect* lr, const RECT* rect, DWORD flags)
{
    if (m_container) return m_container->LockRect(m_level, lr, rect, flags);
    if (!lr) return D8ERR_INVALIDCALL;
    if (m_usage & D8USAGE_DEPTHSTENCIL) return D8ERR_INVALIDCALL;
    const UINT pitch = D8RowPitch(m_format, m_width);
    HRESULT readHr = S_OK;
    if (m_tex) m_dev->m_csmt.Sync([&] {
        // Render target read-back (screenshots): copy through a staging texture.
        ID3D11Texture2D* source = ResolvedTexture();
        D3D11_TEXTURE2D_DESC td;
        source->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(m_dev->D3D()->CreateTexture2D(&td, nullptr, &staging))) { readHr = D8ERR_INVALIDCALL; return; }
        m_dev->Ctx()->CopyResource(staging.Get(), source);
        D3D11_MAPPED_SUBRESOURCE map;
        if (FAILED(m_dev->Ctx()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) { readHr = D8ERR_INVALIDCALL; return; }
        m_lockData.resize((size_t)pitch * m_height);
        const UINT bpp = GetFormatInfo(m_format).bytesPerPixel;
        for (UINT y = 0; y < m_height; y++) {
            const BYTE* s = (const BYTE*)map.pData + (size_t)y * map.RowPitch;
            BYTE* d = m_lockData.data() + (size_t)y * pitch;
            if (bpp == 4) memcpy(d, s, m_width * 4);
            else for (UINT x = 0; x < m_width; x++) { // to R5G6B5
                WORD v = (WORD)(((s[x * 4 + 2] >> 3) << 11) | ((s[x * 4 + 1] >> 2) << 5) | (s[x * 4] >> 3));
                memcpy(d + x * 2, &v, 2);
            }
        }
        m_dev->Ctx()->Unmap(staging.Get(), 0);
    });
    if (FAILED(readHr) || m_lockData.empty()) return D8ERR_INVALIDCALL;
    lr->Pitch = (INT)pitch;
    lr->pBits = m_lockData.data() + (rect ? (size_t)rect->top * pitch + rect->left * GetFormatInfo(m_format).bytesPerPixel : 0);
    return S_OK;
}

HRESULT Surface::UnlockRect()
{
    if (m_container) return m_container->UnlockRect(m_level);
    return S_OK;
}

// ---- Texture ------------------------------------------------------------------------------------

Texture::Texture(Device* dev, UINT w, UINT h, UINT levels, DWORD usage, DWORD fmt, DWORD pool)
    : m_dev(dev), m_width(w), m_height(h), m_levels(levels), m_usage(usage), m_format(fmt), m_pool(pool)
{
    dev->AddRef();
    UINT full = FullMipCount(w, h);
    if (m_levels == 0 || m_levels > full) m_levels = full;
    m_info = GetFormatInfo(fmt);
}

Texture::~Texture()
{
    for (Surface* s : m_surfaces) delete s;
    m_dev->Release();
}

bool Texture::Create()
{
    if (m_info.dxgi == DXGI_FORMAT_UNKNOWN || m_info.depth) { Log("CreateTexture: unsupported format %lu", m_format); return false; }
    bool decompress = m_info.blockBytes && ((m_width & 3) || (m_height & 3));
    DXGI_FORMAT fmt = decompress ? DXGI_FORMAT_B8G8R8A8_UNORM : m_info.dxgi;
    if (decompress) { m_info.dxgi = fmt; }
    D3D11_TEXTURE2D_DESC td = { m_width, m_height, m_levels, 1, fmt, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    if (m_usage & D8USAGE_RENDERTARGET) td.BindFlags |= D3D11_BIND_RENDER_TARGET;
    HRESULT hr = m_dev->D3D()->CreateTexture2D(&td, nullptr, &m_tex);
    if (FAILED(hr)) { Log("CreateTexture2D %ux%u fmt %lu failed 0x%08lx", m_width, m_height, m_format, hr); return false; }
    if (FAILED(m_dev->D3D()->CreateShaderResourceView(m_tex.Get(), nullptr, &m_srv))) return false;
    m_shadow.resize(m_levels);
    for (UINT l = 0; l < m_levels; l++)
        m_surfaces.push_back(new Surface(m_dev, this, l, MipSize(m_width, l), MipSize(m_height, l), m_format, m_usage, m_pool));
    return true;
}

HRESULT Texture::QueryInterface(REFIID, void** out)
{
    if (!out) return E_POINTER;
    *out = this;
    AddRef();
    return S_OK;
}

ULONG Texture::AddRef() { return InterlockedIncrement(&m_ref); }

ULONG Texture::Release()
{
    ULONG r = InterlockedDecrement(&m_ref);
    if (r == 0) {
        Texture* self = this;
        auto destroy = [self] { self->m_dev->OnTextureReleased(self); delete self; };
        if (m_dev->m_csmt.OffWorker()) m_dev->m_csmt.Post(destroy); else destroy();
    }
    return r;
}

HRESULT Texture::GetDevice(ID8Device** dev)
{
    if (!dev) return D8ERR_INVALIDCALL;
    *dev = m_dev->Facade();
    (*dev)->AddRef();
    return S_OK;
}

HRESULT Texture::GetLevelDesc(UINT level, D3D8SurfaceDesc* desc)
{
    if (level >= m_levels) return D8ERR_INVALIDCALL;
    return m_surfaces[level]->GetDesc(desc);
}

HRESULT Texture::GetSurfaceLevel(UINT level, ID8Surface** surf)
{
    if (level >= m_levels || !surf) return D8ERR_INVALIDCALL;
    *surf = m_surfaces[level];
    AddRef();
    return S_OK;
}

HRESULT Texture::LockRect(UINT level, D3D8LockedRect* lr, const RECT* rect, DWORD flags)
{
    if (level >= m_levels || !lr) return D8ERR_INVALIDCALL;
    const UINT w = MipSize(m_width, level), h = MipSize(m_height, level);
    const UINT pitch = D8RowPitch(m_format, w);
    std::vector<BYTE>& sh = m_shadow[level];
    if (sh.empty()) sh.assign((size_t)pitch * D8RowCount(m_format, h), 0);
    size_t offset = 0;
    if (rect) {
        if (m_info.blockBytes) offset = (size_t)(rect->top / 4) * pitch + (rect->left / 4) * m_info.blockBytes;
        else offset = (size_t)rect->top * pitch + rect->left * m_info.bytesPerPixel;
    }
    if (m_usage & D8USAGE_RENDERTARGET)
        LogOnce("lockrt", "LockRect on a render target texture (contents not read back)");
    lr->Pitch = (INT)pitch;
    lr->pBits = sh.data() + offset;
    return S_OK;
}

HRESULT Texture::UnlockRect(UINT level)
{
    if (level >= m_levels) return D8ERR_INVALIDCALL;
    m_dev->m_csmt.Sync([&] { Upload(level); });
    return S_OK;
}

void Texture::Upload(UINT level)
{
    if (m_shadow[level].empty()) return;
    if (level == 0 && !m_hashValid) {
        // Same FNV-1a as the plugin's texture index (offset basis xor size): the game uploads the
        // raw pixels of its .tml entries, so the hash names the texture whatever path created it.
        unsigned long long h = 1469598103934665603ull ^ (unsigned long long)m_shadow[0].size();
        for (BYTE b : m_shadow[0]) { h ^= b; h *= 1099511628211ull; }
        m_contentHash = h; m_hashValid = true;
        DecorateTexture(this);
    }
    const bool decompress = GetFormatInfo(m_format).blockBytes && m_info.dxgi == DXGI_FORMAT_B8G8R8A8_UNORM;
    m_dev->FlushBatches();
    m_dev->FlushShadowBatch();
    UploadLevel(m_dev->Ctx(), m_tex.Get(), level, m_format, m_info, decompress, m_shadow[level].data(),
                MipSize(m_width, level), MipSize(m_height, level), 1);
    // Preserve managed contents: later partial locks and read-only locks must
    // retain pixels outside the updated rectangle. Upload replaces the entire
    // mip, so dropping the shadow would turn unchanged pixels into zeroes.
}

// ---- VolumeTexture ---------------------------------------------------------------------------------

VolumeTexture::VolumeTexture(Device* dev, UINT w, UINT h, UINT d, UINT levels, DWORD usage, DWORD fmt, DWORD pool)
    : m_dev(dev), m_width(w), m_height(h), m_depth(d), m_levels(levels), m_usage(usage), m_format(fmt), m_pool(pool)
{
    dev->AddRef();
    UINT full = FullMipCount(w, h, d);
    if (m_levels == 0 || m_levels > full) m_levels = full;
    m_info = GetFormatInfo(fmt);
}

VolumeTexture::~VolumeTexture()
{
    for (Volume* v : m_volumes) delete v;
    m_dev->Release();
}

bool VolumeTexture::Create()
{
    if (m_info.dxgi == DXGI_FORMAT_UNKNOWN || m_info.depth) { Log("CreateVolumeTexture: unsupported format %lu", m_format); return false; }
    if (m_info.blockBytes) { m_info.dxgi = DXGI_FORMAT_B8G8R8A8_UNORM; } // decompressed per slice
    D3D11_TEXTURE3D_DESC td = { m_width, m_height, m_depth, m_levels, m_info.dxgi, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    HRESULT hr = m_dev->D3D()->CreateTexture3D(&td, nullptr, &m_tex);
    if (FAILED(hr)) { Log("CreateTexture3D failed 0x%08lx", hr); return false; }
    if (FAILED(m_dev->D3D()->CreateShaderResourceView(m_tex.Get(), nullptr, &m_srv))) return false;
    m_shadow.resize(m_levels);
    for (UINT l = 0; l < m_levels; l++) m_volumes.push_back(new Volume(this, l));
    Log("volume texture %ux%ux%u fmt %lu levels %u", m_width, m_height, m_depth, m_format, m_levels);
    return true;
}

HRESULT VolumeTexture::QueryInterface(REFIID, void** out)
{
    if (!out) return E_POINTER;
    *out = this;
    AddRef();
    return S_OK;
}

ULONG VolumeTexture::AddRef() { return InterlockedIncrement(&m_ref); }

ULONG VolumeTexture::Release()
{
    ULONG r = InterlockedDecrement(&m_ref);
    if (r == 0) {
        VolumeTexture* self = this;
        auto destroy = [self] { self->m_dev->OnTextureReleased(self); delete self; };
        if (m_dev->m_csmt.OffWorker()) m_dev->m_csmt.Post(destroy); else destroy();
    }
    return r;
}

HRESULT VolumeTexture::GetDevice(ID8Device** dev)
{
    if (!dev) return D8ERR_INVALIDCALL;
    *dev = m_dev->Facade();
    (*dev)->AddRef();
    return S_OK;
}

HRESULT VolumeTexture::GetLevelDesc(UINT level, D3D8VolumeDesc* d)
{
    if (level >= m_levels || !d) return D8ERR_INVALIDCALL;
    d->Format = m_format;
    d->Type = D8RTYPE_VOLUME;
    d->Usage = m_usage;
    d->Pool = m_pool;
    d->Width = MipSize(m_width, level);
    d->Height = MipSize(m_height, level);
    d->Depth = MipSize(m_depth, level);
    d->Size = D8RowPitch(m_format, d->Width) * D8RowCount(m_format, d->Height) * d->Depth;
    return S_OK;
}

HRESULT VolumeTexture::GetVolumeLevel(UINT level, ID8Volume** vol)
{
    if (level >= m_levels || !vol) return D8ERR_INVALIDCALL;
    *vol = m_volumes[level];
    AddRef();
    return S_OK;
}

HRESULT VolumeTexture::LockBox(UINT level, D3D8LockedBox* lb, const D3D8Box* box, DWORD)
{
    if (level >= m_levels || !lb) return D8ERR_INVALIDCALL;
    const UINT w = MipSize(m_width, level), h = MipSize(m_height, level), d = MipSize(m_depth, level);
    const UINT pitch = D8RowPitch(m_format, w), slice = pitch * D8RowCount(m_format, h);
    std::vector<BYTE>& sh = m_shadow[level];
    if (sh.empty()) sh.assign((size_t)slice * d, 0);
    size_t offset = 0;
    if (box) offset = (size_t)box->Front * slice + (size_t)box->Top * pitch + box->Left * m_info.bytesPerPixel;
    lb->RowPitch = (INT)pitch;
    lb->SlicePitch = (INT)slice;
    lb->pBits = sh.data() + offset;
    return S_OK;
}

HRESULT VolumeTexture::UnlockBox(UINT level)
{
    if (level >= m_levels) return D8ERR_INVALIDCALL;
    m_dev->m_csmt.Sync([&] { Upload(level); });
    return S_OK;
}

void VolumeTexture::Upload(UINT level)
{
    if (m_shadow[level].empty()) return;
    const bool decompress = GetFormatInfo(m_format).blockBytes != 0;
    m_dev->FlushBatches();
    m_dev->FlushShadowBatch();
    UploadLevel(m_dev->Ctx(), m_tex.Get(), level, m_format, m_info, decompress, m_shadow[level].data(),
                MipSize(m_width, level), MipSize(m_height, level), MipSize(m_depth, level));
}

HRESULT Volume::QueryInterface(REFIID, void** out)
{
    if (!out) return E_POINTER;
    *out = this;
    AddRef();
    return S_OK;
}

HRESULT Volume::GetContainer(REFIID, void** container)
{
    if (!container) return D8ERR_INVALIDCALL;
    *container = m_container;
    m_container->AddRef();
    return S_OK;
}

// ---- buffers ----------------------------------------------------------------------------------------

bool BufferImpl::Create(Device* dev, UINT size, DWORD usage, DWORD pool, UINT bind)
{
    static std::atomic<UINT64> next{1};
    m_identity = next.fetch_add(1);
    m_dev = dev;
    m_size = size ? size : 4;
    m_usage = usage;
    m_pool = pool;
    m_dynamic = (usage & D8USAGE_DYNAMIC) != 0;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (m_size + 15) & ~15u;
    bd.BindFlags = bind;
    bd.Usage = m_dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    bd.CPUAccessFlags = m_dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
    if (FAILED(dev->D3D()->CreateBuffer(&bd, nullptr, &m_buf))) { Log("CreateBuffer(%u) failed", m_size); return false; }
    m_shadow.assign(bd.ByteWidth, 0);
    return true;
}

HRESULT BufferImpl::Lock(UINT offset, UINT size, BYTE** data, DWORD flags)
{
    if (!data || offset > m_size) return D8ERR_INVALIDCALL;
    if (size == 0 || offset + size > m_size) size = m_size - offset;
    if (m_lockCount == 0) {
        m_lockOffset = offset;
        m_lockSize = size;
        m_lockFlags = flags;
    } else {
        UINT end = max(m_lockOffset + m_lockSize, offset + size);
        m_lockOffset = min(m_lockOffset, offset);
        m_lockSize = end - m_lockOffset;
        m_lockFlags &= flags;
    }
    m_lockCount++;
    *data = m_shadow.data() + offset;
    return S_OK;
}

HRESULT BufferImpl::Unlock()
{
    if (m_lockCount == 0) return D8ERR_INVALIDCALL;
    if (--m_lockCount) return S_OK;
    if (m_lockFlags & D8LOCK_READONLY) return S_OK;
    if (m_dev->m_csmt.OffWorker()) {
        // The render thread uploads a copy: the game may write the shadow again right away.
        // A discard rewrites the whole buffer, so it carries all of it.
        const bool whole = m_dynamic && !((m_lockFlags & D8LOCK_NOOVERWRITE) && m_queuedDiscard);
        if (whole) m_queuedDiscard = true;
        const UINT offset = whole ? 0 : m_lockOffset, size = whole ? m_size : m_lockSize;
        const DWORD flags = m_lockFlags;
        BufferImpl* self = this;
        m_dev->m_csmt.PostData(m_shadow.data() + offset, size, [self, offset, size, flags](const void* data) {
            self->UploadOnWorker(offset, size, flags, (const BYTE*)data);
        });
        return S_OK;
    }
    return UploadOnWorker(m_lockOffset, m_lockSize, m_lockFlags, m_shadow.data() + m_lockOffset);
}

HRESULT BufferImpl::UploadOnWorker(UINT lockOffset, UINT lockSize, DWORD lockFlags, const BYTE* data)
{
    // data holds bytes [lockOffset, lockOffset + lockSize) of the buffer.
    m_dev->FlushBatches(); // recorded terrain sections were drawn with the textures and buffers bound now
    m_dev->FlushShadowBatch(); // recorded shadow casters may read the old contents
    CpuPerformanceScope cpu(m_dev->m_perf,Performance::CpuGeometryUpload);
    ++m_revision;
    m_dev->InvalidateTessellation(m_identity);
    ID3D11DeviceContext* ctx = m_dev->Ctx();
    if (m_dynamic) {
        D3D11_MAPPED_SUBRESOURCE map;
        if ((lockFlags & D8LOCK_NOOVERWRITE) && m_everDiscarded) {
            if (SUCCEEDED(ctx->Map(m_buf.Get(), 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &map))) {
                memcpy((BYTE*)map.pData + lockOffset, data, lockSize);
                ctx->Unmap(m_buf.Get(), 0);
                ++m_dev->m_perf.geometryUploads; m_dev->m_perf.geometryUploadBytes+=lockSize;
            }
        } else if (SUCCEEDED(ctx->Map(m_buf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            // A discard needs the full contents: from the shadow, or the whole copy queued with it.
            const BYTE* full = lockOffset == 0 && lockSize == m_size ? data : m_shadow.data();
            memcpy(map.pData, full, m_size);
            ctx->Unmap(m_buf.Get(), 0);
            ++m_dev->m_perf.geometryUploads; m_dev->m_perf.geometryUploadBytes+=m_size;
            m_everDiscarded = true;
        }
    } else {
        D3D11_BOX box = { lockOffset, 0, 0, lockOffset + lockSize, 1, 1 };
        ctx->UpdateSubresource(m_buf.Get(), 0, &box, data, 0, 0);
        ++m_dev->m_perf.geometryUploads; m_dev->m_perf.geometryUploadBytes+=lockSize;
    }
    return S_OK;
}

BufferImpl::~BufferImpl()
{
    if (m_dev && m_identity) m_dev->InvalidateTessellation(m_identity);
}

HRESULT VertexBuffer::QueryInterface(REFIID, void** out)
{
    if (!out) return E_POINTER;
    *out = this;
    AddRef();
    return S_OK;
}

ULONG VertexBuffer::Release()
{
    ULONG r = InterlockedDecrement(&m_ref);
    if (r == 0) {
        VertexBuffer* self = this;
        Device* d = m_dev;
        auto destroy = [self, d] { delete self; d->Release(); };
        if (d->m_csmt.OffWorker()) d->m_csmt.Post(destroy); else destroy();
    }
    return r;
}

HRESULT VertexBuffer::GetDevice(ID8Device** dev)
{
    if (!dev) return D8ERR_INVALIDCALL;
    *dev = m_dev->Facade();
    (*dev)->AddRef();
    return S_OK;
}

HRESULT VertexBuffer::GetDesc(D3D8VertexBufferDesc* d)
{
    if (!d) return D8ERR_INVALIDCALL;
    d->Format = D8FMT_VERTEXDATA;
    d->Type = D8RTYPE_VERTEXBUFFER;
    d->Usage = m_impl.m_usage;
    d->Pool = m_impl.m_pool;
    d->Size = m_impl.m_size;
    d->FVF = m_fvf;
    return S_OK;
}

HRESULT IndexBuffer::QueryInterface(REFIID, void** out)
{
    if (!out) return E_POINTER;
    *out = this;
    AddRef();
    return S_OK;
}

ULONG IndexBuffer::Release()
{
    ULONG r = InterlockedDecrement(&m_ref);
    if (r == 0) {
        IndexBuffer* self = this;
        Device* d = m_dev;
        auto destroy = [self, d] { delete self; d->Release(); };
        if (d->m_csmt.OffWorker()) d->m_csmt.Post(destroy); else destroy();
    }
    return r;
}

HRESULT IndexBuffer::GetDevice(ID8Device** dev)
{
    if (!dev) return D8ERR_INVALIDCALL;
    *dev = m_dev->Facade();
    (*dev)->AddRef();
    return S_OK;
}

HRESULT IndexBuffer::GetDesc(D3D8IndexBufferDesc* d)
{
    if (!d) return D8ERR_INVALIDCALL;
    d->Format = m_format;
    d->Type = D8RTYPE_INDEXBUFFER;
    d->Usage = m_impl.m_usage;
    d->Pool = m_impl.m_pool;
    d->Size = m_impl.m_size;
    return S_OK;
}
