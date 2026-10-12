// d3d8_types.h - Direct3D 8 types, constants and interface layouts.
//
// The Windows SDK no longer ships d3d8.h, so the subset used by the game is defined here.
// Interface classes list their methods in exact vtable order; they must not gain virtual
// destructors or reordered methods.
#pragma once
#include <windows.h>

// ---- formats -----------------------------------------------------------------------------------
enum D3D8Format : DWORD {
    D8FMT_UNKNOWN = 0,
    D8FMT_R8G8B8 = 20, D8FMT_A8R8G8B8 = 21, D8FMT_X8R8G8B8 = 22, D8FMT_R5G6B5 = 23,
    D8FMT_X1R5G5B5 = 24, D8FMT_A1R5G5B5 = 25, D8FMT_A4R4G4B4 = 26, D8FMT_R3G3B2 = 27,
    D8FMT_A8 = 28, D8FMT_A8R3G3B2 = 29, D8FMT_X4R4G4B4 = 30, D8FMT_A2B10G10R10 = 31,
    D8FMT_G16R16 = 34,
    D8FMT_A8P8 = 40, D8FMT_P8 = 41,
    D8FMT_L8 = 50, D8FMT_A8L8 = 51, D8FMT_A4L4 = 52,
    D8FMT_V8U8 = 60, D8FMT_L6V5U5 = 61, D8FMT_X8L8V8U8 = 62, D8FMT_Q8W8V8U8 = 63, D8FMT_V16U16 = 64,
    D8FMT_D16_LOCKABLE = 70, D8FMT_D32 = 71, D8FMT_D15S1 = 73, D8FMT_D24S8 = 75, D8FMT_D16 = 80,
    D8FMT_D24X8 = 77, D8FMT_D24X4S4 = 79,
    D8FMT_VERTEXDATA = 100, D8FMT_INDEX16 = 101, D8FMT_INDEX32 = 102,
    D8FMT_DXT1 = MAKEFOURCC('D', 'X', 'T', '1'), D8FMT_DXT2 = MAKEFOURCC('D', 'X', 'T', '2'),
    D8FMT_DXT3 = MAKEFOURCC('D', 'X', 'T', '3'), D8FMT_DXT4 = MAKEFOURCC('D', 'X', 'T', '4'),
    D8FMT_DXT5 = MAKEFOURCC('D', 'X', 'T', '5'),
};

enum { D8POOL_DEFAULT = 0, D8POOL_MANAGED = 1, D8POOL_SYSTEMMEM = 2, D8POOL_SCRATCH = 3 };
enum { D8USAGE_RENDERTARGET = 0x1, D8USAGE_DEPTHSTENCIL = 0x2, D8USAGE_WRITEONLY = 0x8,
       D8USAGE_DYNAMIC = 0x200 };
enum { D8RTYPE_SURFACE = 1, D8RTYPE_VOLUME = 2, D8RTYPE_TEXTURE = 3, D8RTYPE_VOLUMETEXTURE = 4,
       D8RTYPE_CUBETEXTURE = 5, D8RTYPE_VERTEXBUFFER = 6, D8RTYPE_INDEXBUFFER = 7 };
enum { D8LOCK_READONLY = 0x10, D8LOCK_DISCARD = 0x2000, D8LOCK_NOOVERWRITE = 0x1000,
       D8LOCK_NOSYSLOCK = 0x800 };
enum { D8PT_POINTLIST = 1, D8PT_LINELIST = 2, D8PT_LINESTRIP = 3, D8PT_TRIANGLELIST = 4,
       D8PT_TRIANGLESTRIP = 5, D8PT_TRIANGLEFAN = 6 };
enum { D8CLEAR_TARGET = 1, D8CLEAR_ZBUFFER = 2, D8CLEAR_STENCIL = 4 };

#define D8ERR_INVALIDCALL     MAKE_HRESULT(1, 0x876, 2156)
#define D8ERR_NOTAVAILABLE    MAKE_HRESULT(1, 0x876, 2154)
#define D8ERR_OUTOFVIDEOMEMORY MAKE_HRESULT(1, 0x876, 380)

// ---- render states / texture stage states ------------------------------------------------------
enum {
    RS_ZENABLE = 7, RS_FILLMODE = 8, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15,
    RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHAREF = 24,
    RS_ALPHAFUNC = 25, RS_DITHERENABLE = 26, RS_ALPHABLENDENABLE = 27, RS_FOGENABLE = 28,
    RS_SPECULARENABLE = 29, RS_FOGCOLOR = 34, RS_FOGTABLEMODE = 35, RS_FOGSTART = 36, RS_FOGEND = 37,
    RS_FOGDENSITY = 38, RS_ZBIAS = 47, RS_RANGEFOGENABLE = 48, RS_STENCILENABLE = 52,
    RS_STENCILFAIL = 53, RS_STENCILZFAIL = 54, RS_STENCILPASS = 55, RS_STENCILFUNC = 56,
    RS_STENCILREF = 57, RS_STENCILMASK = 58, RS_STENCILWRITEMASK = 59, RS_TEXTUREFACTOR = 60,
    RS_LIGHTING = 137, RS_FOGVERTEXMODE = 140, RS_COLORWRITEENABLE = 168, RS_BLENDOP = 171,
    RS_COUNT = 256,
};
enum {
    TSS_COLOROP = 1, TSS_COLORARG1 = 2, TSS_COLORARG2 = 3, TSS_ALPHAOP = 4, TSS_ALPHAARG1 = 5,
    TSS_ALPHAARG2 = 6, TSS_BUMPENVMAT00 = 7, TSS_BUMPENVMAT01 = 8, TSS_BUMPENVMAT10 = 9,
    TSS_BUMPENVMAT11 = 10, TSS_TEXCOORDINDEX = 11, TSS_ADDRESSU = 13, TSS_ADDRESSV = 14,
    TSS_BORDERCOLOR = 15, TSS_MAGFILTER = 16, TSS_MINFILTER = 17, TSS_MIPFILTER = 18,
    TSS_MIPMAPLODBIAS = 19, TSS_MAXMIPLEVEL = 20, TSS_MAXANISOTROPY = 21, TSS_BUMPENVLSCALE = 22,
    TSS_BUMPENVLOFFSET = 23, TSS_TEXTURETRANSFORMFLAGS = 24, TSS_ADDRESSW = 25, TSS_COLORARG0 = 26,
    TSS_ALPHAARG0 = 27, TSS_RESULTARG = 28, TSS_COUNT = 32,
};
static const int kMaxStages = 8;
static const int kMaxStreams = 16;
static const int kMaxVSConst = 256;
static const int kMaxPSConst = 8;

// ---- structures ----------------------------------------------------------------------------------
struct D3D8PresentParameters {
    UINT BackBufferWidth, BackBufferHeight;
    DWORD BackBufferFormat;
    UINT BackBufferCount;
    DWORD MultiSampleType, SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed, EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat, Flags;
    UINT FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
};
struct D3D8DisplayMode { UINT Width, Height, RefreshRate; DWORD Format; };
struct D3D8SurfaceDesc { DWORD Format, Type, Usage, Pool; UINT Size; DWORD MultiSampleType; UINT Width, Height; };
struct D3D8VolumeDesc { DWORD Format, Type, Usage, Pool; UINT Size; UINT Width, Height, Depth; };
struct D3D8VertexBufferDesc { DWORD Format, Type, Usage, Pool; UINT Size; DWORD FVF; };
struct D3D8IndexBufferDesc { DWORD Format, Type, Usage, Pool; UINT Size; };
struct D3D8LockedRect { INT Pitch; void* pBits; };
struct D3D8LockedBox { INT RowPitch, SlicePitch; void* pBits; };
struct D3D8Box { UINT Left, Top, Right, Bottom, Front, Back; };
struct D3D8Viewport { DWORD X, Y, Width, Height; float MinZ, MaxZ; };
struct D3D8Matrix { float m[4][4]; };
struct D3D8Material { float diffuse[4], ambient[4], specular[4], emissive[4], power; };
struct D3D8Light { DWORD Type; float diffuse[4], specular[4], ambient[4], pos[3], dir[3];
                   float range, falloff, att0, att1, att2, theta, phi; };
struct D3D8ClipStatus { DWORD ClipUnion, ClipIntersection; };
struct D3D8RasterStatus { BOOL InVBlank; UINT ScanLine; };
struct D3D8GammaRamp { WORD red[256], green[256], blue[256]; };
struct D3D8Rect { LONG x1, y1, x2, y2; };
struct D3D8AdapterIdentifier {
    char Driver[512];
    char Description[512];
    LARGE_INTEGER DriverVersion;
    DWORD VendorId, DeviceId, SubSysId, Revision;
    GUID DeviceIdentifier;
    DWORD WHQLLevel;
};
static const size_t kCapsSize = 212; // sizeof(D3DCAPS8)

// ---- interface layouts ---------------------------------------------------------------------------
#define D8M(name) virtual HRESULT STDMETHODCALLTYPE name
#define D8MT(type, name) virtual type STDMETHODCALLTYPE name

struct ID8Unknown {
    D8M(QueryInterface)(REFIID riid, void** out) = 0;
    D8MT(ULONG, AddRef)() = 0;
    D8MT(ULONG, Release)() = 0;
};

struct ID8Device;

struct ID8Resource : ID8Unknown {
    D8M(GetDevice)(ID8Device** dev) = 0;
    D8M(SetPrivateData)(REFGUID g, const void* data, DWORD size, DWORD flags) = 0;
    D8M(GetPrivateData)(REFGUID g, void* data, DWORD* size) = 0;
    D8M(FreePrivateData)(REFGUID g) = 0;
    D8MT(DWORD, SetPriority)(DWORD p) = 0;
    D8MT(DWORD, GetPriority)() = 0;
    D8MT(void, PreLoad)() = 0;
    D8MT(DWORD, GetType)() = 0;
};

struct ID8BaseTexture : ID8Resource {
    D8MT(DWORD, SetLOD)(DWORD lod) = 0;
    D8MT(DWORD, GetLOD)() = 0;
    D8MT(DWORD, GetLevelCount)() = 0;
};

struct ID8Surface;
struct ID8Volume;

struct ID8Texture : ID8BaseTexture {
    D8M(GetLevelDesc)(UINT level, D3D8SurfaceDesc* desc) = 0;
    D8M(GetSurfaceLevel)(UINT level, ID8Surface** surf) = 0;
    D8M(LockRect)(UINT level, D3D8LockedRect* lr, const RECT* rect, DWORD flags) = 0;
    D8M(UnlockRect)(UINT level) = 0;
    D8M(AddDirtyRect)(const RECT* rect) = 0;
};

struct ID8VolumeTexture : ID8BaseTexture {
    D8M(GetLevelDesc)(UINT level, D3D8VolumeDesc* desc) = 0;
    D8M(GetVolumeLevel)(UINT level, ID8Volume** vol) = 0;
    D8M(LockBox)(UINT level, D3D8LockedBox* lb, const D3D8Box* box, DWORD flags) = 0;
    D8M(UnlockBox)(UINT level) = 0;
    D8M(AddDirtyBox)(const D3D8Box* box) = 0;
};

struct ID8Surface : ID8Unknown {
    D8M(GetDevice)(ID8Device** dev) = 0;
    D8M(SetPrivateData)(REFGUID g, const void* data, DWORD size, DWORD flags) = 0;
    D8M(GetPrivateData)(REFGUID g, void* data, DWORD* size) = 0;
    D8M(FreePrivateData)(REFGUID g) = 0;
    D8M(GetContainer)(REFIID riid, void** container) = 0;
    D8M(GetDesc)(D3D8SurfaceDesc* desc) = 0;
    D8M(LockRect)(D3D8LockedRect* lr, const RECT* rect, DWORD flags) = 0;
    D8M(UnlockRect)() = 0;
};

struct ID8Volume : ID8Unknown {
    D8M(GetDevice)(ID8Device** dev) = 0;
    D8M(SetPrivateData)(REFGUID g, const void* data, DWORD size, DWORD flags) = 0;
    D8M(GetPrivateData)(REFGUID g, void* data, DWORD* size) = 0;
    D8M(FreePrivateData)(REFGUID g) = 0;
    D8M(GetContainer)(REFIID riid, void** container) = 0;
    D8M(GetDesc)(D3D8VolumeDesc* desc) = 0;
    D8M(LockBox)(D3D8LockedBox* lb, const D3D8Box* box, DWORD flags) = 0;
    D8M(UnlockBox)() = 0;
};

struct ID8VertexBuffer : ID8Resource {
    D8M(Lock)(UINT offset, UINT size, BYTE** data, DWORD flags) = 0;
    D8M(Unlock)() = 0;
    D8M(GetDesc)(D3D8VertexBufferDesc* desc) = 0;
};

struct ID8IndexBuffer : ID8Resource {
    D8M(Lock)(UINT offset, UINT size, BYTE** data, DWORD flags) = 0;
    D8M(Unlock)() = 0;
    D8M(GetDesc)(D3D8IndexBufferDesc* desc) = 0;
};

struct ID8Direct3D : ID8Unknown {
    D8M(RegisterSoftwareDevice)(void* init) = 0;
    D8MT(UINT, GetAdapterCount)() = 0;
    D8M(GetAdapterIdentifier)(UINT adapter, DWORD flags, D3D8AdapterIdentifier* id) = 0;
    D8MT(UINT, GetAdapterModeCount)(UINT adapter) = 0;
    D8M(EnumAdapterModes)(UINT adapter, UINT mode, D3D8DisplayMode* out) = 0;
    D8M(GetAdapterDisplayMode)(UINT adapter, D3D8DisplayMode* out) = 0;
    D8M(CheckDeviceType)(UINT adapter, DWORD type, DWORD displayFmt, DWORD bbFmt, BOOL windowed) = 0;
    D8M(CheckDeviceFormat)(UINT adapter, DWORD type, DWORD adapterFmt, DWORD usage, DWORD rtype, DWORD fmt) = 0;
    D8M(CheckDeviceMultiSampleType)(UINT adapter, DWORD type, DWORD fmt, BOOL windowed, DWORD ms) = 0;
    D8M(CheckDepthStencilMatch)(UINT adapter, DWORD type, DWORD adapterFmt, DWORD rtFmt, DWORD dsFmt) = 0;
    D8M(GetDeviceCaps)(UINT adapter, DWORD type, void* caps) = 0;
    D8MT(HMONITOR, GetAdapterMonitor)(UINT adapter) = 0;
    D8M(CreateDevice)(UINT adapter, DWORD type, HWND focus, DWORD flags, D3D8PresentParameters* pp,
                      ID8Device** out) = 0;
};

struct ID8Device : ID8Unknown {
    D8M(TestCooperativeLevel)() = 0;                                                      // 3
    D8MT(UINT, GetAvailableTextureMem)() = 0;
    D8M(ResourceManagerDiscardBytes)(DWORD bytes) = 0;
    D8M(GetDirect3D)(ID8Direct3D** out) = 0;
    D8M(GetDeviceCaps)(void* caps) = 0;
    D8M(GetDisplayMode)(D3D8DisplayMode* mode) = 0;
    D8M(GetCreationParameters)(void* params) = 0;
    D8M(SetCursorProperties)(UINT x, UINT y, ID8Surface* bmp) = 0;                        // 10
    D8MT(void, SetCursorPosition)(int x, int y, DWORD flags) = 0;
    D8MT(BOOL, ShowCursor)(BOOL show) = 0;
    D8M(CreateAdditionalSwapChain)(D3D8PresentParameters* pp, void** out) = 0;
    D8M(Reset)(D3D8PresentParameters* pp) = 0;                                              // 14
    D8M(Present)(const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) = 0;  // 15
    D8M(GetBackBuffer)(UINT index, DWORD type, ID8Surface** out) = 0;
    D8M(GetRasterStatus)(D3D8RasterStatus* st) = 0;
    D8MT(void, SetGammaRamp)(DWORD flags, const D3D8GammaRamp* ramp) = 0;
    D8MT(void, GetGammaRamp)(D3D8GammaRamp* ramp) = 0;
    D8M(CreateTexture)(UINT w, UINT h, UINT levels, DWORD usage, DWORD fmt, DWORD pool, ID8Texture** out) = 0; // 20
    D8M(CreateVolumeTexture)(UINT w, UINT h, UINT d, UINT levels, DWORD usage, DWORD fmt, DWORD pool,
                             ID8VolumeTexture** out) = 0;
    D8M(CreateCubeTexture)(UINT edge, UINT levels, DWORD usage, DWORD fmt, DWORD pool, void** out) = 0;
    D8M(CreateVertexBuffer)(UINT len, DWORD usage, DWORD fvf, DWORD pool, ID8VertexBuffer** out) = 0;
    D8M(CreateIndexBuffer)(UINT len, DWORD usage, DWORD fmt, DWORD pool, ID8IndexBuffer** out) = 0;
    D8M(CreateRenderTarget)(UINT w, UINT h, DWORD fmt, DWORD ms, BOOL lockable, ID8Surface** out) = 0; // 25
    D8M(CreateDepthStencilSurface)(UINT w, UINT h, DWORD fmt, DWORD ms, ID8Surface** out) = 0;
    D8M(CreateImageSurface)(UINT w, UINT h, DWORD fmt, ID8Surface** out) = 0;
    D8M(CopyRects)(ID8Surface* src, const RECT* rects, UINT count, ID8Surface* dst, const POINT* pts) = 0;
    D8M(UpdateTexture)(ID8BaseTexture* src, ID8BaseTexture* dst) = 0;
    D8M(GetFrontBuffer)(ID8Surface* dst) = 0;                                               // 30
    D8M(SetRenderTarget)(ID8Surface* rt, ID8Surface* ds) = 0;
    D8M(GetRenderTarget)(ID8Surface** out) = 0;
    D8M(GetDepthStencilSurface)(ID8Surface** out) = 0;
    D8M(BeginScene)() = 0;
    D8M(EndScene)() = 0;                                                                     // 35
    D8M(Clear)(DWORD count, const D3D8Rect* rects, DWORD flags, DWORD color, float z, DWORD stencil) = 0;
    D8M(SetTransform)(DWORD state, const D3D8Matrix* m) = 0;
    D8M(GetTransform)(DWORD state, D3D8Matrix* m) = 0;
    D8M(MultiplyTransform)(DWORD state, const D3D8Matrix* m) = 0;
    D8M(SetViewport)(const D3D8Viewport* vp) = 0;                                           // 40
    D8M(GetViewport)(D3D8Viewport* vp) = 0;
    D8M(SetMaterial)(const D3D8Material* m) = 0;
    D8M(GetMaterial)(D3D8Material* m) = 0;
    D8M(SetLight)(DWORD index, const D3D8Light* l) = 0;
    D8M(GetLight)(DWORD index, D3D8Light* l) = 0;                                           // 45
    D8M(LightEnable)(DWORD index, BOOL enable) = 0;
    D8M(GetLightEnable)(DWORD index, BOOL* enable) = 0;
    D8M(SetClipPlane)(DWORD index, const float* plane) = 0;
    D8M(GetClipPlane)(DWORD index, float* plane) = 0;
    D8M(SetRenderState)(DWORD state, DWORD value) = 0;                                       // 50
    D8M(GetRenderState)(DWORD state, DWORD* value) = 0;
    D8M(BeginStateBlock)() = 0;
    D8M(EndStateBlock)(DWORD* token) = 0;
    D8M(ApplyStateBlock)(DWORD token) = 0;
    D8M(CaptureStateBlock)(DWORD token) = 0;                                                 // 55
    D8M(DeleteStateBlock)(DWORD token) = 0;
    D8M(CreateStateBlock)(DWORD type, DWORD* token) = 0;
    D8M(SetClipStatus)(const D3D8ClipStatus* st) = 0;
    D8M(GetClipStatus)(D3D8ClipStatus* st) = 0;
    D8M(GetTexture)(DWORD stage, ID8BaseTexture** out) = 0;                                 // 60
    D8M(SetTexture)(DWORD stage, ID8BaseTexture* tex) = 0;
    D8M(GetTextureStageState)(DWORD stage, DWORD type, DWORD* value) = 0;
    D8M(SetTextureStageState)(DWORD stage, DWORD type, DWORD value) = 0;
    D8M(ValidateDevice)(DWORD* passes) = 0;
    D8M(GetInfo)(DWORD id, void* data, DWORD size) = 0;                                       // 65
    D8M(SetPaletteEntries)(UINT pal, const PALETTEENTRY* entries) = 0;
    D8M(GetPaletteEntries)(UINT pal, PALETTEENTRY* entries) = 0;
    D8M(SetCurrentTexturePalette)(UINT pal) = 0;
    D8M(GetCurrentTexturePalette)(UINT* pal) = 0;
    D8M(DrawPrimitive)(DWORD type, UINT start, UINT count) = 0;                               // 70
    D8M(DrawIndexedPrimitive)(DWORD type, UINT minIndex, UINT numVertices, UINT startIndex, UINT primCount) = 0;
    D8M(DrawPrimitiveUP)(DWORD type, UINT primCount, const void* data, UINT stride) = 0;
    D8M(DrawIndexedPrimitiveUP)(DWORD type, UINT minIndex, UINT numVertices, UINT primCount,
                                const void* indices, DWORD indexFmt, const void* data, UINT stride) = 0;
    D8M(ProcessVertices)(UINT srcStart, UINT destIndex, UINT count, ID8VertexBuffer* dst, DWORD flags) = 0;
    D8M(CreateVertexShader)(const DWORD* decl, const DWORD* func, DWORD* handle, DWORD usage) = 0; // 75
    D8M(SetVertexShader)(DWORD handle) = 0;
    D8M(GetVertexShader)(DWORD* handle) = 0;
    D8M(DeleteVertexShader)(DWORD handle) = 0;
    D8M(SetVertexShaderConstant)(DWORD reg, const void* data, DWORD count) = 0;
    D8M(GetVertexShaderConstant)(DWORD reg, void* data, DWORD count) = 0;                    // 80
    D8M(GetVertexShaderDeclaration)(DWORD handle, void* data, DWORD* size) = 0;
    D8M(GetVertexShaderFunction)(DWORD handle, void* data, DWORD* size) = 0;
    D8M(SetStreamSource)(UINT stream, ID8VertexBuffer* vb, UINT stride) = 0;
    D8M(GetStreamSource)(UINT stream, ID8VertexBuffer** vb, UINT* stride) = 0;
    D8M(SetIndices)(ID8IndexBuffer* ib, UINT baseVertex) = 0;                                 // 85
    D8M(GetIndices)(ID8IndexBuffer** ib, UINT* baseVertex) = 0;
    D8M(CreatePixelShader)(const DWORD* func, DWORD* handle) = 0;
    D8M(SetPixelShader)(DWORD handle) = 0;
    D8M(GetPixelShader)(DWORD* handle) = 0;
    D8M(DeletePixelShader)(DWORD handle) = 0;                                                 // 90
    D8M(SetPixelShaderConstant)(DWORD reg, const void* data, DWORD count) = 0;
    D8M(GetPixelShaderConstant)(DWORD reg, void* data, DWORD count) = 0;
    D8M(GetPixelShaderFunction)(DWORD handle, void* data, DWORD* size) = 0;
    D8M(DrawRectPatch)(UINT handle, const float* segs, const void* info) = 0;
    D8M(DrawTriPatch)(UINT handle, const float* segs, const void* info) = 0;                 // 95
    D8M(DeletePatch)(UINT handle) = 0;
};
