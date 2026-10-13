# In-game settings panel

Press **Insert** in game (configurable: `[Overlay] Key` in `d3d8to11.ini`, a Windows
virtual-key code) to open the D3D11 renderer's settings panel. Changes apply immediately;
**Save** writes them to `d3d8to11.ini`, **Reload ini** reads the file again.

Sections: sun shadows (on/off, strength, distance, map resolution, terrain/grass casters),
sun direction (follow the game or manual azimuth/height, minimum height), display (FPS
limit, VSync, anisotropic filtering, game gamma, FPS counter) and experimental options
(replacement lighting, debug views).

## Implementation

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
