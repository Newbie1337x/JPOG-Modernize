"""Offline analysis of a d3d8to11 sun shadow dump ([Lighting] DebugView=8).

Usage: python tools/sundump.py <game_dir>\\d3d8to11_dump [out_dir]

Writes PNGs next to the dump (or into out_dir):
  color.png      back buffer before the shadow resolve
  depth.png      scene depth (linearised for display)
  vis.png        CPU recomputation of sun visibility per screen pixel (white = lit)
  gap.png        light-space distance between each receiver and the nearest caster above it
                 (green: occluder 0-30 units in front, red: >30, blue: nothing in front)
  map.png        the shadow map crop that the screen covers, with screen samples projected
"""
import sys, os
import numpy as np
from PIL import Image

d = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else d
meta = {}
for line in open(os.path.join(d, "meta.txt")):
    k, *v = line.split()
    meta[k] = [float(x) for x in v]
W, H = int(meta["screen"][0]), int(meta["screen"][1])
M = int(meta["map"][0])
ctw = np.array(meta["clipToWorld"]).reshape(4, 4)
lr = np.array(meta["lightRows"]).reshape(4, 4)
rng = meta["depthRange"][0]

color = np.fromfile(os.path.join(d, "color.bgra"), np.uint8).reshape(H, W, 4)
Image.fromarray(color[:, :, [2, 1, 0]]).save(os.path.join(out, "color.png"))
raw = np.fromfile(os.path.join(d, "depth.d24s8"), np.uint32).reshape(H, W)
depth = (raw & 0xFFFFFF) / 16777215.0
smap = np.fromfile(os.path.join(d, "sunmap.f32"), np.float32).reshape(M, M)

ys, xs = np.mgrid[0:H, 0:W]
ndc = np.stack([(xs + 0.5) / W * 2 - 1 + 1.0 / W, 1 - (ys + 0.5) / H * 2 - 1.0 / H, depth, np.ones_like(depth)], -1)
wh = ndc @ ctw.T
world = wh[..., :3] / wh[..., 3:4]
sky = depth >= 0.999999
lq = np.concatenate([world, np.ones((H, W, 1))], -1) @ lr.T
u = lq[..., 0] * 0.5 + 0.5
v = -lq[..., 1] * 0.5 + 0.5
z = lq[..., 2]
inside = (u > 0) & (u < 1) & (v > 0) & (v < 1) & ~sky
ui = np.clip((u * M).astype(int), 0, M - 1)
vi = np.clip((v * M).astype(int), 0, M - 1)
mz = smap[vi, ui]
gap = (z - mz) * rng  # world units the stored caster is in front of the receiver
bias = 1.5 * meta["texel"][0] / rng
lit = ~inside | (z - bias <= mz)
vis = np.where(sky, 0.5, lit.astype(float))
Image.fromarray((vis * 255).astype(np.uint8)).save(os.path.join(out, "vis.png"))

lin = np.linalg.norm(world - world[H // 2, W // 2], axis=-1)
Image.fromarray(np.clip(255 - lin, 0, 255).astype(np.uint8)).save(os.path.join(out, "depth.png"))

g = np.zeros((H, W, 3), np.uint8)
occl = inside & (mz < 1)
g[~occl & ~sky] = (0, 0, 160)
near = occl & (gap > 0) & (gap <= 30)
g[near] = (0, 255, 0)
g[occl & (gap > 30)] = (255, 0, 0)
g[occl & (gap <= 0)] = (90, 90, 90)  # caster behind/at receiver: lit
Image.fromarray(g).save(os.path.join(out, "gap.png"))

# Shadow map crop covering the screen footprint
umin, umax = np.percentile(ui[inside], [1, 99]).astype(int)
vmin, vmax = np.percentile(vi[inside], [1, 99]).astype(int)
crop = smap[vmin:vmax + 1, umin:umax + 1]
valid = crop < 1
img = np.zeros(crop.shape + (3,), np.uint8)
if valid.any():
    lo, hi = crop[valid].min(), crop[valid].max()
    img[valid] = (np.clip(1 - (crop[valid] - lo) / max(hi - lo, 1e-9), 0, 1)[:, None] * 255).astype(np.uint8)
img[~valid] = (20, 20, 80)
# screen samples (receivers) as orange dots
sy_, sx_ = np.mgrid[0:H:24, 0:W:24]
for yy, xx in zip(sy_.ravel(), sx_.ravel()):
    if inside[yy, xx]:
        a, b = vi[yy, xx] - vmin, ui[yy, xx] - umin
        if 0 <= a < img.shape[0] and 0 <= b < img.shape[1]:
            img[a, b] = (255, 140, 0)
Image.fromarray(img).save(os.path.join(out, "map.png"))

print("screen", W, H, "map", M, "range", rng, "texel", meta["texel"][0])
print("world x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f" % tuple(
    v for a in range(3) for v in (np.percentile(world[..., a][~sky], 1), np.percentile(world[..., a][~sky], 99))))
print("receivers inside map %.1f%%, with caster in front %.1f%%, shadowed %.1f%%" % (
    100 * inside.mean(), 100 * occl.mean(), 100 * (inside & ~lit).mean()))
print("gap percentiles (world units) where caster stored:", np.percentile(gap[occl], [1, 10, 50, 90, 99]).round(2))
print("map crop u %d..%d v %d..%d, filled %.1f%%" % (umin, umax, vmin, vmax, 100 * valid.mean()))
