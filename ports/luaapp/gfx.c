// gfx.c — the Lua App engine's software renderer: every drawing call lands in a guest-side RGB565
// surface (the screen, an off-screen canvas or an image), anti-aliased where it is cheap (text,
// circle and rounded-rect edges, thin lines), then luaapp.c blits the rows that changed to the OS
// canvas (ABI 6 persist mode) once per frame.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "gen/font_data.h"

surface_t *g_tgt;                  // current target
surface_t  g_screen;
int g_alpha = 255;
static int cx0, cy0, cx1, cy1;     // clip rect on the current target, [x0,x1) x [y0,y1)
int g_dirty_y0 = 1 << 30, g_dirty_y1 = -1;

// transform stack: p' = M p + t, M = [a c; b d]. sx, sy are the scale magnitudes (signed when M is
// diagonal) that sizes (radii, text, line widths) use; rot is set when M is not diagonal.
typedef struct { float tx, ty, sx, sy, a, b, c, d; int rot; } xf_t;
static xf_t xf_stack[32];
static int xf_n;
static xf_t xf = { 0, 0, 1, 1, 1, 0, 0, 1, 0 };
static void xf_fix(void) {
    xf.rot = fabsf(xf.b) > 1e-6f || fabsf(xf.c) > 1e-6f;
    if (xf.rot) { xf.sx = sqrtf(xf.a * xf.a + xf.b * xf.b); xf.sy = sqrtf(xf.c * xf.c + xf.d * xf.d); }
    else { xf.b = xf.c = 0; xf.sx = xf.a; xf.sy = xf.d; }
}
uint32_t g_tint = 0xFFFFFF;        // images are multiplied by it (the LOVE colour on draw)

static inline uint16_t rgb565(uint32_t c) {
    return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}
static inline uint16_t blend(uint16_t d, uint16_t s, int a) {   // a 0..256
    uint32_t dd = (d | ((uint32_t)d << 16)) & 0x07E0F81F;
    uint32_t ss = (s | ((uint32_t)s << 16)) & 0x07E0F81F;
    uint32_t r = ((dd * (uint32_t)(32 - (a >> 3)) + ss * (uint32_t)(a >> 3)) >> 5) & 0x07E0F81F;
    return (uint16_t)(r | (r >> 16));
}

void gfx_reset_clip(void) { cx0 = 0; cy0 = 0; cx1 = g_tgt->w; cy1 = g_tgt->h; }
void gfx_set_target(surface_t *s) { g_tgt = s ? s : &g_screen; gfx_reset_clip(); }
void gfx_clip(int x, int y, int w, int h) {
    float fx = x * xf.sx + xf.tx, fy = y * xf.sy + xf.ty;
    cx0 = (int)fx; cy0 = (int)fy; cx1 = (int)(fx + w * xf.sx); cy1 = (int)(fy + h * xf.sy);
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > g_tgt->w) cx1 = g_tgt->w;
    if (cy1 > g_tgt->h) cy1 = g_tgt->h;
}

static inline void dirty(int y0, int y1) {
    if (g_tgt != &g_screen) return;
    if (y0 < cy0) y0 = cy0;
    if (y1 > cy1) y1 = cy1;
    if (y0 >= y1) return;
    if (y0 < g_dirty_y0) g_dirty_y0 = y0;
    if (y1 > g_dirty_y1) g_dirty_y1 = y1;
}

int g_blend;                       // GFX_BLEND_*: how a pixel meets the one under it
// the non-alpha blend modes, per channel in RGB565 (a = coverage 0..255)
static inline uint16_t blend_mode(uint16_t d, uint16_t s, int a) {
    if (a >= 255 && g_blend == GFX_BLEND_MUL) {         // the hot one (light maps): no alpha
        int r = ((d >> 11) * (s >> 11) * 1057) >> 15, g = (((d >> 5) & 63) * ((s >> 5) & 63) * 1041) >> 16,
            b = ((d & 31) * (s & 31) * 1057) >> 15;
        return (uint16_t)((r << 11) | (g << 5) | b);
    }
    if (a >= 255 && g_blend == GFX_BLEND_ADD) {
        int r = (d >> 11) + (s >> 11), g = ((d >> 5) & 63) + ((s >> 5) & 63), b = (d & 31) + (s & 31);
        if (r > 31) r = 31;
        if (g > 63) g = 63;
        if (b > 31) b = 31;
        return (uint16_t)((r << 11) | (g << 5) | b);
    }
    int dr = d >> 11, dg = (d >> 5) & 63, db = d & 31, sr = s >> 11, sg = (s >> 5) & 63, sb = s & 31, r, g, b;
    switch (g_blend) {
    case GFX_BLEND_ADD: r = dr + sr * a / 255; g = dg + sg * a / 255; b = db + sb * a / 255; break;
    case GFX_BLEND_SUB: r = dr - sr * a / 255; g = dg - sg * a / 255; b = db - sb * a / 255; break;
    case GFX_BLEND_MUL:
        r = dr + (dr * sr / 31 - dr) * a / 255; g = dg + (dg * sg / 63 - dg) * a / 255; b = db + (db * sb / 31 - db) * a / 255; break;
    case GFX_BLEND_SCREEN:
        r = 31 - (31 - dr) * (31 - sr * a / 255) / 31; g = 63 - (63 - dg) * (63 - sg * a / 255) / 63;
        b = 31 - (31 - db) * (31 - sb * a / 255) / 31; break;
    case GFX_BLEND_LIGHTEN: r = sr > dr ? sr : dr; g = sg > dg ? sg : dg; b = sb > db ? sb : db; break;
    case GFX_BLEND_DARKEN: r = sr < dr ? sr : dr; g = sg < dg ? sg : dg; b = sb < db ? sb : db; break;
    default: return s;                                  // replace
    }
    r = r < 0 ? 0 : r > 31 ? 31 : r; g = g < 0 ? 0 : g > 63 ? 63 : g; b = b < 0 ? 0 : b > 31 ? 31 : b;
    return (uint16_t)((r << 11) | (g << 5) | b);
}
// one pixel with coverage a (0..255), global alpha applied
static inline void px_a(int x, int y, uint16_t c, int a) {
    if (x < cx0 || x >= cx1 || y < cy0 || y >= cy1) return;
    a = a * g_alpha / 255;
    if (a <= 0 && g_blend != GFX_BLEND_REPLACE) return;
    size_t i = (size_t)y * g_tgt->w + x;
    if (g_blend) {
        if (g_tgt->a) {
            int da = g_tgt->a[i];
            if (g_blend == GFX_BLEND_REPLACE) g_tgt->a[i] = (uint8_t)a;
            else if (g_blend == GFX_BLEND_ADD || g_blend == GFX_BLEND_SCREEN || g_blend == GFX_BLEND_LIGHTEN) g_tgt->a[i] = (uint8_t)(a > da ? a : da);
            if (!da && g_blend != GFX_BLEND_REPLACE && g_blend != GFX_BLEND_MUL) { g_tgt->px[i] = c; return; }
        }
        g_tgt->px[i] = blend_mode(g_tgt->px[i], c, a);
        return;
    }
    if (g_tgt->a) {
        int da = g_tgt->a[i];
        if (da == 0) { g_tgt->px[i] = c; g_tgt->a[i] = (uint8_t)a; return; }
        g_tgt->a[i] = (uint8_t)(a + da * (255 - a) / 255);
    }
    g_tgt->px[i] = a >= 255 ? c : blend(g_tgt->px[i], c, a + (a >> 7));
}

static void span(int y, int x0, int x1, uint16_t c) {   // [x0,x1)
    if (y < cy0 || y >= cy1) return;
    if (x0 < cx0) x0 = cx0;
    if (x1 > cx1) x1 = cx1;
    if (x0 >= x1) return;
    uint16_t *row = g_tgt->px + (size_t)y * g_tgt->w;
    uint8_t *ar = g_tgt->a ? g_tgt->a + (size_t)y * g_tgt->w : NULL;
    if (g_blend) {
        for (int x = x0; x < x1; x++) px_a(x, y, c, 255);
    } else if (g_alpha >= 255) {
        for (int x = x0; x < x1; x++) row[x] = c;
        if (ar) memset(ar + x0, 255, (size_t)(x1 - x0));
    } else if (!ar) {                                   // translucent fill, opaque target
        int a = g_alpha + (g_alpha >> 7);
        for (int x = x0; x < x1; x++) row[x] = blend(row[x], c, a);
    } else {
        for (int x = x0; x < x1; x++) px_a(x, y, c, 255);
    }
}
// span with fractional ends (horizontal anti-aliasing): covers [xl, xr) in float pixels
static void span_f(int y, float xl, float xr, uint16_t c) {
    if (xr <= xl) return;
    int il = (int)floorf(xl), ir = (int)floorf(xr);
    if (il == ir) { px_a(il, y, c, (int)((xr - xl) * 255)); return; }
    px_a(il, y, c, (int)((il + 1 - xl) * 255));
    span(y, il + 1, ir, c);
    if (xr > ir) px_a(ir, y, c, (int)((xr - ir) * 255));
}

static inline void tx(float x, float y, float *ox, float *oy) { *ox = xf.a * x + xf.c * y + xf.tx; *oy = xf.b * x + xf.d * y + xf.ty; }

void gfx_clear(uint32_t color) {
    uint16_t c = rgb565(color);
    int sa = g_alpha;
    g_alpha = 255;
    for (int y = cy0; y < cy1; y++) span(y, cx0, cx1, c);
    g_alpha = sa;
    dirty(cy0, cy1);
}
void gfx_clear_transparent(void) {
    if (!g_tgt->a) return;
    for (int y = cy0; y < cy1; y++) memset(g_tgt->a + (size_t)y * g_tgt->w + cx0, 0, (size_t)(cx1 - cx0));
}

// filled rect, optional radius (anti-aliased corners)
void gfx_rect(float x, float y, float w, float h, uint32_t color, float r) {
    if (xf.rot) { float p[8] = { x, y, x + w, y, x + w, y + h, x, y + h }; gfx_poly(p, 4, color); return; }
    float X, Y; tx(x, y, &X, &Y);
    float W = w * xf.sx, H = h * xf.sy;
    if (W < 0) { X += W; W = -W; }
    if (H < 0) { Y += H; H = -H; }
    if (X >= cx1 || Y >= cy1 || X + W <= cx0 || Y + H <= cy0) return;   // off the clip: nothing to do
    uint16_t c = rgb565(color);
    r *= xf.sx;
    if (r > W / 2) r = W / 2;
    if (r > H / 2) r = H / 2;
    int y0 = (int)floorf(Y + 0.5f), y1 = (int)floorf(Y + H + 0.5f);
    dirty(y0, y1);
    if (r < 0.5f) {
        int x0 = (int)floorf(X + 0.5f), x1 = (int)floorf(X + W + 0.5f);
        for (int yy = y0; yy < y1; yy++) span(yy, x0, x1, c);
        return;
    }
    for (int yy = y0; yy < y1; yy++) {
        float py = yy + 0.5f, inset = 0;
        if (py < Y + r) { float d = Y + r - py; inset = r - sqrtf(fmaxf(0, r * r - d * d)); }
        else if (py > Y + H - r) { float d = py - (Y + H - r); inset = r - sqrtf(fmaxf(0, r * r - d * d)); }
        span_f(yy, X + inset, X + W - inset, c);
    }
}
void gfx_frame(float x, float y, float w, float h, uint32_t color, float t, float r) {
    if (t < 1) t = 1;
    if (r < 0.5f || xf.rot) {
        gfx_rect(x, y, w, t, color, 0);
        gfx_rect(x, y + h - t, w, t, color, 0);
        gfx_rect(x, y + t, t, h - 2 * t, color, 0);
        gfx_rect(x + w - t, y + t, t, h - 2 * t, color, 0);
        return;
    }
    // rounded outline: per row, outer extent minus inner extent
    float X, Y; tx(x, y, &X, &Y);
    float W = w * xf.sx, H = h * xf.sy, T = t * xf.sx;
    r *= xf.sx;
    if (r > W / 2) r = W / 2;
    if (r > H / 2) r = H / 2;
    float ri = r - T > 0 ? r - T : 0;
    uint16_t c = rgb565(color);
    int y0 = (int)floorf(Y + 0.5f), y1 = (int)floorf(Y + H + 0.5f);
    dirty(y0, y1);
    for (int yy = y0; yy < y1; yy++) {
        float py = yy + 0.5f, io = 0, ii = 0;
        bool inner = py >= Y + T && py < Y + H - T;
        if (py < Y + r) { float d = Y + r - py; io = r - sqrtf(fmaxf(0, r * r - d * d)); }
        else if (py > Y + H - r) { float d = py - (Y + H - r); io = r - sqrtf(fmaxf(0, r * r - d * d)); }
        if (!inner) { span_f(yy, X + io, X + W - io, c); continue; }
        float iy0 = Y + T, iy1 = Y + H - T;
        if (py < iy0 + ri) { float d = iy0 + ri - py; ii = ri - sqrtf(fmaxf(0, ri * ri - d * d)); }
        else if (py > iy1 - ri) { float d = py - (iy1 - ri); ii = ri - sqrtf(fmaxf(0, ri * ri - d * d)); }
        span_f(yy, X + io, X + T + ii, c);
        span_f(yy, X + W - T - ii, X + W - io, c);
    }
}

void gfx_circle(float x, float y, float r, uint32_t color) {
    float X, Y; tx(x, y, &X, &Y);
    float R = r * (fabsf(xf.sx) + fabsf(xf.sy)) * 0.5f;
    if (R <= 0) return;
    if (X - R >= cx1 || Y - R >= cy1 || X + R <= cx0 || Y + R <= cy0) return;
    uint16_t c = rgb565(color);
    int y0 = (int)floorf(Y - R), y1 = (int)ceilf(Y + R);
    dirty(y0, y1 + 1);
    for (int yy = y0; yy <= y1; yy++) {
        float d = yy + 0.5f - Y;
        if (fabsf(d) >= R) continue;
        float e = sqrtf(R * R - d * d);
        span_f(yy, X - e, X + e, c);
    }
}
void gfx_ring(float x, float y, float r, uint32_t color, float t) {
    float X, Y; tx(x, y, &X, &Y);
    float s = (xf.sx + xf.sy) * 0.5f, R = r * s, T = (t < 1 ? 1 : t) * s, Ri = R - T;
    if (R <= 0) return;
    uint16_t c = rgb565(color);
    int y0 = (int)floorf(Y - R), y1 = (int)ceilf(Y + R);
    dirty(y0, y1 + 1);
    for (int yy = y0; yy <= y1; yy++) {
        float d = yy + 0.5f - Y;
        if (fabsf(d) >= R) continue;
        float e = sqrtf(R * R - d * d);
        if (Ri > 0 && fabsf(d) < Ri) {
            float ei = sqrtf(Ri * Ri - d * d);
            span_f(yy, X - e, X - ei, c);
            span_f(yy, X + ei, X + e, c);
        } else {
            span_f(yy, X - e, X + e, c);
        }
    }
}

// Arc / pie: angles in degrees, 0 = 3 o'clock, growing clockwise (screen y points down).
// thick <= 0 -> filled pie. Per-pixel over the bounding box, anti-aliased on the radius.
void gfx_arc(float x, float y, float r, float a0, float a1, uint32_t color, float thick) {
    float X, Y; tx(x, y, &X, &Y);
    float s = (xf.sx + xf.sy) * 0.5f, R = r * s, Ri = thick > 0 ? R - thick * s : 0;
    if (R <= 0) return;
    if (a1 < a0) { float t = a0; a0 = a1; a1 = t; }
    if (a1 - a0 >= 360) { a0 = 0; a1 = 360; }
    float span_deg = a1 - a0;
    a0 = fmodf(a0, 360); if (a0 < 0) a0 += 360;
    uint16_t c = rgb565(color);
    int y0 = (int)floorf(Y - R), y1 = (int)ceilf(Y + R), x0 = (int)floorf(X - R), x1 = (int)ceilf(X + R);
    dirty(y0, y1 + 1);
    for (int yy = y0; yy <= y1; yy++) {
        if (yy < cy0 || yy >= cy1) continue;
        float dy = yy + 0.5f - Y;
        for (int xx = x0; xx <= x1; xx++) {
            if (xx < cx0 || xx >= cx1) continue;
            float dx = xx + 0.5f - X, d = sqrtf(dx * dx + dy * dy);
            if (d > R + 0.5f || d < Ri - 0.5f) continue;
            float ang = atan2f(dy, dx) * 57.2957795f;
            if (ang < 0) ang += 360;
            float rel = ang - a0;
            if (rel < 0) rel += 360;
            if (rel > span_deg) continue;
            float cov = 1;
            if (d > R - 0.5f) cov = R + 0.5f - d;
            if (Ri > 0 && d < Ri + 0.5f) cov = fminf(cov, d - (Ri - 0.5f));
            px_a(xx, yy, c, (int)(cov * 255));
        }
    }
}

// Xiaolin Wu anti-aliased line (thin) or a filled quad (thick)
void gfx_poly(const float *pts, int n, uint32_t color);
void gfx_line(float x0, float y0, float x1, float y1, uint32_t color, float t) {
    float X0, Y0, X1, Y1;
    tx(x0, y0, &X0, &Y0); tx(x1, y1, &X1, &Y1);
    float T = t * (xf.sx + xf.sy) * 0.5f;
    uint16_t c = rgb565(color);
    if (T > 1.5f) {
        float dx = X1 - X0, dy = Y1 - Y0, L = sqrtf(dx * dx + dy * dy);
        if (L < 0.01f) return;
        float nx = -dy / L * T * 0.5f, ny = dx / L * T * 0.5f;
        float q[8] = { X0 + nx, Y0 + ny, X1 + nx, Y1 + ny, X1 - nx, Y1 - ny, X0 - nx, Y0 - ny };
        xf_t save = xf;
        xf = (xf_t){ 0, 0, 1, 1 };
        gfx_poly(q, 4, color);
        xf = save;
        return;
    }
    dirty((int)fminf(Y0, Y1) - 1, (int)fmaxf(Y0, Y1) + 2);
    bool steep = fabsf(Y1 - Y0) > fabsf(X1 - X0);
    if (steep) { float t2 = X0; X0 = Y0; Y0 = t2; t2 = X1; X1 = Y1; Y1 = t2; }
    if (X0 > X1) { float t2 = X0; X0 = X1; X1 = t2; t2 = Y0; Y0 = Y1; Y1 = t2; }
    float dx = X1 - X0, dy = Y1 - Y0, g = dx < 0.001f ? 1 : dy / dx;
    int xa = (int)floorf(X0 + 0.5f), xb = (int)floorf(X1 + 0.5f);
    float yv = Y0 + g * (xa - X0);
    if (xb - xa > 4096) return;
    for (int xx = xa; xx <= xb; xx++) {
        float fy = yv - 0.5f;
        int iy = (int)floorf(fy);
        float f = fy - iy;
        if (steep) { px_a(iy, xx, c, (int)((1 - f) * 255)); px_a(iy + 1, xx, c, (int)(f * 255)); }
        else       { px_a(xx, iy, c, (int)((1 - f) * 255)); px_a(xx, iy + 1, c, (int)(f * 255)); }
        yv += g;
    }
}

// even-odd scanline polygon fill (points already in user space; transformed here)
static int cmp_f(const void *a, const void *b) { float d = *(const float *)a - *(const float *)b; return (d > 0) - (d < 0); }
void gfx_poly(const float *pts, int n, uint32_t color) {
    if (n < 3 || n > 512) return;
    static float P[1024], xs[512];
    float miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) {
        tx(pts[2 * i], pts[2 * i + 1], &P[2 * i], &P[2 * i + 1]);
        if (P[2 * i + 1] < miny) miny = P[2 * i + 1];
        if (P[2 * i + 1] > maxy) maxy = P[2 * i + 1];
    }
    uint16_t c = rgb565(color);
    int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (y0 < cy0) y0 = cy0;
    if (y1 > cy1) y1 = cy1;
    dirty(y0, y1);
    for (int yy = y0; yy < y1; yy++) {
        float py = yy + 0.5f;
        int k = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            float ya = P[2 * i + 1], yb = P[2 * j + 1];
            if ((ya <= py && yb > py) || (yb <= py && ya > py)) {
                float xa = P[2 * i], xb = P[2 * j];
                xs[k++] = xa + (py - ya) / (yb - ya) * (xb - xa);
            }
        }
        qsort(xs, (size_t)k, sizeof xs[0], cmp_f);
        for (int i = 0; i + 1 < k; i += 2) span(yy, (int)floorf(xs[i] + 0.5f), (int)floorf(xs[i + 1] + 0.5f), c);
    }
}

// ---- text ---------------------------------------------------------------------------------------
static const lf_font_t *font_for(float size) {
    const lf_font_t *best = &lf_fonts[0];
    for (int i = 0; i < LF_NFONTS; i++) if (lf_fonts[i].size <= size + 0.5f) best = &lf_fonts[i];
    return best;
}
static uint32_t utf8_next(const unsigned char **p) {
    const unsigned char *s = *p;
    uint32_t c = *s++;
    if (c >= 0xC0 && c < 0xE0 && (*s & 0xC0) == 0x80) { c = ((c & 0x1F) << 6) | (*s++ & 0x3F); }
    else if (c >= 0xE0 && c < 0xF0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80) {
        c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F); s += 2;
    } else if (c >= 0xF0 && (s[0] & 0xC0) == 0x80) {
        while ((*s & 0xC0) == 0x80) s++;
        c = '?';
    }
    *p = s;
    return c;
}
static const lf_glyph_t *glyph(const lf_font_t *f, uint32_t c) {
    if (c > f->last) {        // the extra glyphs follow the font's range
        for (int i = 0; i < LF_NEXTRA; i++) if (lf_extra[i] == c) return &f->g[f->last - 31 + i];
        if (c >= 0xA0 && c <= 0xFF) {   // Latin-1 letters in an ASCII-only (display) size
            static const char fold[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
            if (c >= 0xC0) c = (uint32_t)fold[c - 0xC0];
            else c = '?';
        } else c = '?';
    }
    if (c < 32 || (c >= 127 && c < 160)) c = '?';
    return &f->g[c - 32];
}
int gfx_font_line(float size) { const lf_font_t *f = font_for(size * xf.sy); return (int)((f->ascent + f->descent) / xf.sy); }
int gfx_font_ascent(float size) { return (int)(font_for(size * xf.sy)->ascent / xf.sy); }
int gfx_text_width(const char *s, float size) {
    const lf_font_t *f = font_for(size * xf.sx);
    int w = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) { uint32_t c = utf8_next(&p); if (c == '\n') break; w += glyph(f, c)->adv; }
    return (int)(w / xf.sx);
}
// draws one line (stops at '\n'); y = top of the line box. Returns the advance (user units).
int gfx_text(float x, float y, const char *s, float size, uint32_t color) {
    float X, Y; tx(x, y, &X, &Y);
    const lf_font_t *f = font_for(size * xf.sy);
    uint16_t c = rgb565(color);
    int pen = (int)floorf(X + 0.5f), base = (int)floorf(Y + 0.5f) + f->ascent, x_start = pen;
    dirty(base - f->ascent, base + f->descent + 1);
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t ch = utf8_next(&p);
        if (ch == '\n') break;
        const lf_glyph_t *g = glyph(f, ch);
        if (g->w && g->h) {
            const uint8_t *b = f->bits + g->off;
            int row = (g->w + 1) / 2, gx = pen + g->x, gy = base + g->y;
            if (gx + g->w >= cx0 && gx < cx1 && gy + g->h >= cy0 && gy < cy1) {
                for (int j = 0; j < g->h; j++) {
                    int yy = gy + j;
                    if (yy < cy0 || yy >= cy1) continue;
                    for (int i = 0; i < g->w; i++) {
                        int v = b[j * row + (i >> 1)];
                        v = (i & 1) ? (v & 15) : (v >> 4);
                        if (v) px_a(gx + i, yy, c, v * 17);
                    }
                }
            }
        }
        pen += g->adv;
    }
    return (int)((pen - x_start) / xf.sx);
}

// ---- surfaces (canvases, images) ----------------------------------------------------------------
surface_t *surface_new(int w, int h, bool alpha) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return NULL;
    surface_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->w = w; s->h = h;
    s->px = calloc((size_t)w * h, 2);
    s->a = alpha ? calloc((size_t)w * h, 1) : NULL;
    if (!s->px || (alpha && !s->a)) { surface_free(s); return NULL; }
    return s;
}
void surface_free(surface_t *s) {
    if (!s) return;
    if (g_tgt == s) gfx_set_target(NULL);
    free(s->px); free(s->a); free(s);
}
// Image file format (made by tools/lua_pack.py): "LIMG", u16 w, u16 h, u8 has_alpha, 3 pad,
// then w*h RGB565 little-endian, then (has_alpha) w*h alpha bytes.
surface_t *surface_from_limg(const uint8_t *d, size_t n) {
    if (n < 12 || memcmp(d, "LIMG", 4)) return NULL;
    int w = d[4] | d[5] << 8, h = d[6] | d[7] << 8, al = d[8];
    size_t need = 12 + (size_t)w * h * 2 + (al ? (size_t)w * h : 0);
    if (n < need) return NULL;
    surface_t *s = surface_new(w, h, al != 0);
    if (!s) return NULL;
    memcpy(s->px, d + 12, (size_t)w * h * 2);
    if (al) memcpy(s->a, d + 12 + (size_t)w * h * 2, (size_t)w * h);
    return s;
}
// draw the (qx,qy,qw,qh) part of a surface at (x,y), scaled to (dw,dh) (negative = mirrored),
// rotated by rot radians around (x,y) with the origin offset (ox,oy) in destination units; blended
// by its alpha and the global alpha, multiplied by g_tint. Nearest-neighbour sampling.
void gfx_draw_ex(surface_t *s, float x, float y, float dw, float dh, int qx, int qy, int qw, int qh,
                 float rot, float ox, float oy) {
    if (!s || s == g_tgt || dw == 0 || dh == 0) return;
    if (qx < 0) { qw += qx; qx = 0; }
    if (qy < 0) { qh += qy; qy = 0; }
    if (qx + qw > s->w) qw = s->w - qx;
    if (qy + qh > s->h) qh = s->h - qy;
    if (qw <= 0 || qh <= 0) return;
    float kx = dw / qw, ky = dh / qh, cs = cosf(rot), sn = sinf(rot);
    // quad pixel (u,v) -> canvas: A (u,v) + (ex,ey)
    float r00 = cs * kx, r01 = -sn * ky, r10 = sn * kx, r11 = cs * ky;
    float A00 = xf.a * r00 + xf.c * r10, A01 = xf.a * r01 + xf.c * r11;
    float A10 = xf.b * r00 + xf.d * r10, A11 = xf.b * r01 + xf.d * r11;
    float px = x - (cs * ox - sn * oy), py = y - (sn * ox + cs * oy);
    float ex, ey; tx(px, py, &ex, &ey);
    bool tinted = (g_tint & 0xFFFFFF) != 0xFFFFFF;
    static uint8_t lr[32], lg[64], lb[32];          // tint lookup per channel
    if (tinted) {
        uint32_t tr = (g_tint >> 16) & 255, tg = (g_tint >> 8) & 255, tb = g_tint & 255;
        for (int i = 0; i < 32; i++) { lr[i] = (uint8_t)((i * tr + 127) / 255); lb[i] = (uint8_t)((i * tb + 127) / 255); }
        for (int i = 0; i < 64; i++) lg[i] = (uint8_t)((i * tg + 127) / 255);
    }
#define TINT(c) (tinted ? (uint16_t)((lr[(c) >> 11] << 11) | (lg[((c) >> 5) & 63] << 5) | lb[(c) & 31]) : (c))
    if (fabsf(A01) < 1e-6f && fabsf(A10) < 1e-6f && A00 > 0 && A11 > 0) {   // axis-aligned, not mirrored
        int x0 = (int)floorf(ex + 0.5f), y0 = (int)floorf(ey + 0.5f);
        int w = (int)floorf(ex + qw * A00 + 0.5f) - x0, h = (int)floorf(ey + qh * A11 + 0.5f) - y0;
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return;
        dirty(y0, y0 + h);
        int lo = x0 < cx0 ? cx0 - x0 : 0, hi = x0 + w > cx1 ? cx1 - x0 : w;
        if (lo >= hi) return;
        static int sx[8192];
        for (int i = lo; i < hi; i++) sx[i] = qx + (int)((int64_t)i * qw / w);
        bool exact = w == qw && g_alpha >= 255 && !s->a && !g_tgt->a && !tinted && !g_blend;
        for (int j = 0; j < h; j++) {
            int yy = y0 + j;
            if (yy < cy0 || yy >= cy1) continue;
            int sy = qy + (int)((int64_t)j * qh / h);
            const uint16_t *sr = s->px + (size_t)sy * s->w;
            if (exact) { memcpy(g_tgt->px + (size_t)yy * g_tgt->w + x0 + lo, sr + qx + lo, (size_t)(hi - lo) * 2); continue; }
            const uint8_t *ar = s->a ? s->a + (size_t)sy * s->w : NULL;
            if (g_blend && g_blend != GFX_BLEND_REPLACE && g_alpha >= 255 && !g_tgt->a) {   // blend modes, opaque target
                uint16_t *dr = g_tgt->px + (size_t)yy * g_tgt->w + x0;
                for (int i = lo; i < hi; i++) {
                    int a = ar ? ar[sx[i]] : 255;
                    if (a) dr[i] = blend_mode(dr[i], TINT(sr[sx[i]]), a);
                }
                continue;
            }
            if (g_alpha >= 255 && !g_tgt->a && !g_blend) {          // common case: opaque target
                uint16_t *dr = g_tgt->px + (size_t)yy * g_tgt->w + x0;
                if (!ar) { for (int i = lo; i < hi; i++) dr[i] = TINT(sr[sx[i]]); continue; }
                for (int i = lo; i < hi; i++) {
                    int a = ar[sx[i]];
                    if (a == 255) dr[i] = TINT(sr[sx[i]]);
                    else if (a) dr[i] = blend(dr[i], TINT(sr[sx[i]]), a + (a >> 7));
                }
                continue;
            }
            if (ar && g_alpha >= 255 && !tinted && !g_blend) {                   // canvas onto canvas
                uint16_t *dr = g_tgt->px + (size_t)yy * g_tgt->w + x0;
                uint8_t *da = g_tgt->a + (size_t)yy * g_tgt->w + x0;
                for (int i = lo; i < hi; i++) {
                    int a = ar[sx[i]];
                    if (a == 255) { dr[i] = sr[sx[i]]; da[i] = 255; }
                    else if (a) px_a(x0 + i, yy, sr[sx[i]], a);
                }
                continue;
            }
            for (int i = lo; i < hi; i++) {
                int a = ar ? ar[sx[i]] : 255;
                if (a) px_a(x0 + i, yy, TINT(sr[sx[i]]), a);
            }
        }
        return;
    }
    float det = A00 * A11 - A01 * A10;
    if (fabsf(det) < 1e-9f) return;
    float i00 = A11 / det, i01 = -A01 / det, i10 = -A10 / det, i11 = A00 / det;
    float cxs[4] = { 0, (float)qw, (float)qw, 0 }, cys[4] = { 0, 0, (float)qh, (float)qh };
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int k = 0; k < 4; k++) {
        float X = A00 * cxs[k] + A01 * cys[k] + ex, Y = A10 * cxs[k] + A11 * cys[k] + ey;
        if (X < minx) minx = X;
        if (X > maxx) maxx = X;
        if (Y < miny) miny = Y;
        if (Y > maxy) maxy = Y;
    }
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx), y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    if (x0 < cx0) x0 = cx0;
    if (x1 > cx1) x1 = cx1;
    if (y0 < cy0) y0 = cy0;
    if (y1 > cy1) y1 = cy1;
    if (x0 >= x1 || y0 >= y1) return;
    dirty(y0, y1);
    for (int yy = y0; yy < y1; yy++) {
        float Y = yy + 0.5f - ey;
        for (int xx = x0; xx < x1; xx++) {
            float X = xx + 0.5f - ex;
            float u = i00 * X + i01 * Y, v = i10 * X + i11 * Y;
            if (u < 0 || v < 0 || u >= qw || v >= qh) continue;
            size_t si = (size_t)(qy + (int)v) * s->w + qx + (int)u;
            int a = s->a ? s->a[si] : 255;
            if (a) px_a(xx, yy, TINT(s->px[si]), a);
        }
    }
}
#undef TINT
void gfx_draw(surface_t *s, float x, float y, float dw, float dh) {
    if (s) gfx_draw_ex(s, x, y, dw, dh, 0, 0, s->w, s->h, 0, 0, 0);
}

// ---- transform ----------------------------------------------------------------------------------
void gfx_push(void) { if (xf_n < 32) xf_stack[xf_n++] = xf; }
void gfx_pop(void) { if (xf_n > 0) xf = xf_stack[--xf_n]; }
void gfx_translate(float x, float y) { xf.tx += xf.a * x + xf.c * y; xf.ty += xf.b * x + xf.d * y; }
void gfx_scale(float sx, float sy) { xf.a *= sx; xf.b *= sx; xf.c *= sy; xf.d *= sy; xf_fix(); }
void gfx_rotate(float r) {
    float cs = cosf(r), sn = sinf(r), a = xf.a, b = xf.b, c = xf.c, d = xf.d;
    xf.a = a * cs + c * sn; xf.b = b * cs + d * sn;
    xf.c = c * cs - a * sn; xf.d = d * cs - b * sn;
    xf_fix();
}
void gfx_shear(float kx, float ky) {
    float a = xf.a, b = xf.b, c = xf.c, d = xf.d;
    xf.a = a + c * ky; xf.b = b + d * ky; xf.c = c + a * kx; xf.d = d + b * kx;
    xf_fix();
}
void gfx_origin(void) { xf = (xf_t){ 0, 0, 1, 1, 1, 0, 0, 1, 0 }; xf_n = 0; }
void gfx_identity(void) { xf = (xf_t){ 0, 0, 1, 1, 1, 0, 0, 1, 0 }; }
void gfx_get_xf(float *t) { t[0] = xf.tx; t[1] = xf.ty; t[2] = xf.sx; t[3] = xf.sy; }

void gfx_pixel(float x, float y, uint32_t color) {
    float X, Y; tx(x, y, &X, &Y);
    int xx = (int)floorf(X), yy = (int)floorf(Y);
    dirty(yy, yy + 1);
    px_a(xx, yy, rgb565(color), 255);
}
uint32_t gfx_get_pixel(int x, int y) {
    if (x < 0 || y < 0 || x >= g_tgt->w || y >= g_tgt->h) return 0;
    uint16_t p = g_tgt->px[(size_t)y * g_tgt->w + x];
    uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    return (r << 19 | (r >> 2) << 16) | (g << 10 | (g >> 4) << 8) | (b << 3 | b >> 2);
}
