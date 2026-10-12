// fuzz_img.c — libFuzzer target for the pictures of the Biblioteca app (img.c): the files come from
// downloaded ZIM collections, so any bytes must decode or fail cleanly — WebP, PNG, JPEG, GIF, BMP,
// and SVG with the <use>/currentColor rewriting done before NanoSVG. The first two bytes pick the
// target size. Built and run by test/fuzz.sh (clang, ASan, UBSan).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bib.h"

// img.c draws through gfx.c; the fuzzer only decodes
int g_w = 64, g_h = 64;
uint16_t **g_rows;
void gfx_mask(const uint8_t *m, int mw, int mh, int x, int y, uint16_t c) { (void)m; (void)mw; (void)mh; (void)x; (void)y; (void)c; }
void gfx_row(int y, int x, const uint16_t *px, const uint8_t *a, int w) { (void)y; (void)x; (void)px; (void)a; (void)w; }
void gfx_dirty(int y0, int y1) { (void)y0; (void)y1; }

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n < 3) return 0;
    const int w = 1 + d[0] % 96, h = 1 + d[1] % 96;
    uint8_t *rgba = img_decode_rgba(d + 2, n - 2, w, h);
    if (rgba) {
        volatile uint8_t sink = 0;   // every pixel of the result is readable
        for (size_t i = 0; i < (size_t)w * h * 4; i += 61) sink ^= rgba[i];
        (void)sink;
        free(rgba);
    }
    return 0;
}
