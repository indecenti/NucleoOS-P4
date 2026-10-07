"""junkbb.py - Vertice Bass: the junk lying on the lake bed (img/jb0..3.565) from the catch paintings.

The tin can, boot, tyre and treasure chest of the catch screen (img/junk0..3.565, 150x100, keyed) are
also scattered on the bed as billboards the lure can snag. VX_BILLBOARD textures have row 0 at the
BOTTOM, so these are the paintings flipped, trimmed to the object (no empty frame) and given a clear
1-texel frame (sampled with clamp). Run from the repo root:

    python apps/bass/art/junkbb.py
"""
import os
import struct

import numpy as np

IMG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "img")
KEY = 0xF81F

for k in range(4):
    d = open(os.path.join(IMG, "junk%d.565" % k), "rb").read()
    w, h = struct.unpack("<HH", d[:4])
    px = np.frombuffer(d[4:4 + w * h * 2], dtype="<u2").reshape(h, w)
    # the catch paintings drip water: on the bed those blue drops would hang in the water - keyed out
    r = ((px >> 11) & 31) * 255 / 31; g = ((px >> 5) & 63) * 255 / 63; b = (px & 31) * 255 / 31
    drop = (px != KEY) & (b > 110) & (b > r * 1.35) & (b > g * 1.08)
    px = px.copy()
    px[drop] = KEY
    solid = px != KEY                                 # and the specks left of them: rows with almost nothing
    counts = solid.sum(axis=1)
    for y in range(h - 1, -1, -1):                    # trim thin dribbles off the bottom
        if counts[y] > max(4, counts.max() * 0.12):
            break
        px[y] = KEY
    ys, xs = np.nonzero(px != KEY)
    crop = px[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    ch, cw = crop.shape
    # the object standing on the bottom row, centred
    W = H = 128                                       # one size for all four (lake.c sizes them by kind)
    out = np.full((H, W), KEY, dtype="<u2")
    x0 = (W - cw) // 2
    out[H - ch:H, x0:x0 + cw] = crop
    out = out[::-1]                                   # billboard: row 0 at the bottom
    with open(os.path.join(IMG, "jb%d.565" % k), "wb") as f:
        f.write(struct.pack("<HH", W, H))
        f.write(out.astype("<u2").tobytes())
    print("jb%d" % k, W, "x", H, "object", cw, "x", ch)
