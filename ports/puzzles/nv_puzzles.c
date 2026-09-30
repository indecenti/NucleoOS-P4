// nv_puzzles.c — NucleoOS front end for Simon Tatham's Portable Puzzle Collection (MIT).
//
// One WASI reactor app ("run" export) holding every puzzle (the upstream COMBINED build: gamelist[]
// from list.c). A menu grid picks a puzzle; the game screen has a top bar (Menu, New, Restart, Undo,
// Redo, Solve, Type = presets, Right-click toggle), an optional on-screen key row (the keys the
// puzzle asks for with request_keys: digits for Solo/Keen/Towers/Unequal/Filling, G/V/Z for Undead,
// ...) and a status line.
//
// Rendering: everything is drawn in software into one RGB565 framebuffer in linear memory (the
// upstream drawing API: rects, lines, polygons, circles, clipping, blitters, antialiased text from an
// embedded CC0 font) and only the rows that changed are blitted to the OS canvas, in one
// nv_gfx_blit call per frame, with the canvas in persist mode (nv_gfx_persist) so nothing else is
// re-sent. An idle screen costs three host calls per frame.
//
// Touch: a tap is a left click, a drag is a left drag, a long press (>= 450 ms without moving) is a
// right click (and its drag a right drag); the "Right" toggle in the bar swaps the two. Games are
// saved on leaving (midend_serialise into the app's data folder, "fs" permission) and resumed.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "puzzles.h"
#include "nucleo_sdk.h"
#include "pz_font.h"
#include "pz_meta.h"

// ---- screen, framebuffer, dirty rows ------------------------------------------------------------

#define TOP_H     64      // game top bar
#define STATUS_H  32      // game status line (bottom)
#define KEY_H     60      // one row of on-screen keys
#define MENU_TOP  56      // menu title bar

static int W = 1024, H = 600;
static uint16_t *fb;
static int dirty_y0 = 1, dirty_y1 = 0;          // rows [y0,y1) to blit; empty when y1 <= y0
static int clip_x0, clip_y0, clip_x1, clip_y1;   // current clip (screen px, exclusive)

static inline void mark_rows(int y0, int y1) {
    if (dirty_y1 <= dirty_y0) { dirty_y0 = y0; dirty_y1 = y1; return; }
    if (y0 < dirty_y0) dirty_y0 = y0;
    if (y1 > dirty_y1) dirty_y1 = y1;
}

static void flush(void) {
    if (dirty_y1 <= dirty_y0) return;
    int y0 = dirty_y0 < 0 ? 0 : dirty_y0, y1 = dirty_y1 > H ? H : dirty_y1;
    if (y1 > y0) nv_gfx_blit_raw(fb + (size_t)y0 * W, (y1 - y0) * W * 2, 0, y0, W, y1 - y0);
    dirty_y0 = 1; dirty_y1 = 0;
}

static void set_clip(int x0, int y0, int x1, int y1) {
    clip_x0 = x0 < 0 ? 0 : x0; clip_y0 = y0 < 0 ? 0 : y0;
    clip_x1 = x1 > W ? W : x1; clip_y1 = y1 > H ? H : y1;
}

// ---- software rasteriser (screen coordinates) ---------------------------------------------------

static void fill(int x0, int y0, int x1, int y1, uint16_t c) {   // [x0,x1) x [y0,y1)
    if (x0 < clip_x0) x0 = clip_x0;
    if (y0 < clip_y0) y0 = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;
    if (x0 >= x1 || y0 >= y1) return;
    mark_rows(y0, y1);
    for (int y = y0; y < y1; y++) {
        uint16_t *p = fb + (size_t)y * W + x0;
        for (int n = x1 - x0; n > 0; n--) *p++ = c;
    }
}

static inline void span(int y, int xa, int xb, uint16_t c) {   // inclusive
    fill(xa, y, xb + 1, y + 1, c);
}

static inline void plot(int x, int y, uint16_t c) {
    if (x < clip_x0 || y < clip_y0 || x >= clip_x1 || y >= clip_y1) return;
    fb[(size_t)y * W + x] = c;
    mark_rows(y, y + 1);
}

static void line(int x0, int y0, int x1, int y1, uint16_t c) {
    if (y0 == y1) { if (x0 > x1) { int t = x0; x0 = x1; x1 = t; } span(y0, x0, x1, c); return; }
    if (x0 == x1) { if (y0 > y1) { int t = y0; y0 = y1; y1 = t; } fill(x0, y0, x0 + 1, y1 + 1, c); return; }
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static int isqrt(int v) {
    if (v <= 0) return 0;
    int r = (int)sqrtf((float)v);
    while (r * r > v) r--;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

// Filled polygon, even-odd, sampled on the integer pixel grid (vertices are pixel centres, like the
// outline drawn through them afterwards). xy holds n screen-space points.
static void poly_fill(const int *xy, int n, uint16_t c) {
    int ymin = xy[1], ymax = xy[1];
    for (int i = 1; i < n; i++) {
        if (xy[2 * i + 1] < ymin) ymin = xy[2 * i + 1];
        if (xy[2 * i + 1] > ymax) ymax = xy[2 * i + 1];
    }
    if (ymin < clip_y0) ymin = clip_y0;
    if (ymax >= clip_y1) ymax = clip_y1 - 1;
    int64_t xs_stack[32], *xs = n <= 32 ? xs_stack : malloc(sizeof(int64_t) * n);
    if (!xs) return;
    for (int y = ymin; y <= ymax; y++) {
        int k = 0;
        int64_t yc = (int64_t)y << 16;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            int64_t ya = (int64_t)xy[2 * i + 1] << 16, yb = (int64_t)xy[2 * j + 1] << 16;
            int64_t xa = (int64_t)xy[2 * i] << 16, xb = (int64_t)xy[2 * j] << 16;
            if (ya == yb) continue;
            if (ya > yb) { int64_t t = ya; ya = yb; yb = t; t = xa; xa = xb; xb = t; }
            if (yc < ya || yc >= yb) continue;
            xs[k++] = xa + (xb - xa) * (yc - ya) / (yb - ya);
        }
        for (int a = 1; a < k; a++) {   // insertion sort: k is tiny
            int64_t v = xs[a]; int b = a - 1;
            while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; }
            xs[b + 1] = v;
        }
        for (int a = 0; a + 1 < k; a += 2) {
            int xl = (int)((xs[a] + 0xFFFF) >> 16), xr = (int)(xs[a + 1] >> 16);
            if (xr >= xl) span(y, xl, xr, c);
        }
    }
    if (xs != xs_stack) free(xs);
}

static void circle(int cx, int cy, int r, int fillc, uint16_t fc, uint16_t oc) {
    if (r < 0) return;
    int rr = r * r + r, ri = r - 1, rri = ri * ri + ri;
    for (int dy = -r; dy <= r; dy++) {
        int xo = isqrt(rr - dy * dy);
        int xi = (ri >= 0 && dy * dy <= rri) ? isqrt(rri - dy * dy) : -1;
        if (fillc && xi >= 0) span(cy + dy, cx - xi, cx + xi, fc);
        if (xi < 0) span(cy + dy, cx - xo, cx + xo, oc);
        else { span(cy + dy, cx - xo, cx - xi - 1, oc); span(cy + dy, cx + xi + 1, cx + xo, oc); }
        if (xi >= 0 && xo == xi) { plot(cx - xo, cy + dy, oc); plot(cx + xo, cy + dy, oc); }
    }
}

static void round_rect(int x, int y, int w, int h, int r, uint16_t c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        int in = 0;
        int d = j < r ? r - j : (j >= h - r ? j - (h - r - 1) : 0);
        if (d > 0) in = r - isqrt(r * r - (d - 1) * (d - 1) - (d - 1));
        if (in < 0) in = 0;
        fill(x + in, y + j, x + w - in, y + j + 1, c);
    }
}

// ---- text: the embedded atlas scaled to any size, cached per (size, char) ------------------------

static inline uint16_t blend565(uint16_t d, uint16_t s, int a /* 0..32 */) {
    uint32_t dd = (d | ((uint32_t)d << 16)) & 0x07E0F81F;
    uint32_t ss = (s | ((uint32_t)s << 16)) & 0x07E0F81F;
    uint32_t r = ((((ss - dd) * (uint32_t)a) >> 5) + dd) & 0x07E0F81F;
    return (uint16_t)(r | (r >> 16));
}

static inline int atlas_at(int g, int x, int y) {   // 0..15, 0 outside the glyph box
    int w = pzf_glyphs[g].w, h = pzf_glyphs[g].h;
    if (x < 0 || y < 0 || x >= w || y >= h) return 0;
    uint8_t b = pzf_data[pzf_glyphs[g].off + (size_t)y * ((w + 1) / 2) + x / 2];
    return (x & 1) ? b >> 4 : b & 15;
}

typedef struct { int key; short x0, y0, w, h; uint8_t *a; } glyph_img;   // a = 0..32 alpha
#define GCACHE 1024
static glyph_img gcache[GCACHE];
static int gcache_n;

static const glyph_img *glyph_get(int size, int ch) {
    int key = (size << 8) | ch;
    unsigned hsh = ((unsigned)key * 2654435761u) >> 22;   // 10 bits
    for (int i = 0; i < GCACHE; i++) {
        glyph_img *e = &gcache[(hsh + i) & (GCACHE - 1)];
        if (e->key == key) return e;
        if (!e->key) break;
    }
    if (gcache_n > GCACHE * 3 / 4) {   // full: start over
        for (int i = 0; i < GCACHE; i++) { free(gcache[i].a); gcache[i].a = NULL; gcache[i].key = 0; }
        gcache_n = 0;
    }
    glyph_img *e = NULL;
    for (int i = 0; i < GCACHE; i++) {
        e = &gcache[(hsh + i) & (GCACHE - 1)];
        if (!e->key) break;
    }
    int g = ch - 32;
    const int S = PZF_SIZE;
    int gx0 = pzf_glyphs[g].x0, gy0 = pzf_glyphs[g].y0, gw = pzf_glyphs[g].w, gh = pzf_glyphs[g].h;
    e->key = key; gcache_n++;
    e->a = NULL; e->w = e->h = 0; e->x0 = e->y0 = 0;
    if (!gw || !gh) return e;
    int ox0 = (int)floorf((float)gx0 * size / S), oy0 = (int)floorf((float)gy0 * size / S);
    int ox1 = (int)ceilf((float)(gx0 + gw) * size / S), oy1 = (int)ceilf((float)(gy0 + gh) * size / S);
    int w = ox1 - ox0, h = oy1 - oy0;
    if (w <= 0 || h <= 0) return e;
    uint8_t *a = malloc((size_t)w * h);
    if (!a) return e;
    int n = (S + size - 1) / size;   // samples per axis when shrinking
    if (n < 1) n = 1;
    if (n > 6) n = 6;
    for (int v = 0; v < h; v++) {
        for (int u = 0; u < w; u++) {
            int acc = 0;
            for (int sj = 0; sj < n; sj++) {
                for (int si = 0; si < n; si++) {
                    // Sample point in atlas px (fixed 8.8), bilinear between atlas pixel centres.
                    float fx = ((ox0 + u) + (si + 0.5f) / n) * S / size - gx0 - 0.5f;
                    float fy = ((oy0 + v) + (sj + 0.5f) / n) * S / size - gy0 - 0.5f;
                    int ix = (int)floorf(fx), iy = (int)floorf(fy);
                    int tx = (int)((fx - ix) * 256), ty = (int)((fy - iy) * 256);
                    int p00 = atlas_at(g, ix, iy), p10 = atlas_at(g, ix + 1, iy);
                    int p01 = atlas_at(g, ix, iy + 1), p11 = atlas_at(g, ix + 1, iy + 1);
                    int top = p00 * (256 - tx) + p10 * tx, bot = p01 * (256 - tx) + p11 * tx;
                    acc += (top * (256 - ty) + bot * ty) >> 8;   // 0..15*256
                }
            }
            int al = acc * 32 / (n * n * 15 * 256);
            a[v * w + u] = (uint8_t)(al > 32 ? 32 : al);
        }
    }
    e->a = a; e->x0 = ox0; e->y0 = oy0; e->w = w; e->h = h;
    return e;
}

static int text_width(int size, const char *s) {   // px
    int64_t adv = 0;
    for (; *s; s++) {
        int ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        adv += pzf_glyphs[ch - 32].adv16;
    }
    return (int)((adv * size + PZF_SIZE * 8) / (PZF_SIZE * 16));
}

// Text with its baseline at y (ALIGN_VCENTRE: y is the middle of the capitals).
static void text(int x, int y, int size, int align, uint16_t c, const char *s) {
    if (size < 4) size = 4;
    if (size > 255) size = 255;
    if (align & ALIGN_HCENTRE) x -= text_width(size, s) / 2;
    else if (align & ALIGN_HRIGHT) x -= text_width(size, s);
    if (align & ALIGN_VCENTRE) y += (PZF_CAP * size + PZF_SIZE) / (2 * PZF_SIZE);
    int64_t pen = (int64_t)x * 16 * PZF_SIZE;   // in 1/(16*S) px
    for (; *s; s++) {
        int ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const glyph_img *gi = glyph_get(size, ch);
        int gx = (int)(pen / (16 * PZF_SIZE)) + gi->x0, gy = y + gi->y0;
        if (gi->a) {
            int u0 = 0, v0 = 0, u1 = gi->w, v1 = gi->h;
            if (gx + u0 < clip_x0) u0 = clip_x0 - gx;
            if (gy + v0 < clip_y0) v0 = clip_y0 - gy;
            if (gx + u1 > clip_x1) u1 = clip_x1 - gx;
            if (gy + v1 > clip_y1) v1 = clip_y1 - gy;
            if (u0 < u1 && v0 < v1) {
                mark_rows(gy + v0, gy + v1);
                for (int v = v0; v < v1; v++) {
                    const uint8_t *ar = gi->a + v * gi->w;
                    uint16_t *p = fb + (size_t)(gy + v) * W + gx;
                    for (int u = u0; u < u1; u++) {
                        int al = ar[u];
                        if (al >= 32) p[u] = c;
                        else if (al) p[u] = blend565(p[u], c, al);
                    }
                }
            }
        }
        pen += (int64_t)pzf_glyphs[ch - 32].adv16 * size;
    }
}

// ---- state ---------------------------------------------------------------------------------------

enum { SCR_MENU, SCR_GAME, SCR_PRESETS, SCR_FATAL };
static int screen = SCR_MENU;
static bool lang_it;

struct frontend { int unused; };
static frontend fe_obj;

static midend *me;
static int cur = -1;                 // index into gamelist / pz_meta
static uint16_t *pal;                // game colours as RGB565
static int npal;
static key_label *keys;
static int nkeys, nkeys_all;       // shown / allocated
static char status_msg[160];         // status_bar() text of the puzzle
static char note_msg[160];           // our own transient note (solve errors, load errors)
static bool timer_on;
static int timer_last;
static int area_y0, area_y1;         // the game area between the bars (screen rows)
static int ox, oy, gw, gh;           // the puzzle's drawing origin and size on screen
static bool rmode;                   // swap left/right click
static bool saved[64];               // a save file exists for game i
static bool ui_dirty;                // redraw bars / status line
static int last_undo = -1, last_redo = -1, last_status = 99;

// Presets (flattened preset menu tree) and paging
typedef struct { char title[64]; game_params *params; int id; } pz_preset;
static pz_preset presets[128];
static int npresets, preset_page;

static const char *tr(const char *en, const char *it) { return lang_it ? it : en; }

static uint16_t rgb565f(float r, float g, float b) {
    int R = (int)(r * 255 + 0.5f), G = (int)(g * 255 + 0.5f), B = (int)(b * 255 + 0.5f);
    return (uint16_t)NV_RGB(R < 0 ? 0 : R > 255 ? 255 : R, G < 0 ? 0 : G > 255 ? 255 : G,
                            B < 0 ? 0 : B > 255 ? 255 : B);
}
static inline uint16_t colour(int i) { return (i >= 0 && i < npal) ? pal[i] : 0; }

#define C_BG     NV_RGB(22, 25, 31)
#define C_BAR    NV_RGB(33, 37, 45)
#define C_BTN    NV_RGB(54, 60, 73)
#define C_BTN_ON NV_RGB(52, 120, 220)
#define C_TXT    NV_RGB(240, 242, 246)
#define C_DIM    NV_RGB(120, 126, 138)
#define C_SUB    NV_RGB(165, 172, 186)
#define C_OK     NV_RGB(90, 200, 120)
#define C_BAD    NV_RGB(235, 95, 85)
#define C_TILE   NV_RGB(40, 45, 55)

// ---- the upstream drawing API --------------------------------------------------------------------

static void nd_draw_text(drawing *dr, int x, int y, int fonttype, int fontsize, int align, int colour_i,
                         const char *s) {
    (void)dr; (void)fonttype;
    text(x + ox, y + oy, fontsize, align, colour(colour_i), s);
}
static void nd_draw_rect(drawing *dr, int x, int y, int w, int h, int c) {
    (void)dr;
    fill(x + ox, y + oy, x + ox + w, y + oy + h, colour(c));
}
static void nd_draw_line(drawing *dr, int x1, int y1, int x2, int y2, int c) {
    (void)dr;
    line(x1 + ox, y1 + oy, x2 + ox, y2 + oy, colour(c));
}
static void nd_draw_polygon(drawing *dr, const int *coords, int npoints, int fillc, int outc) {
    (void)dr;
    if (npoints < 2) return;
    int stackbuf[64], *xy = npoints <= 32 ? stackbuf : malloc(sizeof(int) * 2 * npoints);
    if (!xy) return;
    for (int i = 0; i < npoints; i++) { xy[2 * i] = coords[2 * i] + ox; xy[2 * i + 1] = coords[2 * i + 1] + oy; }
    if (fillc >= 0 && npoints >= 3) poly_fill(xy, npoints, colour(fillc));
    if (outc >= 0) {
        uint16_t oc = colour(outc);
        for (int i = 0; i < npoints; i++) {
            int j = (i + 1) % npoints;
            line(xy[2 * i], xy[2 * i + 1], xy[2 * j], xy[2 * j + 1], oc);
        }
    }
    if (xy != stackbuf) free(xy);
}
static void nd_draw_circle(drawing *dr, int cx, int cy, int radius, int fillc, int outc) {
    (void)dr;
    circle(cx + ox, cy + oy, radius, fillc >= 0, colour(fillc), colour(outc >= 0 ? outc : fillc));
}
static void nd_draw_update(drawing *dr, int x, int y, int w, int h) { (void)dr; (void)x; (void)y; (void)w; (void)h; }
static void nd_clip(drawing *dr, int x, int y, int w, int h) {
    (void)dr;
    set_clip(x + ox, y + oy, x + ox + w, y + oy + h);
    if (clip_y0 < area_y0) clip_y0 = area_y0;
    if (clip_y1 > area_y1) clip_y1 = area_y1;
}
static void nd_unclip(drawing *dr) { (void)dr; set_clip(0, area_y0, W, area_y1); }
static void nd_start_draw(drawing *dr) { (void)dr; set_clip(0, area_y0, W, area_y1); }
static void nd_end_draw(drawing *dr) { (void)dr; }
static void nd_status_bar(drawing *dr, const char *s) {
    (void)dr;
    snprintf(status_msg, sizeof status_msg, "%s", s);
    ui_dirty = true;
}

struct blitter { int w, h, x, y; uint16_t *px; };
static blitter *nd_blitter_new(drawing *dr, int w, int h) {
    (void)dr;
    blitter *bl = snew(blitter);
    bl->w = w; bl->h = h; bl->x = bl->y = 0;
    bl->px = snewn((size_t)(w > 0 ? w : 1) * (h > 0 ? h : 1), uint16_t);
    return bl;
}
static void nd_blitter_free(drawing *dr, blitter *bl) { (void)dr; sfree(bl->px); sfree(bl); }
static void nd_blitter_save(drawing *dr, blitter *bl, int x, int y) {
    (void)dr;
    bl->x = x; bl->y = y;
    for (int j = 0; j < bl->h; j++) {
        int sy = y + oy + j;
        if (sy < 0 || sy >= H) continue;
        for (int i = 0; i < bl->w; i++) {
            int sx = x + ox + i;
            if (sx >= 0 && sx < W) bl->px[j * bl->w + i] = fb[(size_t)sy * W + sx];
        }
    }
}
static void nd_blitter_load(drawing *dr, blitter *bl, int x, int y) {
    (void)dr;
    for (int j = 0; j < bl->h; j++) {
        int sy = y + oy + j;
        if (sy < clip_y0 || sy >= clip_y1) continue;
        for (int i = 0; i < bl->w; i++) {
            int sx = x + ox + i;
            if (sx >= clip_x0 && sx < clip_x1) fb[(size_t)sy * W + sx] = bl->px[j * bl->w + i];
        }
        mark_rows(sy, sy + 1);
    }
}

static const drawing_api nv_drawing = {
    1,   // drawing API version
    nd_draw_text, nd_draw_rect, nd_draw_line, nd_draw_polygon, nd_draw_circle, nd_draw_update,
    nd_clip, nd_unclip, nd_start_draw, nd_end_draw, nd_status_bar,
    nd_blitter_new, nd_blitter_free, nd_blitter_save, nd_blitter_load,
    NULL, NULL, NULL, NULL, NULL, NULL,   // printing: not supported
    NULL, NULL,                           // line_width / line_dotted (printing)
    NULL,                                 // text_fallback: the ASCII choice
    NULL,                                 // draw_thick_line: drawing.c's polygon fallback
};

// ---- the upstream front-end hooks ----------------------------------------------------------------

void get_random_seed(void **randseed, int *randseedsize) {
    uint32_t *s = snewn(4, uint32_t);
    s[0] = (uint32_t)nv_rand(); s[1] = (uint32_t)nv_rand();
    s[2] = (uint32_t)nv_millis(); s[3] = (uint32_t)nv_time_unix();
    *randseed = s;
    *randseedsize = 4 * sizeof(uint32_t);
}

void activate_timer(frontend *fe) {
    (void)fe;
    if (!timer_on) { timer_on = true; timer_last = nv_millis(); }
}
void deactivate_timer(frontend *fe) { (void)fe; timer_on = false; }

void frontend_default_colour(frontend *fe, float *output) {
    (void)fe;
    output[0] = output[1] = output[2] = 0.88F;
}

static void show_fatal(const char *msg);
void fatal(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    show_fatal(buf);
}

#ifdef DEBUGGING
void debug_printf(const char *fmt, ...) { (void)fmt; }
#endif

// Printing is not offered; midend.c still references this (printing.c is not built).
void document_add_puzzle(document *doc, const game *game, game_params *par, game_ui *ui,
                         game_state *st, game_state *st2) {
    (void)doc; (void)game; (void)par; (void)ui; (void)st; (void)st2;
}

// ---- buttons -------------------------------------------------------------------------------------

enum { B_NONE = 0, B_MENU, B_NEW, B_RESTART, B_UNDO, B_REDO, B_SOLVE, B_TYPE, B_RCLICK,
       B_BACK, B_PREV, B_NEXT, B_KEY0 = 100, B_PRESET0 = 300, B_TILE0 = 500 };
enum { ST_BAR, ST_KEY, ST_TILE, ST_PRESET };

typedef struct {
    short x, y, w, h;
    int id, style;
    bool enabled, active;
    char label[40];
} ui_btn;
static ui_btn btns[160];
static int nbtns;

static ui_btn *add_btn(int x, int y, int w, int h, int id, int style, const char *label) {
    if (nbtns >= (int)lenof(btns)) return NULL;
    ui_btn *b = &btns[nbtns++];
    b->x = x; b->y = y; b->w = w; b->h = h; b->id = id; b->style = style;
    b->enabled = true; b->active = false;
    snprintf(b->label, sizeof b->label, "%s", label ? label : "");
    return b;
}
static ui_btn *find_btn(int id) {
    for (int i = 0; i < nbtns; i++) if (btns[i].id == id) return &btns[i];
    return NULL;
}
static int hit_btn(int x, int y) {
    for (int i = 0; i < nbtns; i++) {
        ui_btn *b = &btns[i];
        if (x >= b->x && y >= b->y && x < b->x + b->w && y < b->y + b->h) return b->enabled ? b->id : B_NONE;
    }
    return B_NONE;
}

static void draw_btn(const ui_btn *b, bool pressed) {
    int sx0 = clip_x0, sy0 = clip_y0, sx1 = clip_x1, sy1 = clip_y1;
    set_clip(b->x, b->y, b->x + b->w, b->y + b->h);
    uint16_t bg = b->active ? C_BTN_ON : C_BTN;
    if (b->style == ST_TILE) bg = C_TILE;
    if (pressed) bg = NV_RGB(80, 90, 110);
    // Background of the strip the button sits on first (the rounded corners show it).
    fill(b->x, b->y, b->x + b->w, b->y + b->h, b->style == ST_TILE || b->style == ST_PRESET ? C_BG : C_BAR);
    round_rect(b->x, b->y, b->w, b->h, 10, bg);
    uint16_t fg = b->enabled ? C_TXT : C_DIM;
    if (b->style == ST_TILE) {
        int gi = b->id - B_TILE0;
        text(b->x + 14, b->y + 27, 21, ALIGN_HLEFT, fg, pz_meta[gi].name);
        text(b->x + 14, b->y + 50, 14, ALIGN_HLEFT, C_SUB, pz_meta[gi].desc);
        if (saved[gi]) circle(b->x + b->w - 16, b->y + 16, 5, 1, C_BTN_ON, C_BTN_ON);
    } else {
        int size = b->style == ST_KEY ? 26 : 20;
        while (size > 12 && text_width(size, b->label) > b->w - 12) size--;
        text(b->x + b->w / 2, b->y + b->h / 2, size, ALIGN_HCENTRE | ALIGN_VCENTRE, fg, b->label);
    }
    set_clip(sx0, sy0, sx1, sy1);
}

static void draw_all_btns(void) {
    for (int i = 0; i < nbtns; i++) draw_btn(&btns[i], false);
}

// ---- save / resume -------------------------------------------------------------------------------

static void save_path(int gi, char *out, size_t n) {
#ifdef NV_SIM   // PC tests: the data folder is $PZ_DATA
    snprintf(out, n, "%s/%s.sav", getenv("PZ_DATA") ? getenv("PZ_DATA") : ".", pz_meta[gi].id);
#else           // "fs" permission: the app's data folder is "/"
    snprintf(out, n, "/%s.sav", pz_meta[gi].id);
#endif
}

typedef struct { char *buf; size_t len, cap; } membuf;
static void mem_write(void *ctx, const void *buf, int len) {
    membuf *m = ctx;
    if (m->len + len > m->cap) {
        size_t nc = (m->cap ? m->cap * 2 : 4096);
        while (nc < m->len + len) nc *= 2;
        char *nb = realloc(m->buf, nc);
        if (!nb) return;
        m->buf = nb; m->cap = nc;
    }
    memcpy(m->buf + m->len, buf, len);
    m->len += len;
}
typedef struct { const char *buf; size_t len, pos; } memrd;
static bool mem_read(void *ctx, void *buf, int len) {
    memrd *r = ctx;
    if (len < 0 || r->len - r->pos < (size_t)len) return false;
    memcpy(buf, r->buf + r->pos, len);
    r->pos += len;
    return true;
}

static void save_game(void) {
    if (!me || cur < 0) return;
    membuf m = {0};
    midend_serialise(me, mem_write, &m);
    char path[256];
    save_path(cur, path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (f) {
        bool ok = fwrite(m.buf, 1, m.len, f) == m.len;
        ok = (fclose(f) == 0) && ok;
        saved[cur] = ok;
    }
    free(m.buf);
}

static bool load_game(int gi) {
    char path[256];
    save_path(gi, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char *buf = NULL;
    size_t len = 0, cap = 0, n;
    char tmp[4096];
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (len + n > cap) {
            cap = (len + n) * 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); return false; }
            buf = nb;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    fclose(f);
    memrd r = { buf, len, 0 };
    const char *err = len ? midend_deserialise(me, mem_read, &r) : "empty save";
    free(buf);
    return err == NULL;
}

static void scan_saves(void) {
    for (int i = 0; i < gamecount && i < (int)lenof(saved); i++) {
        char path[256];
        save_path(i, path, sizeof path);
        FILE *f = fopen(path, "rb");
        saved[i] = f != NULL;
        if (f) fclose(f);
    }
}

// ---- menu screen ---------------------------------------------------------------------------------

#define MENU_COLS 5
static void menu_layout(void) {
    nbtns = 0;
    int rows = (gamecount + MENU_COLS - 1) / MENU_COLS;
    int gap = 6, top = MENU_TOP + 4;
    int tw = (W - gap * (MENU_COLS + 1)) / MENU_COLS;
    int th = (H - top - gap * rows) / rows;
    if (th > 90) th = 90;
    for (int i = 0; i < gamecount; i++) {
        int c = i % MENU_COLS, r = i / MENU_COLS;
        add_btn(gap + c * (tw + gap), top + r * (th + gap), tw, th, B_TILE0 + i, ST_TILE, NULL);
    }
}

static void menu_draw(void) {
    set_clip(0, 0, W, H);
    fill(0, 0, W, H, C_BG);
    fill(0, 0, W, MENU_TOP, C_BAR);
    text(16, MENU_TOP / 2, 26, ALIGN_VCENTRE, C_TXT, "Puzzles");
    text(W - 16, MENU_TOP / 2, 15, ALIGN_VCENTRE | ALIGN_HRIGHT, C_SUB,
         tr("Simon Tatham's Portable Puzzle Collection", "Collezione di rompicapi di Simon Tatham"));
    draw_all_btns();
}

// ---- game screen ---------------------------------------------------------------------------------

static void free_game(void) {
    if (keys) { free_keys(keys, nkeys_all); keys = NULL; nkeys = nkeys_all = 0; }
    if (me) { midend_free(me); me = NULL; }
    sfree(pal); pal = NULL; npal = 0;
    timer_on = false;
    cur = -1;
}

static int key_rows(void) { return nkeys == 0 ? 0 : nkeys <= 12 ? 1 : 2; }

static void game_bar_layout(void) {
    nbtns = 0;
    static const struct { int id, w; const char *en, *it; } bar[] = {
        { B_MENU, 104, "Menu", "Menu" },       { B_NEW, 104, "New", "Nuova" },
        { B_RESTART, 124, "Restart", "Ricomincia" }, { B_UNDO, 104, "Undo", "Annulla" },
        { B_REDO, 104, "Redo", "Ripeti" },     { B_SOLVE, 104, "Solve", "Risolvi" },
        { B_TYPE, 120, "Type", "Tipo" },       { B_RCLICK, 150, "Right click", "Tasto destro" },
    };
    int x = 8;
    for (size_t i = 0; i < lenof(bar); i++) {
        ui_btn *b = add_btn(x, 8, bar[i].w, TOP_H - 16, bar[i].id, ST_BAR, tr(bar[i].en, bar[i].it));
        x += bar[i].w + 8;
        if (!b) continue;
        if (bar[i].id == B_SOLVE) b->enabled = gamelist[cur]->can_solve;
        if (bar[i].id == B_UNDO) b->enabled = midend_can_undo(me);
        if (bar[i].id == B_REDO) b->enabled = midend_can_redo(me);
        if (bar[i].id == B_RCLICK) b->active = rmode;
        if (bar[i].id == B_TYPE) b->enabled = npresets > 1;
    }
    int rows = key_rows();
    if (rows) {
        int per = (nkeys + rows - 1) / rows;
        int kh = KEY_H - 10, kw = (W - 8) / per - 8;
        if (kw > 110) kw = 110;
        for (int i = 0; i < nkeys; i++) {
            int r = i / per, c = i % per;
            int nrow = (r == rows - 1) ? nkeys - per * (rows - 1) : per;
            int x0 = (W - (nrow * (kw + 8) - 8)) / 2;
            int y0 = H - STATUS_H - rows * KEY_H + r * KEY_H + 5;
            add_btn(x0 + c * (kw + 8), y0, kw, kh, B_KEY0 + i, ST_KEY, keys[i].label);
        }
    }
}

static void status_draw(void) {
    int y0 = H - STATUS_H;
    set_clip(0, y0, W, H);
    fill(0, y0, W, H, C_BAR);
    int x = 12;
    text(x, y0 + STATUS_H / 2, 17, ALIGN_VCENTRE, C_TXT, pz_meta[cur].name);
    x += text_width(17, pz_meta[cur].name) + 16;
    const char *msg = note_msg[0] ? note_msg : (midend_wants_statusbar(me) && status_msg[0]) ? status_msg
                                                                                          : pz_meta[cur].objective;
    int st = midend_status(me);
    const char *right = st > 0 ? tr("Solved!", "Risolto!") : st < 0 ? tr("Lost", "Perso") : "";
    int rw = right[0] ? text_width(17, right) + 24 : 0;
    set_clip(x, y0, W - rw, H);
    text(x, y0 + STATUS_H / 2, 15, ALIGN_VCENTRE, note_msg[0] ? C_BAD : C_SUB, msg);
    set_clip(0, y0, W, H);
    if (right[0]) text(W - 12, y0 + STATUS_H / 2, 17, ALIGN_VCENTRE | ALIGN_HRIGHT, st > 0 ? C_OK : C_BAD, right);
}

static void bars_draw(void) {
    set_clip(0, 0, W, H);
    fill(0, 0, W, TOP_H, C_BAR);
    if (key_rows()) fill(0, area_y1, W, H - STATUS_H, C_BAR);
    draw_all_btns();
    status_draw();
    set_clip(0, area_y0, W, area_y1);
}

// Refresh the bits of the bars that depend on game state (cheap: only when something changed).
static void bars_update(void) {
    int u = midend_can_undo(me), r = midend_can_redo(me), st = midend_status(me);
    ui_btn *b;
    if (u != last_undo && (b = find_btn(B_UNDO))) { b->enabled = u; set_clip(0, 0, W, H); draw_btn(b, false); }
    if (r != last_redo && (b = find_btn(B_REDO))) { b->enabled = r; set_clip(0, 0, W, H); draw_btn(b, false); }
    if (st != last_status) ui_dirty = true;
    last_undo = u; last_redo = r; last_status = st;
    if (ui_dirty) { status_draw(); ui_dirty = false; }
    set_clip(0, area_y0, W, area_y1);
}

static void flatten_presets(struct preset_menu *m, const char *prefix) {
    for (int i = 0; m && i < m->n_entries && npresets < (int)lenof(presets); i++) {
        struct preset_menu_entry *e = &m->entries[i];
        if (e->submenu) {
            flatten_presets(e->submenu, e->title);
        } else {
            pz_preset *p = &presets[npresets++];
            if (prefix) snprintf(p->title, sizeof p->title, "%s: %s", prefix, e->title);
            else snprintf(p->title, sizeof p->title, "%s", e->title);
            p->params = e->params;
            p->id = e->id;
        }
    }
}

// Size the puzzle into the area between the bars and repaint the whole game screen.
static void game_layout(void) {
    if (keys) { free_keys(keys, nkeys_all); keys = NULL; nkeys = nkeys_all = 0; }
    keys = midend_request_keys(me, &nkeys_all);
    nkeys = nkeys_all > 24 ? 24 : nkeys_all;
    area_y0 = TOP_H;
    area_y1 = H - STATUS_H - key_rows() * KEY_H;
    int w = W - 16, h = area_y1 - area_y0 - 12;
    midend_size(me, &w, &h, true, 1.0);
    gw = w; gh = h;
    ox = (W - w) / 2;
    oy = area_y0 + (area_y1 - area_y0 - h) / 2;
    game_bar_layout();
    last_undo = midend_can_undo(me); last_redo = midend_can_redo(me); last_status = midend_status(me);
    set_clip(0, area_y0, W, area_y1);
    fill(0, area_y0, W, area_y1, colour(0));
    bars_draw();
    midend_force_redraw(me);
    ui_dirty = false;
}

static void busy_note(const char *msg) {
    // Shown before a slow generator runs: drawn and pushed to the panel right away.
    int bw = text_width(24, msg) + 60, bh = 70;
    int x = (W - bw) / 2, y = (area_y0 + area_y1 - bh) / 2;
    if (screen == SCR_MENU) y = (H - bh) / 2;
    set_clip(0, 0, W, H);
    round_rect(x, y, bw, bh, 14, C_BAR);
    text(W / 2, y + bh / 2, 24, ALIGN_HCENTRE | ALIGN_VCENTRE, C_TXT, msg);
    flush();
    nv_gfx_present();
}

static void game_open(int gi) {
    busy_note(tr("Preparing the puzzle...", "Preparo il rompicapo..."));
    cur = gi;
    me = midend_new(&fe_obj, gamelist[gi], &nv_drawing, NULL);
    status_msg[0] = note_msg[0] = 0;
    bool resumed = saved[gi] && load_game(gi);
    if (!resumed) midend_new_game(me);
    npal = 0;
    float *cols = midend_colours(me, &npal);
    pal = snewn(npal > 0 ? npal : 1, uint16_t);
    for (int i = 0; i < npal; i++) pal[i] = rgb565f(cols[3 * i], cols[3 * i + 1], cols[3 * i + 2]);
    sfree(cols);
    npresets = 0;
    int idlimit;
    flatten_presets(midend_get_presets(me, &idlimit), NULL);
    screen = SCR_GAME;
    game_layout();
}

static void game_close(void) {
    save_game();
    free_game();
    screen = SCR_MENU;
    menu_layout();
    menu_draw();
}

static void game_new(void) {
    busy_note(tr("New puzzle...", "Nuovo rompicapo..."));
    note_msg[0] = 0;
    midend_new_game(me);
    game_layout();
}

// ---- presets screen ------------------------------------------------------------------------------

#define PRESET_COLS 3
#define PRESET_ROWS 7
static void presets_layout(void) {
    nbtns = 0;
    add_btn(8, 8, 120, TOP_H - 16, B_BACK, ST_BAR, tr("Back", "Indietro"));
    int per = PRESET_COLS * PRESET_ROWS;
    int pages = (npresets + per - 1) / per;
    if (preset_page >= pages) preset_page = pages ? pages - 1 : 0;
    if (pages > 1) {
        ui_btn *b = add_btn(W - 8 - 2 * 100 - 8, 8, 100, TOP_H - 16, B_PREV, ST_BAR, "<");
        if (b) b->enabled = preset_page > 0;
        b = add_btn(W - 8 - 100, 8, 100, TOP_H - 16, B_NEXT, ST_BAR, ">");
        if (b) b->enabled = preset_page < pages - 1;
    }
    int which = midend_which_preset(me);
    int gap = 10, bw = (W - gap * (PRESET_COLS + 1)) / PRESET_COLS;
    int bh = (H - TOP_H - gap * (PRESET_ROWS + 1)) / PRESET_ROWS;
    for (int k = 0; k < per; k++) {
        int i = preset_page * per + k;
        if (i >= npresets) break;
        int c = k % PRESET_COLS, r = k / PRESET_COLS;
        ui_btn *b = add_btn(gap + c * (bw + gap), TOP_H + gap + r * (bh + gap), bw, bh, B_PRESET0 + i,
                            ST_PRESET, presets[i].title);
        if (b) b->active = presets[i].id == which;
    }
}

static void presets_draw(void) {
    set_clip(0, 0, W, H);
    fill(0, 0, W, H, C_BG);
    fill(0, 0, W, TOP_H, C_BAR);
    char title[96];
    snprintf(title, sizeof title, "%s - %s", pz_meta[cur].name, tr("choose the type", "scegli il tipo"));
    text(144, TOP_H / 2, 20, ALIGN_VCENTRE, C_TXT, title);
    draw_all_btns();
}

static void presets_open(void) {
    screen = SCR_PRESETS;
    int which = midend_which_preset(me), per = PRESET_COLS * PRESET_ROWS;
    preset_page = 0;
    for (int i = 0; i < npresets; i++) if (presets[i].id == which) preset_page = i / per;
    presets_layout();
    presets_draw();
}

static void presets_back(void) {
    screen = SCR_GAME;
    game_layout();
}

// ---- fatal errors --------------------------------------------------------------------------------

static void show_fatal(const char *msg) {
#ifdef NV_SIM
    fprintf(stderr, "fatal: %s\n", msg);
    abort();
#else
    set_clip(0, 0, W, H);
    fill(0, 0, W, H, C_BG);
    text(W / 2, H / 2 - 30, 26, ALIGN_HCENTRE | ALIGN_VCENTRE, C_BAD, tr("Puzzle error", "Errore del rompicapo"));
    text(W / 2, H / 2 + 20, 16, ALIGN_HCENTRE | ALIGN_VCENTRE, C_TXT, msg);
    flush();
    int x, y, was = 0;
    while (nv_gfx_present()) {
        int down = nv_touch(&x, &y);
        if (nv_gfx_back() || (was && !down)) break;
        was = down;
    }
    exit(1);   // the reactor's proc_exit: ends the run
#endif
}

// ---- input ---------------------------------------------------------------------------------------

#define LONG_MS   450
#define MOVE_PX   14

static struct {
    bool down;
    int x0, y0, t0;            // where/when the finger went down
    int btn;                   // pressed UI button id (B_NONE: none)
    bool in_game;              // started in the puzzle area
    bool pending;              // puzzle press not sent yet (deciding tap / drag / long press)
    int mbtn;                  // LEFT_BUTTON or RIGHT_BUTTON once sent
    int lx, ly;                // last position sent
    int cx, cy;                // last position seen while down (release reports may not carry one)
} tch;

static void send_mouse(int kind, int x, int y) {   // kind: 0 press, 1 drag, 2 release
    int b = tch.mbtn;
    int code = kind == 0 ? b : kind == 1 ? b + (LEFT_DRAG - LEFT_BUTTON) : b + (LEFT_RELEASE - LEFT_BUTTON);
    midend_process_key(me, x - ox, y - oy, code);
    tch.lx = x; tch.ly = y;
    if (kind != 1) note_msg[0] = 0, ui_dirty = true;
}

static void on_button(int id);

static void touch_update(int down, int x, int y) {
    int now = nv_millis();
    if (down) { tch.cx = x; tch.cy = y; }
    else { x = tch.cx; y = tch.cy; }
    if (down && !tch.down) {                                   // finger down
        tch.down = true; tch.x0 = x; tch.y0 = y; tch.t0 = now;
        tch.btn = hit_btn(x, y);
        tch.in_game = screen == SCR_GAME && tch.btn == B_NONE && y >= area_y0 && y < area_y1;
        tch.pending = tch.in_game;
        if (tch.btn) {
            ui_btn *b = find_btn(tch.btn);
            if (b) { set_clip(0, 0, W, H); draw_btn(b, true); }
        }
        return;
    }
    if (down && tch.down) {                                    // held / moving
        if (!tch.in_game) return;
        int moved = abs(x - tch.x0) > MOVE_PX || abs(y - tch.y0) > MOVE_PX;
        if (tch.pending) {
            if (moved) {
                tch.pending = false;
                tch.mbtn = rmode ? RIGHT_BUTTON : LEFT_BUTTON;
                send_mouse(0, tch.x0, tch.y0);
                send_mouse(1, x, y);
            } else if (now - tch.t0 >= LONG_MS) {
                tch.pending = false;
                tch.mbtn = rmode ? LEFT_BUTTON : RIGHT_BUTTON;
                nv_gfx_tone(1200, 25);                         // tells the finger the long press took
                send_mouse(0, tch.x0, tch.y0);
            }
        } else if (x != tch.lx || y != tch.ly) {
            send_mouse(1, x, y);
        }
        return;
    }
    if (!down && tch.down) {                                   // finger up
        tch.down = false;
        if (tch.in_game) {
            if (tch.pending) {
                tch.pending = false;
                tch.mbtn = rmode ? RIGHT_BUTTON : LEFT_BUTTON;
                send_mouse(0, tch.x0, tch.y0);
                send_mouse(2, tch.x0, tch.y0);
            } else {
                send_mouse(2, x, y);
            }
            return;
        }
        if (tch.btn) {
            ui_btn *b = find_btn(tch.btn);
            if (b) { set_clip(0, 0, W, H); draw_btn(b, false); }
            if (b && hit_btn(x, y) == tch.btn) on_button(tch.btn);
            if (screen == SCR_GAME) set_clip(0, area_y0, W, area_y1);
        }
    }
}

static void on_button(int id) {
    if (screen == SCR_MENU) {
        if (id >= B_TILE0 && id < B_TILE0 + gamecount) game_open(id - B_TILE0);
        return;
    }
    if (screen == SCR_PRESETS) {
        if (id == B_BACK) presets_back();
        else if (id == B_PREV || id == B_NEXT) {
            preset_page += id == B_NEXT ? 1 : -1;
            presets_layout();
            presets_draw();
        } else if (id >= B_PRESET0 && id < B_PRESET0 + npresets) {
            midend_set_params(me, presets[id - B_PRESET0].params);
            screen = SCR_GAME;
            area_y0 = TOP_H; area_y1 = H - STATUS_H;
            set_clip(0, 0, W, H);
            fill(0, 0, W, H, C_BG);
            game_new();
        }
        return;
    }
    set_clip(0, area_y0, W, area_y1);
    switch (id) {
    case B_MENU: game_close(); return;
    case B_NEW: game_new(); return;
    case B_RESTART: note_msg[0] = 0; midend_restart_game(me); break;
    case B_UNDO: midend_process_key(me, 0, 0, UI_UNDO); break;
    case B_REDO: midend_process_key(me, 0, 0, UI_REDO); break;
    case B_SOLVE: {
        const char *msg = midend_solve(me);
        snprintf(note_msg, sizeof note_msg, "%s", msg ? msg : "");
        ui_dirty = true;
        break;
    }
    case B_TYPE: presets_open(); return;
    case B_RCLICK: {
        rmode = !rmode;
        ui_btn *b = find_btn(B_RCLICK);
        if (b) { b->active = rmode; set_clip(0, 0, W, H); draw_btn(b, false); }
        break;
    }
    default:
        if (id >= B_KEY0 && id < B_KEY0 + nkeys) {
            note_msg[0] = 0; ui_dirty = true;
            midend_process_key(me, 0, 0, keys[id - B_KEY0].button);
        }
    }
    set_clip(0, area_y0, W, area_y1);
}

// USB keyboard / game pad: arrows move the puzzle's keyboard cursor, A/B select.
static void pad_update(void) {
    static int prev;
    int p = nv_gfx_pad();
    int edge = p & ~prev;
    prev = p;
    if (screen != SCR_GAME || !edge) return;
    static const struct { int bit, key; } map[] = {
        { NV_PAD_UP, CURSOR_UP }, { NV_PAD_DOWN, CURSOR_DOWN }, { NV_PAD_LEFT, CURSOR_LEFT },
        { NV_PAD_RIGHT, CURSOR_RIGHT }, { NV_PAD_A, CURSOR_SELECT }, { NV_PAD_B, CURSOR_SELECT2 },
    };
    set_clip(0, area_y0, W, area_y1);
    for (size_t i = 0; i < lenof(map); i++)
        if (edge & map[i].bit) midend_process_key(me, 0, 0, map[i].key);
}

// ---- main loop -----------------------------------------------------------------------------------

static bool handle_back(void) {   // false: leave the app
    if (screen == SCR_GAME) { game_close(); return true; }
    if (screen == SCR_PRESETS) { presets_back(); return true; }
    return false;
}

NV_EXPORT("run")
void run(void) {
    char lang[8] = {0};
    nv_lang(lang, sizeof lang);
    lang_it = lang[0] == 'i' && lang[1] == 't';
    W = nv_gfx_width(); H = nv_gfx_height();
    if (W <= 0 || H <= 0) return;
    fb = malloc((size_t)W * H * 2);
    if (!fb) return;
    nv_gfx_persist(1);
    scan_saves();
    screen = SCR_MENU;
    menu_layout();
    menu_draw();
    for (;;) {
        flush();
        if (!nv_gfx_present()) break;
        int backs = nv_gfx_back();
        if (backs && !tch.down) {
            if (!handle_back()) break;
            continue;
        }
        int x, y, down = nv_touch(&x, &y) != 0;
        touch_update(down, x, y);
        pad_update();
        if (screen == SCR_GAME && me) {
            if (timer_on) {
                int now = nv_millis();
                float dt = (now - timer_last) / 1000.0f;
                timer_last = now;
                if (dt > 1.0f) dt = 1.0f;
                if (dt > 0) { set_clip(0, area_y0, W, area_y1); midend_timer(me, dt); }
            }
            bars_update();
        }
    }
    if (screen != SCR_MENU && me) save_game();
    free_game();
}
