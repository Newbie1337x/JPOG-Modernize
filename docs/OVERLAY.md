# In-game settings panel

Press **Insert** in game (configurable: `[Overlay] Key` in `d3d8to11.ini`, a Windows
virtual-key code) to open the D3D11 renderer's settings panel. Changes apply immediately;
**Guardar graficos** writes them to `d3d8to11.ini`, **Recargar graficos** reads the file again.

The bounded, resizable panel uses five tabs, with independent scroll positions:

- **Graficos**: shadows, sun/day-night, colour, AO/shafts, normals, screen/MSAA/FSR1.
- **Agua y pasto**: water, waves/foam/SSR/interactions, interactive grass.
- **Camara**: live native camera limits and its separate save-to-default button.
- **Rendimiento**: frame timings, resource counters and memory measurements.
- **Experimental**: tessellation, parallax, material pilots, temporal AA/FSR31,
  frame generation and replacement-lighting diagnostics.

Visible labels/help are Spanish (standard technical names such as MSAA/FSR remain).
Only the first main section opens by default; advanced sections are collapsed.
Graphics save/reload/close remain in a fixed footer outside the scroll area.
Camera defaults still use **Guardar camara como predeterminada** and
`JPOGMouseFix.ini`; the footer does not save camera settings. No controls were
removed and the renderer option bindings/ranges/defaults are unchanged.

## Implementation

The `Rendimiento y recursos` section shows frame-time history/peak, CPU wall-time
inside Present and the FPS limiter, process private/resident RAM with resettable
baseline, DXGI local process GPU usage/budget when supported, live D3D8 wrapper
objects, shader variants/state cache sizes, and dynamic buffer capacity.
GPU timing is opt-in (session-only), sampled every 30 frames with a fixed four-slot
timestamp/disjoint query ring. Results are read with DONOTFLUSH; no spins/waits.
Pending slots are never reused; full/disjoint samples are discarded. The inclusive
GPU total covers first draw preparation through final presentation/overlay rendering,
not swap-chain Present or the frame limiter. Subsets expose combined AO/shafts,
shadow/color composition, and water snapshot copies. Geometry shadows, water draws,
normal maps and grass are not individually timed: compare matched scenes with their
toggles. Counters count PrepareDraw calls (including failed preparations), not exact
hardware draw calls. Wrapper object counts do not include internal D3D11 resources.
Memory growth after repeated identical scenes merits investigation, but caching and
game allocations can grow legitimately; these counters do not prove a leak.

`[Performance] LogCSV=1` now records once per second even with the panel closed.
Each session uses a unique `d3d8to11_performance_DATE_TIME_PID_PART.csv` next to
the executable, with its path announced in the normal log and the panel. Files
rotate after 36000 rows without overwriting preceding sessions. Each row contains
average/peak frame times, average Present/limiter waits, sampled inclusive GPU
timings with age/validity, RAM and GPU local budget/usage, average draw counts,
live resource counts and caches, MSAA and effect toggles including SSR quality.
Unknown/disabled/stale GPU timing is `NA`, not zero. GPU timing is now enabled
by default via `[Performance] MeasureGPU=1`; both switches are persistent through
Save. Small buffered rows are flushed once per second; this is synchronous disk
I/O, not a background logging thread. CSV numbers use invariant C locale.

The top button switches all custom scene effects off/on through
[Renderer] EffectsEnabled. Individual toggles and strengths are preserved;
Save persists the master switch as well. It covers shadows, ambient occlusion,
light shafts, colour grading, replacement lighting, normal maps, enhanced water
and interaction, and interactive grass. Disabled mode also overrides MSAA,
forced anisotropy, VSync and the frame cap to off without changing their saved
preferences. Turning the master back on restores those preferences. The game's
gamma ramp, texture replacements, camera/HUD improvements and profiling remain
available; this is the custom DX11 baseline, not the original D3D8 backend.
The grass constant cache explicitly uploads a disabled state when switching
off, preventing already-bound pusher data from continuing to bend blades.

- `src/d3d8to11/overlay.cpp` draws [Dear ImGui](https://github.com/ocornut/imgui)
  (v1.91.5, vendored in `third_party/imgui`, MIT) with its DX11 backend onto the swap chain
  in `Device::Present`, after the gamma/present pass. The Win32 backend is not used.
- JPOG reads mouse and keyboard with DirectInput (`TInputDXInterface.dll`). The `dinput8.dll`
  proxy patches `IDirectInput8::CreateDevice` and the devices' `GetDeviceState` /
  `GetDeviceData` vtable slots. Every mouse read is reported to `d3d8.dll`'s
  `D3D8to11_MouseInput` export, which moves the panel's own cursor; while the panel is open
  (`D3D8to11_InputBlocked`) the game receives empty mouse and keyboard input. With another
  `d3d8.dll` (dgVoodoo backend) the exports are absent and input is untouched.
- If no DirectInput mouse is seen for 0.5 s the panel uses the Windows cursor instead.
- The shadow map is created on demand (`Device::EnsureShadowMap`), so shadows can be
  switched on and the map resolution changed at runtime.
