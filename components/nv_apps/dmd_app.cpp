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
#include "dmd_panel.h"
#include "dmd_ui.h"
#include "nv_fonts.h"
#include "nv_i18n.h"
#include "nv_log.h"
#include "nv_mem_attr.h"
#include "nv_icons.h"
#include "nv_pins.h"
#include "nv_theme.h"
#include "nv_time.h"
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
// During a game the last frame stays: FlexDMD / UltraDMD tables send a frame only when the picture changes
// (Goldorak: silent for many seconds on a still screen), and the game's real end comes from the bridge. Not a
// black one: dmdext clears the panel when a table starts, and a table that sends nothing more (Transformers G1:
// its displays are B2S lights) would leave the screen black instead of its art
constexpr int64_t kIdleGameUs = 600000000;
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
// Automatic: each table the fit its pictures suit best (dmd_app_fit_suggested), decided as its pictures come
constexpr int kFitAuto = 9;
constexpr int kFitLast = kFitAuto;
// the fits that draw the DMD inside a window of a picture
constexpr bool fit_in_win(int f) { return f == kFitFullWin || f == kFitB2sWin; }
NV_PSRAM_BSS volatile int s_fit = DMD_FIT_SQUARE;   // in use: the running table's own fit, else s_fit_cfg
NV_PSRAM_BSS int s_fit_cfg = DMD_FIT_SQUARE;        // the panel's "Fit" (every table)
NV_PSRAM_BSS int s_fit_gid = -1, s_fit_tbl = -1;    // the table whose own fit is cached, and that fit (-1 none)
NV_PSRAM_BSS volatile bool s_full_ready = false;  // that art and its window are there (LVGL thread)
// The window (tick writes it, the display task copies it) and the running game's start: both under s_mux, so
// the task never sees half a rectangle or half a 64-bit time. The lock itself stays in internal RAM.
NV_PSRAM_BSS dmd_rect_t s_win{};
NV_PSRAM_BSS uint32_t s_win_gen = 0;
NV_PSRAM_BSS int64_t s_game_us = 0;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
// What the display task needs to know about the UI, computed by tick on the LVGL thread: the task never calls
// into LVGL or the menu (the menu's objects can be deleted under it, by a change of its look)
NV_PSRAM_BSS volatile bool s_in_game = false;     // dmd_menu_in_game()
NV_PSRAM_BSS volatile int s_cover = 0;            // what covers the screen: 4 the shade, 8 the title bar, 16 the menu's
                                                  // overlays, 32 the question before leaving
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
NV_PSRAM_BSS int s_lit_gid = -1;    // the game they belong to (only another game resets them)
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
// The test screen is the layer's own picture (s_testpx), not a frame published in nv_dmd: a PC still sending
// would replace it ten times a second (flicker). s_test_gen changes with the countdown drawn in it.
NV_PSRAM_BSS uint32_t *s_testpx = nullptr;
NV_PSRAM_BSS volatile bool s_test_on = false;   // tick: the test runs (the display task shows it, whatever the PC sends)
NV_PSRAM_BSS volatile bool s_ltest = false;     // the layer draws it (set by the task while detached)
NV_PSRAM_BSS volatile uint32_t s_test_gen = 1;
NV_PSRAM_BSS int s_test_left = -1;              // seconds shown in it
// A colour chosen while the frame is still (dmdext sends a gray frame only when it changes): the layer shows
// that frame in the new colour (tint_frame) until the next one, decoded in the new colour by nv_dmd.
NV_PSRAM_BSS uint32_t *s_tint = nullptr;        // PSRAM, DMD_MAX_PX values, from the first colour change
NV_PSRAM_BSS uint32_t s_tint_gen = 0, s_tint_ok = 0, s_tint_from = 0, s_tint_to = 0;
NV_PSRAM_BSS lv_timer_t *s_bright_timer = nullptr;   // the brightness saved once the slider rests
// The port: a new one is kept (nv_config) only once the server listens on it; a note next to the field
NV_PSRAM_BSS bool s_port_saved = true;
NV_PSRAM_BSS char s_port_typed[8] = "";         // the field's text not applied yet, carried across a rebuild
NV_PSRAM_BSS char s_port_note[96] = "";
NV_PSRAM_BSS int64_t s_port_note_until = 0;
NV_PSRAM_BSS int s_clock_shown = -1;            // minute*2 + colon phase last drawn (-1: none)
NV_PSRAM_BSS bool s_menu_on = false;            // the Popper menu is on screen (dmd_menu)
NV_PSRAM_BSS int s_theme = 0;                   // the menu's look: 0 cabinet, 1 DMD ("dmd_theme")
NV_PSRAM_BSS int s_sys_locked = -1;             // system gestures locked (the DMD on screen); -1 unknown
NV_PSRAM_BSS volatile bool s_menu_ready = false; // the menu wants the screen: the idle clock steps aside
// The app's life is not its widgets': the receivers, the display task and the buffers run from a fresh build
// to app_stop (close, switch, or the root deleted without close). A rebuild (language, theme, rotation, a new
// intent) only makes the widgets again: the cabinet's DMD, the PC's link and a running game carry on.
NV_PSRAM_BSS bool s_open = false;
NV_PSRAM_BSS bool s_keep = false;               // close(NV_APP_REBUILD) came: the root's deletion drops widgets only

const nv_shortcut_t kKeys[] = {   // key names as the system sheet writes them (nv_ui_shortcuts.cpp)
    {"Invio / Spazio", "Stato e impostazioni del DMD (o fine della prova)", "DMD status and settings (or end the test)"},
    {"Esc", "Apri / chiudi il pannello", "Open / close the panel"},
};

// ================================================================== layer

uint32_t layer_latest(void *) { return s_ltest ? s_test_gen : nv_dmd_latest(); }

// How a frame reaches the panel (measured on the board, 128x32 with square 8 px dots, CPU at 360 MHz):
//   * every dot comes from a cache of finished dots, one per colour and dot shape (dmd_sprites_t): a dot
//     row costs a few 32-bit copies instead of a table look-up per pixel (a 512 KB band: 2.2 ms, not 15);
//   * it is drawn in strips in internal SRAM and copied into the frame by the AXI-GDMA while the next
//     strip is drawn: the CPU writing PSRAM itself reads every cache line first (8.6 ms for the band),
//     and the DMA's cost is mostly per transfer (16 KB strips: 8 ms, 32 KB: 4.8 ms for the band, ~0.2 ms a
//     copy), so strips are as big as internal SRAM allows, up to 64 KB: whole dot rows, as many as fit.
// Result: 4.5-5 ms per frame for the 1024x256 band. The two strips stay while the app is open (taking and
// giving back up to 128 KB of internal SRAM at every cover / uncover fragmented it); without them (no SRAM,
// dots too tall for a strip, the DMA retired) the dots are drawn straight into the frame.
NV_PSRAM_BSS uint16_t *s_strip[2] = {};
NV_PSRAM_BSS SemaphoreHandle_t s_strip_done[2] = {};
NV_PSRAM_BSS size_t s_strip_bytes = 0;              // each strip's size
NV_PSRAM_BSS int s_strip_cells = 0;                 // dot rows per strip for the geometry on screen, 0 = no strips
// A copy that timed out may still be reading its strip: that strip is never written again nor freed while
// it may be, and frames are drawn without strips from then on (the DMA channel is retired as well).
NV_PSRAM_BSS volatile bool s_strip_stuck[2] = {};
NV_PSRAM_BSS volatile bool s_strips_dead = false;
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

// Display task, with no draw running (layer detached, or the task exiting).
void strips_free(void) {
    for (int i = 0; i < 2; i++) {
        // a copy that timed out: give it time to land (its semaphore comes then), else keep the strip for good
        // (once: a stuck strip given up is forgotten, and later openings never wait on it again)
        if (s_strip[i] && s_strip_stuck[i] && s_strip_done[i] && xSemaphoreTake(s_strip_done[i], pdMS_TO_TICKS(500)) == pdTRUE)
            s_strip_stuck[i] = false;
        if (!s_strip_stuck[i]) heap_caps_free(s_strip[i]);
        s_strip[i] = nullptr;
    }
    s_strip_bytes = 0;
    s_strip_cells = 0;
}

// Display task, layer detached. A strip holds whole dot rows, each at most ph + 1 panel rows tall: as many as
// fit (8 px dots: 3 rows of a 64 KB strip, so a 1024x512 picture is 22 copies, not 64). Taken once, as big as
// kStripMax allows while internal SRAM keeps kSramReserve for the rest of the system (Wi-Fi, LVGL, sockets).
constexpr size_t kStripMax = 64u << 10, kStripMin = 24u << 10, kSramReserve = 96u << 10;
void strips_alloc(const dmd_geom_t &g) {
    s_strip_cells = 0;
    if (s_strips_dead) return;
    for (size_t bytes = kStripMax; !s_strip[0] && bytes >= kStripMin; bytes -= 8u << 10) {
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 2 * bytes + kSramReserve) continue;
        bool ok = true;
        for (int i = 0; i < 2 && ok; i++) {
            if (!s_strip_done[i]) s_strip_done[i] = xSemaphoreCreateBinary();
            s_strip[i] = (uint16_t *)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            ok = s_strip[i] && s_strip_done[i];
        }
        if (ok) s_strip_bytes = bytes;
        else strips_free();
    }
    if (s_strip[0]) s_strip_cells = (int)(s_strip_bytes / ((size_t)kScrW * 2 * (size_t)(g.ph + 1)));
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
        uint16_t *to = j.band + (size_t)y0 * kScrW;
        if (busy[b] && nv_2d_copy_wait(s_strip_done[b], 100) != ESP_OK) {   // it may still be reading this strip
            s_strip_stuck[b] = true;
            s_strips_dead = true;
        }
        busy[b] = false;
        if (s_strips_dead) {   // the rest of this frame straight into it
            dmd_render_rows(to, kScrW, &g, s_masks, j.px, j.w, j.h, j.rot, s_bright, true, kScrW, cy, cy1,
                            j.cached ? s_sprites : nullptr);
            continue;
        }
        dmd_render_rows(s_strip[b], kScrW, &g, s_masks, j.px, j.w, j.h, j.rot, s_bright, true, kScrW, cy, cy1,
                        j.cached ? s_sprites : nullptr);
        busy[b] = nv_2d_copy_start(to, s_strip[b], bytes, s_strip_done[b]) == ESP_OK;
        if (!busy[b]) memcpy(to, s_strip[b], bytes);   // no DMA (retired): the CPU copies
    }
    for (int i = 0; i < 2; i++)
        if (busy[i] && nv_2d_copy_wait(s_strip_done[i], 100) != ESP_OK) {
            s_strip_stuck[i] = true;
            s_strips_dead = true;
        }
}

bool strips_usable(int stride) { return s_strip_cells && !s_strips_dead && stride == kScrW; }

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
    a->rows_b = (kScrH - 1 - (g.oy + g.gh)) / g.ph;   // the panel's last row is LVGL's (see the layer)
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
        if (bot_start < kScrH - 1) memset(fb + (size_t)bot_start * stride, 0, (size_t)(kScrH - 1 - bot_start) * (size_t)stride * 2);
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
            if (strips_usable(stride)) draw_strips(j);
            else dmd_render_rows(j.band, stride, &b, s_masks, a->band, cols, b.rows, 0, s_bright, true, kScrW, 0, b.rows, nullptr);
        }
    }
    memcpy(a->shown[idx], a->want, (size_t)(2 * cols) * 4);
}

// The still frame in the newly chosen colour (into out): only when every lit dot is a shade of the old
// colour, as a gray frame's are; a palette or an RGB picture stays as the PC made it. from 0: the old colour
// was the PC's, taken from the brightest dot.
bool tint_frame(const uint32_t *px, size_t n, uint32_t from, uint32_t to, uint32_t *out) {
    auto top = [](uint32_t c) {
        const int r = (int)(c >> 16 & 255), g = (int)(c >> 8 & 255), b = (int)(c & 255);
        return r > g ? (r > b ? r : b) : (g > b ? g : b);
    };
    if (!from)
        for (size_t i = 0; i < n; i++)
            if (top(px[i]) > top(from)) from = px[i];
    const int fr = (int)(from >> 16 & 255), fg = (int)(from >> 8 & 255), fb = (int)(from & 255), fm = top(from);
    if (fm < 16) return false;
    const int tr = (int)(to >> 16 & 255), tg = (int)(to >> 8 & 255), tb = (int)(to & 255);
    for (size_t i = 0; i < n; i++) {
        const uint32_t p = px[i];
        const int r = (int)(p >> 16 & 255), g = (int)(p >> 8 & 255), b = (int)(p & 255);
        const int m = top(p) < fm ? top(p) : fm;   // the dot's level, on the old colour's brightest channel
        // each channel the old colour at that level (within the decoder's rounding), else not a gray frame
        if (abs(r * fm - fr * m) > 6 * fm || abs(g * fm - fg * m) > 6 * fm || abs(b * fm - fb * m) > 6 * fm) return false;
        out[i] = (uint32_t)(tr * m / fm) << 16 | (uint32_t)(tg * m / fm) << 8 | (uint32_t)(tb * m / fm);
    }
    return true;
}

uint32_t layer_draw(void *, uint16_t *fb, int stride, bool fresh) {
    int w = 0, h = 0;
    uint32_t gen = 0;
    const bool test = s_ltest;   // the test screen: the board's own picture, nothing from nv_dmd
    const uint32_t *px = test ? s_testpx : nv_dmd_acquire(&w, &h, &gen);
    if (test) {
        w = kLocalW;
        h = kLocalH;
        gen = s_test_gen;
    }
    if (!px) return 0;
    uint32_t ok = 0;
    if (w == s_lw && h == s_lh) {   // else the task re-attaches with the new size right away
        uint16_t *band = fb + (size_t)s_geom.oy * stride;
        int lo, hi;   // the rows this game lights (the rebuilt DMD and the per-table suggestion use them)
        if (!test && dmd_lit_rows(px, w, h, &lo, &hi)) {
            if (s_lit_lo < 0 || lo < s_lit_lo) s_lit_lo = lo;
            if (hi > s_lit_hi) s_lit_hi = hi;
        }
        if (!test && gen == s_tint_gen && s_tint && (size_t)w * (size_t)h <= DMD_MAX_PX) {   // a colour chosen on it
            if (s_tint_ok == gen || tint_frame(px, (size_t)w * (size_t)h, s_tint_from, s_tint_to, s_tint)) {
                s_tint_ok = gen;
                px = s_tint;
            } else {
                s_tint_gen = 0;   // not a gray frame: as it is
            }
        }
        int r0 = 0, n = h;
        if (!test && s_rb_on) dmd_rebuild_rows(h, s_lit_lo, s_lit_hi, &r0, &n);   // a 128x16 DMD: its 16 rows
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
            if (!test) nv_dmd_release();
            return gen;
        }
        Amb *a = s_amb;
        if (a && (size_t)w * (size_t)h <= a->frame_cap) {   // the DMD's unlit dots: the bands' faint colour
            for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; i++) a->frame[i] = px[i] ? px[i] : a->floor;
            px = a->frame;
        }
        const Job j = {&s_geom, band, px, w, h, s_lrot, true};
        if (strips_usable(stride)) draw_strips(j);
        else dmd_render_rows(band, stride, &s_geom, s_masks, px, w, h, s_lrot, s_bright, true, kScrW, 0, s_geom.rows, s_sprites);
        if (a) amb_draw(a, fb, stride, fresh, orig, w, h);
        ok = gen;
    }
    if (!test) nv_dmd_release();
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

// Rotation turns the dots only: the pictures (backglass, table art) and where the bands go are the panel's.
// So a rotated screen shows the classic DMD instead of the layouts made of pictures, and at 90 / 270 also
// instead of the tall and the rebuilt DMD (their bands would run along the cabinet's sides).
int rot_fit(int fit) {
    const int r = s_rot;
    if (!r) return fit;
    if (fit == kFitBackbox || fit == kFitBackglass || fit == kFitFullDmd || fit_in_win(fit)) return DMD_FIT_SQUARE;
    if ((r & 1) && (fit == kFitTall || fit == kFitRebuilt)) return DMD_FIT_SQUARE;
    return fit;
}

// The fit the board really draws for `fit` (resolved: not Automatic) with a w x h DMD: full = its own picture
// with the window is there, art = the backglass for the backbox. Without the picture: FullDMD and the fits in a
// window take the backbox, else the whole screen (never a strip with black bands: a full screen was chosen);
// the backbox the classic DMD; the rebuilt DMD the classic one when twice its rows do not fit.
int fit_shown(int fit, bool full, bool art, int w, int h) {
    fit = rot_fit(fit);
    if (fit == kFitFullDmd || fit_in_win(fit)) return full ? fit : art ? kFitBackbox : DMD_FIT_FILL;
    if (fit == kFitBackbox || fit == kFitBackglass) return art ? fit : DMD_FIT_SQUARE;
    if (fit == kFitRebuilt) return 2 * h <= DMD_MAX_H && w * 2 * h <= DMD_MAX_PX ? kFitRebuilt : DMD_FIT_SQUARE;
    return fit == kFitAuto ? DMD_FIT_SQUARE : fit;
}

// Wakes on every new frame (or every 50 ms) and drives the layer: attach with the current geometry,
// detach while the panel, the shade or the classic title bar covers the screen or the PC is silent. It reads
// only flags and copies the tick prepares (s_cover, s_in_game, s_win under s_mux): no LVGL, no menu calls.
void disp_task(void *) {
    bool attached = false, test_used = false;
    uint32_t look = 0, test_gen = 0;
    int fit_used = -1;
    uint32_t win_used = 0;
    int why_shown = -1;   // the last reason logged for the DMD not on screen (only changes are logged)
    while (s_run) {
        const bool fresh = nv_dmd_wait(50);
        if (!s_run) break;
        nv_dmd_status_t st;
        nv_dmd_get_status(&st);
        dmd_rect_t win;
        uint32_t win_gen;
        int64_t game_us;
        portENTER_CRITICAL(&s_mux);
        win = s_win;
        win_gen = s_win_gen;
        game_us = s_game_us;
        portEXIT_CRITICAL(&s_mux);
        const bool in_game = s_in_game;
        // the test screen: its own 128x32 picture, shown whatever the PC sends meanwhile (and even with
        // "backglass only", where a PC frame gives way to the art)
        const bool test = s_test_on;
        const bool clock = !test && !strcmp(st.fmt, "clock");
        const int fw = test ? kLocalW : st.w, fh = test ? kLocalH : st.h;
        const int64_t idle = in_game && !clock && !st.blank ? kIdleGameUs : kIdleUs;
        bool live = nv_dmd_latest() && st.last_frame_us && esp_timer_get_time() - st.last_frame_us < idle;
        if (s_menu_ready && clock) live = false;   // the Popper menu takes over at once
        // the score displays of a table without a DMD: only this game's (a picture left by the last game or
        // by the menu's time must not stay on screen), and only while a game runs
        if (!strcmp(st.fmt, "score") && (!in_game || st.last_frame_us < game_us)) live = false;
        if (s_fit == kFitBackglass && s_art_ready && !clock) live = false;   // backglass only
        if (test) live = true;
        const int cover = s_cover;
        const bool covered = s_panel_open || cover;
        {   // why the DMD is (not) on screen, logged when it changes: frames, panel, shade, title bar, menu
            const int why = (live ? 1 : 0) | (s_panel_open ? 2 : 0) | cover;
            if (why != why_shown && (why & 1 || why_shown & 1)) {
                nv_log_write(NV_LOG_INFO, "dmd", "frames %s%s%s%s%s", why & 1 ? "on" : "off", why & 2 ? ", panel open" : "",
                             why & 4 ? ", shade open" : "", why & 8 ? ", title bar" : "", why & 16 ? ", menu covering" : "");   // (32: the question before leaving)
            }
            why_shown = why;
        }
        if (!live || covered) {
            if (attached) {
                nv_disp_layer_set(nullptr);
                attached = false;
                s_repaint = true;   // the strips stay: taken again at every cover they fragmented SRAM
            }
            s_showing = false;
            continue;
        }
        // The clock keeps its dots round: square cells for it, the user's choice for everything else.
        const int fit = clock || (test && s_fit == kFitBackglass) ? DMD_FIT_SQUARE
                        : s_fit == kFitBackglass                    ? DMD_FIT_SQUARE   // (only without its art)
                                                                    : fit_shown(s_fit, s_full_ready, s_art_ready, fw, fh);
        if (!attached || fw != s_lw || fh != s_lh || look != s_look_gen || fit != fit_used || test != test_used ||
            ((fit == kFitFullDmd || fit_in_win(fit)) && win_gen != win_used)) {
            if (attached) { nv_disp_layer_set(nullptr); attached = false; }   // draw never sees half a geometry
            look = s_look_gen;
            const int rot = s_rot;
            dmd_geom_t g;
            fit_used = fit;
            win_used = win_gen;
            test_used = test;
            if ((size_t)fw * (size_t)(2 * fh) > s_px2_cap) {   // every fit may rebuild (detached: no draw running)
                heap_caps_free(s_px2);
                heap_caps_free(s_tmp2);
                const size_t cap = (size_t)fw * (size_t)(2 * fh);
                s_px2 = (uint32_t *)heap_caps_malloc(cap * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
                s_tmp2 = (uint32_t *)heap_caps_malloc(cap * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
                s_px2_cap = s_px2 && s_tmp2 ? cap : 0;
            }
            const bool x2 = fit == kFitRebuilt && s_px2_cap;
            // the table's dots: original ones, a margin of unlit dots (only with its frame buffer)
            int dot = clock ? 0 : s_dot;
            const int m = (dot >> 1) & 3;
            const size_t mcap = (size_t)(fw + 2 * m) * (size_t)((x2 ? 2 : 1) * fh + 2 * m);
            if (m && mcap > s_pxm_cap) {   // detached: no draw running
                heap_caps_free(s_pxm);
                s_pxm = (uint32_t *)heap_caps_malloc(mcap * 4, MALLOC_CAP_SPIRAM);
                s_pxm_cap = s_pxm ? mcap : 0;
            }
            if (m && !s_pxm_cap) dot &= 1;
            const bool ok = geom_for(x2 || fit != kFitRebuilt ? fit : DMD_FIT_SQUARE, fw, fh, rot, dot, win, &g);
            if (!ok) {
                s_big_w = fw;
                s_big_h = fh;
                s_showing = false;
                strips_free();   // nothing to draw: the SRAM back
                continue;
            }
            s_big_w = 0;
            s_big_h = 0;
            s_mg = (dot >> 1) & 3;
            s_x2 = x2;
            s_in_win = fit_in_win(fit);
            s_ltest = test;
            // ambient bands where the fit leaves them (not the clock: it fills the panel)
            const bool bands = !clock && (fit == DMD_FIT_SQUARE || fit == kFitTall) && amb_setup(g, fw + 2 * s_mg, fh + 2 * s_mg,
                               s_color_idx > 0 && s_color_idx < kNColors && kColors[s_color_idx] >= 0 ? (uint32_t)kColors[s_color_idx] : 0);
            if (!bands) amb_free();
            dmd_masks_build(s_masks, &g, s_shape, s_glow);
            if (s_sprites) dmd_sprites_reset(s_sprites);
            strips_alloc(g);   // (in a window they stay unused: whole-row strips would paint over the art beside it)
            nv_log_write(NV_LOG_INFO, "dmd", "%dx%d -> %dx%d dots of %dx%d px at %d,%d, %s (%d dot rows a strip)%s", fw,
                         fh, g.cols, g.rows, g.pw, g.ph, g.ox, g.oy,
                         strips_usable(kScrW) && !s_in_win ? "SRAM strips + DMA" : "straight into the frame", s_strip_cells,
                         test ? ", test screen" : "");
            s_geom = g;
            s_lw = fw;
            s_lh = fh;
            s_lrot = rot;
            s_showing = true;
            test_gen = s_test_gen;
            // with bands the layer is the whole panel but its last row: nv_disp asks LVGL for a frame by
            // painting a corner pixel outside the layer, and one inside would make every draw a fresh one
            // (the whole panel redrawn: 31 ms). That row is the gap under the last band's dots.
            const nv_disp_layer_t layer = s_amb      ? nv_disp_layer_t{0, 0, kScrW, kScrH - 1, layer_latest, layer_draw, nullptr}
                                          : s_in_win ? nv_disp_layer_t{g.ox, g.oy, g.gw, g.gh, layer_latest, layer_draw, nullptr}
                                                     : nv_disp_layer_t{0, g.oy, kScrW, g.gh, layer_latest, layer_draw, nullptr};
            nv_disp_layer_set(&layer);
            attached = true;
            s_repaint = true;
        } else if (test ? s_test_gen != test_gen : fresh) {   // the test's countdown, or a new PC frame
            test_gen = s_test_gen;
            nv_disp_layer_update();
        }
    }
    if (attached) nv_disp_layer_set(nullptr);
    amb_free();
    strips_free();
    s_x2 = false;
    s_ltest = false;
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

// The running table's automatic fit: from the pictures it has (its FullDMD art, its backglass with grill).
int auto_fit(void) {
    dmd_rect_t w{};
    const bool art = dmd_menu_game_full(&w) != nullptr;
    const bool grill = dmd_menu_game_grill(&w) != nullptr;
    return dmd_app_fit_suggested(art, grill);
}

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
    s_port_saved = true;
    s_port_note[0] = 0;
    s_fit_cfg = nv_config_get_int("dmd_fit", DMD_FIT_SQUARE);
    if (s_fit_cfg < DMD_FIT_FILL || s_fit_cfg > kFitLast) s_fit_cfg = DMD_FIT_SQUARE;
    // what the display task may use before the first tick resolves it: Automatic needs the game's pictures
    s_fit = rot_fit(s_fit_cfg == kFitAuto ? DMD_FIT_SQUARE : s_fit_cfg);
    s_fit_gid = -1;
    s_lit_gid = -1;
    want_fulldmd();
    s_clock = nv_config_get_bool("dmd_clock", true);
}

bool boot_enabled(void) {
    char b[16];
    nv_config_get_str("boot_app", "", b, sizeof b);
    return !strcmp(b, kAppId);
}

// What the panel's controls do (dmd_panel.cpp draws them).
void set_color(int i) {
    const int old = s_color_idx;
    s_color_idx = i >= 0 && i < kNColors ? i : 1;
    nv_config_set_int("dmd_color", s_color_idx);
    nv_dmd_set_color(kColors[s_color_idx]);
    // nv_dmd colours the next gray frame; a still one (dmdext sends nothing new) is recoloured by the layer
    // until then. Not to "from the PC" (its colour is unknown here) nor a frame from USB (dmdext colours those).
    nv_dmd_status_t st;
    nv_dmd_get_status(&st);
    s_tint_gen = 0;
    if (kColors[s_color_idx] >= 0 && !st.last_usb && !strncmp(st.fmt, "gray", 4)) {
        if (!s_tint) s_tint = (uint32_t *)heap_caps_malloc((size_t)DMD_MAX_PX * 4, MALLOC_CAP_SPIRAM);
        s_tint_from = kColors[old] >= 0 ? (uint32_t)kColors[old] : 0;
        s_tint_to = (uint32_t)kColors[s_color_idx];
        s_tint_ok = 0;
        s_tint_gen = s_tint ? nv_dmd_latest() : 0;
    }
    s_look_gen = s_look_gen + 1;   // drawn again (and the bands' unlit dots take the new colour)
}

void set_shape(int i) {
    s_shape = i ? DMD_SHAPE_SQUARE : DMD_SHAPE_ROUND;
    nv_config_set_int("dmd_shape", s_shape);
    s_look_gen = s_look_gen + 1;
}

void set_theme(int i) {
    s_theme = i ? 1 : 0;
    nv_config_set_int("dmd_theme", s_theme);
    dmd_menu_set_theme(s_theme);
}

void set_rot(int i) {
    s_rot = i & 3;
    nv_config_set_int("dmd_rot", s_rot);
    s_look_gen = s_look_gen + 1;
}

void set_clock(bool on) {
    s_clock = on;
    nv_config_set_bool("dmd_clock", on);
    s_clock_shown = -1;
}

void set_glow(bool on) {
    s_glow = on;
    nv_config_set_bool("dmd_glow", on);
    s_look_gen = s_look_gen + 1;
}

// The brightness is saved when the finger leaves the slider, or once it rests for a moment (the keyboard's
// arrows never "release" it), or when the panel closes.
void bright_save(void) {
    if (s_bright_timer) {
        lv_timer_delete(s_bright_timer);
        s_bright_timer = nullptr;
    }
    nv_config_set_int("dmd_bright", s_bright);
}
void bright_timer_cb(lv_timer_t *) {
    s_bright_timer = nullptr;   // a one-shot timer: LVGL deletes it after this call
    nv_config_set_int("dmd_bright", s_bright);
}
void set_bright(int v, bool done) {
    s_bright = v < 10 ? 10 : v > 100 ? 100 : v;
    if (done) {
        bright_save();
    } else if (s_bright_timer) {
        lv_timer_reset(s_bright_timer);
    } else {
        s_bright_timer = lv_timer_create(bright_timer_cb, 800, nullptr);
        if (s_bright_timer) lv_timer_set_repeat_count(s_bright_timer, 1);
    }
}

// Off clears the system's boot app only when it is this one: another app's choice stays.
void set_boot(bool on) {
    if (on) nv_config_set_str("boot_app", kAppId);
    else if (boot_enabled()) nv_config_set_str("boot_app", "");
}

void port_note(const char *t) {
    snprintf(s_port_note, sizeof s_port_note, "%s", t);
    s_port_note_until = esp_timer_get_time() + 8000000;
    dmd_panel_port_note(t);
}

void port_field_set(int p) {
    if (!s_port_ta) return;
    char b[8];
    snprintf(b, sizeof b, "%d", p);
    lv_textarea_set_text(s_port_ta, b);
}

// The port typed in the panel, applied when the field is done (Enter, the focus leaving it, the panel
// closing). Kept for the next start (tick) only once the server listens on it; out of range or not opened:
// said under the field, which shows the port in use again.
void port_apply_cb(lv_event_t *) {
    if (!s_port_ta) return;
    const char *t = lv_textarea_get_text(s_port_ta);
    const long p = t && *t ? strtol(t, nullptr, 10) : 0;
    if (p < 1 || p > 65535) {
        port_note(nv_tr(NV_STR_DMDX_PORT_RANGE));
        port_field_set(s_port);
        return;
    }
    nv_dmd_status_t st;
    nv_dmd_get_status(&st);
    if (p == s_port && st.listening) return;   // (the same port again while nothing listens: a retry)
    char b[96];
    // only the Wi-Fi server moves: a game on USB keeps its DMD. False: it stayed on the port in use
    if (!nv_dmd_set_port((int)p)) {
        snprintf(b, sizeof b, nv_tr(NV_STR_DMD_PORT_FAIL_FMT), (int)p);
        port_note(b);
        port_field_set(s_port);
        return;
    }
    s_port = (int)p;   // the PC reconnects with retry = true
    s_port_saved = false;
    s_started_us = esp_timer_get_time();
    port_note("");
}

// Twice a second: a new port is saved once the server listens on it; one that never opens is said under
// the field (and on the waiting page) and not kept.
void port_check(const nv_dmd_status_t &st) {
    const int64_t now = esp_timer_get_time();
    if (!s_port_saved && st.listening && st.port == s_port) {
        s_port_saved = true;
        nv_config_set_int("dmd_port", s_port);
    }
    char b[96];
    if (s_port_note[0] && now < s_port_note_until) snprintf(b, sizeof b, "%s", s_port_note);
    else if (!st.listening && now - s_started_us > 3000000) snprintf(b, sizeof b, nv_tr(NV_STR_DMD_PORT_FAIL_FMT), s_port);
    else b[0] = 0;
    dmd_panel_port_note(b);
}

// ================================================================== pictures made on the board

uint32_t mono_colour(void) { return kColors[s_color_idx] >= 0 ? (uint32_t)kColors[s_color_idx] : 0xFF4500; }

// The test screen: "TEST" and the seconds left on top, the monochrome colour in 16 steps, the primaries +
// white as bars, a one-dot border and a solid block in the top-left corner (it must show top-left:
// orientation check).
void draw_test(int left) {
    uint32_t *p = s_testpx;
    const uint32_t mono = mono_colour();
    char t[16];
    snprintf(t, sizeof t, "TEST %d", left);
    dmd_text(p, kLocalW, 16, t, mono);
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

void end_test(void) { s_test_until_us = 0; }   // the next tick hands the screen back

// The idle clock: HH:MM large with the colon blinking every second (12 or 24 hours, as the system's clock),
// the date below, round dots over the whole panel. False when the time is not set yet.
bool draw_clock(void) {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    if (tmv.tm_year + 1900 < 2025) return false;
    const int phase = (tmv.tm_hour * 60 + tmv.tm_min) * 2 + (tmv.tm_sec & 1);
    if (phase == s_clock_shown) return true;   // nothing changed
    char hm[8], date[40];   // room for any int the compiler can imagine
    const int h12 = tmv.tm_hour % 12 ? tmv.tm_hour % 12 : 12;
    if (nv_time_is_24h()) snprintf(hm, sizeof hm, "%02d%c%02d", tmv.tm_hour, (tmv.tm_sec & 1) ? ' ' : ':', tmv.tm_min);
    else snprintf(hm, sizeof hm, "%d%c%02d", h12, (tmv.tm_sec & 1) ? ' ' : ':', tmv.tm_min);
    snprintf(date, sizeof date, "%02d.%02d.%04d", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900);
    const uint32_t c = mono_colour();
    const uint32_t dim = ((c >> 16 & 0xFF) * 3 / 5) << 16 | ((c >> 8 & 0xFF) * 3 / 5) << 8 | (c & 0xFF) * 3 / 5;
    // rows 0..51: the time (5x7 glyphs at x4 = 28 dots tall, centred); rows 52..74: the date (x2)
    dmd_text(s_local, kClockW, 52, hm, c);
    dmd_text(s_local + 52 * kClockW, kClockW, kClockH - 52, date, dim);
    s_clock_shown = phase;
    return true;
}

// LVGL tick, 10 Hz: the test screen for a while (its countdown), else the clock when the PC is silent.
void local_tick(void) {
    if (!s_local) return;
    const int64_t now = esp_timer_get_time();
    if (now < s_test_until_us && s_testpx) {
        const int left = (int)((s_test_until_us - now + 999999) / 1000000);
        if (left != s_test_left) {   // a new second: drawn again, the display task shows it
            s_test_left = left;
            draw_test(left);
            s_test_gen = s_test_gen + 1;
        }
        s_test_on = true;
        s_clock_shown = -1;
        return;
    }
    s_test_on = false;
    s_test_left = -1;
    nv_dmd_status_t st;
    nv_dmd_get_status(&st);
    const bool pc_silent = st.last_pc_us && now - st.last_pc_us > kIdleUs;
    // the menu has the screen; or a game runs (its DMD may be silent on a still picture: never the clock over it)
    if (!s_clock || !pc_silent || s_menu_on || st.bridge || dmd_menu_in_game()) { s_clock_shown = -1; return; }
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

// The address a PC on Wi-Fi puts in DmdDevice.ini ("no address yet" without Wi-Fi).
void ws_url(char *out, size_t n) {
    char ip[16];
    my_ip(ip, sizeof ip);
    if (ip[0]) snprintf(out, n, "ws://%s:%d/dmd", ip, s_port);
    else snprintf(out, n, "%s", nv_tr(NV_STR_DMDX_NO_ADDR));
}

void refresh_page(void) {
    if (!s_page_url) return;
    char ip[16], url[64], setup[300], b[448];
    my_ip(ip, sizeof ip);
    ws_url(url, sizeof url);
    nv_dmd_status_t st;
    nv_dmd_get_status(&st);
    char why[128];
    if (dmd_menu_in_game()) {   // launched from the menu or Popper: the DMD comes as dmdext starts
        snprintf(why, sizeof why, nv_tr(NV_STR_DMDM_INGAME_FMT), dmd_menu_game());
        nv_kit_label_set(s_page_url, why);
        if (s_big_w) snprintf(b, sizeof b, nv_tr(NV_STR_DMD_TOO_BIG_FMT), s_big_w, s_big_h);   // the reason first
        else snprintf(b, sizeof b, "%s", nv_tr(NV_STR_DMD_WAITING));
        nv_kit_label_set(s_page_msg, b);
        return;
    }
    nv_kit_label_set(s_page_url, url);
    snprintf(setup, sizeof setup, nv_tr(NV_STR_DMD_SETUP_FMT), url);
    // The first line says what is wrong, if anything; the setup lines always follow (USB works without
    // Wi-Fi, and a port problem only concerns Wi-Fi).
    const char *first = nv_tr(NV_STR_DMD_WAITING);
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

// dmdext's frame formats as the panel names them: shades of the one colour, colours, RGB.
void fmt_name(const char *f, char *out, size_t n) {
    if (!strcmp(f, "gray2")) snprintf(out, n, nv_tr(NV_STR_DMDX_SHADES_FMT), 4);
    else if (!strcmp(f, "gray4")) snprintf(out, n, nv_tr(NV_STR_DMDX_SHADES_FMT), 16);
    else if (!strcmp(f, "coloredGray2")) snprintf(out, n, nv_tr(NV_STR_DMDX_COLOURS_FMT), 4);
    else if (!strcmp(f, "coloredGray4")) snprintf(out, n, nv_tr(NV_STR_DMDX_COLOURS_FMT), 16);
    else if (!strcmp(f, "rgb24") || !strcmp(f, "ZeDMD")) snprintf(out, n, "RGB");
    else if (!strcmp(f, "clock")) snprintf(out, n, "%s", nv_tr(NV_STR_DMDX_FMT_CLOCK));
    else if (!strcmp(f, "test")) snprintf(out, n, "%s", nv_tr(NV_STR_DMD_TEST));
    else if (!strcmp(f, "score")) snprintf(out, n, "%s", nv_tr(NV_STR_DMDX_FMT_SCORE));
    else snprintf(out, n, "%s", f);
}

void refresh_status(void) {
    if (!s_panel) return;
    char line[220], addr[200], url[64];
    nv_dmd_status_t st;
    nv_dmd_get_status(&st);
    char pc[48];
    const char *wifi = nv_tr(NV_STR_WIFI);
    if (st.client && st.usb) snprintf(pc, sizeof pc, "%s (%s) + USB", st.peer, wifi);
    else if (st.client) snprintf(pc, sizeof pc, "%s (%s)", st.peer, wifi);
    else if (st.usb) snprintf(pc, sizeof pc, "USB");
    else snprintf(pc, sizeof pc, "%s: %s", nv_tr(NV_STR_DMD_ST_PC), nv_tr(NV_STR_DMD_ST_NONE));
    size_t o = (size_t)snprintf(line, sizeof line, "%s", pc);
    if (st.w && o < sizeof line) {
        char f[48];
        fmt_name(st.fmt, f, sizeof f);
        o += (size_t)snprintf(line + o, sizeof line - o, "  \xC2\xB7  %s %dx%d  \xC2\xB7  %u.%u %s", f, st.w, st.h,
                              (unsigned)(st.fps10 / 10), (unsigned)(st.fps10 % 10), nv_tr(NV_STR_DMD_ST_FPS));
    }
    if (st.game[0] && o < sizeof line) snprintf(line + o, sizeof line - o, "  \xC2\xB7  %s", st.game);
    ws_url(url, sizeof url);
    size_t ao = (size_t)snprintf(addr, sizeof addr, "%s:  %s", wifi, url);
    char ver[16];
    bool popper = false;
    if (dmd_menu_bridge_info(ver, sizeof ver, &popper) && ao < sizeof addr)   // the table menu's side of the PC
        snprintf(addr + ao, sizeof addr - ao, "  \xC2\xB7  %s %s  \xC2\xB7  Popper: %s", nv_tr(NV_STR_DMDX_BRIDGE), ver,
                 nv_tr(popper ? NV_STR_DMDX_POPPER_OK : NV_STR_DMDX_POPPER_NO));
    dmd_panel_status(line, addr);
    // the first button goes back to what was on screen: the DMD while a PC drives it, else just back
    const bool pc_on = st.client || st.usb || s_in_game || (st.last_pc_us && esp_timer_get_time() - st.last_pc_us < kIdleUs);
    dmd_panel_resume_label(nv_tr(pc_on ? NV_STR_DMD_RESUME : NV_STR_DMDM_BACK));
    port_check(st);
    const int gid = dmd_menu_game_id();
    const int tf = gid ? dmd_app_table_fit(gid) : -1;
    // a rotated screen: what the chosen layout comes to (see rot_fit)
    const int fa = s_fit_cfg == kFitAuto ? DMD_FIT_SQUARE : s_fit_cfg, ft = tf == kFitAuto ? DMD_FIT_SQUARE : tf;
    const bool rot = rot_fit(fa) != fa || (tf >= 0 && rot_fit(ft) != ft);
    dmd_panel_layout(s_fit_cfg, tf, rot ? nv_tr(NV_STR_DMDX_ROT_CLASSIC) : nullptr);
}

void set_panel(bool open) {
    if (!s_panel) return;
    s_panel_open = open;
    if (open) lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    if (open) refresh_status();
    if (!open && s_port_ta) port_apply_cb(nullptr);
    if (!open && s_bright_timer) bright_save();
}

NV_PSRAM_BSS lv_obj_t *s_leave = nullptr;   // the question before leaving, while on screen (see leave_guard)
void leave_close(void);

void back_cb(void) {
    if (s_leave) { leave_close(); return; }   // Esc on the question: stay
    if (s_test_on) { end_test(); return; }   // Esc ends the test screen early
    if (dmd_comp_is_open()) { dmd_comp_close(); return; }   // the layout's previews, opened from the panel
    if (!s_panel_open && dmd_menu_back()) return;   // a sub-page of the menu first
    set_panel(!s_panel_open);
}
void open_panel(void) { set_panel(true); }
void panel_resume(void) { set_panel(false); }
void exit_app(void) { nv_ui_close_app(); }   // (dmd_close keeps a typed port and the brightness)

// A port typed but not applied yet: kept for the next start (no restart now: the app is stopping). Every way
// out comes here, automation's /api/ui/home included.
void port_keep_typed(void) {
    if (!s_port_ta) return;
    const char *t = lv_textarea_get_text(s_port_ta);
    const long p = t && *t ? strtol(t, nullptr, 10) : 0;
    if (p >= 1 && p <= 65535 && p != s_port) nv_config_set_int("dmd_port", (int)p);
}
void panel_test(void) {
    s_test_until_us = esp_timer_get_time() + 10000000;   // 10 s, then back to the PC / clock / page
    s_test_left = -1;
    set_panel(false);
}
void layout_closed(void) { refresh_status(); }
void panel_layout(void) { dmd_comp_open_all(s_root, layout_closed); }

bool key_cb(uint32_t key, uint8_t, uint8_t) {
    if (key != LV_KEY_ENTER && key != ' ') return false;
    if (s_leave) return false;   // the question's focused button takes it
    if (s_test_on) { end_test(); return true; }
    // only for the DMD, its art or the waiting page themselves: the menu, its quick actions, its dialogs and
    // the composition screen take Enter / Space for their own buttons
    if (s_panel_open || s_menu_on || dmd_menu_covering()) return false;
    set_panel(true);
    return true;
}

void root_press_cb(lv_event_t *e) {
    const lv_event_code_t c = lv_event_get_code(e);
    // a tap on the DMD (or the backbox art): the quick actions (on the test screen: its end); a long press: the panel
    if (c == LV_EVENT_SHORT_CLICKED) {
        if (s_test_on && !s_hold_fired) { end_test(); return; }
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
    if (gid != s_lit_gid) {   // another game: the rebuilt DMD learns its lit rows again
        s_lit_gid = gid;
        s_lit_lo = s_lit_hi = -1;
    }
    if (gid != s_fit_gid) {   // another game, or its settings changed: read again
        s_fit_gid = gid;
        s_fit_tbl = gid ? dmd_app_table_fit(gid) : -1;
        const bool rb = dmd_app_table_rebuild(gid);
        if (rb != s_rb_on) {
            s_rb_on = rb;
            s_look_gen = s_look_gen + 1;
        }
        const int dot = dmd_app_table_dot(gid);
        if (dot != s_dot) {
            s_dot = dot;
            s_look_gen = s_look_gen + 1;
        }
    }
    int eff = s_fit_tbl >= 0 ? s_fit_tbl : s_fit_cfg;
    if (eff == kFitAuto) eff = auto_fit();
    eff = rot_fit(eff);
    if (eff != s_fit) {
        s_fit = eff;
        s_look_gen = s_look_gen + 1;
    }
    s_menu_ready = dmd_menu_ready();
    // what the display task needs from this side, as flags (it never calls into LVGL or the menu)
    const bool in_game = dmd_menu_in_game();
    const bool menu_cover = dmd_menu_covering();
    s_in_game = in_game;
    s_cover = (nv_ui_shade_is_open() ? 4 : 0) | (nv_ui_chrome_over_app() ? 8 : 0) | (menu_cover ? 16 : 0) | (s_leave ? 32 : 0);
    const int64_t gus = dmd_menu_game_start_us();
    if (gus != s_game_us) {
        portENTER_CRITICAL(&s_mux);
        s_game_us = gus;
        portEXIT_CRITICAL(&s_mux);
    }
    local_tick();   // (the test screen's state first: the art and the menu step aside for it)
    const bool test = s_test_on;
    // backglass only, during a game: the art has the screen (the menu's "in game" page would cover it)
    s_art_screen = s_fit == kFitBackglass && s_art_ready && in_game && !menu_cover && !test;
    s_menu_on = dmd_menu_tick(!s_showing && !s_panel_open && !s_art_screen && !test);
    // backbox: the running table's backglass above the DMD, as tall as the DMD leaves it
    // (backglass only: the whole screen, while a game runs and nothing covers it)
    // (FullDMD: its art on the whole screen, the DMD in its window; without it, the backbox)
    const bool bg_only = s_fit == kFitBackglass;
    dmd_rect_t win{};
    const lv_image_dsc_t *full = s_fit == kFitFullDmd || s_fit == kFitFullWin ? dmd_menu_game_full(&win)
                                 : s_fit == kFitB2sWin                       ? dmd_menu_game_grill(&win)
                                                                             : nullptr;
    if (full && (!s_full_ready || win.x != s_win.x || win.y != s_win.y || win.w != s_win.w || win.h != s_win.h)) {
        portENTER_CRITICAL(&s_mux);
        s_win = win;
        s_win_gen = s_win_gen + 1;
        portEXIT_CRITICAL(&s_mux);
    }
    s_full_ready = full != nullptr;
    const int art_h = bg_only ? kScrH : dmd_app_art_h(s_lw, s_lh, s_dot);
    const lv_image_dsc_t *art =
        full ? full
             : s_fit == kFitBackbox || s_fit == kFitFullDmd || fit_in_win(s_fit) || bg_only ? dmd_menu_game_art(art_h)
                                                                                           : nullptr;
    s_art_ready = art != nullptr;
    if (s_art) {
        nv_dmd_status_t ds;
        nv_dmd_get_status(&ds);
        const bool on = art && !test && strcmp(ds.fmt, "clock") &&
                        (bg_only ? in_game && !s_panel_open && !menu_cover : s_showing);
        if (on && lv_image_get_src(s_art) != art) lv_image_set_src(s_art, art);
        // "table art": only the art above its window (the window's frame would sit over the DMD's band)
        const int32_t art_hh = full && s_fit == kFitFullDmd && s_win.y > 0 ? s_win.y : LV_SIZE_CONTENT;
        if (lv_obj_get_style_height(s_art, LV_PART_MAIN) != art_hh) lv_obj_set_height(s_art, art_hh);
        if (on == lv_obj_has_flag(s_art, LV_OBJ_FLAG_HIDDEN)) {
            if (on) lv_obj_remove_flag(s_art, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_art, LV_OBJ_FLAG_HIDDEN);
        }
    }
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
    if (s_panel_open) {
        refresh_status();
    } else if (!s_port_saved) {   // a new port is kept once the server listens on it, panel open or not
        nv_dmd_status_t st;
        nv_dmd_get_status(&st);
        port_check(st);
    }
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

void make_port(lv_obj_t *row) {   // the system's number field (its keyboard), dark as the panel
    char ph[8];
    snprintf(ph, sizeof ph, "%d", NV_DMD_PORT_DEFAULT);
    s_port_ta = nv_kit_textarea_ex(row, ph, true, NV_IME_NUMBER, NV_IME_RET_DONE);
    lv_textarea_set_accepted_chars(s_port_ta, "0123456789");
    lv_textarea_set_max_length(s_port_ta, 5);
    lv_obj_set_size(s_port_ta, lv_pct(100), lv_pct(100));
    port_field_set(s_port);
    if (s_port_typed[0]) lv_textarea_set_text(s_port_ta, s_port_typed);   // typed before a rebuild: still to apply
    s_port_typed[0] = 0;
    lv_obj_add_event_cb(s_port_ta, port_apply_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(s_port_ta, port_apply_cb, LV_EVENT_DEFOCUSED, nullptr);
}

void panel_exit(void);
void build_panel(lv_obj_t *root) {
    static DmdPanelIo io;
    io.theme = s_theme;
    io.color = s_color_idx;
    io.bright = s_bright;
    io.shape = s_shape;
    io.rot = s_rot;
    io.glow = s_glow;
    io.clock = s_clock;
    io.boot = boot_enabled();
    io.colors = kColors;
    io.ncolors = kNColors;
    io.resume = panel_resume;
    io.test = panel_test;
    io.exit = panel_exit;   // asked first, as every way out
    io.layout = panel_layout;
    io.set_theme = set_theme;
    io.set_color = set_color;
    io.set_bright = set_bright;
    io.set_shape = set_shape;
    io.set_glow = set_glow;
    io.set_rot = set_rot;
    io.set_clock = set_clock;
    io.set_boot = set_boot;
    io.make_port = make_port;
    lv_obj_t *re = nullptr;
    s_panel = dmd_panel_build(root, &io, &re);
    nv_focus_skip(s_panel);
    if (re) nv_focus_prefer(re);
}

void fullscreen_async(void *) { nv_ui_app_fullscreen(true); }

// ---- leaving the app: always asked (the cabinet then has no DMD and no table menu until it is opened
// again). Every way out goes through it: the system's (nv_ui's leave guard: Home / Recents gestures, Alt+F4,
// Win...) and the panel's "Close the DMD". Automation (/api/ui/home, before flashing) is never asked.
// The question's veil is deleted once its own button's event is over: by an async call of ours, which the
// teardown can cancel (the veil may go with the app's widgets first: lv_obj_delete_async could not be undone).
NV_PSRAM_BSS lv_obj_t *s_leave_dying = nullptr;
void leave_gone_async(void *o) {
    s_leave_dying = nullptr;
    lv_obj_delete((lv_obj_t *)o);
}
void leave_close(void) {
    if (s_leave) {
        if (lv_async_call(leave_gone_async, s_leave) == LV_RESULT_OK) s_leave_dying = s_leave;
        else lv_obj_add_flag(s_leave, LV_OBJ_FLAG_HIDDEN);   // (no memory for the call: gone with the app)
    }
    s_leave = nullptr;
    if (s_panel_open && s_panel) lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}
// The widgets go (closed, rebuilt): the question with them, and no late deletion of its veil.
void leave_forget(void) {
    if (s_leave_dying) lv_async_call_cancel(leave_gone_async, s_leave_dying);
    s_leave_dying = nullptr;
    s_leave = nullptr;
}
void leave_ok(void) {
    s_leave = nullptr;   // deleted with the app
    nv_ui_set_leave_guard(nullptr);
    exit_app();
}
bool leave_guard(void) {
    if (s_leave) {   // asked already: back in front (a screen rebuilt meanwhile may have covered it)
        lv_obj_move_foreground(s_leave);
        return true;
    }
    if (!s_root) return true;
    lv_obj_t *stay = nullptr;
    s_leave = dmd_panel_ask(s_root, nv_tr(NV_STR_DMDX_LEAVE_TITLE),
                            nv_tr(dmd_menu_in_game() ? NV_STR_DMDX_LEAVE_GAME : NV_STR_DMDX_LEAVE_IDLE),
                            nv_tr(NV_STR_DMDX_STAY), nv_tr(NV_STR_DMDX_LEAVE_OK), leave_close, leave_ok, &stay);
    if (!s_leave) return false;   // no memory for the question: not a reason to keep the user in
    s_cover = s_cover | 32;       // the DMD steps aside at once (Alt+F4 while it is on screen)
    nv_focus_skip(s_leave);
    if (s_panel_open && s_panel) lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);   // Tab stays in the question
    if (stay) {
        nv_focus_prefer(stay);
        nv_focus_set(stay);   // the safe choice under Enter
    }
    return true;
}
void panel_exit(void) { leave_guard(); }

// Everything the app runs, stopped: the display task joined (it detaches the layer first), the receivers off
// (the cabinet has no DMD until the app opens again), the buffers freed, the system's gestures given back.
// save_place: the menu's place kept for a suspended task. Idempotent: dmd_close does it while the widgets
// are alive, the root's deletion comes after (nothing left then) or without a close (all of it).
void app_stop(bool save_place) {
    lv_async_call_cancel(fullscreen_async, nullptr);
    leave_forget();
    nv_ui_set_leave_guard(nullptr);
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    s_keep = false;
    if (!s_open) return;
    s_open = false;
    port_keep_typed();
    if (s_bright_timer) bright_save();
    dmd_menu_destroy(save_place);   // (the composition screen with it, without its callback)
    s_menu_on = false;
    if (s_task) {
        s_run = false;
        xSemaphoreTake(s_task_done, portMAX_DELAY);   // it detaches the layer before exiting
        s_task = nullptr;
    }
    nv_dmd_stop();
    if (s_masks) { heap_caps_free(s_masks); s_masks = nullptr; }
    if (s_local) { heap_caps_free(s_local); s_local = nullptr; }
    if (s_testpx) { heap_caps_free(s_testpx); s_testpx = nullptr; }
    if (s_tint) { heap_caps_free(s_tint); s_tint = nullptr; }
    s_tint_gen = 0;
    s_test_until_us = 0;
    s_test_on = false;
    s_ltest = false;
    s_clock_shown = -1;
    if (s_sprites) { heap_caps_free(s_sprites); s_sprites = nullptr; }
    nv_ui_set_exit_locked(false);
    nv_ui_set_shade_gesture_enabled(true);
    nv_ui_set_back_handler(nullptr);
    nv_ui_app_fullscreen(false);
    lv_obj_invalidate(lv_screen_active());   // the launcher repaints over the last DMD frame
    s_art_ready = false;
    s_full_ready = false;
    s_panel_open = false;
    s_showing = false;
}

// NvApp.close, on the LVGL thread with every widget still alive. Closed or left for another app: all of it
// stops (the cabinet loses its DMD either way); the menu's place is kept only for a task that comes back
// (closed for good, the system drops it at once, and a save would push another task's state out).
// A rebuild: only what points at the widgets. The DMD stays on the cabinet, the PC stays connected, a game,
// the test screen and the panel go on: the display task reads flags and buffers, never a widget.
void dmd_close(nv_app_teardown_t why) {
    if (why != NV_APP_REBUILD) {
        app_stop(why == NV_APP_SWITCH);
        return;
    }
    s_keep = s_open;
    lv_async_call_cancel(fullscreen_async, nullptr);
    leave_forget();   // (the question goes with the widgets: the DMD may come back at once)
    s_cover = s_cover & ~32;
    s_port_typed[0] = 0;
    if (s_port_ta) {   // a port typed, not applied yet: into the new field
        const char *t = lv_textarea_get_text(s_port_ta);
        char cur[12];
        snprintf(cur, sizeof cur, "%d", s_port);
        if (t && strcmp(t, cur)) snprintf(s_port_typed, sizeof s_port_typed, "%s", t);
    }
    dmd_menu_drop_ui();   // (the composition screen with it, without its callback)
}

void page_deleted(lv_event_t *) {
    if (s_keep) {   // a rebuild: dmd_close dropped what pointed at the widgets; the rest runs on
        lv_async_call_cancel(fullscreen_async, nullptr);
        leave_forget();
        if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    } else {
        app_stop(true);   // deleted without a close: all of it, as a switch would
    }
    s_root = s_page = s_page_url = s_page_msg = s_panel = s_port_ta = s_art = nullptr;
    dmd_panel_destroyed();
}


void dmd_build(lv_obj_t *content) {
    // After close(NV_APP_REBUILD): the same app on new widgets. Anything else starts afresh (a build without
    // the close before it stops what still runs first: never two receivers nor two display tasks).
    const bool again = s_keep && s_open && nv_ui_app_rebuilding();
    if (!again) {
        app_stop(true);
        load_cfg();
        s_panel_open = false;
        s_showing = false;
        s_big_w = 0;
        s_big_h = 0;
        if (!s_masks) s_masks = (dmd_masks_t *)heap_caps_malloc(sizeof(dmd_masks_t), MALLOC_CAP_SPIRAM);
        if (!s_local) s_local = (uint32_t *)heap_caps_calloc(kClockW * kClockH, 4, MALLOC_CAP_SPIRAM);   // the larger picture
        if (!s_testpx) s_testpx = (uint32_t *)heap_caps_calloc(kLocalW * kLocalH, 4, MALLOC_CAP_SPIRAM);
        s_test_on = false;
        s_test_left = -1;
        s_cover = 0;        // until the first tick works them out (an earlier opening's are stale)
        s_in_game = false;
        s_game_us = 0;
        s_strips_dead = s_strip_stuck[0] || s_strip_stuck[1];   // a copy given up for lost: no strips this boot (a retired DMA stays retired: nv_2d_copy_start then says so at once)
        s_clock_shown = -1;
        if (!s_sprites) s_sprites = (dmd_sprites_t *)heap_caps_malloc(sizeof(dmd_sprites_t), MALLOC_CAP_SPIRAM);
        if (s_sprites) dmd_sprites_reset(s_sprites);
        if (!s_task_done) s_task_done = xSemaphoreCreateBinary();
        nv_dmd_set_color(kColors[s_color_idx]);
        nv_dmd_start(s_port);
        s_started_us = esp_timer_get_time();
        s_open = true;
    }
    s_keep = false;

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
    dmd_menu_build(s_root, open_panel, again);   // above the page, below the panel
    build_panel(s_root);
    if (s_panel_open) set_panel(true);   // rebuilt with the panel open: open again (the DMD stayed aside)

    nv_ui_set_back_handler(back_cb);
    nv_ui_set_leave_guard(leave_guard);
    nv_ui_set_key_handler(key_cb);
    nv_ui_set_shortcuts(kKeys, 2);
    s_sys_locked = -1;   // the tick sets them from what is on screen
    // After build(): the classic desktop opens built-in apps windowed at start, a later request
    // gets the whole panel (as the video player's fullscreen button does).
    lv_async_call(fullscreen_async, nullptr);

    if (s_masks && s_task_done && !s_task) {   // (a rebuild finds it running)
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
    .close = dmd_close,
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

void dmd_app_set_fit_all(int fit) {
    if (fit < DMD_FIT_FILL || fit > kFitLast || fit == s_fit_cfg) return;
    s_fit_cfg = fit;
    nv_config_set_int("dmd_fit", fit);
    want_fulldmd();
    s_look_gen = s_look_gen + 1;
}

void dmd_app_dmd_size(int *w, int *h) {
    *w = s_lw;
    *h = s_lh;
}

int dmd_app_fit_shown(int fit, bool full, bool art, int w, int h) { return fit_shown(fit, full, art, w, h); }

bool dmd_app_fit_geom_wh(int fit, int dot, const dmd_rect_t *win, int w, int h, dmd_geom_t *g) {
    if (fit < DMD_FIT_FILL || fit > kFitLast || fit == kFitAuto || fit == kFitBackglass) return false;
    dmd_rect_t r{};
    if (win) r = *win;
    if (fit_in_win(fit) && (r.w <= 0 || r.h <= 0)) return false;
    return geom_for(fit, w > 0 ? w : 128, h > 0 ? h : 32, s_rot, dot, r, g);
}

static NV_PSRAM_BSS dmd_geom_t s_art_geom;   // (~2 KB: not on the LVGL task's stack)
int dmd_app_art_h(int w, int h, int dot) {
    // LVGL thread (tick, the composition): computed again only when the DMD changes
    dmd_geom_t &g = s_art_geom;
    static int kw = -1, kh = -1, kd = -1, oy = kScrH - 256;
    if (w <= 0 || h <= 0) w = 128, h = 32;
    if (w != kw || h != kh || dot != kd) {
        kw = w;
        kh = h;
        kd = dot;
        oy = geom_for(kFitBackbox, w, h, 0, dot, dmd_rect_t{}, &g) && g.oy > 0 ? g.oy : kScrH - 256;
    }
    return oy;
}

void dmd_app_rebuild_rows(int gid, int h, int *r0, int *n) {
    *r0 = 0;
    *n = h;
    if (gid > 0 && gid == s_lit_gid && h == s_lh && dmd_app_table_rebuild(gid)) dmd_rebuild_rows(h, s_lit_lo, s_lit_hi, r0, n);
}

// (not nv_dmd_acquire: that copy belongs to the display, which may be drawing from it)
bool dmd_app_snapshot(uint32_t *dst, size_t cap, int *w, int *h) { return nv_dmd_copy(dst, cap, w, h); }

bool dmd_app_fit_geom(int fit, int dot, const dmd_rect_t *win, dmd_geom_t *g) {
    if (fit == kFitAuto) fit = auto_fit();
    if (fit < DMD_FIT_FILL || fit > kFitLast || fit == kFitBackglass) return false;
    dmd_rect_t w{};
    if (win) w = *win;
    else if (fit == kFitFullWin && !dmd_menu_game_full(&w)) return false;
    else if (fit == kFitB2sWin && !dmd_menu_game_grill(&w)) return false;
    return dmd_app_fit_geom_wh(fit, dot, &w, s_lw, s_lh, g);
}

bool dmd_app_fit_rect(int fit, int dot, const dmd_rect_t *win, dmd_rect_t *out) {
    dmd_geom_t *g = (dmd_geom_t *)heap_caps_malloc(sizeof(dmd_geom_t), MALLOC_CAP_SPIRAM);
    if (!g) return false;
    const bool ok = dmd_app_fit_geom(fit, dot, win, g);
    if (ok) *out = dmd_rect_t{g->ox, g->oy, g->gw, g->gh};
    heap_caps_free(g);
    return ok;
}

void dmd_app_look(int *shape, bool *glow, int *bright, int *rot) {
    *shape = s_shape;
    *glow = s_glow;
    *bright = s_bright;
    *rot = s_rot;
}

void dmd_app_set_table_rebuild(int game_id, bool on) {
    if (game_id <= 0) return;
    char k[24];
    snprintf(k, sizeof k, "dmdrb_%d", game_id);
    nv_config_set_int(k, on ? 1 : 0);
    if (game_id == s_fit_gid && on != s_rb_on) {
        s_rb_on = on;
        s_look_gen = s_look_gen + 1;   // drawn again with or without its rebuilt rows
    }
}
