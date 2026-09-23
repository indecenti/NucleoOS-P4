// gfxbench — timing of the host-side gfx primitives (ABI v2) a game frame goes through. Shapes are
// large so the host's pixel loops dominate, not the guest->host call; nothing is presented.
// Build:  .\sdk\build_app.ps1 -AppDir apps\gfxbench
// Run:    POST /api/app/run?id=gfxbench  (one line per primitive + the total)
#include "nucleo_sdk.h"

enum { BW = 96, BH = 64 };
static uint16_t g_sprite[BW * BH];   // RGB565 test image for the blit pass

typedef void (*pass_fn)(int w, int h);

static void p_clear(int w, int h)  { (void)w; (void)h; for (int i = 0; i < 60; i++) nv_gfx_clear(NV_RGB(i * 4, 20, 40)); }
static void p_rect(int w, int h)   { for (int i = 0; i < 400; i++) nv_gfx_rect(i % (w - 200), i % (h - 150), 200, 150, NV_RGB(200, i, 60)); }
static void p_line(int w, int h)   { for (int i = 0; i < 3000; i++) nv_gfx_line(0, i % h, w - 1, h - 1 - i % h, NV_RGB(i, 200, 90)); }
static void p_circle(int w, int h) { for (int i = 0; i < 400; i++) nv_gfx_circle(w / 2, h / 2, 40 + i % 100, NV_RGB(90, i, 220)); }
static void p_tri(int w, int h)    { for (int i = 0; i < 400; i++) nv_gfx_tri(0, h - 1, w / 2, i % h, w - 1, h - 1, NV_RGB(i, 90, 200)); }
static void p_text(int w, int h)   { for (int i = 0; i < 300; i++) nv_gfx_text(i % (w - 300), i % (h - 20), "NucleoOS gfx bench 0123", NV_RGB(250, 250, 250), 2); }
static void p_blit(int w, int h)   { for (int i = 0; i < 600; i++) nv_gfx_blit(g_sprite, BW, BH, i % (w - BW), (i * 7) % (h - BH)); }

NV_EXPORT("run")
void run(void) {
    const int w = nv_gfx_width(), h = nv_gfx_height();
    for (int i = 0; i < BW * BH; i++) g_sprite[i] = (uint16_t)NV_RGB(i, i >> 3, 255 - i);

    static const struct { const char *name; pass_fn fn; } P[] = {
        {"clear", p_clear}, {"rect", p_rect}, {"line", p_line}, {"circle", p_circle},
        {"tri", p_tri},     {"text", p_text}, {"blit", p_blit},
    };
    int32_t total = 0;
    for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++) {
        const int32_t t0 = nv_millis();
        P[i].fn(w, h);
        const int32_t ms = nv_millis() - t0;
        total += ms;
        nv_printf("%s: %d ms", P[i].name, ms);
    }
    nv_printf("total: %d ms (canvas %dx%d)", total, w, h);
}
