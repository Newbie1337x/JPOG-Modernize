"""Reempaqueta texturas PNG en un .tml de JPOG (solo entradas DDS)."""
import struct, io, sys, os
from PIL import Image

def parse(d):
    nt = struct.unpack_from('<I', d, 8)[0]
    o = 0xc; ents = []
    for _ in range(nt):
        idx, size = struct.unpack_from('<II', d, o)
        ents.append([o, idx, d[o:o + 28], d[o + 28:o + 28 + size]])
        o += 28 + size
    nm = struct.unpack_from('<I', d, o)[0]
    names = [d[o + 4 + 32 * i:o + 36 + 32 * i].split(b'\0')[0].decode('latin1') for i in range(nm)]
    return ents, d[o:], names

def dds_bytes(img, fourcc):
    b = io.BytesIO()
    img.save(b, format='DDS', pixel_format=fourcc)
    return b.getvalue()

def repack(tml_in, tml_out, replacements):
    """replacements: {texture_name: PIL.Image}"""
    d = open(tml_in, 'rb').read()
    ents, trailer, names = parse(d)
    out = bytearray(d[:0xc])
    for o, idx, hdr, blob in ents:
        name = names[idx] if idx < len(names) else None
        if name in replacements and blob[:4] == b'DDS ':
            fourcc = blob[0x54:0x58].decode()
            img = replacements.pop(name).convert('RGBA')
            blob = dds_bytes(img, fourcc)
            hdr = bytearray(hdr)
            struct.pack_into('<I', hdr, 4, len(blob))
            struct.pack_into('<HH', hdr, 18, img.width, img.height)
            print('  reemplazada', name, img.size, fourcc)
        out += hdr + blob
    out += trailer
    open(tml_out, 'wb').write(out)
    if replacements:
        print('  NO encontradas:', list(replacements))
