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

The domain shader also writes the quadratic PN normal (the mid-edge normals of Vlachos et al., computed in the
hull shader), so lighting follows the curved surface, not the original facets.

## Displacement

With `Displacement > 0`, the domain shader pushes each generated vertex along that normal by a
height read from the stage-0 colour texture (bound to the domain shader for the draw): the
luminance at `DetailMip` minus the luminance four mips coarser, bright raised (`InvertRelief=1`
for the opposite). This gives real geometry to folds, ridges and muscles; individual scales are
far smaller than a sub-triangle and are left to the normal maps and docs/RELIEF.md. `SeamSafe=1`
fades the displacement to zero at the original triangle edges: triangles meeting at a UV seam
read different heights, and moving those shared edges would open cracks.

The hull/domain stages are bound for that single draw only and unbound right after, before the
shadow-map redraw, which keeps the original flat geometry.

## Limits

- Vertices duplicated with different normals (hard edges) can show thin cracks along them.
  Dinosaur skins share normals across UV seams, so this is rare on them.
- Higher-resolution models still work: dense meshes produce short edges, so they get few or no
  subdivisions automatically.
- `FlipWinding=1` if tessellated models look inside out on some GPU/driver.
