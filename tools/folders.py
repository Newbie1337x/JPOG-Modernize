"""Suggested mods\\textures subfolder for each game texture library.

The plugin reads mods\\textures recursively, so folders are only for organisation: what matters
is the file name (<library>__<texture>.<ext>).

Usage: python folders.py <game_dir>   -> moves loose files in mods\\textures into subfolders
"""
import os
import shutil
import sys

GUI_LIBS = {'gui_main': 'menu', 'gui_ingame': 'hud', 'gui_dinopedia': 'dinopedia',
            'gui_fossilhunt': 'fossils'}
PREFIXES = (('dinos_', 'dinos'), ('structures', 'buildings'), ('vehicles', 'vehicles'),
            ('people', 'people'), ('fences', 'fences'), ('flora', 'plants'), ('iptree', 'plants'),
            ('particles', 'particles'), ('misc', 'misc'))


def folder_for(file_name):
    lib, _, tex = file_name.partition('__')
    lib = lib.lower()
    if tex.lower().startswith('fonts_'):
        return os.path.join('ui', 'fonts')
    if lib.startswith('bkg_'):
        return os.path.join('ui', 'backgrounds')
    if lib.startswith('gui_'):
        base = lib[:-2] if lib.endswith('16') else lib
        return os.path.join('ui', GUI_LIBS.get(base, 'other'))
    for prefix, folder in PREFIXES:
        if lib.startswith(prefix):
            return folder
    return 'other'


if __name__ == '__main__':
    mods = os.path.join(sys.argv[1], 'mods', 'textures')
    moved = 0
    for f in os.listdir(mods):
        p = os.path.join(mods, f)
        if os.path.isfile(p) and '__' in f:
            dst = os.path.join(mods, folder_for(f))
            os.makedirs(dst, exist_ok=True)
            shutil.move(p, os.path.join(dst, f))
            moved += 1
    print(moved, 'files moved')
