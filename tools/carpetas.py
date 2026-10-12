"""Carpeta sugerida dentro de mods\textures para cada libreria del juego.
Uso directo: python carpetas.py   -> ordena los archivos sueltos de mods\textures en subcarpetas."""
import os
import shutil

def carpeta_para(nombre):
    lib, _, tex = nombre.partition('__')
    l = lib.lower()
    if tex.lower().startswith('fonts_'):
        return os.path.join('ui', 'fuentes')
    if l.startswith('bkg_'):
        return os.path.join('ui', 'fondos')
    if l.startswith('gui_'):
        sub = {'gui_main': 'menu', 'gui_ingame': 'hud', 'gui_dinopedia': 'dinopedia',
               'gui_fossilhunt': 'fosiles'}.get(l.rstrip('16') if l.endswith('16') else l, 'otros')
        return os.path.join('ui', sub)
    for pref, dst in (('dinos_', 'dinos'), ('structures', 'edificios'), ('vehicles', 'vehiculos'),
                      ('people', 'personas'), ('fences', 'cercas'), ('flora', 'plantas'),
                      ('iptree', 'plantas'), ('particles', 'particulas'), ('misc', 'varios')):
        if l.startswith(pref):
            return dst
    return 'otros'

if __name__ == '__main__':
    G = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    mods = os.path.join(G, 'mods', 'textures')
    n = 0
    for f in os.listdir(mods):
        p = os.path.join(mods, f)
        if os.path.isfile(p) and '__' in f:
            d = os.path.join(mods, carpeta_para(f))
            os.makedirs(d, exist_ok=True)
            shutil.move(p, os.path.join(d, f))
            n += 1
    print(n, 'archivos ordenados')
