// gfx.c — the app's frame buffer: RGB565 at the canvas size, drawn by the app and sent with
// nv_gfx_blit one band of changed rows per frame (the host keeps the rest: nv_gfx_persist).
// Rows are reached through a table of row pointers: scrolling a band rotates the table instead of
// moving 1 MB of pixels (a memmove of the reader's band took 88 ms on the P4, the whole frame budget).
#include "bib.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "nucleo_sdk.h"

static uint16_t *s_fb;
uint16_t **g_rows;
static uint16_t **s_tmp;
int g_w, g_h;
static int s_dy0 = INT_MAX, s_dy1 = -1;   // dirty rows
static int s_cx0, s_cy0, s_cx1, s_cy1;    // clip

int gfx_init(void) {
    g_w = nv_gfx_width();
    g_h = nv_gfx_height();
    if (g_w <= 0 || g_h <= 0) return -1;
    s_fb = (uint16_t *)calloc((size_t)g_w * g_h, 2);
    g_rows = (uint16_t **)malloc(sizeof *g_rows * g_h);
    s_tmp = (uint16_t **)malloc(sizeof *s_tmp * g_h);
    if (!s_fb || !g_rows || !s_tmp) return -1;
    for (int y = 0; y < g_h; y++) g_rows[y] = s_fb + (size_t)y * g_w;
    gfx_noclip();
    nv_gfx_persist(1);
    return 0;
}

void gfx_dirty(int y0, int y1) {
    if (y0 < 0) y0 = 0;
    if (y1 > g_h) y1 = g_h;
    if (y1 <= y0) return;
    if (y0 < s_dy0) s_dy0 = y0;
    if (y1 > s_dy1) s_dy1 = y1;
}

void gfx_flush(void) {
    if (s_dy1 <= s_dy0) return;
    for (int y = s_dy0; y < s_dy1;) {   // one blit per run of rows that are contiguous in memory
        int e = y + 1;
        while (e < s_dy1 && g_rows[e] == g_rows[e - 1] + g_w) e++;
        nv_gfx_blit_raw(g_rows[y], (int32_t)((size_t)(e - y) * g_w * 2), 0, y, g_w, e - y);
        y = e;
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
    for (int j = 0; j < h; j++) {
        uint16_t *p = g_rows[y + j] + x;
        for (int i = 0; i < w; i++) p[i] = blend565(p[i], c, a);
    }
    gfx_dirty(y, y + h);
}

static inline void put_a(int x, int y, uint16_t c, unsigned a) {
    if (x < s_cx0 || x >= s_cx1 || y < s_cy0 || y >= s_cy1 || !a) return;
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

void gfx_mask(const uint8_t *m, int mw, int mh, int x, int y, uint16_t c) {
    int x0 = x, y0 = y, w = mw, h = mh;
    if (!clip_rect(&x0, &y0, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        const uint8_t *src = m + (size_t)(y0 - y + j) * mw + (x0 - x);
        uint16_t *p = g_rows[y0 + j] + x0;
        for (int i = 0; i < w; i++) {
            const unsigned a = src[i];
            if (a) p[i] = a >= 250 ? c : blend565(p[i], c, a);
        }
    }
    gfx_dirty(y0, y0 + h);
}

void gfx_image(const uint16_t *px, int iw, int ih, int x, int y) {
    int x0 = x, y0 = y, w = iw, h = ih;
    if (!clip_rect(&x0, &y0, &w, &h)) return;
    for (int j = 0; j < h; j++)
        memcpy(g_rows[y0 + j] + x0, px + (size_t)(y0 - y + j) * iw + (x0 - x), (size_t)w * 2);
    gfx_dirty(y0, y0 + h);
}

void gfx_scroll(int y0, int y1, int dy) {
    if (y0 < 0) y0 = 0;
    if (y1 > g_h) y1 = g_h;
    const int n = y1 - y0;
    if (n <= 0 || !dy) return;
    const int k = ((dy % n) + n) % n;   // row i of the band shows what row i - dy showed
    for (int i = 0; i < n; i++) s_tmp[(i + k) % n] = g_rows[y0 + i];
    memcpy(&g_rows[y0], s_tmp, sizeof *s_tmp * (size_t)n);
    gfx_dirty(y0, y1);
}
