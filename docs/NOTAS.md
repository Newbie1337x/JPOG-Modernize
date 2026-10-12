# Notas de ingeniería inversa

Versión analizada: `SimJP.exe` de la edición PC (base fija `0x400000`, sin ASLR). Las DLL del motor (`T*.dll`, `P*.dll`) comparten base `0x10000000` y se reubican, así que se usan RVA o exports por nombre. Las DLL exportan los nombres C++ de Toshi, lo que facilita mucho el análisis.

## Cursor y entrada

| Qué | Dónde |
|---|---|
| Aceleración del puntero | `TGuiInterface!TGUIScreen::ApplyPointerAccel` (export). Duplica o cuadruplica el delta según el umbral `+0x138` y el modo `+0x140` |
| Posición del puntero (GUI) | `TGUIScreen +0x114 / +0x118`; tamaño virtual `+0x70 / +0x72` (shorts, 640x480) |
| Límite del puntero | `TGUIScreen::SetPointerPosition(int,int)`, thiscall, ret 8 |
| Teclado DirectInput | `TInputDXInterface +0x39dc`: `push 0x15` (EXCLUSIVE\|FOREGROUND\|NOWINKEY) → `0x06` |
| Slider de sensibilidad FP | `0x57d672+4` y `0x57d682+6` (mínimo 0.25) |

## Cámara (SimJP)

| Qué | Dónde |
|---|---|
| Zoom con la rueda (dt * 2 por clic) | `0x55c5bf`, `0x55c609`: `fld [esp]; mov edx,[ebx]`. EBP = evento; doodads `0x30009` y `0x3000a` |
| Amortiguación | `0x527d3f` (`fld [0xd12c00]`) y `0x527d51` (`fld [0x8b61f0]`); dt en EDI |
| Velocidad de bordes | `0xd1a954` (vertical, 2.0) y `0xd1a958` (horizontal, 1.0) |
| Bordes (umbral) | `AScrollingState::OnUpdate 0x55cb40`; `0x55cbe5 mov ebx,[edi+0x114]` |
| Límites de cámara | `Data\Constant.ini`: `WorldCamMaxFOVHeight`, `WorldCamMinPitch`, `WorldCamMaxPitch` |

## Render

| Qué | Dónde |
|---|---|
| FOV / Hor+ | `TRenderInterface +0xebf5`: `fld [0.5]; fmul [esi+0x20]` |
| Creación de D3D8 | `TRenderD3DInterface` importa `Direct3DCreate8`. Vtables: `IDirect3D8[15]` CreateDevice, `IDirect3DDevice8[14]` Reset, `[15]` Present |
| Texturas DDS | D3DXCreateTextureFromFileInMemoryEx enlazada estáticamente: `TRenderD3DInterface +0xf51a` (stdcall, 15 args) |
| Texturas crudas | `TTextureResourceHAL::CreateFromMemory5551/4444/8888` en `+0xcfd0 / +0xd1d0 / +0xd3d0` (w, h, mips, data). Datos en orden RGBA y el motor invierte R/B |
| Recurso de textura | `TTextureResourceHAL`: `+0x60` IDirect3DTexture8*, `+0x64` ancho, `+0x68` alto (la GUI usa esto para sus coordenadas en píxeles) |
| Formato `.tml` | `"TML1"`, u32 0, u32 N. Entradas: `{u32 idx, u32 size, u32 3, f32 35, u16 fmt, u16 w, u16 h, u16 0xffff, u16 0, u16 'C'}` + datos. Luego u32 M y M nombres de 32 bytes. fmt: 6 = DDS, 2 = 1555, 0 = 8888 |

## GUI

| Qué | Dónde |
|---|---|
| Proyección ortográfica de la GUI | `PGUIRenderer!PGUITRDisplayContext::BeginScene`. Escala en `+0x224 / +0x228`; pila de matrices en `+0x230 + idx*0x40`, `idx` en `+0x22c`; SetWorld vía `(+0x21c)->vtbl[0x7c]` |
| Widgets | `TGUIWidget`: `+0x6c` x, `+0x6e` y, `+0x70` ancho, `+0x72` alto (shorts); flags `+0x88` (`&4` = visible). Hijos: primero en `+0x50`, centinela `+0x44`, siguiente en `+0xc` |
| Dibujo de pantalla | `TGUIScreen::Paint(TGUIWidget*)` (export) |
| Menús emergentes | `PGUICore!PGUIMenuDialog::UpdatePosition` y `PGUIDiscMenuDialog::UpdatePosition`, limitados a `[0, ancho de pantalla]` |
| Vistas 3D en la GUI | `AGUIRenderObject::SetViewRectangle 0x678a90`: rectángulo GUI * escala `+0x224 / +0x228` |
| GUI ↔ render | `ACamera::ConvertGuiScreenToRenderScreen 0x50bd60`, `ConvertRenderScreenToGuiScreen 0x50bdd0` (stdcall, ret 8) |
| Mundo → GUI | `ARenderer::ConvertToScreenCoords(TVector3&) 0x477ec0`; devuelve (-100, -100) si el punto está detrás de la cámara |
| Picking | `APointerController::RequestTargetPosition 0x51d800` → `ConvertGuiScreenToRenderScreen` → `ARenderer::ConvertScreenCoordsToWorldCoords` |
| Layout de la GUI | `Data\GUI\main.gui` (texto plano), con propiedades `xpos/ypos/alignx` |

## Otros

- Videos: `Data\Movies\*.bik` (Bink 1, 640x448). El menú es `JPGtloop.bik`.
- dgVoodoo 2.87 (`D3D8.dll` en la carpeta del juego) traduce a D3D11. Se configura en `dgVoodoo.conf`.
- `SimJP.exe` no trae Large Address Aware; el bit `0x20` de Characteristics se puede activar.
