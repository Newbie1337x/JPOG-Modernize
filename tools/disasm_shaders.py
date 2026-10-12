"""Disassembles the shaders dumped by the D3D8 profiler ([Debug] ProfileD3D8=1).

Usage: python disasm_shaders.py <game_dir>\\d3d8_profile [out_dir]

- *_func.bin / ps_*.bin: vs_1_1 / ps_1_x bytecode, disassembled with d3dcompiler_47's
  D3DDisassemble (D3D8 and D3D9 share the 1.x token format).
- *_decl.bin: D3D8 vertex declarations (D3DVSD_* tokens), decoded here.
Identical shaders (same bytes) are reported once.
"""
import ctypes
import hashlib
import os
import struct
import sys

D3D8_DECL_TYPES = {0: 'FLOAT1', 1: 'FLOAT2', 2: 'FLOAT3', 3: 'FLOAT4', 4: 'D3DCOLOR', 5: 'UBYTE4', 6: 'SHORT2', 7: 'SHORT4'}


def disassemble(blob):
    d3dc = ctypes.WinDLL('d3dcompiler_47.dll')
    out = ctypes.c_void_p()
    hr = d3dc.D3DDisassemble(blob, len(blob), 0, None, ctypes.byref(out))
    if hr != 0:
        return '; D3DDisassemble failed: 0x%08x' % (hr & 0xffffffff)
    # ID3DBlob: vtable [QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize]
    vtbl = ctypes.cast(ctypes.cast(out, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtbl[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vtbl[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vtbl[2])
    text = ctypes.string_at(get_ptr(out), get_size(out)).decode('latin1').rstrip('\0')
    release(out)
    return text


def decode_decl(blob):
    tokens = struct.unpack('<%dI' % (len(blob) // 4), blob)
    lines = []
    for t in tokens:
        kind = t >> 29
        if t == 0xFFFFFFFF:
            lines.append('END')
            break
        if kind == 1:  # D3DVSD_TOKEN_STREAM
            lines.append('STREAM %d' % (t & 0xF))
        elif kind == 2:  # D3DVSD_TOKEN_STREAMDATA
            if t & 0x10000000:
                lines.append('  SKIP %d dwords' % ((t >> 16) & 0xF))
            else:
                lines.append('  v%-2d %s' % (t & 0x1F, D3D8_DECL_TYPES.get((t >> 16) & 0xF, '?')))
        else:
            lines.append('  token 0x%08x' % t)
    return '\n'.join(lines)


def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(src, 'disasm')
    os.makedirs(out, exist_ok=True)
    seen = {}
    for f in sorted(os.listdir(src)):
        if not f.endswith('.bin'):
            continue
        blob = open(os.path.join(src, f), 'rb').read()
        digest = hashlib.sha1(blob).hexdigest()
        if digest in seen:
            print('%s == %s' % (f, seen[digest]))
            continue
        seen[digest] = f
        text = decode_decl(blob) if f.endswith('_decl.bin') else disassemble(blob)
        open(os.path.join(out, f[:-4] + '.txt'), 'w').write(text + '\n')
        print('wrote', f)


if __name__ == '__main__':
    main()
