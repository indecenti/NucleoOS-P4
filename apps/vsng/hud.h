/* hud.h - drawing over the engine's frame on the quarter-turned canvas, in landscape screen pixels:
 * screen (X, Y) = upright (g_h - 1 - Y, X). Header-only, shared by the apps. Set g_w/g_h first. */
#ifndef VS_HUD_H
#define VS_HUD_H

#include "nucleo_sdk.h"

#define C565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

static int g_w, g_h;                     /* landscape screen = the upright canvas turned back */

/* ---- sine, 4096 steps a turn, Q16 ---- */
static int g_sinq[1025];
static inline void sin_init(void)
{
    long long c = 1073741824LL, s = 0, kc = 1073740560LL, ks = 1647099LL;   /* rotation by 2pi/4096, Q30 */
    int i;
    for (i = 0; i <= 1024; i++) {
        long long nc = (c * kc - s * ks) >> 30, ns = (s * kc + c * ks) >> 30;
        g_sinq[i] = (int)(s >> 14);
        c = nc; s = ns;
    }
    g_sinq[1024] = 65536;
}
static inline int isin(int a)
{
    a &= 4095;
    if (a <= 1024) return g_sinq[a];
    if (a <= 2048) return g_sinq[2048 - a];
    if (a <= 3072) return -g_sinq[a - 2048];
    return -g_sinq[4096 - a];
}
static inline int icos(int a) { return isin(a + 1024); }

/* angle of (dx, dy), 0..4095 (0 = +x, 1024 = +y) */
static inline int iatan2(int dy, int dx)
{
    int best = 0, bd = 0x7FFFFFFF, a;
    for (a = 0; a < 4096; a += 16) {
        long long cr = (long long)icos(a) * dy - (long long)isin(a) * dx;
        long long dot = (long long)icos(a) * dx + (long long)isin(a) * dy;
        int d = (int)((cr < 0 ? -cr : cr) >> 16);
        if (dot > 0 && d < bd) { bd = d; best = a; }
    }
    return best;
}

/* ---- primitives ---- */
static inline void rect_s(int x, int y, int w, int h, int c) { nv_gfx_rect(g_h - y - h, x, h, w, c); }
static inline void line_s(int x0, int y0, int x1, int y1, int c) { nv_gfx_line(g_h - 1 - y0, x0, g_h - 1 - y1, x1, c); }
static inline void tri_s(int x0, int y0, int x1, int y1, int x2, int y2, int c)
{
    nv_gfx_tri(g_h - 1 - y0, x0, g_h - 1 - y1, x1, g_h - 1 - y2, x2, c);
}
static inline void ring_s(int cx, int cy, int r, int segs, int c)
{
    int i, x0 = cx + r, y0 = cy;
    for (i = 1; i <= segs; i++) {
        int a = i * 4096 / segs, x = cx + ((icos(a) * r) >> 16), y = cy + ((isin(a) * r) >> 16);
        line_s(x0, y0, x, y, c);
        x0 = x; y0 = y;
    }
}

/* 3x5 digits; glyph 10 = '.', 11 = ':' */
static const unsigned short HUD_DIG[12] = { 0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249,
                                            0x7BEF, 0x7BCF, 0x0002, 0x0410 };
static inline int hud_glyphs(int x, int y, const char *g, int n, int s, int col)
{
    int i, r, k;
    rect_s(x - s, y - s, (n * 4 + 1) * s, 7 * s, C565(10, 16, 28));
    for (i = 0; i < n; i++) {
        unsigned short m = HUD_DIG[(int)g[i]];
        for (r = 0; r < 5; r++)
            for (k = 0; k < 3; k++)
                if ((m >> (14 - (r * 3 + k))) & 1) rect_s(x + (i * 4 + k) * s, y + r * s, s, s, col);
    }
    return (n * 4 + 1) * s;
}
static inline int hud_int(int x, int y, int v, int s, int col)
{
    char g[10], t[10];
    int n = 0, i;
    if (v < 0) v = 0;
    do { t[n++] = (char)(v % 10); v /= 10; } while (v);
    for (i = 0; i < n; i++) g[i] = t[n - 1 - i];
    return hud_glyphs(x, y, g, n, s, col);
}
static inline int hud_time(int x, int y, int ms, int s, int col)       /* seconds with one decimal */
{
    char g[12], t[8];
    int n = 0, i, sec = ms / 1000, k = 0;
    if (ms < 0) ms = 0, sec = 0;
    do { t[n++] = (char)(sec % 10); sec /= 10; } while (sec);
    for (i = 0; i < n; i++) g[k++] = t[n - 1 - i];
    g[k++] = 10;
    g[k++] = (char)((ms / 100) % 10);
    return hud_glyphs(x, y, g, k, s, col);
}

/* the touch stick under the left thumb */
static inline void hud_stick(int on, int x0, int y0, int x, int y)
{
    int r = g_h / 6;
    if (!on) return;
    ring_s(x0, y0, r, 32, C565(255, 255, 255));
    ring_s(x0, y0, r - 1, 32, C565(40, 40, 40));
    rect_s(x - 10, y - 10, 20, 20, C565(255, 255, 255));
}

/* pause: the frame dimmed, a big "II"; Enter / A / tap resumes, Esc / B again quits */
static inline void hud_pause(void)
{
    nv_gfx_panel(0, 0, g_h, g_w, 0, 0, 0, 150);
    rect_s(g_w / 2 - 50, g_h / 2 - 60, 34, 120, 0xFFFF);
    rect_s(g_w / 2 + 16, g_h / 2 - 60, 34, 120, 0xFFFF);
}

#endif
