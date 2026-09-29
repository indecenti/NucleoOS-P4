// world.c — Vertice GP: mesh builder, the circuit (Catmull-Rom centreline, textured asphalt,
// kerbs, start line) and the scenery (trees, gantry, grandstand, distant mountains).
#include "game.h"

// ---- mesh builder -------------------------------------------------------------------------------
static int32_t  mb_xyz[MB_MAXV * 3];
static int16_t  mb_uv[MB_MAXV * 2];
static uint16_t mb_idx[MB_MAXT * 3];
static uint8_t  mb_mat[MB_MAXT];
int             mb_nv, mb_nt;   // read by world.c flush checks

void mb_reset(void) { mb_nv = mb_nt = 0; }

int mb_v(float x, float y, float z, int u, int v) {
    if (mb_nv >= MB_MAXV) return MB_MAXV - 1;
    mb_xyz[mb_nv * 3] = iroundf(x); mb_xyz[mb_nv * 3 + 1] = iroundf(y); mb_xyz[mb_nv * 3 + 2] = iroundf(z);
    mb_uv[mb_nv * 2] = (int16_t)u; mb_uv[mb_nv * 2 + 1] = (int16_t)v;
    return mb_nv++;
}

void mb_tri(int a, int b, int c, int mat, float ix, float iy, float iz) {
    if (mb_nt >= MB_MAXT) return;
    const int32_t *A = &mb_xyz[a * 3], *B = &mb_xyz[b * 3], *C = &mb_xyz[c * 3];
    const float ux = (float)(B[0] - A[0]), uy = (float)(B[1] - A[1]), uz = (float)(B[2] - A[2]);
    const float vx = (float)(C[0] - A[0]), vy = (float)(C[1] - A[1]), vz = (float)(C[2] - A[2]);
    const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const float cx = (A[0] + B[0] + C[0]) / 3.0f - ix, cy = (A[1] + B[1] + C[1]) / 3.0f - iy,
                cz = (A[2] + B[2] + C[2]) / 3.0f - iz;
    if (nx * cx + ny * cy + nz * cz < 0) { int t = b; b = c; c = t; }   // face away from inside
    mb_idx[mb_nt * 3] = (uint16_t)a; mb_idx[mb_nt * 3 + 1] = (uint16_t)b; mb_idx[mb_nt * 3 + 2] = (uint16_t)c;
    mb_mat[mb_nt++] = (uint8_t)mat;
}

void mb_quad(int a, int b, int c, int d, int mat, float ix, float iy, float iz) {
    mb_tri(a, b, c, mat, ix, iy, iz);
    mb_tri(a, c, d, mat, ix, iy, iz);
}

void mb_box(float x0, float y0, float z0, float x1, float y1, float z1, float tsx, float tsz0, float tsz1,
            int mat) {
    const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, cz = (z0 + z1) / 2;
    const int b0 = mb_v(x0, y0, z0, 0, 0), b1 = mb_v(x1, y0, z0, 0, 0), b2 = mb_v(x1, y0, z1, 0, 0),
              b3 = mb_v(x0, y0, z1, 0, 0);
    const int t0 = mb_v(x0 + tsx, y1, z0 + tsz0, 0, 0), t1 = mb_v(x1 - tsx, y1, z0 + tsz0, 0, 0),
              t2 = mb_v(x1 - tsx, y1, z1 - tsz1, 0, 0), t3 = mb_v(x0 + tsx, y1, z1 - tsz1, 0, 0);
    mb_quad(b0, b1, b2, b3, mat, cx, cy, cz);   // bottom
    mb_quad(t0, t1, t2, t3, mat, cx, cy, cz);   // top
    mb_quad(b0, b1, t1, t0, mat, cx, cy, cz);   // z0 side
    mb_quad(b3, b2, t2, t3, mat, cx, cy, cz);   // z1 side
    mb_quad(b0, b3, t3, t0, mat, cx, cy, cz);   // x0 side
    mb_quad(b1, b2, t2, t1, mat, cx, cy, cz);   // x1 side
}

int mb_commit(int mat_default, int with_uv) {
    const int id = mb_nt ? vx_mesh(mb_xyz, mb_nv, mb_idx, mb_nt, with_uv ? mb_uv : 0, mb_mat, mat_default, 0) : -1;
    mb_reset();
    return id;
}

// ---- the circuit ----------------------------------------------------------------------------------
TrackPt g_trk[TRACK_N];
float   g_trk_len;

// Control points (x, z) of a closed, flowing circuit; the start straight runs +X from point 0.
static const float kCP[][2] = {
    {    0, -3000 }, { 3120, -3000 }, { 4320, -1920 }, { 4080,  -240 }, { 2520,   360 },
    { 2160,  1680 }, { 3360,  2760 }, { 2160,  3600 }, {    0,  3120 }, {-1440,  3720 },
    {-3360,  3240 }, {-4080,  1560 }, {-3120,   240 }, {-4080, -1320 }, {-3240, -2760 },
};
#define NCP ((int)(sizeof kCP / sizeof kCP[0]))
#define DENSE 16

static void catmull(int i, float t, float *x, float *z) {
    const float *p0 = kCP[(i + NCP - 1) % NCP], *p1 = kCP[i], *p2 = kCP[(i + 1) % NCP], *p3 = kCP[(i + 2) % NCP];
    const float t2 = t * t, t3 = t2 * t;
    *x = 0.5f * (2 * p1[0] + (-p0[0] + p2[0]) * t + (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2 +
                 (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3);
    *z = 0.5f * (2 * p1[1] + (-p0[1] + p2[1]) * t + (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2 +
                 (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3);
}

static void track_sample(void) {
    static float dx[NCP * DENSE + 1], dz[NCP * DENSE + 1], ds[NCP * DENSE + 1];
    const int nd = NCP * DENSE;
    for (int i = 0; i <= nd; i++) catmull((i / DENSE) % NCP, (float)(i % DENSE) / DENSE, &dx[i], &dz[i]);
    ds[0] = 0;
    for (int i = 1; i <= nd; i++) {
        const float ex = dx[i] - dx[i - 1], ez = dz[i] - dz[i - 1];
        ds[i] = ds[i - 1] + sqrtf_(ex * ex + ez * ez);
    }
    g_trk_len = ds[nd];
    int j = 0;
    for (int k = 0; k < TRACK_N; k++) {      // resample at equal arc length
        const float s = g_trk_len * k / TRACK_N;
        while (j < nd - 1 && ds[j + 1] < s) j++;
        const float f = (s - ds[j]) / (ds[j + 1] - ds[j] + 1e-6f);
        g_trk[k].x = dx[j] + (dx[j + 1] - dx[j]) * f;
        g_trk[k].z = dz[j] + (dz[j + 1] - dz[j]) * f;
        g_trk[k].s = s;
    }
    for (int k = 0; k < TRACK_N; k++) {
        const TrackPt *a = &g_trk[(k + TRACK_N - 1) % TRACK_N], *b = &g_trk[(k + 1) % TRACK_N];
        float tx = b->x - a->x, tz = b->z - a->z;
        const float l = sqrtf_(tx * tx + tz * tz) + 1e-6f;
        g_trk[k].tx = tx / l; g_trk[k].tz = tz / l;
    }
}

int track_nearest(float x, float z, int hint, int range) {
    int best = hint;
    float bd = 1e30f;
    for (int d = -range; d <= range; d++) {
        const int i = ((hint + d) % TRACK_N + TRACK_N) % TRACK_N;
        const float ex = x - g_trk[i].x, ez = z - g_trk[i].z, dd = ex * ex + ez * ez;
        if (dd < bd) { bd = dd; best = i; }
    }
    return best;
}

// Right-hand side of heading (tx,tz) in this left-handed world (X right, Y up, Z forward): (tz,-tx).
void track_frame(int i, float x, float z, float *s, float *lat) {
    const TrackPt *p = &g_trk[i];
    const float ex = x - p->x, ez = z - p->z;
    float ss = p->s + ex * p->tx + ez * p->tz;
    if (ss < 0) ss += g_trk_len;
    if (ss >= g_trk_len) ss -= g_trk_len;
    *s = ss;
    *lat = ex * p->tz - ez * p->tx;
}

void track_point(float s, float lat, float *x, float *z, float *heading) {
    while (s < 0) s += g_trk_len;
    while (s >= g_trk_len) s -= g_trk_len;
    const float f = s / g_trk_len * TRACK_N;
    const int i = (int)f % TRACK_N, j = (i + 1) % TRACK_N;
    const float t = f - (int)f;
    const float cx = g_trk[i].x + (g_trk[j].x - g_trk[i].x) * t, cz = g_trk[i].z + (g_trk[j].z - g_trk[i].z) * t;
    const float tx = g_trk[i].tx + (g_trk[j].tx - g_trk[i].tx) * t, tz = g_trk[i].tz + (g_trk[j].tz - g_trk[i].tz) * t;
    *x = cx + tz * lat;
    *z = cz - tx * lat;
    if (heading) *heading = atan2f_(tx, tz);
}

static float track_dist(float x, float z) {       // distance to the centreline (coarse)
    float bd = 1e30f;
    for (int i = 0; i < TRACK_N; i++) {
        const float ex = x - g_trk[i].x, ez = z - g_trk[i].z, dd = ex * ex + ez * ez;
        if (dd < bd) bd = dd;
    }
    return sqrtf_(bd);
}

// ---- procedural textures (built in one scratch buffer; the OS copies them) --------------------------
static uint16_t tex_buf[64 * 64];
static uint32_t rng = 0x2545F491u;
static int rnd(int n) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (int)(rng % (uint32_t)n); }

// Asphalt: grey noise, white edge lines, dashed centre line; one repeat = road width x 400 units.
static int tex_asphalt(void) {
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int g = 78 + rnd(22) - (x > 20 && x < 44 && ((x * 7 + y * 3) % 11) == 0 ? 10 : 0);
            uint16_t c = NV_RGB(g, g + 2, g + 8);
            if (x == 2 || x == 3 || x == 60 || x == 61) c = NV_RGB(235, 235, 230);            // edge lines
            if ((x == 31 || x == 32) && y < 30) c = NV_RGB(240, 230, 190);                     // centre dash
            tex_buf[y * 64 + x] = c;
        }
    return vx_texture(tex_buf, 64, 64, 0);
}
static int tex_checker(int w, int h, int cell) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            tex_buf[y * w + x] = ((x / cell + y / cell) & 1) ? NV_RGB(20, 20, 20) : NV_RGB(245, 245, 245);
    return vx_texture(tex_buf, w, h, 0);
}

// 5x7 font (same glyphs as the OS) for banner textures.
static const uint8_t kFont[][7] = {
    {0,0,0,0,0,0,0},{0x0e,0x11,0x13,0x15,0x19,0x11,0x0e},{0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},
    {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f},{0x1f,0x02,0x04,0x02,0x01,0x11,0x0e},{0x02,0x06,0x0a,0x12,0x1f,0x02,0x02},
    {0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e},{0x06,0x08,0x10,0x1e,0x11,0x11,0x0e},{0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e},{0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c},
    {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11},{0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e},{0x0e,0x11,0x10,0x10,0x10,0x11,0x0e},
    {0x1c,0x12,0x11,0x11,0x11,0x12,0x1c},{0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f},{0x1f,0x10,0x10,0x1e,0x10,0x10,0x10},
    {0x0e,0x11,0x10,0x17,0x11,0x11,0x0f},{0x11,0x11,0x11,0x1f,0x11,0x11,0x11},{0x0e,0x04,0x04,0x04,0x04,0x04,0x0e},
    {0x07,0x02,0x02,0x02,0x12,0x12,0x0c},{0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1f},
    {0x11,0x1b,0x15,0x11,0x11,0x11,0x11},{0x11,0x19,0x15,0x13,0x11,0x11,0x11},{0x0e,0x11,0x11,0x11,0x11,0x11,0x0e},
    {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10},{0x0e,0x11,0x11,0x11,0x15,0x12,0x0d},{0x1e,0x11,0x11,0x1e,0x14,0x12,0x11},
    {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e},{0x1f,0x04,0x04,0x04,0x04,0x04,0x04},{0x11,0x11,0x11,0x11,0x11,0x11,0x0e},
    {0x11,0x11,0x11,0x11,0x11,0x0a,0x04},{0x11,0x11,0x11,0x15,0x15,0x1b,0x11},{0x11,0x11,0x0a,0x04,0x0a,0x11,0x11},
    {0x11,0x11,0x0a,0x04,0x04,0x04,0x04},{0x1f,0x01,0x02,0x04,0x08,0x10,0x1f},
};
static const uint8_t *glyph(char c) {
    if (c >= '0' && c <= '9') return kFont[1 + c - '0'];
    if (c >= 'A' && c <= 'Z') return kFont[11 + c - 'A'];
    return kFont[0];
}
// Text centred in a 128x16 texture, 2x glyphs.
static int tex_banner(const char *s, uint16_t bg, uint16_t fg, uint16_t accent) {
    const int W = 128, H = 16;
    int n = 0;
    while (s[n]) n++;
    const int x0 = (W - n * 12) / 2;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) tex_buf[y * W + x] = (y < 1 || y > 14) ? accent : bg;
    for (int i = 0; i < n; i++) {
        const uint8_t *g = glyph(s[i]);
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] & (0x10 >> c))
                    for (int dy = 0; dy < 2; dy++)
                        for (int dx = 0; dx < 2; dx++) {
                            const int px = x0 + i * 12 + c * 2 + dx, py = 1 + r * 2 + dy;
                            if (px >= 0 && px < W) tex_buf[py * W + px] = fg;
                        }
    }
    return vx_texture(tex_buf, W, H, 0);
}

// ---- building the world -------------------------------------------------------------------------------
int g_fx_dust, g_fx_smoke, g_fx_spark, g_fx_confetti;

static void build_road(void) {
    const int asphalt = vx_material(0xFFFF, VX_GOURAUD, 255, tex_asphalt(), 0);
    const float y = 2.0f;
    for (int k = 0; k < TRACK_N; k++) {
        const TrackPt *a = &g_trk[k], *b = &g_trk[(k + 1) % TRACK_N];
        const float seg = (k + 1 < TRACK_N ? b->s : g_trk_len) - a->s;
        const int v0 = (int)(a->s * 1024.0f / 400.0f) & 1023, v1 = v0 + (int)(seg * 1024.0f / 400.0f);
        const int l0 = mb_v(a->x - a->tz * ROAD_HW, y, a->z + a->tx * ROAD_HW, 0, v0);
        const int r0 = mb_v(a->x + a->tz * ROAD_HW, y, a->z - a->tx * ROAD_HW, 1024, v0);
        const int l1 = mb_v(b->x - b->tz * ROAD_HW, y, b->z + b->tx * ROAD_HW, 0, v1);
        const int r1 = mb_v(b->x + b->tz * ROAD_HW, y, b->z - b->tx * ROAD_HW, 1024, v1);
        mb_quad(l0, r0, r1, l1, asphalt, a->x, y - 1000, a->z);
    }
    vx_obj_depth(mb_commit(asphalt, 1), 6, 0);

    // Kerbs on the corners only (tangent turning > ~3.5 deg per sample), red/white blocks.
    const int red = vx_material(NV_RGB(215, 35, 35), VX_FLAT, 255, -1, 0);
    const int white = vx_material(NV_RGB(240, 240, 240), VX_FLAT, 255, -1, 0);
    for (int side = -1; side <= 1; side += 2) {
        for (int k = 0; k < TRACK_N; k++) {
            const TrackPt *p = &g_trk[(k + TRACK_N - 1) % TRACK_N], *a = &g_trk[k], *b = &g_trk[(k + 1) % TRACK_N];
            const float turn = p->tx * b->tz - p->tz * b->tx;       // sin of the heading change
            if (fabsf_(turn) < 0.06f) continue;
            const float in = ROAD_HW - 12, out = ROAD_HW + 30, y = 4.0f;
            if (mb_nv + 8 > MB_MAXV || mb_nt + 4 > MB_MAXT) vx_obj_depth(mb_commit(red, 0), 10, 0);
            for (int h = 0; h < 2; h++) {                            // two blocks per sample
                const float f0 = h * 0.5f, f1 = f0 + 0.5f;
                const float ax = a->x + (b->x - a->x) * f0, az = a->z + (b->z - a->z) * f0;
                const float bx = a->x + (b->x - a->x) * f1, bz = a->z + (b->z - a->z) * f1;
                const float tx = a->tx, tz = a->tz;
                const int q0 = mb_v(ax + side * tz * in, y, az - side * tx * in, 0, 0);
                const int q1 = mb_v(ax + side * tz * out, y, az - side * tx * out, 0, 0);
                const int q2 = mb_v(bx + side * tz * out, y, bz - side * tx * out, 0, 0);
                const int q3 = mb_v(bx + side * tz * in, y, bz - side * tx * in, 0, 0);
                mb_quad(q0, q1, q2, q3, ((k * 2 + h) & 1) ? red : white, ax, y - 1000, az);
            }
        }
        vx_obj_depth(mb_commit(red, 0), 10, 0);
    }

    // Start / finish line across the road at s = 0.
    const int chk = vx_material(0xFFFF, VX_UNLIT, 255, tex_checker(32, 8, 4), 0);
    const TrackPt *a = &g_trk[0];
    const float d = 36.0f, y2 = 5.0f;
    const int s0 = mb_v(a->x - a->tz * ROAD_HW - a->tx * d, y2, a->z + a->tx * ROAD_HW - a->tz * d, 0, 0);
    const int s1 = mb_v(a->x + a->tz * ROAD_HW - a->tx * d, y2, a->z - a->tx * ROAD_HW - a->tz * d, 1024, 0);
    const int s2 = mb_v(a->x + a->tz * ROAD_HW + a->tx * d, y2, a->z - a->tx * ROAD_HW + a->tz * d, 1024, 1024);
    const int s3 = mb_v(a->x - a->tz * ROAD_HW + a->tx * d, y2, a->z + a->tx * ROAD_HW + a->tz * d, 0, 1024);
    mb_quad(s0, s1, s2, s3, chk, a->x, y2 - 1000, a->z);
    vx_obj_depth(mb_commit(chk, 1), 14, 0);
}

static void build_tree(int leaf, int trunk, float r, float h) {
    const int n = 6;
    const int top = mb_v(0, h, 0, 0, 0);
    int ring[6];
    for (int i = 0; i < n; i++) {
        const float a = i * 2 * PI_F / n;
        ring[i] = mb_v(cosf_(a) * r, h * 0.22f, sinf_(a) * r, 0, 0);
    }
    for (int i = 0; i < n; i++) mb_tri(top, ring[i], ring[(i + 1) % n], leaf, 0, h * 0.4f, 0);
    const int bot = mb_v(0, h * 0.22f - 1, 0, 0, 0);
    for (int i = 0; i < n; i++) mb_tri(bot, ring[i], ring[(i + 1) % n], leaf, 0, h * 0.4f, 0);
    mb_box(-14, 0, -14, 14, h * 0.24f, 14, 0, 0, 0, trunk);
}

static void build_scenery(void) {
    // Trees: prototypes, then clones scattered beside (never on) the track.
    const int leaf1 = vx_material(NV_RGB(40, 120, 50), VX_FLAT, 255, -1, 0);
    const int leaf2 = vx_material(NV_RGB(70, 145, 45), VX_FLAT, 255, -1, 0);
    const int trunk = vx_material(NV_RGB(110, 75, 45), VX_FLAT, 255, -1, 0);
    build_tree(leaf1, trunk, 120, 420);
    const int proto1 = mb_commit(leaf1, 0);
    build_tree(leaf2, trunk, 95, 330);
    const int proto2 = mb_commit(leaf2, 0);
    int placed = 0;
    for (int gz = -5400; gz <= 5400 && placed < 90; gz += 620)
        for (int gx = -6000; gx <= 6000 && placed < 90; gx += 620) {
            const float x = gx + rnd(400) - 200, z = gz + rnd(400) - 200;
            const float d = track_dist(x, z);
            if (d < ROAD_HW + 260 || d > 2400 || rnd(100) < 45) continue;
            const int t = placed == 0 ? proto1 : placed == 1 ? proto2 : vx_clone((placed & 1) ? proto2 : proto1);
            if (t < 0) break;
            vx_obj_pos(t, (int)x, 0, (int)z);
            vx_obj_rot(t, 0, rnd(360), 0);
            placed++;
        }

    // Start gantry over the line: pillars, a beam and the banner on both faces.
    const TrackPt *a = &g_trk[0];
    const int steel = vx_material(NV_RGB(70, 75, 90), VX_GOURAUD, 255, -1, 0);
    const int banner = vx_material(0xFFFF, VX_UNLIT, 255,
                                   tex_banner("VERTICE GP", NV_RGB(20, 24, 60), NV_RGB(255, 210, 40), NV_RGB(230, 40, 40)), 0);
    const float w = ROAD_HW + 60;
    for (int side = -1; side <= 1; side += 2) {
        const float px = a->x + side * a->tz * w, pz = a->z - side * a->tx * w;
        mb_box(px - 16, 0, pz - 16, px + 16, 360, pz + 16, 0, 0, 0, steel);
    }
    // The beam (the start straight runs along +X, so an axis-aligned box spans the road).
    mb_box(a->x - 20, 300, a->z - w - 16, a->x + 20, 372, a->z + w + 16, 0, 0, 0, steel);
    const int beam = mb_commit(steel, 0);
    (void)beam;
    for (int face = -1; face <= 1; face += 2) {           // banner quads just proud of both faces
        const float x = a->x + face * 21.0f;
        const int b0 = mb_v(x, 306, a->z - w, face > 0 ? 0 : 1024, 1024);
        const int b1 = mb_v(x, 366, a->z - w, face > 0 ? 0 : 1024, 0);
        const int b2 = mb_v(x, 366, a->z + w, face > 0 ? 1024 : 0, 0);
        const int b3 = mb_v(x, 306, a->z + w, face > 0 ? 1024 : 0, 1024);
        mb_quad(b0, b1, b2, b3, banner, a->x, 336, a->z);
    }
    mb_commit(banner, 1);

    // Grandstand outside the start straight (-Z side): stepped rows of a colourful crowd.
    const int crowd[5] = {
        vx_material(NV_RGB(230, 70, 60), VX_FLAT, 255, -1, 0), vx_material(NV_RGB(60, 110, 230), VX_FLAT, 255, -1, 0),
        vx_material(NV_RGB(245, 205, 60), VX_FLAT, 255, -1, 0), vx_material(NV_RGB(235, 235, 235), VX_FLAT, 255, -1, 0),
        vx_material(NV_RGB(70, 170, 90), VX_FLAT, 255, -1, 0),
    };
    const int concrete = vx_material(NV_RGB(150, 150, 160), VX_GOURAUD, 255, -1, 0);
    const float z0 = a->z - ROAD_HW - 260;
    for (int row = 0; row < 5; row++) {
        for (int blk = 0; blk < 6; blk++) {
            const float x0 = 300 + blk * 260, zr = z0 - row * 70;
            mb_box(x0, 0, zr - 70, x0 + 250, 40 + row * 45, zr, 0, 0, 0, crowd[(row + blk * 3) % 5]);
        }
        mb_commit(concrete, 0);                  // one object per row (fits the scratch buffer)
    }
    mb_box(290, 0, z0 - 420, 1870, 300, z0 - 350, 0, 0, 0, concrete);          // back wall
    mb_commit(concrete, 0);
    const int nb = vx_material(0xFFFF, VX_UNLIT, 255,
                               tex_banner("NUCLEO OS", NV_RGB(240, 240, 245), NV_RGB(30, 60, 170), NV_RGB(30, 60, 170)), 0);
    const int q0 = mb_v(700, 300, z0 - 349, 0, 0), q1 = mb_v(1460, 300, z0 - 349, 1024, 0);
    const int q2 = mb_v(1460, 380, z0 - 349, 1024, 1024), q3 = mb_v(700, 380, z0 - 349, 0, 1024);
    mb_quad(q0, q1, q2, q3, nb, 1080, 340, z0 - 800);
    mb_commit(nb, 1);

    // Distant mountains, softened by the fog.
    const int rock1 = vx_material(NV_RGB(105, 110, 135), VX_GOURAUD, 255, -1, 0);
    const int rock2 = vx_material(NV_RGB(125, 125, 145), VX_GOURAUD, 255, -1, 0);
    for (int i = 0; i < 12; i++) {
        const float ang = i * 2 * PI_F / 12 + (float)rnd(20) / 100.0f, r = 12500 + rnd(2500);
        const int m = vx_prim(VX_PYRAMID, 4200 + rnd(2400), 1800 + rnd(2200), 0, (i & 1) ? rock1 : rock2, -1);
        vx_obj_pos(m, (int)(cosf_(ang) * r), 0, (int)(sinf_(ang) * r));
        vx_obj_rot(m, 0, rnd(90), 0);
    }
}

void world_build(void) {
    track_sample();
    vx_sky(NV_RGB(38, 92, 190), NV_RGB(196, 214, 236));
    vx_sun(215, 52, 0xFFF4E0, 235);
    vx_ambient(0x46505E);
    vx_lens(70, 24, 17000);
    vx_fog(6500, 16000);
    const int grass1 = vx_material(NV_RGB(76, 140, 58), VX_FLAT, 255, -1, 0);
    const int grass2 = vx_material(NV_RGB(88, 152, 64), VX_FLAT, 255, -1, 0);
    vx_prim(VX_GRID, 44000, 44000, 22, grass1, grass2);
    build_road();
    build_scenery();
    // Effects: dust (off track), tyre smoke (hard braking), sparks (contact), confetti (finish).
    g_fx_dust = vx_emitter(160, NV_RGB(180, 150, 100), NV_RGB(150, 130, 100), 30, 130, 900, -20, 0);
    g_fx_smoke = vx_emitter(160, NV_RGB(230, 230, 230), NV_RGB(170, 170, 175), 26, 150, 1100, -30, 0);
    g_fx_spark = vx_emitter(96, NV_RGB(255, 240, 150), NV_RGB(255, 80, 0), 14, 4, 450, 900, VX_PART_ADDITIVE);
    g_fx_confetti = vx_emitter(256, NV_RGB(255, 220, 60), NV_RGB(255, 60, 140), 18, 14, 2600, 260, 0);
}
