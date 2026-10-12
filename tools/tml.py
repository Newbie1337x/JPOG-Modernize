"""Reader for JPOG .tml texture libraries (Data\\matlibs\\*.tml).

Layout: "TML1", u32 0, u32 count, then `count` entries:
    u32 index, u32 size, u32 3, f32 35.0, u16 fmt, u16 width, u16 height, u16 0xffff, u16 0, u16 'C'
    followed by `size` bytes of data.
Then u32 name_count and name_count names of 32 bytes; entry `index` uses name[index].

fmt 6 = DDS file, fmt 2 = 16-bit 1555, fmt 0 = 32-bit 8888. Raw formats store R in the low bits
(RGBA byte order); the engine swaps R/B when uploading.

Usage: python tml.py <file.tml> <out_dir>   -> dumps every texture as PNG
"""
import io
import os
import struct
import sys

from PIL import Image

ENTRY_HEADER = 28


def read_tml(path):
    """Returns ({index: (fmt, width, height, data)}, [names])."""
    d = open(path, 'rb').read()
    count = struct.unpack_from('<I', d, 8)[0]
    o = 0xc
    tex = {}
    for _ in range(count):
        idx, size, _three, _f, fmt, w, h = struct.unpack_from('<IIIfHHH', d, o)
        tex[idx] = (fmt, w, h, d[o + ENTRY_HEADER:o + ENTRY_HEADER + size])
        o += ENTRY_HEADER + size
    name_count = struct.unpack_from('<I', d, o)[0]
    o += 4
    names = [d[o + 32 * i:o + 32 * i + 32].split(b'\0')[0].decode('latin1') for i in range(name_count)]
    return tex, names


def to_img(fmt, w, h, blob):
    """Decodes one texture entry to an RGBA PIL image."""
    if blob[:4] == b'DDS ':
        im = Image.open(io.BytesIO(blob))
        im.load()
        return im.convert('RGBA')
    if fmt == 2:  # 1555, R in the low bits
        px = struct.unpack('<%dH' % (w * h), blob[:w * h * 2])
        a = bytearray(w * h * 4)
        for i, v in enumerate(px):
            a[4 * i] = (v & 31) * 255 // 31
            a[4 * i + 1] = ((v >> 5) & 31) * 255 // 31
            a[4 * i + 2] = ((v >> 10) & 31) * 255 // 31
            a[4 * i + 3] = 255 if v >> 15 else 0
        return Image.frombytes('RGBA', (w, h), bytes(a))
    if fmt == 0:  # 8888, RGBA byte order
        return Image.frombuffer('RGBA', (w, h), blob[:w * h * 4], 'raw', 'RGBA', 0, 1).copy()
    raise ValueError('unsupported fmt %d' % fmt)


def texture_name(names, idx):
    return names[idx] if idx < len(names) else 'tex%d' % idx


if __name__ == '__main__':
    tex, names = read_tml(sys.argv[1])
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for idx, (fmt, w, h, blob) in sorted(tex.items()):
        name = texture_name(names, idx)
        try:
            to_img(fmt, w, h, blob).save(os.path.join(out, '%02d_%s.png' % (idx, name)))
            print(idx, name, w, h)
        except Exception as e:
            print('failed', idx, name, e)
