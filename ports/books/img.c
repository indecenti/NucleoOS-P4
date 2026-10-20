// img.c — the pictures of books (JPEG, PNG, GIF, BMP, WebP, SVG): decoded at the size the page
// shows them, kept in a cache of decoded pixels (RGB565 + alpha, IMG_CACHE_BYTES), drawn into the
// frame buffer. Decoding never happens while drawing: a picture not decoded yet is a placeholder and
// goes into a queue the main loop works through, one picture at a time. In the e-ink render mode a
// picture becomes 16 greys (Floyd-Steinberg), in the 1-bit mode black and white (Atkinson), so
// photographs and illustrations keep their tones on paper-like themes. Formulas (and any picture
// marked "mask") keep only their coverage and take the text colour.
// From the Biblioteca app's img.c (ports/zim/biblioteca), keyed by book path, with the render modes.
#include "bk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lodepng.h"
#include "webp/decode.h"

#define STBI_NO_STDIO
#define STBI_NO_PNG   // LodePNG
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 8192
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast_nv.h"   // ports/zim/biblioteca/nanosvgrast.patch applied (fetch.sh)

#include "tjpgd.h"

#define IMG_SRC_MAX (1300u * 1000u)   // largest picture decoded whole: 5 MB as RGBA, inside 12 MiB
#define JPEG_SRC_MAX (8192u * 8192u)  // baseline JPEG: decoded at 1/2..1/8 by TJpgDec, any size within reason
#define IMG_MAX 96
#define QUEUE_MAX 32
#define KEY_MAX 320

typedef struct {
    char key[KEY_MAX];
    int w, h, mask, mode;
    uint16_t *px;                     // NULL for a mask
    uint8_t *a;                       // coverage
    int state;                        // 1 ready, -1 failed
    uint32_t used;
    size_t bytes;
} img_t;

static img_t s_img[IMG_MAX];
static int s_nimg;
static size_t s_bytes;
static uint32_t s_clock;

typedef struct { char key[KEY_MAX]; int w, h, mask; } job_t;
static job_t s_queue[QUEUE_MAX];
static int s_nqueue;
static char s_last[96];

// -------------------------------------------------------------------------------- decoding

static int is_svg(const uint8_t *d, size_t n) {
    const size_t lim = n < 1024 ? n : 1024;
    for (size_t i = 0; i + 4 <= lim; i++)
        if (d[i] == '<' && (d[i + 1] == 's' || d[i + 1] == 'S') && (d[i + 2] == 'v' || d[i + 2] == 'V') &&
            (d[i + 3] == 'g' || d[i + 3] == 'G'))
            return 1;
    return 0;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int img_size(const uint8_t *d, size_t n, int *w, int *h) {
    *w = *h = 0;
    if (!d || n < 16) return -1;
    if (!memcmp(d, "\x89PNG\r\n\x1a\n", 8) && n >= 24) { *w = (int)be32(d + 16); *h = (int)be32(d + 20); }
    else if (n >= 30 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) { WebPGetInfo(d, n, w, h); }
    else if (is_svg(d, n)) {
        char *t = (char *)malloc(n + 1);
        if (!t) return -1;
        memcpy(t, d, n);
        t[n] = 0;
        NSVGimage *s = nsvgParse(t, "px", 96.0f);
        free(t);
        if (s) { *w = (int)s->width; *h = (int)s->height; nsvgDelete(s); }
    } else {
        int comp;
        if (n <= 0x7FFFFFFF && !stbi_info_from_memory(d, (int)n, w, h, &comp)) *w = *h = 0;
    }
    if (*w <= 0 || *h <= 0 || *w > 30000 || *h > 30000) { *w = *h = 0; return -1; }
    return 0;
}

// RGBA sw x sh -> RGBA w x h: box average when shrinking, bilinear when growing; colour weighted by
// coverage so transparent edges do not darken.
static uint8_t *resample(const uint8_t *src, int sw, int sh, int w, int h) {
    uint8_t *out = (uint8_t *)malloc((size_t)w * h * 4);
    if (!out) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *o = out + ((size_t)y * w + x) * 4;
            if (sw <= w && sh <= h) {
                const float fx = sw > 1 ? (x + 0.5f) * sw / w - 0.5f : 0, fy = sh > 1 ? (y + 0.5f) * sh / h - 0.5f : 0;
                int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
                const float ax = fx - x0, ay = fy - y0;
                int x1 = x0 + 1, y1 = y0 + 1;
                if (x0 < 0) x0 = 0;
                if (y0 < 0) y0 = 0;
                if (x1 >= sw) x1 = sw - 1;
                if (y1 >= sh) y1 = sh - 1;
                const uint8_t *p00 = src + ((size_t)y0 * sw + x0) * 4, *p10 = src + ((size_t)y0 * sw + x1) * 4;
                const uint8_t *p01 = src + ((size_t)y1 * sw + x0) * 4, *p11 = src + ((size_t)y1 * sw + x1) * 4;
                for (int c = 0; c < 4; c++) {
                    const float v = (p00[c] * (1 - ax) + p10[c] * ax) * (1 - ay) + (p01[c] * (1 - ax) + p11[c] * ax) * ay;
                    o[c] = (uint8_t)(v + 0.5f);
                }
                continue;
            }
            const int xa = (int)((long)x * sw / w), xb = (int)((long)(x + 1) * sw / w);
            const int ya = (int)((long)y * sh / h), yb = (int)((long)(y + 1) * sh / h);
            uint32_t r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int yy = ya; yy < (yb > ya ? yb : ya + 1) && yy < sh; yy++)
                for (int xx = xa; xx < (xb > xa ? xb : xa + 1) && xx < sw; xx++, cnt++) {
                    const uint8_t *p = src + ((size_t)yy * sw + xx) * 4;
                    r += p[0] * p[3];
                    g += p[1] * p[3];
                    b += p[2] * p[3];
                    a += p[3];
                }
            o[0] = (uint8_t)(a ? r / a : 0);
            o[1] = (uint8_t)(a ? g / a : 0);
            o[2] = (uint8_t)(a ? b / a : 0);
            o[3] = (uint8_t)(cnt ? a / cnt : 0);
        }
    return out;
}

// ---- baseline JPEG with TJpgDec: decoded at the smallest of 1, 1/2, 1/4, 1/8 that is still at
// least w x h, and sampled straight into the w x h result as the blocks come (2x2 averaged when the
// decoded size is near twice the target): no picture-sized buffer between the two.
typedef struct {
    const uint8_t *d;
    size_t n, at;
    uint8_t *out;            // w x h RGBA
    int w, h;                // the result
    int sw, sh;              // the decoded size
} tj_t;

static size_t tj_in(JDEC *jd, uint8_t *buf, size_t len) {
    tj_t *t = (tj_t *)jd->device;
    if (t->at + len > t->n) len = t->n - t->at;
    if (buf) memcpy(buf, t->d + t->at, len);
    t->at += len;
    return len;
}

static int tj_out(JDEC *jd, void *bitmap, JRECT *r) {
    tj_t *t = (tj_t *)jd->device;
    const uint8_t *s = (const uint8_t *)bitmap;
    const int bw = r->right - r->left + 1, bh = r->bottom - r->top + 1;
    // the target rows and columns whose source centre falls inside this block
    const int ty0 = (int)(((long)r->top * t->h + t->sh - 1) / t->sh), ty1 = (int)(((long)(r->bottom + 1) * t->h + t->sh - 1) / t->sh);
    const int tx0 = (int)(((long)r->left * t->w + t->sw - 1) / t->sw), tx1 = (int)(((long)(r->right + 1) * t->w + t->sw - 1) / t->sw);
    const int avg = t->sw >= t->w * 3 / 2;   // near 2:1: average 2x2 (stays inside the block)
    for (int ty = ty0; ty < ty1 && ty < t->h; ty++) {
        int sy = (int)((long)ty * t->sh / t->h) - r->top;
        if (sy < 0) sy = 0;
        if (sy >= bh) sy = bh - 1;
        const int sy2 = sy + 1 < bh ? sy + 1 : sy;
        uint8_t *o = t->out + ((size_t)ty * t->w + tx0) * 4;
        for (int tx = tx0; tx < tx1 && tx < t->w; tx++, o += 4) {
            int sx = (int)((long)tx * t->sw / t->w) - r->left;
            if (sx < 0) sx = 0;
            if (sx >= bw) sx = bw - 1;
            const uint8_t *p = s + ((size_t)sy * bw + sx) * 3;
            if (avg) {
                const int sx2 = sx + 1 < bw ? sx + 1 : sx;
                const uint8_t *p2 = s + ((size_t)sy * bw + sx2) * 3, *p3 = s + ((size_t)sy2 * bw + sx) * 3,
                              *p4 = s + ((size_t)sy2 * bw + sx2) * 3;
                for (int c = 0; c < 3; c++) o[c] = (uint8_t)((p[c] + p2[c] + p3[c] + p4[c] + 2) >> 2);
            } else {
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
            }
            o[3] = 255;
        }
    }
    return 1;
}

static uint8_t *jpeg_scaled(const uint8_t *d, size_t n, int w, int h) {
    void *pool = malloc(16384);
    if (!pool) return NULL;
    JDEC jd;
    tj_t t = {d, n, 0, NULL, w, h, 0, 0};
    uint8_t *out = NULL;
    if (jd_prepare(&jd, tj_in, pool, 16384, &t) == JDR_OK && (size_t)jd.width * jd.height <= JPEG_SRC_MAX) {
        int s = 0;
        while (s < 3 && (jd.width >> (s + 1)) >= w && (jd.height >> (s + 1)) >= h) s++;
        t.sw = (jd.width + (1 << s) - 1) >> s;
        t.sh = (jd.height + (1 << s) - 1) >> s;
        t.out = (uint8_t *)calloc((size_t)w * h, 4);
        if (t.out && jd_decomp(&jd, tj_out, (uint8_t)s) == JDR_OK) out = t.out;
        else free(t.out);
    }
    free(pool);
    return out;
}

uint8_t *img_decode_rgba(const uint8_t *d, size_t n, int w, int h) {
    if (!d || n < 8 || w <= 0 || h <= 0 || w > 4096 || h > 4096) return NULL;
    if (d[0] == 0xFF && d[1] == 0xD8) {   // JPEG: TJpgDec (baseline); progressive ones go to stb_image below
        uint8_t *out = jpeg_scaled(d, n, w, h);
        if (out) return out;
    }
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) {   // WebP: decoded at the target size
        int sw = 0, sh = 0;
        if (!WebPGetInfo(d, n, &sw, &sh) || sw <= 0 || sh <= 0 || (size_t)sw * sh > IMG_SRC_MAX * 4u) return NULL;
        WebPDecoderConfig cfg;
        if (!WebPInitDecoderConfig(&cfg)) return NULL;
        cfg.options.use_scaling = 1;
        cfg.options.scaled_width = w;
        cfg.options.scaled_height = h;
        cfg.output.colorspace = MODE_RGBA;
        if (WebPDecode(d, n, &cfg) != VP8_STATUS_OK) { WebPFreeDecBuffer(&cfg.output); return NULL; }
        uint8_t *out = (uint8_t *)malloc((size_t)w * h * 4);
        if (out)
            for (int y = 0; y < h; y++)
                memcpy(out + (size_t)y * w * 4, cfg.output.u.RGBA.rgba + (size_t)y * cfg.output.u.RGBA.stride, (size_t)w * 4);
        WebPFreeDecBuffer(&cfg.output);
        return out;
    }
    if (is_svg(d, n)) {   // SVG: rasterised at the target size
        char *text = (char *)malloc(n + 1);
        if (!text) return NULL;
        memcpy(text, d, n);
        text[n] = 0;
        for (size_t i = 0; i < n; i++) if (!text[i]) text[i] = ' ';
        NSVGimage *svg = nsvgParse(text, "px", 96.0f);
        free(text);
        if (!svg) return NULL;
        uint8_t *out = NULL;
        if (svg->width > 0 && svg->height > 0) {
            NSVGrasterizer *r = nsvgCreateRasterizer();
            out = (uint8_t *)calloc((size_t)w * h, 4);
            const float sx = w / svg->width, sy = h / svg->height;
            const float sc = sx < sy ? sx : sy;
            const float ox = (w - svg->width * sc) / 2, oy = (h - svg->height * sc) / 2;
            if (r && out && isfinite(sc) && isfinite(ox) && isfinite(oy)) {
                const float lim = 1e5f;   // shapes far outside any sane range would overflow the rasteriser
                for (NSVGshape *sh = svg->shapes; sh; sh = sh->next) {
                    int ok = 1;
                    for (int k = 0; k < 4; k++) ok &= isfinite(sh->bounds[k]) && fabsf(sh->bounds[k] * sc) < lim;
                    ok &= isfinite(sh->strokeWidth) && fabsf(sh->strokeWidth * sc) < lim;
                    if (!ok) sh->flags &= (unsigned char)~NSVG_FLAGS_VISIBLE;
                }
                nsvgRasterize(r, svg, ox, oy, sc, out, w, h, w * 4);
            }
            nsvgDeleteRasterizer(r);
        }
        nsvgDelete(svg);
        return out;
    }
    uint8_t *rgba = NULL;
    unsigned sw = 0, sh = 0;
    if (!memcmp(d, "\x89PNG\r\n\x1a\n", 8)) {
        LodePNGState st;
        lodepng_state_init(&st);
        if (lodepng_inspect(&sw, &sh, &st, d, n) || !sw || !sh || (size_t)sw * sh > IMG_SRC_MAX) {
            lodepng_state_cleanup(&st);
            return NULL;
        }
        lodepng_state_cleanup(&st);
        if (lodepng_decode32(&rgba, &sw, &sh, d, n)) { free(rgba); return NULL; }
    } else {
        int x = 0, y = 0, comp = 0;
        if (n > 0x7FFFFFFF || !stbi_info_from_memory(d, (int)n, &x, &y, &comp) || x <= 0 || y <= 0 ||
            (size_t)x * y > IMG_SRC_MAX)
            return NULL;
        rgba = stbi_load_from_memory(d, (int)n, &x, &y, &comp, 4);
        if (!rgba) return NULL;
        sw = (unsigned)x;
        sh = (unsigned)y;
    }
    uint8_t *out = resample(rgba, (int)sw, (int)sh, w, h);
    free(rgba);
    return out;
}

// RGBA -> the render mode's tones, in place: grey levels by error diffusion (Floyd-Steinberg for
// 16 greys, Atkinson for 1 bit, whose lighter diffusion keeps line art crisp).
static void tone(uint8_t *rgba, int w, int h, int mode) {
    if (mode == RM_COLOR) return;
    int16_t *e = (int16_t *)calloc((size_t)(w + 4) * 3, sizeof(int16_t));   // three rows of error
    if (!e) return;
    for (int y = 0; y < h; y++) {
        int16_t *r0 = e + (size_t)(y % 3) * (w + 4) + 2, *r1 = e + (size_t)((y + 1) % 3) * (w + 4) + 2,
                *r2 = e + (size_t)((y + 2) % 3) * (w + 4) + 2;
        for (int x = 0; x < w; x++) {
            uint8_t *p = rgba + ((size_t)y * w + x) * 4;
            int v = (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8;
            v += r0[x];
            r0[x] = 0;
            int q;
            if (mode == RM_MONO) q = v >= 128 ? 255 : 0;
            else { q = (v * 15 + 127) / 255; q = q < 0 ? 0 : q > 15 ? 15 : q; q *= 17; }
            const int err = v - q;
            if (mode == RM_MONO) {
                const int a = err / 8;
                r0[x + 1] += a; r0[x + 2] += a; r1[x - 1] += a; r1[x] += a; r1[x + 1] += a; r2[x] += a;
            } else {
                r0[x + 1] += err * 7 / 16; r1[x - 1] += err * 3 / 16; r1[x] += err * 5 / 16; r1[x + 1] += err / 16;
            }
            p[0] = p[1] = p[2] = (uint8_t)(q < 0 ? 0 : q > 255 ? 255 : q);
        }
        memset(r2 - 2, 0, (size_t)(w + 4) * sizeof(int16_t));
    }
    free(e);
}

uint16_t *img_render565(const uint8_t *data, size_t n, int w, int h, uint16_t bg, int fill) {
    int iw, ih;
    if (img_size(data, n, &iw, &ih)) return NULL;
    // fill: cover the box (cropped); else fit inside it
    const long sx = (long)w * 1000 / iw, sy = (long)h * 1000 / ih;
    const long s = fill ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    int dw = (int)(iw * s / 1000), dh = (int)(ih * s / 1000);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if (dw > 4096 || dh > 4096) return NULL;
    uint8_t *rgba = img_decode_rgba(data, n, dw, dh);
    if (!rgba) return NULL;
    tone(rgba, dw, dh, g_rmode);
    uint16_t *out = (uint16_t *)malloc((size_t)w * h * 2);
    if (out) {
        for (int i = 0; i < w * h; i++) out[i] = bg;
        const int ox = (w - dw) / 2, oy = (h - dh) / 2;
        for (int y = 0; y < h; y++) {
            const int sy2 = y - oy;
            if (sy2 < 0 || sy2 >= dh) continue;
            for (int x = 0; x < w; x++) {
                const int sx2 = x - ox;
                if (sx2 < 0 || sx2 >= dw) continue;
                const uint8_t *p = rgba + ((size_t)sy2 * dw + sx2) * 4;
                out[(size_t)y * w + x] = blend565(bg, rgb565(p[0], p[1], p[2]), p[3]);
            }
        }
    }
    free(rgba);
    return out;
}

// -------------------------------------------------------------------------------- the cache

static void evict(size_t need) {
    while (s_nimg && s_bytes + need > IMG_CACHE_BYTES) {
        int lru = 0;
        for (int i = 1; i < s_nimg; i++)
            if (s_img[i].used < s_img[lru].used) lru = i;
        free(s_img[lru].px);
        free(s_img[lru].a);
        s_bytes -= s_img[lru].bytes;
        s_img[lru] = s_img[--s_nimg];
    }
}

static img_t *find(const char *key, int w, int h) {
    for (int i = 0; i < s_nimg; i++)
        if (s_img[i].w == w && s_img[i].h == h && s_img[i].mode == g_rmode && !strcmp(s_img[i].key, key)) return &s_img[i];
    return NULL;
}

void img_clear(void) {
    for (int i = 0; i < s_nimg; i++) {
        free(s_img[i].px);
        free(s_img[i].a);
    }
    s_nimg = 0;
    s_bytes = 0;
    s_nqueue = 0;
}

int img_pending(void) { return s_nqueue; }

static void store(const job_t *j, uint8_t *rgba) {
    const size_t px = (size_t)j->w * j->h, bytes = px * (j->mask ? 1 : 3);
    if (bytes > IMG_CACHE_BYTES * 3 / 4) return;
    evict(bytes);
    if (s_nimg == IMG_MAX) evict(IMG_CACHE_BYTES);
    img_t *e = &s_img[s_nimg];
    memset(e, 0, sizeof *e);
    snprintf(e->key, sizeof e->key, "%s", j->key);
    e->w = j->w;
    e->h = j->h;
    e->mask = j->mask;
    e->mode = g_rmode;
    e->used = ++s_clock;
    if (!rgba) {   // failed: remembered, so the page does not ask again
        e->state = -1;
        s_nimg++;
        return;
    }
    if (!j->mask) tone(rgba, j->w, j->h, g_rmode);
    e->a = (uint8_t *)malloc(px);
    if (!j->mask) e->px = (uint16_t *)malloc(px * 2);
    if (!e->a || (!j->mask && !e->px)) {
        free(e->a);
        free(e->px);
        return;
    }
    for (size_t i = 0; i < px; i++) {
        const uint8_t *p = rgba + i * 4;
        if (j->mask) {   // a formula: dark strokes on transparent (or on white)
            const unsigned lum = (p[0] * 77u + p[1] * 150u + p[2] * 29u) >> 8;
            e->a[i] = (uint8_t)(p[3] * (255u - lum) / 255u);
        } else {
            e->a[i] = p[3];
            e->px[i] = rgb565(p[0], p[1], p[2]);
        }
    }
    e->state = 1;
    e->bytes = bytes;
    s_bytes += bytes;
    s_nimg++;
}

int img_lookup(const char *key, int w, int h, int mask) {
    img_t *e = find(key, w, h);
    if (e) {
        e->used = ++s_clock;
        return e->state;
    }
    for (int i = 0; i < s_nqueue; i++)
        if (s_queue[i].w == w && s_queue[i].h == h && !strcmp(s_queue[i].key, key)) return 0;
    if (s_nqueue < QUEUE_MAX) {
        job_t *j = &s_queue[s_nqueue++];
        snprintf(j->key, sizeof j->key, "%s", key);
        j->w = w;
        j->h = h;
        j->mask = mask;
    }
    return 0;
}

int img_draw(const char *key, int w, int h, int x, int y, uint16_t tint) {
    img_t *e = find(key, w, h);
    if (!e || e->state != 1) return -1;
    if (e->mask) {
        gfx_mask(e->a, w, h, x, y, tint);
        return 0;
    }
    for (int j = 0; j < h; j++) {
        const int sy = y + j;
        if (sy < 0 || sy >= g_h) continue;
        gfx_row(sy, x, e->px + (size_t)j * w, e->a + (size_t)j * w, w);
    }
    gfx_dirty(y, y + h);
    return 0;
}

int img_work(int (*read)(void *ctx, const char *key, uint8_t **data, size_t *n), void *ctx) {
    if (!s_nqueue) return 0;
    const job_t j = s_queue[0];
    memmove(s_queue, s_queue + 1, sizeof(job_t) * (size_t)(s_nqueue - 1));
    s_nqueue--;
    uint8_t *data = NULL;
    size_t n = 0;
    uint8_t *rgba = NULL;
    const char *kind = "missing";
    if (read(ctx, j.key, &data, &n) == 0) {
        kind = n >= 12 && !memcmp(data + 8, "WEBP", 4) ? "webp" : n >= 4 && !memcmp(data + 1, "PNG", 3) ? "png" :
               n >= 2 && data[0] == 0xFF && data[1] == 0xD8 ? "jpeg" : n >= 3 && !memcmp(data, "GIF", 3) ? "gif" :
               is_svg(data, n) ? "svg" : "other";
        rgba = img_decode_rgba(data, n, j.w, j.h);
        free(data);
    }
    snprintf(s_last, sizeof s_last, "%s %u KB -> %dx%d%s", kind, (unsigned)(n / 1024), j.w, j.h, rgba ? "" : " FAILED");
    store(&j, rgba);
    free(rgba);
    return 1;
}

const char *img_last(void) { return s_last; }

void img_stats(int *n, size_t *bytes) {
    if (n) *n = s_nimg;
    if (bytes) *bytes = s_bytes;
}
