"""Corta fondos (1_Fondos, mejorados con IA) en las 6 piezas que usa el juego y los deja en mods\textures.
Uso: python fondos_a_mods.py <carpeta_con_fondos>   (los archivos se llaman bkg_xxx.png)
Si la imagen es mas ancha que 640:448 (ej. 16:9), se usa la parte central."""
import sys, os
from PIL import Image
G = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(G, 'mods', 'textures', 'ui', 'fondos'); os.makedirs(OUT, exist_ok=True)
SCALE = 4
for f in os.listdir(sys.argv[1]):
    if not f.lower().startswith('bkg_') or not f.lower().endswith(('.png', '.jpg', '.jpeg', '.webp')): continue
    base = os.path.splitext(f)[0]
    im = Image.open(os.path.join(sys.argv[1], f)).convert('RGB')
    w, h = im.size
    cw = round(h * 640 / 448)
    if cw < w: im = im.crop(((w - cw) // 2, 0, (w - cw) // 2 + cw, h))
    im = im.resize((640 * SCALE, 448 * SCALE), Image.LANCZOS)
    canvas = Image.new('RGB', (640 * SCALE, 512 * SCALE), (255, 255, 255)); canvas.paste(im, (0, 0))
    T = 256 * SCALE
    cols = [(0, T), (T, 2 * T), (2 * T, 640 * SCALE)]
    for i in range(6):
        c0, c1 = cols[i % 3]; r0 = (i // 3) * T
        canvas.crop((c0, r0, c1, r0 + T)).save(os.path.join(OUT, '%s__%s_%d.png' % (base, base, i + 1)))
    print('ok', base)
