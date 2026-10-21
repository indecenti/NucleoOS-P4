// book_check_stubs.c — what the engine's tools on the PC (book_check, hyph_test) need from the app
// and the system without a screen: the imports of gfx.c and the theme's colours.
#include <stdint.h>

#include "bk.h"

uint16_t g_col[C_COUNT];
int32_t nv_gfx_width(void) { return 1024; }
int32_t nv_gfx_height(void) { return 600; }
void nv_gfx_persist(int32_t on) { (void)on; }
int32_t nv_millis(void) { return 0; }
void nv_gfx_blit_raw(const void *px, int32_t len, int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)px; (void)len; (void)x; (void)y; (void)w; (void)h;
}
