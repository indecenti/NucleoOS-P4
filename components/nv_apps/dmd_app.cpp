// dmd_app — "DMD Flipper": the board as the external DMD of a virtual pinball cabinet. The PC (VPinMAME /
// VPX through dmdext) streams the DMD to nv_dmd over Wi-Fi (dmdext's "networkstream") or USB (as a
// ZeDMD), and this app draws every frame as LED/plasma dots: over the whole panel (rectangular dots) or
// with square dots at the original proportions.
//
// Picture: an nv_disp layer (the panel's rows the dots use) drawn into the back buffer while a frame is
// presented — tear-free, at vsync, only when the picture changed. See "How a frame reaches the panel".
//
// While frames flow nothing else is on screen and touch does nothing: no shade, no Home / Recents
// gesture. Holding the screen for 2 s (or Esc / Enter on a keyboard) opens the status and settings
// panel. With no frame for 5 s: the clock in dots (once a PC has shown something since the app opened,
// if enabled), else the waiting page with the address to put in DmdDevice.ini. The panel also has a test
// screen, to check the cabinet's orientation and colours without a PC.
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_config.h"
#include "nv_2d.h"
#include "nv_disp.h"
#include "nv_dmd.h"
#include "dmd_core.h"
#include "dmd_menu.h"
#include "nv_fonts.h"
#include "nv_i18n.h"
#include "nv_log.h"
#include "nv_mem_attr.h"
#include "nv_icons.h"
#include "nv_pins.h"
#include "nv_theme.h"
#include "nv_ui.h"
#include "nv_ui_focus.h"
#include "nv_ui_kit.h"
#include "nv_ui_scale.h"
#include "nv_wifi.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

constexpr int kScrW = NV_LCD_H_RES;            // the physical panel: the layer's coordinates
constexpr int kScrH = NV_LCD_V_RES;
constexpr int64_t kIdleUs = 5000000;           // no frame for 5 s: back to the waiting page
constexpr uint32_t kHoldMs = 2000;             // press this long for the panel: no accidental opens
constexpr int32_t kColors[] = {-1, 0xFF4500, 0xFF0000, 0xFFC000, 0x20FF40, 0x00C8FF, 0xFFFFFF};
constexpr int kNColors = sizeof kColors / sizeof kColors[0];
constexpr const char *kAppId = "dmd";

// All state lives in PSRAM .bss (task context only, no ISR / DMA): internal SRAM is the scarce tier.

// ---- settings (written on the LVGL thread, read by the display task)
NV_PSRAM_BSS volatile int s_rot = 0;            // 0..3 = 0/90/180/270 degrees clockwise
NV_PSRAM_BSS volatile int s_shape = 0;          // DMD_SHAPE_ROUND
NV_PSRAM_BSS volatile bool s_glow = false;
NV_PSRAM_BSS volatile int s_bright = 0;         // % (load_cfg)
NV_PSRAM_BSS volatile uint32_t s_look_gen = 0;  // bumped on shape / glow / rotation / fit: the layer re-attaches
NV_PSRAM_BSS int s_color_idx = 0;               // (load_cfg)
NV_PSRAM_BSS int s_port = 0;                    // (load_cfg)
// DMD_FIT_FILL / DMD_FIT_SQUARE, or kFitBackbox: square dots along the bottom, the running table's
// backglass above them (a pinball backbox: art on top, the DMD in the speaker panel)
constexpr int kFitBackbox = 2;
constexpr int kFitBackglass = 3;   // the board is the backglass screen: the art only, no DMD
// FullDMD: the table's art made for a big DMD screen (from its .directb2s; dmd_find_window checks it has
// a window for the DMD) on the panel's top, the DMD alone in a band along the bottom: the whole width and
// kFullBandH tall, a little more than the 4:1 picture (its rows spread, the dots stay round)
constexpr int kFitFullDmd = 4;
constexpr int kFullBandH = 320;
// Tall DMD: the DMD alone, centred, the whole width and kFullBandH tall (a little taller than the 4:1
// picture, like the FullDMD band), black above and below
constexpr int kFitTall = 5;
// Rebuilt DMD: the whole panel. A 4:1 picture on a 1.7:1 panel must grow ~2.3x in height; most of it is
// done by rebuilding rows with Scale2x (dmd_rebuild: 32 -> 64, a 128x16 DMD 16 -> 64), which follows the
// outlines instead of stretching dots, and the rest (+17%) only spaces the dot rows out (round 8 px dots,
// rows 9-10 px apart): 128x64 dots over all 1024x600, no black bands
constexpr int kFitRebuilt = 6;
// Art with its window: the table's FullDMD art on the whole panel and the DMD's dots drawn inside the dark
// window the art leaves for them (dmd_find_window), the original look of a FullDMD screen
constexpr int kFitFullWin = 7;
// Backglass with its housing: the backglass of a 2-screen .directb2s with its grill, whole on the panel,
// the DMD drawn in the housing the grill leaves for it
constexpr int kFitB2sWin = 8;
constexpr int kFitLast = kFitB2sWin;
// the fits that draw the DMD inside a window of a picture
constexpr bool fit_in_win(int f) { return f == kFitFullWin || f == kFitB2sWin; }
NV_PSRAM_BSS volatile int s_fit = DMD_FIT_SQUARE;   // in use: the running table's own fit, else s_fit_cfg
NV_PSRAM_BSS int s_fit_cfg = DMD_FIT_SQUARE;        // the panel's "Fit" (every table)
NV_PSRAM_BSS int s_fit_gid = -1, s_fit_tbl = -1;    // the table whose own fit is cached, and that fit (-1 none)
NV_PSRAM_BSS volatile bool s_full_ready = false;  // that art and its window are there (LVGL thread)
NV_PSRAM_BSS dmd_rect_t s_win{};                  // the window (LVGL thread writes, then bumps the gen)
NV_PSRAM_BSS volatile uint32_t s_win_gen = 0;
// The layer changed rectangle (the full-screen clock, then a DMD band; one fit, then another): the panel
// rows it no longer covers still hold its old pixels until LVGL paints them, so the LVGL task repaints
// the whole screen once (set by the display task, cleared by tick)
NV_PSRAM_BSS volatile bool s_repaint = false;
NV_PSRAM_BSS volatile bool s_in_win = false;     // the layer is the art's window: drawn straight, art kept
NV_PSRAM_BSS volatile bool s_x2 = false;         // the layer draws the rebuilt frame (twice the rows)
NV_PSRAM_BSS uint32_t *s_px2 = nullptr;          // it, and its scratch (heap, PSRAM, w*2h values each,
NV_PSRAM_BSS uint32_t *s_tmp2 = nullptr;         // only while needed)
NV_PSRAM_BSS size_t s_px2_cap = 0;               // values each holds
// Rows lit since the game started (LVGL task: tick resets them, the layer's draw widens them): a 128x16 DMD
// that dmdext centres in 128x32 never lights the outer quarters, so only its 16 rows are rebuilt (4x)
NV_PSRAM_BSS int s_lit_lo = -1, s_lit_hi = -1;
NV_PSRAM_BSS bool s_rb_on = true;   // the running table rebuilds a 128x16 DMD (per table, "dmdrb_<id>")
// The running table's dots ("dmddot_<id>"): bit 0 original (square, whole pixels, never stretched), bits 1-2
// a margin of 0-3 rows and columns of unlit dots around the DMD. s_mg: the margin the layer draws now (the
// display task sets it with the geometry), s_pxm: the frame with that margin (heap, while needed).
NV_PSRAM_BSS volatile int s_dot = 0;
NV_PSRAM_BSS volatile int s_mg = 0;
NV_PSRAM_BSS uint32_t *s_pxm = nullptr;
NV_PSRAM_BSS size_t s_pxm_cap = 0;
NV_PSRAM_BSS volatile bool s_art_ready = false;  // the backglass of the running table is there (LVGL thread)
NV_PSRAM_BSS lv_obj_t *s_art = nullptr;          // it, above the DMD (backbox)
NV_PSRAM_BSS bool s_art_screen = false;           // backglass only, in game: the art is the whole screen
NV_PSRAM_BSS bool s_clock = false;              // the idle clock (load_cfg)

// ---- display task
NV_PSRAM_BSS TaskHandle_t s_task = nullptr;
NV_PSRAM_BSS volatile bool s_run = false;
NV_PSRAM_BSS SemaphoreHandle_t s_task_done = nullptr;
NV_PSRAM_BSS volatile bool s_panel_open = false;
NV_PSRAM_BSS volatile bool s_showing = false;      // the layer is attached: the waiting page is hidden
NV_PSRAM_BSS volatile int s_big_w = 0, s_big_h = 0;   // a DMD that does not fit the panel (shown on the page)
// Geometry the layer draws with: the task sets it while the layer is detached; draw reads it.
NV_PSRAM_BSS dmd_geom_t s_geom{};
NV_PSRAM_BSS int s_lw = 0, s_lh = 0, s_lrot = 0;
NV_PSRAM_BSS dmd_masks_t *s_masks = nullptr;       // the four dot shapes, PSRAM

// ---- UI
NV_PSRAM_BSS lv_obj_t *s_root = nullptr;
NV_PSRAM_BSS lv_obj_t *s_page = nullptr;           // waiting page
NV_PSRAM_BSS lv_obj_t *s_page_url = nullptr, *s_page_msg = nullptr;
NV_PSRAM_BSS lv_obj_t *s_panel = nullptr;          // status + settings
NV_PSRAM_BSS lv_obj_t *s_status = nullptr;
NV_PSRAM_BSS lv_obj_t *s_port_ta = nullptr;
NV_PSRAM_BSS lv_timer_t *s_timer = nullptr;
NV_PSRAM_BSS uint32_t s_press_t = 0;
NV_PSRAM_BSS bool s_hold_fired = false;
NV_PSRAM_BSS int64_t s_started_us = 0;
// Pictures made on the board: the idle clock and the test screen (PSRAM).
constexpr int kLocalW = 128, kLocalH = 32;      // the test screen: a classic 128x32 DMD
// The clock: always round dots over the whole panel, whatever "Fit" says. 128x75 dots of 8 px = 1024x600
// with square cells (DMD_FIT_SQUARE), so nothing is stretched.
constexpr int kClockW = NV_LCD_H_RES / 8, kClockH = NV_LCD_V_RES / 8;
NV_PSRAM_BSS uint32_t *s_local = nullptr;
NV_PSRAM_BSS int64_t s_test_until_us = 0;       // the test screen is shown until then
NV_PSRAM_BSS int s_clock_shown = -1;            // minute*2 + colon phase last drawn (-1: none)
NV_PSRAM_BSS bool s_menu_on = false;            // the Popper menu is on screen (dmd_menu)
NV_PSRAM_BSS int s_theme = 0;                   // the menu's look: 0 cabinet, 1 DMD ("dmd_theme")
NV_PSRAM_BSS int s_sys_locked = -1;             // system gestures locked (the DMD on screen); -1 unknown
NV_PSRAM_BSS volatile bool s_menu_ready = false; // the menu wants the screen: the idle clock steps aside

const nv_shortcut_t kKeys[] = {   // key names as the system sheet writes them (nv_ui_shortcuts.cpp)
    {"Invio", "Stato e impostazioni del DMD", "DMD status and settings"},
    {"Esc", "Apri / chiudi il pannello", "Open / close the panel"},
};

// ================================================================== layer

uint32_t layer_latest(void *) { return nv_dmd_latest(); }

// How a frame reaches the panel (measured on the board, 128x32 with square 8 px dots, CPU at 360 MHz):
//   * every dot comes from a cache of finished dots, one per colour and dot shape (dmd_sprites_t): a dot
//     row costs a few 32-bit copies instead of a table look-up per pixel (a 512 KB band: 2.2 ms, not 15);
//   * it is drawn in strips in internal SRAM and copied into the frame by the AXI-GDMA while the next
//     strip is drawn: the CPU writing PSRAM itself reads every cache line first (8.6 ms for the band),
//     and the DMA's cost is mostly per transfer (16 KB strips: 8 ms, 32 KB: 4.8 ms for the band, ~0.2 ms a
//     copy), so strips are as big as internal SRAM allows, up to 64 KB: whole dot rows, as many as fit.
// Result: 4.5-5 ms per frame for the 1024x256 band. The two strips exist only while the DMD is on screen;
// without them (no SRAM, dots taller than 23 px) the dots are drawn straight into the frame.
NV_PSRAM_BSS uint16_t *s_strip[2] = {};
NV_PSRAM_BSS SemaphoreHandle_t s_strip_done[2] = {};
NV_PSRAM_BSS int s_strip_cells = 0;                 // dot rows per strip, 0 = no strips
NV_PSRAM_BSS dmd_sprites_t *s_sprites = nullptr;   // PSRAM; reset whenever the masks are rebuilt
// What to draw in strips: a geometry (the DMD's, or an ambient band's), where its row 0 goes in the frame,
// the picture, its rotation, and whether the dot caches help (a band's many shades would only thrash them).
struct Job {
    const dmd_geom_t *g;
    uint16_t *band;
    const uint32_t *px;
    int w, h, rot;
    bool cached;
};

void strips_free(void) {
    for (uint16_t *&b : s_strip) { heap_caps_free(b); b = nullptr; }
    s_strip_cells = 0;
}

// Display task, layer detached. A strip holds whole dot rows, each at most ph + 1 panel rows tall: as
// many as fit in kStripMax (8 px dots: 3 rows, 55 KB, so a 1024x512 picture is 22 copies, not 64), fewer
// when internal SRAM is short.
constexpr size_t kStripMax = 64u << 10;
void strips_alloc(const dmd_geom_t &g) {
    strips_free();
    const size_t row_bytes = (size_t)kScrW * 2 * (size_t)(g.ph + 1);
    for (int cells = (int)(kStripMax / row_bytes); cells >= 1; cells--) {
        const size_t bytes = row_bytes * (size_t)cells;
        bool ok = true;
        for (int i = 0; i < 2 && ok; i++) {
            if (!s_strip_done[i]) s_strip_done[i] = xSemaphoreCreateBinary();
            s_strip[i] = (uint16_t *)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            ok = s_strip[i] && s_strip_done[i];
        }
        if (ok) {
            s_strip_cells = cells;
            return;
        }
        strips_free();
    }
}

// Strips drawn in turn into the two SRAM buffers, each copied by the DMA while the next one is drawn. (One
// core: splitting them with a helper on core 0 gained ~3% on a 1024x600 picture, the copies into PSRAM
// being most of the cost, and its strips showed another frame's dots: not worth it.)
void draw_strips(const Job &j) {
    const dmd_geom_t &g = *j.g;
    bool busy[2] = {false, false};
    int b = 0;
    for (int cy = 0; cy < g.rows; cy += s_strip_cells, b ^= 1) {
        const int cy1 = cy + s_strip_cells <= g.rows ? cy + s_strip_cells : g.rows;
        const int y0 = dmd_cell_y(&g, cy), y1 = dmd_cell_y(&g, cy1);
        const size_t bytes = (size_t)(y1 - y0) * kScrW * 2;
        if (busy[b] && nv_2d_copy_wait(s_strip_done[b], 100) != ESP_OK) busy[b] = false;
        dmd_render_rows(s_strip[b], kScrW, &g, s_masks, j.px, j.w, j.h, j.rot, s_bright, true, kScrW, cy, cy1,
                        j.cached ? s_sprites : nullptr);
        uint16_t *to = j.band + (size_t)y0 * kScrW;
        busy[b] = nv_2d_copy_start(to, s_strip[b], bytes, s_strip_done[b]) == ESP_OK;
        if (!busy[b]) memcpy(to, s_strip[b], bytes);   // no DMA (retired): the CPU copies
    }
    for (int i = 0; i < 2; i++)
        if (busy[i]) nv_2d_copy_wait(s_strip_done[i], 100);
}

// ---- ambient bands ("DMD only", "Tall DMD"): the rest of the panel in dots too, lit by the DMD's light
// (dmd_ambient / dmd_ambient_band), the DMD's own unlit dots in the same faint colour so it is all one panel.
// A margin row of unlit dots next to the DMD; the light fades within kAmbLit rows after it, so only those
// rows (the "lit" grids) are drawn again when it changes, the rest once. The light follows the scene
// smoothly, at most kAmbHz times a second, rounded to 16 levels a channel, and each frame buffer remembers
// what it shows: a still scene costs nothing.
constexpr int kAmbGap = 1, kAmbLit = 8, kAmbHz = 10;
struct Amb {
    dmd_geom_t top, bot;               // the bands' grids: the DMD's columns and dot rows, carried on
    dmd_geom_t top_lit, bot_lit;       // their rows next to the DMD (margin + lit ones)
    int rows_t, rows_b, lit_t, lit_b;
    int64_t next_us;                   // when the light may change again
    uint32_t floor;                    // an unlit dot
    uint16_t *fb[2];                   // the two frame buffers, and the light each one shows
    uint32_t shown[2][2 * DMD_MAX_W];
    uint32_t want[2 * DMD_MAX_W];      // the light now: top columns, then bottom ones (rounded)
    uint16_t ema[2 * DMD_MAX_W][3];    // its smoothed channels, x256
    uint32_t raw[2 * DMD_MAX_W], scratch[DMD_MAX_W];
    uint32_t *frame;                   // the DMD with its unlit dots in the floor colour (w*h)
    uint32_t *band;                    // a band's picture (cols x rows)
    size_t frame_cap, band_cap;
};
NV_PSRAM_BSS Amb *s_amb = nullptr;        // heap; only while a fit with bands is on screen (display task)

void amb_free(void) {
    if (!s_amb) return;
    heap_caps_free(s_amb->frame);
    heap_caps_free(s_amb->band);
    heap_caps_free(s_amb);
    s_amb = nullptr;
}

// Display task, layer detached: the bands of geometry g (whole dot rows above and below it), or none.
bool amb_setup(const dmd_geom_t &g, int w, int h, uint32_t mono) {
    amb_free();
    if (g.ph < 1 || g.oy < g.ph) return false;   // no room for a dot row
    Amb *a = (Amb *)heap_caps_calloc(1, sizeof(Amb), MALLOC_CAP_SPIRAM);
    if (!a) return false;
    a->rows_t = g.oy / g.ph;
    a->rows_b = (kScrH - (g.oy + g.gh)) / g.ph;
    for (int i = 0; i < 2; i++) {
        dmd_geom_t &b = i ? a->bot : a->top;
        b = g;
        b.rows = i ? a->rows_b : a->rows_t;
        b.gh = b.rows * g.ph;
        b.oy = i ? g.oy + g.gh : g.oy - b.gh;
        for (int r = 0; r <= b.rows; r++) b.y[r] = (int16_t)(r * g.ph);
    }
    a->lit_t = a->rows_t < kAmbGap + kAmbLit ? a->rows_t : kAmbGap + kAmbLit;
    a->lit_b = a->rows_b < kAmbGap + kAmbLit ? a->rows_b : kAmbGap + kAmbLit;
    a->top_lit = a->top;   // the top band's last lit_t rows, the bottom band's first lit_b
    a->top_lit.rows = a->lit_t;
    a->top_lit.gh = a->lit_t * g.ph;
    a->top_lit.oy = g.oy - a->top_lit.gh;
    a->bot_lit = a->bot;
    a->bot_lit.rows = a->lit_b;
    a->bot_lit.gh = a->lit_b * g.ph;
    // the unlit dot: ~8% of the monochrome colour (orange when the PC chooses)
    const uint32_t m = mono ? mono : 0xFF5820;
    a->floor = ((m >> 16 & 255) * 20 / 256) << 16 | ((m >> 8 & 255) * 20 / 256) << 8 | ((m & 255) * 20 / 256);
    a->frame_cap = (size_t)w * (size_t)h;
    const int rows = a->rows_t > a->rows_b ? a->rows_t : a->rows_b;
    a->band_cap = (size_t)g.cols * (size_t)rows;
    a->frame = (uint32_t *)heap_caps_malloc(a->frame_cap * 4, MALLOC_CAP_SPIRAM);
    a->band = (uint32_t *)heap_caps_malloc(a->band_cap * 4, MALLOC_CAP_SPIRAM);
    if (!a->frame || !a->band) {
        heap_caps_free(a->frame);
        heap_caps_free(a->band);
        heap_caps_free(a);
        return false;
    }
    s_amb = a;
    return true;
}

// LVGL task, in the layer's draw: the light from this frame, and the bands of buffer fb when it shows
// another light (or never had them: fresh).
void amb_draw(Amb *a, uint16_t *fb, int stride, bool fresh, const uint32_t *px, int w, int h) {
    const int cols = s_geom.cols;
    const int64_t now = esp_timer_get_time();
    if (now >= a->next_us) {   // a new step of the light: 1/3 of the way to this frame's, rounded to 16 levels
        a->next_us = now + 1000000 / kAmbHz;
        dmd_ambient(px, w, h, s_lrot, a->raw, a->raw + cols, a->scratch);
        for (int i = 0; i < 2 * cols; i++) {
            uint32_t q = 0;
            for (int ch = 0; ch < 3; ch++) {
                const int t = (int)(a->raw[i] >> (16 - 8 * ch) & 255) << 8;
                int v = a->ema[i][ch];
                v += (t - v) / 3;
                a->ema[i][ch] = (uint16_t)v;
                q |= (uint32_t)((v >> 8) & 0xF0) << (16 - 8 * ch);
            }
            a->want[i] = q;
        }
    }
    int idx = fb == a->fb[0] ? 0 : fb == a->fb[1] ? 1 : -1;
    if (idx < 0) {   // a buffer not seen yet: take a free slot
        idx = a->fb[0] ? 1 : 0;
        a->fb[idx] = fb;
        fresh = true;
    }
    if (!fresh && !memcmp(a->shown[idx], a->want, (size_t)(2 * cols) * 4)) return;   // the same light
    if (fresh) {   // the rows the dot grids leave over, black once
        const int top_end = a->top.oy, bot_start = a->bot.oy + a->bot.gh;
        if (top_end > 0) memset(fb, 0, (size_t)top_end * (size_t)stride * 2);
        if (bot_start < kScrH) memset(fb + (size_t)bot_start * stride, 0, (size_t)(kScrH - bot_start) * (size_t)stride * 2);
    }
    // fresh: the whole bands as unlit dots first (the light never reaches past the lit rows); then, fresh or
    // not, the rows next to the DMD with the light
    for (int pass = fresh ? 0 : 1; pass < 2; pass++) {
        if (!pass) memset(a->scratch, 0, (size_t)cols * 4);   // no light
        for (int i = 0; i < 2; i++) {
            const dmd_geom_t &b = pass ? (i ? a->bot_lit : a->top_lit) : (i ? a->bot : a->top);
            if (b.rows < 1 || (size_t)cols * (size_t)b.rows > a->band_cap) continue;
            dmd_ambient_band(pass ? a->want + (i ? cols : 0) : a->scratch, cols, b.rows, !i, kAmbGap, a->floor, a->band);
            const Job j = {&b, fb + (size_t)b.oy * stride, a->band, cols, b.rows, 0, false};
            if (s_strip_cells && stride == kScrW) draw_strips(j);
            else dmd_render_rows(j.band, stride, &b, s_masks, a->band, cols, b.rows, 0, s_bright, true, kScrW, 0, b.rows, nullptr);
        }
    }
    memcpy(a->shown[idx], a->want, (size_t)(2 * cols) * 4);
}

uint32_t layer_draw(void *, uint16_t *fb, int stride, bool fresh) {
    int w = 0, h = 0;
    uint32_t gen = 0;
    const uint32_t *px = nv_dmd_acquire(&w, &h, &gen);
    if (!px) return 0;
    uint32_t ok = 0;
    if (w == s_lw && h == s_lh) {   // else the task re-attaches with the new size right away
        uint16_t *band = fb + (size_t)s_geom.oy * stride;
        int lo, hi;   // the rows this game lights (the rebuilt DMD and the per-table suggestion use them)
        if (dmd_lit_rows(px, w, h, &lo, &hi)) {
            if (s_lit_lo < 0 || lo < s_lit_lo) s_lit_lo = lo;
            if (hi > s_lit_hi) s_lit_hi = hi;
        }
        int r0 = 0, n = h;
        if (s_rb_on) dmd_rebuild_rows(h, s_lit_lo, s_lit_hi, &r0, &n);   // a 128x16 DMD: its 16 rows
        if (s_x2 && s_px2 && s_tmp2 && (size_t)w * (size_t)(2 * h) <= s_px2_cap) {   // the rebuilt DMD: 2h rows
            if (dmd_rebuild(px, w, h, r0, n, s_px2, s_tmp2)) {
                px = s_px2;
                h *= 2;
            }
        } else if (n < h && s_px2 && s_tmp2 && (size_t)w * (size_t)h <= s_px2_cap) {   // any other fit: its own h
            if (dmd_rebuild_to(px, w, h, r0, n, s_px2, s_tmp2, h)) px = s_px2;
        }
        const int mg = s_mg;   // the margin: the frame inside a border of unlit dots
        if (mg && s_pxm && (size_t)(w + 2 * mg) * (size_t)(h + 2 * mg) <= s_pxm_cap) {
            const int mw = w + 2 * mg;
            memset(s_pxm, 0, (size_t)mw * (size_t)(h + 2 * mg) * 4);
            for (int y = 0; y < h; y++) memcpy(s_pxm + (size_t)(y + mg) * mw + mg, px + (size_t)y * w, (size_t)w * 4);
            px = s_pxm;
            w = mw;
            h += 2 * mg;
        }
        const uint32_t *orig = px;
        if (s_in_win) {   // inside the art's window: only its pixels (the strips copy whole rows: the art beside)
            dmd_render_rows(band, stride, &s_geom, s_masks, px, w, h, s_lrot, s_bright, false, kScrW, 0, s_geom.rows, s_sprites);
            nv_dmd_release();
            return gen;
        }
        Amb *a = s_amb;
        if (a && (size_t)w * (size_t)h <= a->frame_cap) {   // the DMD's unlit dots: the bands' faint colour
            for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; i++) a->frame[i] = px[i] ? px[i] : a->floor;
            px = a->frame;
        }
        const Job j = {&s_geom, band, px, w, h, s_lrot, true};
        if (s_strip_cells && stride == kScrW) draw_strips(j);
        else dmd_render_rows(band, stride, &s_geom, s_masks, px, w, h, s_lrot, s_bright, true, kScrW, 0, s_geom.rows, s_sprites);
        if (a) amb_draw(a, fb, stride, fresh, orig, w, h);
        ok = gen;
    }
    nv_dmd_release();
    return ok;
}

// Where a fit puts a w x h DMD's dots (rot; dot: the table's dots, see s_dot; win: the picture's window or
// housing for the fits drawn in one). The rebuilt fit draws 2h rows. The display task attaches with it, the
// composition screen previews with it.
bool geom_for(int fit, int w, int h, int rot, int dot, dmd_rect_t win, dmd_geom_t *g) {
    const int m = (dot >> 1) & 3;
    const int fill = dot & 1 ? DMD_FIT_SQUARE : DMD_FIT_FILL;   // original dots: square, whole pixels
    const int gw = w + 2 * m, gh = (fit == kFitRebuilt ? 2 * h : h) + 2 * m;
    bool ok;
    if (fit_in_win(fit)) {   // the picture's window, inset a little (dots never touching its frame)
        const dmd_rect_t in = dmd_inset(win);
        ok = dmd_geom(gw, gh, rot, fill, in.w, in.h, g);
        if (ok) {
            g->ox += in.x;
            g->oy += in.y;
        }
    } else if (fit == kFitRebuilt) {
        ok = dmd_geom(gw, gh, rot, fill, kScrW, kScrH, g);
    } else if (fit == kFitFullDmd || fit == kFitTall) {
        ok = dmd_geom(gw, gh, rot, fill, kScrW, kFullBandH, g);
        if (ok) g->oy = fit == kFitTall ? (kScrH - g->gh) / 2 : kScrH - g->gh;   // the art takes the rest, above
    } else {
        ok = dmd_geom(gw, gh, rot, fit == kFitBackbox || fit == DMD_FIT_SQUARE ? DMD_FIT_SQUARE : fill, kScrW, kScrH, g);
        if (ok && fit == kFitBackbox) g->oy = kScrH - g->gh;
    }
    return ok;
}

// Wakes on every new frame (or every 50 ms) and drives the layer: attach with the current geometry,
// detach while the panel, the shade or the classic title bar covers the screen or the PC is silent.
void disp_task(void *) {
    bool attached = false;
    uint32_t look = 0;
    int fit_used = -1;
    uint32_t win_used = 0;
    int why_shown = -1;   // the last reason logged for the DMD not on screen (only changes are logged)
    while (s_run) {
        const bool fresh = nv_dmd_wait(50);
        if (!s_run) break;
        nv_dmd_status_t st;
        nv_dmd_get_status(&st);
        bool live = nv_dmd_latest() && st.last_frame_us && esp_timer_get_time() - st.last_frame_us < kIdleUs;
        if (s_menu_ready && !strcmp(st.fmt, "clock")) live = false;   // the Popper menu takes over at once
        if (s_fit == kFitBackglass && s_art_ready && strcmp(st.fmt, "clock")) live = false;   // backglass only
        const bool covered = s_panel_open || nv_ui_shade_is_open() || nv_ui_chrome_over_app() || dmd_menu_covering();
        {   // why the DMD is (not) on screen, logged when it changes: frames, panel, shade, title bar, menu
            const int why = (live ? 1 : 0) | (s_panel_open ? 2 : 0) | (nv_ui_shade_is_open() ? 4 : 0) |
                            (nv_ui_chrome_over_app() ? 8 : 0) | (dmd_menu_covering() ? 16 : 0);
            if (why != why_shown && (why & 1 || why_shown & 1)) {
                nv_log_write(NV_LOG_INFO, "dmd", "frames %s%s%s%s%s", why & 1 ? "on" : "off", why & 2 ? ", panel open" : "",
                             why & 4 ? ", shade open" : "", why & 8 ? ", title bar" : "", why & 16 ? ", menu covering" : "");
            }
            why_shown = why;
        }
        if (!live || covered) {
            if (attached) {
                nv_disp_layer_set(nullptr);
                attached = false;
                s_repaint = true;
                strips_free();   // the 64 KB of SRAM only while the DMD is on screen
            }
            s_showing = false;
            continue;
        }
        // The clock keeps its dots round: square cells for it, the user's choice for everything else.
        // backbox: square dots at the bottom, but only with the art to put above them (else centred)
        const bool clock = !strcmp(st.fmt, "clock");
        // FullDMD: in the art's window; without that art, the backbox; with no art at all, the whole screen
        // (never a strip with black bands: the user chose a full screen)
        const int fit = clock ? DMD_FIT_SQUARE
                        : s_fit == kFitFullDmd ? (s_full_ready ? kFitFullDmd : s_art_ready ? kFitBackbox : DMD_FIT_FILL)
                        : fit_in_win(s_fit) ? (s_full_ready ? s_fit : s_art_ready ? kFitBackbox : DMD_FIT_FILL)
                        : s_fit == kFitBackbox ? (s_art_ready ? kFitBackbox : DMD_FIT_SQUARE)
                        : s_fit == kFitBackglass ? DMD_FIT_SQUARE
                        : s_fit == kFitRebuilt ? (2 * st.h <= DMD_MAX_H && st.w * 2 * st.h <= DMD_MAX_PX ? kFitRebuilt : DMD_FIT_SQUARE)
                        : s_fit;   // FILL, SQUARE, kFitTall
        if (!attached || st.w != s_lw || st.h != s_lh || look != s_look_gen || fit != fit_used ||
            ((fit == kFitFullDmd || fit_in_win(fit)) && s_win_gen != win_used)) {
            if (attached) { nv_disp_layer_set(nullptr); attached = false; }   // draw never sees half a geometry
            look = s_look_gen;
            const int rot = s_rot;
            dmd_geom_t g;
            fit_used = fit;
            win_used = s_win_gen;
            if ((size_t)st.w * (size_t)(2 * st.h) > s_px2_cap) {   // every fit may rebuild (detached: no draw running)
                heap_caps_free(s_px2);
                heap_caps_free(s_tmp2);
                const size_t cap = (size_t)st.w * (size_t)(2 * st.h);
                s_px2 = (uint32_t *)heap_caps_malloc(cap * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
                s_tmp2 = (uint32_t *)heap_caps_malloc(cap * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
                s_px2_cap = s_px2 && s_tmp2 ? cap : 0;
            }
            const bool x2 = fit == kFitRebuilt && s_px2_cap;
            // the table's dots: original ones, a margin of unlit dots (only with its frame buffer)
            int dot = clock ? 0 : s_dot;
            const int m = (dot >> 1) & 3;
            const size_t mcap = (size_t)(st.w + 2 * m) * (size_t)((x2 ? 2 : 1) * st.h + 2 * m);
            if (m && mcap > s_pxm_cap) {   // detached: no draw running
                heap_caps_free(s_pxm);
                s_pxm = (uint32_t *)heap_caps_malloc(mcap * 4, MALLOC_CAP_SPIRAM);
                s_pxm_cap = s_pxm ? mcap : 0;
            }
            if (m && !s_pxm_cap) dot &= 1;
            const bool ok = geom_for(x2 || fit != kFitRebuilt ? fit : DMD_FIT_SQUARE, st.w, st.h, rot, dot, s_win, &g);
            if (!ok) {
                s_big_w = st.w;
                s_big_h = st.h;
                s_showing = false;
                continue;
            }
            s_big_w = 0;
            s_big_h = 0;
            s_mg = (dot >> 1) & 3;
            s_x2 = x2;
            s_in_win = fit_in_win(fit);
            // ambient bands where the fit leaves them (not the clock: it fills the panel)
            const bool bands = !clock && (fit == DMD_FIT_SQUARE || fit == kFitTall) && amb_setup(g, st.w + 2 * s_mg, st.h + 2 * s_mg,
                               s_color_idx > 0 && s_color_idx < kNColors && kColors[s_color_idx] >= 0 ? (uint32_t)kColors[s_color_idx] : 0);
            if (!bands) amb_free();
            dmd_masks_build(s_masks, &g, s_shape, s_glow);
            if (s_sprites) dmd_sprites_reset(s_sprites);
            if (s_in_win) strips_free();   // whole-row strips would paint over the art beside the window
            else if (g.ph != s_geom.ph || !s_strip_cells) strips_alloc(g);   // detached: draw is not running
            nv_log_write(NV_LOG_INFO, "dmd", "%dx%d -> %dx%d dots of %dx%d px at %d,%d, %s (%d dot rows a strip)", st.w,
                         st.h, g.cols, g.rows, g.pw, g.ph, g.ox, g.oy,
                         s_strip_cells ? "SRAM strips + DMA" : "straight into the frame", s_strip_cells);
            s_geom = g;
            s_lw = st.w;
            s_lh = st.h;
            s_lrot = rot;
            s_showing = true;
            // with bands the layer is the whole panel but its last row: nv_disp asks LVGL for a frame by
            // painting a corner pixel outside the layer, and one inside would make every draw a fresh one
            // (the whole panel redrawn: 31 ms). That row is the gap under the last band's dots.
            const nv_disp_layer_t layer = s_amb      ? nv_disp_layer_t{0, 0, kScrW, kScrH - 1, layer_latest, layer_draw, nullptr}
                                          : s_in_win ? nv_disp_layer_t{g.ox, g.oy, g.gw, g.gh, layer_latest, layer_draw, nullptr}
                                                     : nv_disp_layer_t{0, g.oy, kScrW, g.gh, layer_latest, layer_draw, nullptr};
            nv_disp_layer_set(&layer);
            attached = true;
            s_repaint = true;
        } else if (fresh) {
            nv_disp_layer_update();
        }
    }
    if (attached) nv_disp_layer_set(nullptr);
    amb_free();
    strips_free();
    s_x2 = false;
    heap_caps_free(s_px2);
    heap_caps_free(s_tmp2);
    s_px2 = s_tmp2 = nullptr;
    s_px2_cap = 0;
    heap_caps_free(s_pxm);
    s_pxm = nullptr;
    s_pxm_cap = 0;
    s_mg = 0;
    s_geom = {};
    s_showing = false;
    xSemaphoreGive(s_task_done);
    vTaskDeleteWithCaps(nullptr);
}

// ================================================================== settings

// ---- "Fit" per table (from the in-game quick actions): nv_config "dmdfit_<Popper id>" = fit + 1
void table_key(char *k, size_t n, int gid) { snprintf(k, n, "dmdfit_%d", gid); }

// The bridge sends a table's FullDMD art as it starts only when asked: always, so the per-table choice
// knows whether this table has one (on the PC it is a small file already converted, read once a game).
void want_fulldmd(void) { dmd_menu_want_fulldmd(true); }

void load_cfg(void) {
    s_rot = nv_config_get_int("dmd_rot", 0) & 3;
    s_shape = nv_config_get_int("dmd_shape", DMD_SHAPE_ROUND) == DMD_SHAPE_SQUARE ? DMD_SHAPE_SQUARE : DMD_SHAPE_ROUND;
    s_theme = nv_config_get_int("dmd_theme", 0) ? 1 : 0;
    s_glow = nv_config_get_bool("dmd_glow", false);
    int b = nv_config_get_int("dmd_bright", 100);
    s_bright = b < 10 ? 10 : b > 100 ? 100 : b;
    const int c = nv_config_get_int("dmd_color", 1);
    s_color_idx = c >= 0 && c < kNColors ? c : 1;
    const int p = nv_config_get_int("dmd_port", NV_DMD_PORT_DEFAULT);
    s_port = p >= 1 && p <= 65535 ? p : NV_DMD_PORT_DEFAULT;
    s_fit_cfg = nv_config_get_int("dmd_fit", DMD_FIT_SQUARE);
    if (s_fit_cfg < DMD_FIT_FILL || s_fit_cfg > kFitLast) s_fit_cfg = DMD_FIT_SQUARE;
    s_fit = s_fit_cfg;
    s_fit_gid = -1;
    want_fulldmd();
    s_clock = nv_config_get_bool("dmd_clock", true);
}

bool boot_enabled(void) {
    char b[16];
    nv_config_get_str("boot_app", "", b, sizeof b);
    return !strcmp(b, kAppId);
}

// A row of pills (one choice). Each pill's user data is its index; the row restyles in place.
void pills_restyle(lv_obj_t *row, int sel) {
    const NvTheme *th = nv_theme_get();
    for (uint32_t i = 0; i < lv_obj_get_child_count(row); i++) {
        lv_obj_t *p = lv_obj_get_child(row, (int32_t)i);
        const bool on = (int)i == sel;
        nv_kit_bg_color(p, on ? th->primary : th->surface3);
        nv_kit_text_color(lv_obj_get_child(p, 0), on ? th->on_primary : th->text_strong);
    }
}

lv_obj_t *pills(lv_obj_t *col, const char *title, const char *options, int sel, lv_event_cb_t cb) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *t = lv_label_create(col);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, th->text_dim, 0);
    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row, NV_SP_2, 0);
    lv_obj_set_style_pad_row(row, NV_SP_2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    char buf[256];
    snprintf(buf, sizeof buf, "%s", options);
    int i = 0;
    for (char *tok = buf; tok; i++) {
        char *nl = strchr(tok, '\n');
        if (nl) *nl = 0;
        lv_obj_t *p = lv_obj_create(row);
        lv_obj_remove_style_all(p);
        lv_obj_set_size(p, LV_SIZE_CONTENT, NV_TOUCH_MIN);
        lv_obj_set_style_pad_hor(p, NV_SP_4, 0);
        lv_obj_set_style_radius(p, NV_TOUCH_MIN / 2, 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_80, LV_STATE_PRESSED);
        lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(p, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(p);
        lv_label_set_text(l, tok);
        lv_obj_center(l);
        tok = nl ? nl + 1 : nullptr;
    }
    pills_restyle(row, sel);
    return row;
}

int pill_index(lv_event_t *e) { return (int)(intptr_t)lv_event_get_user_data(e); }

void color_cb(lv_event_t *e) {
    s_color_idx = pill_index(e);
    nv_config_set_int("dmd_color", s_color_idx);
    nv_dmd_set_color(kColors[s_color_idx]);
    pills_restyle(lv_obj_get_parent(lv_event_get_target_obj(e)), s_color_idx);
}

void shape_cb(lv_event_t *e) {
    s_shape = pill_index(e);
    nv_config_set_int("dmd_shape", s_shape);
    s_look_gen = s_look_gen + 1;
    pills_restyle(lv_obj_get_parent(lv_event_get_target_obj(e)), s_shape);
}

void theme_cb(lv_event_t *e) {
    s_theme = pill_index(e) ? 1 : 0;
    nv_config_set_int("dmd_theme", s_theme);
    pills_restyle(lv_obj_get_parent(lv_event_get_target_obj(e)), s_theme);
    dmd_menu_set_theme(s_theme);
}

void rot_cb(lv_event_t *e) {
    s_rot = pill_index(e) & 3;
    nv_config_set_int("dmd_rot", s_rot);
    s_look_gen = s_look_gen + 1;
    pills_restyle(lv_obj_get_parent(lv_event_get_target_obj(e)), s_rot);
}

void fit_cb(lv_event_t *e) {
    s_fit_cfg = pill_index(e);
    nv_config_set_int("dmd_fit", s_fit_cfg);
    want_fulldmd();
    s_look_gen = s_look_gen + 1;
    pills_restyle(lv_obj_get_parent(lv_event_get_target_obj(e)), s_fit_cfg);
}

void clock_cb(lv_event_t *e) {
    s_clock = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    nv_config_set_bool("dmd_clock", s_clock);
    s_clock_shown = -1;
}

void glow_cb(lv_event_t *e) {
    s_glow = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    nv_config_set_bool("dmd_glow", s_glow);
    s_look_gen = s_look_gen + 1;
}

void bright_cb(lv_event_t *e) {
    s_bright = (int)lv_slider_get_value(lv_event_get_target_obj(e));
}
void bright_done_cb(lv_event_t *e) {
    nv_config_set_int("dmd_bright", (int)lv_slider_get_value(lv_event_get_target_obj(e)));
}

void boot_cb(lv_event_t *e) {
    nv_config_set_str("boot_app", lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED) ? kAppId : "");
}

void port_apply_cb(lv_event_t *) {
    if (!s_port_ta) return;
    const int p = atoi(lv_textarea_get_text(s_port_ta));
    if (p < 1 || p > 65535) {
        char b[8];
        snprintf(b, sizeof b, "%d", s_port);
        lv_textarea_set_text(s_port_ta, b);
        return;
    }
    if (p == s_port) return;
    s_port = p;
    nv_config_set_int("dmd_port", p);
    nv_dmd_start(p);   // restarts the server on the new port (the PC reconnects with retry = true)
    s_started_us = esp_timer_get_time();
}

// ================================================================== pictures made on the board

uint32_t mono_colour(void) { return kColors[s_color_idx] >= 0 ? (uint32_t)kColors[s_color_idx] : 0xFF4500; }

// The test screen: "TEST" on top, the monochrome colour in 16 steps, the primaries + white as bars, a
// one-dot border and a solid block in the top-left corner (it must show top-left: orientation check).
void draw_test(void) {
    uint32_t *p = s_local;
    const uint32_t mono = mono_colour();
    dmd_text(p, kLocalW, 16, "TEST", mono);
    memset(p + 16 * kLocalW, 0, (size_t)(kLocalH - 16) * kLocalW * 4);
    for (int y = 17; y < 23; y++)
        for (int x = 0; x < kLocalW; x++) {
            const uint32_t l = (uint32_t)(x * 16 / kLocalW);   // 16 steps, as gray4 frames have
            p[y * kLocalW + x] = ((mono >> 16 & 0xFF) * l / 15) << 16 | ((mono >> 8 & 0xFF) * l / 15) << 8 | (mono & 0xFF) * l / 15;
        }
    static const uint32_t bars[8] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0x00FFFF, 0xFF00FF, 0xFFFFFF, 0x808080};
    for (int y = 24; y < kLocalH - 1; y++)
        for (int x = 0; x < kLocalW; x++) p[y * kLocalW + x] = bars[x * 8 / kLocalW];
    for (int x = 0; x < kLocalW; x++) p[x] = p[(kLocalH - 1) * kLocalW + x] = 0xFFFFFF;
    for (int y = 0; y < kLocalH; y++) p[y * kLocalW] = p[y * kLocalW + kLocalW - 1] = 0xFFFFFF;
    for (int y = 1; y < 6; y++)
        for (int x = 1; x < 6; x++) p[y * kLocalW + x] = 0xFFFFFF;
}

// The idle clock: HH:MM large with the colon blinking every second, the date below, round dots over the
// whole panel. False when the time is not set yet.
bool draw_clock(void) {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    if (tmv.tm_year + 1900 < 2025) return false;
    const int phase = (tmv.tm_hour * 60 + tmv.tm_min) * 2 + (tmv.tm_sec & 1);
    if (phase == s_clock_shown) return true;   // nothing changed
    char hm[8], date[40];   // room for any int the compiler can imagine
    snprintf(hm, sizeof hm, "%02d%c%02d", tmv.tm_hour, (tmv.tm_sec & 1) ? ' ' : ':', tmv.tm_min);
    snprintf(date, sizeof date, "%02d.%02d.%04d", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900);
    const uint32_t c = mono_colour();
    const uint32_t dim = ((c >> 16 & 0xFF) * 3 / 5) << 16 | ((c >> 8 & 0xFF) * 3 / 5) << 8 | (c & 0xFF) * 3 / 5;
    // rows 0..51: the time (5x7 glyphs at x4 = 28 dots tall, centred); rows 52..74: the date (x2)
    dmd_text(s_local, kClockW, 52, hm, c);
    dmd_text(s_local + 52 * kClockW, kClockW, kClockH - 52, date, dim);
    s_clock_shown = phase;
    return true;
}

// LVGL tick, 10 Hz: the test screen for a while, else the clock when the PC is silent.
void local_tick(void) {
    if (!s_local) return;
    const int64_t now = esp_timer_get_time();
    if (now < s_test_until_us) {
        draw_test();
        nv_dmd_show(s_local, kLocalW, kLocalH, "test");   // republished: it stays "live"
        s_clock_shown = -1;
        return;
    }
    nv_dmd_status_t st;
    nv_dmd_get_status(&st);
    const bool pc_silent = st.last_pc_us && now - st.last_pc_us > kIdleUs;
    if (!s_clock || !pc_silent || s_menu_on || st.bridge) { s_clock_shown = -1; return; }   // the menu has the screen
    const int before = s_clock_shown;
    if (!draw_clock()) return;
    // republish once a second even when unchanged, so the picture stays "live" (frames younger than 5 s)
    static int64_t last_pub = 0;
    if (s_clock_shown != before || now - last_pub > 1000000) {
        nv_dmd_show(s_local, kClockW, kClockH, "clock");
        last_pub = now;
    }
}

// ================================================================== pages

void my_ip(char *ip, size_t n) {
    ip[0] = 0;
    if (!nv_wifi_get_connected(nullptr, 0, ip, n, nullptr)) ip[0] = 0;
}

void refresh_page(void) {
    if (!s_page_url) return;
    char ip[16], url[48], setup[240], b[400];
    my_ip(ip, sizeof ip);
    snprintf(url, sizeof url, "ws://%s:%d/dmd", ip[0] ? ip : "?", s_port);
    nv_kit_label_set(s_page_url, url);
    nv_dmd_status_t st;
    nv_dmd_get_status(&st);
    snprintf(setup, sizeof setup, nv_tr(NV_STR_DMD_SETUP_FMT), url);
    // The first line says what is wrong, if anything; the setup lines always follow (USB works without
    // Wi-Fi, and a port problem only concerns Wi-Fi).
    const char *first = nv_tr(NV_STR_DMD_WAITING);
    char why[96];
    if (dmd_menu_in_game()) {   // launched from the menu or Popper: the DMD comes as dmdext starts
        nv_kit_label_set(s_page_url, dmd_menu_game());
        snprintf(why, sizeof why, nv_tr(NV_STR_DMDM_INGAME_FMT), "");
        nv_kit_label_set(s_page_msg, why);
        return;
    }
    if (s_big_w) {
        snprintf(why, sizeof why, nv_tr(NV_STR_DMD_TOO_BIG_FMT), s_big_w, s_big_h);
        first = why;
    } else if (!ip[0]) {
        first = nv_tr(NV_STR_DMD_NO_WIFI);
    } else if (!st.listening && esp_timer_get_time() - s_started_us > 3000000) {
        snprintf(why, sizeof why, nv_tr(NV_STR_DMD_PORT_FAIL_FMT), s_port);
        first = why;
    }
    snprintf(b, sizeof b, "%s\n\n%s", first, setup);
    nv_kit_label_set(s_page_msg, b);
}

void refresh_status(void) {
    if (!s_status) return;
    char ip[16], pc[40], b[360];
    my_ip(ip, sizeof ip);
    nv_dmd_status_t st;
    nv_dmd_get_status(&st);
    if (st.client && st.usb) snprintf(pc, sizeof pc, "%s (Wi-Fi) + USB", st.peer);
    else if (st.client) snprintf(pc, sizeof pc, "%s (Wi-Fi)", st.peer);
    else if (st.usb) snprintf(pc, sizeof pc, "USB (ZeDMD 128x32)");
    else snprintf(pc, sizeof pc, "%s", nv_tr(NV_STR_DMD_ST_NONE));
    char size[48] = "-";
    const bool local = !strcmp(st.fmt, "clock") || !strcmp(st.fmt, "test");
    if (st.w && local) snprintf(size, sizeof size, "%dx%d  %s", st.w, st.h, st.fmt);
    else if (st.w) snprintf(size, sizeof size, "%dx%d  %s  (%s)", st.w, st.h, st.fmt, st.last_usb ? "USB" : "Wi-Fi");
    snprintf(b, sizeof b, "%s:  ws://%s:%d/dmd%s\n%s:  %s\n%s:  %s\n%s:  %s\n%s:  %u.%u",
             nv_tr(NV_STR_DMD_ST_ADDRESS), ip[0] ? ip : "?", s_port, st.usb_ready ? "  +  USB" : "",
             nv_tr(NV_STR_DMD_ST_PC), pc,
             nv_tr(NV_STR_DMD_ST_GAME), st.game[0] ? st.game : "-",
             nv_tr(NV_STR_DMD_ST_SIZE), size,
             nv_tr(NV_STR_DMD_ST_FPS), (unsigned)(st.fps10 / 10), (unsigned)(st.fps10 % 10));
    nv_kit_label_set(s_status, b);
}

void set_panel(bool open) {
    if (!s_panel) return;
    s_panel_open = open;
    if (open) lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    if (open) refresh_status();
    if (!open && s_port_ta) port_apply_cb(nullptr);
}

void back_cb(void) {
    if (!s_panel_open && dmd_menu_back()) return;   // a sub-page of the menu first
    set_panel(!s_panel_open);
}
void open_panel(void) { set_panel(true); }
void resume_cb(lv_event_t *) { set_panel(false); }
void exit_cb(lv_event_t *) { nv_ui_close_app(); }
void test_cb(lv_event_t *) {
    s_test_until_us = esp_timer_get_time() + 10000000;   // 10 s, then back to the PC / clock / page
    set_panel(false);
}

bool key_cb(uint32_t key, uint8_t, uint8_t) {
    if (!s_panel_open && (key == LV_KEY_ENTER || key == ' ')) { set_panel(true); return true; }
    return false;
}

void root_press_cb(lv_event_t *e) {
    const lv_event_code_t c = lv_event_get_code(e);
    // a tap on the DMD (or the backbox art): the quick actions; a long press: the panel
    if (c == LV_EVENT_SHORT_CLICKED) {
        if ((s_showing || s_art_screen) && !s_panel_open && !s_hold_fired) dmd_menu_quick_open();
        return;
    }
    if (c == LV_EVENT_PRESSED) { s_press_t = lv_tick_get(); s_hold_fired = false; }
    else if (c == LV_EVENT_PRESSING && !s_hold_fired && !s_panel_open && lv_tick_elaps(s_press_t) >= kHoldMs) {
        s_hold_fired = true;
        set_panel(true);
    }
}

void tick(lv_timer_t *) {
    lv_display_trigger_activity(nullptr);   // a cabinet display never sleeps while the app is open
    if (s_repaint) {   // the layer moved: what it left behind is LVGL's to paint again (black, the art...)
        s_repaint = false;
        lv_obj_invalidate(lv_screen_active());
    }
    // the fit in use: the running table's own, else the panel's
    const int gid = dmd_menu_game_id();
    if (gid != s_fit_gid) {
        s_fit_gid = gid;
        s_fit_tbl = gid ? dmd_app_table_fit(gid) : -1;
        s_rb_on = dmd_app_table_rebuild(gid);
        s_lit_lo = s_lit_hi = -1;   // another game: the rebuilt DMD learns its lit rows again
        const int dot = dmd_app_table_dot(gid);
        if (dot != s_dot) {
            s_dot = dot;
            s_look_gen = s_look_gen + 1;
        }
    }
    const int eff = s_fit_tbl >= 0 ? s_fit_tbl : s_fit_cfg;
    if (eff != s_fit) {
        s_fit = eff;
        s_look_gen = s_look_gen + 1;
    }
    s_menu_ready = dmd_menu_ready();
    // backglass only, during a game: the art has the screen (the menu's "in game" page would cover it)
    s_art_screen = s_fit == kFitBackglass && s_art_ready && dmd_menu_in_game() && !dmd_menu_covering();
    s_menu_on = dmd_menu_tick(!s_showing && !s_panel_open && !s_art_screen);
    // backbox: the running table's backglass above the DMD
    // (backglass only: the whole screen, while a game runs and nothing covers it)
    // (FullDMD: its art on the whole screen, the DMD in its window; without it, the backbox)
    const bool bg_only = s_fit == kFitBackglass;
    dmd_rect_t win{};
    const lv_image_dsc_t *full = s_fit == kFitFullDmd || s_fit == kFitFullWin ? dmd_menu_game_full(&win)
                                 : s_fit == kFitB2sWin                       ? dmd_menu_game_grill(&win)
                                                                             : nullptr;
    if (full && (!s_full_ready || win.x != s_win.x || win.y != s_win.y || win.w != s_win.w || win.h != s_win.h)) {
        s_win = win;
        s_win_gen = s_win_gen + 1;
    }
    s_full_ready = full != nullptr;
    const lv_image_dsc_t *art =
        full ? full
             : s_fit == kFitBackbox || s_fit == kFitFullDmd || fit_in_win(s_fit) || bg_only ? dmd_menu_game_art(bg_only ? kScrH : kScrH - 256)
                                                                       : nullptr;
    s_art_ready = art != nullptr;
    if (s_art) {
        nv_dmd_status_t ds;
        nv_dmd_get_status(&ds);
        const bool on = art && strcmp(ds.fmt, "clock") &&
                        (bg_only ? dmd_menu_in_game() && !s_panel_open && !dmd_menu_covering() : s_showing);
        if (on && lv_image_get_src(s_art) != art) lv_image_set_src(s_art, art);
        // "table art": only the art above its window (the window's frame would sit over the DMD's band)
        const int32_t art_h = full && s_fit == kFitFullDmd && s_win.y > 0 ? s_win.y : LV_SIZE_CONTENT;
        if (lv_obj_get_style_height(s_art, LV_PART_MAIN) != art_h) lv_obj_set_height(s_art, art_h);
        if (on == lv_obj_has_flag(s_art, LV_OBJ_FLAG_HIDDEN)) {
            if (on) lv_obj_remove_flag(s_art, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_art, LV_OBJ_FLAG_HIDDEN);
        }
    }
    local_tick();
    const bool page = !s_showing && !s_panel_open && !s_menu_on && !s_art_screen;
    // NucleoOS's system gestures (shade, Home / Recents, Back) work on the menu, the panel and the waiting
    // page; only the DMD itself, a game on screen, locks them (a stray swipe must not hide the score)
    const int lock = (s_showing || s_art_screen) && !s_panel_open;
    if (lock != s_sys_locked) {
        s_sys_locked = lock;
        nv_ui_set_exit_locked(lock);
        nv_ui_set_shade_gesture_enabled(!lock);
    }
    if (s_page) {
        if (page == lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN)) {
            if (page) lv_obj_remove_flag(s_page, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
        }
    }
    static uint32_t n = 0;
    if (++n % 5) return;    // texts twice a second
    if (page) refresh_page();
    if (s_panel_open) refresh_status();
}

void build_page(lv_obj_t *root) {
    s_page = lv_obj_create(root);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_page, NV_SP_4, 0);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_CLICKABLE);   // presses reach the root (hold = panel)
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    const lv_color_t dot = lv_color_hex(kColors[s_color_idx] >= 0 ? (uint32_t)kColors[s_color_idx] : 0xFF4500);
    lv_obj_t *t = lv_label_create(s_page);
    lv_label_set_text(t, nv_tr(NV_STR_APP_DMD));
    lv_obj_set_style_text_font(t, &nv_font_28, 0);
    lv_obj_set_style_text_color(t, dot, 0);
    s_page_url = lv_label_create(s_page);
    lv_obj_set_style_text_font(s_page_url, &nv_font_28, 0);
    lv_obj_set_style_text_color(s_page_url, lv_color_white(), 0);
    s_page_msg = lv_label_create(s_page);
    lv_obj_set_style_text_font(s_page_msg, &nv_font_20, 0);
    lv_obj_set_style_text_color(s_page_msg, lv_color_hex(0xA0A0A0), 0);
    lv_obj_set_style_text_align(s_page_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *hint = lv_label_create(s_page);
    lv_label_set_text(hint, nv_tr(NV_STR_DMD_HOLD_HINT));
    lv_obj_set_style_text_color(hint, lv_color_hex(0x606060), 0);
    refresh_page();
}

void build_panel(lv_obj_t *root) {
    const NvTheme *th = nv_theme_get();
    s_panel = lv_obj_create(root);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_panel, th->bg, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    nv_focus_skip(s_panel);
    lv_obj_t *c = nv_kit_scroll_column(s_panel);

    lv_obj_t *t = lv_label_create(c);
    lv_label_set_text(t, nv_tr(NV_STR_APP_DMD));
    lv_obj_set_style_text_font(t, &nv_font_28, 0);
    lv_obj_set_style_text_color(t, th->text_strong, 0);
    s_status = nv_kit_info(c);
    lv_obj_set_style_text_font(s_status, &nv_font_mono_17, 0);

    lv_obj_t *btns = lv_obj_create(c);
    lv_obj_remove_style_all(btns);
    lv_obj_set_size(btns, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, NV_SP_3, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *resume = nv_kit_button(btns, nv_tr(NV_STR_DMD_RESUME), true);
    lv_obj_add_event_cb(resume, resume_cb, LV_EVENT_CLICKED, nullptr);
    nv_focus_prefer(resume);
    lv_obj_t *test = nv_kit_button(btns, nv_tr(NV_STR_DMD_TEST), false);
    lv_obj_add_event_cb(test, test_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ex = nv_kit_button(btns, nv_tr(NV_STR_DMD_EXIT), false);
    lv_obj_add_event_cb(ex, exit_cb, LV_EVENT_CLICKED, nullptr);

    pills(c, nv_tr(NV_STR_DMD_THEME), nv_tr(NV_STR_DMD_THEMES), s_theme, theme_cb);
    pills(c, nv_tr(NV_STR_DMD_FIT), nv_tr(NV_STR_DMD_FITS), s_fit_cfg, fit_cb);
    pills(c, nv_tr(NV_STR_DMD_COLOR), nv_tr(NV_STR_DMD_COLORS), s_color_idx, color_cb);
    lv_obj_t *sl = nv_kit_slider_row(c, nv_tr(NV_STR_DMD_BRIGHT), s_bright, 10, 100, bright_cb);
    lv_obj_add_event_cb(sl, bright_done_cb, LV_EVENT_RELEASED, nullptr);
    pills(c, nv_tr(NV_STR_DMD_SHAPE), nv_tr(NV_STR_DMD_SHAPES), s_shape, shape_cb);
    nv_kit_switch_row(c, nv_tr(NV_STR_DMD_GLOW), s_glow, glow_cb);
    pills(c, nv_tr(NV_STR_DMD_ROTATION), "0°\n90°\n180°\n270°", s_rot, rot_cb);

    lv_obj_t *pr = nv_kit_row(c, nv_tr(NV_STR_DMD_PORT));
    s_port_ta = nv_kit_textarea_ex(pr, "9090", true, NV_IME_NUMBER, NV_IME_RET_DONE);
    lv_textarea_set_accepted_chars(s_port_ta, "0123456789");
    lv_textarea_set_max_length(s_port_ta, 5);
    lv_obj_set_width(s_port_ta, 140);
    char b[8];
    snprintf(b, sizeof b, "%d", s_port);
    lv_textarea_set_text(s_port_ta, b);
    lv_obj_add_event_cb(s_port_ta, port_apply_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(s_port_ta, port_apply_cb, LV_EVENT_DEFOCUSED, nullptr);

    nv_kit_switch_row(c, nv_tr(NV_STR_DMD_CLOCK), s_clock, clock_cb);
    nv_kit_switch_row(c, nv_tr(NV_STR_DMD_BOOT), boot_enabled(), boot_cb);
}

void fullscreen_async(void *) { nv_ui_app_fullscreen(true); }

void page_deleted(lv_event_t *) {
    lv_async_call_cancel(fullscreen_async, nullptr);
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    dmd_menu_destroy();
    s_menu_on = false;
    if (s_task) {
        s_run = false;
        xSemaphoreTake(s_task_done, portMAX_DELAY);   // it detaches the layer before exiting
        s_task = nullptr;
    }
    nv_dmd_stop();
    if (s_masks) { heap_caps_free(s_masks); s_masks = nullptr; }
    if (s_local) { heap_caps_free(s_local); s_local = nullptr; }
    s_test_until_us = 0;
    s_clock_shown = -1;
    if (s_sprites) { heap_caps_free(s_sprites); s_sprites = nullptr; }
    nv_ui_set_exit_locked(false);
    nv_ui_set_shade_gesture_enabled(true);
    nv_ui_set_back_handler(nullptr);
    nv_ui_app_fullscreen(false);
    lv_obj_invalidate(lv_screen_active());   // the launcher repaints over the last DMD frame
    s_root = s_page = s_page_url = s_page_msg = s_panel = s_status = s_port_ta = s_art = nullptr;
    s_art_ready = false;
    s_full_ready = false;
    s_panel_open = false;
    s_showing = false;
}


void dmd_build(lv_obj_t *content) {
    load_cfg();
    s_panel_open = false;
    s_showing = false;
    s_big_w = 0;
    s_big_h = 0;
    if (!s_masks) s_masks = (dmd_masks_t *)heap_caps_malloc(sizeof(dmd_masks_t), MALLOC_CAP_SPIRAM);
    if (!s_local) s_local = (uint32_t *)heap_caps_calloc(kClockW * kClockH, 4, MALLOC_CAP_SPIRAM);   // the larger picture
    s_clock_shown = -1;
    if (!s_sprites) s_sprites = (dmd_sprites_t *)heap_caps_malloc(sizeof(dmd_sprites_t), MALLOC_CAP_SPIRAM);
    if (s_sprites) dmd_sprites_reset(s_sprites);
    if (!s_task_done) s_task_done = xSemaphoreCreateBinary();
    nv_dmd_set_color(kColors[s_color_idx]);
    nv_dmd_start(s_port);
    s_started_us = esp_timer_get_time();

    s_root = lv_obj_create(content);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);   // the dark between the dots, not a theme role
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_root, page_deleted, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(s_root, root_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_root, root_press_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_root, root_press_cb, LV_EVENT_SHORT_CLICKED, nullptr);
    nv_focus_skip(s_root);
    build_page(s_root);
    s_art = lv_image_create(s_root);   // the backbox art (behind the menu and the panel)
    lv_image_set_inner_align(s_art, LV_IMAGE_ALIGN_TOP_LEFT);   // cut to a height: its top part
    lv_obj_set_pos(s_art, 0, 0);
    lv_obj_clear_flag(s_art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_art, LV_OBJ_FLAG_HIDDEN);
    dmd_menu_set_theme(s_theme);
    dmd_menu_build(s_root, open_panel);   // above the page, below the panel
    build_panel(s_root);

    nv_ui_set_back_handler(back_cb);
    nv_ui_set_key_handler(key_cb);
    nv_ui_set_shortcuts(kKeys, 2);
    s_sys_locked = -1;   // the tick sets them from what is on screen
    // After build(): the classic desktop opens built-in apps windowed at start, a later request
    // gets the whole panel (as the video player's fullscreen button does).
    lv_async_call(fullscreen_async, nullptr);

    if (s_masks && s_task_done && !s_task) {
        xSemaphoreTake(s_task_done, 0);
        s_run = true;
        // PSRAM stack: no flash access (nv_disp layer calls and nv_dmd only); 8 KB: a dmd_geom_t (~2 KB)
        // lives on it. Core 0, as the video's.
        if (xTaskCreatePinnedToCoreWithCaps(disp_task, "dmddisp", 8192, nullptr, 5, &s_task, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
            s_task = nullptr;
            s_run = false;
        }
    }
    s_timer = lv_timer_create(tick, 100, nullptr);
}

const NvApp kDmdApp = {
    .id = kAppId,
    .name = "DMD Pinball",
    .icon = &nv_icon_dmd,           // drawn by tools/icons/make_dmd_icon.py
    .ram_budget = 1024u << 10,     // ~0.8 MB of PSRAM buffers (nv_dmd, masks, dot cache) + the panel
    .build = dmd_build,
    .name_id = NV_STR_APP_DMD,
    .user = nullptr,
    .flags = 0,
};

}  // namespace

void dmd_app_register(void) { nv_app_register(&kDmdApp); }

int dmd_app_table_fit(int game_id) {
    if (game_id <= 0) return -1;
    char k[24];
    table_key(k, sizeof k, game_id);
    const int v = nv_config_get_int(k, 0) - 1;
    return v >= DMD_FIT_FILL && v <= kFitLast ? v : -1;
}

void dmd_app_set_table_fit(int game_id, int fit) {
    if (game_id <= 0) return;
    char k[24];
    table_key(k, sizeof k, game_id);
    nv_config_set_int(k, fit >= DMD_FIT_FILL && fit <= kFitLast ? fit + 1 : 0);
    if (fit == kFitFullDmd) nv_config_set_bool("dmd_fullany", true);
    want_fulldmd();
    s_fit_gid = -1;   // the next tick takes it
}

bool dmd_app_dmd_half(void) {
    int r0 = 0, n = 0;
    if (s_lh < 8 || s_lit_lo < 0) return false;
    dmd_rebuild_rows(s_lh, s_lit_lo, s_lit_hi, &r0, &n);
    return n < s_lh;
}

int dmd_app_fit_suggested(bool has_art, bool has_grill) {
    return has_art ? kFitFullWin : has_grill ? kFitB2sWin : dmd_app_dmd_half() ? kFitRebuilt : DMD_FIT_SQUARE;
}

bool dmd_app_table_rebuild(int game_id) {
    if (game_id <= 0) return true;
    char k[24];
    snprintf(k, sizeof k, "dmdrb_%d", game_id);
    return nv_config_get_int(k, 1) != 0;
}

int dmd_app_table_dot(int game_id) {
    if (game_id <= 0) return 0;
    char k[24];
    snprintf(k, sizeof k, "dmddot_%d", game_id);
    return nv_config_get_int(k, 0) & 7;
}

void dmd_app_set_table_dot(int game_id, int dot) {
    if (game_id <= 0) return;
    char k[24];
    snprintf(k, sizeof k, "dmddot_%d", game_id);
    nv_config_set_int(k, dot & 7);
    if (game_id == s_fit_gid) s_fit_gid = -1;   // the next tick takes it
}

int dmd_app_fit_all(void) { return s_fit_cfg; }

void dmd_app_dmd_size(int *w, int *h) {
    *w = s_lw;
    *h = s_lh;
}

bool dmd_app_snapshot(uint32_t *dst, size_t cap, int *w, int *h) {
    int fw = 0, fh = 0;
    uint32_t gen = 0;
    const uint32_t *px = nv_dmd_acquire(&fw, &fh, &gen);
    if (!px) return false;
    const bool ok = fw > 0 && fh > 0 && (size_t)fw * (size_t)fh <= cap;
    if (ok) {
        memcpy(dst, px, (size_t)fw * (size_t)fh * 4);
        *w = fw;
        *h = fh;
    }
    nv_dmd_release();
    return ok;
}

bool dmd_app_fit_rect(int fit, int dot, dmd_rect_t *out) {
    if (fit < DMD_FIT_FILL || fit > kFitLast || fit == kFitBackglass) return false;
    dmd_rect_t win{};
    if (fit == kFitFullWin && !dmd_menu_game_full(&win)) return false;
    if (fit == kFitB2sWin && !dmd_menu_game_grill(&win)) return false;
    dmd_geom_t *g = (dmd_geom_t *)heap_caps_malloc(sizeof(dmd_geom_t), MALLOC_CAP_SPIRAM);
    if (!g) return false;
    const int w = s_lw > 0 ? s_lw : 128, h = s_lh > 0 ? s_lh : 32;
    const bool ok = geom_for(fit, w, h, s_rot, dot, win, g);
    if (ok) *out = dmd_rect_t{g->ox, g->oy, g->gw, g->gh};
    heap_caps_free(g);
    return ok;
}

void dmd_app_set_table_rebuild(int game_id, bool on) {
    if (game_id <= 0) return;
    char k[24];
    snprintf(k, sizeof k, "dmdrb_%d", game_id);
    nv_config_set_int(k, on ? 1 : 0);
    if (game_id == s_fit_gid) s_rb_on = on;
}
