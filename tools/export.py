import sys, os, glob, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tml import read_tml, to_img
from PIL import Image
src, out = sys.argv[1], sys.argv[2]
fondos = os.path.join(out, '1_Fondos_640x480'); hud = os.path.join(out, '2_HUD_y_botones'); fonts = os.path.join(out, '3_Fuentes')
for p in (fondos, hud, fonts): os.makedirs(p, exist_ok=True)
for f in sorted(glob.glob(os.path.join(src, 'bkg_*.tml'))):
    if f.endswith('16.tml'): continue
    tex, names = read_tml(f)
    imgs = [to_img(*tex[i]) for i in sorted(tex)]
    if len(imgs) != 6:
        print('skip layout', f, len(imgs)); continue
    canvas = Image.new('RGBA', (640, 480))
    x = y = 0
    for i, im in enumerate(imgs):
        canvas.paste(im, ((i % 3) * 256, (i // 3) * 256))
    base = os.path.basename(f)[:-4]
    canvas.convert('RGB').save(os.path.join(fondos, base + '.png'))
for f in sorted(glob.glob(os.path.join(src, 'gui_*.tml'))):
    if f.endswith('16.tml') or 'Japanese' in f or os.path.getsize(f) < 64: continue
    tex, names = read_tml(f)
    lib = os.path.basename(f)[:-4]
    for i in sorted(tex):
        n = names[i] if i < len(names) else 'tex%d' % i
        try: im = to_img(*tex[i])
        except Exception as e: print('fail', lib, n, e); continue
        dst = fonts if n.startswith('Fonts_') else hud
        im.save(os.path.join(dst, '%s__%s.png' % (lib, re.sub(r'[^\w.-]', '_', n))))
print('ok')
