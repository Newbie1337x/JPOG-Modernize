# Replacement lighting: experimental first stage

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
