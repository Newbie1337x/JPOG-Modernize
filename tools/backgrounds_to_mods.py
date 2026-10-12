"""Cuts full background images into the 6 tiles the game uses and writes them to
mods\\textures\\ui\\backgrounds.

Usage: python backgrounds_to_mods.py <game_dir> <backgrounds_dir> [scale]

Files must be named like the exported ones (e.g. bkg_fe_menu.png). Images wider than 640:448
(e.g. 16:9 AI outpaints) are center-cropped. Default scale is 4 (2560x1792).
"""
import os
import sys

from PIL import Image


def main():
    game, src = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    out = os.path.join(game, 'mods', 'textures', 'ui', 'backgrounds')
    os.makedirs(out, exist_ok=True)
    tile = 256 * scale
    cols = [(0, tile), (tile, 2 * tile), (2 * tile, 640 * scale)]
    for f in os.listdir(src):
        if not f.lower().startswith('bkg_') or not f.lower().endswith(('.png', '.jpg', '.jpeg', '.webp')):
            continue
        base = os.path.splitext(f)[0]
        im = Image.open(os.path.join(src, f)).convert('RGB')
        w, h = im.size
        crop_w = round(h * 640 / 448)
        if crop_w < w:
            x0 = (w - crop_w) // 2
            im = im.crop((x0, 0, x0 + crop_w, h))
        im = im.resize((640 * scale, 448 * scale), Image.LANCZOS)
        canvas = Image.new('RGB', (640 * scale, 512 * scale), (255, 255, 255))
        canvas.paste(im, (0, 0))
        for i in range(6):
            c0, c1 = cols[i % 3]
            r0 = (i // 3) * tile
            canvas.crop((c0, r0, c1, r0 + tile)).save(os.path.join(out, '%s__%s_%d.png' % (base, base, i + 1)))
        print('ok', base)


if __name__ == '__main__':
    main()
