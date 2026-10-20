// gfx.c — the app's frame buffer: RGB565 at the canvas size, drawn by the app and sent with
// nv_gfx_blit one band of changed rows per frame (the host keeps the rest: nv_gfx_persist).
// Drawing goes to the screen or to a surface (a page drawn ahead of time); a page turn composes the
// screen from two surfaces, one memcpy per row and side. The render modes (RM_GREY for the e-ink
// look, RM_MONO for 1-bit) are applied where coverage becomes colour: glyph masks and rows of pixels.
// From the Biblioteca app's gfx.c (ports/zim/biblioteca), with surfaces and render modes.
#include "bk.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nucleo_sdk.h"

static uint16_t *s_fb;
static uint16_t **s_screen;   // the screen's rows
uint16_t **g_rows;
int g_w, g_h;                 // the screen as the app draws it (portrait: 600 x 1024)
int g_rmode;
int g_portrait;
static int s_pw, s_ph;        // the canvas as the system shows it (1024 x 600, the panel's own way)
static uint16_t *s_rot;       // portrait: the rotated columns on their way to the canvas
static int s_dy0 = INT_MAX, s_dy1 = -1;   // dirty rows of the screen
static int s_cx0, s_cy0, s_cx1, s_cy1;    // clip

static int alloc_screen(void) {
    free(s_fb);
    free(s_screen);
    free(s_rot);
    s_rot = NULL;
    g_w = g_portrait ? s_ph : s_pw;
    g_h = g_portrait ? s_pw : s_ph;
    s_fb = (uint16_t *)calloc((size_t)g_w * g_h, 2);
    s_screen = (uint16_t **)malloc(sizeof *s_screen * g_h);
    if (g_portrait) s_rot = (uint16_t *)malloc((size_t)s_pw * s_ph * 2);
    if (!s_fb || !s_screen || (g_portrait && !s_rot)) return -1;
    for (int y = 0; y < g_h; y++) s_screen[y] = s_fb + (size_t)y * g_w;
    g_rows = s_screen;
    gfx_noclip();
    s_dy0 = 0;
    s_dy1 = g_h;
    return 0;
}

int gfx_init(void) {
    s_pw = nv_gfx_width();
    s_ph = nv_gfx_height();
    if (s_pw <= 0 || s_ph <= 0) return -1;
    if (alloc_screen() < 0) return -1;
    nv_gfx_persist(1);
    return 0;
}

// Portrait: the app draws a screen as tall as the canvas is wide and turns it a quarter clockwise
// on its way out, the way the system's own portrait mode does (nv_disp, rotation 90): the device
// held upright shows it upright. Every surface must be made again after a change.
int gfx_set_portrait(int on) {
    if (on == g_portrait) return 0;
    g_portrait = on;
    return alloc_screen();
}

// A touch on the canvas -> the app's screen.
void gfx_touch_map(int *x, int *y) {
    if (!g_portrait) return;
    const int px = *x, py = *y;
    *x = s_ph - 1 - py;
    *y = px;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x >= g_w) *x = g_w - 1;
    if (*y >= g_h) *y = g_h - 1;
}

int surf_init(surf_t *s) {
    s->px = (uint16_t *)malloc((size_t)g_w * g_h * 2);
    s->rows = (uint16_t **)malloc(sizeof *s->rows * g_h);
    if (!s->px || !s->rows) { surf_free(s); return -1; }
    for (int y = 0; y < g_h; y++) s->rows[y] = s->px + (size_t)y * g_w;
    return 0;
}

void surf_free(surf_t *s) {
    if (g_rows == s->rows) g_rows = s_screen;
    free(s->px);
    free(s->rows);
    s->px = NULL;
    s->rows = NULL;
}

void gfx_target(surf_t *s) {
    g_rows = s && s->rows ? s->rows : s_screen;
    gfx_noclip();
}

void gfx_dirty(int y0, int y1) {
    if (g_rows != s_screen) return;   // a surface reaches the screen by gfx_copy / gfx_compose
    if (y0 < 0) y0 = 0;
    if (y1 > g_h) y1 = g_h;
    if (y1 <= y0) return;
    if (y0 < s_dy0) s_dy0 = y0;
    if (y1 > s_dy1) s_dy1 = y1;
}

static void dirty_screen(int y0, int y1) {
    uint16_t **keep = g_rows;
    g_rows = s_screen;
    gfx_dirty(y0, y1);
    g_rows = keep;
}

void gfx_flush(void) {
    if (s_dy1 <= s_dy0) return;
    if (!g_portrait) {
        nv_gfx_blit_raw(s_screen[s_dy0], (int32_t)((size_t)(s_dy1 - s_dy0) * g_w * 2), 0, s_dy0, g_w, s_dy1 - s_dy0);
    } else {
        // rows [y0, y1) of the app's screen are columns [y0, y1) of the canvas: canvas (cx, cy) shows
        // the screen's (x = g_w - 1 - cy, y = cx). Written row by row of the canvas, in 32-row bands
        // of the screen so the source stays in the cache.
        const int y0 = s_dy0, n = s_dy1 - s_dy0;
        for (int b0 = 0; b0 < n; b0 += 32) {
            const int b1 = b0 + 32 < n ? b0 + 32 : n;
            for (int cy = 0; cy < s_ph; cy++) {
                uint16_t *d = s_rot + (size_t)cy * n;
                const int sx = g_w - 1 - cy;
                for (int c = b0; c < b1; c++) d[c] = s_screen[y0 + c][sx];
            }
        }
        nv_gfx_blit_raw(s_rot, (int32_t)((size_t)n * s_ph * 2), y0, 0, n, s_ph);
    }
    s_dy0 = INT_MAX;
    s_dy1 = -1;
}

void gfx_clip(int x0, int y0, int x1, int y1) {
    s_cx0 = x0 < 0 ? 0 : x0;
    s_cy0 = y0 < 0 ? 0 : y0;
    s_cx1 = x1 > g_w ? g_w : x1;
    s_cy1 = y1 > g_h ? g_h : y1;
}

void gfx_noclip(void) { gfx_clip(0, 0, g_w, g_h); }

static int clip_rect(int *x, int *y, int *w, int *h) {
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
    if (x0 < s_cx0) x0 = s_cx0;
    if (y0 < s_cy0) y0 = s_cy0;
    if (x1 > s_cx1) x1 = s_cx1;
    if (y1 > s_cy1) y1 = s_cy1;
    if (x1 <= x0 || y1 <= y0) return 0;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
}

// Coverage as the render mode shows it: 16 steps of grey, or on/off.
static inline unsigned cov(unsigned a) {
    if (g_rmode == RM_MONO) return a >= 120 ? 255 : 0;
    if (g_rmode == RM_GREY) return ((a * 15 + 127) / 255) * 17;
    return a;
}

void gfx_fill(int x, int y, int w, int h, uint16_t c) {
    if (!clip_rect(&x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint16_t *p = g_rows[y + j] + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
    gfx_dirty(y, y + h);
}

void gfx_blend_rect(int x, int y, int w, int h, uint16_t c, unsigned a) {
    if (!clip_rect(&x, &y, &w, &h)) return;
    if (g_rmode == RM_MONO) a = a >= 128 ? 255 : 0;
    for (int j = 0; j < h; j++) {
        uint16_t *p = g_rows[y + j] + x;
        for (int i = 0; i < w; i++) p[i] = blend565(p[i], c, a);
    }
    gfx_dirty(y, y + h);
}

static inline void put_a(int x, int y, uint16_t c, unsigned a) {
    if (x < s_cx0 || x >= s_cx1 || y < s_cy0 || y >= s_cy1) return;
    a = cov(a);
    if (!a) return;
    uint16_t *p = g_rows[y] + x;
    *p = a >= 255 ? c : blend565(*p, c, a);
}

// Coverage (0..255) of pixel (px, py) by a rounded rectangle; only called near the corners.
static unsigned rr_cov(float px, float py, float x0, float y0, float x1, float y1, float r) {
    if (r < 0.5f) r = 0.5f;   // a square corner: full coverage inside, none outside
    if (x1 - x0 < 2 * r || y1 - y0 < 2 * r) return 0;
    float cx = px < x0 + r ? x0 + r : px > x1 - r ? x1 - r : px;
    float cy = py < y0 + r ? y0 + r : py > y1 - r ? y1 - r : py;
    float dx = px - cx, dy = py - cy;
    float d = sqrtf(dx * dx + dy * dy);
    float c = r - d + 0.5f;
    if (c <= 0) return 0;
    if (c >= 1) return 255;
    return (unsigned)(c * 255.0f);
}

void gfx_rrect(int x, int y, int w, int h, int r, uint16_t c) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { gfx_fill(x, y, w, h, c); return; }
    gfx_fill(x, y + r, w, h - 2 * r, c);   // middle band
    gfx_fill(x + r, y, w - 2 * r, r, c);   // top and bottom between the corners
    gfx_fill(x + r, y + h - r, w - 2 * r, r, c);
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            const unsigned a = rr_cov(x + i + 0.5f, y + j + 0.5f, (float)x, (float)y, (float)(x + w), (float)(y + h), (float)r);
            put_a(x + i, y + j, c, a);
            put_a(x + w - 1 - i, y + j, c, a);
            put_a(x + i, y + h - 1 - j, c, a);
            put_a(x + w - 1 - i, y + h - 1 - j, c, a);
        }
    gfx_dirty(y, y + h);
}

void gfx_ring(int x, int y, int w, int h, int r, int t, uint16_t c) {
    if (w <= 0 || h <= 0 || t <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    const float X0 = (float)x, Y0 = (float)y, X1 = (float)(x + w), Y1 = (float)(y + h);
    const float ir = r > t ? (float)(r - t) : 0.0f;
    const int edge = (r > t ? r : t) + 1;
    for (int j = 0; j < h; j++) {
        const int full = j < edge || j >= h - edge;
        for (int i = 0; i < w; i++) {
            if (!full && i == edge && w - edge - 1 > i) { i = w - edge - 1; continue; }   // the inside
            const float px = x + i + 0.5f, py = y + j + 0.5f;
            const unsigned a = rr_cov(px, py, X0, Y0, X1, Y1, (float)r);
            if (!a) continue;
            const unsigned in = rr_cov(px, py, X0 + t, Y0 + t, X1 - t, Y1 - t, ir);
            put_a(x + i, y + j, c, a > in ? a - in : 0);
        }
    }
    gfx_dirty(y, y + h);
}

void gfx_row(int y, int x, const uint16_t *px, const uint8_t *a, int w) {
    if (y < s_cy0 || y >= s_cy1) return;
    const int i0 = x < s_cx0 ? s_cx0 - x : 0, i1 = x + w > s_cx1 ? s_cx1 - x : w;
    uint16_t *d = g_rows[y] + x;
    if (g_rmode == RM_MONO) {   // the pixels are black or white already (img.c): alpha on or off
        for (int i = i0; i < i1; i++)
            if (a[i] >= 128) d[i] = px[i];
        return;
    }
    for (int i = i0; i < i1; i++) {
        const unsigned al = a[i];
        if (al) d[i] = al >= 250 ? px[i] : blend565(d[i], px[i], al);
    }
}

void gfx_mask(const uint8_t *m, int mw, int mh, int x, int y, uint16_t c) {
    int x0 = x, y0 = y, w = mw, h = mh;
    if (!clip_rect(&x0, &y0, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        const uint8_t *src = m + (size_t)(y0 - y + j) * mw + (x0 - x);
        uint16_t *p = g_rows[y0 + j] + x0;
        for (int i = 0; i < w; i++) {
            const unsigned a = cov(src[i]);
            if (a) p[i] = a >= 250 ? c : blend565(p[i], c, a);
        }
    }
    gfx_dirty(y0, y0 + h);
}

void gfx_compose(const surf_t *left, int left_x, const surf_t *right, int right_x, int split, int y0, int y1) {
    if (split < 0) split = 0;
    if (split > g_w) split = g_w;
    if (y0 < 0) y0 = 0;
    if (y1 > g_h) y1 = g_h;
    for (int y = y0; y < y1; y++) {
        uint16_t *d = s_screen[y];
        if (split > 0) {
            int lx = left_x, n = split;
            if (lx < 0) { memset(d, 0, (size_t)-lx * 2); n += lx; d += -lx; lx = 0; }
            if (lx + n > g_w) n = g_w - lx;
            if (n > 0) memcpy(d, left->rows[y] + lx, (size_t)n * 2);
            d = s_screen[y];
        }
        if (split < g_w) {
            int rx = right_x, n = g_w - split;
            if (rx + n > g_w) n = g_w - rx;
            if (rx < 0) rx = 0;
            if (n > 0) memcpy(d + split, right->rows[y] + rx, (size_t)n * 2);
        }
    }
    dirty_screen(y0, y1);
}

void gfx_shadow(int x, int w, int dir, int y0, int y1, unsigned strength) {
    if (w <= 0) return;
    if (y0 < 0) y0 = 0;
    if (y1 > g_h) y1 = g_h;
    for (int i = 0; i < w; i++) {
        const int cx = x + i * dir;
        if (cx < 0 || cx >= g_w) continue;
        const unsigned a = strength * (unsigned)(w - i) * (unsigned)(w - i) / ((unsigned)w * w);   // eases out
        if (!a) continue;
        for (int y = y0; y < y1; y++) {
            uint16_t *p = &s_screen[y][cx];
            *p = blend565(*p, 0, a);
        }
    }
    dirty_screen(y0, y1);
}

void gfx_copy(const surf_t *s, int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > g_h) y1 = g_h;
    for (int y = y0; y < y1; y++) memcpy(s_screen[y], s->rows[y], (size_t)g_w * 2);
    dirty_screen(y0, y1);
}

void gfx_invert(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > g_h) y1 = g_h;
    for (int y = y0; y < y1; y++) {
        uint32_t *p = (uint32_t *)s_screen[y];
        for (int i = 0; i < g_w / 2; i++) p[i] = ~p[i];
    }
    dirty_screen(y0, y1);
}
