import struct, io, sys, os
from PIL import Image
def read_tml(path):
    d=open(path,'rb').read()
    nt=struct.unpack_from('<I',d,8)[0]
    o=0xc; tex={}
    for i in range(nt):
        idx,size,a,f,fmt,h,w=struct.unpack_from('<IIIfHHH',d,o)
        tex[idx]=(fmt,w,h,d[o+28:o+28+size])
        o+=28+size
    nm=struct.unpack_from('<I',d,o)[0]; o+=4
    names=[d[o+32*i:o+32*i+32].split(b'\0')[0].decode('latin1') for i in range(nm)]
    return tex,names
def to_img(fmt,w,h,blob):
    if blob[:4]==b'DDS ':
        im=Image.open(io.BytesIO(blob)); im.load(); return im.convert('RGBA')
    if fmt == 2:   # 1555 con R en los bits bajos (el motor lo convierte a A1R5G5B5)
        import struct as _s
        px = _s.unpack('<%dH' % (w * h), blob[:w * h * 2])
        a = bytearray(w * h * 4)
        for i, v in enumerate(px):
            a[4*i] = (v & 31) * 255 // 31; a[4*i+1] = ((v >> 5) & 31) * 255 // 31
            a[4*i+2] = ((v >> 10) & 31) * 255 // 31; a[4*i+3] = 255 if v >> 15 else 0
        return Image.frombytes('RGBA', (w, h), bytes(a))
    if fmt == 0:   # 8888 en orden RGBA en memoria
        return Image.frombuffer('RGBA', (w, h), blob[:w * h * 4], 'raw', 'RGBA', 0, 1).copy()
    raise ValueError('fmt %d' % fmt)
if __name__=='__main__':
    tex,names=read_tml(sys.argv[1]); out=sys.argv[2]; os.makedirs(out,exist_ok=True)
    print(names)
    for idx,(fmt,w,h,b) in sorted(tex.items()):
        n=names[idx] if idx<len(names) else 'tex%d'%idx
        try: to_img(fmt,w,h,b).save(os.path.join(out,'%02d_%s.png'%(idx,n))); print(idx,n,w,h)
        except Exception as e: print('fail',idx,n,e)
