#!/usr/bin/env python3
"""fishtex.py - Vertice Bass: the painted fish (img/fish<N>.565, Qwen-Image side views, nose to the
right on a magenta key) become the skins of the 3D fish.

Writes
  img/fx.565        256x256 atlas, 2 columns x 4 rows of 128x64 cells, one species per cell, keyed
  fishprof.h        per species: the painting's box in the atlas (texels) and the body's outline
                    sampled at NR stations from tail to nose (centre row, half height) — the C mesh
                    builder puts a ring at each station, so the 3D body has the painting's silhouette;
                    the fins and the tail come from a keyed flat plane through the middle of the fish.

Run from the repo root:  python apps/bass/art/fishtex.py
"""
import os
import struct

KEY = 0xF81F
AW, AH, CW, CH = 512, 512, 256, 128
NSP = 8
NR = 12                                  # stations along the body


def load565(path):
    d = open(path, 'rb').read()
    w, h = struct.unpack('<HH', d[:4])
    px = list(struct.unpack('<%dH' % (w * h), d[4:4 + 2 * w * h]))
    return w, h, px


def load_hd(path):
    """A Qwen panel on navy: the navy (and the blue fringe) keyed out, as a 565 pixel list."""
    import numpy as np
    from PIL import Image
    im = Image.open(path).convert('RGB')
    im = im.crop((12, 12, im.width - 12, im.height - 12))
    a = np.asarray(im).astype(int)
    bg = a[4, 4]
    near = np.abs(a - bg).sum(axis=2) < 75
    # only the background connected to the border (dark scales inside the fish stay): flood it
    from collections import deque
    hh, ww = near.shape
    out = np.zeros_like(near)
    q = deque([(y, x) for y in range(hh) for x in (0, ww - 1) if near[y, x]] + [(y, x) for x in range(ww) for y in (0, hh - 1) if near[y, x]])
    for y, x in q:
        out[y, x] = True
    while q:
        y, x = q.popleft()
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ny, nx = y + dy, x + dx
            if 0 <= ny < hh and 0 <= nx < ww and near[ny, nx] and not out[ny, nx]:
                out[ny, nx] = True
                q.append((ny, nx))
    # one pixel of the fringe around the fish (navy anti-aliasing) goes too
    edge = out.copy()
    edge[1:, :] |= out[:-1, :]; edge[:-1, :] |= out[1:, :]; edge[:, 1:] |= out[:, :-1]; edge[:, :-1] |= out[:, 1:]
    fringe = edge & ~out & (a[..., 2] > a[..., 1]) & (a[..., 2] > a[..., 0])
    v = ((a[..., 0] & 0xF8) << 8) | ((a[..., 1] & 0xFC) << 3) | (a[..., 2] >> 3)
    v[out | fringe] = KEY
    return im.width, im.height, [int(x) for x in v.reshape(-1)]


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    app = os.path.dirname(here)
    atlas = [KEY] * (AW * AH)
    rows = []
    for sp in range(NSP):
        hd = os.path.join(os.environ.get('TEMP', '/tmp'), 'bass_art', 'hd%d.png' % sp)
        if os.path.exists(hd):                          # the high-resolution side views (gen9.py fishhd*)
            w, h, px = load_hd(hd)
        else:
            w, h, px = load565(os.path.join(app, 'img', 'fish%d.565' % sp))
        solid = lambda x, y: px[y * w + x] != KEY
        cols = [x for x in range(w) if any(solid(x, y) for y in range(h))]
        rws = [y for y in range(h) if any(solid(x, y) for x in range(w))]
        x0, x1, y0, y1 = cols[0], cols[-1], rws[0], rws[-1]
        bw, bh = x1 - x0 + 1, y1 - y0 + 1
        s = min((CW - 2) / bw, (CH - 2) / bh)          # keep the aspect
        dw, dh = int(bw * s), int(bh * s)
        cx, cy = (sp % 2) * CW, (sp // 2) * CH
        ox, oy = cx + (CW - dw) // 2, cy + (CH - dh) // 2
        for y in range(dh):
            for x in range(dw):
                atlas[(oy + y) * AW + ox + x] = px[(y0 + int(y / s)) * w + x0 + int(x / s)]
        # Body outline per column of the scaled painting: the extent of solid texels, then a running
        # minimum of the half height over a window (fins are thin spikes on the body: they drop out)
        # and the centre as the middle of that body.
        top, bot = [], []
        for x in range(dw):
            ys = [y for y in range(dh) if atlas[(oy + y) * AW + ox + x] != KEY]
            top.append(ys[0] if ys else dh // 2)
            bot.append(ys[-1] if ys else dh // 2)
        half = [(b - t) / 2 for t, b in zip(top, bot)]
        mid = [(b + t) / 2 for t, b in zip(top, bot)]
        win = max(3, dw // 14)
        hm, mm = [], []
        for x in range(dw):
            lo, hi = max(0, x - win), min(dw, x + win + 1)
            k = min(range(lo, hi), key=lambda i: half[i])
            hm.append(half[k])
            mm.append(mid[k])
        hb, mb = [], []                                 # then smoothed (the min filter steps)
        for x in range(dw):
            lo, hi = max(0, x - win), min(dw, x + win + 1)
            hb.append(sum(hm[lo:hi]) / (hi - lo))
            mb.append(sum(mm[lo:hi]) / (hi - lo))
        st = []
        for r in range(NR):
            f = 0.14 + 0.84 * r / (NR - 1)              # from the tail's root to the snout
            x = min(dw - 1, int(f * (dw - 1)))
            hh = hb[x] * (0.55 if r == NR - 1 else 1.0)
            st.append((round(f, 3), round(mb[x], 1), round(max(0.8, hh), 1)))
        rows.append((ox, oy, dw, dh, st))
    with open(os.path.join(app, 'img', 'fx.565'), 'wb') as f:
        f.write(struct.pack('<HH', AW, AH))
        f.write(struct.pack('<%dH' % (AW * AH), *atlas))
    with open(os.path.join(app, 'fishprof.h'), 'w', newline='\n') as f:
        f.write('// fishprof.h - generated by art/fishtex.py from img/fish<N>.565: do not edit.\n')
        f.write('// Per species: the painting in img/fx.565 (x, y, w, h texels) and the body outline at\n')
        f.write('// %d stations from the tail root to the snout: {fraction of the length, centre row, half height}.\n' % NR)
        f.write('#define FX_NR %d\n' % NR)
        f.write('#define FX_UVK %d   // UV units per atlas texel (1024 = one repeat of %d texels)\n' % (1024 // AW, AW))
        f.write('typedef struct { short x, y, w, h; float st[FX_NR][3]; } FishSkin;\n')
        f.write('static const FishSkin k_skin[%d] = {\n' % NSP)
        for ox, oy, dw, dh, st in rows:
            f.write('    { %d, %d, %d, %d, {%s} },\n' % (ox, oy, dw, dh, ', '.join('{%.3ff, %.1ff, %.1ff}' % t for t in st)))
        f.write('};\n')
    print('fx.565 + fishprof.h written')


if __name__ == '__main__':
    main()
