# Water: initial depth-aware test build

## Creature interaction: first stage

Interaction=1 and InteractionStrength control expanding ripple pulses and
trails. The Insert Water panel includes the controls and an interaction debug
view (Water DebugView=4). Creature capture works even with grass interaction
disabled. This reuses CPU skinning of sampled vertices and the shared world
camera; it does not identify named feet or native actor IDs.

At the world scene resolve, up to 32 approximate body footprints are associated by nearby
predicted position. Low-pass horizontal velocity controls ripple amplitude and
emission cadence. Up to 64 pulses persist for 3.2 seconds, decay, and expand.
Moving footprints leave a trail; stationary footprints do not emit repeated
pulses. Implausible jumps are treated as new tracks rather than fast motion.
The next rendered water frame reads the pulses (one-frame observation latency).
JPOG can clear main depth before Present, resetting scene-camera validity.
Capturing at Present therefore lost all contacts in real game frames. Capture
now happens before the resolve marks the scene finished and before the clear.
The GPU wet/dry actor test includes this clear-before-HUD ordering: the previous
DLL produces zero wet interaction and fails, while the corrected DLL produces
wet ripples and passes, including MSAA 8x. Actual in-game verification is pending.
The shader rejects a pulse where its saved lowest body height lies more than
0.10 world units above that water surface. Pulses alter the continuous normal
field, so Fresnel, reflection lookup and refraction react to them.

Corrected an inherited footprint issue: merging body-part draws now updates the
lowest point, rather than preserving the first part's height.

Verification: renderer-independent tracker tests cover stationary/moving
contacts, teleport rejection, decay, preserved contact height and reset.
The captured six-bone synthetic GPU actor produces a measurable ripple outside
its body when submerged, and zero outside-body disturbance when dry. Both
cases pass with MSAA disabled and 8x; existing shader, tile-seam, depth, grass
colour and HUD regressions remain green.

Limits: nearby overlapping creatures can share footprints; nearest matching
can misassociate crossing animals; visibility and depth-segment order affect
capture; exact footstep timing is not established. This is approximate visual
interaction, not fluid simulation. Real droplets/splash particles, fountain
replacement, enhanced scene reflections and automatic ocean/lake classification
remain future stages. Global shoreline foam behavior is unchanged.

Enabled with [Water] Enabled=1. The Insert panel exposes absorption depth,
transparency, Fresnel reflection strength, refraction displacement and shoreline
foam. Settings can be saved/reloaded in the existing panel. Disabling restores
the original pixel shader and blending path.

Grid-seam correction: the enhanced path uses a continuous world-space gradient
field from three analytic waves. The legacy simulation's channels were not a
validated world-space normal map. Using them directly for Fresnel and distortion
exposed batch/UV boundaries. The generated normal now drives the texm3x2 reflection
lookup and Fresnel/refraction. All batches share time frozen at snapshot capture.
Ripples controls normal strength; Speed controls animation (0 freezes it).

The original captured water program is recognized by its texture-0 sample and
texm3x2pad t1 / texm3x2tex t2 pair, not by mutable shader handles. The original
reflection texture is retained; enhancement supplies a new normal field.
The disabled enhancement retains the original waves too. The enhancement is limited
to the full-size main backbuffer with its auto depth, after terrain has established
the camera and before scene effects resolve.

Before the first eligible water draw, copy the opaque depth and resolve/copy
opaque colour into separate shader-readable textures. These stay frozen across
water batches and reset on depth clear, Present, device reset or MSAA change.
The live DSV/RTV are not sampled. Depth snapshots retain MSAA samples; the water
variant uses the nearest sample. Supported variants: single sample, 2, 4 and 8.

Unproject opaque depth through the shared terrain camera and measure thickness
using JPOG's downward-positive Y. Apply wavelength-dependent absorption and
shallow/deep colour, fade to full scene transparency at zero thickness, then mix
the original reflection using a Schlick-style Fresnel factor. Small refraction
uses the continuous normal field; displaced samples in front of the water are
rejected. Shore foam is procedural and fades outside a configurable shallow band.
The result already composites the opaque scene, so it uses opaque blending.

DebugView=1: thickness / DepthScale. 2: Fresnel. 3: foam.

## Verification

58 captured shader/variant compilation checks passed, including four water
depth variants. tools/watercheck.cpp exercised shaded, disabled, thickness,
Fresnel, foam and grass-colour regression on a hidden 64x64 GPU surface, with
MSAA disabled and at 8x:

- 0.2/2/8 world-unit thickness -> grayscale 08/55/ff.
- Deep water attenuates the test green floor; repeated frame snapshots match.
- Foam appears in the shallow band and vanishes at larger depths.
- HUD remains exactly 00ff00.
- Replacement-lit grass preserves the green terrain tint (RGB 1f9d30).
- Seam regression: one surface split into two UV-inconsistent batches with
  conflicting legacy simulation textures, compared to its unsplit reference
  at a grazing angle. Previous DLL: 432 channel mismatches. Corrected DLL:
  zero mismatches with MSAA off and at 8x. This validates the synthetic seam
  case, not every real island camera or reflection texture.

These synthetic results do not verify every real water pass, floor coverage,
camera or post-effect ordering in JPOG. The user launches the game for validation.

## Current scene reflections

`[Water] SceneReflections=1` enables bounded screen-space ray tracing in the
water material, from the frozen opaque color/depth snapshot. The shared terrain
world-to-clip matrix is retained alongside its inverse. Reflected world-space
rays use the same continuous wave/interaction normals as Fresnel and refraction.
Quadratically spaced steps (8..48, default 24) cover 80 world units by default,
with five bisection refinements at a depth crossing. Hits require geometry above
the water surface and a small depth error; underwater floor/self hits are rejected.
Screen-edge/range fading, a small five-tap blur and the original reflection
fallback avoid hard black borders. No previous-camera/frame history is reused.
GUI controls expose strength, distance and steps, plus debug `Scene reflection
hits` (Water DebugView=5). SSR is still modulated by the existing Fresnel slider.
This is not planar rerendering, ray tracing or offscreen reflection capture:
objects outside the screen, hidden behind foreground geometry, or drawn after
the water snapshot cannot contribute. Ocean/pond classification is handled by the
separate native-terrain mask described below, not by SSR.

## Ocean and lagoon separation

`[Water] SurfaceMode=0` now uses a complete native terrain connectivity map,
not screen depth, camera distance, visible water bounds or a lake-size heuristic.
Terrain samples submerged below the active common water plane are flood-filled
from all map borders. Border-connected water is ocean; enclosed water is lagoon.
Outside native terrain bounds is treated as ocean. It updates after terraforming
and opening/closing a channel changes classification on the next completed scan.

The bridge resolves the game's exported AParkManager::GetTerrain and the loaded
TTerrainInterface.dll's GetHeightField/GetSizeX/GetSizeZ/GetHeight(uint,uint)
methods, on the render/game thread. It does not load engine DLLs, use absolute
addresses, hook/patch native code, write terrain, modify save files or launch the
game. Native pointer reads/calls use bounded POD-only SEH guards and finite-value
checks. Missing exports/not-yet-loaded terrain retains the ocean fallback; invalid
height reads stop native sampling and report the failure. Modes 1/2 explicitly
force all recognized water to lagoon/ocean as a manual fallback.

The shared height field is Y-up while rendering is Y-down (confirmed in native
GetHeightWithOffset). Current water height is reconstructed from an active
FLOAT3/FLOAT4 water vertex and its model-to-clip/shared inverse-camera matrices,
using the draw's first/base/min vertex rather than unrelated buffer contents.
The real water declaration can be FLOAT2 (8-byte stride), not FLOAT3. Its VS
position prefix swizzles X/Y into X/Z, scales by c8 and adds offsets before c0
projection. A bounded CPU evaluator mirrors MOV/ADD/SUB/MUL/M4x4, masks, swizzles
and negation through oPos; unfamiliar prefixes fail closed. Captured FLOAT2 water
and FLOAT3 offset prefixes have targeted regression checks. Handles are not used
to identify layouts. The October 13 all-blue report was a missing plane/readiness
failure, not evidence of ocean connectivity: the old reader rejected FLOAT2.
Camera round-off no longer restarts a partial native height scan.
Unsupported UP-only water vertex streams do not supply this plane yet.

At most 8192 native world-unit samples are read per rendered frame, with at most
1,048,576 samples overall. Full scans start every two seconds after completion;
thus large maps/low FPS add scan latency. Maps over that limit retain fallback.
An R8 ocean mask is allocated per map size and reused; one dry coast sample is
extended for foam continuity without overwriting enclosed wet samples. The mask
is sampled in world X/Z, invariant to camera motion/UV tiles. Grid resolution is
one native world unit: sub-unit channels and shore interpolation can be imperfect.

Lagoon profile defaults: foam multiplier .03, ambient ripple multiplier .25,
slower waves, weaker refraction distortion and greenish absorption tint. Ocean
keeps shore foam and uses ambient ripple multiplier 1.4. Creature interaction
gradients are added afterward, so calm lagoons still respond to dinosaurs. All
strengths and global shoreline settings remain editable. No higher geometric sea
wave displacement, actual fluid simulation or hydraulic wave propagation was added
by the initial classifier. The subsequent ocean swell update adds vertex displacement:

- Three world-space sine swells, approximately 40/21/13-unit wavelengths, share
  phase across draw batches and VS/PS. Default amplitude is .65 native units;
  `SwellLength` scales wavelengths. Lagoon displacement is zero.
- A reused R32_FLOAT native terrain texture supplies shallow-water attenuation,
  default fade depth 3 units, and the R8 connectivity mask limits it to the sea.
  Auto mode keeps displacement off until classification is ready. Manual ocean
  uses full amplitude when no native terrain is available.
- Vertices are projected after vertical displacement; analytic gradients augment
  reflection/refraction normals. Legacy mesh density is unchanged: coarse cells
  can limit silhouettes, and normals omit the derivative of shore attenuation.
  This is not Gerstner horizontal motion, tessellation or a fluid solver.
- Shore foam uses depth-space moving fronts and drifting two-scale procedural
  noise; sparse crest foam is independently adjustable (`CrestFoam`). No foam
  accumulation, spray particles or physical wave breaking is simulated.
- Insert has amplitude (0 disables displacement), wavelength scale, coast fade,
  crest strength and debug view 7 (absolute analytic swell height).
- The extra persistent terrain texture costs 4 bytes/sample, at most 4 MiB;
  it uploads only upon completed native scans, without depth readback or GPU waits.
  Existing temporal reprojection still lacks water motion vectors, so TAA can
  soften moving crests. Tune amplitude down if the native mesh appears faceted.

Swell validation (`tools/check_water_swell.bat`): 58 captured shader/variant
compilations passed, including the actual FLOAT2 water VS and MSAA water variants.
Synthetic MSAA8 debug 7 produced 00 for forced lagoons and 45 for forced oceans
across four scene resets; HUD checks passed. Updated foam profile checks produced
01 lagoon / 18 ocean. These are hidden fixtures, not visual validation on an
actual native coastline; no SimJP process was started by the tests.

Top-view correction following the user's actual coastline screenshot: the original
combined-height crest threshold created isolated white blobs, while low top-view
Fresnel left the surface shape almost unreadable. Swell now has a dominant direction
(80/15/5 weights, nearby secondary directions), a mild second harmonic and matching
analytic derivatives. Foam uses a narrow primary travelling crest rather than total
height, at reduced intensity with sparse noise breakup. Bounded, art-directed
normal-versus-flat sun lighting and a directional glint give top-view slope contrast
without camera-dependent wave phases. This does not add mesh subdivisions and must
still be visually checked at both top-down and grazing game cameras.

Coastward propagation update: the complete native height grid now also builds
an 8-neighbour, two-pass chamfer distance to dry land. This approximate distance
is uploaded to a reused R32_FLOAT t14 texture and sampled in both VS and PS.
Automatic waves use phase `-k*distance-omega*time`, so constant-phase fronts travel
to decreasing distance (towards land) and follow bays and opposite island shores.
Normals use the negative distance gradient with the same wave derivatives. Foam
fronts and shore drift also move coastwards. A varying direction dotted with world
position is deliberately avoided because it would introduce phase discontinuities.
Manual modes without a valid native mask, or a grid without any dry land, retain
the earlier dominant directional fallback. Outside-grid distance is approximated
by clamped field distance plus distance to the grid boundary. Equidistant shores
can form convergence lines; this is not a hydrodynamic refraction/breaking solver.
Distance rebuild uses two O(N) passes after a completed height scan, no per-frame
CPU rebuild, no readback. Added persistent CPU/GPU storage: 4 bytes/sample each
(up to 4 MiB each). Existing map CPU telemetry includes the rebuild work.
CPU checks cover a straight coastline, inward gradients on opposite shores and
the coastward phase sign; shader compilations and ocean/lagoon/HUD fixtures pass.
The actual island animation still requires user visual confirmation.

Insert exposes automatic/manual mode, multipliers, scan progress, sampled level,
native cell counts and last map CPU-step time. Water debug 6 is lagoon green / ocean
blue. The CSV includes mode/readiness, counts and map CPU time. The masked material
is applied once the first full scan finishes; first-scan fallback can briefly look
like the earlier ocean profile.

Validation: standalone CPU tests cover enclosed water, border-connected water,
channel changes and invalid heights. One synthetic GPU check per profile with
MSAA8 preserves the HUD and produces shoreline foam 02 for lagoon versus 3d for
ocean. Real native live-map classification still requires the user's game test.

Additional REA static observations, not runtime execution:

- SimJP.exe SHA256 ea043f1ea354dbf2848fcd76ca5739339b773a9913f6f68afdfa94bafad03b05.
- SimJPTerrain.dll SHA256 26ee1abf898fa5de132d5fc433d2e8d49bd5b51685855c7bca3ce658ad36da77.
- TTerrainInterface.dll SHA256 4961df57871f539525b95c41dc72001414e4acb23569c8df3c958e155e50e3a3.
- Native GetUIFlags uses full terrain dimensions and 8x8 subregion labels;
  IsWater means label 7, not a separate ocean ID: ev_f0ee3f41170df087270d4d95210733f920abcda3c41246bf1a36970526d659ae,
  ev_cbbf17fbe69a9ae3254dc5447d4dbc1d8ae3251ec9670b1c8dcfa8dd8a4e1081.
- Native InitializeUIFlags derives those labels from heights; labels alone cannot
  reliably distinguish ocean from ponds: ev_e6907972aeb8edd6cc0ad7288c41624a7c3755b0adfb0daa1f1897f1e4a5a509.
- GetHeightWithOffset negates height without translating world X/Z:
  ev_91ff1a6b5f24ef9aee55c05d0a1ef912538ac416dfedf7ac8b85f20d4f95ea40,
  ev_ad360784e7587ed2615ca5b5d2c4c5ae6d1bb3162a0acade9f4d1f9d76a59008.
- GetSizeX/GetSizeZ and GetHeightField export the actual shared field and dimensions:
  ev_b192638d52c8d6d5124a78a816b8f0989e221250d290f375f26077f6fba53558,
  ev_88ba05e730fdc388e0b246bd1ef4be605c86d3c213d21e8061ba9f3c7ad3e375,
  ev_66f5d1235e429254c486fd546f9dfab52a3a109206d78e797473434e517c7118.
- GetHeight(uint,uint) reads a signed height byte plus section baseline, clamping
  coordinates: ev_689ba12c475b974fc04c04a38c0bce2e5efcef44193f8afd125b110c603e0517.

REA static investigation, SimJP.exe SHA256
ea043f1ea354dbf2848fcd76ca5739339b773a9913f6f68afdfa94bafad03b05:

- Water string inventory: ev_6d92eab537330ccf5895fea264d6674eb858347b1daa3798982e9cb516de58d3.
- Ocean string inventory: ev_61da4378a4542ddd10a4b0bee46aba819b1b02b157ff1674a0b78c38c25f628f.
- deepoceancolour reference: ev_94fb58f8b9393c35e5d971d1c24030888060a9b56c5aee0098986f924b9f8c0f.
- Enclosing procedure AWeatherManager::LoadWeatherVals:
  ev_3478f3482ae5964738aacb1672654a517770c3a92aeceb45d4d4274d42c44c0d.

These establish water/weather parameters and symbols, not runtime ocean/lake
classification or separate draw paths. No game process was launched by the agent.
