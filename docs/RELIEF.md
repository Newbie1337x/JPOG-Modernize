# Skin relief (parallax + self-shadowing)

Per-pixel depth for dinosaur skins: scales, folds and wrinkles shift with the view angle and
cast small shadows on their neighbours away from the sun. `[Relief]` in `d3d8to11.ini`, also in
the Insert panel. Complements the tessellation displacement (docs/TESSELLATION.md), which moves
real geometry for the broad shapes; this handles detail far finer than any triangle.

## What gets it

Main-view skinned draws (world kind 1) with a translated pixel shader whose stage-0 texture comes
from a `dinos_*` library (names reported by the plugin, see docs/NORMAL_MAPS.md). Shadow-map,
offscreen and fixed-function draws are untouched.

## How

There is no height map in the game data, so the height is the skin texture's local luminance:
the displayed mip minus a mip three levels coarser (bright = raised, `Invert=1` for the opposite).
At the start of the pixel shader:

- A tangent frame is built from screen-space derivatives (no tangents in JPOG meshes), as for the
  normal maps. The displayed mip is estimated from the same derivatives, and every step is taken
  in texels of that mip so the effect stays stable with distance.
- Parallax: offset-limited, three refinements; the texture coordinate of stage 0 is shifted toward
  the viewer by the height. `Parallax` scales it (0 = shadows only).
- Self-shadowing: eight steps toward the sun in tangent space; a texel is occluded where the height
  field rises above the ray. `Softness` is the penumbra width in texels, `SelfShadow` the darkening.
  It fades out at grazing sun angles, where the game's own shading takes over.

The shadow factor multiplies the final colour after the game's lighting or the material pilot.
Debug view 12 ("Skin relief self-shadow") shows the factor alone.

## Parameters (`[Relief]`)

| Key | Default | Meaning |
| --- | --- | --- |
| Enabled | 1 | Master switch |
| Depth | 3 | Height range in texels |
| Parallax | 0.5 | View-dependent shift, 0..1 |
| SelfShadow | 0.6 | Shadow darkness, 0..1 |
| Softness | 1 | Penumbra width in texels |
| Invert | 0 | Dark raised instead of bright |

## Limits

- Luminance is only an estimate of height: painted dark spots read as dents.
- The parallax shift can sample across UV island borders at very low angles; keep `Parallax` low
  if seams show colour from a neighbouring island.
