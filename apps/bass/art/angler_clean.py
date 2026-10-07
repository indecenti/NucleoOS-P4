"""angler_clean.py - Vertice Bass: clear the navy studio background trapped along the angler's rod.

The poses were keyed by flooding the navy backdrop from the image borders, so the navy enclosed
between the rod and the fishing line stayed, as blue blotches along the rod. It can't be told from
the vest's shading by colour (the same navy), so by place: islands of backdrop colour lying wholly
to the right of the hands (the right edge of the white sleeves and the skin) are keyed out. Works on
the final textures img/an_*.565 (keyed magenta, row 0 at the bottom); then rerun angler_mask.py.

    python apps/bass/art/angler_clean.py
"""
import os
import struct
from collections import deque

import numpy as np

APP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
POSES = ("an_idle", "an_wind", "an_cast", "an_reel")
KEY = 0xF81F
BG = np.array([7, 44, 92])               # the generator's navy backdrop

for n in POSES:
    path = os.path.join(APP, "img", n + ".565")
    with open(path, "rb") as f:
        head = f.read(4)
        w, h = struct.unpack("<HH", head)
        px = np.frombuffer(f.read(w * h * 2), dtype="<u2").reshape(h, w).copy()
    rgb = np.dstack([((px >> 11) & 31) * 255 // 31, ((px >> 5) & 63) * 255 // 63, (px & 31) * 255 // 31]).astype(int)
    solid = px != KEY
    sleeve = solid & (rgb.min(axis=2) > 170)
    skin = solid & (rgb[..., 0] > 150) & (rgb[..., 1] > 90) & (rgb[..., 2] < 140) & (rgb[..., 0] > rgb[..., 2] + 40)
    hands = sleeve | skin
    for _ in range(2):                                        # drop thin things (the white line on the rod)
        hands = hands & np.roll(hands, 1, 0) & np.roll(hands, -1, 0) & np.roll(hands, 1, 1) & np.roll(hands, -1, 1)
    edge = int(np.where(hands.any(axis=0))[0].max()) + 2      # the hands' right edge (column)
    near = solid & (np.abs(rgb - BG).sum(axis=2) < 40)
    seen = np.zeros_like(near)
    killed = 0
    for y in range(h):
        for x in range(edge + 1, w):
            if near[y, x] and not seen[y, x]:
                q = deque([(y, x)]); seen[y, x] = True; pts = []
                while q:
                    cy, cx = q.popleft(); pts.append((cy, cx))
                    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)):
                        ny, nx = cy + dy, cx + dx
                        if 0 <= ny < h and 0 <= nx < w and near[ny, nx] and not seen[ny, nx]:
                            seen[ny, nx] = True; q.append((ny, nx))
                if min(p[1] for p in pts) > edge - 2:              # wholly beyond the hands: the rod's
                    for p in pts:
                        px[p] = KEY
                    killed += len(pts)
    with open(path, "wb") as f:
        f.write(head)
        f.write(px.astype("<u2").tobytes())
    print(f"{n}: hands end at column {edge}, {killed} trapped navy pixels keyed")
