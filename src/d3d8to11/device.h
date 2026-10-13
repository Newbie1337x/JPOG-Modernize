// device.h - IDirect3DDevice8 implemented on D3D11.
#pragma once
#include "objects.h"
#include "shaders.h"

struct VertexShader8 {
    std::vector<DWORD> decl, func;
    VSDecl parsed;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11InputLayout> layout;
    bool failed = false;
};

struct PixelShader8 {
    std::vector<DWORD> func;
    std::vector<std::pair<PSVariant, ComPtr<ID3D11PixelShader>>> variants;
};

class Device : public ID8Device {
public:
    Device(Direct3D8* d3d, HWND focus, DWORD flags, D3D8PresentParameters* pp);
    ~Device();
    bool Init();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override;

    // IDirect3DDevice8
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return S_OK; }
    UINT STDMETHODCALLTYPE GetAvailableTextureMem() override { return 0x7FF00000; }
    HRESULT STDMETHODCALLTYPE ResourceManagerDiscardBytes(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDirect3D(ID8Direct3D** out) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(void* caps) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3D8DisplayMode* mode) override;
    HRESULT STDMETHODCALLTYPE GetCreationParameters(void* params) override;
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT, UINT, ID8Surface*) override { return S_OK; }
    void STDMETHODCALLTYPE SetCursorPosition(int, int, DWORD) override {}
    BOOL STDMETHODCALLTYPE ShowCursor(BOOL show) override { BOOL o = m_cursorShown; m_cursorShown = show; return o; }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3D8PresentParameters*, void**) override { return D8ERR_NOTAVAILABLE; }
    HRESULT STDMETHODCALLTYPE Reset(D3D8PresentParameters* pp) override;
    HRESULT STDMETHODCALLTYPE Present(const RECT*, const RECT*, HWND, const RGNDATA*) override;
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT index, DWORD type, ID8Surface** out) override;
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3D8RasterStatus* st) override;
    void STDMETHODCALLTYPE SetGammaRamp(DWORD flags, const D3D8GammaRamp* ramp) override;
    void STDMETHODCALLTYPE GetGammaRamp(D3D8GammaRamp* ramp) override { if (ramp) *ramp = m_gamma; }
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, DWORD fmt, DWORD pool, ID8Texture** out) override;
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, DWORD fmt, DWORD pool, ID8VolumeTexture** out) override;
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT, UINT, DWORD, DWORD, DWORD, void**) override;
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, DWORD pool, ID8VertexBuffer** out) override;
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD usage, DWORD fmt, DWORD pool, ID8IndexBuffer** out) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w, UINT h, DWORD fmt, DWORD ms, BOOL lockable, ID8Surface** out) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w, UINT h, DWORD fmt, DWORD ms, ID8Surface** out) override;
    HRESULT STDMETHODCALLTYPE CreateImageSurface(UINT w, UINT h, DWORD fmt, ID8Surface** out) override;
    HRESULT STDMETHODCALLTYPE CopyRects(ID8Surface* src, const RECT* rects, UINT count, ID8Surface* dst, const POINT* pts) override;
    HRESULT STDMETHODCALLTYPE UpdateTexture(ID8BaseTexture* src, ID8BaseTexture* dst) override;
    HRESULT STDMETHODCALLTYPE GetFrontBuffer(ID8Surface*) override { return D8ERR_NOTAVAILABLE; }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(ID8Surface* rt, ID8Surface* ds) override;
    HRESULT STDMETHODCALLTYPE GetRenderTarget(ID8Surface** out) override;
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(ID8Surface** out) override;
    HRESULT STDMETHODCALLTYPE BeginScene() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EndScene() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3D8Rect* rects, DWORD flags, DWORD color, float z, DWORD stencil) override;
    HRESULT STDMETHODCALLTYPE SetTransform(DWORD state, const D3D8Matrix* m) override;
    HRESULT STDMETHODCALLTYPE GetTransform(DWORD state, D3D8Matrix* m) override;
    HRESULT STDMETHODCALLTYPE MultiplyTransform(DWORD state, const D3D8Matrix* m) override;
    HRESULT STDMETHODCALLTYPE SetViewport(const D3D8Viewport* vp) override;
    HRESULT STDMETHODCALLTYPE GetViewport(D3D8Viewport* vp) override { if (!vp) return D8ERR_INVALIDCALL; *vp = m_viewport; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3D8Material* m) override { if (m) m_material = *m; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3D8Material* m) override { if (m) *m = m_material; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3D8Light* l) override;
    HRESULT STDMETHODCALLTYPE GetLight(DWORD index, D3D8Light* l) override;
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD index, BOOL enable) override;
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD index, BOOL* enable) override;
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD, const float*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD, float* plane) override { if (plane) memset(plane, 0, 16); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetRenderState(DWORD state, DWORD value) override;
    HRESULT STDMETHODCALLTYPE GetRenderState(DWORD state, DWORD* value) override;
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override;
    HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD* token) override;
    HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD token) override;
    HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(DWORD, DWORD* token) override;
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3D8ClipStatus*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3D8ClipStatus* st) override { if (st) memset(st, 0, sizeof(*st)); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD stage, ID8BaseTexture** out) override;
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, ID8BaseTexture* tex) override;
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD stage, DWORD type, DWORD* value) override;
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage, DWORD type, DWORD value) override;
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* passes) override { if (passes) *passes = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetInfo(DWORD, void*, DWORD) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT, const PALETTEENTRY*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT, PALETTEENTRY*) override { return D8ERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* pal) override { if (pal) *pal = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(DWORD type, UINT start, UINT count) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(DWORD type, UINT minIndex, UINT numVertices, UINT startIndex, UINT primCount) override;
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(DWORD type, UINT primCount, const void* data, UINT stride) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(DWORD type, UINT minIndex, UINT numVertices, UINT primCount,
                                                     const void* indices, DWORD indexFmt, const void* data, UINT stride) override;
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT, UINT, UINT, ID8VertexBuffer*, DWORD) override { return D8ERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD* decl, const DWORD* func, DWORD* handle, DWORD usage) override;
    HRESULT STDMETHODCALLTYPE SetVertexShader(DWORD handle) override;
    HRESULT STDMETHODCALLTYPE GetVertexShader(DWORD* handle) override { if (handle) *handle = m_vsHandle; return S_OK; }
    HRESULT STDMETHODCALLTYPE DeleteVertexShader(DWORD handle) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstant(DWORD reg, const void* data, DWORD count) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstant(DWORD reg, void* data, DWORD count) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderDeclaration(DWORD handle, void* data, DWORD* size) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderFunction(DWORD handle, void* data, DWORD* size) override;
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT stream, ID8VertexBuffer* vb, UINT stride) override;
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT stream, ID8VertexBuffer** vb, UINT* stride) override;
    HRESULT STDMETHODCALLTYPE SetIndices(ID8IndexBuffer* ib, UINT baseVertex) override;
    HRESULT STDMETHODCALLTYPE GetIndices(ID8IndexBuffer** ib, UINT* baseVertex) override;
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD* func, DWORD* handle) override;
    HRESULT STDMETHODCALLTYPE SetPixelShader(DWORD handle) override;
    HRESULT STDMETHODCALLTYPE GetPixelShader(DWORD* handle) override { if (handle) *handle = m_psHandle; return S_OK; }
    HRESULT STDMETHODCALLTYPE DeletePixelShader(DWORD handle) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstant(DWORD reg, const void* data, DWORD count) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstant(DWORD reg, void* data, DWORD count) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderFunction(DWORD handle, void* data, DWORD* size) override;
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT, const float*, const void*) override { return D8ERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT, const float*, const void*) override { return D8ERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT) override { return S_OK; }

    // Internals used by resources
    ID3D11Device* D3D() { return m_device.Get(); }
    ID3D11DeviceContext* Ctx() { return m_ctx.Get(); }
    void OnTextureReleased(ID8BaseTexture* tex);
    void OnSurfaceReleased(Surface* s);

private:
    bool CreateSwapChainAndBackBuffer();
    bool CreatePresentResources();
    void UpdateSwapChainSize();
    bool PrepareDraw();
    ID3D11BlendState* GetBlendState();
    ID3D11DepthStencilState* GetDepthState();
    ID3D11RasterizerState* GetRasterState();
    ID3D11SamplerState* GetSampler(int stage);
    ID3D11PixelShader* GetPixelShader();
    bool BuildVertexShader(VertexShader8& vs, DWORD handle);
    void ApplyRenderTargets();
    void ClearWithQuad(DWORD count, const D3D8Rect* rects, DWORD flags, DWORD color, float z);
    bool DrawWithTopology(DWORD type, UINT primCount, UINT& indexCount, D3D11_PRIMITIVE_TOPOLOGY& topo);
    void UploadDynamicIndices(const void* data, UINT bytes, DXGI_FORMAT fmt);
    void UploadDynamicVertices(const void* data, UINT bytes, UINT stride);
    void LimitFrameRate();
    bool CreateSunResources();
    void UpdateSun(bool shadowPass);
    void DrawSunShadow(bool indexed, UINT count, UINT start, INT base);

    LONG m_ref = 1;
    Direct3D8* m_d3d;
    HWND m_focus, m_window;
    DWORD m_flags;
    D3D8PresentParameters m_pp;

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<ID3D11DeviceContext1> m_ctx1;
    ComPtr<IDXGISwapChain1> m_swap;
    UINT m_swapW = 0, m_swapH = 0;

    Surface* m_backBuffer = nullptr;
    Surface* m_autoDepth = nullptr;
    Surface* m_rt = nullptr;
    Surface* m_ds = nullptr;
    bool m_rtDirty = true;

    // D3D8 state
    DWORD m_rs[RS_COUNT];
    DWORD m_tss[kMaxStages][TSS_COUNT];
    ID8BaseTexture* m_textures[kMaxStages] = {};
    struct Stream { VertexBuffer* vb = nullptr; UINT stride = 0; } m_streams[kMaxStreams];
    IndexBuffer* m_indices = nullptr;
    UINT m_baseVertex = 0;
    DWORD m_vsHandle = 0, m_psHandle = 0;
    float m_vsConst[kMaxVSConst][4];
    PSConstants m_psConst = {};
    D3D8Viewport m_viewport = {};
    D3D8Matrix m_transforms[512];
    D3D8Material m_material = {};
    std::unordered_map<DWORD, D3D8Light> m_lights;
    std::unordered_map<DWORD, BOOL> m_lightEnabled;
    D3D8GammaRamp m_gamma;
    bool m_gammaDirty = true, m_gammaIdentity = true;
    BOOL m_cursorShown = FALSE;
    int m_recordingStateBlock = 0;

    bool m_vsConstDirty = true, m_psConstDirty = true, m_viewportDirty = true;

    // Shaders
    std::vector<VertexShader8*> m_vertexShaders; // handle = (index << 1) | 1
    std::vector<PixelShader8*> m_pixelShaders;   // handle = index + 1

    // D3D11 object caches
    std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>> m_blendStates;
    std::unordered_map<uint64_t, ComPtr<ID3D11DepthStencilState>> m_depthStates;
    std::unordered_map<uint64_t, ComPtr<ID3D11RasterizerState>> m_rasterStates;
    std::unordered_map<std::string, ComPtr<ID3D11SamplerState>> m_samplers;
    std::unordered_map<std::string, ComPtr<ID3D11PixelShader>> m_ffShaders;
    ComPtr<ID3D11Buffer> m_vsCB, m_vsExtraCB, m_psCB;
    ComPtr<ID3D11Buffer> m_dynVB, m_dynIB;
    UINT m_dynVBSize = 0, m_dynIBSize = 0;
    ComPtr<ID3D11Buffer> m_fanIB;   // index pattern for triangle fans (0, i, i+1)
    UINT m_fanIBTriangles = 0;

    // Present / clear helpers
    ComPtr<ID3D11VertexShader> m_fullscreenVS;
    ComPtr<ID3D11PixelShader> m_presentPS, m_clearPS;
    ComPtr<ID3D11Buffer> m_clearCB;
    ComPtr<ID3D11Texture1D> m_gammaTex;
    ComPtr<ID3D11ShaderResourceView> m_gammaSRV;
    ComPtr<ID3D11SamplerState> m_linearClamp, m_pointClamp;
    ComPtr<ID3D11BlendState> m_noBlend;
    ComPtr<ID3D11DepthStencilState> m_noDepth;
    ComPtr<ID3D11RasterizerState> m_noCull, m_noCullScissor;

    ComPtr<ID3D11RenderTargetView> m_swapRTV;

    // Last bound D3D11 objects, to skip redundant calls. Reset whenever helper passes rebind state.
    ID3D11VertexShader* m_lastVS = nullptr;
    ID3D11PixelShader* m_lastPS = nullptr;
    ID3D11InputLayout* m_lastLayout = nullptr;
    ID3D11BlendState* m_lastBlend = nullptr;
    ID3D11DepthStencilState* m_lastDepth = nullptr;
    ID3D11RasterizerState* m_lastRaster = nullptr;
    DWORD m_lastStencilRef = 0;
    ID3D11Buffer* m_lastVB[kMaxStreams] = {};
    UINT m_lastStride[kMaxStreams] = {};
    ID3D11Buffer* m_lastIB = nullptr;
    DXGI_FORMAT m_lastIBFormat = DXGI_FORMAT_UNKNOWN;

    LARGE_INTEGER m_qpcFreq = {}, m_lastFrame = {};
    UINT64 m_frame = 0;
    // Sun shadows (sun.cpp). Casters are drawn into m_sunMap as the frame is rendered; once the
    // world is finished, a full-screen pass darkens the back buffer using the scene depth.
    ComPtr<ID3D11Buffer> m_sunCB, m_resolveCB;
    ComPtr<ID3D11Texture2D> m_sunMap;
    ComPtr<ID3D11DepthStencilView> m_sunDSV;
    ComPtr<ID3D11ShaderResourceView> m_sunSRV;
    ComPtr<ID3D11SamplerState> m_sunSampler;
    ComPtr<ID3D11DepthStencilState> m_sunDepth;
    ComPtr<ID3D11RasterizerState> m_sunRaster;
    ComPtr<ID3D11VertexShader> m_resolveVS;
    ComPtr<ID3D11PixelShader> m_resolvePS, m_resolvePSMS, m_fxPS, m_fxPSMS;
    ComPtr<ID3D11Texture2D> m_fxTex;
    ComPtr<ID3D11RenderTargetView> m_fxRTV;
    ComPtr<ID3D11ShaderResourceView> m_fxSRV;
    UINT m_fxW = 0, m_fxH = 0;
    bool EnsureFxTarget();
    ComPtr<ID3D11Texture2D> m_sceneCopy;
    ComPtr<ID3D11ShaderResourceView> m_sceneCopySRV;
    UINT m_sceneCopyW = 0, m_sceneCopyH = 0;
    bool EnsureSceneCopy();
    void CopySceneColor();
    ComPtr<ID3D11BlendState> m_multiplyBlend;
    SunConstants m_sun = {};
    SunConstants m_sunUploaded = {};
    bool m_sunUploadedValid = false;
    int m_worldKind = 0;
    bool m_sunStarted = false, m_sunResolved = false, m_worldDraw = false;
    bool m_worldCaster = false;
    UINT m_segmentCasters = 0, m_shadowDraws = 0, m_earlyCasters = 0, m_lateWorldDraws = 0;
    float m_sunDepthRange = 1.0f, m_sunTexelWorld = 0.1f;
    float m_gameSun[3] = {};
    bool m_gameSunValid = false;
    // One-frame trace of the world/HUD draw order (d3d8to11_frame.log), written once,
    // 1000 frames after the first shadowed frame.
    bool m_trace = false;
    std::string m_traceBuf;
    UINT64 m_traceAt = 0;
    bool m_traceDone = false;
    std::string m_traceLast;
    UINT m_traceRepeat = 0;
    void TraceSun(const char* fmt, ...);
    void DumpSunFrame();
    UINT64 m_dumpAt = 0;
    bool m_dumped = false;
    void TraceFlush();
    bool EnsureShadowMap();
    // Scene effects need the camera from the terrain draw (StartSunFrame).
    bool SceneFx() const { return g_config.sunLighting || g_config.postEnabled || g_config.grassPhysics || g_config.aoEnabled; }
    void CapturePusher(UINT firstVertex, UINT numVertices);
    // Stage-0 normal map for this draw (world draws only, not the shadow pass).
    ID3D11ShaderResourceView* ActiveNormalMap() const {
        if (!g_config.normalMaps || !m_worldDraw || m_sun.flags[1] > 0.5f || !m_textures[0] ||
            m_textures[0]->GetType() != D8RTYPE_TEXTURE) return nullptr;
        return static_cast<Texture*>(static_cast<ID8Texture*>(m_textures[0]))->m_normalSrv.Get();
    }
    bool GrassCaptureActive() const {
        return g_config.grassPhysics && m_worldDraw && m_worldKind == 1 && m_sunStarted && !m_sunResolved;
    }
    float m_pushBoxes[32][4] = {};  // world x/z footprint per pusher: min x, min z, max x, max z
    void UploadGrass();
    ComPtr<ID3D11Buffer> m_grassCB;
    float m_pushers[32][4] = {};
    UINT m_pusherCount = 0, m_lastBones = 0;
    bool m_grassUploaded = false;
    UINT m_vsWriteLo = 0, m_vsWriteHi = 0;
    UINT SupportedSamples(int requested);
    void ApplyMsaa();
    UINT m_msaaApplied = 1;
    // Alpha-tested draws to the multisampled back buffer use alpha-to-coverage instead of a hard cut.
    bool AlphaToCoverageActive() const {
        return g_config.alphaToCoverage && m_rt && m_rt->m_samples > 1 && m_rs[RS_ALPHATESTENABLE] &&
               (m_rs[RS_ALPHAFUNC] == 5 || m_rs[RS_ALPHAFUNC] == 7) && m_sun.flags[1] < 0.5f;
    }
    void DrawOverlay();     // overlay.cpp
    void ShutdownOverlay();
    UINT m_lastCasters = 0;
    UINT m_sunMapSize = 0;
    void StartSunFrame();
    void CaptureGameSun();
    void ResolveSun(const char* reason);
    void EndSunFrame();
};
