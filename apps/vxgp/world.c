// world.c — Vertice GP: mesh builder, the circuit (Catmull-Rom centreline, textured asphalt,
// kerbs, start line, boost pads, coins) and the scenery. Everything that can be a trick instead
// of triangles is one: the ground is the engine's Mode-7 floor, the mountains and clouds a 360°
// panorama, the trees camera-facing impostors, and all the road layers are drawn as a painter's
// background (no depth, no z-fighting at the edges).
#include "game.h"

// ---- mesh builder -------------------------------------------------------------------------------
static int32_t  mb_xyz[MB_MAXV * 3];
static int16_t  mb_uv[MB_MAXV * 2];
static uint16_t mb_idx[MB_MAXT * 3];
static uint8_t  mb_mat[MB_MAXT];
int             mb_nv, mb_nt;
int             mb_no_bottom;   // read by the flush checks below and in cars.c

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
    if (y0 > 0.5f && !mb_no_bottom) mb_quad(b0, b1, b2, b3, mat, cx, cy, cz);   // bottom (not on the ground)
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

// Painter's background: drawn first, in creation order, no depth test or write. Ground-level
// layers (road, kerbs, lines, pads, shadows) stack exactly as created — never a z-fight.
static void background(int id) { vx_obj_depth(id, 0, VX_DEPTH_NOTEST | VX_DEPTH_NOWRITE); }

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

float g_trk_bend[TRACK_N];

static float dist2_to(int i, float x, float z) {
    i = (i % TRACK_N + TRACK_N) % TRACK_N;
    const float ex = x - g_trk[i].x, ez = z - g_trk[i].z;
    return ex * ex + ez * ez;
}

int track_nearest(float x, float z, int hint, int range) {
    if (range > 8) {                               // a real search (grid, respawn)
        int best = hint;
        float bd = 1e30f;
        for (int d = -range; d <= range; d++) {
            const float dd = dist2_to(hint + d, x, z);
            if (dd < bd) { bd = dd; best = hint + d; }
        }
        return (best % TRACK_N + TRACK_N) % TRACK_N;
    }
    // Per frame a kart moves less than a sample: walk downhill from the last one (2-4 distances
    // instead of 25), at most `range` steps.
    int i = hint;
    float bd = dist2_to(i, x, z);
    for (int step = 0; step < range; step++) {
        const float f = dist2_to(i + 1, x, z), b = dist2_to(i - 1, x, z);
        if (f < bd && f <= b) { bd = f; i++; }
        else if (b < bd) { bd = b; i--; }
        else break;
    }
    return (i % TRACK_N + TRACK_N) % TRACK_N;
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

float track_curvature(float s) {          // heading change per unit length around s (rad/unit)
    float h0, h1, x, z;
    track_point(s - 150, 0, &x, &z, &h0);
    track_point(s + 150, 0, &x, &z, &h1);
    return wrap_pi(h1 - h0) / 300.0f;
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
Solid g_solid[MAX_SOLIDS];
int   g_nsolid;
static void solid_circle(float x, float z, float r) {
    if (g_nsolid < MAX_SOLIDS) { Solid *o = &g_solid[g_nsolid++]; o->x0 = x; o->z0 = z; o->x1 = x; o->z1 = z; o->r = r; }
}
static void solid_box(float x0, float z0, float x1, float z1) {
    if (g_nsolid < MAX_SOLIDS) { Solid *o = &g_solid[g_nsolid++]; o->x0 = x0; o->z0 = z0; o->x1 = x1; o->z1 = z1; o->r = 0; }
}

int rnd(int n) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (int)(rng % (uint32_t)n); }
static uint16_t rgb(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)NV_RGB(r, g, b);
}
#define KEY 0xF81F

// Asphalt: fine grey grain, two darker racing-line bands, 3-texel white edge lines, a dashed
// centre line. One repeat = the road width across, 400 units along.
static int tex_asphalt(void) {
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const int band = (x > 12 && x < 22) || (x > 42 && x < 52);   // rubbered-in lines
            int g = 92 + rnd(16) - (band ? 9 : 0) - ((x * 5 + y * 3) % 17 == 0 ? 7 : 0);
            uint16_t c = rgb(g, g + 2, g + 7);
            if (x <= 3 || x >= 60) c = rgb(236, 236, 230);                        // edge lines
            if ((x == 31 || x == 32) && y < 28) c = rgb(245, 232, 180);           // centre dash
            tex_buf[y * 64 + x] = c;
        }
    return vx_texture(tex_buf, 64, 64, 0);
}

// Mowed grass for the Mode-7 floor: two tones in wide stripes, speckles, a few clover patches.
static int tex_grass(void) {
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const int stripe = (y / 16) & 1;
            int r = stripe ? 70 : 86, g = stripe ? 150 : 166, b = stripe ? 52 : 60;
            const int n = rnd(18) - 9;
            if (rnd(40) == 0) { r += 30; g += 26; }                 // lighter blades
            if (((x - 20) * (x - 20) + (y - 40) * (y - 40)) < 30) { g -= 18; r -= 10; }
            tex_buf[y * 64 + x] = rgb(r + n, g + n, b + n / 2);
        }
    return vx_texture(tex_buf, 64, 64, 0);
}

static int tex_checker(int w, int h, int cell) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            tex_buf[y * w + x] = ((x / cell + y / cell) & 1) ? rgb(20, 20, 20) : rgb(245, 245, 245);
    return vx_texture(tex_buf, w, h, 0);
}

// Boost pad: glowing chevrons pointing along +v (the direction of travel).
static int tex_boost(void) {
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            const int cx = x < 16 ? x : 31 - x;                       // mirror across the centre
            const int yy = (y + cx) % 16;                              // chevron rows
            uint16_t c = rgb(40, 30, 70);
            if (yy < 6) c = rgb(255, 150 + yy * 15, 30);
            if (x < 2 || x > 29) c = rgb(255, 230, 90);
            tex_buf[y * 32 + x] = c;
        }
    return vx_texture(tex_buf, 32, 32, 0);
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

// Tree impostors (row 0 = the bottom: the engine's quads map v=0 to their lower edge). A layered
// conifer and a round broadleaf, shaded from the sun side (left) with a trunk; magenta = empty.
static int tex_tree(int kind) {
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            uint16_t c = KEY;
            const int dx = x - 32, h = y;                              // h: height from the bottom
            if (h < 14 && dx >= -3 && dx <= 3) c = rgb(110 - dx * 6, 72 - dx * 4, 40);   // trunk
            if (kind == 0) {                                          // conifer: 3 stacked cones
                for (int tier = 0; tier < 3; tier++) {
                    const int base = 10 + tier * 16, top = base + 28;
                    if (h >= base && h < top) {
                        const int half = (top - h) * (22 - tier * 4) / 28;
                        if (dx >= -half && dx <= half) {
                            const int lit = dx < 0 ? 18 : -12;
                            const int edge = (dx == -half || dx == half || h == base) ? -22 : 0;
                            c = rgb(40 + lit + edge + rnd(10), 118 + lit + edge + rnd(12), 52 + edge);
                        }
                    }
                }
            } else {                                                  // broadleaf: two blobs
                const int d1 = dx * dx + (h - 36) * (h - 36), d2 = (dx + 8) * (dx + 8) + (h - 28) * (h - 28);
                if (d1 < 20 * 20 || d2 < 13 * 13) {
                    const int lit = (dx < 0 && h > 30) ? 22 : (dx > 8 ? -16 : 0);
                    c = rgb(70 + lit + rnd(14), 150 + lit + rnd(16), 50 + rnd(10));
                    if (d1 > 18 * 18 && d2 > 11 * 11) c = rgb(46, 110, 38);   // rim
                }
            }
            tex_buf[y * 64 + x] = c;
        }
    return vx_texture(tex_buf, 64, 64, VX_TEX_KEY);
}

// Gold coin (billboard, v=0 at the bottom), rim, shine and an embossed star.
// Crowd for the grandstand risers: eight fans per repeat — shirts, heads, a few arms up — in front
// of the seat backs. v = 0 is the top of the riser.
static int tex_crowd(void) {
    static const uint8_t skin[4][3] = { {240, 200, 160}, {205, 150, 110}, {150, 100, 70}, {90, 60, 45} };
    static const uint8_t shirt[8][3] = { {230, 50, 45}, {50, 100, 230}, {250, 210, 50}, {240, 240, 240},
                                         {60, 170, 90}, {250, 130, 30}, {150, 60, 200}, {30, 30, 40} };
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 64; x++)
            tex_buf[y * 64 + x] = y >= 24 ? rgb(120, 124, 136) : rgb(44, 48, 60);    // seat backs / shadow
    for (int p = 0; p < 8; p++) {
        const int cx = p * 8 + 4 + (rnd(3) - 1), top = 7 + rnd(4);
        const uint8_t *sk = skin[rnd(4)], *sh = shirt[rnd(8)];
        for (int y = top + 5; y < 26; y++)                                         // body
            for (int x = cx - 3; x <= cx + 3; x++)
                if (x >= 0 && x < 64 && !(y == top + 5 && (x == cx - 3 || x == cx + 3)))
                    tex_buf[y * 64 + x] = rgb(sh[0] - (x == cx + 3) * 40, sh[1] - (x == cx + 3) * 40, sh[2] - (x == cx + 3) * 40);
        for (int y = top; y < top + 5; y++)                                        // head
            for (int x = cx - 2; x <= cx + 2; x++)
                if (x >= 0 && x < 64 && !((y == top || y == top + 4) && (x == cx - 2 || x == cx + 2)))
                    tex_buf[y * 64 + x] = rgb(sk[0], sk[1], sk[2]);
        if (rnd(3) == 0) {                                                         // arms up, cheering
            for (int y = top - 5; y < top + 6; y++) {
                if (y < 0) continue;
                if (cx - 4 >= 0) tex_buf[y * 64 + cx - 4] = rgb(sk[0], sk[1], sk[2]);
                if (cx + 4 < 64) tex_buf[y * 64 + cx + 4] = rgb(sk[0], sk[1], sk[2]);
            }
        }
    }
    return vx_texture(tex_buf, 64, 32, 0);
}

static int tex_coin(void) {
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            const int dx = x - 16, dy = y - 16, d = dx * dx * 16 / 9 + dy * dy;   // a slightly narrow ellipse
            uint16_t c = KEY;
            if (d < 15 * 15) c = rgb(250, 196, 40);
            if (d < 12 * 12) c = rgb(255, 222, 70);
            if (d < 12 * 12 && dx < -2 && dy > 2) c = rgb(255, 250, 190);            // shine (upper left)
            if (d >= 13 * 13 && d < 15 * 15) c = rgb(200, 140, 20);                  // rim
            tex_buf[y * 32 + x] = c;
        }
    return vx_texture(tex_buf, 32, 32, VX_TEX_KEY);
}

// 360° panorama, 512x64, built in 8-row strips (the app has 64 KB of memory; the texture 64 KB):
// two mountain ranges with snow, rolling hills at the horizon (row 60), clouds; magenta = sky.
static float pano_far(int x) {
    const float a = 2 * PI_F * x / 512.0f;
    return 30 + 11 * sinf_(2 * a + 0.7f) + 7 * sinf_(5 * a + 1.3f) + 3 * sinf_(11 * a + 0.2f) + 2 * sinf_(23 * a);
}
static float pano_near(int x) {
    const float a = 2 * PI_F * x / 512.0f;
    return 11 + 5 * sinf_(3 * a + 2.1f) + 4 * sinf_(7 * a + 0.4f) + 1.5f * sinf_(19 * a + 1.0f);
}
static int in_cloud(int x, int y) {
    static const int kc[][3] = { {40, 12, 22}, {150, 8, 30}, {240, 15, 18}, {330, 10, 26}, {430, 13, 24} };
    for (int i = 0; i < 5; i++) {
        for (int k = -1; k <= 1; k++) {                               // three puffs per cloud
            int dx = x - (kc[i][0] + k * kc[i][2] / 2);
            if (dx > 256) dx -= 512;
            if (dx < -256) dx += 512;
            const int dy = (y - kc[i][1] - (k == 0 ? -2 : 1)) * 3;
            if (dx * dx + dy * dy < (kc[i][2] - (k ? 6 : 0)) * (kc[i][2] - (k ? 6 : 0)) / 2)
                return y > kc[i][1] + 1 ? 2 : 1;                      // 2 = shaded underside
        }
    }
    return 0;
}
static int tex_panorama(void) {
    const int tex = vx_texture_new(512, 64, KEY, VX_TEX_KEY);
    if (tex < 0) return -1;
    for (int y0 = 0; y0 < 64; y0 += 8) {
        for (int y = y0; y < y0 + 8; y++)
            for (int x = 0; x < 512; x++) {
                const int hf = (int)pano_far(x), hn = (int)pano_near(x), h = 60 - y;   // height above horizon
                uint16_t c = KEY;
                const int cl = in_cloud(x, y);
                if (cl) c = cl == 2 ? rgb(214, 222, 236) : rgb(248, 250, 255);
                if (h < hf) {
                    const int t = hf - h;                                              // depth below the ridge
                    c = rgb(118 + t, 128 + t, 176 - t / 2);
                    if (h > 38 && t < 5) c = rgb(245, 248, 255);                        // snow caps
                }
                if (h < hn) {
                    const int t = hn - h;
                    c = rgb(64 + t * 2, 118 + t, 100 + t);
                }
                tex_buf[(y - y0) * 512 + x] = c;
            }
        vx_texture_write(tex, 0, y0, 512, 8, tex_buf);
    }
    return tex;
}

// ---- building the world -------------------------------------------------------------------------------
int g_fx_dust, g_fx_smoke, g_fx_spark, g_fx_confetti, g_fx_boost, g_fx_drift;
Pickup g_pad[NPADS];
Pickup g_coin[NCOINS];

static void build_road(void) {
    const int asphalt = vx_material(0xFFFF, VX_GOURAUD, 255, tex_asphalt(), 0);
    const float y = 1.0f;
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
    background(mb_commit(asphalt, 1));

    // Kerbs on the corners (tangent turning > ~3.5 deg per sample): red/white blocks, laid just
    // over the road edge and outward. Background band, drawn after the road: always on top of it.
    const int red = vx_material(NV_RGB(222, 36, 36), VX_FLAT, 255, -1, 0);
    const int white = vx_material(NV_RGB(245, 245, 245), VX_FLAT, 255, -1, 0);
    for (int side = -1; side <= 1; side += 2) {
        for (int k = 0; k < TRACK_N; k++) {
            const TrackPt *p = &g_trk[(k + TRACK_N - 1) % TRACK_N], *a = &g_trk[k], *b = &g_trk[(k + 1) % TRACK_N];
            const float turn = p->tx * b->tz - p->tz * b->tx;       // sin of the heading change
            if (fabsf_(turn) < 0.06f) continue;
            const float in = ROAD_HW - 8, out = ROAD_HW + 36, y2 = 2.0f;
            if (mb_nv + 8 > MB_MAXV || mb_nt + 4 > MB_MAXT) background(mb_commit(red, 0));
            for (int h = 0; h < 2; h++) {                            // two blocks per sample
                const float f0 = h * 0.5f, f1 = f0 + 0.5f;
                const float ax = a->x + (b->x - a->x) * f0, az = a->z + (b->z - a->z) * f0;
                const float bx = a->x + (b->x - a->x) * f1, bz = a->z + (b->z - a->z) * f1;
                const float ta = a->tx + (b->tx - a->tx) * f0, tza = a->tz + (b->tz - a->tz) * f0;
                const float tb = a->tx + (b->tx - a->tx) * f1, tzb = a->tz + (b->tz - a->tz) * f1;
                const int q0 = mb_v(ax + side * tza * in, y2, az - side * ta * in, 0, 0);
                const int q1 = mb_v(ax + side * tza * out, y2, az - side * ta * out, 0, 0);
                const int q2 = mb_v(bx + side * tzb * out, y2, bz - side * tb * out, 0, 0);
                const int q3 = mb_v(bx + side * tzb * in, y2, bz - side * tb * in, 0, 0);
                mb_quad(q0, q1, q2, q3, ((k * 2 + h) & 1) ? red : white, ax, y2 - 1000, az);
            }
        }
        background(mb_commit(red, 0));
    }

    // Start / finish line across the road at s = 0.
    const int chk = vx_material(0xFFFF, VX_UNLIT, 255, tex_checker(32, 8, 4), 0);
    const TrackPt *a = &g_trk[0];
    const float d = 36.0f, y3 = 3.0f;
    const int s0 = mb_v(a->x - a->tz * ROAD_HW - a->tx * d, y3, a->z + a->tx * ROAD_HW - a->tz * d, 0, 0);
    const int s1 = mb_v(a->x + a->tz * ROAD_HW - a->tx * d, y3, a->z - a->tx * ROAD_HW - a->tz * d, 1024, 0);
    const int s2 = mb_v(a->x + a->tz * ROAD_HW + a->tx * d, y3, a->z - a->tx * ROAD_HW + a->tz * d, 1024, 1024);
    const int s3 = mb_v(a->x - a->tz * ROAD_HW + a->tx * d, y3, a->z + a->tx * ROAD_HW + a->tz * d, 0, 1024);
    mb_quad(s0, s1, s2, s3, chk, a->x, y3 - 1000, a->z);
    background(mb_commit(chk, 1));

    // Boost pads on the straightest stretches, alternating sides of the racing line.
    const int boost = vx_material(0xFFFF, VX_UNLIT, 255, tex_boost(), 0);
    int placed = 0;
    for (int k = 12; k < TRACK_N && placed < NPADS; k += 7) {
        if (fabsf_(track_curvature(g_trk[k].s)) > 0.00012f) continue;
        const float lat = (placed & 1) ? 70.0f : -70.0f, s = g_trk[k].s;
        float cx, cz, hd;
        track_point(s, lat, &cx, &cz, &hd);
        const float fx = sinf_(hd), fz = cosf_(hd), rx = fz, rz = -fx, hw = 55, hl = 95;
        const int p0 = mb_v(cx - rx * hw - fx * hl, 4, cz - rz * hw - fz * hl, 0, 0);
        const int p1 = mb_v(cx + rx * hw - fx * hl, 4, cz + rz * hw - fz * hl, 1024, 0);
        const int p2 = mb_v(cx + rx * hw + fx * hl, 4, cz + rz * hw + fz * hl, 1024, 2048);
        const int p3 = mb_v(cx - rx * hw + fx * hl, 4, cz - rz * hw + fz * hl, 0, 2048);
        mb_quad(p0, p1, p2, p3, boost, cx, -1000, cz);
        g_pad[placed].x = cx; g_pad[placed].z = cz; g_pad[placed].s = s; g_pad[placed].lat = lat;
        placed++;
        k += 9;
    }
    background(mb_commit(boost, 1));
}

static void build_scenery(void) {
    // Tree impostors: two painted textures on camera-facing quads, scattered beside the track.
    const int leaf[2] = { vx_material(0xFFFF, VX_UNLIT, 255, tex_tree(0), 0),
                          vx_material(0xFFFF, VX_UNLIT, 255, tex_tree(1), 0) };
    int proto[2] = { vx_prim(VX_BILLBOARD, 360, 440, 0, leaf[0], -1), vx_prim(VX_BILLBOARD, 400, 380, 0, leaf[1], -1) };
    int placed = 0;
    for (int gz = -5600; gz <= 5600 && placed < 130; gz += 520)
        for (int gx = -6200; gx <= 6200 && placed < 130; gx += 520) {
            const float x = gx + rnd(380) - 190, z = gz + rnd(380) - 190;
            const float d = track_dist(x, z);
            if (d < ROAD_HW + 330 || d > 2600 || rnd(100) < 40) continue;
            const int kind = rnd(3) == 0;
            const int t = placed < 2 ? proto[placed] : vx_clone(proto[kind]);
            if (t < 0) break;
            const int hgt = placed < 2 ? (placed ? 380 : 440) : (kind ? 380 : 440);
            vx_obj_pos(t, (int)x, hgt / 2 - 6, (int)z);
            if (d < LIMIT_HW + 80) solid_circle(x, z, 34);   // reachable trunks are solid
            placed++;
        }

    // Start gantry over the line: pillars, a beam and the banner on both faces.
    const TrackPt *a = &g_trk[0];
    const int steel = vx_material(NV_RGB(64, 70, 92), VX_GOURAUD, 255, -1, 0);
    const int banner = vx_material(0xFFFF, VX_UNLIT, 255,
                                   tex_banner("VERTICE GP", NV_RGB(20, 24, 60), NV_RGB(255, 210, 40), NV_RGB(230, 40, 40)), 0);
    const float w = ROAD_HW + 70;
    for (int side = -1; side <= 1; side += 2) {
        const float px = a->x + side * a->tz * w, pz = a->z - side * a->tx * w;
        mb_box(px - 16, 0, pz - 16, px + 16, 360, pz + 16, 0, 0, 0, steel);
        solid_box(px - 16, pz - 16, px + 16, pz + 16);
    }
    mb_box(a->x - 20, 300, a->z - w - 16, a->x + 20, 372, a->z + w + 16, 0, 0, 0, steel);
    mb_commit(steel, 0);
    for (int face = -1; face <= 1; face += 2) {           // banner quads just proud of both faces
        const float x = a->x + face * 21.0f;
        const int b0 = mb_v(x, 306, a->z - w, face > 0 ? 0 : 1024, 1024);
        const int b1 = mb_v(x, 366, a->z - w, face > 0 ? 0 : 1024, 0);
        const int b2 = mb_v(x, 366, a->z + w, face > 0 ? 1024 : 0, 0);
        const int b3 = mb_v(x, 306, a->z + w, face > 0 ? 1024 : 0, 1024);
        mb_quad(b0, b1, b2, b3, banner, a->x, 336, a->z);
    }
    mb_commit(banner, 1);

    // Grandstand outside the start straight (-Z side): a concrete staircase whose risers carry a
    // painted crowd (the 16-bit trick: people are texels, not polygons), under a steel canopy.
    const int concrete = vx_material(NV_RGB(150, 150, 160), VX_GOURAUD, 255, -1, 0);
    const int crowd = vx_material(0xFFFF, VX_GOURAUD, 255, tex_crowd(), 0);
    const int roof = vx_material(NV_RGB(200, 40, 40), VX_GOURAUD, 255, -1, 0);
    const float z0 = a->z - ROAD_HW - 280, gx0 = 300, gx1 = 1860;
    float hprev = 0;
    for (int row = 0; row < 5; row++) {
        const float zr = z0 - row * 70, h = 40 + row * 45, cx = (gx0 + gx1) / 2, cz = zr - 35;
        const int t0 = mb_v(gx0, h, zr - 70, 0, 0), t1 = mb_v(gx1, h, zr - 70, 0, 0);
        const int t2 = mb_v(gx1, h, zr, 0, 0), t3 = mb_v(gx0, h, zr, 0, 0);
        mb_quad(t0, t1, t2, t3, concrete, cx, 0, cz);                              // tread
        for (int seg = 0; seg < 6; seg++) {                                        // riser = crowd, in
            const float xa = gx0 + (gx1 - gx0) * seg / 6, xb = gx0 + (gx1 - gx0) * (seg + 1) / 6;   // short
            const int u0 = (row * 300) & 1023, u1 = u0 + (int)((xb - xa) / 176.0f * 1024);     // UV runs
            const int r0 = mb_v(xa, hprev, zr, u0, 1024), r1 = mb_v(xb, hprev, zr, u1, 1024);
            const int r2 = mb_v(xb, h, zr, u1, 0), r3 = mb_v(xa, h, zr, u0, 0);
            mb_quad(r0, r1, r2, r3, crowd, cx, 0, cz);
        }
        for (int side = 0; side < 2; side++) {                                     // stair ends
            const float x = side ? gx1 : gx0;
            const int s0 = mb_v(x, 0, zr - 70, 0, 0), s1 = mb_v(x, 0, zr, 0, 0);
            const int s2 = mb_v(x, h, zr, 0, 0), s3 = mb_v(x, h, zr - 70, 0, 0);
            mb_quad(s0, s1, s2, s3, concrete, cx, 0, cz);
        }
        hprev = h;
    }
    mb_commit(concrete, 1);
    solid_box(gx0, z0 - 420, gx1, z0);                                         // the whole stand is solid
    mb_box(290, 0, z0 - 420, 1870, 300, z0 - 350, 0, 0, 0, concrete);          // back wall
    mb_commit(concrete, 0);
    for (int k = 0; k < 4; k++) {                                              // canopy on four pillars
        const float px = gx0 + 10 + k * (gx1 - gx0 - 20) / 3;
        mb_box(px - 7, 0, z0 - 40, px + 7, 334, z0 - 26, 0, 0, 0, roof);
    }
    mb_box(280, 334, z0 - 430, 1880, 346, z0 - 10, 0, 0, 0, roof);
    mb_commit(roof, 0);
    const int nb = vx_material(0xFFFF, VX_UNLIT, 255,
                               tex_banner("NUCLEO OS", NV_RGB(240, 240, 245), NV_RGB(30, 60, 170), NV_RGB(30, 60, 170)), 0);
    const int q0 = mb_v(700, 300, z0 - 349, 0, 0), q1 = mb_v(1460, 300, z0 - 349, 1024, 0);
    const int q2 = mb_v(1460, 380, z0 - 349, 1024, 1024), q3 = mb_v(700, 380, z0 - 349, 0, 1024);
    mb_quad(q0, q1, q2, q3, nb, 1080, 340, z0 - 800);
    mb_commit(nb, 1);

    // Coins: rows of five on the racing line, spinning gold impostors.
    const int gold = vx_material(0xFFFF, VX_UNLIT, 255, tex_coin(), 0);
    const int coin0 = vx_prim(VX_BILLBOARD, 64, 64, 0, gold, -1);
    int n = 0;
    for (int grp = 0; grp < NCOINS / 5; grp++) {
        const float s0 = g_trk_len * (0.12f + grp * 0.27f);
        const float lat = (grp & 1) ? 60.0f : -40.0f;
        for (int k = 0; k < 5 && n < NCOINS; k++, n++) {
            Pickup *c = &g_coin[n];
            track_point(s0 + k * 120.0f, lat, &c->x, &c->z, 0);
            c->s = s0 + k * 120.0f; c->lat = lat;
            c->obj = n == 0 ? coin0 : vx_clone(coin0);
            c->respawn_ms = 0;
            vx_obj_pos(c->obj, iroundf(c->x), 48, iroundf(c->z));
        }
    }
}

void world_build(void) {
    track_sample();
    for (int i = 0; i < TRACK_N; i++) {
        const TrackPt *a = &g_trk[(i + 1) % TRACK_N], *b = &g_trk[(i + 6) % TRACK_N];
        g_trk_bend[i] = fabsf_(wrap_pi(atan2f_(b->tx, b->tz) - atan2f_(a->tx, a->tz)));
    }
    vx_sky(NV_RGB(40, 104, 214), NV_RGB(186, 214, 246));
    vx_sun(215, 52, 0xFFF4E0, 235);
    vx_ambient(0x4A5464);
    vx_lens(70, 24, 16000);
    vx_fog(5200, 15000);
    vx_floor(0, tex_grass(), 1400, NV_RGB(80, 158, 58));
    vx_panorama(tex_panorama(), 60);
    build_road();
    build_scenery();
    // Effects: dust (off track), tyre smoke, sparks (contact / drift), confetti, boost flames.
    g_fx_dust = vx_emitter(64, NV_RGB(170, 150, 100), NV_RGB(150, 138, 104), 14, 40, 480, -10, 0);
    g_fx_smoke = vx_emitter(48, NV_RGB(200, 200, 206), NV_RGB(150, 150, 156), 16, 56, 700, -30, 0);
    g_fx_spark = vx_emitter(64, NV_RGB(255, 240, 150), NV_RGB(255, 80, 0), 10, 3, 380, 900, VX_PART_ADDITIVE);
    g_fx_drift = vx_emitter(96, NV_RGB(120, 190, 255), NV_RGB(40, 80, 255), 12, 4, 300, 500, VX_PART_ADDITIVE);
    g_fx_boost = vx_emitter(96, NV_RGB(255, 250, 200), NV_RGB(255, 90, 20), 22, 8, 260, -200, VX_PART_ADDITIVE);
    g_fx_confetti = vx_emitter(200, NV_RGB(255, 220, 60), NV_RGB(255, 60, 140), 18, 14, 2600, 260, 0);
}
