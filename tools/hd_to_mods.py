"""Converts upscaled PNG textures to compressed DDS with mipmaps and puts them in mods\\textures.

Usage: python hd_to_mods.py <game_dir> <upscaled_dir> [originals_dir]

- Opaque textures -> DXT1 (0.5 byte/pixel); with transparency -> DXT5 (1 byte/pixel).
- With originals_dir, alpha is taken from the original texture (resized), which is more faithful
  than the upscaler's alpha. If the original alpha was 1-bit (fences, leaves) it is thresholded
  back to 0/255 so edges stay crisp.
- Files are sorted into subfolders with folders.folder_for().
The game is 32-bit: compression is what makes x4 textures fit in memory.
"""
import io
import os
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from folders import folder_for


def dxt_level(img, fourcc):
    b = io.BytesIO()
    img.save(b, format='DDS', pixel_format=fourcc)
    return b.getvalue()[128:]  # strip the DDS header Pillow writes


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
    linear_size = max(1, (W + 3) // 4) * max(1, (H + 3) // 4) * block
    # DDSD_CAPS | HEIGHT | WIDTH | PIXELFORMAT | MIPMAPCOUNT | LINEARSIZE
    hdr = struct.pack('<4sIIIIIII44x', b'DDS ', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000,
                      H, W, linear_size, 0, len(levels))
    hdr += struct.pack('<II4sIIIII', 32, 0x4, fourcc.encode(), 0, 0, 0, 0, 0)  # DDPF_FOURCC
    hdr += struct.pack('<IIII4x', 0x1000 | 0x8 | 0x400000, 0, 0, 0)          # TEXTURE | COMPLEX | MIPMAP
    assert len(hdr) == 128
    with open(path, 'wb') as f:
        f.write(hdr)
        for lv in levels:
            f.write(lv)


def main():
    game, src = sys.argv[1], sys.argv[2]
    orig_dir = sys.argv[3] if len(sys.argv) > 3 else None
    out = os.path.join(game, 'mods', 'textures')
    count = 0
    for f in sorted(os.listdir(src)):
        if '__' not in f or not f.lower().endswith('.png'):
            continue
        im = Image.open(os.path.join(src, f)).convert('RGBA')
        orig = None
        if orig_dir and os.path.exists(os.path.join(orig_dir, f)):
            orig = Image.open(os.path.join(orig_dir, f)).convert('RGBA')
        lo, _ = (orig or im).getchannel('A').getextrema()
        has_alpha = lo < 250
        if has_alpha:
            alpha = orig.getchannel('A').resize(im.size, Image.LANCZOS) if orig else im.getchannel('A')
            if orig is not None and set(orig.getchannel('A').getdata()) <= {0, 255}:
                alpha = alpha.point(lambda v: 255 if v >= 128 else 0)
            im.putalpha(alpha)
        dst = os.path.join(out, folder_for(f))
        os.makedirs(dst, exist_ok=True)
        save_dds_mips(im, os.path.join(dst, f[:-4] + '.dds'), 'DXT5' if has_alpha else 'DXT1')
        count += 1
        if count % 25 == 0:
            print(count, 'done...')
    print(count, 'textures written to', out)


if __name__ == '__main__':
    main()
