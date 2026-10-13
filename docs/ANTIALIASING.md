# Temporal antialiasing, FSR 3.1 Native AA and AMD FSR1

## Frame generation x2 prototype (off by default)

`FrameGeneration=0` is the safe default. The checkbox enables FSR31 and VSync;
generation only runs with valid world history and equal render/window sizes.
This is a synchronous experiment, NOT the full asynchronous AMD swapchain
integration. The DX11 SDK's swapchain configuration callback is a no-op success;
we directly run OpticalFlow and FrameInterpolation and own the extra Present.

The original package crashes creating FI_Counters: structured buffers lack
D3D11 misc/stride flags. The local backend override adds them and both SRV/UAV,
and fixes the error formatter's non-truncating conversion. An isolated x86 probe
creates all 7 OF / 11 FI compute pipelines with no failures. The supplied binary
libraries/package source remain unchanged outside this renderer's vendored copy.

Flow/interpolation operate on FSR's linear FP16 HUD-less output and native
R32 depth/camera motion. FI prepare reuses the shared depth/motion/reconstruction
surfaces. scRGB transfer uses a 0..80 luminance interval so the SDK normalization
is identity for our SDR-linear 0..1 values, without adding an HDR display mode.
First frame, skipped history and camera cuts update history but suppress
generated presentation. Unsupported sizes/VSync-off skip generation. Failure
disables the prototype while retaining native FSR and real frames.

At Present, generated linear color is gamma-encoded and composited with the
current real image at changed HUD pixels (difference from HUD-less native FSR).
This is an approximate mask, NOT exact HUD alpha: semitransparent panels retain
current real background in their regions. The game gamma LUT follows, and existing
ImGui draw data is replayed without rebuilding input/UI. One VSync Present shows
the generated frame, then the already-rendered real frame/GUI is restored and
shown at the next VBlank. No extra simulation/input tick occurs.

Consequences: CPU waits for two synchronized presents, extra latency, render FPS
can be capped around half refresh; there is no promise of higher FPS than baseline
or improved responsiveness. Intended for evaluation of low/base-rate motion, not
high-refresh competitive pacing. Correct animated-object vectors, exact HUD alpha,
asynchronous scheduling and extensive latency/visual validation are still missing.

`FrameGenerationDebug=1` shows the intermediate as the real scene for inspection
and suppresses the extra Present. Disable it for x2 mode. GUI exposes successful
generated Present call count. CSV appends requested/presented/failed and cumulative
generated presents; actual display visibility/scanout is not measured. The legacy
temporal GPU timer includes OF/FI computation; Present CPU time includes both
VSync waits. Generated composition/replay happens after the GPU frame query ends.

`tools/check_framegen.bat` validates hidden 64x64 real/generated presentation calls,
reads actual SDK intermediate pixels on a stable background/moving-actor fixture
(different from current scene), camera pan,
depth clears and exact real-HUD green. Diagnostics mode only reads intermediate
output; x2 mode requires a generated Present log. No game is launched. Native FSR
MSAA1/8 regression checks also pass. No claim of real 1080p quality/performance.
The x2 fixture also passes MSAA8: CSV records requested=1, presented=1,
failed=0 and three generated Present calls after four rendered frames (warmup
frame is not generated). Direct intermediate readback produced distinct colors,
e.g. current 18652f -> intermediate 1b7d2f, not a duplicate of the real frame.

## FSR 3.1 DX11 x86 (experimental)

`FSR31=1` now selects the actual local FidelityFX DX11 port, native-resolution
temporal accumulation plus SDK sharpening, before depth clear/HUD. The SDK is
vendored with licenses in `third_party/fsr31`, from OptiScaler's optiscaler-build
commit `9b04fa49abae5aa341c0007af13c8ba1b4ad9913`, plus the supplied x86 patch.
Renderer uses /MD to match Release static SDK libraries; plugin/proxy remain /MT.
MSVC x86 runtime is required (already present on the development machine).

This stage does NOT lower internal resolution, offer Quality/Performance modes,
or itself generate frames. It can cost more GPU time; the SDK is used as native AA.
Do not describe it as an FPS boost or completed motion-vector integration.

Inputs: scene color converted approximately gamma 2.2 -> linear FP16; MSAA depth
reduced to nearest R32; normalized unjittered previous-minus-current camera motion
converted by SDK pixel scales; SDK jitter sequence; clip planes/FOV derived from
captured inverse VP; explicit reset for camera cuts, missing history/frames and
resource changes. Output returns to approximate gamma 2.2 before the existing
game gamma/presentation. No HDR display conversion is added.

Conservative reactive mask: sky, invalid/offscreen reprojection, native submerged
terrain coordinates and approximate creature footprints. No actual previous
skinning matrices, animated grass motion or complete alpha/transparency coverage
yet. Water without a native map can still ghost; water-floor reprojection is not
an accurate water-surface vector. This remains experimental visual quality.

FSR overrides old TAA while active and skips FSR1 at presentation for that world
frame, preserving the HUD. Original settings remain stored for fallback. FL11.1
and <=4,194,304 input pixels are required for this prototype. Initialization or
SDK dispatch failure logs a reason and falls back to old TAA/FSR1; GUI exposes
retry/reset. Disabling/master-off/resize/device destruction destroys the context
and releases compute bindings. Textures and scratch are reused per resolution.

GUI temporal GPU timing includes FSR31 input generation, SDK dispatch and copy;
CSV appends `fsr31_requested,fsr31_active,fsr31_failed`. The legacy `gpu_taa_ms`
column now measures whichever temporal implementation ran, not TAA exclusively.

Hidden fixture `tools/check_fsr31.bat <captured profile>` runs actual SDK dispatch,
reads the post-resolve output before HUD, and checks the HUD remains green. It
does not launch SimJP or establish 1080p performance/moving-dinosaur quality.
Validated with MSAA1 and MSAA8, camera pan, four depth-clear scene resolves,
nonzero post-SDK readback within 40/255 of current color and exact green HUD.
Log assertions require actual SDK activation and reject silent fallback/double
TAA/FSR1 processing. This does not measure temporal reconstruction quality.
Synthetic CSV confirms requested=1, active=1, failed=0; `gpu_taa_ms` is populated.

`[Antialiasing]` settings (all available in Insert and persisted by Save):

- `TAA=1`: experimental native-resolution temporal AA before the HUD.
- `HistoryWeight=0.72`: 0..0.9; lower it if moving objects show trails.
- `FSR1=1`: official AMD EASU spatial upscaling and RCAS sharpening.
- `Sharpness=0.5`: 0..1, mapped to RCAS attenuation stops.

Legacy options default on as fallback; FSR31 takes precedence. All respect the
master effects button. Existing MSAA is retained.

## TAA

Eight-phase Halton (bases 2/3) jitter affects recognized main-view world and
enhanced-water vertices, not shadow-map casters or HUD. Water/post depth
reconstruction compensates for jitter. Before HUD/depth clear, a temporal pass
writes FP16 color and single-sample R32 depth to two reused ping-pong histories.
Current color is sampled with inverse jitter; history uses the previous terrain
world-to-clip matrix, depth rejection, 3x3 neighborhood clamping, motion-dependent
attenuation and color-difference attenuation. History resets on resize/MSAA,
disabling, skipped world frames and large camera changes.

This is camera-reprojected TAA, NOT DLSS or FSR2/3. No skinned motion vectors exist.
Moving dinosaurs/foliage can ghost; water depth may belong to the floor. Sky is
not accumulated. Use the history slider or switch TAA off if undesirable.
It adds GPU cost and does not reduce internal resolution. MSAA is independent.

## FSR1

The Shader Model 5 pixel shaders call official AMD FsrEasuF/FsrRcasF using FP32.
EASU runs when input is smaller than output in both dimensions with matching
aspect ratio; RCAS follows. Native resolution skips EASU and runs only RCAS.
Downsampling/aspect conversion uses original presentation. This feature does NOT
lower the game's internal render resolution, so a performance gain requires a
genuinely lower input, not downsampling an already rendered native image.
The game HUD is in the input and is spatially processed; ImGui is drawn afterward.
Final game gamma remains afterward.

Runtime dependencies: `shaders/fsr1/` next to the executable, copied by build.bat
from `third_party/fsr1/`, with AMD's MIT license. Source:
https://github.com/GPUOpen-Effects/FidelityFX-FSR/tree/master/ffx-fsr

- ffx_a.h SHA256: F60E2722FCD13989523B9164D776AB382B3692791767F3BF8BB19967F763F3FB
- ffx_fsr1.h SHA256: 93C3922362EA7FC99CBCC698CA30C98DE4F8C246D1FBB0B09E015DDEF38CE3A5

Missing dependencies/shader/resource failures log a reason and keep original
presentation. Fixed resources are reused; no per-frame history allocations.

## Diagnostics and unavailable features

GUI/CSV expose gpu_taa_ms, gpu_fsr1_ms, taa and fsr1. Times are inclusive: scene
composition currently also encloses TAA, so do not sum them as separate costs.
DLSS is not implemented: installed GPU is Radeon RX 6700 XT, not RTX. Standard
DLSS Windows SDK binaries target Windows_x86_64 and cannot load in this x86 game.
No unsupported NVIDIA runtime was installed. A future x64/RTX integration still
needs motion vectors, exposure, jitter/reset and capability checks. FSR2/3 and
frame generation are not part of the legacy TAA/FSR1 path. FSR31 Native AA is
described above; the separate synchronous frame generation prototype is described
at the beginning of this document.

`tools/check_antialiasing.bat <captured profile>` runs one hidden synthetic GPU
check: TAA, 64-to-128 EASU/RCAS, MSAA8 and pre-present HUD. It does not launch
SimJP or validate moving-dinosaur quality; real visual validation remains in-game.
