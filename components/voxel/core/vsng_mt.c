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

static void worker(void *arg, int core)
{
    vs_mt *mt = (vs_mt *)arg;
    const vs_view *v = mt->v;
    const int H = v->h, sw = mt->p.strip_w;
    const int fast = mt->buf[core][0] && mt->buf[core][1] && H <= mt->buf_h;
    int slot = 0, pending[2] = { 0, 0 }, s;
    vs_stats acc = { 0, 0, 0 }, st;
    uint32_t n = 0;

    while ((s = take_strip(mt)) < mt->nstrips) {
        const int x0 = s * sw, x1 = x0 + sw < v->w ? x0 + sw : v->w;
        uint16_t *dst = mt->fb + (size_t)x0 * H;
        if (fast) {
            if (pending[slot]) mt->p.copy_wait(mt->p.ctx, core, slot);
            vs_render_strip(v, mt->wd, mt->sky, mt->cam, mt->buf[core][slot], x0, x1, &st);
            mt->p.copy_start(mt->p.ctx, dst, mt->buf[core][slot], (size_t)(x1 - x0) * H * 2, core, slot);
            pending[slot] = 1;
            slot ^= 1;
        } else {
            vs_render_strip(v, mt->wd, mt->sky, mt->cam, dst, x0, x1, &st);
        }
        acc.samples += st.samples;
        acc.pixels_near += st.pixels_near;
        acc.pixels_sky += st.pixels_sky;
        n++;
    }
    if (pending[0]) mt->p.copy_wait(mt->p.ctx, core, 0);
    if (pending[1]) mt->p.copy_wait(mt->p.ctx, core, 1);
    mt->st[core] = acc;
    mt->strips_done[core] = n;
}

void vs_mt_frame(vs_mt *mt, const vs_view *v, const vs_world *wd, const vs_sky *sky, const vs_cam *cam,
                 uint16_t *fb, vs_stats *total)
{
    int c;
    mt->v = v; mt->wd = wd; mt->sky = sky; mt->cam = cam; mt->fb = fb;
    mt->nstrips = (v->w + mt->p.strip_w - 1) / mt->p.strip_w;
    __atomic_store_n(&mt->next, 0, __ATOMIC_RELAXED);
    if (mt->p.run_parallel && mt->p.ncores > 1) mt->p.run_parallel(mt->p.ctx, worker, mt);
    else worker(mt, 0);
    if (total) {
        memset(total, 0, sizeof *total);
        for (c = 0; c < mt->p.ncores; c++) {
            total->samples += mt->st[c].samples;
            total->pixels_near += mt->st[c].pixels_near;
            total->pixels_sky += mt->st[c].pixels_sky;
        }
    }
}
