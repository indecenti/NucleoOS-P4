// vx_build.h — a mesh builder for the Vertice engine (header-only, needs nucleo_sdk.h + nv_math.h).
//
// Build geometry in a scratch buffer, then hand it to vx_mesh in one call. Triangles are oriented
// automatically: every face is given an "inside" point and turned to face away from it, so shapes
// can be written without thinking about winding. UVs are in world units (`tile` = world units per
// texture repeat), so a texture keeps its scale on shapes of any size. Shapes: quads, boxes (with or
// without UVs), tapered limbs / cylinders, lathe (surface of revolution), rocks, flat discs and
// upright billboard quads facing a point. One builder per C file (the buffers are static).
//
//   vxb_reset();
//   vxb_box_uv(-50, 0, -50, 50, 80, 50, wall, 100);
//   vxb_limb(0, 80, 0, 12, 0, 220, 0, 6, 8, VXB_CAP_B, wood, 120);
//   int house = vxb_commit(wall, 1, 0);           // -> vx_mesh handle (or -1)
//
// Size the buffers before including: #define VXB_MAXV 2048 / VXB_MAXT 2048 (defaults 1024 each).
// The engine merges faces per material: build a whole decor group into one mesh, but split huge
// rings of decor into sectors so the far ones can be culled (see docs/VERTICE.md "Culling").
#pragma once
#include "nucleo_sdk.h"
#include "nv_math.h"

#ifndef VXB_MAXV
#define VXB_MAXV 1024
#endif
#ifndef VXB_MAXT
#define VXB_MAXT 1024
#endif

static int32_t  vxb_xyz[VXB_MAXV * 3];
static int16_t  vxb_uvs[VXB_MAXV * 2];
static uint16_t vxb_idx[VXB_MAXT * 3];
static uint8_t  vxb_mats[VXB_MAXT];
static int vxb_nv, vxb_nt, vxb_full;

static inline void vxb_reset(void) { vxb_nv = vxb_nt = vxb_full = 0; }
// Room left (vertices / triangles): commit before a big shape if it would not fit.
static inline int vxb_room_v(void) { return VXB_MAXV - vxb_nv; }
static inline int vxb_room_t(void) { return VXB_MAXT - vxb_nt; }

// A vertex; u, v in texture units (1024 = one repeat). Returns its index.
static inline int vxb_v(float x, float y, float z, int u, int v) {
    if (vxb_nv >= VXB_MAXV) { vxb_full = 1; return VXB_MAXV - 1; }
    vxb_xyz[vxb_nv * 3] = nv_roundi(x); vxb_xyz[vxb_nv * 3 + 1] = nv_roundi(y); vxb_xyz[vxb_nv * 3 + 2] = nv_roundi(z);
    vxb_uvs[vxb_nv * 2] = (int16_t)u; vxb_uvs[vxb_nv * 2 + 1] = (int16_t)v;
    return vxb_nv++;
}
// A triangle turned to face AWAY from the inside point (ix, iy, iz).
static inline void vxb_tri(int a, int b, int c, int mat, float ix, float iy, float iz) {
    if (vxb_nt >= VXB_MAXT) { vxb_full = 1; return; }
    const int32_t *A = &vxb_xyz[a * 3], *B = &vxb_xyz[b * 3], *C = &vxb_xyz[c * 3];
    const float ux = (float)(B[0] - A[0]), uy = (float)(B[1] - A[1]), uz = (float)(B[2] - A[2]);
    const float vx = (float)(C[0] - A[0]), vy = (float)(C[1] - A[1]), vz = (float)(C[2] - A[2]);
    const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const float cx = (A[0] + B[0] + C[0]) / 3.0f - ix, cy = (A[1] + B[1] + C[1]) / 3.0f - iy,
                cz = (A[2] + B[2] + C[2]) / 3.0f - iz;
    if (nx * cx + ny * cy + nz * cz < 0) { const int t = b; b = c; c = t; }
    vxb_idx[vxb_nt * 3] = (uint16_t)a; vxb_idx[vxb_nt * 3 + 1] = (uint16_t)b; vxb_idx[vxb_nt * 3 + 2] = (uint16_t)c;
    vxb_mats[vxb_nt++] = (uint8_t)mat;
}
static inline void vxb_quad(int a, int b, int c, int d, int mat, float ix, float iy, float iz) {
    vxb_tri(a, b, c, mat, ix, iy, iz);
    vxb_tri(a, c, d, mat, ix, iy, iz);
}

// Axis-aligned box. bottom = 0 leaves the underside out (most things are never seen from below).
static inline void vxb_box(float x0, float y0, float z0, float x1, float y1, float z1, int mat, int bottom) {
    const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, cz = (z0 + z1) / 2;
    const int b0 = vxb_v(x0, y0, z0, 0, 0), b1 = vxb_v(x1, y0, z0, 0, 0), b2 = vxb_v(x1, y0, z1, 0, 0), b3 = vxb_v(x0, y0, z1, 0, 0);
    const int t0 = vxb_v(x0, y1, z0, 0, 0), t1 = vxb_v(x1, y1, z0, 0, 0), t2 = vxb_v(x1, y1, z1, 0, 0), t3 = vxb_v(x0, y1, z1, 0, 0);
    vxb_quad(t0, t1, t2, t3, mat, cx, cy, cz);
    vxb_quad(b0, b1, t1, t0, mat, cx, cy, cz);
    vxb_quad(b3, b2, t2, t3, mat, cx, cy, cz);
    vxb_quad(b0, b3, t3, t0, mat, cx, cy, cz);
    vxb_quad(b1, b2, t2, t1, mat, cx, cy, cz);
    if (bottom) vxb_quad(b0, b1, b2, b3, mat, cx, cy, cz);
}
// Box with UVs in world units: `tile` world units per texture repeat on every face.
static inline void vxb_box_uv(float x0, float y0, float z0, float x1, float y1, float z1, int mat, float tile) {
    const float k = 1024.0f / tile, cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, cz = (z0 + z1) / 2;
#define VXB_V(x, y, z, u, v) vxb_v(x, y, z, nv_roundi((u) * k), nv_roundi((v) * k))
    { const int a = VXB_V(x0, y1, z0, x0, z0), b = VXB_V(x1, y1, z0, x1, z0), c = VXB_V(x1, y1, z1, x1, z1), d = VXB_V(x0, y1, z1, x0, z1);
      vxb_quad(a, b, c, d, mat, cx, cy, cz); }
    { const int a = VXB_V(x0, y0, z0, x0, y0), b = VXB_V(x1, y0, z0, x1, y0), c = VXB_V(x1, y1, z0, x1, y1), d = VXB_V(x0, y1, z0, x0, y1);
      vxb_quad(a, b, c, d, mat, cx, cy, cz); }
    { const int a = VXB_V(x0, y0, z1, x0, y0), b = VXB_V(x1, y0, z1, x1, y0), c = VXB_V(x1, y1, z1, x1, y1), d = VXB_V(x0, y1, z1, x0, y1);
      vxb_quad(a, b, c, d, mat, cx, cy, cz); }
    { const int a = VXB_V(x0, y0, z0, z0, y0), b = VXB_V(x0, y0, z1, z1, y0), c = VXB_V(x0, y1, z1, z1, y1), d = VXB_V(x0, y1, z0, z0, y1);
      vxb_quad(a, b, c, d, mat, cx, cy, cz); }
    { const int a = VXB_V(x1, y0, z0, z0, y0), b = VXB_V(x1, y0, z1, z1, y0), c = VXB_V(x1, y1, z1, z1, y1), d = VXB_V(x1, y1, z0, z0, y1);
      vxb_quad(a, b, c, d, mat, cx, cy, cz); }
#undef VXB_V
}

// A tapered limb from a (radius ra) to b (radius rb), n sides (3..12): trunks, branches, pipes,
// columns, legs. The texture runs along it. caps: VXB_CAP_A / VXB_CAP_B close an end (own vertices,
// so a smooth mesh keeps the edge).
#define VXB_CAP_A 1
#define VXB_CAP_B 2
static inline void vxb_limb(float ax, float ay, float az, float ra, float bx, float by, float bz, float rb,
                            int n, int caps, int mat, float tile) {
    if (n < 3) n = 3;
    if (n > 12) n = 12;
    const float k = 1024.0f / tile;
    float dx = bx - ax, dy = by - ay, dz = bz - az;
    const float len = nv_sqrtf(dx * dx + dy * dy + dz * dz);
    if (len < 1) return;
    dx /= len; dy /= len; dz /= len;
    float px = -dz, py = 0, pz = dx;
    if (px * px + pz * pz < 0.01f) { px = 1; py = 0; pz = 0; }
    const float pl = nv_sqrtf(px * px + py * py + pz * pz); px /= pl; py /= pl; pz /= pl;
    const float qx = dy * pz - dz * py, qy = dz * px - dx * pz, qz = dx * py - dy * px;
    const float cx = (ax + bx) / 2, cy = (ay + by) / 2, cz = (az + bz) / 2, circ = 2 * NV_PI * (ra + rb) / 2;
    int va[13], vb[13];
    float o[12][3];
    for (int i = 0; i <= n; i++) {
        const float a = (i % n) * (2 * NV_PI / n), c = nv_cosf(a), s = nv_sinf(a);
        const float ox = px * c + qx * s, oy = py * c + qy * s, oz = pz * c + qz * s;
        const int v = nv_roundi((float)i / n * circ * k);
        va[i] = vxb_v(ax + ox * ra, ay + oy * ra, az + oz * ra, 0, v);
        vb[i] = vxb_v(bx + ox * rb, by + oy * rb, bz + oz * rb, nv_roundi(len * k), v);
        if (i < n) { o[i][0] = ox; o[i][1] = oy; o[i][2] = oz; }
    }
    for (int i = 0; i < n; i++) vxb_quad(va[i], va[i + 1], vb[i + 1], vb[i], mat, cx, cy, cz);
    for (int e = 0; e < 2; e++) {
        if (!(caps & (1 << e))) continue;
        const float r = e ? rb : ra, ex = e ? bx : ax, ey = e ? by : ay, ez = e ? bz : az;
        int v[12];
        for (int i = 0; i < n; i++) {
            const float a = i * (2 * NV_PI / n);
            v[i] = vxb_v(ex + o[i][0] * r, ey + o[i][1] * r, ez + o[i][2] * r,
                         nv_roundi((0.5f + 0.5f * nv_cosf(a)) * r * 2 * k), nv_roundi((0.5f + 0.5f * nv_sinf(a)) * r * 2 * k));
        }
        for (int i = 1; i + 1 < n; i++) vxb_tri(v[0], v[i], v[i + 1], mat, cx, cy, cz);
    }
}

// A surface of revolution round the vertical axis at (cx, cz): `np` profile points (radius r[i] at
// height y[i], bottom to top), `n` sides: vases, bottles, towers, buoys, mushrooms, trophies.
static inline void vxb_lathe(float cx, float cz, const float *r, const float *y, int np, int n, int mat, float tile) {
    if (np < 2 || n < 3) return;
    if (n > 24) n = 24;
    const float k = 1024.0f / tile;
    float ymid = 0;
    for (int i = 0; i < np; i++) ymid += y[i] / np;
    int prev[25];
    for (int j = 0; j < np; j++) {
        int cur[25];
        float rmax = 0;
        for (int i = 0; i < np; i++) rmax = nv_maxf(rmax, r[i]);
        for (int i = 0; i <= n; i++) {
            const float a = (i % n) * (2 * NV_PI / n);
            cur[i] = vxb_v(cx + nv_sinf(a) * r[j], y[j], cz + nv_cosf(a) * r[j],
                           nv_roundi((float)i / n * 2 * NV_PI * rmax * k), nv_roundi(y[j] * k));
        }
        if (j) for (int i = 0; i < n; i++) vxb_quad(prev[i], prev[i + 1], cur[i + 1], cur[i], mat, cx, ymid, cz);
        for (int i = 0; i <= n; i++) prev[i] = cur[i];
    }
}

// An irregular boulder (seeded): jittered base and shoulder rings and an off-centre crown, 18 faces.
static inline void vxb_rock(NvRand *rnd, float cx, float cz, float r, float h, int mat, float tile) {
    enum { N = 6 };
    const float k = 1024.0f / tile, circ = 2 * NV_PI * r, a0 = nv_rand_float(rnd) * 2 * NV_PI;
    float bx[N], bz[N], mx[N], my[N], mz[N];
    for (int j = 0; j < N; j++) {
        const float a = a0 + j * (2 * NV_PI / N), am = a + 0.45f;
        const float rb = r * nv_rand_range(rnd, 0.82f, 1.16f), rm = r * nv_rand_range(rnd, 0.55f, 0.8f);
        bx[j] = cx + nv_sinf(a) * rb; bz[j] = cz + nv_cosf(a) * rb;
        mx[j] = cx + nv_sinf(am) * rm; mz[j] = cz + nv_cosf(am) * rm; my[j] = h * nv_rand_range(rnd, 0.5f, 0.75f);
    }
    int base[N + 1], mid[N + 1];
    for (int i = 0; i <= N; i++) {
        const int j = i % N;
        const float u = (float)i / N * circ * k;
        base[i] = vxb_v(bx[j], 0, bz[j], nv_roundi(u), 0);
        mid[i] = vxb_v(mx[j], my[j], mz[j], nv_roundi(u + 0.06f * circ * k), nv_roundi(my[j] * k));
    }
    const float tx = cx + nv_rand_range(rnd, -0.2f, 0.2f) * r, tz = cz + nv_rand_range(rnd, -0.2f, 0.2f) * r;
    const int top = vxb_v(tx, h, tz, nv_roundi(0.5f * circ * k), nv_roundi(h * 1.3f * k));
    for (int i = 0; i < N; i++) {
        vxb_quad(base[i], base[i + 1], mid[i + 1], mid[i], mat, cx, h * 0.3f, cz);
        vxb_tri(mid[i], mid[i + 1], top, mat, cx, h * 0.3f, cz);
    }
}

// A flat disc at height y facing up (n sides): a blob shadow under a car or a character (give it a
// dark translucent unlit material and move it with the object), a pad, a puddle.
static inline void vxb_disc(float cx, float y, float cz, float r, int n, int mat) {
    if (n < 3) n = 3;
    if (n > 24) n = 24;
    const int c = vxb_v(cx, y, cz, 512, 512);
    int first = -1, prev = -1;
    for (int i = 0; i <= n; i++) {
        const float a = (i % n) * (2 * NV_PI / n);
        const int v = vxb_v(cx + nv_sinf(a) * r, y, cz + nv_cosf(a) * r,
                            nv_roundi(512 + 512 * nv_sinf(a)), nv_roundi(512 + 512 * nv_cosf(a)));
        if (i == 0) first = v; else vxb_tri(c, prev, v, mat, cx, y - 10, cz);
        prev = v;
    }
    (void)first;
}

// An upright textured quad standing on (x, y, z), `w` wide and `h` tall, turned to face the point
// (fx, fz) - a ring of trees or reeds round a lake all facing its middle is one mesh this way, not
// hundreds of billboard objects. The texture's row 0 sits at the foot (as billboard textures).
static inline void vxb_facing_quad(float x, float y, float z, float w, float h, float fx, float fz, int mat) {
    float dx = fx - x, dz = fz - z;
    const float l = nv_sqrtf(dx * dx + dz * dz);
    if (l < 1e-3f) return;
    dx /= l; dz /= l;
    const float tx = dz * w * 0.5f, tz = -dx * w * 0.5f;
    const int a = vxb_v(x - tx, y, z - tz, 0, 0), b = vxb_v(x + tx, y, z + tz, 1024, 0);
    const int c = vxb_v(x + tx, y + h, z + tz, 1024, 1024), d = vxb_v(x - tx, y + h, z - tz, 0, 1024);
    vxb_quad(a, b, c, d, mat, x - dx * w, y + h * 0.5f, z - dz * w);   // inside point behind it: faces (fx, fz)
}

// Hand the built geometry to the engine (and clear the builder). with_uv: pass the UVs (textured
// materials); flags: VX_MESH_SMOOTH for rounded shading. Returns the object handle or -1 (nothing
// built, or a full buffer: check vxb_room_* / raise VXB_MAXV, VXB_MAXT).
static inline int vxb_commit(int mat_default, int with_uv, int flags) {
    const int id = vxb_nt && !vxb_full
                 ? vx_mesh(vxb_xyz, vxb_nv, vxb_idx, vxb_nt, with_uv ? vxb_uvs : 0, vxb_mats, mat_default, flags) : -1;
    vxb_reset();
    return id;
}
