# D3D8 → D3D11 layer: findings and plan

## What the game uses (profiler, ~41,700 frames of real gameplay)

Captured with `[Debug] ProfileD3D8=1`; shaders disassembled with `tools/disasm_shaders.py`.

### Pipeline
- **All 3D geometry uses programmable vertex shaders.** There are 11 `vs_1_1` programs (handles 0x3 to 0x17). `SetTransform` is only used for VIEW/PROJECTION (sprites/UI), and `SetLight` is never called. Lighting and fog are computed in the vertex shaders: Hoffman–Preetham style atmospheric scattering with Henyey-Greenstein phase, in-scattering and extinction.
- **Pixel processing mixes two paths:**
  - 15 unique `ps_1_1` programs, used in ~42% of `SetPixelShader` calls. They include terrain texture splatting (`lrp` by alpha), alpha-tested foliage (`texkill`), the water reflection (`texm3x2pad/texm3x2tex`, i.e. bump-env reflection) and a render-to-texture water normal-map simulation (ps 10–13).
  - Fixed-function texture stages for the rest. Stages 0–3 are used, with bump env-map matrices on stage 1 (TSS 7–10).
- **Draw calls:** about 865 per frame, almost all `DrawIndexedPrimitive`.
  - Triangle strips: 33.5M calls; triangle lists: 2.6M.
  - `DrawPrimitive` fans (117k) for the GUI.
  - No `*UP` draws and no `ProcessVertices`.
- **Vertex streams:** up to 2 streams. Strides seen: 40 (main meshes), 24, 20, 8 and 4.
  - Terrain: stream 0 is a `D3DCOLOR` height/normal word, stream 1 is a `FLOAT2` grid position.
  - Grass/flora: a second stream with a `FLOAT1` per-instance value.
- **Constants:** up to c95. c20–c83 are 3-register slots, most likely skinning matrices, 22 bones.

### Resources
- **Textures:**

  | Format | Code | Created |
  |---|---|---|
  | A8R8G8B8 | 21 | 635 |
  | X8R8G8B8 | 22 | 2 |
  | R5G6B5 | 23 | 2 (render targets) |
  | A1R5G5B5 | 25 | 17 |
  | A4R4G4B4 | 26 | 2 |
  | DXT1 / DXT3 / DXT5 | | 229 / 272 / 155 |
  | Volume textures | | 4 (scattering/fog lookups) |

- **Render targets:** around 5 `SetRenderTarget` calls per frame (water simulation, reflections, 3D views in the GUI). 2 depth-stencil surfaces.
- **Buffers:** dynamic write-only VB/IB in `D3DPOOL_DEFAULT`, recreated often (~13.6k VB and ~12.3k IB over the session), plus a few managed ones. Indices are 16-bit.
- **Other:**
  - `SetGammaRamp` every frame (brightness/contrast/gamma options).
  - Only 2 state blocks.
  - `SetRenderState` about 1,500 calls per frame.

### Render states in use
Z enable/write/func, cull mode, alpha blend (src/dest, `BLENDOP`), alpha test (ref/func), linear table fog (color/start/end), color write mask, plus a few lighting/clipping toggles. The full list of value sets is in the profiler output.

## Architecture

The layer is a `d3d8.dll` replacement implementing `IDirect3D8` / `IDirect3DDevice8` on top of D3D11. We only implement what is listed above; anything else asserts and logs.

1. **Device and resources:** textures (all formats above, DXT stays compressed), volume textures, VB/IB with dynamic-discard semantics, render targets and depth surfaces, viewport, `Clear`, `Present`, `Reset`, gamma ramp (applied in a final pass).
2. **State:**
   - Render states map to cached D3D11 blend, depth-stencil and rasterizer state objects.
   - Alpha test and fog are emulated in the pixel shader.
   - The texture stage states form a key for generated fixed-function pixel shaders.
3. **Shader translation:**
   - `vs_1_1` and `ps_1_1` token streams are translated to HLSL and compiled with `D3DCompile` at runtime, then cached.
   - The instruction sets are small; `ps_1_x` modifiers (`_bx2`, `_bias`, `_x2/_x4/_d2`, saturation, `texkill`, `texm3x2*`) are emulated explicitly.
   - Constants are mirrored into constant buffers: 96 vertex shader registers, 8 pixel shader registers.
4. **Hand-written replacements:** since there are only 11 vertex shaders and 15 pixel shaders, each one can later be replaced by a hand-written HLSL version keyed by bytecode hash. That is where per-pixel lighting, shadow mapping and other improvements will go.
5. **Shadows (later):**
   - Identify terrain, skinned and static draws by vertex shader handle.
   - Render a depth pass from the sun, then sample it in the replacement shaders.

## Milestones
1. Device, resources and `Present`: menus and the GUI render (FF pixel path plus the GUI vertex shader).
2. Shader translator: the park renders identically to D3D8.
3. Water and render targets: water simulation and reflections.
4. Feature parity, then the first replacement shaders (per-pixel sun lighting).
5. Shadow maps.
