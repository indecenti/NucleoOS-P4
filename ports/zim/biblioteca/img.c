// img.c — the pictures of the pages: decoded from the collection (WebP, PNG, JPEG, GIF, BMP, SVG) at
// the size the page shows them, kept in a cache of decoded pixels (RGB565 + alpha, IMG_CACHE bytes),
// drawn into the frame buffer. Decoding is never done while drawing: a picture not decoded yet is a
// placeholder and goes into a queue the main loop works through, one picture at a time.
// Formulas (and any picture marked "mask") keep only their coverage and take the text colour, so a
// black-on-transparent formula of Wikipedia reads on the dark theme too.
#include "bib.h"

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

#define IMG_CACHE (3u << 20)          // decoded pixels kept, bytes
#define IMG_SRC_MAX (1200u * 850u)    // largest picture decoded whole: 4 MB as RGBA, inside 12 MiB (WebP: 4x, scaled while decoding)
#define IMG_MAX 160                   // cache entries
#define QUEUE_MAX 48

typedef struct {
    char path[NVZIM_PATH_MAX];
    int w, h, mask;
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

typedef struct {
    char path[NVZIM_PATH_MAX];
    int w, h, mask;
} job_t;
static job_t s_queue[QUEUE_MAX];
static int s_nqueue;
static char s_last[96];   // what the last img_work did, for the log

// -------------------------------------------------------------------------------- decoding

static int is_svg(const uint8_t *d, size_t n) {
    const size_t lim = n < 1024 ? n : 1024;
    for (size_t i = 0; i + 4 <= lim; i++)
        if (d[i] == '<' && (d[i + 1] == 's' || d[i + 1] == 'S') && (d[i + 2] == 'v' || d[i + 2] == 'V') &&
            (d[i + 3] == 'g' || d[i + 3] == 'G'))
            return 1;
    return 0;
}

// ---- SVG: <use> and currentColor, which NanoSVG does not know, written out before it parses.
// MathJax (the formulas of Wikipedia) defines each glyph once in <defs> and places it with
// <use xlink:href="#id" x=".." y="..">: each <use> becomes <path d="(the glyph)" transform="translate(x y)"/>.

typedef struct { char *p; size_t n, cap; int oom; } buf_t;

static void bput(buf_t *b, const char *s, size_t n) {
    if (b->oom) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + n + 1) cap *= 2;
        char *q = (char *)realloc(b->p, cap);
        if (!q) { b->oom = 1; return; }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

// The value of attribute `name` in the tag [t, e) (quoted), or NULL; *len its length.
static const char *tag_attr(const char *t, const char *e, const char *name, size_t *len) {
    const size_t nl = strlen(name);
    for (const char *p = t; p + nl + 2 < e; p++) {
        if ((p == t || p[-1] == ' ' || p[-1] == '\n' || p[-1] == '\t' || p[-1] == '\r') && !strncmp(p, name, nl) &&
            p[nl] == '=' && (p[nl + 1] == '"' || p[nl + 1] == '\'')) {
            const char q = p[nl + 1];
            const char *v = p + nl + 2, *ve = memchr(v, q, (size_t)(e - v));
            if (!ve) return NULL;
            *len = (size_t)(ve - v);
            return v;
        }
    }
    return NULL;
}

typedef struct { const char *id, *d; size_t idn, dn; } glyph_def_t;

static char *svg_expand(const char *s, size_t n) {
    glyph_def_t *defs = NULL;
    size_t ndefs = 0, cdefs = 0;
    for (const char *p = s; (p = strstr(p, "<path")) != NULL; p += 5) {   // every <path id=.. d=..>
        const char *e = strchr(p, '>');
        if (!e) break;
        size_t il, dl;
        const char *id = tag_attr(p, e, "id", &il), *d = tag_attr(p, e, "d", &dl);
        if (!id || !d) continue;
        if (ndefs == cdefs) {
            cdefs = cdefs ? cdefs * 2 : 64;
            glyph_def_t *nd = (glyph_def_t *)realloc(defs, sizeof *defs * cdefs);
            if (!nd) break;
            defs = nd;
        }
        defs[ndefs++] = (glyph_def_t){id, d, il, dl};
    }
    buf_t b = {0};
    const char *p = s, *end = s + n;
    while (p < end) {
        const char *u = strstr(p, "<use");
        const char *cc = strstr(p, "currentColor");
        if (cc && (!u || cc < u)) {   // currentColor -> black: a formula is drawn as a mask anyway
            bput(&b, p, (size_t)(cc - p));
            bput(&b, "#000000", 7);
            p = cc + 12;
            continue;
        }
        if (!u) { bput(&b, p, (size_t)(end - p)); break; }
        bput(&b, p, (size_t)(u - p));
        const char *e = strchr(u, '>');
        if (!e) break;
        size_t hl = 0, xl = 0, yl = 0, tl = 0;
        const char *h = tag_attr(u, e, "xlink:href", &hl);
        if (!h) h = tag_attr(u, e, "href", &hl);
        const char *x = tag_attr(u, e, "x", &xl), *y = tag_attr(u, e, "y", &yl);
        const char *tf = tag_attr(u, e, "transform", &tl);   // a subscript: scale(0.707)
        if (h && hl > 1 && h[0] == '#')
            for (size_t i = 0; i < ndefs; i++)
                if (defs[i].idn == hl - 1 && !memcmp(defs[i].id, h + 1, hl - 1)) {
                    // <use transform=T x y> is <g transform="T translate(x y)"> around the glyph
                    char tr[224];
                    snprintf(tr, sizeof tr, "<path transform=\"%.*s translate(%.*s %.*s)\" d=\"",
                             (int)(tf && tl < 96 ? tl : 0), tf && tl < 96 ? tf : "", (int)(x && xl < 30 ? xl : 1),
                             x && xl < 30 ? x : "0", (int)(y && yl < 30 ? yl : 1), y && yl < 30 ? y : "0");
                    bput(&b, tr, strlen(tr));
                    bput(&b, defs[i].d, defs[i].dn);
                    bput(&b, "\"/>", 3);
                    break;
                }
        p = e + 1;
        if (e[-1] != '/' && !strncmp(p, "</use>", 6)) p += 6;
    }
    free(defs);
    if (b.oom) { free(b.p); return NULL; }
    if (!b.p) b.p = (char *)calloc(1, 1);
    return b.p;
}

// RGBA sw x sh -> RGBA w x h: the average of the source pixels under each target pixel (shrinking),
// or bilinear (growing). The colour is averaged weighted by coverage, so transparent edges do not darken.
static uint8_t *resample(const uint8_t *src, int sw, int sh, int w, int h) {
    uint8_t *out = (uint8_t *)malloc((size_t)w * h * 4);
    if (!out) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *o = out + ((size_t)y * w + x) * 4;
            if (sw <= w && sh <= h) {   // growing (or the same size): bilinear
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
            uint32_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = ya; yy < (yb > ya ? yb : ya + 1) && yy < sh; yy++)
                for (int xx = xa; xx < (xb > xa ? xb : xa + 1) && xx < sw; xx++, n++) {
                    const uint8_t *p = src + ((size_t)yy * sw + xx) * 4;
                    r += p[0] * p[3];
                    g += p[1] * p[3];
                    b += p[2] * p[3];
                    a += p[3];
                }
            o[0] = (uint8_t)(a ? r / a : 0);
            o[1] = (uint8_t)(a ? g / a : 0);
            o[2] = (uint8_t)(a ? b / a : 0);
            o[3] = (uint8_t)(n ? a / n : 0);
        }
    return out;
}

uint8_t *img_decode_rgba(const uint8_t *d, size_t n, int w, int h) {
    if (!d || n < 8 || w <= 0 || h <= 0 || w > 4096 || h > 4096) return NULL;
    // WebP: decoded straight at the target size
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) {
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
        if (out) {
            for (int y = 0; y < h; y++)
                memcpy(out + (size_t)y * w * 4, cfg.output.u.RGBA.rgba + (size_t)y * cfg.output.u.RGBA.stride, (size_t)w * 4);
        }
        WebPFreeDecBuffer(&cfg.output);
        return out;
    }
    // SVG: rasterised at the target size
    if (is_svg(d, n)) {
        char *raw = (char *)malloc(n + 1);   // a string for strstr; NanoSVG then parses a copy in place
        if (!raw) return NULL;
        memcpy(raw, d, n);
        raw[n] = 0;
        for (size_t i = 0; i < n; i++)
            if (!raw[i]) raw[i] = ' ';   // a NUL inside the file would cut it short
        char *text = svg_expand(raw, n);
        free(raw);
        if (!text) return NULL;
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
                // Shapes far outside any sane range (a hostile file: 1e20) would overflow the
                // rasteriser's fixed point: they are not drawn.
                const float lim = 1e5f;
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
    // PNG (LodePNG), JPEG, GIF (first frame) and BMP (stb_image): decoded whole, then resampled
    uint8_t *rgba = NULL;
    unsigned sw = 0, sh = 0;
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) {
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
    free(rgba);   // stb_image and LodePNG both allocate with malloc
    return out;
}

// -------------------------------------------------------------------------------- the cache

static void evict(size_t need) {
    while (s_nimg && s_bytes + need > IMG_CACHE) {
        int lru = 0;
        for (int i = 1; i < s_nimg; i++)
            if (s_img[i].used < s_img[lru].used) lru = i;
        free(s_img[lru].px);
        free(s_img[lru].a);
        s_bytes -= s_img[lru].bytes;
        s_img[lru] = s_img[--s_nimg];
    }
}

static img_t *find(const char *path, int w, int h) {
    for (int i = 0; i < s_nimg; i++)
        if (s_img[i].w == w && s_img[i].h == h && !strcmp(s_img[i].path, path)) return &s_img[i];
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

static void store(const job_t *j, const uint8_t *rgba) {
    const size_t px = (size_t)j->w * j->h, bytes = px * (j->mask ? 1 : 3);
    if (bytes > IMG_CACHE / 2) return;
    evict(bytes);
    if (s_nimg == IMG_MAX) evict(IMG_CACHE);   // no slot left: start over
    img_t *e = &s_img[s_nimg];
    memset(e, 0, sizeof *e);
    snprintf(e->path, sizeof e->path, "%s", j->path);
    e->w = j->w;
    e->h = j->h;
    e->mask = j->mask;
    e->used = ++s_clock;
    if (!rgba) {   // failed: remembered, so the page does not ask again
        e->state = -1;
        s_nimg++;
        return;
    }
    e->a = (uint8_t *)malloc(px);
    if (!j->mask) e->px = (uint16_t *)malloc(px * 2);
    if (!e->a || (!j->mask && !e->px)) {
        free(e->a);
        free(e->px);
        return;
    }
    for (size_t i = 0; i < px; i++) {
        const uint8_t *p = rgba + i * 4;
        if (j->mask) {
            // a formula: dark strokes on transparent (or on white): coverage from alpha and darkness
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

int img_lookup(const char *path, int w, int h, int mask) {
    img_t *e = find(path, w, h);
    if (e) {
        e->used = ++s_clock;
        return e->state;
    }
    for (int i = 0; i < s_nqueue; i++)
        if (s_queue[i].w == w && s_queue[i].h == h && !strcmp(s_queue[i].path, path)) return 0;
    if (s_nqueue < QUEUE_MAX) {
        job_t *j = &s_queue[s_nqueue++];
        snprintf(j->path, sizeof j->path, "%s", path);
        j->w = w;
        j->h = h;
        j->mask = mask;
    }
    return 0;
}

int img_draw(const char *path, int w, int h, int x, int y, uint16_t tint) {
    img_t *e = find(path, w, h);
    if (!e || e->state != 1) return -1;
    if (e->mask) {
        gfx_mask(e->a, w, h, x, y, tint);
        return 0;
    }
    // clipped like gfx_mask, colour by colour
    for (int j = 0; j < h; j++) {
        const int sy = y + j;
        if (sy < 0 || sy >= g_h) continue;
        gfx_row(sy, x, e->px + (size_t)j * w, e->a + (size_t)j * w, w);
    }
    gfx_dirty(y, y + h);
    return 0;
}

// Decodes a queued picture with `read`: the first one whose cluster is decoded in memory already
// (the pictures of a page share a few clusters; going by the queue alone would decompress the same
// cluster again and again), else the oldest. Returns 1 when one was handled, 0 when the queue is empty.
int img_work(int (*read)(void *ctx, const char *path, const uint8_t **data, size_t *n, void **tok),
             void (*done)(void *ctx, void *tok), int (*cluster_ready)(void *ctx, const char *path), void *ctx) {
    if (!s_nqueue) return 0;
    int pick = 0;
    for (int i = 0; cluster_ready && i < s_nqueue && i < 12; i++)
        if (cluster_ready(ctx, s_queue[i].path) > 0) { pick = i; break; }
    const job_t j = s_queue[pick];
    memmove(s_queue + pick, s_queue + pick + 1, sizeof(job_t) * (size_t)(s_nqueue - pick - 1));
    s_nqueue--;
    const uint8_t *data = NULL;
    size_t n = 0;
    void *tok = NULL;
    uint8_t *rgba = NULL;
    const char *kind = "missing";
    if (read(ctx, j.path, &data, &n, &tok) == 0) {
        kind = n >= 12 && !memcmp(data + 8, "WEBP", 4) ? "webp" : n >= 4 && !memcmp(data, "PNG", 4) ? "png" :
               n >= 2 && data[0] == 0xFF && data[1] == 0xD8 ? "jpeg" : n >= 3 && !memcmp(data, "GIF", 3) ? "gif" :
               is_svg(data, n) ? "svg" : "other";
        rgba = img_decode_rgba(data, n, j.w, j.h);
        done(ctx, tok);
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
