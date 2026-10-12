"""Exports every game texture to PNG for editing/upscaling.

Usage: python export.py <game_dir> <out_dir>

Output:
  backgrounds/  menu and loading backgrounds stitched into one image (640x448 or 640x480)
  ui/           HUD/menu texture atlases (keep the layout: pieces are cut at fixed pixel positions)
  fonts/        bitmap fonts
  3d/<library>/ model textures (dinos, buildings, vehicles, people, fences, plants, particles)
File names follow the plugin convention <library>__<texture>.png.
16-bit duplicate libraries (*16.tml) and the Japanese GUI are skipped.
"""
import glob
import os
import re
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tml import read_tml, texture_name, to_img


def safe(name):
    return re.sub(r'[^\w.-]', '_', name)


def export_backgrounds(matlibs, out):
    """Backgrounds are 6 tiles: 3 columns (256, 256, 128) x 2 rows (256, 256)."""
    os.makedirs(out, exist_ok=True)
    for f in sorted(glob.glob(os.path.join(matlibs, 'bkg_*.tml'))):
        if f.endswith('16.tml'):
            continue
        tex, _ = read_tml(f)
        tiles = [to_img(*tex[i]) for i in sorted(tex)]
        if len(tiles) != 6:
            print('skipping unexpected layout', f, len(tiles))
            continue
        canvas = Image.new('RGB', (640, 512), (255, 255, 255))
        for i, tile in enumerate(tiles):
            canvas.paste(tile.convert('RGB'), ((i % 3) * 256, (i // 3) * 256))
        # Trim the near-white padding below the image (most backgrounds are 640x448).
        px = canvas.load()
        h = 512
        while h > 0 and all(min(px[x, h - 1]) >= 245 for x in range(0, 640, 8)):
            h -= 1
        h = 448 if h < 400 else h
        canvas.crop((0, 0, 640, h)).save(os.path.join(out, os.path.basename(f)[:-4] + '.png'))


def export_library(path, out, fonts_out=None):
    lib = os.path.basename(path)[:-4]
    tex, names = read_tml(path)
    for i in sorted(tex):
        name = texture_name(names, i)
        try:
            im = to_img(*tex[i])
        except Exception as e:
            print('failed', lib, name, e)
            continue
        dst = fonts_out if fonts_out and name.startswith('Fonts_') else out
        os.makedirs(dst, exist_ok=True)
        im.save(os.path.join(dst, '%s__%s.png' % (lib, safe(name))))


def main():
    game, out = sys.argv[1], sys.argv[2]
    matlibs = os.path.join(game, 'Data', 'matlibs')
    export_backgrounds(matlibs, os.path.join(out, 'backgrounds'))
    for f in sorted(glob.glob(os.path.join(matlibs, '*.tml'))):
        base = os.path.basename(f)[:-4]
        if base.startswith('bkg_') or base.endswith('16') or os.path.getsize(f) < 64:
            continue
        if base.startswith('gui_'):
            if 'Japanese' not in base:
                export_library(f, os.path.join(out, 'ui'), os.path.join(out, 'fonts'))
        else:
            export_library(f, os.path.join(out, '3d', base))
    print('done')


if __name__ == '__main__':
    main()
