# Experimental DX11 sun lighting

Enable `[Lighting] Enabled=1` in the game's `d3d8to11.ini`. Set it back to 0
to restore the original lighting. The shipped default remains disabled pending
in-game verification.

This implementation adds a sun depth shadow map with 3x3 PCF. JPOG already
lights its colors, so shadow composition preserves the original RGB exactly
where visibility is 1. Ambient and SunIntensity control the fraction attenuated
by a shadow rather than applying a second lighting/tint pass over the game.
It is not global illumination: there is no indirect bounce calculation.

Recognized vertex declarations select skinned models, terrain, rigid geometry
and grass. The first recognized main-world terrain draw establishes the shared
camera from c0. All recognized shaders reconstruct world position from their
original clip position through that camera's inverse, so model-local transforms
in a mesh's c0 do not place its shadow at a local-space origin. Geometric normals
are derived from screen derivatives of this reconstructed position.
Shadow receivers and casters are distinct: main-world terrain can receive
shadows when blending is enabled or depth writes are disabled; only eligible
depth-writing draws cast. UI, offscreen texture generation and water are excluded.
These rules still require validation against all game materials. Material
eligibility is logged for each VS/PS pair.

Each eligible draw is repeated into a sun-view depth texture using its original
animation, textures and alpha-test logic. A dedicated depth-only pixel shader
avoids a color target. Two maps alternate: the current frame builds one map while
sampling the completed preceding frame. Consequently animated shadows lag by
one frame. Only geometry submitted in the camera's visible world pass casts
shadows; off-camera casters and cascaded shadow maps are not implemented yet.

The shadow camera follows the terrain camera's center ray intersected with a
ground plane; models before that terrain pass are left unchanged. The sun
direction is currently fixed in world space, and does not
follow the game's time of day. The shadow extent and ambient/direct strengths
are configurable. Invalid camera matrices disable lighting for that draw.

Verification on Radeon RX 6700 XT:

- All 14 captured vertex shaders, 18 pixel shaders, 18 depth variants and four
  fixed-function variants compile successfully: 54 passed.
- A GPU readback test uses the captured terrain shader with blending enabled
  and depth writes disabled, plus an elevated model with translation in its c0.
  The model casts at the correct world-space offset. Lit
  pixels remain exactly 128 before and after shadow-map completion; the selected
  occluded pixel changes from 128 to 79 with the test's 0.9 shadow strength.
- The partial managed-texture update / HUD regression remains green (ff00ff00)
  with lighting enabled.
- The synthetic shadow scene reports no D3D11 debug-layer diagnostics.

These tests verify the mechanism, not its visual correctness in every JPOG
scene. Validate camera movement, dinosaur animation, terrain, foliage, UI and
performance in-game before enabling by default.
