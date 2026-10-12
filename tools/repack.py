"""(Experimental) Repacks PNG images into a JPOG .tml library (DDS entries only).

Prefer the plugin's mods\\textures loader: it keeps the original files untouched and preserves the
original texture size the GUI relies on. Replacing a GUI texture here with a different size breaks
its layout, because the GUI addresses texture pieces in pixels.
"""
import io
import struct

from PIL import Image


def parse(d):
    count = struct.unpack_from('<I', d, 8)[0]
    o = 0xc
    entries = []
    for _ in range(count):
        idx, size = struct.unpack_from('<II', d, o)
        entries.append([o, idx, d[o:o + 28], d[o + 28:o + 28 + size]])
        o += 28 + size
    name_count = struct.unpack_from('<I', d, o)[0]
    names = [d[o + 4 + 32 * i:o + 36 + 32 * i].split(b'\0')[0].decode('latin1') for i in range(name_count)]
    return entries, d[o:], names


def dds_bytes(img, fourcc):
    b = io.BytesIO()
    img.save(b, format='DDS', pixel_format=fourcc)
    return b.getvalue()


def repack(tml_in, tml_out, replacements):
    """replacements: {texture_name: PIL.Image}"""
    d = open(tml_in, 'rb').read()
    entries, trailer, names = parse(d)
    out = bytearray(d[:0xc])
    for _, idx, hdr, blob in entries:
        name = names[idx] if idx < len(names) else None
        if name in replacements and blob[:4] == b'DDS ':
            fourcc = blob[0x54:0x58].decode()
            img = replacements.pop(name).convert('RGBA')
            blob = dds_bytes(img, fourcc)
            hdr = bytearray(hdr)
            struct.pack_into('<I', hdr, 4, len(blob))
            struct.pack_into('<HH', hdr, 18, img.width, img.height)
            print('  replaced', name, img.size, fourcc)
        out += hdr + blob
    out += trailer
    open(tml_out, 'wb').write(out)
    if replacements:
        print('  not found:', list(replacements))
