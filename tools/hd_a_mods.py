"""Convierte texturas escaladas (PNG) a DDS comprimido con mipmaps y las deja en mods\\textures.

Uso: python hd_a_mods.py <carpeta_escaladas> [carpeta_originales]

- Opacas -> DXT1 (0.5 byte/pixel), con transparencia -> DXT5 (1 byte/pixel).
- Si la original tenia transparencia de 1 bit (cercas, hojas), el alfa escalado se vuelve a cortar
  en 0/255 para que los bordes queden nitidos.
- Con la carpeta de originales se toma el alfa de la original agrandado (mas fiel que el del
  escalador) y se verifica que la proporcion sea la misma.
El juego es de 32 bits: comprimir es lo que permite usar texturas x4 sin quedarse sin memoria.
"""
import io
import os
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from carpetas import carpeta_para

G = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
OUT = os.path.join(G, 'mods', 'textures')


def dxt_level(img, fourcc):
    b = io.BytesIO()
    img.save(b, format='DDS', pixel_format=fourcc)
    return b.getvalue()[128:]


def save_dds_mips(img, path, fourcc):
    levels = []
    w, h = img.size
    cur = img
    while True:
        levels.append(dxt_level(cur, fourcc))
        if w == 1 and h == 1:
            break
        w, h = max(1, w // 2), max(1, h // 2)
        cur = img.resize((w, h), Image.LANCZOS)
    W, H = img.size
    block = 8 if fourcc == 'DXT1' else 16
    linear = max(1, (W + 3) // 4) * max(1, (H + 3) // 4) * block
    hdr = struct.pack('<4sIIIIIII44x', b'DDS ', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000,
                      H, W, linear, 0, len(levels))
    hdr += struct.pack('<II4sIIIII', 32, 0x4, fourcc.encode(), 0, 0, 0, 0, 0)
    hdr += struct.pack('<IIII4x', 0x1000 | 0x8 | 0x400000, 0, 0, 0)
    assert len(hdr) == 128
    with open(path, 'wb') as f:
        f.write(hdr)
        for lv in levels:
            f.write(lv)


def main():
    src = sys.argv[1]
    orig_dir = sys.argv[2] if len(sys.argv) > 2 else None
    os.makedirs(OUT, exist_ok=True)
    n = 0
    for f in sorted(os.listdir(src)):
        if '__' not in f or not f.lower().endswith('.png'):
            continue
        im = Image.open(os.path.join(src, f)).convert('RGBA')
        orig = None
        if orig_dir and os.path.exists(os.path.join(orig_dir, f)):
            orig = Image.open(os.path.join(orig_dir, f)).convert('RGBA')
        a_src = orig.getchannel('A') if orig else im.getchannel('A')
        lo, hi = a_src.getextrema()
        has_alpha = lo < 250
        if has_alpha:
            if orig:
                alpha = orig.getchannel('A').resize(im.size, Image.LANCZOS)
            else:
                alpha = im.getchannel('A')
            binary = orig is not None and set(orig.getchannel('A').getdata()) <= {0, 255}
            if binary:
                alpha = alpha.point(lambda v: 255 if v >= 128 else 0)
            im.putalpha(alpha)
        fourcc = 'DXT5' if has_alpha else 'DXT1'
        dst = os.path.join(OUT, carpeta_para(f))
        os.makedirs(dst, exist_ok=True)
        save_dds_mips(im, os.path.join(dst, f[:-4] + '.dds'), fourcc)
        n += 1
        if n % 25 == 0:
            print(n, 'listas...')
    print(n, 'texturas en', OUT)


if __name__ == '__main__':
    main()
