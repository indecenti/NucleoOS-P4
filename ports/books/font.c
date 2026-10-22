// font.c — text with the Noto fonts (SIL OFL 1.1): Sans and Mono of the Biblioteca app
// (ports/zim/biblioteca/fonts) and Serif (ports/books/fonts), stored deflated in the app (gen_fontdata.py)
// and inflated the first time a face is used, rasterised at run time by stb_truetype at any pixel
// size and kept in a glyph cache (a hash table over one pool of 8-bit coverage). Pen positions
// advance in 1/16 px and each glyph lands on the nearest pixel. A code point a face lacks falls back
// to Sans, then Mono, then U+FFFD. Soft hyphens and zero-width characters take no room.
// From the Biblioteca app's font.c, with the serif faces and the deflated data.
#include "bk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

int bk_inflate(const void *src, size_t n, void *out, size_t out_n, int zlib);   // inflate.c

typedef struct { const unsigned char *z; unsigned zn, n; } fontdata_t;
extern const fontdata_t bk_fonts[F_COUNT];   // fontdata.c (gen_fontdata.py), in F_* order

static stbtt_fontinfo s_font[F_COUNT];
static unsigned char *s_data[F_COUNT];
static int s_state[F_COUNT];   // 0 not loaded, 1 ready, -1 failed
static int s_asc[F_COUNT], s_desc[F_COUNT], s_upem[F_COUNT], s_xh[F_COUNT];

typedef struct {
    uint32_t key;            // face (4 bits) | px (8 bits) | code point (20 bits); 0xFFFFFFFF = empty
    int16_t x0, y0;          // bitmap offset from the pen on the baseline
    uint16_t w, h;
    int32_t adv16;           // advance, 1/16 px
    uint32_t off;            // coverage in the pool
} glyph_t;

#define GC_SLOTS 4096u                    // power of two
#define GC_POOL (640u * 1024u)
static glyph_t *s_gc;
static uint8_t *s_pool;
static size_t s_pool_n;
static int s_glyphs;
static uint8_t s_gamma[256];

static int load(int f) {
    if (s_state[f]) return s_state[f];
    s_state[f] = -1;
    const fontdata_t *fd = &bk_fonts[f];
    unsigned char *d = (unsigned char *)malloc(fd->n);
    if (!d) return -1;
    if (bk_inflate(fd->z, fd->zn, d, fd->n, 1) != (int)fd->n ||
        !stbtt_InitFont(&s_font[f], d, stbtt_GetFontOffsetForIndex(d, 0))) {
        free(d);
        return -1;
    }
    s_data[f] = d;
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&s_font[f], &asc, &desc, &gap);
    s_asc[f] = asc;
    s_desc[f] = -desc;
    s_upem[f] = ttUSHORT(s_font[f].data + s_font[f].head + 18);
    int x0, y0, x1, y1;
    s_xh[f] = stbtt_GetCodepointBox(&s_font[f], 'x', &x0, &y0, &x1, &y1) ? y1 : s_upem[f] / 2;
    s_state[f] = 1;
    return 1;
}

static int face(int f) {   // the face to use for f: f itself when it loads, else Sans
    if (f < 0 || f >= F_COUNT) f = F_SANS;
    return load(f) > 0 ? f : F_SANS;
}

int font_init(void) {
    if (load(F_SANS) < 0 || load(F_BOLD) < 0) return -1;
    s_gc = (glyph_t *)malloc(sizeof(glyph_t) * GC_SLOTS);
    s_pool = (uint8_t *)malloc(GC_POOL);
    if (!s_gc || !s_pool) return -1;
    memset(s_gc, 0xFF, sizeof(glyph_t) * GC_SLOTS);
    // Coverage through a mild gamma: stb_truetype's linear coverage looks thin on an sRGB panel.
    for (int i = 0; i < 256; i++) s_gamma[i] = (uint8_t)(powf(i / 255.0f, 0.78f) * 255.0f + 0.5f);
    return 0;
}

static float scale_of(int f, int px) { return (float)px / (float)s_upem[f]; }
int font_ascent(int f, int px) { f = face(f); return (int)ceilf(s_asc[f] * scale_of(f, px)); }
int font_descent(int f, int px) { f = face(f); return (int)ceilf(s_desc[f] * scale_of(f, px)); }
int font_xheight(int f, int px) { f = face(f); return (int)(s_xh[f] * scale_of(f, px) + 0.5f); }

void font_cache_clear(void) {
    memset(s_gc, 0xFF, sizeof(glyph_t) * GC_SLOTS);
    s_pool_n = 0;
    s_glyphs = 0;
}

// Characters that take no room: soft hyphen (a place to break, drawn by the layout when it breaks
// there), zero-width space / joiners, byte-order mark.
static inline int invisible(uint32_t cp) {
    return cp < 0x20 || cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) || cp == 0x2060 || cp == 0xFEFF;
}

static const glyph_t *glyph(int f, int px, uint32_t cp) {
    f = face(f);
    if (px < 4) px = 4;
    if (px > 255) px = 255;
    if (cp > 0xFFFFF) cp = 0xFFFD;
    const uint32_t key = (uint32_t)f << 28 | (uint32_t)px << 20 | cp;
    uint32_t h = (key * 2654435761u) >> 20 & (GC_SLOTS - 1);
    for (;;) {
        glyph_t *g = &s_gc[h];
        if (g->key == key) return g;
        if (g->key == 0xFFFFFFFFu) break;
        h = (h + 1) & (GC_SLOTS - 1);
    }
    // not cached: find the glyph (with the fallbacks), measure and rasterise it
    int ff = f, gi = stbtt_FindGlyphIndex(&s_font[f], (int)cp);
    if (!gi && f != F_SANS) { ff = F_SANS; gi = stbtt_FindGlyphIndex(&s_font[ff], (int)cp); }
    if (!gi && f != F_MONO && load(F_MONO) > 0) { ff = F_MONO; gi = stbtt_FindGlyphIndex(&s_font[ff], (int)cp); }
    if (!gi) { ff = f; gi = stbtt_FindGlyphIndex(&s_font[ff], 0xFFFD); }
    const float sc = scale_of(ff, px);
    int adv, lsb, x0, y0, x1, y1;
    stbtt_GetGlyphHMetrics(&s_font[ff], gi, &adv, &lsb);
    stbtt_GetGlyphBitmapBox(&s_font[ff], gi, sc, sc, &x0, &y0, &x1, &y1);
    int w = x1 - x0, hh = y1 - y0;
    if (w < 0 || hh < 0 || w > 512 || hh > 512) w = hh = 0;
    if (s_glyphs >= (int)(GC_SLOTS * 3 / 4) || s_pool_n + (size_t)w * hh > GC_POOL) {
        font_cache_clear();
        h = (key * 2654435761u) >> 20 & (GC_SLOTS - 1);
    }
    glyph_t *g = &s_gc[h];
    g->key = key;
    g->x0 = (int16_t)x0;
    g->y0 = (int16_t)y0;
    g->w = (uint16_t)w;
    g->h = (uint16_t)hh;
    g->adv16 = (int32_t)lroundf(adv * sc * 16.0f);
    g->off = (uint32_t)s_pool_n;
    if (w && hh) {
        uint8_t *bm = s_pool + s_pool_n;
        stbtt_MakeGlyphBitmap(&s_font[ff], bm, w, hh, w, sc, sc, gi);
        for (int i = 0; i < w * hh; i++) bm[i] = s_gamma[bm[i]];
        s_pool_n += (size_t)w * hh;
    }
    s_glyphs++;
    return g;
}

uint32_t utf8_next(const char **p, const char *end) {
    const unsigned char *s = (const unsigned char *)*p;
    if ((const char *)s >= end) return 0;
    uint32_t c = s[0];
    int n = c < 0x80 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (n < 0) { *p += 1; return 0xFFFD; }
    if (n == 0) { *p += 1; return c; }
    if (end - (const char *)s <= n) { *p = end; return 0xFFFD; }   // cut short
    c &= 0x3F >> n;
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p += i; return 0xFFFD; }
        c = c << 6 | (s[i] & 0x3F);
    }
    *p += n + 1;
    return c;
}

int utf8_put(uint32_t cp, char *o) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 63)); o[2] = (char)(0x80 | (cp & 63));
        return 3;
    }
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 63));
    o[2] = (char)(0x80 | (cp >> 6 & 63)); o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

int32_t text_width16(int f, int px, const char *s, int n) {
    const char *p = s, *e = s + n;
    int32_t pen = 0;
    while (p < e) {
        const uint32_t cp = utf8_next(&p, e);
        if (invisible(cp)) continue;   // markers of the rich text, soft hyphens
        pen += glyph(f, px, cp)->adv16;
    }
    return pen;
}

int text_width(int f, int px, const char *s, int n) { return (text_width16(f, px, s, n) + 8) >> 4; }

int text_draw(int f, int px, int x, int y_base, const char *s, int n, uint16_t c) {
    const char *p = s, *e = s + n;
    int32_t pen = (int32_t)x << 4;
    while (p < e) {
        const uint32_t cp = utf8_next(&p, e);
        if (invisible(cp)) continue;
        const glyph_t *g = glyph(f, px, cp);
        if (g->w && g->h) gfx_mask(s_pool + g->off, g->w, g->h, ((pen + 8) >> 4) + g->x0, y_base + g->y0, c);
        pen += g->adv16;
    }
    return ((pen + 8) >> 4) - x;
}

int text_fit(int f, int px, const char *s, int n, int maxw) {
    const char *p = s, *e = s + n;
    int32_t pen = 0;
    const int32_t lim = (int32_t)maxw << 4;
    while (p < e) {
        const char *q = p;
        const uint32_t cp = utf8_next(&p, e);
        if (invisible(cp)) continue;
        pen += glyph(f, px, cp)->adv16;
        if (pen > lim) return (int)(q - s);
    }
    return n;
}

int text_draw_fit(int f, int px, int x, int y_base, const char *s, int n, int maxw, uint16_t c) {
    if (text_width(f, px, s, n) <= maxw) return text_draw(f, px, x, y_base, s, n, c);
    static const char kEll[] = "\xE2\x80\xA6";
    const int ew = text_width(f, px, kEll, 3);
    int k = text_fit(f, px, s, n, maxw - ew);
    while (k > 0 && s[k - 1] == ' ') k--;
    const int w = text_draw(f, px, x, y_base, s, k, c);
    return w + text_draw(f, px, x + w, y_base, kEll, 3, c);
}

void font_cache_stats(int *glyphs, size_t *bytes) {
    if (glyphs) *glyphs = s_glyphs;
    if (bytes) *bytes = s_pool_n;
}
