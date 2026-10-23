/* Sky Ranger - an arcade flight game on the VoxelSpaceNG engine (NucleoOS ABI 20).
 *
 * Fly through 12 rings in order over the valley as fast as you can. The terrain is rendered natively
 * on both cores (vs_render); this app flies the plane, draws it (a low-poly model projected with the
 * engine's own camera), the 3D rings and the HUD on the quarter-turned canvas.
 *
 * Flight: the stick rolls and pitches the plane; the turn comes from the bank (rate ~ tan(bank) / v),
 * climbing costs speed and diving gains it, below the stall speed the nose drops. Collisions: the
 * ground (and water) under the fuselage, the nose and both wing tips; rings count only when the plane
 * crosses their plane inside the radius (no tunnelling at speed), a clipped rim slows it down;
 * terrain ahead on the flight path raises PULL UP. Units: cells (x, y and heights alike), seconds.
 *
 * Controls (input.h): roll/pitch = left stick / arrows-WASD / mouse / touch stick (left half, pull
 * back to climb); throttle = RT / Shift / left button / hold the right half; view = C / Y button /
 * tap VIEW; pause = Esc / Start / B (again = quit); Enter / A / tap = resume, retry.
 */
#include "hud.h"
#include "input.h"
#include "font.h"

#define NRINGS 12
#define PI_F   3.14159265f

/* ---- float helpers (no libm in a freestanding module) ---- */
static float fsin(float a)
{
    float x2;
    int k = (int)(a * 0.15915494f + (a >= 0 ? 0.5f : -0.5f));
    a -= (float)k * 6.2831853f;                                   /* -pi..pi */
    if (a > 1.5707963f) a = PI_F - a;
    else if (a < -1.5707963f) a = -PI_F - a;                      /* -pi/2..pi/2 */
    x2 = a * a;
    return a * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f * (1.0f - x2 / 72.0f))));
}
static float fcos(float a) { return fsin(a + 1.5707963f); }
static float fatan2(float y, float x)
{
    const float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    float a, t, r;
    if (ax < 1e-6f && ay < 1e-6f) return 0;
    t = ax > ay ? ay / ax : ax / ay;
    a = t * t;
    r = t * (0.9998660f + a * (-0.3302995f + a * (0.1801410f + a * (-0.0851330f + a * 0.0208351f))));
    if (ay > ax) r = 1.5707963f - r;
    if (x < 0) r = PI_F - r;
    return y < 0 ? -r : r;
}
static float fsqrt(float v) { return v > 0 ? __builtin_sqrtf(v) : 0; }
static float fclamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

typedef struct { float x, y, h; } v3;          /* cells; h = height in cells */

/* ---- world ---- */
static float g_cs;                              /* world cells per cell of a 1024^2 world (speeds, sizes) */
static float g_vs;                              /* one height unit in cells */
static float g_F;                               /* vertical focal, px */
static float g_water_h;                         /* water surface, cells */
static int g_size, g_maxh;

static float ground_at(float x, float y)        /* terrain or water surface, cells */
{
    const float g = (float)vs_height((int)(x * 65536.0f), (int)(y * 65536.0f)) * (g_vs / 16.0f);
    return g > g_water_h ? g : g_water_h;
}

typedef struct { v3 c, n; float r; } ring_t;    /* centre, facing (to the next ring), radius */
static ring_t g_ring[NRINGS];

static void make_course(void)
{
    int i;
    for (i = 0; i < NRINGS; i++) {
        const float a = (float)i * 6.2831853f / NRINGS;
        const float rad = (float)g_size * (0.25f + 0.04f * fsin(a * 3.0f));
        float hmax = 0, x = (float)g_size * 0.5f + fcos(a) * rad, y = (float)g_size * 0.5f + fsin(a) * rad;
        int k;
        for (k = -3; k <= 3; k++) {
            const float g1 = ground_at(x + (float)k * 6 * g_cs, y), g2 = ground_at(x, y + (float)k * 6 * g_cs);
            if (g1 > hmax) hmax = g1;
            if (g2 > hmax) hmax = g2;
        }
        g_ring[i].c.x = x;
        g_ring[i].c.y = y;
        g_ring[i].r = 14.0f * g_cs;
        g_ring[i].c.h = hmax + g_ring[i].r + (10.0f + (float)(i % 3) * 8.0f) * g_cs;
    }
    for (i = 0; i < NRINGS; i++) {
        const ring_t *b = &g_ring[(i + 1) % NRINGS];
        float dx = b->c.x - g_ring[i].c.x, dy = b->c.y - g_ring[i].c.y, l = fsqrt(dx * dx + dy * dy);
        g_ring[i].n.x = dx / l;
        g_ring[i].n.y = dy / l;
        g_ring[i].n.h = 0;
    }
}

/* ---- camera: the engine's projection (planar columns, horizon shift, roll as a shear) ---- */
typedef struct { v3 p; float yaw, pitch_px, roll; } cam_t;     /* roll = screen slope (px per px) */

static int project(const cam_t *c, v3 p, float *sx, float *sy, float *z)
{
    const float dx = p.x - c->p.x, dy = p.y - c->p.y, cy = fcos(c->yaw), sy_ = fsin(c->yaw);
    const float zz = dx * cy + dy * sy_, r = -dx * sy_ + dy * cy;
    if (zz < 0.5f * g_cs) return 0;
    *sx = (float)g_w * 0.5f + r * g_F / zz;
    *sy = (float)g_h * 0.5f - c->pitch_px + (c->p.h - p.h) * g_F / zz;
    *sy += c->roll * (*sx - (float)g_w * 0.5f);
    *z = zz;
    return 1;
}

static void engine_camera(const cam_t *c, int t)
{
    float yaw = c->yaw * (4096.0f / 6.2831853f);
    int iy = (int)yaw & 4095;
    vs_camera((int)(c->p.x * 65536.0f), (int)(c->p.y * 65536.0f), (int)(c->p.h / g_vs * 16.0f), iy,
              (int)c->pitch_px, (int)(c->roll * 65536.0f), t);
}

static int line_clear(const cam_t *c, v3 p)    /* terrain between the eye and p? */
{
    int k;
    for (k = 1; k < 10; k++) {
        const float t = (float)k / 10.0f;
        if (ground_at(c->p.x + (p.x - c->p.x) * t, c->p.y + (p.y - c->p.y) * t) > c->p.h + (p.h - c->p.h) * t)
            return 0;
    }
    return 1;
}

/* a ring is a circle in its own vertical plane: 32 points projected, two strokes */
static void draw_ring(const cam_t *c, const ring_t *g, int col, int col2)
{
    float px[33], py[33], z;
    int ok[33], i;
    const v3 side = { -g->n.y, g->n.x, 0 };
    for (i = 0; i <= 32; i++) {
        const float a = (float)i * 6.2831853f / 32;
        v3 p = { g->c.x + side.x * fcos(a) * g->r, g->c.y + side.y * fcos(a) * g->r, g->c.h + fsin(a) * g->r };
        ok[i] = project(c, p, &px[i], &py[i], &z);
    }
    for (i = 0; i < 32; i++) {
        if (!ok[i] || !ok[i + 1]) continue;
        line_s((int)px[i], (int)py[i], (int)px[i + 1], (int)py[i + 1], col);
        line_s((int)px[i] + 1, (int)py[i], (int)px[i + 1] + 1, (int)py[i + 1], col);
        line_s((int)px[i], (int)py[i] + 1, (int)px[i + 1], (int)py[i + 1] + 1, col2);
    }
}

/* ---- the plane: a low-poly sport plane built at start, local axes f (forward), r (right), u (up),
 * in cells of a 1024^2 world (scaled by g_cs). Fuselage = rings of 8 points along f; wings, tailplane
 * and fin as thin slabs; a glass canopy; a two-blade propeller that spins. Every triangle is wound to
 * face away from its part's centre, so back faces can be culled per frame. ---- */
#define PV_MAX 128
#define PT_MAX 160
enum { MAT_WHITE, MAT_GREY, MAT_RED, MAT_GLASS, MAT_DARK };
static const int MATRGB[5][3] = { { 236, 238, 240 }, { 170, 176, 186 }, { 210, 36, 32 }, { 40, 70, 120 }, { 40, 40, 44 } };
static float g_pv[PV_MAX][3];
static unsigned char g_pt[PT_MAX][4];
static int g_npv, g_npt, g_prop0;            /* the propeller's two quads are the last 4 triangles */

static int pv_add(float f, float r, float u)
{
    g_pv[g_npv][0] = f; g_pv[g_npv][1] = r; g_pv[g_npv][2] = u;
    return g_npv++;
}

/* triangle a b c of material m, wound so its normal points away from (cf, cr, cu) */
static void pt_add(int a, int b, int c, int m, float cf, float cr, float cu)
{
    const float *A = g_pv[a], *B = g_pv[b], *C = g_pv[c];
    const float ux = B[0] - A[0], uy = B[1] - A[1], uz = B[2] - A[2], vx = C[0] - A[0], vy = C[1] - A[1], vz = C[2] - A[2];
    const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const float ox = (A[0] + B[0] + C[0]) / 3 - cf, oy = (A[1] + B[1] + C[1]) / 3 - cr, oz = (A[2] + B[2] + C[2]) / 3 - cu;
    if (g_npt >= PT_MAX) return;
    if (nx * ox + ny * oy + nz * oz < 0) { int t = b; b = c; c = t; }
    g_pt[g_npt][0] = (unsigned char)a; g_pt[g_npt][1] = (unsigned char)b; g_pt[g_npt][2] = (unsigned char)c;
    g_pt[g_npt][3] = (unsigned char)m;
    g_npt++;
}

/* a thin slab between a root and a tip chord (wing, tailplane): top and bottom faces */
static void slab(float rf0, float rf1, float rr, float ru, float tf0, float tf1, float tr, float tu, float th, int m)
{
    const int a = pv_add(rf0, rr, ru + th), b = pv_add(rf1, rr, ru + th), c = pv_add(tf1, tr, tu + th), d = pv_add(tf0, tr, tu + th);
    const int e = pv_add(rf0, rr, ru - th), f = pv_add(rf1, rr, ru - th), g = pv_add(tf1, tr, tu - th), h = pv_add(tf0, tr, tu - th);
    const float cf = (rf0 + rf1 + tf0 + tf1) / 4, cr = (rr + tr) / 2, cu = (ru + tu) / 2;
    pt_add(a, b, c, m, cf, cr, cu); pt_add(a, c, d, m, cf, cr, cu);
    pt_add(e, f, g, m == MAT_WHITE ? MAT_GREY : m, cf, cr, cu); pt_add(e, g, h, m == MAT_WHITE ? MAT_GREY : m, cf, cr, cu);
    pt_add(d, c, g, m, cf, cr, cu); pt_add(d, g, h, m, cf, cr, cu);              /* the tip edge */
}

static void build_plane(void)
{
    static const float st[7][4] = {          /* station f, half width, half height, centre u */
        { 4.65f, 0.06f, 0.06f, 0.10f }, { 4.05f, 0.42f, 0.40f, 0.10f }, { 2.6f, 0.62f, 0.62f, 0.12f },
        { 0.6f, 0.66f, 0.70f, 0.14f }, { -1.6f, 0.52f, 0.56f, 0.16f }, { -3.4f, 0.30f, 0.34f, 0.24f },
        { -4.7f, 0.10f, 0.14f, 0.30f },
    };
    int ring[7][8], s, k;
    g_npv = g_npt = 0;
    for (s = 0; s < 7; s++)
        for (k = 0; k < 8; k++) {
            const float a = (float)k * 0.785398f;
            ring[s][k] = pv_add(st[s][0], st[s][1] * fcos(a), st[s][3] + st[s][2] * fsin(a));
        }
    for (s = 0; s < 6; s++)
        for (k = 0; k < 8; k++) {
            const int k1 = (k + 1) & 7;
            const float cf = (st[s][0] + st[s + 1][0]) / 2, cu = (st[s][3] + st[s + 1][3]) / 2;
            /* livery: red nose and tail, a red band low on the cabin, grey belly, white elsewhere */
            int m = (k >= 5 && k <= 6) || k == 4 ? MAT_GREY : MAT_WHITE;
            if (s == 0 || s == 5) m = MAT_RED;
            if (s == 2 && (k == 0 || k == 3 || k == 7 || k == 4)) m = MAT_RED;
            pt_add(ring[s][k], ring[s + 1][k], ring[s + 1][k1], m, cf, 0, cu);
            pt_add(ring[s][k], ring[s + 1][k1], ring[s][k1], m, cf, 0, cu);
        }
    {   /* canopy: a glass bubble over the cabin */
        const int a = pv_add(2.3f, -0.40f, 0.62f), b = pv_add(2.3f, 0.40f, 0.62f), c = pv_add(0.5f, -0.44f, 0.74f);
        const int d = pv_add(0.5f, 0.44f, 0.74f), e = pv_add(1.3f, 0, 1.12f), f = pv_add(-0.6f, 0, 0.86f);
        pt_add(a, b, e, MAT_GLASS, 1.2f, 0, 0.5f); pt_add(a, e, c, MAT_GLASS, 1.2f, 0, 0.5f);
        pt_add(b, d, e, MAT_GLASS, 1.2f, 0, 0.5f); pt_add(c, e, f, MAT_GLASS, 1.2f, 0, 0.5f);
        pt_add(d, f, e, MAT_GLASS, 1.2f, 0, 0.5f);
    }
    slab(1.2f, -1.5f, -0.5f, 0.02f, -0.2f, -1.25f, -4.9f, 0.38f, 0.07f, MAT_WHITE);   /* left wing, dihedral */
    slab(1.2f, -1.5f, 0.5f, 0.02f, -0.2f, -1.25f, 4.9f, 0.38f, 0.07f, MAT_WHITE);     /* right wing */
    slab(-3.3f, -4.5f, -0.2f, 0.28f, -3.95f, -4.55f, -1.95f, 0.32f, 0.04f, MAT_WHITE); /* tailplane */
    slab(-3.3f, -4.5f, 0.2f, 0.28f, -3.95f, -4.55f, 1.95f, 0.32f, 0.04f, MAT_WHITE);
    {   /* fin, both sides */
        const int a = pv_add(-3.1f, -0.04f, 0.4f), b = pv_add(-4.7f, -0.04f, 0.4f), c = pv_add(-4.75f, -0.04f, 2.0f), d = pv_add(-4.2f, -0.04f, 2.0f);
        const int e = pv_add(-3.1f, 0.04f, 0.4f), f = pv_add(-4.7f, 0.04f, 0.4f), g = pv_add(-4.75f, 0.04f, 2.0f), h = pv_add(-4.2f, 0.04f, 2.0f);
        pt_add(a, b, c, MAT_RED, -4, 0, 1); pt_add(a, c, d, MAT_RED, -4, 0, 1);
        pt_add(e, f, g, MAT_RED, -4, 0, 1); pt_add(e, g, h, MAT_RED, -4, 0, 1);
    }
    {   /* propeller: two blades, rewritten each frame (spinning) */
        g_prop0 = g_npt;
        for (k = 0; k < 8; k++) pv_add(4.7f, 0, 0.1f);
        pt_add(g_npv - 8, g_npv - 7, g_npv - 6, MAT_DARK, 4.0f, 0, 0.1f); pt_add(g_npv - 8, g_npv - 6, g_npv - 5, MAT_DARK, 4.0f, 0, 0.1f);
        pt_add(g_npv - 4, g_npv - 3, g_npv - 2, MAT_DARK, 4.0f, 0, 0.1f); pt_add(g_npv - 4, g_npv - 2, g_npv - 1, MAT_DARK, 4.0f, 0, 0.1f);
    }
}

static void spin_prop(float a)                 /* the last 8 vertices: two blades at angle a */
{
    int b;
    for (b = 0; b < 2; b++) {
        const float t = a + (float)b * PI_F, c = fcos(t), s = fsin(t), w = 0.12f;
        float *v = g_pv[g_npv - 8 + b * 4];
        v[0] = 4.75f; v[1] = -s * w; v[2] = 0.1f + c * w;                       /* root, one side */
        v = g_pv[g_npv - 7 + b * 4]; v[0] = 4.75f; v[1] = c * 1.15f - s * w * 0.5f; v[2] = 0.1f + s * 1.15f + c * w * 0.5f;
        v = g_pv[g_npv - 6 + b * 4]; v[0] = 4.75f; v[1] = c * 1.15f + s * w * 0.5f; v[2] = 0.1f + s * 1.15f - c * w * 0.5f;
        v = g_pv[g_npv - 5 + b * 4]; v[0] = 4.75f; v[1] = s * w; v[2] = 0.1f - c * w;
    }
}

typedef struct { float x, y, h, yaw, pitch, bank, v; } plane_t;

static v3 plane_point(const plane_t *pl, const float *m)
{
    /* roll about f, pitch about r, then yaw about u */
    const float cb = fcos(pl->bank), sb = fsin(pl->bank), cp = fcos(pl->pitch), sp = fsin(pl->pitch);
    const float cy = fcos(pl->yaw), sy = fsin(pl->yaw);
    const float f = m[0] * g_cs, r0 = m[1] * g_cs, u0 = m[2] * g_cs;
    const float r = r0 * cb - u0 * sb, u1 = r0 * sb + u0 * cb;     /* roll */
    const float f2 = f * cp - u1 * sp, u = f * sp + u1 * cp;      /* pitch */
    v3 o = { pl->x + f2 * cy - r * sy, pl->y + f2 * sy + r * cy, pl->h + u };
    return o;
}

/* soft shadow on the ground under the plane: it shows the height above the terrain */
static void draw_shadow(const cam_t *c, const plane_t *pl)
{
    const float g = ground_at(pl->x, pl->y), above = pl->h - g;
    v3 p = { pl->x, pl->y, g };
    float sx, sy, z;
    int w, h, a;
    if (above > 70.0f * g_cs || !project(c, p, &sx, &sy, &z)) return;
    w = (int)(9.0f * g_cs * g_F / z);
    h = w / 3 + 2;
    a = 120 - (int)(above / g_cs * 1.5f);
    if (w < 4 || a < 10) return;
    plate_s((int)sx - w / 2, (int)sy - h / 2, w, h, C565(10, 12, 16), a);
}

static void draw_plane(const cam_t *c, const plane_t *pl, int t_ms)
{
    static float sx[PV_MAX], sy[PV_MAX], z[PV_MAX], dz[PT_MAX];
    static v3 w[PV_MAX];
    static int ok[PV_MAX], ord[PT_MAX];
    const float lx = -0.6f, ly = 0.6f, lz = 0.53f;            /* the baked sun (az 135, el 32) */
    int i, j, n = 0;
    spin_prop((float)t_ms * 0.045f);
    for (i = 0; i < g_npv; i++) {
        w[i] = plane_point(pl, g_pv[i]);
        ok[i] = project(c, w[i], &sx[i], &sy[i], &z[i]);
    }
    for (i = 0; i < g_npt; i++) {               /* front faces only, far first */
        const unsigned char *t = g_pt[i];
        const v3 a = w[t[0]], b = w[t[1]], d = w[t[2]];
        const float nx = (b.y - a.y) * (d.h - a.h) - (b.h - a.h) * (d.y - a.y);
        const float ny = (b.h - a.h) * (d.x - a.x) - (b.x - a.x) * (d.h - a.h);
        const float nh = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
        if (!ok[t[0]] || !ok[t[1]] || !ok[t[2]]) continue;
        if (i < g_prop0 && nx * (c->p.x - a.x) + ny * (c->p.y - a.y) + nh * (c->p.h - a.h) <= 0) continue;
        ord[n] = i;
        dz[n] = z[t[0]] + z[t[1]] + z[t[2]];
        for (j = n; j > 0 && dz[j] > dz[j - 1]; j--) {
            const float tf = dz[j]; const int ti = ord[j];
            dz[j] = dz[j - 1]; ord[j] = ord[j - 1]; dz[j - 1] = tf; ord[j - 1] = ti;
        }
        n++;
    }
    for (i = 0; i < n; i++) {
        const unsigned char *t = g_pt[ord[i]];
        const v3 a = w[t[0]], b = w[t[1]], d = w[t[2]];
        float nx = (b.y - a.y) * (d.h - a.h) - (b.h - a.h) * (d.y - a.y);
        float ny = (b.h - a.h) * (d.x - a.x) - (b.x - a.x) * (d.h - a.h);
        float nh = (b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x);
        const float nl = fsqrt(nx * nx + ny * ny + nh * nh) + 1e-6f;
        float vx = c->p.x - a.x, vy = c->p.y - a.y, vh = c->p.h - a.h, vl, ndl, hx, hy, hz, hl, sp, lit;
        int k, rgb[3];
        nx /= nl; ny /= nl; nh /= nl;
        vl = fsqrt(vx * vx + vy * vy + vh * vh) + 1e-6f;
        vx /= vl; vy /= vl; vh /= vl;
        if (nx * vx + ny * vy + nh * vh < 0) { nx = -nx; ny = -ny; nh = -nh; }    /* the propeller's back */
        ndl = nx * lx + ny * ly + nh * lz;
        hx = lx + vx; hy = ly + vy; hz = lz + vh;
        hl = fsqrt(hx * hx + hy * hy + hz * hz) + 1e-6f;
        sp = (nx * hx + ny * hy + nh * hz) / hl;
        sp = sp > 0 ? sp : 0;
        sp *= sp; sp *= sp; sp *= sp; sp *= sp;                                      /* ^16 */
        lit = 0.42f + 0.72f * (ndl > 0 ? ndl : 0) + 0.18f * nh;
        for (k = 0; k < 3; k++) {
            int v = (int)((float)MATRGB[t[3]][k] * lit + sp * (t[3] == MAT_GLASS ? 230.0f : 110.0f));
            rgb[k] = v > 255 ? 255 : v < 0 ? 0 : v;
        }
        tri_s((int)sx[t[0]], (int)sy[t[0]], (int)sx[t[1]], (int)sy[t[1]], (int)sx[t[2]], (int)sy[t[2]],
              C565(rgb[0], rgb[1], rgb[2]));
    }
    if ((t_ms / 500) & 1) {                     /* navigation lights: red left tip, green right tip */
        static const float tipl[3] = { -0.75f, -4.95f, 0.42f }, tipr[3] = { -0.75f, 4.95f, 0.42f };
        float x, y, zz;
        if (project(c, plane_point(pl, tipl), &x, &y, &zz)) rect_s((int)x - 2, (int)y - 2, 5, 5, C565(255, 40, 40));
        if (project(c, plane_point(pl, tipr), &x, &y, &zz)) rect_s((int)x - 2, (int)y - 2, 5, 5, C565(60, 255, 90));
    }
}

/* ---- HUD ---- */
static void hud_label_value(int x, int y, const char *label, const char *value, int col)
{
    const int w = (text_w(label, 2) > text_w(value, 4) ? text_w(label, 2) : text_w(value, 4)) + 20;
    plate_s(x, y, w, 52, C565(8, 14, 26), 150);
    text_s(x + 10, y + 6, label, 2, C565(150, 170, 190));
    text_sh(x + 10, y + 22, value, 4, col);
}

/* ring progress: 12 dots under the ring counter */
static void hud_progress(int x, int y, int done)
{
    int i;
    for (i = 0; i < NRINGS; i++) {
        const int c = i < done ? C565(255, 215, 40) : i == done ? C565(255, 255, 255) : C565(70, 80, 95);
        rect_s(x + i * 11, y, 8, i == done ? 8 : 6, c);
    }
}

/* radar: the rings around the plane, heading up, the next one bright */
static void hud_radar(const plane_t *pl, int ring, int cx, int cy, int rad)
{
    const float range = 520.0f * g_cs, cyw = fcos(pl->yaw), syw = fsin(pl->yaw);
    int i;
    plate_s(cx - rad - 6, cy - rad - 6, 2 * rad + 12, 2 * rad + 12, C565(8, 14, 26), 140);
    ring_s(cx, cy, rad, 32, C565(80, 100, 120));
    ring_s(cx, cy, rad / 2, 24, C565(50, 64, 80));
    for (i = 0; i < NRINGS; i++) {
        const ring_t *g = &g_ring[i];
        const float dx = g->c.x - pl->x, dy = g->c.y - pl->y;
        float fwd = dx * cyw + dy * syw, rt = -dx * syw + dy * cyw, d = fsqrt(fwd * fwd + rt * rt);
        int px, py, col, sz;
        if (d > range) { fwd *= range / d; rt *= range / d; }
        px = cx + (int)(rt / range * (float)rad);
        py = cy - (int)(fwd / range * (float)rad);
        col = i == ring ? C565(255, 215, 40) : ((i - ring + NRINGS) % NRINGS) < 3 ? C565(150, 130, 70) : C565(80, 85, 95);
        sz = i == ring ? 7 : 4;
        rect_s(px - sz / 2, py - sz / 2, sz, sz, col);
    }
    tri_s(cx, cy - 7, cx - 5, cy + 5, cx + 5, cy + 5, 0xFFFF);                 /* the plane */
}

/* cockpit: horizon line and pitch ladder, rolling with the bank */
static void hud_attitude(const cam_t *c)
{
    const int cx = g_w / 2, cy = g_h / 2;
    int k;
    for (k = -2; k <= 2; k++) {
        const float y0 = (float)cy - c->pitch_px - (float)k * 0.1745f * g_F;   /* every 10 degrees */
        const int half = k == 0 ? 240 : 60, col = k == 0 ? C565(120, 255, 160) : C565(90, 200, 130);
        const int x0 = cx - half, x1 = cx + half;
        const int ya = (int)(y0 + c->roll * (float)(x0 - cx)), yb = (int)(y0 + c->roll * (float)(x1 - cx));
        if (k && ya > 0 && ya < g_h) {
            line_s(x0, ya, cx - 24, (int)(y0 + c->roll * -24.0f), col);
            line_s(cx + 24, (int)(y0 + c->roll * 24.0f), x1, yb, col);
        } else if (!k) {
            line_s(x0, ya, x1, yb, col);
        }
    }
}

static void center_text(int y, const char *s, int k, int col)
{
    text_sh((g_w - text_w(s, k)) / 2, y, s, k, col);
}

static void draw_pointer(const cam_t *c, const ring_t *g, const plane_t *pl)
{
    const float a = fatan2(g->c.y - c->p.y, g->c.x - c->p.x) - c->yaw;
    const float s = fsin(a), co = fcos(a);
    const int cx = g_w / 2, cy = g_h / 2, ex = cx + (int)(s * (float)(g_w / 2 - 50)), ey = cy - (int)(co * (float)(g_h / 2 - 50));
    const int dx = (int)(s * 26), dy = (int)(-co * 26);
    char buf[16];
    const float dist = fsqrt((g->c.x - pl->x) * (g->c.x - pl->x) + (g->c.y - pl->y) * (g->c.y - pl->y)) / g_cs;
    tri_s(ex + dx, ey + dy, ex - dy / 2, ey + dx / 2, ex + dy / 2, ey - dx / 2, C565(255, 210, 40));
    fmt_int(buf, (int)dist);
    text_sh(ex - text_w(buf, 2) / 2 - dx, ey - 7 - dy, buf, 2, C565(255, 230, 120));
}

static void rumble(int ms)
{
    int i, n = nv_pad_count();
    for (i = 0; i < n; i++) nv_pad_rumble(i, 40000, 60000, ms);
}

enum { ST_COUNT, ST_FLY, ST_CRASH, ST_DONE };

NV_EXPORT("run")
void run(void)
{
    in_state in;
    plane_t pl, last_ok;
    cam_t cam;
    v3 prev;
    int ring = 0, state = ST_COUNT, view = 0, paused = 0, warn = 0, newbest = 0;
    int t_now, last, t_state = 0, t_start = 0, best = 0, finish_ms = 0, pause_t = 0, rim_until = 0;
    float cam_yaw = 0;

    sin_init();
    if (nv_gfx_set_rotation(1) != 1 || vs_open("vsng:alps", "vsng:day") < 0) {
        nv_log(3, "vsfly: needs ABI 20 firmware and the VoxelSpace NG app (world files)");
        while (nv_gfx_present()) { nv_gfx_clear(C565(60, 0, 0)); if (nv_gfx_back()) break; }
        return;
    }
    g_w = nv_gfx_height();
    g_h = nv_gfx_width();
    g_size = 1 << vs_info(VS_INFO_LOG2SIZE);
    g_maxh = vs_info(VS_INFO_MAXH);
    g_cs = (float)g_size / 1024.0f;
    g_vs = vs_info(VS_INFO_VSCALE) > 0 ? (float)vs_info(VS_INFO_VSCALE) / 256.0f : 0.8f * g_cs;
    g_F = (float)g_w * 0.596f;                   /* w/2 / tan(40 deg): the engine's fov 80 */
    g_water_h = (float)vs_info(VS_INFO_WATER) * g_vs;
    vs_config(VS_CFG_TARGET_MS, 45);             /* adaptive quality, never below its floors */
    if (nv_load("best", &best, sizeof best) != sizeof best) best = 0;
    __builtin_memset(&in, 0, sizeof in);
    make_course();
    build_plane();

respawn:
    {
        const ring_t *to = &g_ring[ring], *from = &g_ring[(ring + NRINGS - 1) % NRINGS];
        float back = ring == 0 ? 110.0f * g_cs : 0.0f;
        int k;
        pl.x = (ring == 0 ? to->c.x : from->c.x) - to->n.x * back;
        pl.y = (ring == 0 ? to->c.y : from->c.y) - to->n.y * back;
        if (ring) { pl.x = from->c.x; pl.y = from->c.y; }
        pl.h = ring == 0 ? to->c.h : from->c.h;
        for (k = 0; k <= 8; k++) {                /* clear of the ground along the first leg */
            const float t = (float)k / 8.0f, g = ground_at(pl.x + (to->c.x - pl.x) * t, pl.y + (to->c.y - pl.y) * t);
            if (g + 22.0f * g_cs > pl.h) pl.h = g + 22.0f * g_cs;
        }
        pl.yaw = fatan2(to->c.y - pl.y, to->c.x - pl.x);
        pl.pitch = 0; pl.bank = 0; pl.v = 70.0f * g_cs;
        cam_yaw = pl.yaw;
        prev.x = pl.x; prev.y = pl.y; prev.h = pl.h;
        last_ok = pl;
        last = nv_millis();
        state = ST_COUNT;
        t_state = last + (ring == 0 ? 3000 : 1200);
        if (ring == 0) { t_start = t_state; finish_ms = 0; newbest = 0; }
    }

    for (;;) {
        float dt;
        t_now = nv_millis();
        dt = (float)(t_now - last) / 1000.0f;
        if (dt > 0.05f) dt = 0.05f;
        last = t_now;
        if (nv_gfx_back()) { if (paused) break; paused = 1; pause_t = t_now; }
        in_poll(&in, g_w, g_h, (int)(dt * 1000));

        if (in_key_edge(&in, 0x06) || (in.tapped && in.tap_x > g_w - 150 && in.tap_y > g_h - 70)) view = !view;
        {
            static unsigned pad_prev_y;
            nv_pad_state_t ps;
            if (nv_pad_count() > 0 && nv_pad_state(0, &ps, sizeof ps) > 0) {
                if ((ps.buttons & NV_PADB_Y) && !(pad_prev_y & NV_PADB_Y)) view = !view;
                pad_prev_y = ps.buttons;
            }
        }

        if (paused) {
            if (in.back_edge) break;
            if (in.confirm_edge) {
                const int d = t_now - pause_t;
                paused = 0; t_start += d; t_state += d;
            }
        } else if (in.pause_edge) {
            paused = 1; pause_t = t_now;
        } else if (state == ST_COUNT && t_now >= t_state) {
            state = ST_FLY;
        } else if (state == ST_CRASH && t_now >= t_state) {
            goto respawn;
        } else if (state == ST_DONE && in.confirm_edge && t_now - finish_ms > 1200) {
            ring = 0;
            goto respawn;
        }

        if (!paused && (state == ST_FLY || state == ST_DONE)) {
            /* ---- flight model ---- */
            const float sx = fclamp((float)in.move_x / 256.0f + (float)in.look_x * 0.04f, -1, 1);
            const float sy = fclamp(-(float)in.move_y / 256.0f - (float)in.look_y * 0.04f, -1, 1);
            const float thr = state == ST_DONE ? 0.6f : 1.0f + 0.9f * (float)in.boost / 256.0f;
            const float vmin = 40.0f * g_cs, vstall = 48.0f * g_cs;
            float tb = sx * 0.95f, cb, sb, cp, sp;
            int k;
            pl.bank += (tb - pl.bank) * fclamp(dt * 3.0f, 0, 1);
            pl.pitch += (sy * 0.9f - pl.pitch * 0.4f) * dt + (float)in.climb / 256.0f * 0.6f * dt;
            if (pl.v < vstall) pl.pitch -= (vstall - pl.v) / g_cs * 0.02f * dt;          /* stall: nose drops */
            pl.pitch = fclamp(pl.pitch, -0.75f, 0.75f);
            cb = fcos(pl.bank); sb = fsin(pl.bank); cp = fcos(pl.pitch); sp = fsin(pl.pitch);
            pl.yaw += 85.0f * g_cs * (sb / cb) / pl.v * dt;                                  /* coordinated turn */
            pl.v += (thr * 0.6f * 72.0f * g_cs - 0.6f * pl.v - 70.0f * g_cs * sp) * dt;      /* thrust, drag, gravity */
            pl.v = fclamp(pl.v, vmin, 175.0f * g_cs);
            pl.x += pl.v * cp * fcos(pl.yaw) * dt;
            pl.y += pl.v * cp * fsin(pl.yaw) * dt;
            pl.h += pl.v * sp * dt;
            if (pl.h > ((float)g_maxh * g_vs) + 220.0f * g_cs) pl.h = (float)g_maxh * g_vs + 220.0f * g_cs;

            /* ---- collisions: fuselage, nose and wing tips against ground and water ---- */
            if (state == ST_FLY) {
                static const float probe[4][3] = { { 0, 0, -0.45f }, { 4.2f, 0, 0 }, { -1.0f, -4.8f, 0.3f }, { -1.0f, 4.8f, 0.3f } };
                for (k = 0; k < 4; k++) {
                    const v3 p = plane_point(&pl, probe[k]);
                    if (p.h < ground_at(p.x, p.y)) break;
                }
                if (k < 4) {
                    state = ST_CRASH;
                    t_state = t_now + 1600;
                    rumble(500);
                }
            }
            /* ---- terrain ahead on the flight path ---- */
            warn = 0;
            for (k = 1; k <= 4 && state == ST_FLY; k++) {
                const float t = 0.45f * (float)k;
                const float x = pl.x + pl.v * cp * fcos(pl.yaw) * t, y = pl.y + pl.v * cp * fsin(pl.yaw) * t;
                if (pl.h + pl.v * sp * t < ground_at(x, y) + 3.0f * g_cs) warn = 1;
            }
            /* ---- rings: crossing the ring's plane inside the radius ---- */
            if (state == ST_FLY) {
                const ring_t *g = &g_ring[ring];
                const float s0 = (prev.x - g->c.x) * g->n.x + (prev.y - g->c.y) * g->n.y;
                const float s1 = (pl.x - g->c.x) * g->n.x + (pl.y - g->c.y) * g->n.y;
                if (s0 < 0 && s1 >= 0) {
                    const float t = s0 / (s0 - s1);
                    const float hx = prev.x + (pl.x - prev.x) * t - g->c.x, hy = prev.y + (pl.y - prev.y) * t - g->c.y;
                    const float hh = prev.h + (pl.h - prev.h) * t - g->c.h;
                    const float d = fsqrt(hx * hx + hy * hy + hh * hh);
                    if (d < g->r) {
                        rumble(60);
                        if (++ring == NRINGS) {
                            const int tt = t_now - t_start;
                            state = ST_DONE;
                            finish_ms = t_now;
                            if (!best || tt < best) { best = tt; newbest = 1; nv_save("best", &best, sizeof best); }
                            ring = 0;
                        }
                    } else if (d < g->r * 1.3f) {
                        pl.v *= 0.6f;                   /* clipped the rim */
                        pl.bank += 0.6f;
                        rim_until = t_now + 600;
                        rumble(200);
                    }
                }
            }
            prev.x = pl.x; prev.y = pl.y; prev.h = pl.h;
            if (state == ST_FLY) last_ok = pl;
        }

        /* ---- camera ---- */
        {
            const plane_t *p = state == ST_CRASH ? &last_ok : &pl;
            float d = cam_yaw - p->yaw;
            while (d > PI_F) d -= 6.2831853f;
            while (d < -PI_F) d += 6.2831853f;
            cam_yaw -= d * fclamp(dt * 4.0f, 0, 1);
            if (view == 0) {                    /* chase */
                const float back = 17.0f * g_cs, up = 5.0f * g_cs;
                float cz, sxp, syp;
                cam.p.x = p->x - fcos(cam_yaw) * back;
                cam.p.y = p->y - fsin(cam_yaw) * back;
                cam.p.h = p->h + up;
                if (cam.p.h < ground_at(cam.p.x, cam.p.y) + 2.0f * g_cs) cam.p.h = ground_at(cam.p.x, cam.p.y) + 2.0f * g_cs;
                cam.yaw = cam_yaw;
                cam.roll = -(fsin(p->bank) / fcos(p->bank)) * 0.35f;
                cam.pitch_px = 0;
                {   /* aim so the plane sits a little below the centre */
                    v3 q = { p->x, p->y, p->h };
                    if (project(&cam, q, &sxp, &syp, &cz)) cam.pitch_px = syp - (float)g_h * 0.62f;
                }
            } else {                            /* cockpit */
                cam.p.x = p->x; cam.p.y = p->y; cam.p.h = p->h + 0.6f * g_cs;
                cam.yaw = p->yaw;
                cam.roll = -(fsin(p->bank) / fcos(p->bank));
                cam.pitch_px = -(fsin(p->pitch) / fcos(p->pitch)) * g_F;
            }
            cam.pitch_px = fclamp(cam.pitch_px, -(float)g_h * 0.8f, (float)g_h * 0.8f);
        }
        engine_camera(&cam, t_now);
        vs_render();

        /* ---- world overlays: rings, plane ---- */
        if (state != ST_DONE) {
            const ring_t *g = &g_ring[ring], *g2 = &g_ring[(ring + 1) % NRINGS];
            float sx, sy, z;
            if (line_clear(&cam, g2->c)) draw_ring(&cam, g2, C565(150, 120, 50), C565(90, 70, 30));
            if (line_clear(&cam, g->c)) draw_ring(&cam, g, C565(255, 215, 40), C565(255, 130, 0));
            if (!project(&cam, g->c, &sx, &sy, &z) || sx < 0 || sx >= g_w || sy < 0 || sy >= g_h)
                draw_pointer(&cam, g, &pl);
        }
        if (view == 0 && state != ST_CRASH) { draw_shadow(&cam, &pl); draw_plane(&cam, &pl, t_now); }
        if (view == 1) hud_attitude(&cam);
        if (view == 1) {                        /* cockpit: a flight path marker */
            rect_s(g_w / 2 - 16, g_h / 2 - 1, 10, 3, 0xFFFF);
            rect_s(g_w / 2 + 6, g_h / 2 - 1, 10, 3, 0xFFFF);
            rect_s(g_w / 2 - 1, g_h / 2 - 1, 3, 3, C565(255, 80, 60));
        }

        /* ---- HUD ---- */
        {
            char a[24], b[24];
            fmt_int(fmt_str(fmt_int(a, state == ST_DONE ? NRINGS : ring), "/"), NRINGS);
            hud_label_value(16, 14, "RING", a, C565(255, 215, 40));
            hud_progress(18, 72, state == ST_DONE ? NRINGS : ring);
            hud_radar(&pl, ring, g_w - 70, g_h - 150, 52);
            fmt_time(b, state == ST_DONE ? finish_ms - t_start : t_now - t_start);
            {
                const int w = text_w(b, 5) + 24;
                plate_s((g_w - w) / 2, 12, w, 52, C565(8, 14, 26), 150);
                text_sh((g_w - text_w(b, 5)) / 2, 20, b, 5, 0xFFFF);
            }
            if (best) { fmt_time(a, best); hud_label_value(g_w - 150, 14, "BEST", a, C565(120, 230, 140)); }
            fmt_int(a, (int)(pl.v / g_cs * 3.0f));
            hud_label_value(16, g_h - 70, "SPEED", a, pl.v < 50.0f * g_cs ? C565(255, 120, 80) : 0xFFFF);
            fmt_int(b, (int)((pl.h - ground_at(pl.x, pl.y)) / g_cs * 4.0f));
            hud_label_value(150, g_h - 70, "ALT", b, warn ? C565(255, 80, 60) : 0xFFFF);
            plate_s(g_w - 120, g_h - 62, 104, 44, C565(8, 14, 26), 150);
            text_s(g_w - 106, g_h - 48, view ? "CHASE" : "COCKPIT", 2, C565(150, 170, 190));
            if (warn && ((t_now / 200) & 1)) center_text(g_h / 2 + 60, "PULL UP", 5, C565(255, 70, 50));
            if (t_now < rim_until) center_text(g_h / 2 + 110, "RIM!", 3, C565(255, 160, 60));
            if (state == ST_COUNT && !paused) {
                const int left = t_state - t_now;
                fmt_int(a, left / 1000 + 1);
                center_text(g_h / 2 - 110, left < 250 ? "GO!" : a, 12, 0xFFFF);
            }
            if (state == ST_CRASH) {
                plate_s(g_w / 2 - 170, g_h / 2 - 70, 340, 120, C565(60, 6, 6), 190);
                center_text(g_h / 2 - 50, "CRASH", 8, C565(255, 90, 70));
                center_text(g_h / 2 + 12, "BACK TO THE LAST RING", 2, 0xFFFF);
            }
            if (state == ST_DONE) {
                plate_s(g_w / 2 - 220, g_h / 2 - 110, 440, 210, C565(6, 30, 14), 200);
                center_text(g_h / 2 - 92, "FINISH", 7, C565(140, 255, 160));
                fmt_time(a, finish_ms - t_start);
                center_text(g_h / 2 - 30, a, 6, 0xFFFF);
                if (newbest) center_text(g_h / 2 + 22, "NEW RECORD!", 3, C565(255, 215, 40));
                else if (best) {
                    const int tt = finish_ms - t_start;
                    center_text(g_h / 2 + 22, tt <= best * 105 / 100 ? "GOLD" : tt <= best * 115 / 100 ? "SILVER" : "BRONZE", 3,
                                tt <= best * 105 / 100 ? C565(255, 215, 40) : tt <= best * 115 / 100 ? C565(200, 210, 220) : C565(205, 127, 50));
                }
                center_text(g_h / 2 + 66, "TAP OR ENTER TO FLY AGAIN", 2, C565(200, 220, 230));
            }
            if (in.last == IN_TOUCH) hud_stick(in.stick_on, in.stick_x0, in.stick_y0, in.stick_x, in.stick_y);
            if (paused) {
                hud_pause();
                center_text(g_h / 2 + 80, "PAUSED", 4, 0xFFFF);
                center_text(g_h / 2 + 124, "ENTER RESUME   ESC QUIT", 2, C565(200, 220, 230));
            }
        }
        if (!nv_gfx_present()) break;
    }
    vs_close();
}
