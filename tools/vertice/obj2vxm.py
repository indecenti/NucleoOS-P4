#!/usr/bin/env python3
"""obj2vxm.py - convert a Wavefront .obj model (with its .mtl) to a Vertice .vxm.

A game loads it with vx_model("name", flags) from its models/<name>.vxm. Every material of the .mtl
becomes a Vertice material with its diffuse colour (Kd) and opacity (d); faces are triangulated (fans);
UVs are kept when the .obj has them (then texture the model in the game: vx_mat_set(mat,
VX_MAT_TEXTURE, tex), or give the object one textured material with vx_obj_material). The model is
scaled to the size you ask for and can be centred on its base, so it stands on y = 0.

    python tools/vertice/obj2vxm.py car.obj apps/mygame/models/car.vxm --size 120 --base
    python tools/vertice/obj2vxm.py ship.obj out.vxm --scale 40 --smooth --flip-v

Format (little endian): "VXM1", u16 verts, u16 tris, u8 materials, u8 flags (1 smooth, 2 uv), u16 0;
per material u16 RGB565, u8 shading (0 flat, 1 gouraud, ... as VX_*), u8 alpha; per vertex int32
x, y, z; per vertex int16 u, v (1024 = one texture repeat) when flag 2; per triangle u16 a, b, c,
u8 material, u8 0. Limits: 65535 vertices and triangles, 255 materials (Vertice keeps 250 in all).
"""
import argparse
import os
import struct
import sys


def rgb565(r, g, b):
    r, g, b = (max(0, min(255, int(c * 255 + 0.5))) for c in (r, g, b))
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def read_mtl(path):
    mats, cur = {}, None
    if not path or not os.path.exists(path):
        return mats
    for line in open(path, encoding="utf-8", errors="replace"):
        p = line.split()
        if not p:
            continue
        if p[0] == "newmtl":
            cur = " ".join(p[1:])
            mats[cur] = {"kd": (0.8, 0.8, 0.8), "d": 1.0}
        elif cur and p[0] == "Kd" and len(p) >= 4:
            mats[cur]["kd"] = tuple(float(x) for x in p[1:4])
        elif cur and p[0] == "d" and len(p) >= 2:
            mats[cur]["d"] = float(p[1])
        elif cur and p[0] == "Tr" and len(p) >= 2:
            mats[cur]["d"] = 1.0 - float(p[1])
    return mats


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("obj")
    ap.add_argument("out")
    ap.add_argument("--size", type=float, help="scale so the longest side is this many world units")
    ap.add_argument("--scale", type=float, default=1.0, help="plain scale factor (if no --size)")
    ap.add_argument("--base", action="store_true", help="centre on x/z and stand the model on y = 0")
    ap.add_argument("--smooth", action="store_true", help="smooth shading (rounded); default faceted")
    ap.add_argument("--flip-v", action="store_true", help="v = 1 - v (images stored top-down)")
    ap.add_argument("--shading", type=int, default=1, help="Vertice shading for every material (1 gouraud)")
    a = ap.parse_args()

    pos, uvs, verts, vidx, tris = [], [], [], {}, []
    mtl_path, mat_names, cur = None, [], "default"
    base_dir = os.path.dirname(os.path.abspath(a.obj))
    for line in open(a.obj, encoding="utf-8", errors="replace"):
        p = line.split()
        if not p:
            continue
        if p[0] == "v":
            pos.append(tuple(float(x) for x in p[1:4]))
        elif p[0] == "vt":
            u, v = float(p[1]), float(p[2]) if len(p) > 2 else 0.0
            uvs.append((u, 1.0 - v if a.flip_v else v))
        elif p[0] == "mtllib":
            mtl_path = os.path.join(base_dir, " ".join(p[1:]))
        elif p[0] == "usemtl":
            cur = " ".join(p[1:])
        elif p[0] == "f":
            if cur not in mat_names:
                mat_names.append(cur)
            corner = []
            for tok in p[1:]:
                f = tok.split("/")
                pi = int(f[0]); pi = pi - 1 if pi > 0 else len(pos) + pi
                ti = None
                if len(f) > 1 and f[1]:
                    ti = int(f[1]); ti = ti - 1 if ti > 0 else len(uvs) + ti
                key = (pi, ti)
                if key not in vidx:
                    vidx[key] = len(verts)
                    verts.append(key)
                corner.append(vidx[key])
            for k in range(1, len(corner) - 1):          # fan
                tris.append((corner[0], corner[k], corner[k + 1], mat_names.index(cur)))
    if not tris:
        sys.exit("no faces in " + a.obj)
    if len(verts) > 65535 or len(tris) > 65535 or len(mat_names) > 255:
        sys.exit("too big for .vxm: %d verts, %d tris, %d materials" % (len(verts), len(tris), len(mat_names)))

    xs = [pos[v[0]] for v in verts]
    lo = [min(c[i] for c in xs) for i in range(3)]
    hi = [max(c[i] for c in xs) for i in range(3)]
    k = a.size / max(hi[i] - lo[i] for i in range(3)) if a.size else a.scale
    off = [-(lo[0] + hi[0]) / 2, -lo[1], -(lo[2] + hi[2]) / 2] if a.base else [0, 0, 0]
    has_uv = any(v[1] is not None for v in verts)

    mats = read_mtl(mtl_path)
    out = bytearray(b"VXM1")
    out += struct.pack("<HHBBH", len(verts), len(tris), len(mat_names), (1 if a.smooth else 0) | (2 if has_uv else 0), 0)
    for n in mat_names:
        m = mats.get(n, {"kd": (0.8, 0.8, 0.8), "d": 1.0})
        out += struct.pack("<HBB", rgb565(*m["kd"]), a.shading, max(1, min(255, int(m["d"] * 255 + 0.5))))
    for v in verts:
        x, y, z = pos[v[0]]
        out += struct.pack("<iii", round((x + off[0]) * k), round((y + off[1]) * k), round((z + off[2]) * k))
    if has_uv:
        for v in verts:
            u, w = uvs[v[1]] if v[1] is not None else (0.0, 0.0)
            out += struct.pack("<hh", max(-32768, min(32767, round(u * 1024))), max(-32768, min(32767, round(w * 1024))))
    for t in tris:
        out += struct.pack("<HHHBB", t[0], t[1], t[2], t[3], 0)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    open(a.out, "wb").write(out)
    print("%s: %d vertices, %d triangles, %d materials%s, %d bytes" %
          (a.out, len(verts), len(tris), len(mat_names), ", uv" if has_uv else "", len(out)))


if __name__ == "__main__":
    main()
