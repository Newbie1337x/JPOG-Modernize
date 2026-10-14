# Replacement lighting

## Calibrated relighting (current)

The replacement now starts from the game's own light, read per draw from the vertex shader
constants, instead of invented colours:

| Kind | Game light (vs_1_1) | Replacement |
| --- | --- | --- |
| 1 skinned (dinosaurs, trees, buildings, props), vs_000/vs_001 | `oD0 = (max(N . -c89, 0) * c91 + c90) * c88` | ambient `c90*c88`, sun `c91*c88`, our N and sun direction |
| 2 terrain, vs_005 | `r9 = v0.w * c7 + c6` (v0.w: baked per-vertex sun factor) | ambient `c6`, sun `c7`, factor `v0.w` (VS passes it in t7.x) |
| 3 rigid (paths), vs_002 | `oD0 = vertex colour * c4` (pre-lit with the terrain's light) | the last terrain draw's `c6`/`c7`; sun factor recovered from the vertex colour's brightness, the remainder kept as tint |
| 4 grass, vs_010 | terrain colour map in t1 | unchanged |

Lighting is evaluated in linear light (gamma 2.2): ambient `A^2.2`, sun `sat(A+S)^2.2 - sat(A)^2.2`
(the game's vertex colour output is clamped to 1, so its brightest light is `saturate(A+S)`; without
that clamp the terrain came out far too bright). Full sun
and full shade therefore reproduce the original colours exactly; intermediate angles are summed
physically. The ambient gets a sky/ground tint (`RelightSkyTint`: bluish on faces looking up, warm
bounce on faces looking down). The terrain keeps the game's distance fade of its lighting under
haze (`oD1.w`). The result goes through `RelightExposure` and, with `RelightTonemap=1`, a
highlight-only shoulder above 0.8 (the hue-preserving shoulder of Khronos PBR Neutral, without its
toe) so everything the game kept below 1 is unchanged. The fog lerp is applied afterwards, as the
game does. With the replacement on, the material pilot also uses these light colours and output.

### Terrain height field

With `HeightfieldLighting=1` the terrain and the paths no longer use the baked sun factor: their
normal comes from the game's own terrain height field, read through the exported native accessors
also used by the water (`native_terrain.h`: `THeightField::GetHeight`, one sample per world unit,
height up = -renderer Y). `terrain_light.cpp` scans it incrementally (16384 samples per frame),
uploads it as an R32 texture (t15, sampler s11) and rescans every 3 s so terraforming shows up.
The pixel shader takes central differences for a smooth normal at any sun direction and marches
up to ~300 units toward the sun for horizon shadows (`HorizonShadows`: hills shade the terrain
behind them). The game's baked factor also contains painted darkening (under trees, in hollows):
its ratio to the field normal's N.L under the same sun is kept as occlusion (`BakedTerrainDetail`),
so with the game's sun the terrain matches the baked look and a moved sun keeps that detail; the
smaller of it and the horizon shadow is used. Paths keep the tint recovered from their vertex colour. Outside the field, or until
the first scan completes, the baked factor is used. Debug view 2 shows the resulting normals.

Known limits: cast shadows still come from the shadow resolve multiplying the whole colour, not
only the sun term. Grass keeps the terrain colour map's baked light.

## Earlier experimental stage

Grass correction: preserve its second texture RGB. That channel contains
terrain material colour as well as legacy lighting; whitening it made blades
look pale/dry. This is a partial correction, not full separation of grass
albedo and baked lighting. See WATER.md for the subsequent water test build.

The user's goal is replacement of the game's world lighting, including stable
cast shadows and eventually indirect illumination. The former shadow modulation
pass did not meet that goal. This revision implements replacement **direct and
hemispherical ambient lighting**, not the complete goal.

## Configuration

Under [Lighting], Enabled=1 and ReplaceOriginal=1 select the new path.
Enabled=0 restores the original world lighting. Cast shadows are no longer part of
this path: with Enabled=1 they come from the same-frame shadow resolve described in
SUN_LIGHTING.md, which works with or without ReplaceOriginal. The earlier
`CastShadows` key (previous-frame shadow maps) was removed.

Without ReplaceOriginal (the default) the game's baked lighting is kept: terrain
brightness comes from the height vertex's w channel (VS `r9 = v0.wwwx * c7 + c6`),
rigid meshes are pre-lit through their vertex colour, and only skinned meshes are lit
per vertex (`N . -c89`). Replacement discards all of that, so flat derivative normals
on rigid meshes show facets; treat it as a research path.

SunX/Y/Z specify the direction toward the sun, normalized by the renderer.
Ambient controls hemispherical ground/sky light, SunIntensity the directional
light. DebugView=1 displays extracted base colour, 2 world normals, 0 shading.
Changes require restart. With SunSource=game the direction follows the game's own
light (see SUN_LIGHTING.md); SunSource=fixed uses SunX/Y/Z.

## What changes

Captured shaders were disassembled with tools/disasm_shaders.py and D3DDisassemble:

- Skin VS001 computes bone-weighted positions and r6 normals, then original
  directional/ambient light in oD0. PS001 mixes t0/t1, multiplies by v0 and
  interpolates fog using v1.w.
- Terrain VS005 computes height-grid positions and vertex colour. PS004
  blends three texture layers, applies v0 RGB and then fog.
- Rigid PS003 multiplies the diffuse texture by vertex colour before fog.
- Grass PS014 multiplies a blade texture with a terrain colour texture.

For eligible world draws, replacement mode removes diffuse vertex RGB and
additive vertex lighting from the material calculation, preserves their alpha
channels, and defers the recognized material fog lerp until after new lighting.
Terrain layer weights and dinosaur texture transitions remain intact.
Grass retains its second texture's alpha but removes its RGB modulation.
Fixed-function world materials are eligible only when depth writes are enabled.
HUD, offscreen render targets and unrecognized world paths retain old behavior.

Lighting uses approximate gamma decoding/encoding (power 2.2), a warm Lambert
sun and a ground/sky hemispherical term. Skin normals are transformed from
the bone-weighted shader normal through the inverse transpose of the
model-to-world basis. Terrain and rigid meshes without provided normals use
position derivatives (flat geometric normals). Terrain normals are oriented
upward and grass uses two-sided diffuse.

## Verification and limits

54 captured shader/variant compilation checks pass. tools/relightcheck.cpp uses
the captured terrain/skin shaders on a hidden 64x64 D3D11 test surface:

- Upward skin normal: RGB 898a89.
- Camera rotated and legacy material-light multiplier changed from 0.1 to 0.9:
  same RGB 898a89.
- Inverted skin normal: RGB 3f3c36.
- Fully fogged: RGB 336699, matching the supplied fog colour.
- Managed-texture/HUD regression: ff00ff00, unchanged.

These are synthetic GPU tests, not in-game validation. They do not establish
correct extraction for every material, camera or terrain tile.

Outstanding work: correct same-frame projected shadows and offscreen casters,
indirect bounce/GI, sky/water replacement, retain artistic vertex tint separately
from baked vertex lighting, smooth normals on rigid meshes, and textures that
already contain painted or precomputed light. Shared camera extraction and
material classification still require validation in the real game.

Do not launch or close SimJP automatically. The user performs the game test.

## Time-of-day preview

Insert panel, Sun direction: "Time of day (preview)" and an Hour slider (not saved). The sun
crosses a tilted arc (east at 6 h, 70 degrees at noon, west at 18 h); below the horizon the moon,
opposite the sun, becomes the light. `EvaluateTimeOfDay` (sun.cpp) derives per hour:

- light colour (golden low sun, white noon, silvery full moon) and ambient colour (blue sky at
  dusk and night), as multipliers on the game's own light colours;
- eye adaptation: exposure raised up to 2.2x when the light is low or grazing;
- shadow depth from the direct light's share of the total away from noon, and a cooler shadow tint;
- rim light on skinned models (silhouettes outlined by the moon or a low sun);
- a tint for surfaces that are not relit (grass, water, other world draws);
- the sky: the game's sky texture by day, a procedural sky at dusk and night (horizon glow toward
  the sun, stars, moon disc and halo), composited in the shadow resolve on sky pixels.

Not yet: lights at night (lamps, windows), relit grass and water, clouds.
