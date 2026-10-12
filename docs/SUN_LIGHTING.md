# DX11 sun shadows

Enable with `[Lighting] Enabled=1` in the game's `d3d8to11.ini` (restart required).
`Enabled=0` gives the original image. The game's own lighting is kept; shadows only
darken it.

## How it works (`src/d3d8to11/sun.cpp`)

JPOG has no world/view/projection split: every world vertex shader receives one
world-to-clip (or model-to-clip) matrix in `c0..c3`. The terrain shader (`kind 2`,
v0 `D3DCOLOR` height + v1 grid position) adds only a chunk offset (`c4`) to its
vertices, so its `c0..c3` is the camera's view-projection.

1. **Frame start** — the first full-screen terrain draw inverts its `c0..c3`
   (`clipToWorld`). Rays through a 3x3 grid of screen points are intersected with the
   chunk's base height and clamped to `ShadowDistance`; an orthographic light box is
   fitted around those points in light space, its size rounded to 32 units and its
   centre snapped to whole texels (no shimmering while panning). The shadow map is cleared.
2. **Casters** — every opaque world draw (depth writes on, full-screen viewport, back
   buffer target) is drawn a second time into the shadow map. The translated vertex
   shader maps its own clip position back through `clipToWorld`, so skinned dinosaurs,
   rigid meshes and terrain get world positions without knowing their model matrices.
   Alpha-tested foliage runs its pixel shader up to the alpha test. Grass is excluded
   unless `GrassShadows=1`.
3. **Resolve** — before the first draw without depth to the back buffer (the HUD), before
   a depth clear, or at Present, a full-screen pass reads the scene depth (the depth buffer
   is created typeless), rebuilds world position and a depth-derived normal (per axis the
   neighbour on the same surface), offsets along the normal and takes 12 Poisson PCF taps.
   Surfaces facing away from the sun are left alone (the game already darkens them);
   shadows fade at the light box edge and towards `ShadowDistance`. The back buffer is
   multiplied by the result (`ShadowStrength`, slight blue tint).

Everything happens in the same frame: no latency, no dependence on the previous camera.

## Sun direction

`SunSource=game`: the skinned shader lights with `N . -c89`, with N in the space that its
`c0..c3` maps to clip. `-c89` mapped through `c0..c3` and `clipToWorld` is the game's sun
in world space; it is smoothed over frames and used from the next frame on. Until a
dinosaur has been drawn, `SunX/Y/Z` is used. `SunMinHeight` raises a low sun (keeping its
azimuth) to avoid endless shadows. `SunSource=fixed` always uses `SunX/Y/Z`.

## Diagnostics

`d3d8to11.log` reports once:

- `Sun frame:` ground height, light box size and texel size, sun direction and source.
- `Game sun (skin c89)`: the derived direction, the 4th column of the reconstructed model
  matrix (should be 0 0 0 1; anything else means the skin camera differs from the terrain
  camera) and the terrain shader's `c22` for comparison.
- `Sun shadows resolved at hud|clear|present`.
- `Sun frame order`: casters drawn before the terrain (not in the map) and world draws after
  the resolve (not shadowed). Non-zero values point at objects without shadows.

`DebugView=3` shows the shadow mask instead of the image (sky magenta), `DebugView=4` the
reconstructed normals.

## Tests

`tools/suncheck.cpp` renders a terrain quad, a floating caster and a HUD patch with captured
JPOG shaders on a hidden 64x64 device and checks, in the same frame, that the shadow lands
9 units away from a 0.6/0.8 sun, follows the caster when its matrix moves it, does not touch
the caster's top and leaves the HUD unshadowed.

## Limits

- Objects outside the game's view frustum are culled by the game and cast nothing.
- World draws before the first terrain chunk are not in the map (see `Sun frame order`).
- Transparent surfaces (water, particles) are shaded by the depth behind them.
- The light box assumes the chunk base height as ground; very hilly views may clip.
