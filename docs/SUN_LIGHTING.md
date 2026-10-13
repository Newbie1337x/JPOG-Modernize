# DX11 sun shadows

Enable with `[Lighting] Enabled=1` in the game's `d3d8to11.ini` (restart required).
`Enabled=0` gives the original image. The game's own lighting is kept; shadows only
darken it.

## World conventions

- **JPOG's world is Y-down: "up" is -Y.** Verified from a frame dump: camera at y = -55,
  palm crowns at y = -6 over ground at y = -1. Every "is the sun above?" test uses -Y.
  The ini's `SunX/Y/Z` are Y-up for readability and Y is negated internally.
- JPOG has no world/view/projection split: every world vertex shader receives one
  world-to-clip (or model-to-clip) matrix in `c0..c3`. The terrain shader (`kind 2`,
  v0 `D3DCOLOR` height + v1 grid position) adds only a chunk offset (`c4`) to its
  vertices, so its `c0..c3` is the camera's view-projection (near ~1, far ~470).
- Draw kinds (`WorldShaderKind`): 1 = skinned shader, used by dinosaurs **and** trees,
  buildings and most props; 2 = terrain; 3 = rigid (fences, water); 4 = grass.
- Frame order: an offscreen 256x256 pass of the park (own depth buffer), then the main
  world (terrain, kind 1, grass, water), a depth clear, then the HUD.

## How it works (`src/d3d8to11/sun.cpp`)

1. **Frame start** — the first full-screen terrain draw inverts its `c0..c3`
   (`clipToWorld`). An orthographic light box is fitted around the view frustum cut at
   `ShadowDistance` (near corners plus corner rays clamped to that distance), its size
   rounded to 32 units and its centre snapped to whole texels (no shimmering while
   panning). The shadow map is cleared. (An earlier fit against the chunk's base height
   put the box beside the view when zoomed in: terrain heights are added to that offset.)
2. **Casters** — every opaque world draw (depth writes on, full-screen viewport, back
   buffer target) is drawn a second time into the shadow map. The translated vertex
   shader maps its own clip position back through `clipToWorld`, so skinned dinosaurs,
   rigid meshes and props get world positions without knowing their model matrices.
   Alpha-tested foliage runs its pixel shader up to the alpha test. Grass
   (`GrassShadows`) and terrain (`TerrainShadows`) are excluded by default: terrain
   self-shadowing produced acne that grew with zoom, and the game already shades slopes.
3. **Resolve** — before the first draw without depth to the back buffer (the HUD), before
   a depth clear, or at Present, a full-screen pass reads the scene depth (the depth buffer
   is created typeless), rebuilds world position and a depth-derived normal (per axis the
   neighbour on the same surface), offsets the sample toward the sun (robust on noisy
   grass/foliage normals) and takes 12 Poisson PCF taps. Faces turned away from the sun
   get at least 60% of the shadow (depth normals under grass are unreliable). Shadows fade
   at the light box edge and towards `ShadowDistance`. The back buffer is multiplied by the
   result (`ShadowStrength`, slight blue tint).

Everything happens in the same frame: no latency, no dependence on the previous camera.

## Sun direction

`SunSource=game`: the skinned shader lights with `N . -c89`, with N in the space that its
`c0..c3` maps to clip. `-c89` mapped through `c0..c3` and `clipToWorld` is the game's sun
in world space (e.g. `0.47 -0.74 0.47`, i.e. above the park); it is smoothed over frames
and used from the next frame on. Until a skinned mesh has been drawn, `SunX/Y/Z` is used.
`SunMinHeight` raises a low sun (keeping its azimuth) to avoid endless shadows.
`SunSource=fixed` always uses `SunX/Y/Z`.

## Diagnostics

`d3d8to11.log` reports once:

- `Sun frame:` light box size and texel size, sun direction and source.
- `Sun camera viewport`: viewport depth range and the camera's z/w rows.
- `Game sun (skin c89)`: the derived direction and the 4th column of the reconstructed model
  matrix (should be 0 0 0 1; anything else means the skin camera differs from the terrain
  camera).
- `Sun shadows resolved at hud|clear|present`.
- `Sun frame order`: casters drawn before the terrain (not in the map) and world draws after
  the resolve (not shadowed).
- `Frame trace written`: 1000 frames after the first shadowed frame, the draw/clear/render
  target sequence of one shadowed frame goes to `d3d8to11_frame.log`.

`DebugView` (restart required):

| value | shows |
|---|---|
| 3 | shadow mask instead of the image (sky magenta) |
| 4 | reconstructed normals |
| 5 | mask plus the shadow map (contour bands, blue = empty) in the top-left corner |
| 6 | red 8-unit grid on reconstructed world positions (must stay glued to the ground) |
| 7 | raw sun visibility without facing/fade |
| 8 | normal mask; ~600 frames in, dumps shadow map, depth, colour and matrices once to `d3d8to11_dump\` |

`python tools/sundump.py <game>\d3d8to11_dump <out>` recomputes visibility on the CPU and
writes `vis.png`, `gap.png` (how far the stored caster is in front of each receiver) and a
crop of the shadow map. That analysis found the Y-down convention.

## Tests

`tools/suncheck.cpp` renders a terrain quad, a floating caster and a HUD patch with captured
JPOG shaders on a hidden 64x64 device (Y-down, top-down camera) and checks, in the same
frame, that the shadow lands 9 units away from a 0.6/0.8 sun, follows the caster when its
matrix moves it, does not touch the caster's top and leaves the HUD unshadowed.

## Limits

- Objects outside the game's view frustum are culled by the game and cast nothing.
- World draws before the first terrain chunk are not in the map (see `Sun frame order`).
- Transparent surfaces (water, particles) are shaded by the depth behind them.
- One map for the whole view: about 0.07-0.1 units per texel at `ShadowDistance=250`.
  Cascades would sharpen close-ups.
