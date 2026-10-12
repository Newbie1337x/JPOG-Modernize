"""Los upscalers con IA suelen perder la transparencia (queda un fondo o un 'tablero').
Este script copia la transparencia de la textura ORIGINAL (JPOG_UI_Export) a la version mejorada.
Uso: python arreglar_alfa.py            (procesa todo mods/textures; guarda copia en mods/textures/_backup)"""
import os, sys, shutil
from PIL import Image
G = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
MODS = os.path.join(G, 'mods', 'textures'); BK = os.path.join(MODS, '_backup')
EXP = os.path.join(G, 'JPOG_UI_Export')
def find_orig(name):
    for d in ('2_HUD_y_botones', '3_Fuentes'):
        p = os.path.join(EXP, d, name)
        if os.path.exists(p): return p
for f in sorted(os.listdir(MODS)):
    if '__' not in f or not f.lower().endswith('.png') or f.startswith('bkg_'): continue
    orig = find_orig(os.path.splitext(f)[0] + '.png')
    if not orig: print('sin original:', f); continue
    o = Image.open(orig).convert('RGBA')
    if o.getchannel('A').getextrema()[0] == 255: continue   # el original no tiene transparencia
    p = os.path.join(MODS, f)
    n = Image.open(p).convert('RGBA')
    os.makedirs(BK, exist_ok=True)
    if not os.path.exists(os.path.join(BK, f)): shutil.copy2(p, os.path.join(BK, f))
    a = o.getchannel('A').resize(n.size, Image.LANCZOS)
    n.putalpha(a)
    n.save(p)
    print('alfa restaurada:', f)
