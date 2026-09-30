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

int mb_commit(int mat_default, int with_uv) {
    const int id = mb_nt ? vx_mesh(mb_xyz, mb_nv, mb_idx, mb_nt, with_uv ? mb_uv : 0, mb_mat, mat_default, 0) : -1;
    mb_reset();
    return id;
}

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
