/* font.h - 5x7 text on the quarter-turned canvas (nv_gfx_text would come out sideways), in landscape
 * screen pixels through hud.h. Characters: space 0-9 A-Z . : / - ! % + ? (lowercase is upper-cased).
 * Header-only, shared by the apps. */
#ifndef VS_FONT_H
#define VS_FONT_H

#include "hud.h"

/* one row per byte, 5 bits wide (bit 4 = left), 7 rows */
static const unsigned char FONT5x7[][7] = {
    {0, 0, 0, 0, 0, 0, 0},                                  /* space */
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},  /* 0 1 */
    {14, 17, 1, 2, 4, 8, 31}, {31, 2, 4, 2, 1, 17, 14},     /* 2 3 */
    {2, 6, 10, 18, 31, 2, 2}, {31, 16, 30, 1, 1, 17, 14},   /* 4 5 */
    {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},     /* 6 7 */
    {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12}, /* 8 9 */
    {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14}, /* A B C */
    {28, 18, 17, 17, 17, 18, 28}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16}, /* D E F */
    {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},      /* G H I */
    {7, 2, 2, 2, 2, 18, 12}, {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},      /* J K L */
    {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17}, {14, 17, 17, 17, 17, 17, 14}, /* M N O */
    {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17}, /* P Q R */
    {15, 16, 16, 14, 1, 1, 30}, {31, 4, 4, 4, 4, 4, 4}, {17, 17, 17, 17, 17, 17, 14},         /* S T U */
    {17, 17, 17, 17, 17, 10, 4}, {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},   /* V W X */
    {17, 17, 10, 4, 4, 4, 4}, {31, 1, 2, 4, 8, 16, 31},                                      /* Y Z */
    {0, 0, 0, 0, 0, 12, 12}, {0, 12, 12, 0, 12, 12, 0}, {1, 2, 2, 4, 8, 8, 16},              /* . : / */
    {0, 0, 0, 31, 0, 0, 0}, {4, 4, 4, 4, 4, 0, 4}, {25, 25, 2, 4, 8, 19, 19},                 /* - ! % */
    {0, 4, 4, 31, 4, 4, 0}, {14, 17, 1, 2, 4, 0, 4},                                         /* + ? */
};

static inline int font_index(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c >= '0' && c <= '9') return 1 + c - '0';
    if (c >= 'A' && c <= 'Z') return 11 + c - 'A';
    switch (c) {
    case '.': return 37; case ':': return 38; case '/': return 39; case '-': return 40;
    case '!': return 41; case '%': return 42; case '+': return 43; case '?': return 44;
    default: return 0;
    }
}

/* width in px of s at scale k (6 px per character, last column dropped) */
static inline int text_w(const char *s, int k)
{
    int n = 0;
    while (s[n]) n++;
    return n ? (n * 6 - 1) * k : 0;
}

/* text at screen (x, y), k px per dot; runs of lit dots in a row become one rect */
static inline void text_s(int x, int y, const char *s, int k, int col)
{
    for (; *s; s++, x += 6 * k) {
        const unsigned char *g = FONT5x7[font_index(*s)];
        int r;
        for (r = 0; r < 7; r++) {
            int b = 0;
            while (b < 5) {
                int e;
                if (!(g[r] & (16 >> b))) { b++; continue; }
                for (e = b; e < 5 && (g[r] & (16 >> e)); e++) {}
                rect_s(x + b * k, y + r * k, (e - b) * k, k, col);
                b = e;
            }
        }
    }
}

/* text with a 1-dot dark shadow, readable over any terrain */
static inline void text_sh(int x, int y, const char *s, int k, int col)
{
    text_s(x + k / 2 + 1, y + k / 2 + 1, s, k, 0x0000);
    text_s(x, y, s, k, col);
}

/* translucent rounded plate behind HUD text (nv_gfx_panel is drawn on the turned canvas) */
static inline void plate_s(int x, int y, int w, int h, int col, int alpha)
{
    nv_gfx_panel(g_h - y - h, x, h, w, 6, col, col, alpha);
}

/* small integer / decimal formatting without libc */
static inline char *fmt_int(char *p, int v)
{
    char t[12];
    int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}
static inline char *fmt_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    *p = 0;
    return p;
}
static inline char *fmt_time(char *p, int ms)          /* "12.3" */
{
    if (ms < 0) ms = 0;
    p = fmt_int(p, ms / 1000);
    *p++ = '.';
    *p++ = (char)('0' + (ms / 100) % 10);
    *p = 0;
    return p;
}

#endif
