# JPOG Modernize

Mejoras para **Jurassic Park: Operation Genesis** (PC, 2003, motor Toshi de Blue Tongue) en forma de plugin `.asi` cargado por un proxy `dinput8.dll`. No modifica archivos del juego (salvo opcionalmente el flag Large Address Aware del exe).

## Qué hace

**Control**
- Cursor sin aceleración, sensibilidad lineal configurable y misma velocidad en X e Y en pantallas anchas.
- Slider de sensibilidad en primera persona con mínimo más bajo.
- Zoom con la rueda, suavizado de cámara y desplazamiento por bordes independientes de los FPS.
- Teclado no exclusivo: funcionan Impr Pant, Alt+F4 y la tecla Windows.

**Pantalla**
- Ventana sin bordes a pantalla completa (fuerza modo ventana en Direct3D 8).
- Widescreen Hor+ (en 16:9 se ve más a los costados, no se recorta arriba/abajo).
- HUD sin estirar, con tres modos (`HUDMode`):
  - `2`: 16:9 anclado, el HUD de partida va a los bordes y los menús quedan centrados.
  - `1`: todo en 4:3 centrado.
  - `0`: estirado como el original.
- Clics, iconos sobre unidades, vistas 3D de la interfaz y menús radiales corregidos para el HUD centrado.

**Texturas**
- Cargador de texturas: reemplaza cualquier textura del juego por archivos en `mods\textures\` (con subcarpetas), en `.png`, `.dds`, `.jpg`, `.tga` o `.bmp`, de cualquier resolución.
- Funciona con texturas DDS y crudas (1555/4444/8888). La GUI mantiene sus coordenadas originales.

## Instalación

1. Compilar con `build.bat` (Visual Studio x86). Si VS está en otra ruta, definir `VCVARS`.
2. `install.bat "C:\ruta\al\juego"`. Copia `JPOGMouseFix.asi`, `dinput8.dll` y el `.ini` (este último solo si no existe).
3. Configurar en `JPOGMouseFix.ini`.

## Herramientas (`tools/`, Python 3 + Pillow)

| Script | Uso |
|---|---|
| `tml.py` / `export.py` | Leer los `.tml` del juego y exportar texturas a PNG |
| `hd_a_mods.py` | PNG escalados → DDS DXT1/DXT5 con mipmaps en `mods\textures\<carpeta>` |
| `fondos_a_mods.py` | Fondos de menú enteros → las 6 piezas que usa el juego |
| `arreglar_alfa.py` | Copia la transparencia de la textura original a la escalada |
| `carpetas.py` | Ordena `mods\textures` en subcarpetas por categoría |
| `repack.py` | (Experimental) reempaqueta PNG dentro de un `.tml` |

Para escalar texturas recomendamos Real-ESRGAN (ncnn-vulkan) con el modelo 4x-UltraSharp.

## Ramas

- `main`: plugin actual (DirectX 8, se puede usar junto con dgVoodoo).
- `dx11`: renderizador propio que traduce Direct3D 8 → Direct3D 11, para shaders modernos (iluminación por píxel, sombras dinámicas, etc.). En desarrollo.

Las notas de ingeniería inversa (direcciones, estructuras, funciones) están en [docs/NOTAS.md](docs/NOTAS.md).

> No se incluyen archivos ni assets del juego. Hace falta una copia propia de JPOG.
