// nv_jpeg_util.h — the pure parts of the ABI v19 JPEG import (nv_wasm_jpeg.cpp): a strict header probe
// that says whether the P4's hardware decoder can take a stream, and the area-average scaler that brings
// the decoded picture to the exact size the app asked for. Header-only, no ESP-IDF: tests/host checks
// both (unit + fuzz), since the bytes come from the app.
//
// The probe walks the marker segments by their lengths (a search for FF C0, as jpeg_decoder_get_info
// does, can hit entropy-coded data of a progressive file) up to the first frame header. The hardware
// takes baseline DCT (SOF0), 8-bit, grey (1x1) or YCbCr with Y sampled 1x1, 2x1 or 2x2 and Cb/Cr 1x1.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Results, mirrored in sdk/include/nucleo_sdk.h (NV_JPEG_E_*).
enum {
    NV_JPEG_OK = 0,
    NV_JPEG_E_PERM = -1,      // the app has no "gfx"
    NV_JPEG_E_ARG = -2,       // empty input, w/h outside 1..4096, out smaller than w*h*2 or odd
    NV_JPEG_E_UNSUP = -3,     // not a baseline JPEG the hardware decodes (progressive, 12-bit, CMYK,
                              // other sampling), or bigger than 4096 on a side
    NV_JPEG_E_NOMEM = -4,     // no room for the decoded picture
    NV_JPEG_E_DECODE = -5,    // the stream is damaged, or the decoder did not finish
};

#define NV_JPEG_MAX_SIDE 4096

typedef struct {
    int w, h;           // picture size
    int nf;             // components: 1 grey, 3 YCbCr
    int mcu_w, mcu_h;   // the decoder writes whole MCUs: rows of round_up(w, mcu_w) pixels,
                        // round_up(h, mcu_h) rows
} nv_jpeg_hdr_t;

static inline int nvj_u16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

// NV_JPEG_OK with *h filled when the hardware can decode it; NV_JPEG_E_UNSUP (with w/h filled when a
// frame header was found) when it can't; NV_JPEG_E_DECODE when the header is malformed or cut short.
static inline int nv_jpeg_probe(const uint8_t *p, size_t n, nv_jpeg_hdr_t *h) {
    h->w = h->h = h->nf = h->mcu_w = h->mcu_h = 0;
    if (!p || n < 4 || p[0] != 0xFF || p[1] != 0xD8) return NV_JPEG_E_DECODE;
    size_t i = 2;
    for (;;) {
        while (i < n && p[i] == 0xFF) i++;           // fill bytes before a marker
        if (i >= n || i < 3 || p[i - 1] != 0xFF) return NV_JPEG_E_DECODE;
        const int m = p[i++];
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;   // no length
        if (m == 0xD9 || m == 0xDA) return NV_JPEG_E_DECODE;                // EOI / SOS before SOF
        if (i + 2 > n) return NV_JPEG_E_DECODE;
        const int len = nvj_u16(p + i);
        if (len < 2 || i + (size_t)len > n) return NV_JPEG_E_DECODE;
        const bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
        if (!sof) { i += (size_t)len; continue; }
        // Frame header: P, Y, X, Nf, then Nf x (C, HV, Tq)
        if (len < 8) return NV_JPEG_E_DECODE;
        const uint8_t *s = p + i + 2;
        const int prec = s[0], nf = s[5];
        h->h = nvj_u16(s + 1);
        h->w = nvj_u16(s + 3);
        h->nf = nf;
        if (len < 8 + 3 * nf) return NV_JPEG_E_DECODE;
        if (m != 0xC0 || prec != 8 || h->w <= 0 || h->h <= 0 || h->w > NV_JPEG_MAX_SIDE ||
            h->h > NV_JPEG_MAX_SIDE)
            return NV_JPEG_E_UNSUP;
        if (nf == 1) {   // grey: decoded as 8-bit grey (the hardware makes no RGB of it), 8x8 MCUs
            if (s[7] != 0x11) return NV_JPEG_E_UNSUP;
            h->mcu_w = h->mcu_h = 8;
            return NV_JPEG_OK;
        }
        if (nf != 3) return NV_JPEG_E_UNSUP;          // CMYK / YCCK / two components
        const int hv0 = s[7], hv1 = s[10], hv2 = s[13];
        if (hv1 != 0x11 || hv2 != 0x11) return NV_JPEG_E_UNSUP;
        if (hv0 == 0x11)      { h->mcu_w = 8;  h->mcu_h = 8; }
        else if (hv0 == 0x21) { h->mcu_w = 16; h->mcu_h = 8; }
        else if (hv0 == 0x22) { h->mcu_w = 16; h->mcu_h = 16; }
        else return NV_JPEG_E_UNSUP;
        return NV_JPEG_OK;
    }
}

// Scratch the scaler needs for a dw-wide output: uint32_t words.
static inline size_t nv_scale_scratch_words(int dw) { return 6u * (size_t)dw; }

// Source row y (0 <= y < sh), asked for in non-decreasing order; NULL stops the scale (output undefined).
typedef const void *(*nv_scale_row_fn)(void *ctx, int y);

// Area-average scale to exactly dw x dh RGB565 (rows of dw pixels): each output pixel is the mean of the
// source pixels it covers (at least one: enlarging repeats pixels). The source is sw x sh visible pixels,
// each row from row(ctx, y): RGB565 (bpp 2; the 565 bit order as given, channels only summed) or 8-bit
// grey (bpp 1, made into grey RGB565). Reads each source row once, in order (a caller can stream rows
// into fast memory); `scr` holds nv_scale_scratch_words(dw) words. 32-bit arithmetic only (sides <=
// 4096): no division in the pixel loops, one reciprocal per output column for each of the (at most two)
// source row counts. False when row() returned NULL.
static inline bool nv_scale_area_rows(nv_scale_row_fn row_of, void *ctx, int bpp, int sw, int sh, uint16_t *dst,
                                      int dw, int dh, uint32_t *scr) {
    uint32_t *acc = scr, *span = scr + 3 * dw, *inv = scr + 4 * dw;   // inv: two tables of dw
    int inv_cy[2] = {0, 0}, next = 0, max_cx = 1;
    for (int ox = 0; ox < dw; ox++) {
        const int x0 = ox * sw / dw;
        int x1 = (ox + 1) * sw / dw;
        if (x1 <= x0) x1 = x0 + 1;
        if (x1 - x0 > max_cx) max_cx = x1 - x0;
        span[ox] = (uint32_t)x0 | (uint32_t)x1 << 16;
    }
    // RGB565 fast path: (v | v << 16) & 0x07E0F81F spreads a pixel's channels apart in one word (B bits
    // 0-9, R 11-20, G 21-31 once summed), so up to 32 pixels add up in a single 32-bit add without
    // carrying into each other: every downscale up to ~5.6x per side, and every enlargement.
    const int max_cy = (sh + dh - 1) / dh;
    const bool packed = bpp == 2 && max_cx * max_cy <= 32;
    for (int oy = 0; oy < dh; oy++) {
        const int y0 = oy * sh / dh;
        int y1 = (oy + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int i = 0; i < 3 * dw; i++) acc[i] = 0;
        for (int sy = y0; sy < y1; sy++) {
            const void *rp = row_of(ctx, sy);
            if (!rp) return false;
            if (packed) {
                const uint16_t *row = (const uint16_t *)rp;
                for (int ox = 0; ox < dw; ox++) {
                    const int x1 = (int)(span[ox] >> 16);
                    uint32_t s = 0;
                    for (int sx = (int)(span[ox] & 0xFFFF); sx < x1; sx++) {
                        const uint32_t v = row[sx];
                        s += (v | v << 16) & 0x07E0F81Fu;
                    }
                    acc[ox] += s;
                }
            } else if (bpp == 2) {
                const uint16_t *row = (const uint16_t *)rp;
                uint32_t *a = acc;
                for (int ox = 0; ox < dw; ox++, a += 3) {
                    const int x1 = (int)(span[ox] >> 16);
                    uint32_t r = 0, g = 0, b = 0;
                    for (int sx = (int)(span[ox] & 0xFFFF); sx < x1; sx++) {
                        const uint32_t v = row[sx];
                        r += v >> 11;
                        g += (v >> 5) & 63;
                        b += v & 31;
                    }
                    a[0] += r; a[1] += g; a[2] += b;
                }
            } else {
                const uint8_t *row = (const uint8_t *)rp;
                for (int ox = 0; ox < dw; ox++) {
                    const int x1 = (int)(span[ox] >> 16);
                    uint32_t v = 0;
                    for (int sx = (int)(span[ox] & 0xFFFF); sx < x1; sx++) v += row[sx];
                    acc[ox] += v;
                }
            }
        }
        // 1 / count as 2^24 / count: a mean is (sum * inv + 2^23) >> 24 (sum <= 255 * count: fits 32 bits)
        const int cy = y1 - y0;
        uint32_t *iv;
        if (cy == inv_cy[0]) {
            iv = inv;
        } else if (cy == inv_cy[1]) {
            iv = inv + dw;
        } else {
            iv = inv + next * dw;
            inv_cy[next] = cy;
            next ^= 1;
            for (int ox = 0; ox < dw; ox++)
                iv[ox] = (1u << 24) / ((uint32_t)((span[ox] >> 16) - (span[ox] & 0xFFFF)) * (uint32_t)cy);
        }
        uint16_t *out = dst + (size_t)oy * dw;
        if (packed) {
            for (int ox = 0; ox < dw; ox++) {
                const uint32_t s = acc[ox], k = iv[ox];
                const uint32_t r = (s >> 11) & 0x3FF, g = s >> 21, b = s & 0x3FF;
                out[ox] = (uint16_t)(((r * k + (1u << 23)) >> 24) << 11 | ((g * k + (1u << 23)) >> 24) << 5 |
                                     ((b * k + (1u << 23)) >> 24));
            }
        } else if (bpp == 2) {
            const uint32_t *a = acc;
            for (int ox = 0; ox < dw; ox++, a += 3) {
                const uint32_t k = iv[ox];
                out[ox] = (uint16_t)(((a[0] * k + (1u << 23)) >> 24) << 11 | ((a[1] * k + (1u << 23)) >> 24) << 5 |
                                     ((a[2] * k + (1u << 23)) >> 24));
            }
        } else {
            for (int ox = 0; ox < dw; ox++) {
                const uint32_t g = (acc[ox] * iv[ox] + (1u << 23)) >> 24;
                out[ox] = (uint16_t)((g >> 3) << 11 | (g >> 2) << 5 | (g >> 3));
            }
        }
    }
    return true;
}

// The same from a picture in one buffer, rows `pitch` pixels apart.
typedef struct { const uint8_t *base; size_t stride; } nv_scale_plain_t;
static inline const void *nv_scale_plain_row(void *ctx, int y) {
    const nv_scale_plain_t *p = (const nv_scale_plain_t *)ctx;
    return p->base + (size_t)y * p->stride;
}
static inline void nv_scale_area(const void *src, int bpp, int sw, int sh, int pitch, uint16_t *dst, int dw,
                                 int dh, uint32_t *scr) {
    nv_scale_plain_t p = {(const uint8_t *)src, (size_t)pitch * bpp};
    nv_scale_area_rows(nv_scale_plain_row, &p, bpp, sw, sh, dst, dw, dh, scr);
}

#ifdef __cplusplus
}
#endif
