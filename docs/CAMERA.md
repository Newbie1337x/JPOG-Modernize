# Park camera — independent controller

## Live tuning and selected objects

Insert → **Camara y limites (en vivo)** adjusts park/selected-unit min/max zoom,
pitch bounds, fixed FOV, response and drag sensitivity immediately. The dedicated
**Guardar camara como predeterminada** button writes only these `[Camera]` keys
to JPOGMouseFix.ini (not d3d8to11.ini). Existing unrelated settings are preserved.
Normal overlay Save is still for renderer settings, not camera defaults.

`FocusMinDistance=2` / `FocusMaxDistance=250` are independent of the selected
unit's size. Both park and focus maximum distances accept up to 2000; going far
enough can expose the renderer's existing far clip/LOD rather than a camera limit.
`MaxPitch=0` stops at the horizon; negative values stop short of it. Positive
values are no longer accepted for this park camera.

Fixed native virtual limits replace all four distance/pitch limit getters for
AWorldCamera and AFollowCamera, but only while their camera is actively owned.
Every vtable slot must match the corresponding exported function before patching.
The follow camera retains native selected-unit position tracking, then passes
its inherited world-controller step to our independent controller. Other camera
classes are excluded. The panel displays actual controller/follow readiness.

Root cause of the max-zoom angle bug: AWorldCamera::ConstMaxPitch interpolates
its allowed pitch with **squared normalized distance**. Updating +0x3c4 alone
was insufficient: the native setter still applied that changing limit.
REA evidence `ev_d3b86eeb30e8e9d8e96ffec3b0210018f3ab6bbdd3987f342c17de14a1485d90`.
GetOrientation only reconstructs the quaternion with shake offsets; it did not
establish the initially suspected collision-look-at explanation (that hypothesis
was rejected; flag 0xe is not removed).
Follow limits previously used selected-unit size and a short distance margin:
`ev_ff14bb1dc9025783ce7bfa2f1a62fd631bda70d9d1aafddab064c62d5156a948`,
`ev_f55c3f32655ff29bdcee9b5bbcca4dc9c31374809022cd3c02aea47c8ca70c18`.
Follow OnUpdate calls World OnUpdate after position tracking:
`ev_23e63079d367258fde065a966d662683be0673a9c3a61825c06cd32214a643fd`.

`tools/camera_bridge_test.cpp` executes the actual controller and limit/settings
bridges against a mock native camera (no game/DLL loading). It covers repeated
scrolling at maximum distance without pitch change, horizon clamping, terrain
flag retention, selected-unit limits and finite/bounded live settings updates.

## Stage 3: Jurassic World Evolution style controls

- Middle drag orbits and tilts (Shift + middle pans, Alt + middle changes height).
- Right drag pans. The proxy hides the right press from the game until the mouse
  moves 6 raw counts (a drag) or the button is released (a click): a click is then
  delivered as a press in place of the release, and the release on the next
  buffered read, so right-click actions keep working. A drag never reaches the game.
  `RightDragPan=0` disables it.
- W/A/S/D call InputPushForwards/InputPushRight with the frame time, the same
  impulse AScrollingState::ProcessInput gives the arrow keys (the camera scales it
  by its height-dependent speed, vtable +0xf4). Shift multiplies by `KeyboardFast`
  (2.5), Ctrl by `KeyboardSlow` (0.35); `KeyboardPanSpeed` scales both, `KeyboardPan=0`
  disables them. Evidence: arrow handling
  `ev_563d8b84fbef55e430c2af2d73714e84e82ef04ebaf40587b2f45f2ae7ba016f`, push setters
  `ev_4a4de7216e62e240dc37855cd80f9dd9e1072142b29eb0dc2d0d1b648480b58d`.
- The game reads mouse buttons only from buffered DirectInput events
  (TInputDXDeviceMouse::ProcessEvents) and the axes from GetDeviceState
  (TInputDXDeviceMouse::Update): `ev_1badb65dda2c3fad0640c89761862d76adefe3c73533dec456cee08459d31b31`.
- Default pitch range -89 (top-down) to -10 degrees (low, near the horizon),
  following reference screenshots from Jurassic World Evolution 3.

## Stage 2: independent behavior (default)

`IndependentController=1` replaces the active park camera's AWorldCamera::OnUpdate
with our own six-axis exponential velocity integrator. Native frame-based momentum,
default-pitch resets, altitude-driven FOV and orientation/distance interpolation
are not executed for this camera. The base ACamera update remains the bridge for
scene position, sound listener, render/picking and existing focal transitions.

Wheel changes distance only. Rotation/tilt are manual. Alt + middle + vertical
drag changes height only. The stage-1 middle pan / Shift-middle orbit controls
are retained, including their DirectInput suppression and permission gates.

Defaults: distance 2–250 (`MinDistance`/`MaxDistance`), pitch -85–0 degrees
(`MinPitch`/`MaxPitch`), fixed 50-degree FOV (`FixedFOV`). ResponseSeconds now
controls our own integrator independently of FixSmoothing. Set
`IndependentController=0` to use the stage-1 behavior; ModernControls=0 disables
both stages at the next launch.

Native GetPosition terrain protection (flag 8) is retained: it can lift the eye
above the ground without overwriting manual pitch. Native focal-ground deflection
(flag 0x15) is disabled to permit independent focal height. X/Z island limits are
retained. ConstMinDistance is overridden only for the active independent camera,
removing the legacy coupling between minimum zoom distance and focal altitude.
Other cameras fall through to their original code.

Verified with the production math helper: movement/braking equivalence at
30–240 FPS; zoom-only and height-only inputs keep pitch/yaw unchanged; bounds
clear outward momentum. These are offline tests, not an in-game acceptance test.

REA evidence (same binary/hash below): base ACamera update
`ev_a46c17e777d39d180056cf14f7b18572b045e5887911c11d6b2ee1ac793af8f5`;
default-pitch flag 6 `ev_e0f83512063b88f4097ebc04930d4fe512c710f8f6d354f2637e22efbd9aa914`;
terrain-protected eye position `ev_ab7114fd1abb237c09a14dc13ddf6bedf23adcbb1dbe1b615c4a0504d1731610`;
focal ground deflection `ev_ceaf9e6dc138d4cc00b483019a462b1906e92f8bf104feed8d11807062eaf99f`;
height-dependent zoom limit `ev_ca5b4807e71522eba1bc1c3d4c361686be594242c1496aea8e876802f016ee1e`.
World-update and minimum-distance trampolines copy whole-instruction prologues
(7/6 bytes), checked in native assembly; matching exports/signatures are required.

## Stage 1: retained input bridge

This stage extends the native world camera, not the renderer's view matrices.
Native terrain/zoom/pitch limits and world-to-screen picking remain in place.
The new gestures require the matching `JPOGMouseFix.asi` and `dinput8.dll`.

- Hold middle mouse and drag: grab/pan the park, in camera-relative directions.
- Shift + middle drag: orbit and tilt.
- Wheel: smaller steps close to the ground, larger steps far away.
- Existing keyboard, selection and first-person controls are retained.

The raw DirectInput bridge consumes only mouse X/Y and middle-button events
during an eligible drag. Left, right and wheel remain available to the game.
It supports both state and buffered reads; peek reads do not feed camera deltas.
The existing graphics settings panel still has priority over camera input.
Dragging suppresses native edge-scroll so it does not fight the new gesture.

Controls are gated on the active camera's exact AWorldCamera vtable, the native
AScrollingState translate/rotate permission flags and input-map mouse permission.
Window focus, overlay state and a recent scrolling-state update are also required.
Jeep, helicopter, follow cameras and inactive states don't receive these gestures.
Raw deltas are displacement: there is no second multiplication by frame dt.

## Configuration

`JPOGMouseFix.ini`, `[Camera]`, restart after changes:

- `ModernControls=1`; set 0 to disable the new gestures, response and zoom scaling.
- `ResponseSeconds=0.12`: exponential response time (range 0.03–0.5).
  Lower is more direct; higher gives more glide. Requires `FixSmoothing=1`.
- `DragPanSensitivity=0.0015`: native input units per raw mouse count.
- `DragRotateSensitivity=0.0018`: native rotation units per raw mouse count.
- `ProportionalZoom=1`: wheel strength scales with distance/45, clamped 0.35–2.
  Requires `FixWheel=1`. `WheelStrength` still controls overall wheel speed.

Existing `ReferenceFPS` behavior is retained when ModernControls=0.
These are desktop park gestures, not a complete free-camera replacement.
No game was launched for development: drag direction, feel, build/menu switching,
limits and vehicle transitions require the user's in-game acceptance test.

## Reverse-engineering basis

REA/Ghidra 12.1.4, SimJP.exe SHA256
`ea043f1ea354dbf2848fcd76ca5739339b773a9913f6f68afdfa94bafad03b05`:

- AScrollingState::OnUpdate 0x55cb40: native permissions +0x48 translate,
  +0x4a rotate; AInputMap singleton +0x78 bit 2 permits mouse edge controls.
  Evidence `ev_463aa8fdd6f3488d984755b454118e69a09ff8548c9fd454c6d531ae0937f456`.
- AScrollingState::ProcessInput 0x55c3d0 sends input to the active camera,
  respects permissions and uses elapsed time for native drag gestures.
  Evidence `ev_913aea55eb6631a6527754149444e913572118528b0c153bcc6ee085356381f7`.
- AWorldCamera::InputPushRight/Forwards and InputRotateRight/Down accept float
  impulses. Native OnUpdate 0x527c10 damps and clears those inputs each frame,
  then calls the native camera update.
  Evidence `ev_a7f3d9907536cc8d69682d434995ada0b08b75012bcaff08f2695276222c6c8f`.
- InputZoomOut 0x528100 accumulates zoom impulses and attenuates near native
  distance limits. Scaling wheel input retains this path.
  Evidence `ev_cbefb61b5d7c01231c2d8ba5de411bc8b98e653cc0c2ae1004f6c16be16f6a36`.

The scrolling hook trampoline copies seven whole instructions bytes with no
relative operands. Named exports and the checked prologue gate installation.
These are static observations, not evidence of completed in-game validation.

## Verification

Built the plugin and input proxy for x86 with MSVC. The standalone
`tools/camera_controls_test.cpp` includes the real proxy hooks, supplies synthetic
DirectInput reads, and verifies state/buffered/peek handling, preservation of
left/right buttons and wheel, response equivalence at 30/60/144/240 FPS, zero/large
dt and zoom bounds. All checks passed. The exported callback's x86 decorated name
was verified with dumpbin. None of these checks launches SimJP.exe.
