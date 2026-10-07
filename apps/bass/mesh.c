// mesh.c — Vertice Bass: a scratch mesh builder handed to vx_mesh, a tiny RNG and colour helpers.
#include "bass.h"

static int32_t  mb_xyz[MB_MAXV * 3];
static int16_t  mb_uv[MB_MAXV * 2];
static uint16_t mb_idx[MB_MAXT * 3];
static uint8_t  mb_mat[MB_MAXT];
int mb_nv, mb_nt;
uint16_t tex_buf[4096];

void mb_reset(void) { mb_nv = mb_nt = 0; }

int mb_v(float x, float y, float z, int u, int v) {
    if (mb_nv >= MB_MAXV) return MB_MAXV - 1;
    mb_xyz[mb_nv * 3] = iroundf(x); mb_xyz[mb_nv * 3 + 1] = iroundf(y); mb_xyz[mb_nv * 3 + 2] = iroundf(z);
    mb_uv[mb_nv * 2] = (int16_t)u; mb_uv[mb_nv * 2 + 1] = (int16_t)v;
    return mb_nv++;
}

// A triangle oriented to face AWAY from the inside point (ix,iy,iz).
void mb_tri(int a, int b, int c, int mat, float ix, float iy, float iz) {
    if (mb_nt >= MB_MAXT) return;
    const int32_t *A = &mb_xyz[a * 3], *B = &mb_xyz[b * 3], *C = &mb_xyz[c * 3];
    const float ux = (float)(B[0] - A[0]), uy = (float)(B[1] - A[1]), uz = (float)(B[2] - A[2]);
    const float vx = (float)(C[0] - A[0]), vy = (float)(C[1] - A[1]), vz = (float)(C[2] - A[2]);
    const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const float cx = (A[0] + B[0] + C[0]) / 3.0f - ix, cy = (A[1] + B[1] + C[1]) / 3.0f - iy,
                cz = (A[2] + B[2] + C[2]) / 3.0f - iz;
    if (nx * cx + ny * cy + nz * cz < 0) { int t = b; b = c; c = t; }
    mb_idx[mb_nt * 3] = (uint16_t)a; mb_idx[mb_nt * 3 + 1] = (uint16_t)b; mb_idx[mb_nt * 3 + 2] = (uint16_t)c;
    mb_mat[mb_nt++] = (uint8_t)mat;
}

void mb_quad(int a, int b, int c, int d, int mat, float ix, float iy, float iz) {
    mb_tri(a, b, c, mat, ix, iy, iz);
    mb_tri(a, c, d, mat, ix, iy, iz);
}

// Axis-aligned box; no bottom face (nothing here is ever seen from below).
void mb_box(float x0, float y0, float z0, float x1, float y1, float z1, int mat) {
    const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, cz = (z0 + z1) / 2;
    const int b0 = mb_v(x0, y0, z0, 0, 0), b1 = mb_v(x1, y0, z0, 0, 0), b2 = mb_v(x1, y0, z1, 0, 0),
              b3 = mb_v(x0, y0, z1, 0, 0);
    const int t0 = mb_v(x0, y1, z0, 0, 0), t1 = mb_v(x1, y1, z0, 0, 0), t2 = mb_v(x1, y1, z1, 0, 0),
              t3 = mb_v(x0, y1, z1, 0, 0);
    mb_quad(t0, t1, t2, t3, mat, cx, cy, cz);
    mb_quad(b0, b1, t1, t0, mat, cx, cy, cz);
    mb_quad(b3, b2, t2, t3, mat, cx, cy, cz);
    mb_quad(b0, b3, t3, t0, mat, cx, cy, cz);
    mb_quad(b1, b2, t2, t1, mat, cx, cy, cz);
}

// Box with texture coordinates: every face maps world units to texels (1024 = one repeat every `tile`
// units), so a texture keeps its scale on boxes of any size.
void mb_box_uv(float x0, float y0, float z0, float x1, float y1, float z1, int mat, float tile) {
    const float k = 1024.0f / tile;
    const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, cz = (z0 + z1) / 2;
#define V(x, y, z, u, v) mb_v(x, y, z, iroundf((u) * k), iroundf((v) * k))
    {   const int a = V(x0, y1, z0, x0, z0), b = V(x1, y1, z0, x1, z0), c = V(x1, y1, z1, x1, z1), d = V(x0, y1, z1, x0, z1);
        mb_quad(a, b, c, d, mat, cx, cy, cz); }                                            // top
    {   const int a = V(x0, y0, z0, x0, y0), b = V(x1, y0, z0, x1, y0), c = V(x1, y1, z0, x1, y1), d = V(x0, y1, z0, x0, y1);
        mb_quad(a, b, c, d, mat, cx, cy, cz); }                                            // z0
    {   const int a = V(x0, y0, z1, x0, y0), b = V(x1, y0, z1, x1, y0), c = V(x1, y1, z1, x1, y1), d = V(x0, y1, z1, x0, y1);
        mb_quad(a, b, c, d, mat, cx, cy, cz); }                                            // z1
    {   const int a = V(x0, y0, z0, z0, y0), b = V(x0, y0, z1, z1, y0), c = V(x0, y1, z1, z1, y1), d = V(x0, y1, z0, z0, y1);
        mb_quad(a, b, c, d, mat, cx, cy, cz); }                                            // x0
    {   const int a = V(x1, y0, z0, z0, y0), b = V(x1, y0, z1, z1, y0), c = V(x1, y1, z1, z1, y1), d = V(x1, y1, z0, z0, y1);
        mb_quad(a, b, c, d, mat, cx, cy, cz); }                                            // x1
#undef V
}

// A boulder: an irregular faceted lump (jittered base ring, narrower jittered shoulder ring, an
// off-centre crown) instead of a box, which under water read as a translucent crate. Texture wrapped
// around it (u = arc length, v = height), `tile` world units per repeat; ~21 triangles.
void mb_rock(float cx, float cz, float r, float h, int mat, float tile) {
    enum { N = 7 };
    const float k = 1024.0f / tile, circ = 6.2831853f * r;
    const float a0 = rnd(1000) * 0.006283f;
    float bx[N], bz[N], mx[N], my[N], mz[N];
    for (int j = 0; j < N; j++) {
        const float a = a0 + j * (6.2831853f / N), am = a + 0.45f;
        const float rb = r * (0.82f + rnd(34) * 0.01f), rm = r * (0.55f + rnd(25) * 0.01f);
        bx[j] = cx + sinf_(a) * rb; bz[j] = cz + cosf_(a) * rb;
        mx[j] = cx + sinf_(am) * rm; mz[j] = cz + cosf_(am) * rm; my[j] = h * (0.5f + rnd(25) * 0.01f);
    }
    int base[N + 1], mid[N + 1];
    for (int i = 0; i <= N; i++) {       // the last column repeats the first with the wrapped u
        const int j = i % N;
        const float u = (float)i / N * circ * k;
        base[i] = mb_v(bx[j], 0, bz[j], iroundf(u), 0);
        mid[i] = mb_v(mx[j], my[j], mz[j], iroundf(u + 0.06f * circ * k), iroundf(my[j] * k));
    }
    const float tx = cx + (rnd(40) - 20) * 0.01f * r, tz = cz + (rnd(40) - 20) * 0.01f * r;
    const int top = mb_v(tx, h, tz, iroundf(0.5f * circ * k), iroundf(h * 1.3f * k));
    for (int i = 0; i < N; i++) {
        mb_quad(base[i], base[i + 1], mid[i + 1], mid[i], mat, cx, h * 0.3f, cz);
        mb_tri(mid[i], mid[i + 1], top, mat, cx, h * 0.3f, cz);
    }
}

// A six-sided log from a to b, radius r, capped at both ends: bark wrapped round it (u along the
// length, v round the girth), `tile` world units per repeat; 24 triangles.
void mb_cyl(float ax, float ay, float az, float bx, float by, float bz, float r, int mat, float tile) {
    enum { N = 6 };
    const float k = 1024.0f / tile;
    float dx = bx - ax, dy = by - ay, dz = bz - az;
    const float len = sqrtf_(dx * dx + dy * dy + dz * dz);
    if (len < 1) return;
    dx /= len; dy /= len; dz /= len;
    // two unit vectors across the axis
    float px = -dz, py = 0, pz = dx;                          // horizontal, unless the axis is vertical
    if (px * px + pz * pz < 0.01f) { px = 1; py = 0; pz = 0; }
    float pl = sqrtf_(px * px + py * py + pz * pz); px /= pl; py /= pl; pz /= pl;
    const float qx = dy * pz - dz * py, qy = dz * px - dx * pz, qz = dx * py - dy * px;
    const float cx = (ax + bx) / 2, cy = (ay + by) / 2, cz = (az + bz) / 2;
    const float circ = 6.2831853f * r;
    int ra[N + 1], rb[N + 1];
    for (int i = 0; i <= N; i++) {
        const float a = (i % N) * (6.2831853f / N), c = cosf_(a) * r, sn = sinf_(a) * r;
        const float ox = px * c + qx * sn, oy = py * c + qy * sn, oz = pz * c + qz * sn;
        const int v = iroundf((float)i / N * circ * k);
        ra[i] = mb_v(ax + ox, ay + oy, az + oz, 0, v);
        rb[i] = mb_v(bx + ox, by + oy, bz + oz, iroundf(len * k), v);
    }
    for (int i = 0; i < N; i++) mb_quad(ra[i], ra[i + 1], rb[i + 1], rb[i], mat, cx, cy, cz);
    for (int i = 1; i + 1 < N; i++) {                         // end caps: the sawn faces
        mb_tri(ra[0], ra[i], ra[i + 1], mat, cx, cy, cz);
        mb_tri(rb[0], rb[i], rb[i + 1], mat, cx, cy, cz);
    }
}

int mb_commit_ex(int mat_default, int with_uv, int flags) {
    const int id = mb_nt ? vx_mesh(mb_xyz, mb_nv, mb_idx, mb_nt, with_uv ? mb_uv : 0, mb_mat, mat_default, flags) : -1;
    mb_reset();
    return id;
}
int mb_commit(int mat_default, int with_uv) { return mb_commit_ex(mat_default, with_uv, 0); }

static uint32_t rng = 0x2545F491u;
void rnd_seed(uint32_t s) { rng = s ? s : 0x2545F491u; }
int rnd(int n) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return n > 0 ? (int)(rng % (uint32_t)n) : 0;
}
uint16_t rgb(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)NV_RGB(r, g, b);
}
