# JPOG Modernize

Improvements for **Jurassic Park: Operation Genesis** (PC, 2003, Blue Tongue's Toshi engine), delivered as an `.asi` plugin loaded by a `dinput8.dll` proxy. Game files are not modified (except, optionally, the Large Address Aware flag of the executable).

## Features

**Controls**
- Pointer acceleration removed; linear, configurable sensitivity with equal X/Y speed on wide screens.
- Lower minimum for the first-person mouse sensitivity slider.
- Framerate-independent mouse wheel zoom, camera smoothing and edge scrolling.
- Non-exclusive keyboard: Print Screen, Alt+F4 and the Windows key work.

**Display**
- Borderless fullscreen window (forces windowed mode in Direct3D 8).
- Hor+ widescreen: 16:9 shows more on the sides instead of cropping top/bottom.
- Unstretched UI with three modes (`HUDMode`):
  - `2`: 16:9 anchored. The in-game HUD sits on the real screen edges and menus stay centered.
  - `1`: everything centered in 4:3.
  - `0`: stretched, like the original.
- Clicks, icons over units, 3D views inside the UI and radial/popup menus are corrected for the centered UI.

**Textures**
- Texture loader: replaces any game texture with files from `mods\textures\`.
  - Subfolders are supported; folders starting with `_` are ignored.
  - Formats: `.png`, `.dds`, `.jpg`, `.tga` or `.bmp`, at any resolution.
- Works for DDS and raw (1555/4444/8888) textures. The UI keeps its original coordinates, so upscaled atlases line up.

## Building and installing

1. Run `build.bat` (Visual Studio, x86 tools). If Visual Studio is installed elsewhere, set `VCVARS`.
2. Run `install.bat "C:\path\to\game"`. It copies `JPOGMouseFix.asi`, `dinput8.dll` and the `.ini`; the `.ini` is copied only if missing.
3. Edit `JPOGMouseFix.ini` in the game folder.

## Tools (`tools/`, Python 3 + Pillow)

| Script | Purpose |
|---|---|
| `tml.py` | Reads the game's `.tml` texture libraries |
| `export.py` | Exports every texture to PNG (backgrounds, UI, fonts, 3D) |
| `hd_to_mods.py` | Upscaled PNGs → DXT1/DXT5 DDS with mipmaps, sorted into `mods\textures` |
| `backgrounds_to_mods.py` | Full menu backgrounds → the 6 tiles the game uses |
| `fix_alpha.py` | Copies the original alpha channel onto upscaled textures |
| `folders.py` | Sorts loose files in `mods\textures` into category folders |
| `repack.py` | (Experimental) repacks PNGs into a `.tml` |

For upscaling we recommend Real-ESRGAN (ncnn-vulkan) with the 4x-UltraSharp model.

## Branches

- `main`: current plugin (Direct3D 8; works alongside dgVoodoo).
- `dx11`: custom renderer translating Direct3D 8 to Direct3D 11, to enable modern shaders (per-pixel lighting, dynamic shadows, ...). Work in progress.

Reverse-engineering notes (addresses, structures, functions) are in [docs/NOTES.md](docs/NOTES.md).

> No game files or assets are included. You need your own copy of JPOG.
