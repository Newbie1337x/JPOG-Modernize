# Reverse-engineering notes

Analysed version: PC `SimJP.exe` (fixed base `0x400000`, no ASLR). Engine DLLs (`T*.dll`, `P*.dll`) share base `0x10000000` and get relocated, so RVAs or exports by name are used. The DLLs export Toshi's C++ names, which makes analysis much easier.

## Cursor and input

| What | Where |
|---|---|
| Pointer acceleration | `TGuiInterface!TGUIScreen::ApplyPointerAccel` (export). Doubles or quadruples the delta depending on threshold `+0x138` and mode `+0x140` |
| GUI pointer position | `TGUIScreen +0x114 / +0x118`; virtual size `+0x70 / +0x72` (shorts, 640x480) |
| Pointer clamp | `TGUIScreen::SetPointerPosition(int,int)`, thiscall, ret 8 |
| DirectInput keyboard | `TInputDXInterface +0x39dc`: `push 0x15` (EXCLUSIVE\|FOREGROUND\|NOWINKEY) → `0x06` |
| First-person sensitivity slider | `0x57d672+4` and `0x57d682+6` (minimum 0.25) |

## Camera (SimJP)

| What | Where |
|---|---|
| Wheel zoom (dt * 2 per click) | `0x55c5bf`, `0x55c609`: `fld [esp]; mov edx,[ebx]`. EBP = input event; doodads `0x30009` and `0x3000a` |
| Damping | `0x527d3f` (`fld [0xd12c00]`) and `0x527d51` (`fld [0x8b61f0]`); dt in EDI |
| Edge scroll speed | `0xd1a954` (vertical, 2.0) and `0xd1a958` (horizontal, 1.0) |
| Edge scroll threshold | `AScrollingState::OnUpdate 0x55cb40`; `0x55cbe5 mov ebx,[edi+0x114]` |
| Camera limits | `Data\Constant.ini`: `WorldCamMaxFOVHeight`, `WorldCamMinPitch`, `WorldCamMaxPitch` |

## Rendering

| What | Where |
|---|---|
| FOV / Hor+ | `TRenderInterface +0xebf5`: `fld [0.5]; fmul [esi+0x20]` |
| D3D8 creation | `TRenderD3DInterface` imports `Direct3DCreate8`. Vtables: `IDirect3D8[15]` CreateDevice, `IDirect3DDevice8[14]` Reset, `[15]` Present |
| DDS textures | Statically linked `D3DXCreateTextureFromFileInMemoryEx`: `TRenderD3DInterface +0xf51a` (stdcall, 15 args) |
| Raw textures | `TTextureResourceHAL::CreateFromMemory5551/4444/8888` at `+0xcfd0 / +0xd1d0 / +0xd3d0` (w, h, mips, data). Data is RGBA byte order; the engine swaps R/B |
| Texture resource | `TTextureResourceHAL`: `+0x60` IDirect3DTexture8*, `+0x64` width, `+0x68` height (the GUI uses these for its pixel coordinates) |
| Shader modules | `Shaders\D3D\*.dll`, see below |
| `.tml` format | `"TML1"`, u32 0, u32 N. Entries: `{u32 idx, u32 size, u32 3, f32 35, u16 fmt, u16 w, u16 h, u16 0xffff, u16 0, u16 'C'}` + data. Then u32 M and M 32-byte names. fmt: 6 = DDS, 2 = 1555, 0 = 8888 |

### Shader modules (`Shaders\D3D`)

The engine renders through per-category "shader" DLLs (`T*ShaderHAL` classes). They are the natural hook points for a custom renderer:

| DLL | Draws | Notes |
|---|---|---|
| `TTerrainShaderD3D` | terrain, water, grass | `TTerrainShaderHAL`, `TWaterShaderHAL`, `TGrassShaderHAL`; wind, sun angle, scattering constants |
| `TSkinShaderD3D` | skinned meshes (dinos, people) | light scattering (`SetHG`, `SetInscatteringMult`, `SetSunTheta`, `SetReflectance`) |
| `TSysShaderD3D` | static meshes (buildings, flora) | `SetVertexShaderScatteringConstants`, high-end/scattering capability checks |
| `TSpriteShaderD3D` | UI, text, sprites | |
| `TLineShaderD3D` | lines | |

Vertex shaders implement atmospheric light scattering (Henyey-Greenstein phase function, in-scattering), so the D3D11 layer must translate `vs_1_1` programs in addition to the fixed-function pipeline.

## GUI

| What | Where |
|---|---|
| GUI ortho projection | `PGUIRenderer!PGUITRDisplayContext::BeginScene`. Scale at `+0x224 / +0x228`; matrix stack at `+0x230 + idx*0x40`, `idx` at `+0x22c`; set world via `(+0x21c)->vtbl[0x7c]` |
| Widgets | `TGUIWidget`: `+0x6c` x, `+0x6e` y, `+0x70` width, `+0x72` height (shorts); flags `+0x88` (`&4` = visible). Children: first at `+0x50`, sentinel `+0x44`, next at `+0xc` |
| Screen paint | `TGUIScreen::Paint(TGUIWidget*)` (export) |
| Popup menus | `PGUICore!PGUIMenuDialog::UpdatePosition` and `PGUIDiscMenuDialog::UpdatePosition`, clamped to `[0, screen width]` |
| 3D views in the GUI | `AGUIRenderObject::SetViewRectangle 0x678a90`: GUI rect * scale `+0x224 / +0x228` |
| GUI ↔ render | `ACamera::ConvertGuiScreenToRenderScreen 0x50bd60`, `ConvertRenderScreenToGuiScreen 0x50bdd0` (stdcall, ret 8) |
| World → GUI | `ARenderer::ConvertToScreenCoords(TVector3&) 0x477ec0`; returns (-100, -100) behind the camera |
| Picking | `APointerController::RequestTargetPosition 0x51d800` → `ConvertGuiScreenToRenderScreen` → `ARenderer::ConvertScreenCoordsToWorldCoords` |
| GUI layout data | `Data\GUI\main.gui` (plain text) with `xpos/ypos/alignx` properties |

## Other

- Movies: `Data\Movies\*.bik` (Bink 1, 640x448). The main menu loop is `JPGtloop.bik`.
- dgVoodoo 2.87 (`D3D8.dll` in the game folder) translates to D3D11 and is configured in `dgVoodoo.conf`.
- `SimJP.exe` is not Large Address Aware; bit `0x20` of the PE Characteristics can be set.
