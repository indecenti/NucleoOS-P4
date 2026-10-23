/* VoxelSpaceNG multi-core frame. See vsng_mt.h. */
#include "vsng_mt.h"

#include <string.h>

static int take_strip(vs_mt *mt)
{
    return __atomic_fetch_add(&mt->next, 1, __ATOMIC_RELAXED);
}

int vs_mt_init(vs_mt *mt, const vs_mt_platform *p, int max_h)
{
    int c, s, ok = 1;
    memset(mt, 0, sizeof *mt);
    mt->p = *p;
    if (mt->p.ncores < 1) mt->p.ncores = 1;
    if (mt->p.ncores > VS_MT_MAX_CORES) mt->p.ncores = VS_MT_MAX_CORES;
    if (mt->p.strip_w < 1) mt->p.strip_w = 16;
    mt->buf_h = max_h;
    mt->buf_bytes = ((size_t)mt->p.strip_w * max_h * 2 + 63) & ~(size_t)63;
    if (!mt->p.alloc_fast || !mt->p.copy_start) return 0;
    for (c = 0; c < mt->p.ncores && ok; c++)
        for (s = 0; s < 2 && ok; s++) {
            mt->buf[c][s] = (uint16_t *)mt->p.alloc_fast(mt->p.ctx, mt->buf_bytes, c);
            if (!mt->buf[c][s]) ok = 0;
        }
    if (!ok) { vs_mt_free(mt); return -1; }
    return 0;
}

void vs_mt_free(vs_mt *mt)
{
    int c, s;
    for (c = 0; c < VS_MT_MAX_CORES; c++)
        for (s = 0; s < 2; s++)
            if (mt->buf[c][s]) {
                if (mt->p.free_fast) mt->p.free_fast(mt->p.ctx, mt->buf[c][s]);
                mt->buf[c][s] = 0;
            }
}

/* Half-rate columns: the strip holds n rendered columns at its start; spread them to 2n - 1 in place,
 * the odd ones the average of their neighbours (the last one repeats), from the end backwards so
 * no source column is overwritten before it is read. */
static void widen(uint16_t *b, int n, int out, int H)
{
    int k, y;
    for (k = n - 1; k >= 0; k--) {
        const uint16_t *a = b + (size_t)k * H, *c = k + 1 < n ? a + H : a;
        uint16_t *e = b + (size_t)(2 * k) * H, *o = e + H;
        if (2 * k + 1 < out)
            for (y = 0; y < H; y++) o[y] = (uint16_t)(((a[y] & 0xF7DEu) >> 1) + ((c[y] & 0xF7DEu) >> 1));
        if (k) for (y = 0; y < H; y++) e[y] = a[y];
    }
}

static void worker(void *arg, int core)
{
    vs_mt *mt = (vs_mt *)arg;
    const vs_view *v = mt->v;
    const int H = v->h, sw = mt->p.strip_w, xs = mt->xscale;
    const int fast = mt->buf[core][0] && mt->buf[core][1] && H <= mt->buf_h;
    int slot = 0, pending[2] = { 0, 0 }, s;
    vs_stats acc, st;
    memset(&acc, 0, sizeof acc);
    uint32_t n = 0;

    while ((s = take_strip(mt)) < mt->nstrips) {
        /* output columns [o0, o1) of the frame; rendered view columns [x0, x1) */
        const int o0 = s * sw, o1 = o0 + sw < mt->cols ? o0 + sw : mt->cols;
        const int x0 = o0 / xs, x1 = (o1 + xs - 1) / xs < v->w ? (o1 + xs - 1) / xs : v->w;
        uint16_t *dst = mt->fb + (size_t)o0 * H;
        uint16_t *b = fast ? mt->buf[core][slot] : dst;
        if (fast && pending[slot]) mt->p.copy_wait(mt->p.ctx, core, slot);
        vs_render_strip(v, mt->wd, mt->sky, mt->cam, b, x0, x1, &st);
        if (xs == 2) widen(b, x1 - x0, o1 - o0, H);
        if (fast) {
            mt->p.copy_start(mt->p.ctx, dst, b, (size_t)(o1 - o0) * H * 2, core, slot);
            pending[slot] = 1;
            slot ^= 1;
        }
        acc.samples += st.samples;
        acc.pixels_near += st.pixels_near;
        acc.pixels_sky += st.pixels_sky;
        { int q; for (q = 0; q < 7; q++) acc.cyc[q] += st.cyc[q]; }
        n++;
    }
    if (pending[0]) mt->p.copy_wait(mt->p.ctx, core, 0);
    if (pending[1]) mt->p.copy_wait(mt->p.ctx, core, 1);
    mt->st[core] = acc;
    mt->strips_done[core] = n;
}

void vs_mt_frame(vs_mt *mt, vs_view *v, const vs_world *wd, const vs_sky *sky, const vs_cam *cam,
                 uint16_t *fb, vs_stats *total)
{
    int c;
    vs_frame(v, cam);                    /* pinned steps: once per frame, before the cores start */
    mt->v = v; mt->wd = wd; mt->sky = sky; mt->cam = cam; mt->fb = fb;
    if (mt->xscale != 2) mt->xscale = 1;
    if (mt->xscale == 2 && (mt->p.strip_w & 1)) mt->xscale = 1;     /* strips must hold whole pairs */
    mt->cols = v->w * mt->xscale;
    mt->nstrips = (mt->cols + mt->p.strip_w - 1) / mt->p.strip_w;
    __atomic_store_n(&mt->next, 0, __ATOMIC_RELAXED);
    if (mt->p.run_parallel && mt->p.ncores > 1) mt->p.run_parallel(mt->p.ctx, worker, mt);
    else worker(mt, 0);
    if (total) {
        memset(total, 0, sizeof *total);
        for (c = 0; c < mt->p.ncores; c++) {
            total->samples += mt->st[c].samples;
            total->pixels_near += mt->st[c].pixels_near;
            total->pixels_sky += mt->st[c].pixels_sky;
            { int q; for (q = 0; q < 7; q++) total->cyc[q] += mt->st[c].cyc[q]; }
        }
    }
}
