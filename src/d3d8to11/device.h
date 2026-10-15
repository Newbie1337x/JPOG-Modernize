// device.h - IDirect3DDevice8 implemented on D3D11.
#pragma once
#include "water_interaction.h"
#include "objects.h"
#include "shaders.h"
#include "performance.h"
#include "draw_diagnostics.h"
#include "descriptor_reuse.h"
#include "water_regions.h"
#include "csmt.h"
#include "shader_jobs.h"
#include <map>
#include <tuple>

struct VertexShader8 {
    std::vector<DWORD> decl, func;
    VSDecl parsed;
    int worldKind=0; // immutable declaration classification, computed at creation
    UINT constRegs=256; // c0..constRegs-1 can be read (VSConstantRegisters)
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11InputLayout> layout;
    bool failed = false;
    ComPtr<ID3D11VertexShader> pulled; // batched terrain variant (terrain_batch.cpp), built on first use
    int pulledState = 0;               // 0 not tried, 1 ready, -1 not batchable
    ComPtr<ID3D11VertexShader> instanced; // instanced skinned variant (skin_batch.cpp), built on first use
    int instancedState = 0;
};

struct PixelShader8 {
    std::vector<DWORD> func;
    bool samplesStage1=false;
    bool water=false; // immutable bytecode classification, computed at creation
    UINT64 hash = 0; // shader_jobs::FunctionHash of func
    struct Variant {
        PSVariant key;
        ComPtr<ID3D11PixelShader> shader;
        std::shared_ptr<shader_jobs::Job> job; // compiling in the background (AsyncShaders)
    };
    std::vector<Variant> variants;
    PSVariant lastVariant; ID3D11PixelShader* lastShader = nullptr; // most recent lookup
};

class Device;
// [Performance] Threaded: the device the game gets, recording calls for a render thread
// (csmt_device.cpp). nullptr when the thread cannot start.
ID8Device* CreateThreadedDevice(Device* dev);

class Device : public ID8Device {
    friend struct TessCacheKeyTest;
    friend class CsmtDevice;
    friend class BufferImpl; // geometry upload profiling, no ownership change
public:
    Device(Direct3D8* d3d, HWND focus, DWORD flags, D3D8PresentParameters* pp);
    ~Device();
    bool Init();
    void InvalidateTessellation(UINT64 identity);

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

    // Render thread (csmt.h): with [Performance] Threaded the game holds m_facade, which records
    // its calls; everything that touches the D3D11 context runs on the queue's worker.
    CommandQueue m_csmt;
    ID8Device* m_facade = nullptr;
    DWORD m_gameThread = 0; // the thread the game records on (threaded device)
    ID8Device* Facade() { return m_facade ? m_facade : this; }

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
    void DrawSunShadow(bool indexed, UINT count, UINT start, INT base, UINT instances = 1, const UINT* instanceInfo = nullptr,
                       const float* instanceConsts = nullptr);
    bool CreateWaterResources();
    void PrepareWaterDraw();
    void UpdateWaterRegions();
    bool WaterSurfaceHeight(float& height) const;
    bool EnsureWaterSnapshot();

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
    UINT m_drawFirstVertex=0;
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
    DescriptorReuse<DWORD,ID3D11SamplerState*> m_samplerSelections[kMaxStages];
    struct FFShader { ComPtr<ID3D11PixelShader> shader; std::shared_ptr<shader_jobs::Job> job; };
    std::unordered_map<std::string, FFShader> m_ffShaders;
    bool m_psPending = false; // GetPixelShader returned null because the variant is still compiling
    ID3D11PixelShader* ShaderOf(ComPtr<ID3D11PixelShader>& shader, std::shared_ptr<shader_jobs::Job>& job);
    void PrewarmFF();
    ComPtr<ID3D11Buffer> m_vsCB, m_vsExtraCB, m_psCB;
    UINT m_swapFlags = 0; // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING when supported
    UINT m_samplerDirty = ~0u;            // bit per stage: a sampler texture-stage state changed
    int m_samplerAniso[kMaxStages] = {};  // effective anisotropy the stage's sampler was chosen with
    BYTE m_drawTexDim[kMaxStages] = {};   // this draw's texture kinds (PrepareDraw), 0 none, 2 2D, 3 volume
    // Vertex constants in a ring (D3D11.1 no-overwrite maps, bound by offset): each draw writes
    // a fresh 256-byte-aligned window instead of renaming one buffer per draw.
    ComPtr<ID3D11Buffer> m_vsRing;
    static const UINT kVsRingBytes = 32u << 20; // several frames: a wrap (discard) hands out fresh memory
    UINT m_vsRingPos = kVsRingBytes, m_vsRingFirst = 0;
    bool m_vsRingBound = false, m_vsRingWasOn = false;
    // Last pipeline-state keys: an unchanged key skips the cache lookup.
    uint64_t m_blendKey = ~0ull, m_depthKey = ~0ull, m_rasterKey = ~0ull;
    ID3D11BlendState* m_blendKeyState = nullptr;
    ID3D11DepthStencilState* m_depthKeyState = nullptr;
    ID3D11RasterizerState* m_rasterKeyState = nullptr;
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
    bool EnsureTemporal();
    struct Fsr31State;
    Fsr31State* m_fsr31=nullptr;
    bool m_fsr31Failed=false;
    UINT64 m_fsr31Frame=~UINT64(0);
    bool EnsureFsr31();
    bool ResolveFsr31();
    void ReleaseFsr31();
    void Fsr31Jitter(float& x,float& y) const;
    int ActiveMaterial() const;
    int MaterialTextureStage() const;
    bool PrepareMaterial();
    void PrepareRelief();           // relief.cpp: decides v.relief and binds cbuffer b4
    bool m_reliefActive = false;
    // terrain_light.cpp: native height field for terrain normals and horizon shadows
    // automation.cpp: ini hot reload and file-requested frame captures
    void PollAutomation();
    // d3d8to11_texdump.req: the named textures of one frame's world draws go to d3d8to11_textures/
    void CollectDumpTextures();
    void WriteTextureDump();
    bool m_texDumpActive = false;
    UINT64 m_texDumpFrame = 0;
    std::vector<std::pair<Texture*, int>> m_texDumpList;
    bool CaptureSwapChain(const std::string& path);
    FILETIME m_iniTime = {};
    bool m_iniTimeValid = false;
    void UpdateTerrainHeights();
    void EvaluateTimeOfDay();
    bool ProceduralSkyActive() const { return g_config.replaceLighting && g_config.proceduralSky; }
    struct TimeOfDay { bool active = false; float dir[3] = {}, light[3] = {}, ambient[3] = {}, sky[3] = {}, fog[3] = {}, surface[3] = {}, sunDir[3] = {}; float sunLight = 1, procSky = 0, moon = 0, rim = 0, exposure = 1, dayW = 1, sheen = 0, shadowStrength = 0, shadowTint[3] = {}; } m_tod;
    void BindTerrainHeights();
    bool TerrainHeightsActive() const { return m_terrainReady && m_terrainHeightSRV && g_config.heightfieldLighting; }
    ComPtr<ID3D11Texture2D> m_terrainHeightTex;
    ComPtr<ID3D11ShaderResourceView> m_terrainHeightSRV;
    ComPtr<ID3D11SamplerState> m_terrainSampler;
    std::vector<float> m_terrainHeights;
    std::vector<float> m_terrainScan;          // rescan chunk, compared with m_terrainHeights
    unsigned m_terrainDirtyLo = ~0u, m_terrainDirtyHi = 0; // rows changed by the current rescan
    void* m_terrainField = nullptr;
    unsigned m_terrainW = 0, m_terrainH = 0, m_terrainNext = 0;
    bool m_terrainReady = false, m_terrainFault = false;
    ULONGLONG m_terrainRefresh = 0;
    UINT64 m_terrainFrame = ~0ull;
    float m_terrainLight[2][3] = {};   // last terrain draw's ambient (c6) and sun (c7): relights paths
    bool m_terrainLightValid = false;
    ComPtr<ID3D11Buffer> m_reliefCB;
    UINT m_reliefDraws = 0, m_reliefDrawsShown = 0;
    // PN-triangle tessellation of eligible skinned draws (tessellation.cpp).
    bool TessellationCandidate() const;
    bool BeginTessellation(DWORD type, D3D11_PRIMITIVE_TOPOLOGY& topo);
    void EndTessellation();
    void DrawTessellated(const std::vector<UINT>& list, INT baseVertex);
    bool m_tessCandidate=false, m_tessFailed=false, m_tessBuiltFlip=false;
    ComPtr<ID3D11HullShader> m_tessHS;
    ComPtr<ID3D11DomainShader> m_tessDS;
    ComPtr<ID3D11Buffer> m_tessCB;
    ComPtr<ID3D11SamplerState> m_tessSampler;
    UINT m_tessDraws=0, m_tessDrawsShown=0;
    struct TessCacheEntry {
        UINT64 identity=0, revision=0;
        INT base=0; UINT stride=0, offset=0; bool weld=false;
        std::vector<UINT> list;
        ComPtr<ID3D11Buffer> buffer;
        UINT count=0; size_t bytes=0;
        bool Matches(UINT64 id,UINT64 rev,INT b,UINT s,UINT o,bool w,const std::vector<UINT>& indices) const {
            return identity==id && revision==rev && base==b && stride==s && offset==o && weld==w && list==indices;
        }
    };
    std::vector<TessCacheEntry> m_tessCache;
    size_t m_tessCacheBytes=0;
    UINT64 m_tessCacheHits=0, m_tessCacheMisses=0;
    void TrackSkinTexture();
    // Skin picker (Insert panel): distinct stage-0 textures of skinned world draws, in draw order.
    int m_pickSkin=-1;
    std::vector<Texture*> m_skinList, m_skinListPrev;
    UINT64 m_skinListFrame=~UINT64(0);
    bool PickedDraw() const;
    ComPtr<ID3D11Buffer> m_materialCB;
    bool m_materialFailed=false;
    bool m_materialDebug=false;
    UINT64 m_materialStatsFrame=~UINT64(0);
    UINT m_materialDraws[3]={};
    bool m_frameGenFailed=false,m_frameGenReady=false;
    UINT64 m_generatedPresents=0;
    UINT64 m_generatedPresentFrame=~UINT64(0);
    bool PresentGeneratedFrame();
    void ReplayOverlay();
    void ResolveTemporal();
    void TemporalJitter(float& x,float& y) const;
    ID3D11ShaderResourceView* PrepareFsr(ID3D11ShaderResourceView* source);
    void FullscreenPass(ID3D11PixelShader*,ID3D11RenderTargetView*,UINT,UINT);
    ComPtr<ID3D11PixelShader> m_taaPS,m_aaCopyPS,m_fsrEasuPS,m_fsrRcasPS;
    ComPtr<ID3D11Buffer> m_taaCB,m_fsrCB;
    ComPtr<ID3D11Texture2D> m_taaColor[2],m_taaDepth[2],m_fsrTex[2];
    ComPtr<ID3D11RenderTargetView> m_taaColorRTV[2],m_taaDepthRTV[2],m_fsrRTV[2];
    ComPtr<ID3D11ShaderResourceView> m_taaColorSRV[2],m_taaDepthSRV[2],m_fsrSRV[2];
    UINT m_taaW=0,m_taaH=0,m_taaSamples=0,m_fsrW=0,m_fsrH=0;
    bool m_taaValid=false,m_taaFailed=false,m_fsrFailed=false;
    int m_taaIndex=0;
    UINT64 m_taaFrame=~UINT64(0);
    float m_taaPrevVP[4][4]={},m_taaPrevOffset[4]={};
    Performance m_perf;
    DrawDiagnostics m_drawDiagnostics;
    void RecordPerformance();
    UINT m_perfDraws=0, m_perfWaterDraws=0, m_perfShadowDraws=0;
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
    // bloom.cpp
    ComPtr<ID3D11PixelShader> m_bloomBright, m_bloomBlur, m_bloomComposite;
    ComPtr<ID3D11Buffer> m_bloomCB;
    ComPtr<ID3D11SamplerState> m_bloomSampler;
    ComPtr<ID3D11BlendState> m_bloomAdd;
    ComPtr<ID3D11Texture2D> m_bloomTex[2];
    ComPtr<ID3D11RenderTargetView> m_bloomRTV[2];
    ComPtr<ID3D11ShaderResourceView> m_bloomSRV[2];
    UINT m_bloomW = 0, m_bloomH = 0;
    bool m_bloomFailed = false;
    bool EnsureBloom();
    void ApplyBloom();
    ComPtr<ID3D11BlendState> m_multiplyBlend;
    SunConstants m_sun = {};
    SunConstants m_sunUploaded = {};
    bool m_sunUploadedValid = false;
    bool m_sunNeutral = false; // effects off: m_sun is zero and uploaded; nothing to compare per draw
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
    bool SceneFx() const { return g_config.effectsEnabled &&
        (g_config.sunLighting || g_config.postEnabled || g_config.grassPhysics || g_config.aoEnabled || g_config.waterEnabled || g_config.taa || g_config.fsr31 || g_config.materialsEnabled); }
    void CapturePusher(UINT firstVertex, UINT numVertices);
    // lights.cpp: night point lights from the emission masks.
    static const int kMaxPointLights = 1024; // lamps lit at once (t18)
    // b5: x = lamps, y = inverse-square knee, z = tiles per row, w = 1 when the tiles are valid.
    struct PointLightConstants { float count[4]; };
    struct LightEmitter { float local[3]; BYTE weights[4], indices[4]; float rgb[3]; float weight; UINT count; };
    struct WorldLight { float pos[3]; float rgb[3]; UINT lastSeen; UINT count; };
    std::unordered_map<unsigned long long, std::vector<LightEmitter>> m_emitterCache;
    std::unordered_map<unsigned long long, WorldLight> m_worldLights;
    // Lamps as drawn, keyed by their world light: eased in and out so a lamp that appears,
    // disappears or leaves the kMaxPointLights nearest does not pop.
    struct ShownLight { float pos[3]; float rgb[3]; float level; float dist; bool live; };
    std::unordered_map<unsigned long long, ShownLight> m_shownLights;
    LARGE_INTEGER m_shownLightsTime = {};
    PointLightConstants m_pointLights = {};
    std::vector<float> m_lightData; // per lamp: position, radius, colour, 0
    ComPtr<ID3D11Buffer> m_lightDataBuf;
    ComPtr<ID3D11ShaderResourceView> m_lightDataSRV;
    UINT m_lightDataCapacity = 0;
    ComPtr<ID3D11Buffer> m_pointLightCB;
    bool m_pointLightsDirty = true;
    UINT m_pointLightsShown = 0, m_pointLightTris = 0;
    void CaptureEmitters(DWORD type, UINT baseVertex, UINT startIndex, UINT primCount, bool indexed);
    void BuildPointLights();
    void BindPointLights();
    // Shadow casters recorded during the world pass and drawn together (sun.cpp FlushShadowBatch).
    // Plain pointers: m_shadowKeep holds a reference to each object while the batch is open, taken
    // only when a slot changes from the previous caster (consecutive casters share most of them).
    struct ShadowDraw {
        ID3D11VertexShader* vs; ID3D11PixelShader* ps; ID3D11InputLayout* layout;
        D3D11_PRIMITIVE_TOPOLOGY topo;
        ID3D11Buffer* vb[kMaxStreams]; UINT stride[kMaxStreams], offset[kMaxStreams];
        ID3D11Buffer* ib; DXGI_FORMAT ibFormat; UINT ibOffset;
        ID3D11ShaderResourceView* srv[kMaxStages]; ID3D11SamplerState* sampler[kMaxStages];
        UINT vsFirst, sun, psc; bool indexed; UINT count, start; INT base;
        UINT instances, instanceInfo[4]; // skin_batch.cpp: instanced casters
    };
    std::vector<ShadowDraw> m_shadowBatch;
    std::vector<ComPtr<IUnknown>> m_shadowKeep;
    IUnknown* m_shadowKept[3 + kMaxStreams + 2 * kMaxStages] = {};
    void KeepForShadow(int slot, IUnknown* p) { if (p && m_shadowKept[slot] != p) { m_shadowKeep.emplace_back(p); m_shadowKept[slot] = p; } }
    std::vector<SunConstants> m_shadowSuns;
    std::vector<PSConstants> m_shadowPS;
    // Window constants of the recorded instanced casters, uploaded to m_shadowInstBuf by the flush,
    // so the main instance buffer can be rewritten while casters are still recorded.
    std::vector<float> m_shadowInstConsts;
    ComPtr<ID3D11Buffer> m_shadowInstBuf;
    ComPtr<ID3D11ShaderResourceView> m_shadowInstSRV;
    UINT m_shadowInstElements = 0;
    ComPtr<ID3D11Buffer> m_shadowPSCB;
    UINT m_shadowBatchesFrame = 0;
public:
    void FlushShadowBatch(); // also before game buffers and textures are written (resources.cpp)
    // Draws the recorded terrain sections (terrain_batch.cpp) and skinned instances (skin_batch.cpp).
    // Called before anything that reads or replaces the state they were recorded with: every other
    // draw, clears, copies, uploads.
    void FlushBatches() { if (m_terrainBatchOpen) DrawTerrainBatch(); if (m_skinBatchOpen) DrawSkinBatch(); }
    void FlushSkinBatch() { if (m_skinBatchOpen) DrawSkinBatch(); }
private:
    // Terrain sections: the game draws each 8x8-unit quarter of every 16x16 section separately,
    // with the same shader, textures and states; only stream 0, the index range and c4/c5 change.
    // Those draws are recorded and drawn as one instanced draw of the pulled shader variant.
    struct BatchSection { float r4[4], r5[4]; UINT vertexBase, indexBase, indexCount; INT baseVertex; };
    struct PoolEntry { UINT64 revision; UINT offset, count; };
    bool m_terrainBatchOpen = false; // any other state change draws it first (the setters call FlushBatches)
    // Skinned draws of one mesh in a row: one instanced draw (skin_batch.cpp). Between the draws
    // only the vertex constants c[m_skinLo, m_skinHi) change; each draw adds a copy of them.
    bool m_skinBatchOpen = false, m_skinUnavailable = false;
    DWORD m_skinType = 0;
    UINT m_skinMinIndex = 0, m_skinNumVertices = 0, m_skinStart = 0, m_skinPrims = 0, m_skinCount = 0;
    INT m_skinBase = 0;
    D3D11_PRIMITIVE_TOPOLOGY m_skinTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    UINT m_skinLo = 0, m_skinHi = 0, m_skinInstances = 0, m_skinBufUsed = 0;
    VertexShader8* m_skinVS = nullptr;
    std::vector<float> m_skinConsts;
    ComPtr<ID3D11Buffer> m_skinBuf, m_skinCB;
    ComPtr<ID3D11ShaderResourceView> m_skinSRV;
    UINT m_skinStatsFrame = 0, m_skinBatches = 0, m_skinBatchedDraws = 0, m_skinAppended = 0, m_skinLogged = 0;
    bool CreateSkinBatchResources();
    void SetSkinInstanceInfo(const UINT info[4], ID3D11ShaderResourceView* consts);
    bool StartSkinBatch(DWORD type, UINT minIndex, UINT numVertices, UINT startIndex, UINT primCount,
                        UINT writeLo, UINT writeHi, UINT count, D3D11_PRIMITIVE_TOPOLOGY topo);
    bool AppendSkinBatch(DWORD type, UINT minIndex, UINT numVertices, UINT startIndex, UINT primCount);
    void DrawSkinBatch();
    DWORD m_terrainType = 0;
    UINT m_terrainStride0 = 0, m_terrainMaxIndices = 0, m_terrainStream1[2] = {};
    VertexShader8* m_terrainVS = nullptr;
    std::vector<BatchSection> m_terrainSections;
    ComPtr<ID3D11Buffer> m_batchVertexPool, m_batchIndexPool, m_batchSectionBuf, m_batchCB;
    ComPtr<ID3D11ShaderResourceView> m_batchVertexSRV, m_batchIndexSRV, m_batchSectionSRV;
    UINT m_batchVertexUsed = 0, m_batchIndexUsed = 0, m_batchSectionCapacity = 0;
    std::unordered_map<UINT64, PoolEntry> m_batchVertices;                    // by buffer identity
    std::map<std::tuple<UINT64, UINT, UINT, DWORD>, PoolEntry> m_batchIndices; // identity, start, prims, type
    bool m_batchUnavailable = false;
    UINT m_batchDrawsLogged = 0, m_batchStatsFrame = 0, m_batchDraws = 0, m_batchSectionCount = 0, m_batchSingles = 0;
    bool CreateBatchResources();
    bool PoolVertices(VertexBuffer* vb, UINT& offset);
    bool PoolIndices(DWORD type, UINT startIndex, UINT primCount, UINT& offset, UINT& count);
    bool RecordTerrainSection(DWORD type, UINT startIndex, UINT primCount);
    bool StartTerrainBatch(DWORD type, UINT startIndex, UINT primCount);
    bool AppendTerrainBatch(DWORD type, UINT startIndex, UINT primCount);
    void DrawTerrainBatch();
    // Screen tiles of kLightTile pixels, each with the lamps whose sphere can reach it (t17): the
    // pixel shader walks those instead of every lamp. count[2] = tiles per row, count[3] = 1 valid.
    static const int kLightTile = 32;
    static const UINT kLightsPerTile = 48; // the nearest are kept
    ComPtr<ID3D11Buffer> m_lightTileBuf;
    ComPtr<ID3D11ShaderResourceView> m_lightTileSRV;
    UINT m_lightTileCapacity = 0;
    std::vector<UINT> m_lightTiles;
    void BuildLightTiles();
    // Stage-0 night emission mask (world draws, time-of-day night only).
    ID3D11ShaderResourceView* ActiveEmissive() const {
        if (!g_config.emissive || !m_tod.active || m_tod.dayW > 0.98f || !m_worldDraw || m_sun.flags[1] > 0.5f ||
            !m_textures[0] || m_textures[0]->GetType() != D8RTYPE_TEXTURE) return nullptr;
        return static_cast<Texture*>(static_cast<ID8Texture*>(m_textures[0]))->m_emissiveSrv.Get();
    }
    // Stage-0 normal map for this draw (world draws only, not the shadow pass).
    ID3D11ShaderResourceView* ActiveNormalMap() const {
        int stage=ActiveMaterial()?MaterialTextureStage():0;
        if (!g_config.normalMaps || !m_worldDraw || m_sun.flags[1] > 0.5f || !m_textures[stage] ||
            m_textures[stage]->GetType() != D8RTYPE_TEXTURE) return nullptr;
        return static_cast<Texture*>(static_cast<ID8Texture*>(m_textures[stage]))->m_normalSrv.Get();
    }
    // Stage-0 roughness map: relit dinosaur draws (specular per pixel).
    ID3D11ShaderResourceView* ActiveRoughness() const {
        if (!g_config.normalMaps || !m_worldDraw || m_worldKind != 1 || m_sun.flags[1] > 0.5f || ActiveMaterial() ||
            !m_textures[0] || m_textures[0]->GetType() != D8RTYPE_TEXTURE) return nullptr;
        return static_cast<Texture*>(static_cast<ID8Texture*>(m_textures[0]))->m_roughSrv.Get();
    }
    bool GrassCaptureActive() const {
        return (g_config.grassPhysics || (g_config.waterEnabled && g_config.waterInteraction) || (g_config.fsr31 && !m_fsr31Failed)) &&
            m_worldDraw && m_worldKind == 1 && m_sunStarted && !m_sunResolved;
    }
    float m_pushBoxes[32][4] = {};  // world x/z footprint per pusher: min x, min z, max x, max z
    void UploadGrass();
    ComPtr<ID3D11Buffer> m_grassCB;
    float m_pushers[32][4] = {};
    UINT m_pusherCount = 0, m_lastBones = 0;
    bool m_grassUploaded = false;
    bool m_grassApplied = false;
    UINT m_vsWriteLo = 0, m_vsWriteHi = 0;
    size_t m_vsRingValid = 0; // bytes of the current ring window copied from m_vsConst
    UINT m_vsConstUsed = 0; // registers 0..used-1 have been written at least once: the upload copies only those
    // Per-draw bindings last sent to the driver: unchanged ones are not sent again. Passes that
    // bind outside the draw path (clears, resolve, bloom, AA, FSR, Present, ImGui) end with
    // InvalidateBindings(); binding a render target nulls stages 0-7 through BindPSSrv.
    struct BindingCache {
        static const UINT Srv = 20, Samp = 12, Cb = 6;
        ID3D11ShaderResourceView *psSrv[Srv], *vsSrv[Srv];
        ID3D11SamplerState *psSamp[Samp], *vsSamp[Samp];
        ID3D11Buffer *vsCb[Cb], *psCb[Cb];
    } m_bind{};
    void InvalidateBindings();
    void BindPSSrv(UINT slot, UINT n, ID3D11ShaderResourceView* const* v);
    void BindVSSrv(UINT slot, UINT n, ID3D11ShaderResourceView* const* v);
    void BindPSSamplers(UINT slot, UINT n, ID3D11SamplerState* const* v);
    void BindVSSamplers(UINT slot, UINT n, ID3D11SamplerState* const* v);
    void BindVSCB(UINT slot, UINT n, ID3D11Buffer* const* v);
    void BindPSCB(UINT slot, UINT n, ID3D11Buffer* const* v);
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
    ComPtr<ID3D11Buffer> m_waterCB;
    bool m_waterCBZero = false; // m_waterCB holds the all-zero block of non-water draws
    ComPtr<ID3D11Texture2D> m_waterDepthCopy, m_waterColorCopy;
    ComPtr<ID3D11ShaderResourceView> m_waterDepthSRV, m_waterColorSRV;
    ComPtr<ID3D11SamplerState> m_waterSampler;
    bool m_waterDraw = false, m_waterSnapshotReady = false;
    bool m_waterShaderDraw = false; // current draw uses the water pixel shader (set before UpdateSun)
    UINT m_waterW = 0, m_waterH = 0, m_waterSamples = 0;
    float m_waterFrameTime = 0; // frozen for every batch sharing an opaque snapshot
    float m_waterViewProjection[4][4]={};
    ULONGLONG m_waterTimeOrigin = 0;
    WaterInteractionState m_waterInteraction;
    WaterRegionMap m_waterRegions;
    ComPtr<ID3D11Texture2D> m_waterRegionTexture;
    ComPtr<ID3D11ShaderResourceView> m_waterRegionSRV;
    ComPtr<ID3D11Texture2D> m_waterTerrainTexture;
    ComPtr<ID3D11ShaderResourceView> m_waterTerrainSRV;
    ComPtr<ID3D11Texture2D> m_waterShoreTexture;
    ComPtr<ID3D11ShaderResourceView> m_waterShoreSRV;
    std::vector<float> m_waterRegionHeights;
    std::vector<float> m_waterRegionScan;   // rescan chunk, compared with m_waterRegionHeights
    bool m_waterRegionChanged = false;      // the current rescan found different heights
    float m_waterRegionBuiltLevel = 0;      // water height the region map was built for
    void* m_waterRegionTerrain=nullptr;
    void* m_waterRegionHeightField=nullptr;
    UINT m_waterRegionW=0,m_waterRegionH=0,m_waterRegionNext=0;
    UINT64 m_waterRegionFrame=~UINT64(0);
    ULONGLONG m_waterRegionRefresh=0;
    float m_waterRegionLevel=0,m_waterRegionCpuMs=0;
    bool m_waterRegionReady=false,m_waterNativeAvailable=false,m_waterNativeFault=false;
    void UpdateWaterInteractions();
    float WaterTime() const;
    void EndSunFrame();
};
