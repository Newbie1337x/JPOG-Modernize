"""AI upscalers often lose transparency (leaving a background or a baked 'checkerboard').
Copies the alpha channel of the ORIGINAL texture onto the upscaled one.

Usage: python fix_alpha.py <upscaled_dir> <originals_dir>

Only files present in both folders whose original has transparency are touched. A copy of each
modified file is kept in <upscaled_dir>/_backup.
"""
import os
import shutil
import sys

from PIL import Image


def main():
    src, orig_dir = sys.argv[1], sys.argv[2]
    backup = os.path.join(src, '_backup')
    for f in sorted(os.listdir(src)):
        path = os.path.join(src, f)
        orig_path = os.path.join(orig_dir, f)
        if not f.lower().endswith('.png') or not os.path.exists(orig_path):
            continue
        orig = Image.open(orig_path).convert('RGBA')
        if orig.getchannel('A').getextrema()[0] == 255:
            continue  # original is opaque
        im = Image.open(path).convert('RGBA')
        os.makedirs(backup, exist_ok=True)
        if not os.path.exists(os.path.join(backup, f)):
            shutil.copy2(path, os.path.join(backup, f))
        im.putalpha(orig.getchannel('A').resize(im.size, Image.LANCZOS))
        im.save(path)
        print('alpha restored:', f)


if __name__ == '__main__':
    main()
