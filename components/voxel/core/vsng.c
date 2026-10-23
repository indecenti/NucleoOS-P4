/* VoxelSpaceNG core renderer. See vsng.h for the conventions.
 *
 * One ray per screen column, marched front to back over the heightfield. A per-column "y-buffer"
 * (yb: the next unpainted row, bottom-up) means each pixel is written once; the column stops as
 * soon as it reaches the top, or as soon as even the highest cell of the map could no longer rise
 * above what is already painted. Everything per sample is integer: step lengths, 1/z factors, fog
 * and mip levels are tables shared by all columns.
 *
 * Up close (steps nearer than near_z) the spans between two samples are textured per pixel: the
 * ground position is interpolated down the span, colour is bilinear and a material detail texture
 * adds sub-cell grain, so the foreground is crisp instead of a mosaic of cells. Farther away the
 * march reads coarser mips: no shimmer, and the working set fits the caches.
 */
#include "vsng.h"

#ifdef VS_CYCLES                         /* device profiling: the platform supplies a cycle counter */
uint32_t vs_clock(void);
#define VS_T0() (c0 = vs_clock())
#define VS_TA(k) (cyc[k] += vs_clock() - c0)
#define VS_T1() (c1 = vs_clock())
#define VS_TB(k) (cyc[k] += vs_clock() - c1)
#else
#define VS_T0() ((void)0)
#define VS_TA(k) ((void)0)
#define VS_T1() ((void)0)
#define VS_TB(k) ((void)0)
#endif

#ifdef VS_PROFILE                        /* host profiling: what the frame spends its work on */
unsigned long long vs_prof[9];
#define VS_PROF(k, n) (vs_prof[k] += (unsigned long long)(n))
#else
#define VS_PROF(k, n) ((void)0)
#endif

/* Float only at init, without libm (the device build may be freestanding). */
static double sin_poly(double x)         /* x in [-pi, pi] */
{
    double x2 = x * x;
    return x * (1.0 - x2 / 6.0 * (1.0 - x2 / 20.0 * (1.0 - x2 / 42.0 * (1.0 - x2 / 72.0 *
           (1.0 - x2 / 110.0 * (1.0 - x2 / 156.0))))));
}

static double atan_poly(double x)
{
    int inv = 0, neg = 0;
    double r, x2;
    if (x < 0) { x = -x; neg = 1; }
    if (x > 1) { x = 1.0 / x; inv = 1; }
    x2 = x * x;
    r = x * (0.9998660 + x2 * (-0.3302995 + x2 * (0.1801410 + x2 * (-0.0851330 + x2 * 0.0208351))));
    if (inv) r = 1.5707963267948966 - r;
    return neg ? -r : r;
}

void vs_init_tables(void) {}   /* nothing left to build: vs_sin is a polynomial (no static tables) */

/* sin(2 pi ang / 4096), Q16, from a 5th-order fit on the quarter wave (max error ~11 / 65536):
 * no table, so nothing static takes internal RAM on small parts. */
int32_t vs_sin(int32_t ang)
{
    const int32_t a = ang & VS_ANG_MASK, q = a >> (VS_ANG_BITS - 2), r = a & (VS_ANG_FULL / 4 - 1);
    const int32_t x = ((q & 1) ? VS_ANG_FULL / 4 - r : r) << (15 - (VS_ANG_BITS - 2));   /* Q15, 0..1 */
    const int32_t x2 = (x * x) >> 15;
    const int32_t u = 21089 - ((x2 * 2385) >> 15);                 /* 0.6435776, 0.0727813: s(1) = 1 */
    const int32_t v = 51472 - ((x2 * u) >> 15);                    /* pi / 2 */
    const int32_t s = (x * v) >> 14;                               /* Q16 */
    return q >= 2 ? -s : s;
}
int32_t vs_cos(int32_t ang) { return vs_sin(ang + VS_ANG_FULL / 4); }

static int ilog2(uint32_t v)
{
    int l = 0;
    while (v > 1) { v >>= 1; l++; }
    return l;
}

void vs_setup(vs_view *v)
{
    const double pi = 3.14159265358979323846;
    int32_t z = 256, n = 0, x, nnear = 0, nsmooth = 0;
    int32_t zfar = v->zfar << 8, fog0 = v->fog_start << 8;
    int32_t growth = 96 * (v->quality > 0 ? v->quality : 256) / 256;   /* cells per +1 cell of step */
    vs_init_tables();
    if (v->w > VS_MAX_W) v->w = VS_MAX_W;
    if (v->h > VS_MAX_H) v->h = VS_MAX_H;
    if (growth < 16) growth = 16;
    while (z < zfar && n < VS_MAX_STEPS) {
        /* half-cell steps up close, then growing with distance */
        int32_t dz = z < (32 << 8) ? 128 : 256 + (z - (32 << 8)) / growth;
        int32_t f = 0, fp;
        z += dz;
        v->dz_q8[n] = dz;
        v->kz[n] = (int32_t)(((int64_t)(v->focal_y ? v->focal_y : v->focal) * v->vscale_q8 << 8) / z);
        if (z > fog0) {
            int32_t t = (int32_t)(((int64_t)(z - fog0) << 8) / (zfar - fog0));   /* Q8 0..256 */
            if (t > 256) t = 256;
            f = (t * t * (768 - 2 * t)) >> 19;                                  /* smoothstep * 32 */
        }
        v->fog[n] = (uint8_t)(f > 32 ? 32 : f);
        /* water's sky share (Fresnel, by distance): 10/32 at the bow, 26/32 from ~600 cells on */
        v->fres[n] = (uint8_t)(10 + (z >= (600 << 8) ? 16 : (int32_t)(((int64_t)z * 16) / (600 << 8))));
        /* footprint of a sample: the step along the ray or the column spacing, whichever is wider */
        fp = z / v->focal;                                                    /* Q8 cells per column */
        if (fp < dz) fp = dz;
        fp = (int32_t)(((int64_t)fp * (v->lod_q8 > 0 ? v->lod_q8 : 256)) >> 8);
        v->lod[n] = (uint8_t)(fp > 256 ? ilog2((uint32_t)fp >> 8) : 0);
        {   /* brilinear: through the second half of each octave, fade toward the next level, so a level
             * change is a blend instead of a jump that crawls across the ground as the camera moves */
            const int32_t lo = 256 << v->lod[n], t = fp > 256 ? (int32_t)(((int64_t)(fp - lo) << 8) / lo) : 0;
            v->lodf[n] = (uint8_t)(t < 128 ? 0 : (t - 128) >> 2 > 31 ? 31 : (t - 128) >> 2);
        }
        if (z < (v->near_z << 8)) nnear = n + 1;
        if (z < (v->smooth_z << 8)) nsmooth = n + 1;
        n++;
    }
    v->nsteps = n;
    v->nnear = nnear;
    v->nsmooth = nsmooth < nnear ? nnear : nsmooth;
    for (x = 0; x < v->w; x++) {
        double off = ((double)(2 * x + 1 - v->w) * 0.5) / v->focal;
        v->col_dx_q16[x] = (int32_t)(off * 65536.0);
        v->col_ang[x] = (int16_t)(atan_poly(off) * VS_ANG_FULL / (2.0 * pi));
    }
    v->recip[0] = 65535;
    for (x = 1; x <= VS_MAX_H; x++) v->recip[x] = (uint16_t)(65535 / x);
}

void vs_view_fit(vs_view *v, int w, int h, int fov_deg, int xscale)
{
    const double pi = 3.14159265358979323846;
    double half = fov_deg * pi / 360.0;
    v->xscale = xscale == 4 ? 4 : xscale == 2 ? 2 : 1;
    if (w > VS_MAX_W) w = VS_MAX_W;
    v->w = w / v->xscale;
    v->h = h > VS_MAX_H ? VS_MAX_H : h;
    v->focal_y = (int)(w * 0.5 * sin_poly(pi / 2 - half) / sin_poly(half) + 0.5);    /* screen w/2 / tan */
    if (v->focal_y < 16) v->focal_y = 16;
    v->focal = v->focal_y / v->xscale;                                             /* per traced column */
    if (!v->zfar) v->zfar = 1100;
    if (!v->vscale_q8) v->vscale_q8 = 205;
    if (!v->fog_start) v->fog_start = v->zfar / 4;
    if (!v->near_z) v->near_z = 48;
    if (!v->smooth_z) v->smooth_z = v->focal_y * 2 / 3; /* bilinear while a cell spans > ~1.5 px */
    if (!v->lod_q8) v->lod_q8 = 256;
    if (!v->detail_q8) v->detail_q8 = 256;
    if (!v->quality) v->quality = 256;
    vs_setup(v);
}

/* ---- adaptive quality --------------------------------------------------------------------------- */

void vs_governor_init(vs_governor *g, int target_ms, int scale_min_q8)
{
    g->target_ms = target_ms;
    g->scale_q8 = 256;
    g->scale_min_q8 = scale_min_q8;
    g->avg_ms_q4 = target_ms << 4;
    g->cooldown = 30;
    g->q_min = 192;
    g->near0 = 0;
}

/* Order of trade-offs, cheapest loss first when slow: step density down to 224, then interpolated
 * columns (render scale), then step density down to the floor (q_min, default 192) and the textured
 * near zone down to half. It never goes below those floors: past them a slow frame stays slow rather
 * than turning into a mosaic. Best gain first when there is headroom. */
int vs_tune(vs_governor *g, vs_view *v, int frame_ms)
{
    const int32_t t = g->target_ms << 4;
    const int qmin = g->q_min > 0 ? g->q_min : 192;
    if (!g->near0) g->near0 = v->near_z;
    g->avg_ms_q4 += ((frame_ms << 4) - g->avg_ms_q4) / 8;
    if (g->cooldown > 0) { g->cooldown--; return 0; }
    if (g->avg_ms_q4 > t + t / 10) {                                 /* too slow */
        g->cooldown = 12;
        if (v->quality > 224) { v->quality -= 16; vs_setup(v); return 1; }
        if (g->scale_q8 > g->scale_min_q8) {
            g->scale_q8 -= 16;
            if (g->scale_q8 < g->scale_min_q8) g->scale_q8 = g->scale_min_q8;
            return 2;
        }
        if (v->quality > qmin) { v->quality -= 16; vs_setup(v); return 1; }
        if (v->near_z > g->near0 / 2) { v->near_z -= g->near0 / 8 + 1; vs_setup(v); return 1; }
    } else if (g->avg_ms_q4 < t * 3 / 4) {                           /* headroom */
        g->cooldown = 24;
        if (v->near_z < g->near0) { v->near_z += g->near0 / 8 + 1; if (v->near_z > g->near0) v->near_z = g->near0; vs_setup(v); return 1; }
        if (v->quality < 256) { v->quality += 16; vs_setup(v); return 1; }
        if (g->scale_q8 < 256) {
            g->scale_q8 += 16;
            if (g->scale_q8 > 256) g->scale_q8 = 256;
            return 2;
        }
        if (v->quality < 320) { v->quality += 16; vs_setup(v); return 1; }
    }
    return 0;
}

/* ---- mips --------------------------------------------------------------------------------------- */

static int mip_levels(int log2size)
{
    int n = log2size - 1;                /* smallest level 4x4 */
    if (n > VS_MAX_MIPS) n = VS_MAX_MIPS;
    return n < 1 ? 1 : n;
}

uint32_t vs_mip_cells(int log2size)
{
    uint32_t n = 0;
    int l, nm = mip_levels(log2size);
    for (l = 1; l < nm; l++) n += 1u << (2 * (log2size - l));
    return n;
}

/* nearest palette shade of colour group g to RGB565 colour c (build time only) */
static uint32_t pal_near(const uint16_t *pal, uint32_t g, uint32_t c)
{
    const int r = (int)(c >> 11), gr = (int)((c >> 5) & 63), b = (int)(c & 31);
    uint32_t k, best = g << 4;
    int bd = 0x7FFFFFFF;
    for (k = g << 4; k < (g << 4) + 16; k++) {
        const int dr = r - (int)(pal[k] >> 11), dg = gr - (int)((pal[k] >> 5) & 63), db = b - (int)(pal[k] & 31);
        const int d = 4 * dr * dr + dg * dg + 4 * db * db;
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

void vs_build_mips(vs_world *wd, uint16_t *buf)
{
    int l, nm = mip_levels(wd->log2size);
    wd->nmips = nm;
    for (l = 1; l < nm; l++) {
        const uint16_t *src = wd->mip[l - 1];
        int sl = wd->log2size - l + 1, dl = sl - 1;
        uint32_t side = 1u << dl, x, y;
        for (y = 0; y < side; y++)
            for (x = 0; x < side; x++) {
                uint32_t c[4], k, hs = 0, hm = 0, nw = 0, cs = 0, h, g;
                c[0] = src[vs_tidx(2 * x, 2 * y, sl)];
                c[1] = src[vs_tidx(2 * x + 1, 2 * y, sl)];
                c[2] = src[vs_tidx(2 * x, 2 * y + 1, sl)];
                c[3] = src[vs_tidx(2 * x + 1, 2 * y + 1, sl)];
                for (k = 0; k < 4; k++) {
                    const uint32_t hk = c[k] >> 7, ck = wd->pal[c[k] & 127];
                    hs += hk;
                    if (hk > hm) hm = hk;
                    nw += ((c[k] & 127) >> 4) == VS_G_WATER;
                    cs += (ck | (ck << 16)) & 0x07E0F81Fu;
                }
                cs = (cs >> 2) & 0x07E0F81Fu;
                h = (3 * hs / 4 + hm + 2) / 4;   /* (3 mean + max) / 4: peaks keep their silhouette */
                if (h > 511) h = 511;
                g = nw >= 2 ? VS_G_WATER : (c[0] & 127) >> 4;
                if (nw >= 2) h = (uint32_t)wd->water * 2;
                else if (g == VS_G_WATER) g = (c[3] & 127) >> 4;                 /* a dry shade, not water */
                if (g == VS_G_WATER && nw < 2) g = 0;
                buf[vs_tidx(x, y, dl)] = (uint16_t)(h << 7 | pal_near(wd->pal, g, (uint16_t)(cs | cs >> 16)));
            }
        wd->mip[l] = buf;
        buf += side * side;
    }
}

int vs_maxb_shift(int log2size)
{
    const int s = log2size - 7;
    return s < 2 ? 2 : s;
}

uint32_t vs_maxb_bytes(int log2size, int shift)
{
    return 1u << (2 * (log2size - shift));
}

void vs_build_maxb(vs_world *wd, uint8_t *buf, int shift)
{
    const int lb = wd->log2size - shift, B = 1 << shift;
    const uint32_t mask = (1u << wd->log2size) - 1, nb = 1u << lb;
    uint32_t bx, by;
    int x, y;
    for (by = 0; by < nb; by++)
        for (bx = 0; bx < nb; bx++) {
            uint32_t m = 0;
            for (y = -2; y < B + 2; y++)
                for (x = -2; x < B + 2; x++) {
                    const uint32_t cx = ((uint32_t)((int)(bx << shift) + x)) & mask;
                    const uint32_t cy = ((uint32_t)((int)(by << shift) + y)) & mask;
                    const uint32_t h = wd->mip[0][vs_tidx(cx, cy, wd->log2size)] >> 7;
                    if (h > m) m = h;
                }
            m = (m + 1) >> 1;                                  /* half units -> units, rounded up */
            buf[(by << lb) | bx] = (uint8_t)(m > 255 ? 255 : m);
        }
    wd->maxb = buf;
    wd->maxb_shift = shift;
}

uint32_t vs_maxs_bytes(int log2size, int shift)
{
    return 1u << (2 * (log2size - shift - 2));
}

void vs_build_maxs(vs_world *wd, uint8_t *buf)
{
    const int lb = wd->log2size - wd->maxb_shift, ls = lb - 2;
    const uint32_t ns = 1u << ls, msk = ns - 1;
    uint32_t sx, sy, bx, by;
    int dx, dy;
    uint8_t *tmp = buf;                                        /* pass 1 in place: 4x4 block max */
    if (!wd->maxb || ls < 1) return;
    for (sy = 0; sy < ns; sy++)
        for (sx = 0; sx < ns; sx++) {
            uint8_t m = 0;
            for (by = 0; by < 4; by++)
                for (bx = 0; bx < 4; bx++) {
                    const uint8_t v = wd->maxb[(((sy << 2) + by) << lb) | ((sx << 2) + bx)];
                    if (v > m) m = v;
                }
            tmp[(sy << ls) | sx] = m;
        }
    /* pass 2: dilate by one super block, rows then columns (separable max), with a small row buffer */
    for (sy = 0; sy < ns; sy++) {
        uint8_t row[512];
        for (sx = 0; sx < ns && sx < 512; sx++) {
            uint8_t m = 0;
            for (dx = -1; dx <= 1; dx++) {
                const uint8_t v = tmp[(sy << ls) | ((sx + (uint32_t)dx) & msk)];
                if (v > m) m = v;
            }
            row[sx] = m;
        }
        for (sx = 0; sx < ns && sx < 512; sx++) tmp[(sy << ls) | sx] = row[sx];
    }
    for (sx = 0; sx < ns && sx < 512; sx++) {
        uint8_t colv[512];
        for (sy = 0; sy < ns; sy++) {
            uint8_t m = 0;
            for (dy = -1; dy <= 1; dy++) {
                const uint8_t v = tmp[(((sy + (uint32_t)dy) & msk) << ls) | sx];
                if (v > m) m = v;
            }
            colv[sy] = m;
        }
        for (sy = 0; sy < ns; sy++) tmp[(sy << ls) | sx] = colv[sy];
    }
    wd->maxs = buf;
}

/* ---- pixel helpers ------------------------------------------------------------------------------ */

/* a -> b by f/32, both RGB565: the three channels spread so one multiply blends them all. */
static inline uint16_t mix565(uint32_t a, uint32_t b, uint32_t f)
{
    uint32_t A = (a | (a << 16)) & 0x07E0F81Fu, B = (b | (b << 16)) & 0x07E0F81Fu;
    uint32_t r = ((A * (32 - f) + B * f) >> 5) & 0x07E0F81Fu;
    return (uint16_t)(r | (r >> 16));
}

static inline uint16_t half565(uint32_t a, uint32_t b)
{
    return (uint16_t)(((a & 0xF7DEu) >> 1) + ((b & 0xF7DEu) >> 1));
}

static inline uint32_t spread(uint32_t c) { c &= 0xFFFF; return (c | (c << 16)) & 0x07E0F81Fu; }

#if defined(__GNUC__)
#define VS_HOT static inline __attribute__((always_inline))
#else
#define VS_HOT static inline
#endif

/* A cell expanded for the march: IIIIIII HHHHHHHHH CCCCCCCCCCCCCCCC (index, height, palette colour). */
#define VS_X(pal, c16) (((uint32_t)(c16) & 127u) << 25 | ((uint32_t)(c16) >> 7) << 16 | (pal)[(c16) & 127])
#define VS_XGROUP(x) ((x) >> 29)
#define VS_XHQ(x) ((int32_t)(((x) >> 13) & 0xFF8))       /* height, Q4 units */

/* Bilinear colour (and height, if hq) at Q16 (x, y) on a tiled map; index of the nearest corner.
 * The tile index is computed once: inside a 4x4 tile the neighbours are +1 / +4, across a tile edge
 * +13 / +(4 << log2) - 12, and only on the map's own edge (the wrap) the full index. vs_tidx is
 * separable (x and y bits are disjoint), so the diagonal is the sum of the two offsets. */
VS_HOT uint32_t bilerp(const uint16_t *m, const uint16_t *pal, int log2, uint32_t mask, int32_t x, int32_t y,
                       int32_t *hq)
{
    const uint32_t x0 = ((uint32_t)x >> 16) & mask, y0 = ((uint32_t)y >> 16) & mask;
    const uint32_t i00 = vs_tidx(x0, y0, log2);
    const uint32_t ox = (x0 & 3) != 3 ? 1u : x0 != mask ? 13u : vs_tidx(0, y0, log2) - i00;
    const uint32_t oy = (y0 & 3) != 3 ? 4u : y0 != mask ? (4u << log2) - 12u : vs_tidx(x0, 0, log2) - i00;
    const uint32_t c00 = m[i00], c10 = m[i00 + ox], c01 = m[i00 + oy], c11 = m[i00 + ox + oy];
    const uint32_t wx = ((uint32_t)x >> 11) & 31, wy = ((uint32_t)y >> 11) & 31;
    uint32_t top, bot, r;
    top = ((spread(pal[c00 & 127]) * (32 - wx) + spread(pal[c10 & 127]) * wx) >> 5) & 0x07E0F81Fu;
    bot = ((spread(pal[c01 & 127]) * (32 - wx) + spread(pal[c11 & 127]) * wx) >> 5) & 0x07E0F81Fu;
    r = ((top * (32 - wy) + bot * wy) >> 5) & 0x07E0F81Fu;
    if (hq) {
        const int32_t fx = (x >> 8) & 255, fy = (y >> 8) & 255;
        const int32_t a = (int32_t)(c00 >> 7) * (256 - fx) + (int32_t)(c10 >> 7) * fx;
        const int32_t b = (int32_t)(c01 >> 7) * (256 - fx) + (int32_t)(c11 >> 7) * fx;
        *hq = (a * (256 - fy) + b * fy) >> 13;
    }
    return (c00 & 127u) << 25 | (uint16_t)(r | (r >> 16));
}

int32_t vs_height_at(const vs_world *wd, int32_t x, int32_t y)
{
    int32_t h;
    bilerp(wd->mip[0], wd->pal, wd->log2size, (1u << wd->log2size) - 1, x, y, &h);
    return h;
}

void vs_frame(vs_view *v, const vs_cam *cam)
{
    /* distance of the camera along the view direction, Q8 cells, in world terms */
    const int64_t s = ((int64_t)cam->x * vs_cos(cam->yaw) + (int64_t)cam->y * vs_sin(cam->yaw)) >> 24;
    const int32_t fy = v->focal_y ? v->focal_y : v->focal;
    int64_t w = s, zp = 0;
    int i;
    v->pinned = 0;
    if (v->nopin) {
        int32_t z = 256;
        for (i = 0; i < v->nsteps; i++) { z += v->dz_q8[i]; v->pz[i] = z; }
        return;
    }
    for (i = 0; i < v->nsteps; i++) {
        int64_t P = 128, wn, zn;
        while (P * 3 / 2 < v->dz_q8[i]) P <<= 1;              /* the power of two nearest the step */
        wn = (w >= 0 ? w / P : -((-w + P - 1) / P)) * P + P;  /* next plane of that lattice */
        zn = wn - s;
        v->pdz[i] = (int32_t)(zn - zp);
        v->pkz[i] = (int32_t)(((int64_t)fy * v->vscale_q8 << 8) / zn);
        v->pz[i] = (int32_t)zn;
        zp = zn;
        w = wn;
    }
    v->pinned = 1;
}

/* ---- the march ---------------------------------------------------------------------------------- */

/* Columns march in groups of VS_GROUP, step by step: at each distance the group's rays read cells a
 * few apart, so a fetched cache line serves the whole group before it is evicted (one column at a
 * time walked ~300 lines across the map and threw them away before its neighbour came back for
 * them), and the step's tables (length, 1/z, fog, mip level) are read once for the group. */
#define VS_GROUP 8
#define VS_RS_MAX 96                      /* super blocks along a ray (zfar / 64 cells + 2) */

typedef struct {
    int32_t px, py, dx, dy, jh, yb;
    uint16_t *col;
    const uint16_t *sc;
    uint32_t pc;                         /* colour of the previous sample (span start) */
    uint16_t pend;                       /* colour of a span top that covers part of row yb */
    uint16_t pcov;                       /* ...and how much of it, 0..15 sixteenths (0 = none) */
    uint8_t rem[VS_RS_MAX];              /* tallest terrain from super block k on along the ray */
    int live;
} vs_ray;

void vs_render_strip(const vs_view *v, const vs_world *wd, const vs_sky *sky, const vs_cam *cam,
                     uint16_t *out, int x0, int x1, vs_stats *st)
{
    const int W = v->w, H = v->h, log2 = wd->log2size;
    const uint32_t mask = (1u << log2) - 1;
    const uint16_t *map0 = wd->mip[0], *pal = wd->pal;
    const int8_t *detail = v->detail_q8 ? wd->detail : 0;
    const int32_t det_shift = v->detail_q8 >= 384 ? 2 : v->detail_q8 >= 192 ? 3 : 4;
    const int32_t fx = vs_cos(cam->yaw), fy = vs_sin(cam->yaw);
    const int32_t rx = -fy, ry = fx;
    const int32_t camh = cam->h, hmax = wd->maxh << 4;
    const int32_t ys0 = H / 2 - cam->pitch;
    const int32_t fy_ = v->focal_y ? v->focal_y : v->focal, xsc = v->xscale ? v->xscale : 1;
    const int32_t sky_scale = sky->focal > 0 ? (int32_t)(((int64_t)sky->focal << 16) / fy_) : 65536;
    const uint16_t fogc = sky->fog;
    const int nnear = v->nnear, nsmooth = v->nsmooth, nsteps = v->nsteps;
    const uint8_t *maxb = wd->maxb;
    const int bshift = wd->maxb_shift, blog2 = log2 - bshift;
    const uint32_t bmask = (1u << blog2) - 1;
    const uint8_t *maxs = wd->maxs;
    const int sshift = bshift + 2, slog2 = log2 - sshift;
    const uint32_t smask = (1u << slog2) - 1;
    const int nrs = (v->pz[v->nsteps - 1] >> (8 + sshift)) + 2 < VS_RS_MAX ? (v->pz[v->nsteps - 1] >> (8 + sshift)) + 2 : VS_RS_MAX;
    uint32_t samples = 0, psky = 0, pnear = 0;
    vs_ray rays[VS_GROUP];
    int g;
#ifdef VS_CYCLES
    uint32_t cyc[7] = { 0, 0, 0, 0, 0, 0, 0 }, c0 = 0, c1 = 0;
#endif

    if (x1 > W) x1 = W;
    for (g = x0; g < x1; g += VS_GROUP) {
        const int n = x1 - g < VS_GROUP ? x1 - g : VS_GROUP;
        int k, i, live = n;
        VS_T0();

        for (k = 0; k < n; k++) {
            const int x = g + k;
            const int32_t cdx = v->col_dx_q16[x];
            const int32_t ys = ys0 + (int32_t)(((int64_t)cam->roll * xsc * (2 * x + 1 - W)) >> 17);
            vs_ray *r = &rays[k];
            r->dx = fx + (int32_t)(((int64_t)rx * cdx) >> 16);
            r->dy = fy + (int32_t)(((int64_t)ry * cdx) >> 16);
            r->jh = H - 1 - ys;                     /* horizon row, bottom-up */
            r->px = cam->x;
            r->py = cam->y;
            r->yb = 0;
            r->col = out + (uint32_t)(x - x0) * H;
            r->sc = sky->px + (uint32_t)((((cam->yaw + v->col_ang[x]) & VS_ANG_MASK) * sky->w) >> VS_ANG_BITS) * sky->h;
            r->pc = 0xFFFFFFFFu;
            r->pcov = 0;
            if (maxs) {   /* the tallest terrain still ahead, every super-block side along the ray */
                int32_t q;
                uint8_t m = 0;
                for (q = nrs - 1; q >= 0; q--) {
                    const int32_t d = q << sshift;                          /* cells */
                    const uint32_t cx = ((uint32_t)(cam->x + r->dx * d) >> (16 + sshift)) & smask;
                    const uint32_t cy = ((uint32_t)(cam->y + r->dy * d) >> (16 + sshift)) & smask;
                    const uint8_t v = maxs[(cy << slog2) | cx];
                    if (v > m) m = v;
                    r->rem[q] = m;
                }
            }
            r->live = 1;
        }

        VS_TA(0);
        for (i = 0; i < nsteps && live; i++) {
            const int32_t dz = v->pinned ? v->pdz[i] : v->dz_q8[i], kz = v->pinned ? v->pkz[i] : v->kz[i];
            const uint32_t f = v->fog[i];
            const int l = v->lod[i] < wd->nmips ? v->lod[i] : wd->nmips - 1;
            const uint16_t *mp = wd->mip[l];
            const int ll = log2 - l;
            const uint32_t ml = mask >> l;
            /* the farthest any cell can reach on screen from here on, for the early exit */
            const int32_t jtop = hmax <= camh ? 0x7FFFFFFF : (((hmax - camh) * kz) >> 12);
            /* level blend (brilinear) only where samples are single cells: two reads, the far mips are
             * in internal RAM; in the bilinear zone it would cost eight reads for little */
            const uint32_t tb = l + 1 < wd->nmips && i >= nsmooth ? v->lodf[i] : 0;
            /* block-max skipping must cover every base cell the sample can read: a bilinear sample on
             * level l reaches 2^(l+1) cells (2^(l+2) while blending with l+1), a nearest one 2^l. Up to
             * the dilation (2) one block lookup covers it, up to a block side the four corner blocks do. */
            const int32_t reach = i < nsmooth ? 2 << l : 1 << (l + (tb ? 1 : 0));
            const int skipok = maxb && reach <= (1 << bshift);
            for (k = 0; k < n; k++) {
                vs_ray *r = &rays[k];
                const int32_t ppx = r->px, ppy = r->py, jh = r->jh;
                uint16_t *col = r->col;
                int32_t yb = r->yb, hq, j;
                uint32_t c;
                if (!r->live) continue;
                VS_T0();
                r->px += (r->dx * dz) >> 8;
                r->py += (r->dy * dz) >> 8;
                if (skipok) {
                    const uint32_t bx0 = ((uint32_t)r->px >> (16 + bshift)) & bmask, by0 = ((uint32_t)r->py >> (16 + bshift)) & bmask;
                    int32_t top = maxb[(by0 << blog2) | bx0];
                    if (reach > 2) {
                        const uint32_t bx1 = (((uint32_t)r->px >> 16) + (uint32_t)reach) >> bshift & bmask;
                        const uint32_t by1 = (((uint32_t)r->py >> 16) + (uint32_t)reach) >> bshift & bmask;
                        const int32_t t1 = maxb[(by0 << blog2) | bx1], t2 = maxb[(by1 << blog2) | bx0],
                                      t3 = maxb[(by1 << blog2) | bx1];
                        if (t1 > top) top = t1;
                        if (t2 > top) top = t2;
                        if (t3 > top) top = t3;
                    }
                    if (jh + (((top << 4) - camh) * kz >> 12) <= yb) {
                        VS_PROF(8, 1);
                        r->pc = 0xFFFFFFFFu;               /* the next span starts fresh */
                        VS_TA(1);
                        goto next;
                    }
                }
                if (i < nnear) {
                    VS_PROF(0, 1);
                    c = bilerp(map0, pal, log2, mask, r->px, r->py, &hq);
                } else if (i < nsmooth) {
                    VS_PROF(1, 1);
                    c = bilerp(mp, pal, ll, ml, r->px >> l, r->py >> l, &hq);
                } else {
                    VS_PROF(2, 1);
                    const uint32_t c16 = mp[vs_tidx(((uint32_t)r->px >> (16 + l)) & ml,
                                                    ((uint32_t)r->py >> (16 + l)) & ml, ll)];
                    c = VS_X(pal, c16);
                    hq = VS_XHQ(c);
                    if (tb) {
                        const uint32_t m2 = ml >> 1;
                        const uint32_t d16 = wd->mip[l + 1][vs_tidx(((uint32_t)r->px >> (17 + l)) & m2,
                                                                    ((uint32_t)r->py >> (17 + l)) & m2, ll - 1)];
                        c = (c & 0xFFFF0000u) | mix565(c & 0xFFFF, pal[d16 & 127], tb);
                        hq += ((int32_t)((d16 >> 7) << 3) - hq) * (int32_t)tb >> 5;
                    }
                }
                VS_TA(1);
                VS_T0();
                const int32_t jf = ((hq - camh) * kz) >> 8;  /* span top, Q4 rows */
                j = jh + (jf >> 4);
                if (j > yb) {
                    const int32_t jj = j;             /* unclamped: the span's true length */
                    const int32_t y0 = yb;
                    if (j > H) j = H;
                    VS_PROF(3, 1);
                    VS_T1();
                    if (VS_XGROUP(c) == VS_G_WATER) {
                        VS_PROF(4, j - yb);
                        /* water: the sky mirrored about the horizon, rippled; Fresnel: the farther (the
                         * more grazing) the more mirror, the nearer the more of the water's own colour.
                         * Weights fixed per span, so a pixel costs one multiply-add on spread RGB. */
                        const int32_t tw = ((i << 7) + cam->time_ms) & 4095;          /* ripple: triangle wave */
                        const int32_t wob = ((tw < 2048 ? tw : 4095 - tw) >> 9) - 2;   /* -2..1 rows */
                        const uint16_t *sc = r->sc;
                        const uint32_t fr = v->fres[i];
                        const uint32_t wsk = (fr * (32 - f)) >> 5, wc = ((32 - fr) * (32 - f)) >> 5;
                        const uint32_t K = spread(c) * wc + spread(fogc) * (32 - wc - wsk);
                        int32_t dq = (jh - yb + wob) * sky_scale;                     /* sky row, Q16 */
                        for (; yb < j; yb++, dq -= sky_scale) {
                            const int32_t d = dq >> 16;
                            const uint32_t sp = ((K + spread(d < 0 ? sc[0] : d < sky->h ? sc[d] : sky->top) * wsk) >> 5)
                                                & 0x07E0F81Fu;
                            col[yb] = (uint16_t)(sp | (sp >> 16));
                        }
                        VS_TB(2);
                    } else if (i < nnear) {
                        /* per pixel, cheaply: the colour runs from the previous sample to this one down
                         * the span (both already bilinear), the ground position walks with it and picks
                         * the material's detail grain - one table read, no map reads per pixel */
                        const int32_t m = jj - yb < VS_MAX_H ? jj - yb : VS_MAX_H;
                        const int32_t rc = v->recip[m];
                        const int32_t sx = (int32_t)(((int64_t)(r->px - ppx) * rc) >> 16);
                        const int32_t sy = (int32_t)(((int64_t)(r->py - ppy) * rc) >> 16);
                        const uint32_t A = spread(r->pc != 0xFFFFFFFFu ? r->pc : i ? bilerp(map0, pal, log2, mask, ppx, ppy, 0) : c),
                                       B = spread(c);   /* span start: the previous sample, re-read if it was skipped */
                        /* detail grain fades out toward the edge of the near zone (full at the bow): unfiltered
                         * texels shrinking under a pixel would sparkle as the camera moves */
                        const int32_t amp = ((nnear - i) << 5) / (nnear ? nnear : 1);
                        const int8_t *t = detail && amp > 3 ? detail + (VS_XGROUP(c) << 12) : 0;
                        const uint32_t ws = (uint32_t)rc << 5;                  /* 32 / n, Q16 */
                        uint32_t wq = ws >> 1;
                        int32_t qx = ppx + (sx >> 1), qy = ppy + (sy >> 1);
                        pnear += (uint32_t)(j - yb);
                        VS_PROF(5, j - yb);
                        for (; yb < j; yb++, qx += sx, qy += sy, wq += ws) {
                            const uint32_t w = wq >> 16 > 32 ? 32 : wq >> 16;
                            const uint32_t sp = ((A * (32 - w) + B * w) >> 5) & 0x07E0F81Fu;
                            uint16_t o = (uint16_t)(sp | (sp >> 16));
                            if (t) {
                                int32_t d = (t[(((uint32_t)qy >> 13) & 63) << 6 | (((uint32_t)qx >> 13) & 63)] * amp) >> 5;
                                o = d >= 0 ? mix565(o, 0xFFFF, (uint32_t)d >> det_shift)
                                           : mix565(o, 0x0000, (uint32_t)(-d) >> det_shift);
                            }
                            col[yb] = f ? mix565(o, fogc, f) : o;
                        }
                        VS_TB(3);
                    } else {
                        const uint16_t o = f ? mix565(c & 0xFFFF, fogc, f) : (uint16_t)c;
                        VS_PROF(6, j - yb);
                        for (; yb < j; yb++) col[yb] = o;
                        VS_TB(4);
                    }
                    VS_T1();
                    /* anti-aliased ridges: the row this span's top only partly covered (left pending by
                     * the previous span) gets its colour blended over what turned out to lie behind it;
                     * this span's own top fraction is left pending for whatever comes next */
                    if (r->pcov) col[y0] = mix565(col[y0], r->pend, (uint32_t)r->pcov * 2);
                    r->pcov = jj < H ? (uint16_t)(jf & 15) : 0;
                    r->pend = col[yb - 1];
                    r->yb = yb;
                    VS_TB(5);
                }
                r->pc = c;
            next:
                /* the top reached, or nothing farther can rise above what is painted */
                if (yb >= H) {
                    r->live = 0;
                    live--;
                    samples += (uint32_t)(i + 1);
                } else if ((i & 3) == 3) {
                    int32_t t = jtop;                   /* tallest cell of the map, or of what is left ahead */
                    if (maxs) {
                        const int32_t q = (v->pz[i] >> 8) >> sshift;
                        const int32_t hr = (int32_t)r->rem[q < nrs ? q : nrs - 1] << 4;
                        t = hr <= camh ? 0x7FFFFFFF : (((hr - camh) * kz) >> 12);
                    }
                    if (t == 0x7FFFFFFF ? yb >= jh : jh + t <= yb) {
                        r->live = 0;
                        live--;
                        samples += (uint32_t)(i + 1);
                    }
                }
            }
        }
        VS_T0();
        for (k = 0; k < n; k++) {
            vs_ray *r = &rays[k];
            int32_t yb = r->yb;
            const int32_t jh = r->jh;
            if (r->live) samples += (uint32_t)nsteps;
            if (yb < H) {
                const int32_t y0 = yb;
                psky += (uint32_t)(H - yb);
                VS_PROF(7, H - yb);
                for (; yb < H && yb <= jh; yb++) r->col[yb] = fogc;      /* haze below the horizon */
                {
                    int32_t q = (yb - jh) * sky_scale;                       /* sky row, Q16 */
                    for (; yb < H; yb++, q += sky_scale) r->col[yb] = (q >> 16) < sky->h ? r->sc[q >> 16] : sky->top;
                }
                if (r->pcov) r->col[y0] = mix565(r->col[y0], r->pend, (uint32_t)r->pcov * 2);   /* ridge over sky */
            }
        }
        VS_TA(6);
    }
    if (st) {
#ifdef VS_CYCLES
        { int q; for (q = 0; q < 7; q++) st->cyc[q] = cyc[q]; }
#else
        { int q; for (q = 0; q < 7; q++) st->cyc[q] = 0; }
#endif
        st->samples = samples;
        st->pixels_sky = psky;
        st->pixels_near = pnear;
    }
}
