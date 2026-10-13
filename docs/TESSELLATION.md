# Tessellation (PN triangles)

Rounds the silhouette of low-poly models on the GPU with Direct3D 11 hull and domain shaders,
without touching the game's models. `[Tessellation]` in `d3d8to11.ini`, also in the Insert panel.

## What is tessellated

Main-view skinned draws (world kind 1) drawn as triangle lists, with the `[Tessellation]` switch
and the master effects switch on. With `DinosOnly=1` (default) only draws whose stage-0 texture
comes from a `dinos_*` library are tessellated; the plugin reports texture names through
`D3D8to11_SetTextureName` (see docs/NORMAL_MAPS.md). Shadow-map, offscreen and HUD draws are
never tessellated. Dinosaurs are drawn as indexed triangle strips: those are expanded to a
list on the CPU for the tessellated draw (the shadow redraw keeps the strip). Fans are skipped.

## How

The translated vertex shader is unchanged apart from emitting the skinned world normal
(`sunControl.z`), compiled as `vs_5_0` on feature level 11. Each triangle becomes a
3-control-point patch:

- Hull shader: PN-triangle control points (Vlachos et al. 2001) from the corner world positions
  and normals, scaled by `Curvature`. Edge factors come from each edge's own screen length
  (`PixelsPerEdge`, capped by `MaxFactor`), so adjacent patches agree and no cracks open between
  them where vertices are shared. Compiled without optimization: fxc's optimizer emits invalid
  fork/join phases for these patch constants (error X8000).
- Domain shader: interpolates every VS output, evaluates the cubic PN surface in world space and
  adds its offset from the flat triangle to the interpolated clip position through the camera
  view-projection, so the game's own projection and the TAA jitter are kept.

The hull/domain stages are bound for that single draw only and unbound right after, before the
shadow-map redraw, which keeps the original flat geometry.

## Limits

- Vertices duplicated with different normals (hard edges) can show thin cracks along them.
  Dinosaur skins share normals across UV seams, so this is rare on them.
- Lighting normals are interpolated linearly; only the shape is curved.
- Higher-resolution models still work: dense meshes produce short edges, so they get few or no
  subdivisions automatically.
- `FlipWinding=1` if tessellated models look inside out on some GPU/driver.
