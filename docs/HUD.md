# Native HUD — first stage

The park minimap remains the game's `AMinimap`, not an ImGui overlay. Its original
palette, transparency, camera indicator, visibility and terrain refresh are retained.
The helicopter radar and the full tactical map are untouched.

`JPOGMouseFix.ini`, `[HUD]` (restart required):

- `EnhancedMinimap=1`: higher-resolution native map, lower-right anchor.
- `MinimapSize=128`: supported 64/128/256 GUI units and real texture pixels.
- `MinimapMargin=16`: inset from the right edge.
- `MinimapBottom=56`: space reserved above the native mail button.
- `EnhancedMinimap=0`: no minimap hooks; original size/position on next launch.

The map allocates its normal texture at the selected resolution, rather than
stretching the old 64-pixel image. Terrain sampling changes from shift 4 to 3
(128) or 2 (256), preserving the original 1024-cell coverage. Dimensions are
changed only after construction, before Init/CreateTexture allocates buffers.
Changing the dimensions of an already initialized map would corrupt its strides.
Placement updates use the existing GUI widescreen extension. Native GetPosition,
GetBBox, paint and camera-marker placement read the same coordinates.

## Offline native evidence

REA/Ghidra 12.1.4, SimJP.exe SHA256
`ea043f1ea354dbf2848fcd76ca5739339b773a9913f6f68afdfa94bafad03b05`:

- Constructor `0x51cac0`: width/height 64 at +0x26/+0x28, sampling shift 4 at
  +0x2a, unsigned x/y 60/290 at +0x3a/+0x3c.
  Evidence `ev_e9fb5e63c61c1c5c90828eac2e8cd2bfb4141f374c244c424fffd6eb41e350c7`.
- SetSize `0x482fc0` is a no-op. GetBBox `0x482ff0` reads the same fields.
  Evidence `ev_c9691224b94fd71c483b26942bea92aa3091779a2614cba12279e9c93853629e`.
- CreateTexture `0x51d3b0` allocates width*height*2 and Refresh `0x51cdc0`
  samples terrain at pixel coordinates shifted by +0x2a.
  Evidence `ev_25f8424df1592ef0bf873cc6c7103cf8288f12d9546282202a13e0cbafd02ad2`,
  `ev_a592dde716314c18d401d0e09037040b20aae455ee15f8694e1a82972f6ea2d1`.
- Paint `0x51d0e0` draws the terrain and camera marker using these dimensions
  and coordinates; SetPosition `0x482fd0` writes the two unsigned shorts.
  Whole-instruction assembly was checked before making the trampolines.
  Evidence `ev_a79055742eca42b8c5cc22f66d0d28865f1409098dc8c4d7515ced7292b7c196`.

Exports and prologue signatures gate installation. This stage does not replace
the whole HUD or add time/speed buttons. Runtime placement and visibility still
need the user's game test; no game was launched during development.
