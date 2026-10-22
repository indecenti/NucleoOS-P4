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

static int32_t g_sin[VS_ANG_FULL];       /* Q16 */
static int g_tables_ready;

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

void vs_init_tables(void)
{
    const double pi = 3.14159265358979323846;
    int i;
    if (g_tables_ready) return;
    for (i = 0; i < VS_ANG_FULL; i++) {
        double a = (double)i * 2.0 * pi / VS_ANG_FULL;
        if (a > pi) a -= 2.0 * pi;
        g_sin[i] = (int32_t)(sin_poly(a) * 65536.0 + (a >= 0 ? 0.5 : -0.5));
    }
    g_tables_ready = 1;
}

int32_t vs_sin(int32_t ang) { return g_sin[ang & VS_ANG_MASK]; }
int32_t vs_cos(int32_t ang) { return g_sin[(ang + VS_ANG_FULL / 4) & VS_ANG_MASK]; }

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
        v->kz[n] = (int32_t)(((int64_t)v->focal * v->vscale_q8 << 8) / z);
        if (z > fog0) {
            int32_t t = (int32_t)(((int64_t)(z - fog0) << 8) / (zfar - fog0));   /* Q8 0..256 */
            if (t > 256) t = 256;
            f = (t * t * (768 - 2 * t)) >> 19;                                  /* smoothstep * 32 */
        }
        v->fog[n] = (uint8_t)(f > 32 ? 32 : f);
        /* footprint of a sample: the step along the ray or the column spacing, whichever is wider */
        fp = z / v->focal;                                                    /* Q8 cells per column */
        if (fp < dz) fp = dz;
        fp = (int32_t)(((int64_t)fp * (v->lod_q8 > 0 ? v->lod_q8 : 256)) >> 8);
        v->lod[n] = (uint8_t)(fp > 256 ? ilog2((uint32_t)fp >> 8) : 0);
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

void vs_view_fit(vs_view *v, int w, int h, int fov_deg)
{
    const double pi = 3.14159265358979323846;
    double half = fov_deg * pi / 360.0;
    v->w = w > VS_MAX_W ? VS_MAX_W : w;
    v->h = h > VS_MAX_H ? VS_MAX_H : h;
    v->focal = (int)(v->w * 0.5 * sin_poly(pi / 2 - half) / sin_poly(half) + 0.5);   /* w/2 / tan */
    if (v->focal < 16) v->focal = 16;
    if (!v->zfar) v->zfar = 1100;
    if (!v->vscale_q8) v->vscale_q8 = 205;
    if (!v->fog_start) v->fog_start = v->zfar / 4;
    if (!v->near_z) v->near_z = 48;
    if (!v->smooth_z) v->smooth_z = v->focal * 2 / 3;   /* where a cell shrinks under ~1.5 px */
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
}

/* Order of trade-offs (cheapest loss first when slow, best gain first when fast): step density
 * down to 160, then render scale, then step density to 96 and the textured near zone. */
int vs_tune(vs_governor *g, vs_view *v, int frame_ms)
{
    int32_t t = g->target_ms << 4;
    g->avg_ms_q4 += ((frame_ms << 4) - g->avg_ms_q4) / 8;
    if (g->cooldown > 0) { g->cooldown--; return 0; }
    if (g->avg_ms_q4 > t + t / 10) {                                 /* too slow */
        g->cooldown = 12;
        if (v->quality > 160) { v->quality -= 24; vs_setup(v); return 1; }
        if (g->scale_q8 > g->scale_min_q8) {
            g->scale_q8 -= 16;
            if (g->scale_q8 < g->scale_min_q8) g->scale_q8 = g->scale_min_q8;
            return 2;
        }
        if (v->quality > 96) { v->quality -= 16; vs_setup(v); return 1; }
        if (v->near_z > 16) { v->near_z -= 8; vs_setup(v); return 1; }
    } else if (g->avg_ms_q4 < t * 3 / 4) {                           /* headroom */
        g->cooldown = 24;
        if (v->near_z < 48) { v->near_z += 8; vs_setup(v); return 1; }
        if (v->quality < 160) { v->quality += 16; vs_setup(v); return 1; }
        if (g->scale_q8 < 256) {
            g->scale_q8 += 16;
            if (g->scale_q8 > 256) g->scale_q8 = 256;
            return 2;
        }
        if (v->quality < 320) { v->quality += 24; vs_setup(v); return 1; }
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

void vs_build_mips(vs_world *wd, uint32_t *buf)
{
    int l, nm = mip_levels(wd->log2size);
    wd->nmips = nm;
    for (l = 1; l < nm; l++) {
        const uint32_t *src = wd->mip[l - 1];
        int sl = wd->log2size - l + 1, dl = sl - 1;
        uint32_t side = 1u << dl, x, y;
        for (y = 0; y < side; y++)
            for (x = 0; x < side; x++) {
                uint32_t c[4], k, hs = 0, hm = 0, nw = 0, cs = 0, h;
                c[0] = src[vs_tidx(2 * x, 2 * y, sl)];
                c[1] = src[vs_tidx(2 * x + 1, 2 * y, sl)];
                c[2] = src[vs_tidx(2 * x, 2 * y + 1, sl)];
                c[3] = src[vs_tidx(2 * x + 1, 2 * y + 1, sl)];
                for (k = 0; k < 4; k++) {
                    uint32_t hk = (c[k] >> 16) & 255, ck = c[k] & 0xFFFF;
                    hs += hk;
                    if (hk > hm) hm = hk;
                    nw += (c[k] >> 24) & VS_CELL_WATER;
                    cs += (ck | (ck << 16)) & 0x07E0F81Fu;
                }
                cs = (cs >> 2) & 0x07E0F81Fu;
                h = (3 * hs / 4 + hm + 2) / 4;   /* (3 mean + max) / 4: peaks keep their silhouette */
                if (h > 255) h = 255;
                buf[vs_tidx(x, y, dl)] = ((c[0] >> 24 & 0xF0u) | (nw >= 2 ? VS_CELL_WATER : 0)) << 24 |
                                         (nw >= 2 ? (uint32_t)wd->water : h) << 16 | (uint16_t)(cs | cs >> 16);
            }
        wd->mip[l] = buf;
        buf += side * side;
    }
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

/* Bilinear colour (and height, if hq) at Q16 (x, y) on the full-resolution map; flags of the
 * nearest corner. */
static inline uint32_t bilerp(const uint32_t *m, int log2, uint32_t mask, int32_t x, int32_t y, int32_t *hq)
{
    uint32_t x0 = ((uint32_t)x >> 16) & mask, y0 = ((uint32_t)y >> 16) & mask;
    uint32_t x1 = (x0 + 1) & mask, y1 = (y0 + 1) & mask;
    uint32_t c00 = m[vs_tidx(x0, y0, log2)], c10 = m[vs_tidx(x1, y0, log2)];
    uint32_t c01 = m[vs_tidx(x0, y1, log2)], c11 = m[vs_tidx(x1, y1, log2)];
    uint32_t wx = ((uint32_t)x >> 11) & 31, wy = ((uint32_t)y >> 11) & 31, top, bot, r;
    top = ((spread(c00) * (32 - wx) + spread(c10) * wx) >> 5) & 0x07E0F81Fu;
    bot = ((spread(c01) * (32 - wx) + spread(c11) * wx) >> 5) & 0x07E0F81Fu;
    r = ((top * (32 - wy) + bot * wy) >> 5) & 0x07E0F81Fu;
    if (hq) {
        int32_t fx = (x >> 8) & 255, fy = (y >> 8) & 255;
        int32_t a = (int32_t)((c00 >> 16) & 255) * (256 - fx) + (int32_t)((c10 >> 16) & 255) * fx;
        int32_t b = (int32_t)((c01 >> 16) & 255) * (256 - fx) + (int32_t)((c11 >> 16) & 255) * fx;
        *hq = (a * (256 - fy) + b * fy) >> 12;
    }
    return (c00 & 0xFF000000u) | (uint16_t)(r | (r >> 16));
}

int32_t vs_height_at(const vs_world *wd, int32_t x, int32_t y)
{
    int32_t h;
    bilerp(wd->mip[0], wd->log2size, (1u << wd->log2size) - 1, x, y, &h);
    return h;
}

/* ---- the march ---------------------------------------------------------------------------------- */

void vs_render_strip(const vs_view *v, const vs_world *wd, const vs_sky *sky, const vs_cam *cam,
                     uint16_t *out, int x0, int x1, vs_stats *st)
{
    const int W = v->w, H = v->h, log2 = wd->log2size;
    const uint32_t mask = (1u << log2) - 1;
    const uint32_t *map0 = wd->mip[0];
    const int8_t *detail = v->detail_q8 ? wd->detail : 0;
    const int32_t det_shift = v->detail_q8 >= 384 ? 2 : v->detail_q8 >= 192 ? 3 : 4;
    const int32_t fx = vs_cos(cam->yaw), fy = vs_sin(cam->yaw);
    const int32_t rx = -fy, ry = fx;
    const int32_t camh = cam->h, hmax = wd->maxh << 4;
    const int32_t ys0 = H / 2 - cam->pitch;
    const int32_t sky_scale = sky->focal > 0 ? (int32_t)(((int64_t)sky->focal << 16) / v->focal) : 65536;
    const uint16_t fogc = sky->fog;
    const int nnear = v->nnear, nsmooth = v->nsmooth, nsteps = v->nsteps;
    uint32_t samples = 0, psky = 0, pnear = 0;
    int x;

    if (x1 > W) x1 = W;
    for (x = x0; x < x1; x++) {
        uint16_t *col = out + (uint32_t)(x - x0) * H;
        const int32_t cdx = v->col_dx_q16[x];
        const int32_t dx = fx + (int32_t)(((int64_t)rx * cdx) >> 16);
        const int32_t dy = fy + (int32_t)(((int64_t)ry * cdx) >> 16);
        const int32_t ys = ys0 + (int32_t)(((int64_t)cam->roll * (2 * x + 1 - W)) >> 17);
        const int32_t jh = H - 1 - ys;               /* horizon row, bottom-up */
        const uint16_t *sc = sky->px + (uint32_t)((((cam->yaw + v->col_ang[x]) & VS_ANG_MASK)
                                         * sky->w) >> VS_ANG_BITS) * sky->h;
        int32_t px = cam->x, py = cam->y, yb = 0, i;

        for (i = 0; i < nsteps; i++) {
            const int32_t ppx = px, ppy = py;
            uint32_t c;
            int32_t hq, j;
            px += (dx * v->dz_q8[i]) >> 8;
            py += (dy * v->dz_q8[i]) >> 8;
            if (i < nnear) {
                c = bilerp(map0, log2, mask, px, py, &hq);
            } else if (i < nsmooth) {
                const int l = v->lod[i] < wd->nmips ? v->lod[i] : wd->nmips - 1;
                c = bilerp(wd->mip[l], log2 - l, mask >> l, px >> l, py >> l, &hq);
            } else {
                const int l = v->lod[i] < wd->nmips ? v->lod[i] : wd->nmips - 1;
                const int ll = log2 - l;
                const uint32_t m = mask >> l;
                c = wd->mip[l][vs_tidx(((uint32_t)px >> (16 + l)) & m, ((uint32_t)py >> (16 + l)) & m, ll)];
                hq = (int32_t)((c >> 12) & 0xFF0);
            }
            j = jh + (((hq - camh) * v->kz[i]) >> 12);
            if (j > yb) {
                const uint32_t f = v->fog[i];
                const int32_t jj = j;                 /* unclamped: the span's true length */
                if (j > H) j = H;
                if (c & (VS_CELL_WATER << 24)) {
                    /* water: the sky mirrored about the horizon, rippled, half and half */
                    int32_t wob = vs_sin((i << 7) + cam->time_ms) >> 15;     /* -2..2 rows */
                    for (; yb < j; yb++) {
                        int32_t d = (int32_t)(((int64_t)(jh - yb + wob) * sky_scale) >> 16);
                        uint32_t s = d < 0 ? sc[0] : d < sky->h ? sc[d] : sky->top;
                        col[yb] = mix565(half565(c & 0xFFFF, s), fogc, f);
                    }
                } else if (i < nnear) {
                    /* per pixel: walk the ground from the previous sample to this one down the span */
                    const int32_t n = jj - yb < VS_MAX_H ? jj - yb : VS_MAX_H;
                    const int32_t r = v->recip[n];
                    const int32_t sx = (int32_t)(((int64_t)(px - ppx) * r) >> 16);
                    const int32_t sy = (int32_t)(((int64_t)(py - ppy) * r) >> 16);
                    int32_t qx = ppx + (sx >> 1), qy = ppy + (sy >> 1);
                    pnear += (uint32_t)(j - yb);
                    for (; yb < j; yb++, qx += sx, qy += sy) {
                        uint32_t p = bilerp(map0, log2, mask, qx, qy, 0);
                        uint16_t o = (uint16_t)p;
                        if (detail) {
                            const int8_t *t = detail + (((p >> 24) >> VS_MAT_SHIFT) << 12);
                            int32_t d = t[(((uint32_t)qy >> 12) & 63) << 6 | (((uint32_t)qx >> 12) & 63)];
                            o = d >= 0 ? mix565(o, 0xFFFF, (uint32_t)d >> det_shift)
                                       : mix565(o, 0x0000, (uint32_t)(-d) >> det_shift);
                        }
                        col[yb] = f ? mix565(o, fogc, f) : o;
                    }
                } else {
                    const uint16_t o = f ? mix565(c & 0xFFFF, fogc, f) : (uint16_t)c;
                    for (; yb < j; yb++) col[yb] = o;
                }
                if (yb >= H) break;
            }
            /* nothing farther can rise above what is painted: stop this column */
            if ((i & 7) == 7) {
                if (hmax <= camh) { if (yb >= jh) break; }
                else if (jh + (((hmax - camh) * v->kz[i]) >> 12) <= yb) break;
            }
        }
        samples += (uint32_t)i;

        if (yb < H) {
            psky += (uint32_t)(H - yb);
            for (; yb < H && yb <= jh; yb++) col[yb] = fogc;      /* haze below the horizon */
            for (; yb < H; yb++) {
                int32_t r = (int32_t)(((int64_t)(yb - jh) * sky_scale) >> 16);
                col[yb] = r < sky->h ? sc[r] : sky->top;
            }
        }
    }
    if (st) {
        st->samples = samples;
        st->pixels_sky = psky;
        st->pixels_near = pnear;
    }
}
