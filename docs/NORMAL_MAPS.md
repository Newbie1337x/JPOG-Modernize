# Normal maps

Put `<library>__<texture>_n.dds` next to a texture in `mods\textures` (any subfolder; the
name is the texture's name plus `_n`). It works for HD replacements and for original textures
(no replacement needed). `[NormalMaps]` in `d3d8to11.ini` (or the Insert panel) controls it.

## Generating maps

```
python tools\make_normals.py "<game>\mods\textures" --skip plants,particles,people,HUD_Remaster_4x,Fuentes_y_atlas_4x
```

Height is estimated from luminance with the large-scale brightness removed (bright = raised),
gradients become normals, output is BC5 (ATI2) with mipmaps, capped at 1024 px (`--max`),
`--strength` scales the relief, `--force` overwrites. It is a stylised relief; maps from better
tools (Materialize, DeepBump, hand-made) can replace any file. Supported formats: BC5, BC3, BC1,
32-bit, DX10 BC5. Green follows the DirectX convention (+v, down the image); OpenGL maps need
`FlipGreen=1`.

## How it works

1. `JPOGMouseFix.asi` scans `mods\textures` for `_n` files and stores them under the hash of the
   original texture bytes (the same key used for replacements). Right after the game creates
   that texture (D3DX DDS path or raw 5551/4444/8888 path) it calls
   `D3D8to11_SetNormalMap(texture, path)` exported by our `d3d8.dll`.
2. `src/d3d8to11/normalmap.cpp` loads the DDS into an immutable texture kept on the `Texture`.
3. For world draws whose stage-0 texture has a normal map, the map is bound at t9 and the pixel
   shader variant (`PSVariant::normalMap` / `FFKey::normalMap`) adds `ApplyNormalMap`:
   - JPOG meshes have no tangents: the frame comes from screen-space derivatives of the world
     position and of `t0` (cotangent frame from the derivative-consistent normal
     `cross(dp1, dp2)`); only the normal component is turned toward the camera.
   - The game's lighting is kept: the colour is scaled by
     `1 + strength * (max(N.L, 0) - max(Ng.L, 0))` with the sun direction, plus a slight cavity
     term, clamped to 0.35..1.7.
4. The camera position for the facing test comes from the inverse view-projection
   (`SunConstants::camera`), `camera.w` carries the strength (negative = flip green).

Only stage 0 is used (rigid meshes, dinosaurs, buildings). Terrain layers are not covered yet.

## Test

A synthetic 64x64 BC5 map tilted toward +u on one half and -u on the other, attached to the
test caster texture with the sun toward +x: the half facing the sun goes 128 -> 140, the other
128 -> 71 (strength 0.8).
