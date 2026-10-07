"""barktex.py - Vertice Bass: the bark of the dead trees in the water (img/t_bark.565), warmer and lighter.

The painted bark (gen5.py) averaged RGB (59, 45, 29): with the engine's shading on top the logs read
as black on screen. This scales it to a fixed mean brightness and pulls the hue to a warm wood brown,
keeping the grain's contrast. Idempotent: it aims at the target mean, so running it twice changes
nothing. Run from the repo root:

    python apps/bass/art/barktex.py
"""
import os
import struct

import numpy as np

PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "img", "t_bark.565")
TARGET = np.array([132.0, 96.0, 62.0])        # mean colour of the result: a sunlit weathered brown

d = open(PATH, "rb").read()
w, h = struct.unpack("<HH", d[:4])
px = np.frombuffer(d[4:4 + w * h * 2], dtype="<u2").reshape(h, w)
rgb = np.dstack([((px >> 11) & 31) * 255 / 31, ((px >> 5) & 63) * 255 / 63, (px & 31) * 255 / 31]).astype(float)
lum = rgb.mean(axis=2, keepdims=True)
grain = lum / lum.mean()                       # the texture's light and dark, around 1
out = np.clip(grain * TARGET + (rgb - lum) * 0.5, 0, 255)   # warm brown, a little of the original tint
v = ((out[..., 0].astype(int) & 0xF8) << 8) | ((out[..., 1].astype(int) & 0xFC) << 3) | (out[..., 2].astype(int) >> 3)
with open(PATH, "wb") as f:
    f.write(struct.pack("<HH", w, h))
    f.write(v.astype("<u2").tobytes())
print("t_bark: mean", out.reshape(-1, 3).mean(0).round(), "(was", rgb.reshape(-1, 3).mean(0).round(), ")")
