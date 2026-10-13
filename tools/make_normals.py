"""Generate normal maps for JPOG textures (BC5 DDS with mipmaps).

Usage:
  python tools/make_normals.py <folder> [--strength 3.0] [--max 1024] [--force] [--skip plants,particles]

Walks <folder> recursively (folders starting with "_" are skipped) and, for every
"<library>__<texture>.dds/.png" that is not itself a normal map, writes
"<library>__<texture>_n.dds" next to it. The texture-replacement plugin hands these to the
D3D11 renderer (DX convention: green = +v, i.e. down the image).

The height is estimated from luminance (bright = raised), so it is a stylised relief, not a
scanned one. Better maps (e.g. from Materialize or DeepBump) can replace any file: keep the
name, DirectX green convention (or set [NormalMaps] FlipGreen=1), any of BC5/BC3/BC1/RGBA8.
"""
import argparse
import os
import struct
import sys

import numpy as np
from PIL import Image


def height_from(img):
    rgba = np.asarray(img.convert("RGBA"), dtype=np.float32) / 255.0
    lum = rgba[..., 0] * 0.299 + rgba[..., 1] * 0.587 + rgba[..., 2] * 0.114
    alpha = rgba[..., 3]
    # Transparent texels (foliage cut-outs) count as low ground so leaf edges get a rim.
    h = lum * np.clip(alpha * 2, 0, 1)
    # Remove the large-scale brightness (baked lighting, colour patches): keep detail only.
    k = max(3, min(h.shape) // 32)
    low = box_blur(h, k)
    detail = h - low
    return box_blur(detail, 1)


def box_blur(a, r):
    if r <= 0:
        return a
    p = np.pad(a, r, mode="wrap")
    c = np.cumsum(np.cumsum(p, 0), 1)
    c = np.pad(c, ((1, 0), (1, 0)))
    n = 2 * r + 1
    s = c[n:, n:] - c[:-n, n:] - c[n:, :-n] + c[:-n, :-n]
    return s / (n * n)


def normals(h, strength):
    # Gradients in texels, scaled so the look is resolution independent (relative to 512).
    scale = strength * max(h.shape) / 512.0
    dx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5 * scale
    dy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5 * scale
    n = np.stack([-dx, -dy, np.ones_like(h)], -1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


def downsample(n):
    h, w = n.shape[:2]
    if h > 1:
        n = (n[0:h - h % 2:2] + n[1:h:2]) * 0.5 if h % 2 == 0 else n[::2]
    if w > 1:
        n = (n[:, 0:w - w % 2:2] + n[:, 1:w:2]) * 0.5 if w % 2 == 0 else n[:, ::2]
    return n / np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)


def bc4_blocks(ch):
    """ch: (H, W) uint8 with H, W multiples of 4 -> bytes of BC4 blocks (row-major)."""
    h, w = ch.shape
    b = ch.reshape(h // 4, 4, w // 4, 4).transpose(0, 2, 1, 3).reshape(h // 4, w // 4, 16).astype(np.int32)
    r0 = b.max(-1)
    r1 = b.min(-1)
    k = np.arange(8)
    # 8-value mode (r0 > r1): code 0 = r0, 1 = r1, 2..7 = ((8-c)*r0 + (c-1)*r1) / 7
    pal = np.empty(b.shape[:2] + (8,), np.int32)
    pal[..., 0] = r0
    pal[..., 1] = r1
    for c in range(2, 8):
        pal[..., c] = ((8 - c) * r0 + (c - 1) * r1 + 3) // 7
    idx = np.abs(b[..., :, None] - pal[..., None, :]).argmin(-1).astype(np.uint64)
    idx[r0 == r1] = 0
    bits = np.zeros(b.shape[:2], np.uint64)
    for i in range(16):
        bits |= idx[..., i] << np.uint64(3 * i)
    out = np.zeros(b.shape[:2] + (8,), np.uint8)
    out[..., 0] = r0
    out[..., 1] = r1
    for j in range(6):
        out[..., 2 + j] = ((bits >> np.uint64(8 * j)) & np.uint64(0xFF)).astype(np.uint8)
    return out


def bc5(n):
    h, w = n.shape[:2]
    ph, pw = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    n = np.pad(n, ((0, ph - h), (0, pw - w), (0, 0)), mode="edge")
    x = np.clip(np.round((n[..., 0] * 0.5 + 0.5) * 255), 0, 255).astype(np.uint8)
    y = np.clip(np.round((n[..., 1] * 0.5 + 0.5) * 255), 0, 255).astype(np.uint8)
    bx, by = bc4_blocks(x), bc4_blocks(y)
    return np.concatenate([bx, by], -1).tobytes()  # per block: 8 bytes red, 8 bytes green


def write_dds(path, levels, w, h):
    data = b"".join(levels)
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000  # caps|height|width|pixelformat|mipcount|linearsize
    linear = ((w + 3) // 4) * ((h + 3) // 4) * 16
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, flags, h, w, linear, 0, len(levels))
    pf = struct.pack("<II4sIIIII", 32, 0x4, b"ATI2", 0, 0, 0, 0, 0)
    caps = struct.pack("<IIII4x", 0x1000 | 0x400000 | 0x8, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(header + pf + caps + data)


def convert(src, dst, strength, max_size):
    img = Image.open(src)
    img.load()
    if max(img.size) > max_size:
        s = max_size / max(img.size)
        img = img.resize((max(4, int(img.width * s)), max(4, int(img.height * s))), Image.LANCZOS)
    n = normals(height_from(img), strength)
    levels = []
    h, w = n.shape[:2]
    while True:
        levels.append(bc5(n))
        if n.shape[0] == 1 and n.shape[1] == 1:
            break
        n = downsample(n)
    write_dds(dst, levels, w, h)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--strength", type=float, default=3.0)
    ap.add_argument("--max", type=int, default=1024)
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--skip", default="", help="comma-separated folder names to skip")
    a = ap.parse_args()
    skip = {s.strip().lower() for s in a.skip.split(",") if s.strip()}
    done = failed = 0
    for root, dirs, files in os.walk(a.folder):
        dirs[:] = [d for d in dirs if not d.startswith("_") and d.lower() not in skip]
        for f in files:
            stem, ext = os.path.splitext(f)
            if ext.lower() not in (".dds", ".png", ".tga", ".jpg", ".bmp") or "__" not in stem or stem.lower().endswith("_n"):
                continue
            dst = os.path.join(root, stem + "_n.dds")
            if os.path.exists(dst) and not a.force:
                continue
            try:
                convert(os.path.join(root, f), dst, a.strength, a.max)
                done += 1
                print("ok", os.path.relpath(dst, a.folder))
            except Exception as e:  # unreadable formats are reported and skipped
                failed += 1
                print("FAILED", f, e, file=sys.stderr)
    print(f"{done} normal maps written, {failed} failed")


if __name__ == "__main__":
    main()
