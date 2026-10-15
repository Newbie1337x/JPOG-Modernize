// csmt_device.cpp - the device the game holds when [Performance] Threaded=1.
//
// Calls that change state or draw are recorded (csmt.h) and run on the render thread by the real
// Device. The state the game reads back often (render and texture stage states, viewport, render
// targets, bound objects, shader handles) is mirrored here, so those reads answer at once; rare
// reads and calls that return something only the real Device knows (creation, shader handles,
// back buffer, Reset) wait for the render thread. Objects passed in a recorded call are kept
// alive (AddRef) until it runs.
#include "csmt.h"
#include "device.h"
#include <xmmintrin.h>

namespace {

template <class T> void Hold(T*& slot, T* value)
{
    if (slot == value) return;
    if (value) value->AddRef();
    if (slot) slot->Release();
    slot = value;
}

UINT VertexCount(DWORD type, UINT primCount)
{
    switch (type) {
    case 1: return primCount;          // point list
    case 2: return primCount * 2;      // line list
    case 3: return primCount + 1;      // line strip
    case 4: return primCount * 3;      // triangle list
    case 5: case 6: return primCount + 2; // strip, fan
    default: return 0;
    }
}

} // namespace

class CsmtDevice final : public ID8Device {
public:
    explicit CsmtDevice(Device* dev) : m_dev(dev)
    {
        // The real device is idle here: copy its state before the worker starts.
        memcpy(m_rs, dev->m_rs, sizeof(m_rs));
        memcpy(m_tss, dev->m_tss, sizeof(m_tss));
        m_viewport = dev->m_viewport;
        m_vsHandle = dev->m_vsHandle;
        m_psHandle = dev->m_psHandle;
        Hold(m_rt, dev->m_rt);
        Hold(m_ds, dev->m_ds);
        dev->m_facade = this;
        dev->m_gameThread = GetCurrentThreadId();
    }
    bool Start() { return m_dev->m_csmt.Start(); }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override
    {
        if (!out) return E_POINTER;
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = InterlockedDecrement(&m_ref);
        if (r) return r;
        for (auto& t : m_textures) Hold(t, (ID8BaseTexture*)nullptr);
        for (auto& s : m_streams) Hold(s.vb, (ID8VertexBuffer*)nullptr);
        Hold(m_indices, (ID8IndexBuffer*)nullptr);
        Hold(m_rt, (Surface*)nullptr);
        Hold(m_ds, (Surface*)nullptr);
        m_dev->m_csmt.Stop(); // runs everything still queued, including releases
        m_dev->m_facade = nullptr;
        Log("Threaded device released");
        m_dev->Release();
        delete this;
        return 0;
    }

    // Constant or thread-safe answers: straight to the real device.
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return S_OK; }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { return m_dev->GetAvailableTextureMem(); }
    HRESULT STDMETHODCALLTYPE ResourceManagerDiscardBytes(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDirect3D(ID8Direct3D** out) override { return m_dev->GetDirect3D(out); }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(void* caps) override { return m_dev->GetDeviceCaps(caps); }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3D8DisplayMode* mode) override { return m_dev->GetDisplayMode(mode); }
    HRESULT STDMETHODCALLTYPE GetCreationParameters(void* p) override { return m_dev->GetCreationParameters(p); }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT x, UINT y, ID8Surface* s) override { return m_dev->SetCursorProperties(x, y, s); }
    void STDMETHODCALLTYPE SetCursorPosition(int x, int y, DWORD f) override { m_dev->SetCursorPosition(x, y, f); }
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL show) override { return m_dev->ShowCursor(show); }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3D8PresentParameters* pp, void** out) override { return m_dev->CreateAdditionalSwapChain(pp, out); }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3D8RasterStatus* st) override { return m_dev->GetRasterStatus(st); }
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT e, UINT l, DWORD u, DWORD f, DWORD p, void** out) override { return m_dev->CreateCubeTexture(e, l, u, f, p, out); }
    HRESULT STDMETHODCALLTYPE UpdateTexture(ID8BaseTexture* s, ID8BaseTexture* d) override { return m_dev->UpdateTexture(s, d); }
    HRESULT STDMETHODCALLTYPE GetFrontBuffer(ID8Surface* s) override { return m_dev->GetFrontBuffer(s); }
    HRESULT STDMETHODCALLTYPE BeginScene() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EndScene() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD i, const float* p) override { return m_dev->SetClipPlane(i, p); }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD i, float* p) override { return m_dev->GetClipPlane(i, p); }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3D8ClipStatus* s) override { return m_dev->SetClipStatus(s); }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3D8ClipStatus* s) override { return m_dev->GetClipStatus(s); }
    HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(DWORD t, DWORD* token) override { return m_dev->CreateStateBlock(t, token); }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* passes) override { return m_dev->ValidateDevice(passes); }
    HRESULT STDMETHODCALLTYPE GetInfo(DWORD id, void* data, DWORD size) override { return m_dev->GetInfo(id, data, size); }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT p, const PALETTEENTRY* e) override { return m_dev->SetPaletteEntries(p, e); }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT p, PALETTEENTRY* e) override { return m_dev->GetPaletteEntries(p, e); }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT p) override { return m_dev->SetCurrentTexturePalette(p); }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* p) override { return m_dev->GetCurrentTexturePalette(p); }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT a, UINT b, UINT c, ID8VertexBuffer* d, DWORD e) override { return m_dev->ProcessVertices(a, b, c, d, e); }
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT h, const float* s, const void* i) override { return m_dev->DrawRectPatch(h, s, i); }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT h, const float* s, const void* i) override { return m_dev->DrawTriPatch(h, s, i); }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT h) override { return m_dev->DeletePatch(h); }

    // Calls that wait for the render thread.
    HRESULT STDMETHODCALLTYPE Reset(D3D8PresentParameters* pp) override
    {
        HRESULT hr = S_OK;
        Sync([&] {
            hr = m_dev->Reset(pp);
            m_viewport = m_dev->m_viewport;
            Hold(m_rt, m_dev->m_rt);
            Hold(m_ds, m_dev->m_ds);
        });
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT i, DWORD t, ID8Surface** out) override { return SyncCall([&] { return m_dev->GetBackBuffer(i, t, out); }); }
    void STDMETHODCALLTYPE GetGammaRamp(D3D8GammaRamp* r) override { Sync([&] { m_dev->GetGammaRamp(r); }); }
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT l, DWORD u, DWORD f, DWORD p, ID8Texture** out) override
    { return SyncCall([&] { return m_dev->CreateTexture(w, h, l, u, f, p, out); }); }
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w, UINT h, UINT d, UINT l, DWORD u, DWORD f, DWORD p, ID8VolumeTexture** out) override
    { return SyncCall([&] { return m_dev->CreateVolumeTexture(w, h, d, l, u, f, p, out); }); }
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD u, DWORD fvf, DWORD p, ID8VertexBuffer** out) override
    { return SyncCall([&] { return m_dev->CreateVertexBuffer(len, u, fvf, p, out); }); }
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD u, DWORD f, DWORD p, ID8IndexBuffer** out) override
    { return SyncCall([&] { return m_dev->CreateIndexBuffer(len, u, f, p, out); }); }
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w, UINT h, DWORD f, DWORD ms, BOOL lock, ID8Surface** out) override
    { return SyncCall([&] { return m_dev->CreateRenderTarget(w, h, f, ms, lock, out); }); }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w, UINT h, DWORD f, DWORD ms, ID8Surface** out) override
    { return SyncCall([&] { return m_dev->CreateDepthStencilSurface(w, h, f, ms, out); }); }
    HRESULT STDMETHODCALLTYPE CreateImageSurface(UINT w, UINT h, DWORD f, ID8Surface** out) override
    { return SyncCall([&] { return m_dev->CreateImageSurface(w, h, f, out); }); }
    HRESULT STDMETHODCALLTYPE CopyRects(ID8Surface* s, const RECT* r, UINT n, ID8Surface* d, const POINT* p) override
    { return SyncCall([&] { return m_dev->CopyRects(s, r, n, d, p); }); }
    HRESULT STDMETHODCALLTYPE GetTransform(DWORD s, D3D8Matrix* m) override { return SyncCall([&] { return m_dev->GetTransform(s, m); }); }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3D8Material* m) override { return SyncCall([&] { return m_dev->GetMaterial(m); }); }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD i, D3D8Light* l) override { return SyncCall([&] { return m_dev->GetLight(i, l); }); }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD i, BOOL* e) override { return SyncCall([&] { return m_dev->GetLightEnable(i, e); }); }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* decl, const DWORD* func, DWORD* handle, DWORD u) override
    { return SyncCall([&] { return m_dev->CreateVertexShader(decl, func, handle, u); }); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstant(DWORD r, void* d, DWORD n) override { return SyncCall([&] { return m_dev->GetVertexShaderConstant(r, d, n); }); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderDeclaration(DWORD h, void* d, DWORD* s) override { return SyncCall([&] { return m_dev->GetVertexShaderDeclaration(h, d, s); }); }
    HRESULT STDMETHODCALLTYPE GetVertexShaderFunction(DWORD h, void* d, DWORD* s) override { return SyncCall([&] { return m_dev->GetVertexShaderFunction(h, d, s); }); }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* func, DWORD* handle) override { return SyncCall([&] { return m_dev->CreatePixelShader(func, handle); }); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstant(DWORD r, void* d, DWORD n) override { return SyncCall([&] { return m_dev->GetPixelShaderConstant(r, d, n); }); }
    HRESULT STDMETHODCALLTYPE GetPixelShaderFunction(DWORD h, void* d, DWORD* s) override { return SyncCall([&] { return m_dev->GetPixelShaderFunction(h, d, s); }); }

    // Present: recorded; the game goes on with the next frame while this one is translated.
    HRESULT STDMETHODCALLTYPE Present(const RECT* src, const RECT* dst, HWND wnd, const RGNDATA*) override
    {
        struct Args { RECT src, dst; bool hasSrc, hasDst; } a = {};
        if (src) { a.src = *src; a.hasSrc = true; }
        if (dst) { a.dst = *dst; a.hasDst = true; }
        Device* d = m_dev;
        CommandQueue* q = &m_dev->m_csmt;
        q->Post([d, q, a, wnd] {
            d->Present(a.hasSrc ? &a.src : nullptr, a.hasDst ? &a.dst : nullptr, wnd, nullptr);
            q->FrameDone();
        });
        q->FramePosted();
        LogGameSpike(q);
        return S_OK;
    }

    void STDMETHODCALLTYPE SetGammaRamp(DWORD flags, const D3D8GammaRamp* ramp) override
    {
        if (!ramp) return;
        Device* d = m_dev;
        Queue().PostData(ramp, sizeof(*ramp), [d, flags](const void* p) { d->SetGammaRamp(flags, (const D3D8GammaRamp*)p); });
    }

    // Render targets (mirrored: SetRenderTarget also resets the viewport to the target).
    HRESULT STDMETHODCALLTYPE SetRenderTarget(ID8Surface* rt, ID8Surface* ds) override
    {
        if (rt) Hold(m_rt, static_cast<Surface*>(rt));
        Hold(m_ds, static_cast<Surface*>(ds));
        if (m_rt) m_viewport = { 0, 0, m_rt->m_width, m_rt->m_height, 0.0f, 1.0f };
        if (rt) rt->AddRef();
        if (ds) ds->AddRef();
        Device* d = m_dev;
        Queue().Post([d, rt, ds] {
            d->SetRenderTarget(rt, ds);
            if (rt) rt->Release();
            if (ds) ds->Release();
        });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(ID8Surface** out) override
    {
        if (!out || !m_rt) return D8ERR_INVALIDCALL;
        *out = m_rt;
        m_rt->AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(ID8Surface** out) override
    {
        if (!out) return D8ERR_INVALIDCALL;
        *out = m_ds;
        if (!m_ds) return D8ERR_NOTAVAILABLE;
        m_ds->AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3D8Rect* rects, DWORD flags, DWORD color, float z, DWORD stencil) override
    {
        Device* d = m_dev;
        const size_t bytes = rects ? count * sizeof(D3D8Rect) : 0;
        Queue().PostData(rects, bytes, [d, count, flags, color, z, stencil, bytes](const void* p) {
            d->Clear(count, bytes ? (const D3D8Rect*)p : nullptr, flags, color, z, stencil);
        });
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetTransform(DWORD state, const D3D8Matrix* m) override
    {
        if (!m || state >= 512) return D8ERR_INVALIDCALL;
        Device* d = m_dev;
        Queue().PostData(m, sizeof(*m), [d, state](const void* p) { d->SetTransform(state, (const D3D8Matrix*)p); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(DWORD state, const D3D8Matrix* m) override
    {
        if (!m || state >= 512) return D8ERR_INVALIDCALL;
        Device* d = m_dev;
        Queue().PostData(m, sizeof(*m), [d, state](const void* p) { d->MultiplyTransform(state, (const D3D8Matrix*)p); });
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetViewport(const D3D8Viewport* vp) override
    {
        if (!vp) return D8ERR_INVALIDCALL;
        if (m_recording) return S_OK;
        m_viewport = *vp;
        Device* d = m_dev;
        const D3D8Viewport v = *vp;
        Queue().Post([d, v] { d->SetViewport(&v); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetViewport(D3D8Viewport* vp) override
    {
        if (!vp) return D8ERR_INVALIDCALL;
        *vp = m_viewport;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetMaterial(const D3D8Material* m) override
    {
        if (!m) return S_OK;
        Device* d = m_dev;
        Queue().PostData(m, sizeof(*m), [d](const void* p) { d->SetMaterial((const D3D8Material*)p); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3D8Light* l) override
    {
        if (!l) return D8ERR_INVALIDCALL;
        Device* d = m_dev;
        Queue().PostData(l, sizeof(*l), [d, index](const void* p) { d->SetLight(index, (const D3D8Light*)p); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD index, BOOL enable) override
    {
        Device* d = m_dev;
        Queue().Post([d, index, enable] { d->LightEnable(index, enable); });
        return S_OK;
    }

    // States: mirrored; a value equal to the current one is not recorded (the device ignores it).
    HRESULT STDMETHODCALLTYPE SetRenderState(DWORD state, DWORD value) override
    {
        if (state >= RS_COUNT) return D8ERR_INVALIDCALL;
        if (m_recording || m_rs[state] == value) return S_OK;
        m_rs[state] = value;
        Device* d = m_dev;
        Queue().Post([d, state, value] { d->SetRenderState(state, value); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRenderState(DWORD state, DWORD* value) override
    {
        if (state >= RS_COUNT || !value) return D8ERR_INVALIDCALL;
        *value = m_rs[state];
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage, DWORD type, DWORD value) override
    {
        if (stage >= kMaxStages || type >= TSS_COUNT) return D8ERR_INVALIDCALL;
        if (m_recording || m_tss[stage][type] == value) return S_OK;
        m_tss[stage][type] = value;
        Device* d = m_dev;
        Queue().Post([d, stage, type, value] { d->SetTextureStageState(stage, type, value); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage, DWORD type, DWORD* value) override
    {
        if (stage >= kMaxStages || type >= TSS_COUNT || !value) return D8ERR_INVALIDCALL;
        *value = m_tss[stage][type];
        return S_OK;
    }

    // State blocks: the device records nothing (sets are ignored while recording).
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override
    {
        ++m_recording;
        Device* d = m_dev;
        Queue().Post([d] { d->BeginStateBlock(); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD* token) override
    {
        if (m_recording) --m_recording;
        if (token) *token = 1;
        Device* d = m_dev;
        Queue().Post([d] { d->EndStateBlock(nullptr); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD token) override
    {
        Device* d = m_dev;
        Queue().Post([d, token] { d->ApplyStateBlock(token); });
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, ID8BaseTexture* tex) override
    {
        if (stage >= kMaxStages) return D8ERR_INVALIDCALL;
        if (m_recording || m_textures[stage] == tex) return S_OK;
        Hold(m_textures[stage], tex);
        if (tex) tex->AddRef();
        Device* d = m_dev;
        Queue().Post([d, stage, tex] { d->SetTexture(stage, tex); if (tex) tex->Release(); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, ID8BaseTexture** out) override
    {
        if (stage >= kMaxStages || !out) return D8ERR_INVALIDCALL;
        *out = m_textures[stage];
        if (*out) (*out)->AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DrawPrimitive(DWORD type, UINT start, UINT count) override
    {
        Device* d = m_dev;
        Queue().Post([d, type, start, count] { d->DrawPrimitive(type, start, count); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(DWORD type, UINT minIndex, UINT numVertices, UINT startIndex, UINT primCount) override
    {
        Device* d = m_dev;
        Queue().Post([d, type, minIndex, numVertices, startIndex, primCount] {
            d->DrawIndexedPrimitive(type, minIndex, numVertices, startIndex, primCount);
        });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(DWORD type, UINT primCount, const void* data, UINT stride) override
    {
        if (!data) return D8ERR_INVALIDCALL;
        const size_t bytes = (size_t)VertexCount(type, primCount) * stride;
        Device* d = m_dev;
        Queue().PostData(data, bytes, [d, type, primCount, stride](const void* p) { d->DrawPrimitiveUP(type, primCount, p, stride); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(DWORD type, UINT minIndex, UINT numVertices, UINT primCount,
                                                     const void* indices, DWORD indexFmt, const void* data, UINT stride) override
    {
        if (!data || !indices) return D8ERR_INVALIDCALL;
        const size_t vbytes = (size_t)(minIndex + numVertices) * stride;
        const size_t ibytes = (size_t)VertexCount(type, primCount) * (indexFmt == D8FMT_INDEX32 ? 4 : 2);
        std::vector<BYTE> both(vbytes + ibytes);
        memcpy(both.data(), data, vbytes);
        memcpy(both.data() + vbytes, indices, ibytes);
        Device* d = m_dev;
        Queue().PostData(both.data(), both.size(), [d, type, minIndex, numVertices, primCount, indexFmt, stride, vbytes](const void* p) {
            d->DrawIndexedPrimitiveUP(type, minIndex, numVertices, primCount, (const BYTE*)p + vbytes, indexFmt, p, stride);
        });
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetVertexShader(DWORD handle) override
    {
        if (m_recording) return S_OK;
        m_vsHandle = handle; // always recorded: setting it again reloads its declaration constants
        Device* d = m_dev;
        Queue().Post([d, handle] { d->SetVertexShader(handle); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetVertexShader(DWORD* handle) override { if (handle) *handle = m_vsHandle; return S_OK; }
    HRESULT STDMETHODCALLTYPE DeleteVertexShader(DWORD handle) override
    {
        Device* d = m_dev;
        Queue().Post([d, handle] { d->DeleteVertexShader(handle); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstant(DWORD reg, const void* data, DWORD count) override
    {
        if (!data || reg + count > kMaxVSConst) return D8ERR_INVALIDCALL;
        if (!count) return S_OK;
        // The skin shader uploads its bones three registers per call: adjacent writes are merged
        // into one command, sent before the next command of any other kind (Queue()).
        if (m_vscHi > m_vscLo && (reg > m_vscHi || reg + count < m_vscLo)) FlushConstants();
        const float* src = (const float*)data;
        for (DWORD i = 0; i < count; i++) _mm_storeu_ps(m_vsc[reg + i], _mm_loadu_ps(src + i * 4)); // memmove call costs more for 3 registers
        if (m_vscHi > m_vscLo) {
            m_vscLo = min(m_vscLo, reg);
            m_vscHi = max(m_vscHi, reg + count);
        } else {
            m_vscLo = reg;
            m_vscHi = reg + count;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream, ID8VertexBuffer* vb, UINT stride) override
    {
        if (stream >= kMaxStreams) return D8ERR_INVALIDCALL;
        if (m_recording) return S_OK;
        if (m_streams[stream].vb == vb && m_streams[stream].stride == stride) return S_OK;
        Hold(m_streams[stream].vb, vb);
        m_streams[stream].stride = stride;
        if (vb) vb->AddRef();
        Device* d = m_dev;
        Queue().Post([d, stream, vb, stride] { d->SetStreamSource(stream, vb, stride); if (vb) vb->Release(); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT stream, ID8VertexBuffer** vb, UINT* stride) override
    {
        if (stream >= kMaxStreams || !vb || !stride) return D8ERR_INVALIDCALL;
        *vb = m_streams[stream].vb;
        if (*vb) (*vb)->AddRef();
        *stride = m_streams[stream].stride;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetIndices(ID8IndexBuffer* ib, UINT baseVertex) override
    {
        if (m_recording) return S_OK;
        if (m_indices == ib && m_baseVertex == baseVertex) return S_OK;
        Hold(m_indices, ib);
        m_baseVertex = baseVertex;
        if (ib) ib->AddRef();
        Device* d = m_dev;
        Queue().Post([d, ib, baseVertex] { d->SetIndices(ib, baseVertex); if (ib) ib->Release(); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetIndices(ID8IndexBuffer** ib, UINT* baseVertex) override
    {
        if (!ib || !baseVertex) return D8ERR_INVALIDCALL;
        *ib = m_indices;
        if (*ib) (*ib)->AddRef();
        *baseVertex = m_baseVertex;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetPixelShader(DWORD handle) override
    {
        if (m_recording || m_psHandle == handle) return S_OK;
        m_psHandle = handle;
        Device* d = m_dev;
        Queue().Post([d, handle] { d->SetPixelShader(handle); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelShader(DWORD* handle) override { if (handle) *handle = m_psHandle; return S_OK; }
    HRESULT STDMETHODCALLTYPE DeletePixelShader(DWORD handle) override
    {
        Device* d = m_dev;
        Queue().Post([d, handle] { d->DeletePixelShader(handle); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstant(DWORD reg, const void* data, DWORD count) override
    {
        if (!data || reg + count > kMaxPSConst) return D8ERR_INVALIDCALL;
        Device* d = m_dev;
        Queue().PostData(data, count * 16, [d, reg, count](const void* p) { d->SetPixelShaderConstant(reg, p, count); });
        return S_OK;
    }

private:
    // A game-thread frame (Present to Present) over 15 ms: where it stalled on the render thread.
    LARGE_INTEGER m_lastPresent = {};
    UINT m_spikesLogged = 0;
    void LogGameSpike(CommandQueue* q)
    {
        LARGE_INTEGER now, f;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&f);
        const double ms = m_lastPresent.QuadPart ? (now.QuadPart - m_lastPresent.QuadPart) * 1000.0 / f.QuadPart : 0;
        m_lastPresent = now;
        CommandQueue::Stalls s = q->m_stalls;
        q->m_stalls = {};
        if (ms < 15 || ms > 200 || m_spikesLogged >= 300) return;
        m_spikesLogged++;
        std::string where;
        for (int i = 0; i < 6 && s.where[i]; i++) {
            HMODULE mod = nullptr;
            char name[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)s.where[i], &mod))
                GetModuleFileNameA(mod, name, MAX_PATH);
            const char* base = strrchr(name, '\\');
            char item[MAX_PATH + 64];
            sprintf_s(item, " %s+0x%x x%u %.1fms;", base ? base + 1 : name, (unsigned)((uintptr_t)s.where[i] - (uintptr_t)mod),
                      s.whereCount[i], s.whereTicks[i] * 1000.0 / f.QuadPart);
            where += item;
        }
        Log("Game frame %.1f ms: %u syncs %.1f ms, waited %.1f ms for the render thread;%s", ms, s.syncs,
            s.syncTicks * 1000.0 / f.QuadPart, s.frameWaitTicks * 1000.0 / f.QuadPart, where.c_str());
    }
    CommandQueue& Queue() { if (m_vscHi > m_vscLo) FlushConstants(); return m_dev->m_csmt; }
    void FlushConstants()
    {
        const UINT reg = m_vscLo, count = m_vscHi - m_vscLo;
        m_vscLo = m_vscHi = 0;
        Device* d = m_dev;
        m_dev->m_csmt.PostData(m_vsc[reg], count * 16, [d, reg, count](const void* p) { d->SetVertexShaderConstant(reg, p, count); });
    }
    template<class F> void Sync(F&& f) { Queue().Sync(std::forward<F>(f)); }
    template<class F> HRESULT SyncCall(F&& f) { HRESULT hr = S_OK; Queue().Sync([&] { hr = f(); }); return hr; }

    LONG m_ref = 1;
    Device* m_dev;
    int m_recording = 0;
    DWORD m_rs[RS_COUNT];
    DWORD m_tss[kMaxStages][TSS_COUNT];
    D3D8Viewport m_viewport = {};
    DWORD m_vsHandle = 0, m_psHandle = 0;
    Surface* m_rt = nullptr;
    Surface* m_ds = nullptr;
    ID8BaseTexture* m_textures[kMaxStages] = {};
    struct Stream { ID8VertexBuffer* vb = nullptr; UINT stride = 0; } m_streams[kMaxStreams];
    ID8IndexBuffer* m_indices = nullptr;
    UINT m_baseVertex = 0;
    float m_vsc[kMaxVSConst][4];       // pending vertex constants; only [m_vscLo, m_vscHi) is valid
    UINT m_vscLo = 0, m_vscHi = 0;
};

ID8Device* CreateThreadedDevice(Device* dev)
{
    CsmtDevice* front = new CsmtDevice(dev);
    if (!front->Start()) {
        Log("Threaded device: the render thread could not start; running on the game thread");
        dev->m_facade = nullptr;
        delete front; // holds references only; the real device keeps its own
        return nullptr;
    }
    Log("Threaded device: Direct3D 8 calls run on render thread %lu", dev->m_csmt.WorkerThreadId());
    return front;
}
