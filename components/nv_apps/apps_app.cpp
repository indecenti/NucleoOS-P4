// apps_app — WASM app model UI (Phase 4).
//   * a native "Apps" manager listing every installed app (/sdcard/apps/<id>/), and
//   * one launcher tile per installed app, so WASM apps are first-class home entries. Opening a
//     tile goes through the normal open_app path, so the Memory Broker reserves the app's
//     manifest ram_budget before it runs (solo-mode discipline, for free).
// Runs are ASYNCHRONOUS: nv_wasm executes the module on a worker pthread and this UI polls it
// from an LVGL timer (output streaming, queued nv.toast delivery, Stop button, manifest-declared
// timeout watchdog). The LVGL thread never blocks on a module. Native chrome; the WASM modules
// themselves are sandboxed by WAMR + gated on manifest permissions.
#include "apps_internal.h"
#include "nv_apps.h"         // nv_apps_store_installed (C linkage)

#include "nv_app.h"
#include "nv_ui.h"
#include "nv_ui_kit.h"
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_theme.h"
#include "nv_fonts.h"
#include "nv_notify.h"
#include "nv_wasm.h"
#include "nv_gesture.h"
#include "nv_open.h"       // ABI v7: installed apps as "Open with" targets + launch-file grant
#include "nv_appstore.h"   // remote catalog: install/update apps over Wi-Fi
#include "nv_telemetry.h"  // opt-in statistics: store uninstalls
#include "nv_wifi.h"      // store: tell "no Wi-Fi" from "store unreachable"
#include "nv_eth.h"       // store update watch: wired network counts as online too
#include "gallery_jpeg_hw.h" // store screenshots: HW JPEG decode + PPA scale
#include "nv_hal.h"   // nv_hal_touch_points — feed the game canvas full multi-touch
#include "nv_pins.h"  // NV_LCD_H_RES/V_RES: ABI v9 scaled canvas blits to the whole panel
#include "esp_cache.h" // msync the CPU-written canvas before the PPA reads it
#include "nv_config.h"  // restore user brightness when a backlight (ABI v4) app exits
#include "nv_mem_attr.h" // NV_PSRAM_BSS: cold UI tables out of internal SRAM
#include "nv_log.h"
#include <cstdarg>
#include "nv_sd.h"

#include "esp_heap_caps.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <ctime>

namespace {

// Installed apps discovered once at boot. PSRAM-backed and stable for the whole session: the
// launcher NvApp tiles point their .user here, so the records must outlive registration.
constexpr int  kMaxWasmApps = 64;   // Home tiles for installed apps (nv_ui's registry holds 96 with the natives)
nv_wasm_app_t *s_installed  = nullptr;
int            s_installed_n = 0;
NvApp         *s_tiles       = nullptr;   // one launcher descriptor per installed app
// ABI v7: one nv_open OPENER per app declaring manifest "opens", parallel to s_tiles. nv_open keeps
// the pointers (static lifetime), so both arrays are allocated once in PSRAM and never freed.
NvOpenHandler *s_open_h      = nullptr;
char         (*s_open_ids)[NV_OPEN_ID_MAX] = nullptr;   // "<appid>.open"

// Per-app launcher/store icon, picked from the compiled icon set (flash-resident, so no SD load at
// boot — unlike the deferred icon.argb path that boot-looped in 1.1.57). Known app ids map to a
// tailored icon; anything else falls back to a generic game/puzzle glyph. Keep ids in sync with the
// gen_icons.py "WASM app icons" block.
const lv_image_dsc_t *wasm_icon_for(const char *id, bool is_game) {
    static const struct { const char *id; const lv_image_dsc_t *ic; } kMap[] = {
        { "timer",   &nv_icon_wtimer },  { "torch",  &nv_icon_wtorch },
        { "pianino", &nv_icon_wpiano },  { "cannon", &nv_icon_wcannon },
        { "tanks",   &nv_icon_wtank },   { "abc123", &nv_icon_wabc },
        { "ciao",    &nv_icon_wcode },   { "wedge",  &nv_icon_wbug },
        { "bench",   &nv_icon_wbench }, { "meteo",  &nv_icon_wmeteo },
        { "deskhub", &nv_icon_wdeskhub },
    };
    if (id) for (auto &m : kMap) if (!strcmp(m.id, id)) return m.ic;
    return is_game ? &nv_icon_wgame : &nv_icon_wasm;   // sensible default for future/remote apps
}

// Manifest "file_types" kind string (already validated by nv_wasm) -> nv_open file kind.
nv_file_kind_t wasm_file_kind(const char *kind) {
    static const struct { const char *s; nv_file_kind_t k; } kMap[] = {
        { "text",  NV_FILE_TEXT },  { "image", NV_FILE_IMAGE }, { "audio",   NV_FILE_AUDIO },
        { "video", NV_FILE_VIDEO }, { "app",   NV_FILE_APP },   { "archive", NV_FILE_ARCHIVE },
    };
    if (kind) for (auto &m : kMap) if (!strcmp(m.s, kind)) return m.k;
    return NV_FILE_OTHER;
}

// ---- shared async runner panel (used by the tile view and the manager) -------------------------
// One run at a time (engine-enforced). The panel owns an LVGL poll timer while its run is in
// flight; teardown of the hosting screen aborts the run, so a background module can never
// outlive its UI (solo-mode discipline).

constexpr size_t   kUiOutCap  = 4096;
constexpr uint32_t kPollMs    = 120;

struct Runner {
    lv_obj_t     *status   = nullptr;   // status line ("Running..." / "OK - 12 ms" / error)
    lv_obj_t     *stop_btn = nullptr;
    lv_obj_t     *out      = nullptr;   // streamed nv.print output
    lv_timer_t   *timer    = nullptr;
    nv_wasm_app_t app{};                // copy of the app this panel started
    uint32_t      t0        = 0;        // lv_tick at start (timeout watchdog)
    bool          timed_out = false;
    bool          active    = false;    // this panel started the in-flight run
};
NV_PSRAM_BSS Runner s_run;              // LVGL thread only
char  *s_run_buf = nullptr;             // UI-side output accumulator (PSRAM)
size_t s_run_len = 0;

void runner_stop_timer(void) {
    if (s_run.timer) { lv_timer_delete(s_run.timer); s_run.timer = nullptr; }
}

void runner_finish(bool ok, uint32_t elapsed_ms, const char *err) {
    char line[128];
    if (ok) {
        snprintf(line, sizeof line, nv_tr(NV_STR_WASM_OK_FMT), (unsigned)elapsed_ms);
    } else if (s_run.timed_out) {
        snprintf(line, sizeof line, "%s", nv_tr(NV_STR_WASM_TIMEOUT));
    } else {
        snprintf(line, sizeof line, "%s", (err && err[0]) ? err : "error");
    }
    if (s_run.status) lv_label_set_text(s_run.status, line);
    if (!ok) nv_toast(NV_NOTE_ERROR, line);
    if (s_run.stop_btn) lv_obj_add_flag(s_run.stop_btn, LV_OBJ_FLAG_HIDDEN);
    s_run.active = false;
    runner_stop_timer();
}

void runner_drain_output(void) {
    if (!s_run_buf) return;
    char chunk[257];
    size_t k;
    bool changed = false;
    while ((k = nv_wasm_exec_read(chunk, sizeof chunk - 1)) > 0) {
        chunk[k] = '\0';
        const size_t room = kUiOutCap - 1 - s_run_len;
        const size_t take = k < room ? k : room;
        memcpy(s_run_buf + s_run_len, chunk, take);
        s_run_len += take;
        s_run_buf[s_run_len] = '\0';
        changed = true;
    }
    if (changed && s_run.out) lv_label_set_text(s_run.out, s_run_buf);
}

void runner_drain_toasts(void) {
    int kind; char msg[64];
    while (nv_wasm_exec_take_toast(&kind, msg, sizeof msg))
        nv_toast((nv_note_kind_t)kind, msg);   // same 0..3 order: info, ok, warn, error
}

void runner_poll(lv_timer_t *) {
    runner_drain_output();
    runner_drain_toasts();

    const nv_wrun_state_t st = nv_wasm_exec_state();
    if (st == NV_WRUN_DONE) {
        runner_drain_output();   // the tail may have landed after the drains above
        runner_drain_toasts();
        bool ok = false; uint32_t ms = 0; char err[128] = "";
        nv_wasm_exec_collect(&ok, &ms, err, sizeof err);
        runner_finish(ok, ms, err);
        return;
    }
    if (st == NV_WRUN_RUNNING && s_run.active && !s_run.timed_out &&
        lv_tick_elaps(s_run.t0) > s_run.app.timeout_ms) {
        s_run.timed_out = true;
        nv_wasm_exec_abort();    // worker lands in DONE; the next poll reports the timeout
    }
}

void runner_start(const nv_wasm_app_t *a) {
    if (!a || !s_run.status) return;
    if (!s_run_buf) {
        s_run_buf = (char *)heap_caps_malloc(kUiOutCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_run_buf) s_run_buf = (char *)malloc(kUiOutCap);
        if (!s_run_buf) return;
    }

    char err[96] = "";
    // Only the launcher tile's view runs this: an app opened on a file (nv_open) may read that file.
    const NvIntent *in = nv_open_intent();
    nv_wasm_exec_set_launch_file(in && in->verb == NV_INTENT_OPEN ? in->path : nullptr);
    if (!nv_wasm_exec_start(a, err, sizeof err)) {
        nv_toast(NV_NOTE_WARN, !strcmp(err, "busy") ? nv_tr(NV_STR_WASM_BUSY) : err);
        return;
    }

    s_run.app = *a;
    s_run.t0 = lv_tick_get();
    s_run.timed_out = false;
    s_run.active = true;
    s_run_len = 0;
    s_run_buf[0] = '\0';
    if (s_run.out) lv_label_set_text(s_run.out, "");
    lv_label_set_text(s_run.status, nv_tr(NV_STR_RUNNING));
    if (s_run.stop_btn) lv_obj_clear_flag(s_run.stop_btn, LV_OBJ_FLAG_HIDDEN);
    if (!s_run.timer) s_run.timer = lv_timer_create(runner_poll, kPollMs, nullptr);
}

void runner_stop_cb(lv_event_t *) { nv_wasm_exec_abort(); }

void runner_deleted(lv_event_t *) {
    runner_stop_timer();
    if (s_run.active) { nv_wasm_exec_abort(); s_run.active = false; }
    // an aborted worker parks in DONE; the engine auto-collects it on the next start
    s_run.status = s_run.stop_btn = s_run.out = nullptr;
}

// Status row + output card. Build LAST in the column; registers the teardown hook.
void runner_panel(lv_obj_t *col) {
    const NvTheme *th = nv_theme_get();

    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    s_run.status = lv_label_create(row);
    lv_label_set_text(s_run.status, "");
    lv_obj_set_style_text_font(s_run.status, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_run.status, th->text_dim, 0);
    lv_obj_set_flex_grow(s_run.status, 1);

    s_run.stop_btn = nv_kit_button(row, nv_tr(NV_STR_STOP), false);
    lv_obj_add_event_cb(s_run.stop_btn, runner_stop_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_run.stop_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *card = lv_obj_create(col);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, th->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, NV_RAD_MD, 0);
    lv_obj_set_style_pad_all(card, NV_SP_4, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, th->divider, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_run.out = lv_label_create(card);
    lv_label_set_text(s_run.out, "");
    lv_obj_set_width(s_run.out, lv_pct(100));
    lv_obj_set_style_text_font(s_run.out, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_run.out, th->text, 0);

    lv_obj_add_event_cb(col, runner_deleted, LV_EVENT_DELETE, nullptr);

    // Rebuilt while a run this panel started is still in flight (e.g. theme/lang re-render):
    // re-arm the poll timer so the run keeps streaming into the fresh widgets.
    if (s_run.active && nv_wasm_exec_state() != NV_WRUN_IDLE) {
        lv_label_set_text(s_run.status, nv_tr(NV_STR_RUNNING));
        lv_obj_clear_flag(s_run.stop_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_run_buf && s_run_len) lv_label_set_text(s_run.out, s_run_buf);
        if (!s_run.timer) s_run.timer = lv_timer_create(runner_poll, kPollMs, nullptr);
    }
}

// ---- app info card ------------------------------------------------------------------------------

// Info card + Run button for one app. Reused by the tile view and the manager rows.
void app_card(lv_obj_t *col, const nv_wasm_app_t *a, lv_event_cb_t run_cb, void *run_ud) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *card = lv_obj_create(col);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, th->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, NV_RAD_MD, 0);
    lv_obj_set_style_pad_all(card, NV_SP_4, 0);
    lv_obj_set_style_pad_row(card, NV_SP_2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text_fmt(title, "%s   v%s", a->name, a->version);
    lv_obj_set_style_text_font(title, &nv_font_20, 0);
    lv_obj_set_style_text_color(title, th->text_strong, 0);

    char perms[64];
    nv_wasm_perm_str(a->perms, perms, sizeof perms);
    lv_obj_t *meta = lv_label_create(card);
    lv_label_set_text_fmt(meta, "%s  ·  %s", a->id, perms);
    lv_obj_set_style_text_font(meta, &nv_font_14, 0);
    lv_obj_set_style_text_color(meta, th->text_dim, 0);

    lv_obj_t *limits = lv_label_create(card);
    lv_label_set_text_fmt(limits, "RAM %u KB  ·  stack %u KB  ·  timeout %u s  ·  ABI v%u",
                          (unsigned)(a->ram_budget / 1024), (unsigned)a->stack_kb,
                          (unsigned)(a->timeout_ms / 1000), (unsigned)a->abi);
    lv_obj_set_style_text_font(limits, &nv_font_14, 0);
    lv_obj_set_style_text_color(limits, th->text_dim, 0);

    lv_obj_t *btn = nv_kit_button(card, nv_tr(NV_STR_RUN), true);
    lv_obj_add_event_cb(btn, run_cb, LV_EVENT_CLICKED, run_ud);
}

// ---------------------------------------------------------------- ABI v2 game view
// A game app (nv_wasm_app_is_game) gets a full-bleed canvas instead of the console panel. The run
// is started here; nv_wasm double-buffers the RGB565 canvas and the guest draws into it on the
// worker thread. This LVGL timer shows finished frames (take_frame -> set_buffer -> invalidate)
// and forwards touch as the guest's input. Teardown aborts the run (solo-mode discipline).
struct GameView {
    lv_obj_t   *canvas  = nullptr;
    lv_obj_t   *overlay = nullptr;
    lv_timer_t *timer   = nullptr;
    bool        active  = false;
    // Wedge watchdog: gfx_present is the guest's only cooperative point, so its heartbeat going
    // quiet while RUNNING means an infinite loop that will never see want_stop. The watchdog
    // aborts the run — with THREAD_MGR in the WAMR build, terminate forcibly interrupts the
    // interpreter, so the run collects normally instead of freezing the engine until reboot.
    uint32_t    hb_seq  = 0;
    uint32_t    hb_tick = 0;
    bool        bl_touched = false;   // ABI v4: guest changed the backlight -> restore brightness on exit
    uint16_t   *last_fb = nullptr;    // ABI v6: last framebuffer set on the canvas (detect persist single-buffer)
    // Start retry (see gv_try_start): the app, its launch file and the "Starting..." label while
    // the previous app's aborted run is still unwinding.
    const nv_wasm_app_t *app = nullptr;
    lv_obj_t   *root    = nullptr;
    lv_obj_t   *status  = nullptr;
    lv_timer_t *retry   = nullptr;
    uint32_t    wait_t0 = 0;
    char        launch[NV_OPEN_PATH_MAX] = "";
    // ABI v9 scaled canvas (manifest "canvas_scale"): frames go straight to the panel through the
    // PPA (nv_hal_video_blit); LVGL only keeps an empty full-screen object for the chrome logic.
    // Panel space is PHYSICAL (landscape) whatever the UI rotation, so touch comes from the raw
    // GT911 points too, mapped back to canvas pixels with the blit's own geometry.
    int         fit_mode = -1;        // NV_HAL_BLIT_* or -1 = classic 1:1 LVGL canvas
    nv_hal_blit_geom_t fit_geom = {};
    uint16_t   *fit_last = nullptr;   // last frame shown (re-blit after an overlay closes)
    bool        fit_clear = true;     // black the letterbox bars on the next blit
    bool        fit_occluded = false; // shade / lock screen over the game: LVGL owns the pixels
    bool        loading = false;      // "Starting..." shown until the first frame arrives
};
GameView s_gv;
// Pop-down title bar state (PSRAM: internal RAM is full). Rows above the picture while the
// system title bar shows; a 1:1 canvas app is then shown scaled below it by the PPA.
NV_PSRAM_BSS struct { int fit_top; bool cv_scaled; } s_gvx;

// Manifest canvas_scale -> blit mode (fit never crops a game canvas: HUD and touch at the edges).
int gv_fit_mode_for(const nv_wasm_app_t *app) {
    switch (app->canvas_scale) {
    case NV_WASM_SCALE_FIT:     return NV_HAL_BLIT_FIT_EXACT;
    case NV_WASM_SCALE_STRETCH: return NV_HAL_BLIT_STRETCH;
    case NV_WASM_SCALE_ZOOM:    return NV_HAL_BLIT_ZOOM;
    default:                    return -1;
    }
}

// Show a canvas frame on the panel. The guest (worker thread) wrote it through the CPU cache:
// write it back to PSRAM before the PPA reads it. nv_hal_video_blit writes into the frame on
// screen and registers the panel as a direct region that nv_disp carries across LVGL swaps (so the
// view must end it — gv_fit_stop — whenever LVGL UI covers the game or the view goes). False = a
// swap held the buffer: this frame is skipped and the letterbox retried with the next one.
void gv_fit_blit(uint16_t *fr) {
    int w = 0, h = 0; nv_wasm_gfx_size(&w, &h);
    if (!fr || w <= 0 || h <= 0) return;
    esp_cache_msync(fr, (size_t)w * h * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    s_gv.fit_last = fr;
    if (nv_hal_video_blit(fr, w, h, w, 0, s_gvx.fit_top, NV_LCD_H_RES, NV_LCD_V_RES - s_gvx.fit_top,
                          s_gv.fit_mode, s_gv.fit_clear))
        s_gv.fit_clear = false;
}
// Stop owning the panel pixels (overlay opened, error screen, view closed).
void gv_fit_stop(void) {
    if (s_gv.fit_mode >= 0 || s_gvx.cv_scaled) nv_hal_video_blit_end();
    s_gvx.cv_scaled = false;
}

// Panel point -> canvas pixel through the blit geometry (inverse of the k/16 scale), clamped.
void gv_fit_map(int px, int py, int *cx, int *cy) {
    const nv_hal_blit_geom_t &g = s_gv.fit_geom;
    int w = 0, h = 0; nv_wasm_gfx_size(&w, &h);
    const int x = g.kx > 0 ? g.bx + (px - g.ox) * 16 / g.kx : 0;
    const int y = g.ky > 0 ? g.by + (py - g.oy) * 16 / g.ky : 0;
    *cx = x < 0 ? 0 : (x >= w ? w - 1 : x);
    *cy = y < 0 ? 0 : (y >= h ? h - 1 : y);
}
constexpr uint32_t kGameWedgeMs = 8000;   // generous: a legit frame never takes 8 s
// Longest wait for the previous app's run to wind down: a guest inside nv.http_get only sees the
// abort when that call returns (10 s timeout), everything else stops within a frame or two.
constexpr uint32_t kGameStartWaitMs = 12000;
constexpr uint32_t kGameRetryMs     = 50;

void gv_input_cb(lv_event_t *e) {
    if (!s_gv.canvas || s_gv.fit_mode >= 0) return;   // scaled canvas: raw touch, see gv_poll
    const lv_event_code_t code = lv_event_get_code(e);
    const int state = (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) ? 0 : 1;
    lv_indev_t *ind = lv_indev_active();
    if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    lv_area_t cc; lv_obj_get_coords(s_gv.canvas, &cc);
    int w = 0, h = 0; nv_wasm_gfx_size(&w, &h);
    int x = p.x - cc.x1, y = p.y - cc.y1;
    if (x < 0) x = 0;
    if (x >= w) x = w - 1;
    if (y < 0) y = 0;
    if (y >= h) y = h - 1;
    nv_wasm_gfx_set_input(x, y, state);
}

void gv_stop_timer(void) { if (s_gv.timer) { lv_timer_delete(s_gv.timer); s_gv.timer = nullptr; } }
void gv_stop_retry(void) { if (s_gv.retry) { lv_timer_delete(s_gv.retry); s_gv.retry = nullptr; } }

// Back gesture while a game is fullscreen -> forward to the game (it pops its own screen and exits
// on its own when at root); the app is NOT closed here.
void gv_back(void) { nv_wasm_gfx_request_back(); }

void gv_poll(lv_timer_t *) {
    int bl = nv_wasm_gfx_take_backlight();   // ABI v4: apply the guest's backlight request on THIS thread
    if (bl >= 0) { nv_hal_backlight_set(bl); s_gv.bl_touched = true; }
    int dx = 0, dy = 0, dw = 0, dh = 0;
    uint16_t *fr = nv_wasm_gfx_take_frame_ex(&dx, &dy, &dw, &dh);
    if (fr && s_gv.loading) {   // first frame: the game draws from here on
        s_gv.loading = false;
        if (s_gv.overlay) { lv_label_set_text(s_gv.overlay, ""); lv_obj_add_flag(s_gv.overlay, LV_OBJ_FLAG_HIDDEN); }
    }
    if (s_gv.fit_mode >= 0 && s_gv.canvas) {
        // Scaled canvas: whole frame straight to the panel (the small source keeps the PPA pass to
        // a few ms). Never over a system overlay — the notification shade or the lock screen are
        // LVGL's pixels; the blit would paint the game on top of them. Re-show the last frame (and
        // its letterbox) once they close, even if the game is idle and sends no new one.
        const bool occ = nv_ui_shade_is_open() || nv_ui_is_locked() || nv_ui_chrome_over_app();
        // System title bar popped down: fit the picture (and the touch mapping) below it.
        const int top = nv_ui_chrome_top();
        if (top != s_gvx.fit_top) {
            int cw = 0, ch = 0; nv_wasm_gfx_size(&cw, &ch);
            nv_hal_blit_geom_t g;
            if (cw > 0 && ch > 0 &&
                nv_hal_video_geom(cw, ch, 0, top, NV_LCD_H_RES, NV_LCD_V_RES - top, s_gv.fit_mode, &g)) {
                gv_fit_stop();                       // the old region goes back to LVGL (bar row)
                s_gv.fit_geom = g;
                s_gvx.fit_top = top;
                s_gv.fit_clear = true;
                if (!fr) fr = s_gv.fit_last;
            }
        }
        if (occ) {
            if (!s_gv.fit_occluded) gv_fit_stop();   // hand the pixels back before LVGL draws on them
            s_gv.fit_occluded = true;
        } else {
            if (s_gv.fit_occluded) { s_gv.fit_occluded = false; s_gv.fit_clear = true; if (!fr) fr = s_gv.fit_last; }
            if (fr) gv_fit_blit(fr);
        }
        if (s_gv.active) {   // raw panel touch -> canvas pixels (single pointer = finger 0, + ABI v3 set)
            int16_t px[NV_TOUCH_MAX], py[NV_TOUCH_MAX];
            const int n = occ ? 0 : nv_hal_touch_points(px, py, NV_TOUCH_MAX);
            int mx[NV_TOUCH_MAX], my[NV_TOUCH_MAX];
            for (int i = 0; i < n; i++) gv_fit_map(px[i], py[i], &mx[i], &my[i]);
            nv_wasm_gfx_set_multi(mx, my, n);
            static int lx = 0, ly = 0;   // a release keeps the last position (games tap on release)
            if (n > 0) { lx = mx[0]; ly = my[0]; }
            nv_wasm_gfx_set_input(lx, ly, n > 0 ? 1 : 0);
        }
    } else if (s_gv.canvas && nv_ui_chrome_top() > 0) {
        // 1:1 canvas app with the system title bar popped down: show its frame scaled below the bar
        // by the PPA (LVGL can't scale it without a transform layer); the canvas resumes after.
        const int top = nv_ui_chrome_top();
        if (fr) s_gv.last_fb = fr;
        uint16_t *src = s_gv.last_fb;
        int w = 0, h = 0; nv_wasm_gfx_size(&w, &h);
        if (!s_gvx.cv_scaled) { s_gvx.cv_scaled = true; s_gv.fit_clear = true; }
        if (src && w > 0 && h > 0) {
            esp_cache_msync(src, (size_t)w * h * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
            if (nv_hal_video_blit(src, w, h, w, 0, top, NV_LCD_H_RES, NV_LCD_V_RES - top,
                                  NV_HAL_BLIT_FIT_EXACT, s_gv.fit_clear))
                s_gv.fit_clear = false;
        }
    } else if (s_gvx.cv_scaled) {                      // bar gone: hand the panel back to the canvas
        nv_hal_video_blit_end();
        s_gvx.cv_scaled = false;
        if (s_gv.canvas) {
            if (fr && fr != s_gv.last_fb) {
                int w = 0, h = 0; nv_wasm_gfx_size(&w, &h);
                lv_canvas_set_buffer(s_gv.canvas, fr, w, h, LV_COLOR_FORMAT_RGB565);
                s_gv.last_fb = fr;
            }
            lv_obj_invalidate(lv_screen_active());
        }
    } else if (fr && s_gv.canvas) {
        int w = 0, h = 0; nv_wasm_gfx_size(&w, &h);
        if (fr != s_gv.last_fb) {                       // new buffer (legacy double-buffer, or first frame)
            lv_canvas_set_buffer(s_gv.canvas, fr, w, h, LV_COLOR_FORMAT_RGB565);
            s_gv.last_fb = fr;
            lv_obj_invalidate(s_gv.canvas);             // set_buffer already invalidates; full repaint
        } else if (dw > 0 && dh > 0 && (dw < w || dh < h)) {   // same buffer, partial dirty -> partial blit
            lv_area_t cc; lv_obj_get_coords(s_gv.canvas, &cc);
            lv_area_t a = { cc.x1 + dx, cc.y1 + dy, cc.x1 + dx + dw - 1, cc.y1 + dy + dh - 1 };
            lv_obj_invalidate_area(s_gv.canvas, &a);    // ABI v6: LVGL/PPA re-blits only the changed region
        } else {
            lv_obj_invalidate(s_gv.canvas);             // same buffer, whole frame changed
        }
    }
    // Feed the guest the FULL multi-touch set (ABI v3), mapped panel -> canvas the same way the
    // single-pointer path (gv_input_cb) maps finger 0. Games are full-screen landscape (canvas at the
    // panel origin, 1:1), so panel coords line up; still offset/clamp by the canvas rect for safety.
    if (s_gv.active && s_gv.canvas && s_gv.fit_mode < 0) {
        int16_t px[NV_TOUCH_MAX], py[NV_TOUCH_MAX];
        int n = nv_hal_touch_points(px, py, NV_TOUCH_MAX);
        int cw = 0, ch = 0; nv_wasm_gfx_size(&cw, &ch);
        lv_area_t cc; lv_obj_get_coords(s_gv.canvas, &cc);
        int mx[NV_TOUCH_MAX], my[NV_TOUCH_MAX];
        for (int i = 0; i < n; i++) {
            int x = px[i] - cc.x1, y = py[i] - cc.y1;
            if (x < 0) x = 0; else if (cw > 0 && x >= cw) x = cw - 1;
            if (y < 0) y = 0; else if (ch > 0 && y >= ch) y = ch - 1;
            mx[i] = x; my[i] = y;
        }
        nv_wasm_gfx_set_multi(mx, my, n);
    }
    if (nv_wasm_exec_state() == NV_WRUN_DONE) {
        bool ok = false; uint32_t ms = 0; char err[96] = "";
        nv_wasm_exec_collect(&ok, &ms, err, sizeof err);
        s_gv.active = false;
        gv_stop_timer();
        if (ok) { nv_ui_close_app(); return; }   // game exited on its own (Back at root) -> launcher
        // Error: no guest is left to consume gv_back's back_req, so restore the default Back
        // (close the app) — the home pill is hidden in fullscreen and Back was dead. Hide the
        // canvas too: the engine is IDLE with its buffers reclaimable (wasm_reclaim / a bigger
        // gfx_open frees them), and a visible canvas would keep reading freed PSRAM on redraw.
        nv_ui_set_back_handler(nullptr);
        if (s_gv.canvas) lv_obj_add_flag(s_gv.canvas, LV_OBJ_FLAG_HIDDEN);
        // Scaled canvas: the last frame sits in the panel framebuffer outside LVGL — repaint the
        // whole view so the error isn't drawn over a frozen game.
        gv_fit_stop();
        if (s_gv.fit_mode >= 0 && s_gv.root) lv_obj_invalidate(s_gv.root);
        s_gv.fit_mode = -1;
        s_gv.fit_last = nullptr;
        if (s_gv.overlay) {
            lv_label_set_text(s_gv.overlay, err[0] ? err : "error");
            lv_obj_clear_flag(s_gv.overlay, LV_OBJ_FLAG_HIDDEN);
        }
        nv_toast(NV_NOTE_ERROR, err[0] ? err : "error");
        return;
    }
    // Wedge watchdog (see GameView): heartbeat moved -> reset the clock; quiet too long -> the
    // guest is stuck in a loop that never presents. Abort forcibly interrupts the interpreter
    // (THREAD_MGR terminate); the run lands in DONE within a tick and the branch above collects
    // it and shows the error ("terminated by user"). Keep polling — just re-arm the clock so a
    // slow teardown doesn't re-fire the abort every 16 ms.
    if (s_gv.active && nv_wasm_exec_state() == NV_WRUN_RUNNING) {
        const uint32_t seq = nv_wasm_gfx_present_seq();
        if (seq != s_gv.hb_seq) {
            s_gv.hb_seq = seq;
            s_gv.hb_tick = lv_tick_get();
        } else if (lv_tick_elaps(s_gv.hb_tick) > kGameWedgeMs) {
            s_gv.hb_tick = lv_tick_get();
            nv_toast(NV_NOTE_ERROR, "app bloccata (loop infinito) — terminata");
            nv_wasm_exec_abort();
        }
    }
}

// Manifest "system_gestures": false: the OS edge swipes stay off while the game is on screen.
bool s_gv_gestures_off = false;
void gv_gestures(bool on) {
    for (int e = 0; e < NV_GESTURE_EDGE_COUNT; e++) nv_gesture_set_edge_enabled((nv_gesture_edge_t)e, on);
    s_gv_gestures_off = !on;
}

void gv_deleted(lv_event_t *) {
    gv_stop_timer();
    gv_stop_retry();
    s_gv.app = nullptr;
    s_gv.root = s_gv.status = nullptr;
    if (s_gv.active) { nv_wasm_exec_abort(); s_gv.active = false; }
    if (s_gv.bl_touched) {   // ABI v4: a backlight app (torch) ran -> restore the user's brightness
        nv_hal_backlight_set(nv_config_get_int("brightness", 90));
        s_gv.bl_touched = false;
    }
    s_gv.canvas = s_gv.overlay = nullptr;
    gv_fit_stop();
    s_gv.fit_mode = -1;
    s_gv.fit_last = nullptr;
    nv_ui_set_back_handler(nullptr);
    nv_ui_app_fullscreen(false);   // restore the status bar / chrome for the launcher
    if (s_gv_gestures_off) gv_gestures(true);
}

// The run has started: canvas, input and the frame timer.
void gv_begin(void) {
    const NvTheme *th = nv_theme_get();
    const nv_wasm_app_t *app = s_gv.app;
    lv_obj_t *root = s_gv.root;
    s_gv.active = true;
    s_gv.bl_touched = false;                    // fresh run: no backlight change yet
    s_gv.last_fb = nullptr;                      // ABI v6: force a set_buffer on the first frame
    s_gv.hb_seq  = nv_wasm_gfx_present_seq();   // seed the wedge watchdog from "now", not from 0
    s_gv.hb_tick = lv_tick_get();
    nv_ui_set_back_handler(gv_back);   // Back navigates inside the game, not straight out

    // Bind with the SAME clamp gfx_open applies to the allocation (16..1024 x 16..600): a store
    // manifest saying 4096x4096 made LVGL read 32 MB from a 1.2 MB buffer on the first render.
    int cw = (int)app->canvas_w, ch = (int)app->canvas_h;
    if (cw < 16) cw = 16; if (cw > 1024) cw = 1024;
    if (ch < 16) ch = 16; if (ch > 600)  ch = 600;
    s_gv.fit_mode = gv_fit_mode_for(app);
    if (s_gv.fit_mode >= 0 && !nv_hal_video_geom(cw, ch, 0, 0, NV_LCD_H_RES, NV_LCD_V_RES, s_gv.fit_mode,
                                                 &s_gv.fit_geom))
        s_gv.fit_mode = -1;                      // degenerate size: classic canvas
    s_gv.fit_last = nullptr;
    s_gvx.fit_top = 0;
    s_gvx.cv_scaled = false;
    s_gv.fit_clear = true;
    s_gv.fit_occluded = false;
    if (s_gv.fit_mode >= 0) {
        // ABI v9 scaled canvas: gv_poll blits frames to the panel; this empty full-screen object
        // only keeps the view's layout (LVGL draws nothing here, so it never fights the blit).
        s_gv.canvas = lv_obj_create(root);
        lv_obj_remove_style_all(s_gv.canvas);
        lv_obj_set_size(s_gv.canvas, lv_pct(100), lv_pct(100));
        lv_obj_clear_flag(s_gv.canvas, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        s_gv.canvas = lv_canvas_create(root);
        uint16_t *buf = nv_wasm_gfx_current();
        if (buf) lv_canvas_set_buffer(s_gv.canvas, buf, cw, ch, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_style_radius(s_gv.canvas, 10, 0);
        lv_obj_set_style_clip_corner(s_gv.canvas, false, 0);
    }
    lv_obj_add_flag(s_gv.canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_gv.canvas, gv_input_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_gv.canvas, gv_input_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_gv.canvas, gv_input_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_gv.canvas, gv_input_cb, LV_EVENT_PRESS_LOST, nullptr);

    s_gv.overlay = lv_label_create(root);
    lv_obj_set_style_text_font(s_gv.overlay, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_gv.overlay, th->text_dim, 0);
    // Until the guest presents its first frame (module load, a big app's own init) the panel
    // says so instead of staying black.
    lv_label_set_text(s_gv.overlay, nv_tr(NV_STR_WASM_STARTING));
    s_gv.loading = true;

    s_gv.timer = lv_timer_create(gv_poll, 16, nullptr);
}

void gv_retry_cb(lv_timer_t *);

// One start attempt. Opened straight out of another WASM app (Recents, Anima, "Open with", remote
// /api/ui/open), the engine is still unwinding that app's run — its teardown aborted it a moment
// ago — so the start is refused as "busy". While that run is stopping, show "Starting..." and
// retry until it lands instead of stranding the user on "Another app is running" until Home. A
// run that is NOT being stopped (a genuine concurrent run) or any other error shows at once.
void gv_try_start(void) {
    char err[96] = "";
    // ABI v7 launch file: every attempt consumes the parked grant, so park it again each time.
    nv_wasm_exec_set_launch_file(s_gv.launch[0] ? s_gv.launch : nullptr);
    if (nv_wasm_exec_start(s_gv.app, err, sizeof err)) {
        gv_stop_retry();
        if (s_gv.status) { lv_obj_delete(s_gv.status); s_gv.status = nullptr; }
        gv_begin();
        return;
    }
    const NvTheme *th = nv_theme_get();
    const bool busy = !strcmp(err, "busy");
    if (!s_gv.status) {
        s_gv.status = lv_label_create(s_gv.root);
        lv_obj_set_style_text_font(s_gv.status, &nv_font_14, 0);
    }
    if (busy && nv_wasm_exec_stopping() && lv_tick_elaps(s_gv.wait_t0) < kGameStartWaitMs) {
        lv_label_set_text(s_gv.status, nv_tr(NV_STR_WASM_STARTING));
        lv_obj_set_style_text_color(s_gv.status, th->text_dim, 0);
        if (!s_gv.retry) s_gv.retry = lv_timer_create(gv_retry_cb, kGameRetryMs, nullptr);
        return;
    }
    if (s_gv.retry) NV_LOGE("apps", "'%s': previous run did not stop within %u ms", s_gv.app->id,
                            (unsigned)kGameStartWaitMs);
    gv_stop_retry();
    lv_label_set_text(s_gv.status, busy ? nv_tr(NV_STR_WASM_BUSY) : err);
    lv_obj_set_style_text_color(s_gv.status, th->danger, 0);
}

void gv_retry_cb(lv_timer_t *) { gv_try_start(); }

void game_view_build(lv_obj_t *content, const nv_wasm_app_t *app) {
    nv_ui_app_fullscreen(true);   // games own the whole panel — expand content before sizing the canvas
    if (app && app->no_gestures) gv_gestures(false);
    lv_obj_set_style_bg_color(content, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);

    lv_obj_t *root = lv_obj_create(content);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(root, 6, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, gv_deleted, LV_EVENT_DELETE, nullptr);

    s_gv.app = app;
    s_gv.root = root;
    s_gv.status = nullptr;
    s_gv.wait_t0 = lv_tick_get();
    // Launcher tile / nv_open launch: grant the file the game was opened on (ABI v7), if any.
    const NvIntent *in = nv_open_intent();
    snprintf(s_gv.launch, sizeof s_gv.launch, "%s",
             in && in->verb == NV_INTENT_OPEN ? in->path : "");
    // Show "Starting..." first and start on the next tick: nv_wasm_exec_start reads the module
    // (an app.aot can be several MB) on this thread, and the panel must not sit black meanwhile.
    const NvTheme *th = nv_theme_get();
    s_gv.status = lv_label_create(root);
    lv_obj_set_style_text_font(s_gv.status, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_gv.status, th->text_dim, 0);
    lv_label_set_text(s_gv.status, nv_tr(NV_STR_WASM_STARTING));
    if (!s_gv.retry) s_gv.retry = lv_timer_create(gv_retry_cb, 30, nullptr);
}

// ---------------------------------------------------------------- per-app tile view
const nv_wasm_app_t *s_view_app = nullptr;

void tile_run_cb(lv_event_t *) { runner_start(s_view_app); }
void tile_deleted(lv_event_t *) { s_view_app = nullptr; }

void wasm_tile_build(lv_obj_t *content) {
    const NvApp *cur = nv_ui_current_app();
    s_view_app = cur ? static_cast<const nv_wasm_app_t *>(cur->user) : nullptr;

    if (s_view_app && nv_wasm_app_is_game(s_view_app)) {   // games run full-screen, not in the console
        game_view_build(content, s_view_app);
        return;
    }
    if (s_view_app && s_view_app->console) {   // a terminal program opens as a Terminal running it
        terminal_build_with(content, s_view_app->id);
        return;
    }

    lv_obj_t *c = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(c, tile_deleted, LV_EVENT_DELETE, nullptr);
    const NvTheme *th = nv_theme_get();
    if (!s_view_app) {
        lv_obj_t *info = nv_kit_info(c);
        lv_label_set_text(info, nv_tr(NV_STR_NO_APPS));
        lv_obj_set_style_text_color(info, th->text_dim, 0);
        return;
    }
    app_card(c, s_view_app, tile_run_cb, nullptr);
    runner_panel(c);
    // Opened on a file (nv_open): the user already chose to run it on that file — start at once.
    const NvIntent *in = nv_open_intent();
    if (in && in->verb == NV_INTENT_OPEN) runner_start(s_view_app);
}

// ---------------------------------------------------------------- Apps: installed + store
// One screen, two tabs: Installed (/sdcard/apps, scanned when the screen opens and after changes)
// and Store (the remote catalog, nv_appstore). Both show a two-column grid of compact cards —
// 80 px icon at its native size (a scaled lv_image re-transforms on every redraw of a scroll on the
// P4's software renderer), name, author, status and one action — paged, filtered by the search
// field and, in the Store, by category chips. Tapping a card opens its detail page: 2x icon,
// description, author, license and web page (the credits third-party apps require), and
// Install / Open / Update / Uninstall. Only the part below the search field is rebuilt on a
// keystroke, a chip, a page or a state change, so the field keeps its focus and the keyboard.
extern "C" void lv_image_cache_drop(const void *src);   // LVGL 9 (not in the public header)

constexpr int kPageCards = 16;               // cards per page (8 rows of 2)
constexpr int kCardIconPx = NV_STORE_ICON_PX;

nv_wasm_app_t *s_mgr        = nullptr;       // this screen's scan of /sdcard/apps (not the boot one)
int            s_mgr_n      = 0;
bool           s_mgr_scanned = false;        // rescanned only after an install / uninstall
lv_obj_t      *s_mgr_col    = nullptr;       // the scroll column
lv_obj_t      *s_head       = nullptr;       // title + tabs + search (persistent)
lv_obj_t      *s_body       = nullptr;       // everything below it (rebuilt)
lv_obj_t      *s_search_ta  = nullptr;
lv_timer_t    *s_search_tmr = nullptr;
lv_timer_t    *s_store_timer = nullptr;      // polls nv_appstore while the screen is open
int            s_tab        = 1;             // 0 = Installed, 1 = Store
int            s_page       = 0;
int32_t        s_list_y     = 0;             // list scroll position, restored when a detail closes
char           s_query[40]  = "";
char           s_detail[32] = "";            // id on the detail page ("" = the list)
char           s_armed[32]  = "";            // id whose Uninstall waits for the confirming tap
char           s_armed_inst[32] = "";        // id whose Install waits for the permission-review tap
char           s_store_inst[32] = "";        // id being installed: gets its Home tile when done
nv_store_state_t s_store_last = NV_STORE_IDLE;
int            s_store_last_prog = -1;
lv_obj_t      *s_prog_lbl   = nullptr;       // label showing the running install's %, patched in place
NV_PSRAM_BSS char s_ids[NV_STORE_MAX][32];   // stable id strings for event user data
NV_PSRAM_BSS char s_cats[NV_STORE_MAX][24];  // stable category ids for the chips
NV_PSRAM_BSS char s_filter[24];              // chip: see the kF* keys below, else a category id
NV_PSRAM_BSS char     s_sub[24];               // sub-category chip within an open category ("" = all)
NV_PSRAM_BSS int      s_order[NV_STORE_MAX];  // catalog rows of the current list / shelf, sorted
// Emulated platforms (store2): '\x07' = the Consoles hub, '\x08' + id = one platform's carts. The
// carts of a platform arrive in parts (nv_appstore_platform_open); only the one shown is in memory.
struct PlatView {
    char host[32];          // the platform's emulator / engine app, listed first ("" = none)
    int  part;              // part shown, 1-based
    bool tried;             // this part was asked for once (no retry loop on a failed fetch)
    char keys[NV_STORE_PLATS_MAX][24];   // "\x08<id>" per platform: stable chip / card user data
};
NV_PSRAM_BSS PlatView s_pv;
NV_PSRAM_BSS uint32_t s_okey[NV_STORE_MAX];   // their sort key (downloads / date)

// Store views (s_filter): "" Discover (shelves of a few cards: featured, most downloaded, new,
// recently updated, each with "See all"),  Featured,  All,  Most downloaded (the
// store's anonymous install counter),  New (release day),  Recently updated; else a
// category id.
constexpr int  kShelfCards  = 4;         // 4 shelves x 4 cards = kPageCards icon slots

// Icons of the cards on screen: an installed app shows its Home tile icon; a store entry the
// catalog's icon (fetched in the background, patched into its image when it arrives); anything
// else the compiled glyph. The detail page shows the icon twice as big, pixel-doubled once.
struct CardIcon {
    char           id[32];
    lv_obj_t      *img;        // nullptr = slot unused on this build
    lv_image_dsc_t dsc;
    bool           ready;
    bool           big;        // the detail page's image: shown pixel-doubled
};
NV_PSRAM_BSS CardIcon s_ci[kPageCards];
uint8_t       *s_ci_px  = nullptr;           // kPageCards x 80x80 ARGB8888 (PSRAM, while open)
uint8_t       *s_big_px = nullptr;           // 160x160 ARGB8888 for the detail page
lv_image_dsc_t s_big_dsc;
int            s_ci_n   = 0;

// Store screenshots on the app page: fetched by nv_appstore, decoded by the JPEG engine and scaled
// 3/4 by the PPA (512x300 -> 384x225, an exact 12/16 step), in a row that scrolls sideways. No
// rounded corners: clip_corner hangs the software renderer (ENGINEERING_RULES).
constexpr int kShotW = 384, kShotH = 225;
struct ShotImg { lv_obj_t *img; lv_image_dsc_t dsc; uint8_t *px; bool done; };
NV_PSRAM_BSS ShotImg s_shot[NV_STORE_SHOTS_MAX];
int  s_shot_n = 0;
char s_shot_id[32] = "";

void shots_release(void) {   // after the images are gone (or never shown)
    for (ShotImg &sh : s_shot) {
        if (sh.px) { lv_image_cache_drop(&sh.dsc); heap_caps_free(sh.px); }
        sh = {};
    }
    s_shot_n = 0;
}
void shots_poll(void) {
    for (int k = 0; k < s_shot_n; k++) {
        ShotImg &sh = s_shot[k];
        if (sh.done || !sh.img) continue;
        const uint8_t *jpg = nullptr;
        size_t len = 0;
        const int st = nv_appstore_shot_get(s_shot_id, k + 1, &jpg, &len);
        if (st == 0) continue;
        sh.done = true;
        gallery_raster_t r;
        if (st < 0 || !gallery_jpeg_hw_decode_mem(jpg, len, &r)) continue;
        const size_t cap = gallery_ppa_align_size((size_t)kShotW * kShotH * 2);
        sh.px = (uint8_t *)heap_caps_aligned_alloc(GALLERY_PPA_ALIGN, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (sh.px) {
            memset(sh.px, 0, cap);
            if (!gallery_ppa_scale_fit(&r, sh.px, kShotW, kShotH, cap)) { heap_caps_free(sh.px); sh.px = nullptr; }
        }
        gallery_jpeg_hw_free(&r);
        if (!sh.px) continue;
        sh.dsc = {};
        sh.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        sh.dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        sh.dsc.header.w = kShotW;
        sh.dsc.header.h = kShotH;
        sh.dsc.header.stride = kShotW * 2;
        sh.dsc.data_size = (uint32_t)kShotW * kShotH * 2;
        sh.dsc.data = sh.px;
        lv_image_set_src(sh.img, &sh.dsc);
    }
}

void body_build(void);
void head_build(void);
void body_refresh(void) {
    s_prog_lbl = nullptr;
    for (CardIcon &c : s_ci) c.img = nullptr;
    s_ci_n = 0;
    if (s_body) { lv_obj_clean(s_body); shots_release(); body_build(); }
}

void mgr_scan(void) {
    if (s_mgr_scanned) return;
    s_mgr_n = (s_mgr && nv_sd_is_mounted()) ? nv_wasm_scan(s_mgr, kMaxWasmApps) : 0;
    s_mgr_scanned = true;
}
const nv_wasm_app_t *mgr_find(const char *id) {
    mgr_scan();
    for (int i = 0; i < s_mgr_n; i++) if (!strcmp(s_mgr[i].id, id)) return &s_mgr[i];
    return nullptr;
}
long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : 0;
}
long installed_kb(const nv_wasm_app_t *a) {
    char aot[176];
    snprintf(aot, sizeof aot, "%.160s", a->wasm_path);
    char *dot = strrchr(aot, '.');
    if (dot) snprintf(dot, sizeof aot - (size_t)(dot - aot), ".aot");
    return (file_size(a->wasm_path) + (dot ? file_size(aot) : 0) + 1023) / 1024;
}

char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
bool ci_has(const char *hay, const char *needle) {
    if (!needle[0]) return true;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && lc(*a) == lc(*b)) { a++; b++; }
        if (!*b) return true;
    }
    return false;
}
bool store_match(const nv_store_entry_t *e) {
    const char f = s_filter[0];
    if (f == '\x07') return false;                         // the hub lists platforms, not apps
    if (f == '\x08') {                                     // one platform: its carts + its host
        if (strcmp(e->platform, s_filter + 1) != 0 && strcmp(e->id, s_pv.host) != 0) return false;
    } else if (e->platform[0]) {
        return false;                                      // carts stay out of every other list
    }
    if (f == '\x01' && !e->featured) return false;
    if ((f == '\x03' || f == '\x04' || f == '\x05') && e->library) return false;   // not apps
    if (f == '\x05' && !e->updated) return false;
    if (f && f > '\x08' && strcmp(e->category, s_filter) != 0) return false;
    if (f && f > '\x08' && s_sub[0] && strcmp(e->subcategory, s_sub) != 0) return false;
    return ci_has(e->name, s_query) || ci_has(e->author, s_query) || ci_has(e->category_name, s_query);
}
bool catalog_find(const char *id, nv_store_entry_t *out) {
    const int n = nv_appstore_count();
    for (int i = 0; i < n; i++) if (nv_appstore_get(i, out) && !strcmp(out->id, id)) return true;
    return false;
}

// ---- icons
const lv_image_dsc_t *installed_icon(const char *id) {
    const NvApp *a = nv_ui_find_app(id);
    return a ? a->icon : nullptr;
}
// Icon for a card: `store_icon` lets a store entry take a background-fetched icon.
void card_icon(lv_obj_t *img, const char *id, bool is_game, bool store_icon) {
    const lv_image_dsc_t *ic = installed_icon(id);
    if (!ic && store_icon && s_ci_px && s_ci_n < kPageCards) {
        CardIcon &c = s_ci[s_ci_n];
        uint8_t *px = s_ci_px + (size_t)s_ci_n * NV_STORE_ICON_BYTES;
        s_ci_n++;
        snprintf(c.id, sizeof c.id, "%s", id);
        c.img = img;
        c.big = false;
        c.ready = nv_appstore_icon_get(id, px);
        lv_image_cache_drop(&c.dsc);
        c.dsc = {};
        c.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        c.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
        c.dsc.header.w = c.dsc.header.h = kCardIconPx;
        c.dsc.header.stride = kCardIconPx * 4;
        c.dsc.data_size = NV_STORE_ICON_BYTES;
        c.dsc.data = px;
        if (c.ready) ic = &c.dsc;
    }
    lv_image_set_src(img, ic ? ic : wasm_icon_for(id, is_game));
}
// Ask nv_appstore for the icons this page still lacks (the store fetches them in the background).
void icons_request(void) {
    const char *want[kPageCards];
    int n = 0;
    for (int i = 0; i < s_ci_n; i++) if (s_ci[i].img && !s_ci[i].ready) want[n++] = s_ci[i].id;
    if (n) nv_appstore_icons_want(want, n);
}
void icons_poll(void) {
    for (int i = 0; i < s_ci_n; i++) {
        CardIcon &c = s_ci[i];
        if (!c.img || c.ready) continue;
        if (!nv_appstore_icon_get(c.id, s_ci_px + (size_t)i * NV_STORE_ICON_BYTES)) continue;
        c.ready = true;
        lv_image_cache_drop(&c.dsc);
        const lv_image_dsc_t *big_icon(const lv_image_dsc_t *src);
        lv_image_set_src(c.img, c.big ? big_icon(&c.dsc) : &c.dsc);
    }
}
// 80x80 ARGB8888 -> 160x160, each pixel doubled (crisp pixel art, no per-frame transform).
const lv_image_dsc_t *big_icon(const lv_image_dsc_t *src) {
    if (!s_big_px || !src || !src->data || src->header.cf != LV_COLOR_FORMAT_ARGB8888 ||
        src->header.w != kCardIconPx || src->header.h != kCardIconPx)
        return src;
    const uint32_t *in = (const uint32_t *)src->data;
    uint32_t *out = (uint32_t *)s_big_px;
    const int W = kCardIconPx * 2;
    for (int y = 0; y < kCardIconPx; y++) {
        uint32_t *r0 = out + (size_t)(2 * y) * W;
        for (int x = 0; x < kCardIconPx; x++) r0[2 * x] = r0[2 * x + 1] = in[y * kCardIconPx + x];
        memcpy(r0 + W, r0, (size_t)W * 4);
    }
    lv_image_cache_drop(&s_big_dsc);
    s_big_dsc = {};
    s_big_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_big_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    s_big_dsc.header.w = s_big_dsc.header.h = (uint32_t)W;
    s_big_dsc.header.stride = (uint32_t)W * 4;
    s_big_dsc.data_size = (uint32_t)W * W * 4;
    s_big_dsc.data = s_big_px;
    return &s_big_dsc;
}

// ---- actions
void goto_list(void);
void back_from_detail(void) {
    // Back gesture: leave the detail page (deferred: the gesture is still being dispatched).
    lv_async_call([](void *) { goto_list(); }, nullptr);
}
void goto_detail(const char *id) {
    s_list_y = s_mgr_col ? lv_obj_get_scroll_y(s_mgr_col) : 0;
    snprintf(s_detail, sizeof s_detail, "%s", id);
    s_armed[0] = 0;
    s_armed_inst[0] = 0;
    nv_ime_hide();
    if (s_head) lv_obj_add_flag(s_head, LV_OBJ_FLAG_HIDDEN);
    nv_ui_set_back_handler(back_from_detail);
    body_refresh();
    if (s_mgr_col) lv_obj_scroll_to_y(s_mgr_col, 0, LV_ANIM_OFF);
}
void goto_list(void) {
    if (!s_detail[0]) return;
    s_detail[0] = 0;
    s_armed[0] = 0;
    s_armed_inst[0] = 0;
    nv_ui_set_back_handler(nullptr);
    if (s_head) lv_obj_clear_flag(s_head, LV_OBJ_FLAG_HIDDEN);
    body_refresh();
    if (s_mgr_col) {
        lv_obj_update_layout(s_mgr_col);
        lv_obj_scroll_to_y(s_mgr_col, s_list_y, LV_ANIM_OFF);
    }
}
void detail_cb(lv_event_t *e) { goto_detail((const char *)lv_event_get_user_data(e)); }
void back_cb(lv_event_t *) { goto_list(); }
void open_cb(lv_event_t *e) {
    // Async: this click runs inside the store's own widgets, which opening the app deletes.
    nv_ui_open_app_id_async((const char *)lv_event_get_user_data(e));
}
// Sensitive permissions an install/update of `id` would grant that the user hasn't accepted yet
// (for an update: only the ones the installed version didn't have).
uint32_t perms_to_accept(const char *id) {
    nv_store_entry_t e;
    if (!catalog_find(id, &e)) return 0;
    const nv_wasm_app_t *inst = mgr_find(id);
    return e.perms & ~(inst ? inst->perms : 0u);
}
// Editions ("variants", e.g. a game's languages): the one picked on the app page. Default: the
// installed one, else the one in the UI language, else the first.
char s_var_for[32] = "";     // app id the choice belongs to
char s_var_sel[9]  = "";
const char *ui_lang_code(void) {
    switch (nv_i18n_get_lang()) {
        case NV_LANG_IT: return "it";
        case NV_LANG_ES: return "es";
        case NV_LANG_FR: return "fr";
        case NV_LANG_DE: return "de";
        default:         return "en";
    }
}
void variant_default(const nv_store_entry_t &e) {
    if (!strcmp(s_var_for, e.id)) return;
    snprintf(s_var_for, sizeof s_var_for, "%s", e.id);
    s_var_sel[0] = 0;
    if (!e.n_var) return;
    nv_appstore_variant_get(e.id, s_var_sel, sizeof s_var_sel);
    for (int k = 0; k < e.n_var; k++) if (!strcmp(e.var[k].id, s_var_sel)) return;   // installed one
    snprintf(s_var_sel, sizeof s_var_sel, "%s", e.var[0].id);
    for (int k = 0; k < e.n_var; k++)
        if (!strcmp(e.var[k].lang, ui_lang_code())) { snprintf(s_var_sel, sizeof s_var_sel, "%s", e.var[k].id); break; }
}
void variant_cb(lv_event_t *e) {
    const char *v = (const char *)lv_event_get_user_data(e);
    snprintf(s_var_sel, sizeof s_var_sel, "%s", v);
    // Already installed: switching only rewrites data/variant, the app fetches the rest itself.
    if (mgr_find(s_var_for) && !nv_appstore_variant_set(s_var_for, s_var_sel))
        nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_STORE_FAILED));
    body_refresh();
}

void install_cb(lv_event_t *e) {
    const char *id = (const char *)lv_event_get_user_data(e);
    // Consent: an app asking for sensitive permissions installs only from its detail page, on a
    // second tap, with the list on screen. A tap on a grid card opens that page armed.
    if (perms_to_accept(id) && strcmp(s_armed_inst, id) != 0) {
        char keep[32];
        snprintf(keep, sizeof keep, "%s", id);        // user data may point into a rebuilt list
        if (strcmp(s_detail, keep) != 0) goto_detail(keep);
        snprintf(s_armed_inst, sizeof s_armed_inst, "%s", keep);
        body_refresh();
        return;
    }
    s_armed_inst[0] = 0;
    nv_store_entry_t ce;
    const char *variant = nullptr;
    if (catalog_find(id, &ce) && ce.n_var) { variant_default(ce); variant = s_var_sel; }
    if (nv_appstore_install_variant(id, variant)) snprintf(s_store_inst, sizeof s_store_inst, "%s", id);
    else nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_WASM_BUSY));
    body_refresh();
}
void uninstall_cb(lv_event_t *e) {
    const char *id = (const char *)lv_event_get_user_data(e);
    if (strcmp(s_armed, id) != 0) {   // first tap arms, the second one deletes
        snprintf(s_armed, sizeof s_armed, "%s", id);
        body_refresh();
        return;
    }
    s_armed[0] = 0;
    char err[112] = "";
    if (nv_wasm_uninstall(id, err, sizeof err)) {
        nv_telemetry_store(NV_TL_STORE_UNINSTALL);
        nv_appstore_forget_installed(id);      // no "update available" for an app that's gone
        nv_app_unregister(id);                 // remove the Home tile live (no reboot needed)
        nv_open_unregister_app(id);            // ...and its "Open with" entry (ABI v7)
        s_mgr_scanned = false;
        nv_toast(NV_NOTE_OK, nv_tr(NV_STR_STORE_UNINSTALLED));
    } else {
        nv_toast(NV_NOTE_ERROR, err[0] ? err : nv_tr(NV_STR_STORE_FAILED));
    }
    body_refresh();
}
void retry_cb(lv_event_t *) { nv_appstore_refresh(); body_refresh(); }
void chip_cb(lv_event_t *e) {
    snprintf(s_filter, sizeof s_filter, "%s", (const char *)lv_event_get_user_data(e));
    s_sub[0] = 0;
    s_page = 0;
    body_refresh();
}
void page_cb(lv_event_t *e) {
    s_page += (int)(intptr_t)lv_event_get_user_data(e);
    body_refresh();
    if (s_mgr_col) lv_obj_scroll_to_y(s_mgr_col, 0, LV_ANIM_OFF);
}
void search_apply(lv_timer_t *) {
    s_search_tmr = nullptr;
    s_page = 0;
    body_refresh();
}
void search_cb(lv_event_t *) {
    // Filter as you type, 250 ms after the last key (a rebuild per key would lag the keyboard).
    snprintf(s_query, sizeof s_query, "%s", lv_textarea_get_text(s_search_ta));
    if (s_search_tmr) lv_timer_reset(s_search_tmr);
    else {
        s_search_tmr = lv_timer_create(search_apply, 250, nullptr);
        lv_timer_set_repeat_count(s_search_tmr, 1);
    }
}
void tab_cb(lv_event_t *e) {
    const int tab = (int)(intptr_t)lv_event_get_user_data(e);
    if (tab == s_tab) return;
    s_tab = tab;
    s_page = 0;
    s_armed[0] = 0;
    if (s_tab == 1 && nv_appstore_state() == NV_STORE_IDLE) nv_appstore_refresh();   // first visit
    head_build();
    body_refresh();
}

// ---- building blocks
lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *font, lv_color_t c) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}
lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
// "<  2 / 5  >" (nothing when everything fits on one page).
void pager(lv_obj_t *parent, int total) {
    const int pages = (total + kPageCards - 1) / kPageCards;
    if (pages <= 1) return;
    const NvTheme *th = nv_theme_get();
    lv_obj_t *row = box(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, NV_SP_4, 0);
    lv_obj_t *prev = nv_kit_button(row, LV_SYMBOL_LEFT, false);
    char t[24];
    snprintf(t, sizeof t, "%d / %d", s_page + 1, pages);
    label(row, t, &nv_font_20, th->text_dim);
    lv_obj_t *next = nv_kit_button(row, LV_SYMBOL_RIGHT, false);
    if (s_page > 0) lv_obj_add_event_cb(prev, page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    else            lv_obj_add_state(prev, LV_STATE_DISABLED);
    if (s_page < pages - 1) lv_obj_add_event_cb(next, page_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    else                    lv_obj_add_state(next, LV_STATE_DISABLED);
}
void page_clamp(int total) {
    const int pages = (total + kPageCards - 1) / kPageCards;
    if (s_page > pages - 1) s_page = pages - 1;
    if (s_page < 0) s_page = 0;
}
void empty_state(lv_obj_t *parent, const char *txt, lv_color_t c) {
    lv_obj_t *info = nv_kit_info(parent);
    lv_label_set_text(info, txt);
    lv_obj_set_style_text_color(info, c, 0);
}

// A grid card. `action` is the button label (nullptr = none), `status` the third text line.
lv_obj_t *card(lv_obj_t *grid, const char *id_slot, const char *name, const char *sub,
               const char *status, lv_color_t status_c, bool is_game, bool store_icon,
               const char *action, lv_event_cb_t action_cb, bool action_primary) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *c = lv_obj_create(grid);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, lv_pct(49), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, th->surface, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, th->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_radius(c, NV_RAD_MD, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, th->divider, 0);
    lv_obj_set_style_pad_all(c, NV_SP_3, 0);
    lv_obj_set_style_pad_column(c, NV_SP_3, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, detail_cb, LV_EVENT_CLICKED, (void *)id_slot);

    lv_obj_t *img = lv_image_create(c);
    card_icon(img, id_slot, is_game, store_icon);

    lv_obj_t *col = lv_obj_create(c);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *nm = label(col, name, &nv_font_20, th->text_strong);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, lv_pct(100));
    if (sub && sub[0]) {
        lv_obj_t *sl = label(col, sub, &nv_font_14, th->text_dim);
        lv_label_set_long_mode(sl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(sl, lv_pct(100));
    }
    lv_obj_t *st = label(col, status, &nv_font_14, status_c);
    lv_label_set_long_mode(st, LV_LABEL_LONG_DOT);
    lv_obj_set_width(st, lv_pct(100));

    if (action) {
        lv_obj_t *b = nv_kit_button(c, action, action_primary);
        if (action_cb) lv_obj_add_event_cb(b, action_cb, LV_EVENT_CLICKED, (void *)id_slot);
        else lv_obj_add_state(b, LV_STATE_DISABLED);
        if (!strcmp(nv_appstore_installing_id(), id_slot)) s_prog_lbl = st;
    }
    return st;
}
lv_obj_t *grid(lv_obj_t *parent) {
    lv_obj_t *g = box(parent, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(g, NV_SP_3, 0);
    lv_obj_set_style_pad_row(g, NV_SP_3, 0);
    return g;
}

// ---- dates and counts
// YYYYMMDD -> days since 1970-01-01 (civil calendar), to tell how old a release is.
int32_t ymd_days(uint32_t ymd) {
    int y = (int)(ymd / 10000);
    const int m = (int)(ymd / 100 % 100), d = (int)(ymd % 100);
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = y - era * 400;
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
// Today as YYYYMMDD, 0 while the clock isn't set (no NEW badges from a 1970 clock).
uint32_t today_ymd(void) {
    const time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year + 1900 < 2026) return 0;
    return (uint32_t)(tm.tm_year + 1900) * 10000 + (uint32_t)(tm.tm_mon + 1) * 100 + (uint32_t)tm.tm_mday;
}
// Released in the last two weeks: the card gets a NEW badge.
bool fresh(uint32_t added) {
    const uint32_t t = today_ymd();
    return added && t && ymd_days(t) - ymd_days(added) <= 14;
}
// "30 Sep 2026" (month in the UI language); "" for 0.
void fmt_date(char *out, size_t n, uint32_t ymd) {
    if (!ymd) { if (n) out[0] = 0; return; }
    snprintf(out, n, "%u %s %u", (unsigned)(ymd % 100), nv_i18n_month_short((int)(ymd / 100 % 100) - 1),
             (unsigned)(ymd / 10000));
}
// "30 Sep" when it's this year, else with the year.
void fmt_date_short(char *out, size_t n, uint32_t ymd) {
    const uint32_t t = today_ymd();
    if (ymd && t && ymd / 10000 == t / 10000)
        snprintf(out, n, "%u %s", (unsigned)(ymd % 100), nv_i18n_month_short((int)(ymd / 100 % 100) - 1));
    else
        fmt_date(out, n, ymd);
}

// A cart of one of the retro consoles (WASM-4, Game Boy, Arduboy, CHIP-8 packs): hundreds of them
// arrive in bulk, so on "New" they come after the NucleoOS apps released the same day.
bool retro_cart(const nv_store_entry_t &e) {
    return !strcmp(e.category, "retro") || !strcmp(e.category, "wasm4") || !strcmp(e.subcategory, "wasm4");
}

// The rows a view shows, in its order, into s_order (returns how many). Top: most installs first
// (featured, then catalog order, among equals); New: newest release first, NucleoOS apps before
// the WASM-4 carts of the same day; Recent: last updated first; the others: catalog order.
int store_collect(void) {
    const int n = nv_appstore_count();
    const char f = s_filter[0];
    int m = 0;
    for (int i = 0; i < n && i < NV_STORE_MAX; i++) {
        nv_store_entry_t e;
        if (!nv_appstore_get(i, &e) || !store_match(&e)) continue;
        uint32_t k = 0;
        if (f == '\x03')      k = e.downloads;
        else if (f == '\x04') k = e.added * 2 + !retro_cart(e);
        else if (f == '\x05') k = e.updated;
        s_order[m] = i;
        s_okey[m] = k;
        m++;
    }
    if (f == '\x03' || f == '\x04' || f == '\x05')   // stable insertion sort, biggest key first
        for (int a = 1; a < m; a++)
            for (int b = a; b > 0 && s_okey[b] > s_okey[b - 1]; b--) {
                const int ti = s_order[b]; s_order[b] = s_order[b - 1]; s_order[b - 1] = ti;
                const uint32_t tk = s_okey[b]; s_okey[b] = s_okey[b - 1]; s_okey[b - 1] = tk;
            }
    return m;
}

// ---- Store tab
void store_chips(lv_obj_t *parent, int n) {
    const NvTheme *th = nv_theme_get();
    static char s_disc[1] = "";
    static char s_all[2]  = "\x02";
    static char s_feat[2] = "\x01";
    static char s_top[2]  = "\x03";
    static char s_new[2]  = "\x04";
    // Whole store at a glance: native apps + every platform's carts (the chips count each part).
    int carts = 0;
    for (int i = 0; i < nv_appstore_platform_count(); i++) {
        nv_store_platform_t p;
        if (nv_appstore_platform_get(i, &p)) carts += p.count;
    }
    {
        int nat = 0;
        for (int i = 0; i < nv_appstore_count(); i++) {
            nv_store_entry_t e;
            if (nv_appstore_get(i, &e) && !e.platform[0]) nat++;
        }
        char tot[64];
        snprintf(tot, sizeof tot, nv_tr(NV_STR_STORE_TOTAL_FMT), nat + carts);
        label(parent, tot, &nv_font_14, th->text_dim);
    }
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);        // one row, scrolls sideways
    lv_obj_set_style_pad_column(row, NV_SP_2, 0);
    lv_obj_set_scroll_dir(row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    auto chip = [&](const char *txt, int count, const char *key, bool sel) {
        char t[48];
        if (count >= 0) snprintf(t, sizeof t, "%s  %d", txt, count);
        else            snprintf(t, sizeof t, "%s", txt);
        lv_obj_t *b = nv_kit_button(row, t, sel);
        if (!sel) lv_obj_set_style_text_color(lv_obj_get_child(b, 0), th->text_dim, 0);
        lv_obj_add_event_cb(b, chip_cb, LV_EVENT_CLICKED, (void *)key);
        return b;
    };
    int featured = 0;
    int cn = 0, counts[24] = {0};
    char names[24][28];
    int natives = 0;
    for (int i = 0; i < n && i < NV_STORE_MAX; i++) {
        nv_store_entry_t e;
        if (!nv_appstore_get(i, &e) || e.platform[0]) continue;   // carts: in their platform's tab
        natives++;
        if (e.featured) featured++;
        if (!e.category[0]) continue;
        int k = 0;
        while (k < cn && strcmp(s_cats[k], e.category) != 0) k++;
        if (k == cn) {
            if (cn >= 24) continue;
            snprintf(s_cats[cn], sizeof s_cats[cn], "%s", e.category);
            snprintf(names[cn], sizeof names[cn], "%s", e.category_name[0] ? e.category_name : e.category);
            cn++;
        }
        counts[k]++;
    }
    lv_obj_t *sel = chip(nv_tr(NV_STR_STORE_DISCOVER), -1, s_disc, s_filter[0] == 0);
    static char s_catv[2] = "\x06";
    auto special = [&](nv_str_id_t txt, int count, char *key) {
        lv_obj_t *b = chip(nv_tr(txt), count, key, s_filter[0] == key[0]);
        if (s_filter[0] == key[0]) sel = b;
    };
    special(NV_STR_STORE_CATEGORIES, -1, s_catv);
    static char s_hub[2] = "\x07";
    const int np = nv_appstore_platform_count();
    if (np) special(NV_STR_STORE_CONSOLES, carts, s_hub);
    if (s_filter[0] == '\x08')                                    // the open platform
        for (int i = 0; i < np && i < NV_STORE_PLATS_MAX; i++) {
            nv_store_platform_t p;
            if (!nv_appstore_platform_get(i, &p) || strcmp(p.id, s_filter + 1) != 0) continue;
            snprintf(s_pv.keys[i], sizeof s_pv.keys[i], "\x08%s", p.id);
            sel = chip(p.name, p.count, s_pv.keys[i], true);
        }
    special(NV_STR_STORE_APPS, natives, s_all);
    if (featured) special(NV_STR_STORE_FEATURED, -1, s_feat);
    special(NV_STR_STORE_TOP, -1, s_top);
    special(NV_STR_STORE_NEW, -1, s_new);
    static char s_rec[2] = "\x05";
    if (s_filter[0] == '\x05') special(NV_STR_STORE_RECENT, -1, s_rec);   // reached by "See all"
    // Biggest categories first (the catalog's own order depends on which app is listed first).
    int order[24];
    for (int k = 0; k < cn; k++) order[k] = k;
    for (int a = 1; a < cn; a++)
        for (int b = a; b > 0 && counts[order[b]] > counts[order[b - 1]]; b--) {
            const int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t;
        }
    for (int i = 0; i < cn; i++) {
        const int k = order[i];
        lv_obj_t *b = chip(names[k], counts[k], s_cats[k], !strcmp(s_filter, s_cats[k]));
        if (!strcmp(s_filter, s_cats[k])) sel = b;
    }
    lv_obj_update_layout(row);
    lv_obj_scroll_to_view(sel, LV_ANIM_OFF);   // the selected chip stays in sight
}

// One store card for catalog row `i`. The subtitle says what the view is about: installs on
// "Most downloaded", the release day on "New", the update on "Recently updated", else the author.
void store_card(lv_obj_t *g, int i, const nv_store_entry_t &e, char view) {
    const NvTheme *th = nv_theme_get();
    snprintf(s_ids[i], sizeof s_ids[i], "%s", e.id);
    const bool installed = mgr_find(e.id) != nullptr;
    const bool busy = !strcmp(nv_appstore_installing_id(), e.id);
    const bool too_new = e.abi > (uint32_t)NV_WASM_ABI;
    char sub[80] = "", d[24];
    if (view == '\x03' && e.downloads) {
        snprintf(sub, sizeof sub, nv_tr(NV_STR_STORE_DL_FMT), (unsigned)e.downloads);
    } else if (view == '\x04' && e.added) {
        fmt_date_short(d, sizeof d, e.added);
        if (fresh(e.added)) snprintf(sub, sizeof sub, "%s  -  %s", nv_tr(NV_STR_STORE_NEW_BADGE), d);
        else                snprintf(sub, sizeof sub, "%s %s", nv_tr(NV_STR_STORE_ADDED), d);
    } else if (view == '\x05' && e.updated) {
        fmt_date_short(d, sizeof d, e.updated);
        snprintf(sub, sizeof sub, "v%s  -  %s", e.version, d);
    } else if (e.author[0]) {
        snprintf(sub, sizeof sub, nv_tr(NV_STR_STORE_BY_FMT), e.author);
    } else if (e.subcategory_name[0]) {
        snprintf(sub, sizeof sub, "%s  -  %s", e.category_name, e.subcategory_name);
    } else {
        snprintf(sub, sizeof sub, "%s", e.category_name);
    }
    char status[48];
    const char *action = nullptr;
    lv_event_cb_t cb = nullptr;
    bool primary = true;
    lv_color_t sc = th->text_dim;
    if (busy) {
        snprintf(status, sizeof status, "%s %d%%", nv_tr(NV_STR_STORE_INSTALLING), nv_appstore_progress());
        sc = th->primary;
        action = nv_tr(NV_STR_STORE_INSTALL);
    } else if (too_new) {
        snprintf(status, sizeof status, "%s", nv_tr(NV_STR_STORE_NEEDS_OS));
        sc = th->danger;
    } else if (installed && e.update) {
        snprintf(status, sizeof status, "%s", nv_tr(NV_STR_STORE_UPDATE_AVAIL));
        sc = th->accent;
        action = nv_tr(NV_STR_STORE_UPDATE); cb = install_cb;
    } else if (installed) {
        snprintf(status, sizeof status, "%s", nv_tr(NV_STR_STORE_IS_INSTALLED));
        action = nv_tr(NV_STR_OPEN); cb = open_cb; primary = false;
    } else {
        snprintf(status, sizeof status, "%u KB", (unsigned)((e.size + e.aot_size + 1023) / 1024));
        if (view != '\x04' && fresh(e.added)) {                  // NEW badge on every other view
            const size_t l = strlen(status);
            snprintf(status + l, sizeof status - l, "  -  %s", nv_tr(NV_STR_STORE_NEW_BADGE));
            sc = th->accent;
        }
        action = nv_tr(NV_STR_STORE_INSTALL); cb = install_cb;
    }
    if (busy) cb = nullptr;
    card(g, s_ids[i], e.name, sub, status, sc, e.is_game, e.icon_z > 0, action, cb, primary);
}

void see_all_cb(lv_event_t *e) {
    snprintf(s_filter, sizeof s_filter, "%s", (const char *)lv_event_get_user_data(e));
    s_sub[0] = 0;
    s_page = 0;
    body_refresh();
    if (s_mgr_col) lv_obj_scroll_to_y(s_mgr_col, 0, LV_ANIM_OFF);
}

// ---- Categories -------------------------------------------------------------------------------
// The store's curated categories as cards: a coloured monogram tile, name, app count, the one-line
// description and the three apps that lead it. Tapping one opens its list (with the same header).
NV_PSRAM_BSS char s_catpage_ids[NV_STORE_CATS_MAX][24];

lv_color_t cat_color(const nv_store_category_t &c) {
    return c.color ? lv_color_hex(c.color) : nv_theme_get()->primary;
}
// First character of a UTF-8 name (Latin-1 letters are two bytes), upper-cased when ASCII.
void monogram_text(const char *name, char *out, size_t n) {
    size_t k = 0;
    if (name[0]) {
        out[k++] = (name[0] >= 'a' && name[0] <= 'z') ? (char)(name[0] - 32) : name[0];
        while (k < n - 1 && ((unsigned char)name[k] & 0xC0) == 0x80) { out[k] = name[k]; k++; }
    }
    out[k] = 0;
}
lv_obj_t *monogram(lv_obj_t *parent, const nv_store_category_t &c, int px, const lv_font_t *font) {
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, px, px);
    lv_obj_set_style_radius(m, px / 4, 0);
    lv_obj_set_style_bg_color(m, cat_color(c), 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);
    char t[8];
    monogram_text(c.name, t, sizeof t);
    lv_obj_t *l = label(m, t, font, lv_color_white());
    lv_obj_center(l);
    return m;
}
void cat_open_cb(lv_event_t *e) {
    snprintf(s_filter, sizeof s_filter, "%s", (const char *)lv_event_get_user_data(e));
    s_sub[0] = 0;
    s_page = 0;
    body_refresh();
    if (s_mgr_col) lv_obj_scroll_to_y(s_mgr_col, 0, LV_ANIM_OFF);
}
void cat_card(lv_obj_t *grid, const nv_store_category_t &c, int slot, lv_event_cb_t cb = cat_open_cb,
              char *key = nullptr) {
    const NvTheme *th = nv_theme_get();
    if (!key) {
        snprintf(s_catpage_ids[slot], sizeof s_catpage_ids[slot], "%s", c.id);
        key = s_catpage_ids[slot];
    }
    lv_obj_t *k = lv_obj_create(grid);
    lv_obj_remove_style_all(k);
    lv_obj_set_size(k, lv_pct(32), 196);
    lv_obj_set_style_bg_color(k, th->surface, 0);
    lv_obj_set_style_bg_color(k, th->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(k, NV_RAD_MD, 0);
    lv_obj_set_style_border_width(k, 1, 0);
    lv_obj_set_style_border_color(k, th->divider, 0);
    lv_obj_set_style_pad_all(k, NV_SP_4, 0);
    lv_obj_set_style_pad_row(k, NV_SP_2, 0);
    lv_obj_set_flex_flow(k, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(k, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(k, cb, LV_EVENT_CLICKED, key);

    lv_obj_t *hd = box(k, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(hd, NV_SP_3, 0);
    lv_obj_set_flex_align(hd, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hd, LV_OBJ_FLAG_CLICKABLE);
    monogram(hd, c, 52, &nv_font_28);
    lv_obj_t *col = box(hd, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *nm = label(col, c.name, &nv_font_20, th->text_strong);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, lv_pct(100));
    char cnt[32];
    snprintf(cnt, sizeof cnt, nv_tr(NV_STR_STORE_APPS_FMT), (int)c.count);
    label(col, cnt, &nv_font_14, th->text_dim);

    if (c.desc[0]) {
        lv_obj_t *d = label(k, c.desc, &nv_font_14, th->text);
        lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
        lv_obj_set_size(d, lv_pct(100), 40);             // two lines, the same on every card
    }
    char top[160] = "";
    for (int i = 0; i < 3 && c.top[i][0]; i++) {
        const size_t l = strlen(top);
        snprintf(top + l, sizeof top - l, "%s%s", i ? "  -  " : "", c.top[i]);
    }
    if (top[0]) {
        lv_obj_t *t = label(k, top, &nv_font_14, cat_color(c));
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, lv_pct(100));
    }
}
lv_obj_t *cat_grid(lv_obj_t *parent) {
    lv_obj_t *g = box(parent, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(g, NV_SP_3, 0);
    lv_obj_set_style_pad_row(g, NV_SP_3, 0);
    return g;
}
void store_categories(lv_obj_t *parent) {
    const NvTheme *th = nv_theme_get();
    const int n = nv_appstore_category_count();
    int apps = 0;
    for (int i = 0; i < n; i++) { nv_store_category_t c; if (nv_appstore_category_get(i, &c)) apps += c.count; }
    label(parent, nv_tr(NV_STR_STORE_CATEGORIES), &nv_font_28, th->text_strong);
    char sub[64];
    snprintf(sub, sizeof sub, nv_tr(NV_STR_STORE_CATS_SUB_FMT), n, apps);
    label(parent, sub, &nv_font_14, th->text_dim);
    if (!n) { empty_state(parent, nv_tr(NV_STR_STORE_NO_RESULTS), th->text_dim); return; }
    lv_obj_t *g = cat_grid(parent);
    for (int i = 0; i < n && i < NV_STORE_CATS_MAX; i++) {
        nv_store_category_t c;
        if (nv_appstore_category_get(i, &c)) cat_card(g, c, i);
    }
}
void list_header(lv_obj_t *parent, const nv_store_category_t &c);
// The opened category's header above its list: monogram, name, description, count.
void category_header(lv_obj_t *parent) {
    nv_store_category_t c;
    bool found = false;
    for (int i = 0; i < nv_appstore_category_count() && !found; i++)
        found = nv_appstore_category_get(i, &c) && !strcmp(c.id, s_filter);
    if (found) list_header(parent, c);
}

// ---- Consoles (store2 platforms) ----------------------------------------------------------------
// A platform as a category card / header: its colour, name, cart count, description and, as the
// "top" line, the emulator or engine app that plays it.
bool platform_card_data(int i, nv_store_category_t *c, nv_store_platform_t *p) {
    if (!nv_appstore_platform_get(i, p)) return false;
    *c = {};
    snprintf(c->id, sizeof c->id, "%s", p->id);
    snprintf(c->name, sizeof c->name, "%s", p->name);
    snprintf(c->desc, sizeof c->desc, "%s", p->desc);
    c->color = p->color;
    c->count = p->count;
    nv_store_entry_t h;
    if (p->host[0] && catalog_find(p->host, &h)) snprintf(c->top[0], sizeof c->top[0], "%s", h.name);
    return true;
}
void plat_enter(const char *key, int part) {
    snprintf(s_filter, sizeof s_filter, "%s", key);
    s_sub[0] = 0;
    s_page = 0;
    s_pv.host[0] = 0;
    s_pv.part = part < 1 ? 1 : part;
    s_pv.tried = false;
    for (int i = 0; i < nv_appstore_platform_count(); i++) {
        nv_store_platform_t p;
        if (nv_appstore_platform_get(i, &p) && !strcmp(p.id, key + 1))
            snprintf(s_pv.host, sizeof s_pv.host, "%s", p.host);
    }
    body_refresh();
    if (s_mgr_col) lv_obj_scroll_to_y(s_mgr_col, 0, LV_ANIM_OFF);
}
void plat_open_cb(lv_event_t *e) { plat_enter((const char *)lv_event_get_user_data(e), 1); }
void plat_cards(lv_obj_t *parent, int max) {
    lv_obj_t *g = cat_grid(parent);
    for (int i = 0; i < nv_appstore_platform_count() && i < NV_STORE_PLATS_MAX && i < max; i++) {
        nv_store_category_t c;
        nv_store_platform_t p;
        if (!platform_card_data(i, &c, &p)) continue;
        snprintf(s_pv.keys[i], sizeof s_pv.keys[i], "\x08%s", p.id);
        cat_card(g, c, i, plat_open_cb, s_pv.keys[i]);
    }
}
void store_platforms(lv_obj_t *parent) {
    const NvTheme *th = nv_theme_get();
    const int n = nv_appstore_platform_count();
    int carts = 0;
    for (int i = 0; i < n; i++) { nv_store_platform_t p; if (nv_appstore_platform_get(i, &p)) carts += p.count; }
    label(parent, nv_tr(NV_STR_STORE_CONSOLES), &nv_font_28, th->text_strong);
    char sub[64];
    snprintf(sub, sizeof sub, nv_tr(NV_STR_STORE_CONSOLES_SUB_FMT), n, carts);
    label(parent, sub, &nv_font_14, th->text_dim);
    plat_cards(parent, NV_STORE_PLATS_MAX);
}
// Search hits in the platforms whose carts aren't in memory: one button each, opening the part
// that holds the first hit (the query stays, so the list shows the matches).
void plat_hits(lv_obj_t *parent) {
    if (!s_query[0]) return;
    lv_obj_t *row = nullptr;
    for (int i = 0; i < nv_appstore_platform_count() && i < NV_STORE_PLATS_MAX; i++) {
        nv_store_platform_t p;
        int first = -1;
        if (!nv_appstore_platform_get(i, &p)) continue;
        const int hits = nv_appstore_platform_search(i, s_query, &first);
        if (!hits) continue;
        if (!row) {
            row = box(parent, LV_FLEX_FLOW_ROW_WRAP);
            lv_obj_set_style_pad_column(row, NV_SP_2, 0);
            lv_obj_set_style_pad_row(row, NV_SP_2, 0);
        }
        snprintf(s_pv.keys[i], sizeof s_pv.keys[i], "\x08%s", p.id);
        char t[64];
        snprintf(t, sizeof t, nv_tr(NV_STR_STORE_PLAT_HITS_FMT), p.name, hits);
        lv_obj_t *b = nv_kit_button(row, t, false);
        const int part = p.chunk ? first / p.chunk + 1 : 1;
        lv_obj_set_user_data(b, (void *)(intptr_t)part);
        lv_obj_add_event_cb(b, [](lv_event_t *e) {
            lv_obj_t *t = (lv_obj_t *)lv_event_get_target(e);
            plat_enter((const char *)lv_event_get_user_data(e), (int)(intptr_t)lv_obj_get_user_data(t));
        }, LV_EVENT_CLICKED, s_pv.keys[i]);
    }
}
void part_cb(lv_event_t *e) {
    s_pv.part += (int)(intptr_t)lv_event_get_user_data(e);
    s_pv.tried = false;
    s_page = 0;
    body_refresh();
    if (s_mgr_col) lv_obj_scroll_to_y(s_mgr_col, 0, LV_ANIM_OFF);
}
// The open platform: header, part switcher, and its carts once the part is in memory. False while
// the part is being fetched (a spinner, or the error with a retry) — the list waits.
bool platform_page(lv_obj_t *parent) {
    const NvTheme *th = nv_theme_get();
    int idx = -1;
    nv_store_category_t c;
    nv_store_platform_t p;
    for (int i = 0; i < nv_appstore_platform_count() && idx < 0; i++)
        if (nv_appstore_platform_get(i, &p) && !strcmp(p.id, s_filter + 1)) idx = i;
    if (idx < 0 || !platform_card_data(idx, &c, &p)) {   // gone with a catalog refresh
        s_filter[0] = 0;
        empty_state(parent, nv_tr(NV_STR_STORE_NO_RESULTS), th->text_dim);
        return false;
    }
    if (s_pv.part > p.parts) s_pv.part = p.parts;
    if (s_pv.part < 1) s_pv.part = 1;
    list_header(parent, c);
    if (p.parts > 1) {
        lv_obj_t *row = box(parent, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, NV_SP_4, 0);
        lv_obj_t *prev = nv_kit_button(row, LV_SYMBOL_LEFT, false);
        char t[40];
        snprintf(t, sizeof t, nv_tr(NV_STR_STORE_PART_FMT), s_pv.part, (int)p.parts);
        label(row, t, &nv_font_20, th->text_dim);
        lv_obj_t *next = nv_kit_button(row, LV_SYMBOL_RIGHT, false);
        if (s_pv.part > 1) lv_obj_add_event_cb(prev, part_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
        else               lv_obj_add_state(prev, LV_STATE_DISABLED);
        if (s_pv.part < p.parts) lv_obj_add_event_cb(next, part_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
        else                     lv_obj_add_state(next, LV_STATE_DISABLED);
    }
    char loaded[16];
    const int lpart = nv_appstore_platform_loaded(loaded, sizeof loaded);
    if (!strcmp(loaded, p.id) && lpart == s_pv.part) return true;
    const nv_store_state_t st = nv_appstore_state();
    if (st == NV_STORE_ERROR && s_pv.tried) {
        empty_state(parent, nv_tr(NV_STR_STORE_UNREACHABLE), th->danger);
        lv_obj_t *rb = nv_kit_button(parent, nv_tr(NV_STR_STORE_RETRY), true);
        lv_obj_add_event_cb(rb, [](lv_event_t *) { s_pv.tried = false; body_refresh(); }, LV_EVENT_CLICKED, nullptr);
        return false;
    }
    // Ask once; a busy store (an install) is asked again when its state changes (store_poll).
    if (st != NV_STORE_FETCHING && st != NV_STORE_INSTALLING && !s_pv.tried)
        s_pv.tried = nv_appstore_platform_open(p.id, s_pv.part);
    lv_obj_t *row = box(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, NV_SP_3, 0);
    lv_obj_t *sp = lv_spinner_create(row);
    lv_obj_set_size(sp, 32, 32);
    label(row, nv_tr(NV_STR_STORE_CONTACTING), &nv_font_20, th->text_dim);
    return false;
}

// Header above a category's (or a platform's) list: monogram, name, description, count.
void list_header(lv_obj_t *parent, const nv_store_category_t &c) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *h = box(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_color(h, th->surface, 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(h, NV_RAD_MD, 0);
    lv_obj_set_style_border_side(h, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(h, 6, 0);
    lv_obj_set_style_border_color(h, cat_color(c), 0);
    lv_obj_set_style_pad_all(h, NV_SP_4, 0);
    lv_obj_set_style_pad_column(h, NV_SP_4, 0);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    monogram(h, c, 72, &nv_font_28);
    lv_obj_t *col = box(h, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_style_pad_row(col, NV_SP_1, 0);
    label(col, c.name, &nv_font_28, th->text_strong);
    if (c.desc[0]) {
        lv_obj_t *d = label(col, c.desc, &nv_font_20, th->text);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(d, lv_pct(100));
    }
    char cnt[32];
    snprintf(cnt, sizeof cnt, nv_tr(NV_STR_STORE_APPS_FMT), (int)c.count);
    label(col, cnt, &nv_font_14, th->text_dim);
}

// Sub-categories of the open category: a second, smaller chip row ("All N", then each sub with its
// count, in catalog order). Only when the category has at least two of them.
void sub_cb(lv_event_t *e) {
    snprintf(s_sub, sizeof s_sub, "%s", (const char *)lv_event_get_user_data(e));
    s_page = 0;
    body_refresh();
}
void sub_chips(lv_obj_t *parent) {
    const NvTheme *th = nv_theme_get();
    static char s_sub_ids[16][24];
    static char s_sub_all[1] = "";
    char names[16][28];
    int counts[16] = {0}, ns = 0, total = 0;
    const int n = nv_appstore_count();
    for (int i = 0; i < n && i < NV_STORE_MAX; i++) {
        nv_store_entry_t e;
        if (!nv_appstore_get(i, &e) || strcmp(e.category, s_filter) != 0 || e.library) continue;
        total++;
        if (!e.subcategory[0]) continue;
        int k = 0;
        while (k < ns && strcmp(s_sub_ids[k], e.subcategory) != 0) k++;
        if (k == ns) {
            if (ns >= 16) continue;
            snprintf(s_sub_ids[ns], sizeof s_sub_ids[ns], "%s", e.subcategory);
            snprintf(names[ns], sizeof names[ns], "%s", e.subcategory_name[0] ? e.subcategory_name : e.subcategory);
            ns++;
        }
        counts[k]++;
    }
    if (ns < 2) { s_sub[0] = 0; return; }
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, NV_SP_2, 0);
    lv_obj_set_scroll_dir(row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *sel = nullptr;
    auto chip = [&](const char *name, int count, char *key) {
        char t[48];
        snprintf(t, sizeof t, "%s  %d", name, count);
        const bool on = !strcmp(s_sub, key);
        lv_obj_t *b = nv_kit_button(row, t, on);
        lv_obj_set_height(b, 40);
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &nv_font_14, 0);
        if (!on) lv_obj_set_style_text_color(lv_obj_get_child(b, 0), th->text_dim, 0);
        lv_obj_add_event_cb(b, sub_cb, LV_EVENT_CLICKED, key);
        if (on) sel = b;
    };
    chip(nv_tr(NV_STR_STORE_ALL), total, s_sub_all);
    for (int k = 0; k < ns; k++) chip(names[k], counts[k], s_sub_ids[k]);
    if (sel) { lv_obj_update_layout(row); lv_obj_scroll_to_view(sel, LV_ANIM_OFF); }
}

// Discover: one shelf per view, its first kShelfCards cards and a "See all". A shelf with nothing
// to show (no installs counted yet, nothing updated) is left out.
void store_discover(lv_obj_t *parent) {
    const NvTheme *th = nv_theme_get();
    static const struct { char key; nv_str_id_t title; } kShelves[] = {
        { '\x01', NV_STR_STORE_FEATURED }, { '\x03', NV_STR_STORE_TOP },
        { '\x04', NV_STR_STORE_NEW },      { '\x05', NV_STR_STORE_RECENT } };
    static char s_keys[4][2] = { "\x01", "\x03", "\x04", "\x05" };
    for (int k = 0; k < 4; k++) {
        snprintf(s_filter, sizeof s_filter, "%s", s_keys[k]);
        int m = store_collect();
        if (kShelves[k].key == '\x03')                           // only apps someone installed
            while (m > 0 && !s_okey[m - 1]) m--;
        if (!m) continue;
        lv_obj_t *hd = box(parent, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hd, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(hd, k ? NV_SP_3 : 0, 0);
        label(hd, nv_tr(kShelves[k].title), &nv_font_20, th->text_strong);
        if (m > kShelfCards) {
            char t[48];
            snprintf(t, sizeof t, "%s  %d  " LV_SYMBOL_RIGHT, nv_tr(NV_STR_STORE_SEE_ALL), m);
            lv_obj_t *b = nv_kit_button(hd, t, false);
            lv_obj_set_style_text_color(lv_obj_get_child(b, 0), th->text_dim, 0);
            lv_obj_add_event_cb(b, see_all_cb, LV_EVENT_CLICKED, s_keys[k]);
        }
        lv_obj_t *g = grid(parent);
        for (int j = 0; j < m && j < kShelfCards; j++) {
            nv_store_entry_t e;
            if (nv_appstore_get(s_order[j], &e)) store_card(g, s_order[j], e, kShelves[k].key);
        }
    }
    s_filter[0] = 0;
    // Consoles: the emulated platforms (store2), their carts one tap away but out of the shelves.
    const int np = nv_appstore_platform_count();
    if (np) {
        static char s_hub_key[2] = "\x07";
        lv_obj_t *hd = box(parent, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hd, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(hd, NV_SP_3, 0);
        label(hd, nv_tr(NV_STR_STORE_CONSOLES), &nv_font_20, th->text_strong);
        if (np > 3) {
            char t[48];
            snprintf(t, sizeof t, "%s  %d  " LV_SYMBOL_RIGHT, nv_tr(NV_STR_STORE_SEE_ALL), np);
            lv_obj_t *b = nv_kit_button(hd, t, false);
            lv_obj_set_style_text_color(lv_obj_get_child(b, 0), th->text_dim, 0);
            lv_obj_add_event_cb(b, see_all_cb, LV_EVENT_CLICKED, s_hub_key);
        }
        plat_cards(parent, 3);
    }
    // Browse by category: the first six curated categories, "See all" opens the Categories page.
    const int nc = nv_appstore_category_count();
    if (nc) {
        static char s_cat_key[2] = "\x06";
        lv_obj_t *hd = box(parent, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(hd, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_top(hd, NV_SP_3, 0);
        label(hd, nv_tr(NV_STR_STORE_CAT_BROWSE), &nv_font_20, th->text_strong);
        if (nc > 6) {
            char t[48];
            snprintf(t, sizeof t, "%s  %d  " LV_SYMBOL_RIGHT, nv_tr(NV_STR_STORE_SEE_ALL), nc);
            lv_obj_t *b = nv_kit_button(hd, t, false);
            lv_obj_set_style_text_color(lv_obj_get_child(b, 0), th->text_dim, 0);
            lv_obj_add_event_cb(b, see_all_cb, LV_EVENT_CLICKED, s_cat_key);
        }
        lv_obj_t *g = cat_grid(parent);
        for (int i = 0; i < nc && i < 6; i++) {
            nv_store_category_t c;
            if (nv_appstore_category_get(i, &c)) cat_card(g, c, i);
        }
    }
    // Everything else: the full list.
    char t[64];
    int natives = 0;
    for (int i = 0; i < nv_appstore_count(); i++) {
        nv_store_entry_t e;
        if (nv_appstore_get(i, &e) && !e.platform[0]) natives++;
    }
    snprintf(t, sizeof t, "%s  %d  " LV_SYMBOL_RIGHT, nv_tr(NV_STR_STORE_APPS), natives);
    static char s_all[2] = "\x02";
    lv_obj_t *b = nv_kit_button(parent, t, true);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_add_event_cb(b, see_all_cb, LV_EVENT_CLICKED, s_all);
    icons_request();
}

void store_list(lv_obj_t *parent) {
    const NvTheme *th = nv_theme_get();
    const nv_store_state_t st = nv_appstore_state();
    const int n = nv_appstore_count();
    if (!nv_sd_is_mounted()) { empty_state(parent, nv_tr(NV_STR_STORE_NO_SD), th->text_dim); return; }
    if (n == 0 && (st == NV_STORE_FETCHING || st == NV_STORE_IDLE)) {
        lv_obj_t *row = box(parent, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, NV_SP_3, 0);
        lv_obj_t *sp = lv_spinner_create(row);
        lv_obj_set_size(sp, 32, 32);
        label(row, nv_tr(NV_STR_STORE_CONTACTING), &nv_font_20, th->text_dim);
        return;
    }
    if (n == 0) {
        const bool offline = st == NV_STORE_ERROR && !nv_wifi_get_connected(nullptr, 0, nullptr, 0, nullptr);
        empty_state(parent, st != NV_STORE_ERROR ? nv_tr(NV_STR_STORE_EMPTY)
                            : offline            ? nv_tr(NV_STR_STORE_OFFLINE)
                                                 : nv_tr(NV_STR_STORE_UNREACHABLE),
                    st == NV_STORE_ERROR ? th->danger : th->text_dim);
        char url[192];
        nv_appstore_get_url(url, sizeof url);
        label(parent, url, &nv_font_14, th->text_dim);
        lv_obj_t *rb = nv_kit_button(parent, nv_tr(NV_STR_STORE_RETRY), true);
        lv_obj_add_event_cb(rb, retry_cb, LV_EVENT_CLICKED, nullptr);
        return;
    }
    store_chips(parent, n);
    // Discover, unless a search is typed: then every app is searched.
    if (!s_filter[0] && !s_query[0]) { store_discover(parent); return; }
    if (s_filter[0] == '\x06' && !s_query[0]) { store_categories(parent); return; }
    if (s_filter[0] == '\x07' && !s_query[0]) { store_platforms(parent); return; }
    if (s_filter[0] == '\x07') s_filter[0] = 0;               // a search from the hub: every app
    if (s_filter[0] == '\x08' && !platform_page(parent)) return;
    if (s_filter[0] > '\x08') { category_header(parent); sub_chips(parent); }
    if (s_filter[0] != '\x08') plat_hits(parent);

    const bool search_all = !s_filter[0];
    if (search_all) s_filter[0] = '\x02';
    const int total = store_collect();
    const char view = s_filter[0];
    if (search_all) s_filter[0] = 0;
    if (!total) { empty_state(parent, nv_tr(NV_STR_STORE_NO_RESULTS), th->text_dim); return; }
    page_clamp(total);
    pager(parent, total);
    lv_obj_t *g = grid(parent);
    for (int v = s_page * kPageCards; v < total && v < (s_page + 1) * kPageCards; v++) {
        nv_store_entry_t e;
        if (nv_appstore_get(s_order[v], &e)) store_card(g, s_order[v], e, view);
    }
    pager(parent, total);
    icons_request();
}

// ---- Installed tab
void installed_list(lv_obj_t *parent) {
    const NvTheme *th = nv_theme_get();
    if (!nv_sd_is_mounted()) { empty_state(parent, nv_tr(NV_STR_STORE_NO_SD), th->text_dim); return; }
    mgr_scan();
    int total = 0;
    for (int i = 0; i < s_mgr_n; i++) if (ci_has(s_mgr[i].name, s_query)) total++;
    char t[48];
    snprintf(t, sizeof t, nv_tr(NV_STR_STORE_COUNT_FMT), s_mgr_n);
    label(parent, t, &nv_font_14, th->text_dim);
    if (!s_mgr_n) { empty_state(parent, nv_tr(NV_STR_STORE_NONE), th->text_dim); return; }
    if (!total) { empty_state(parent, nv_tr(NV_STR_STORE_NO_RESULTS), th->text_dim); return; }
    page_clamp(total);
    pager(parent, total);
    lv_obj_t *g = grid(parent);
    // Apps with an update in the catalog come first (pass 0), with Update as their action; that is
    // where the "updates available" notification lands.
    for (int pass = 0, v = 0; pass < 2; pass++)
    for (int i = 0; i < s_mgr_n; i++) {
        const nv_wasm_app_t &a = s_mgr[i];
        if (!ci_has(a.name, s_query)) continue;
        nv_store_entry_t e;
        const bool in_cat = catalog_find(a.id, &e);
        const bool upd = in_cat && e.update && e.abi <= (uint32_t)NV_WASM_ABI &&
                         strcmp(nv_appstore_installing_id(), a.id) != 0;
        if (upd != (pass == 0)) continue;
        if (v++ / kPageCards != s_page) continue;
        char sub[64] = "";
        if (in_cat && e.author[0]) snprintf(sub, sizeof sub, nv_tr(NV_STR_STORE_BY_FMT), e.author);
        char status[48];
        if (upd) snprintf(status, sizeof status, "v%s -> v%s", a.version, e.version);
        else     snprintf(status, sizeof status, "v%s  -  %ld KB", a.version, installed_kb(&a));
        const bool tile = nv_ui_find_app(a.id) != nullptr;
        if (upd)
            card(g, a.id, a.name, sub, status, th->accent, nv_wasm_app_is_game(&a), false,
                 nv_tr(NV_STR_STORE_UPDATE), install_cb, true);
        else
            card(g, a.id, a.name, sub, status, th->text_dim, nv_wasm_app_is_game(&a), false,
                 tile ? nv_tr(NV_STR_OPEN) : nullptr, tile ? open_cb : nullptr, false);
    }
    pager(parent, total);
}

// ---- detail page
void info_row(lv_obj_t *parent, const char *k, const char *v) {
    if (!v || !v[0]) return;
    const NvTheme *th = nv_theme_get();
    lv_obj_t *row = box(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, NV_SP_3, 0);
    lv_obj_t *kl = label(row, k ? k : "", &nv_font_14, th->text_dim);
    lv_obj_set_width(kl, 150);
    lv_obj_t *vl = label(row, v, &nv_font_14, th->text);
    lv_obj_set_flex_grow(vl, 1);
    lv_label_set_long_mode(vl, LV_LABEL_LONG_WRAP);
}

nv_str_id_t perm_desc(uint32_t bit) {
    switch (bit) {
        case NV_WPERM_NET:    return NV_STR_PERMD_NET;
        case NV_WPERM_LAN:    return NV_STR_PERMD_LAN;
        case NV_WPERM_WS:     return NV_STR_PERMD_WS;
        case NV_WPERM_MQTT:   return NV_STR_PERMD_MQTT;
        case NV_WPERM_HA:     return NV_STR_PERMD_HA;
        case NV_WPERM_FS:     return NV_STR_PERMD_FS;
        case NV_WPERM_CAMERA: return NV_STR_PERMD_CAMERA;
        case NV_WPERM_MIC:    return NV_STR_PERMD_MIC;
        default:              return NV_STR_COUNT;
    }
}

void perms_box(lv_obj_t *parent, uint32_t perms, uint32_t fresh) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *b = box(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_style_bg_color(b, th->surface2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, NV_RAD_MD, 0);
    lv_obj_set_style_pad_all(b, NV_SP_3, 0);
    lv_obj_set_style_pad_row(b, NV_SP_1, 0);
    label(b, nv_tr(NV_STR_PERM_TITLE), &nv_font_14, th->text_dim);
    for (int i = 0; i < 32; i++) {
        const uint32_t bit = 1u << i;
        const nv_str_id_t d = perm_desc(bit);
        if (!(perms & bit) || d == NV_STR_COUNT) continue;
        char line[96];
        snprintf(line, sizeof line, "·  %s", nv_tr(d));
        lv_obj_t *l = label(b, line, &nv_font_20, (fresh & bit) ? th->accent : th->text);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, lv_pct(100));
    }
}

void detail_page(lv_obj_t *parent) {
    const NvTheme *th = nv_theme_get();
    nv_store_entry_t e;
    const bool in_cat = catalog_find(s_detail, &e);
    const nv_wasm_app_t *inst = mgr_find(s_detail);
    if (!in_cat && !inst) { goto_list(); return; }   // gone (uninstalled, catalog refreshed)
    // A stable copy of the id for the buttons' user data.
    static char s_id[32];
    snprintf(s_id, sizeof s_id, "%s", s_detail);
    const bool is_game = in_cat ? e.is_game : nv_wasm_app_is_game(inst);

    char bt[48];
    snprintf(bt, sizeof bt, LV_SYMBOL_LEFT "  %s", nv_tr(NV_STR_BACK));
    lv_obj_t *bk = nv_kit_button(parent, bt, false);
    lv_obj_add_event_cb(bk, back_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *hero = box(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(hero, NV_SP_5, 0);
    lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *img = lv_image_create(hero);
    const int slot = s_ci_n;
    card_icon(img, s_id, is_game, in_cat && e.icon_z > 0);
    if (s_ci_n > slot) s_ci[slot].big = true;       // a fetched icon arriving later is doubled too
    lv_image_set_src(img, big_icon((const lv_image_dsc_t *)lv_image_get_src(img)));

    lv_obj_t *col = lv_obj_create(hero);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, NV_SP_2, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *nm = label(col, in_cat ? e.name : inst->name, &nv_font_28, th->text_strong);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(nm, lv_pct(100));
    if (in_cat && e.author[0]) {
        char by[64];
        snprintf(by, sizeof by, nv_tr(NV_STR_STORE_BY_FMT), e.author);
        label(col, by, &nv_font_20, th->accent);
    }
    char meta[96];
    int mo = 0;
    auto add = [&](const char *fmt, auto... a) {
        if (mo < (int)sizeof meta - 1) {
            const int w = snprintf(meta + mo, sizeof meta - (size_t)mo, fmt, a...);
            if (w > 0) mo = mo + w > (int)sizeof meta - 1 ? (int)sizeof meta - 1 : mo + w;
        }
    };
    meta[0] = 0;
    if (in_cat && e.category_name[0]) add("%s   ", e.category_name);
    add("v%s", inst ? inst->version : e.version);
    long kb = inst ? installed_kb(inst) : (long)((e.size + e.aot_size + 1023) / 1024);
    if (in_cat && e.n_var) {
        variant_default(e);
        for (int k = 0; k < e.n_var; k++)
            if (!strcmp(e.var[k].id, s_var_sel) && e.var[k].size) kb = (long)((e.var[k].size + 1023) / 1024);
    }
    add("   %ld KB", kb);
    if (in_cat && e.rating10) add("   %u.%u/5", (unsigned)(e.rating10 / 10), (unsigned)(e.rating10 % 10));
    if (in_cat && e.downloads) {
        char dl[32];
        snprintf(dl, sizeof dl, nv_tr(NV_STR_STORE_DL_FMT), (unsigned)e.downloads);
        add("   %s", dl);
    }
    label(col, meta, &nv_font_14, th->text_dim);
    if (in_cat && fresh(e.added)) label(col, nv_tr(NV_STR_STORE_NEW_BADGE), &nv_font_14, th->accent);
    const bool sys_app = nv_wasm_is_system_app(s_id);
    if (sys_app) label(col, nv_tr(NV_STR_STORE_SYSTEM_BADGE), &nv_font_14, th->accent);

    // actions
    lv_obj_t *act = box(col, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(act, NV_SP_2, 0);
    lv_obj_set_style_pad_row(act, NV_SP_2, 0);
    lv_obj_set_style_pad_top(act, NV_SP_2, 0);
    const bool busy = !strcmp(nv_appstore_installing_id(), s_id);
    lv_obj_t *status = label(col, "", &nv_font_14, th->primary);
    if (busy) {
        lv_obj_t *b = nv_kit_button(act, nv_tr(NV_STR_STORE_INSTALL), true);
        lv_obj_add_state(b, LV_STATE_DISABLED);
        lv_label_set_text_fmt(status, "%s %d%%", nv_tr(NV_STR_STORE_INSTALLING), nv_appstore_progress());
        s_prog_lbl = status;
    } else if (in_cat && e.abi > (uint32_t)NV_WASM_ABI) {
        lv_label_set_text_fmt(status, "%s (ABI v%u)", nv_tr(NV_STR_STORE_NEEDS_OS), (unsigned)e.abi);
        lv_obj_set_style_text_color(status, th->danger, 0);
    } else {
        if (inst && nv_ui_find_app(s_id)) {
            lv_obj_t *b = nv_kit_button(act, nv_tr(NV_STR_OPEN), true);
            lv_obj_add_event_cb(b, open_cb, LV_EVENT_CLICKED, s_id);
        }
        if (in_cat && (!inst || e.update)) {
            const bool review = !strcmp(s_armed_inst, s_id) && perms_to_accept(s_id);
            lv_obj_t *b = nv_kit_button(act, nv_tr(review ? NV_STR_PERM_ACCEPT
                                                          : inst ? NV_STR_STORE_UPDATE : NV_STR_STORE_INSTALL),
                                        !inst || review);
            lv_obj_add_event_cb(b, install_cb, LV_EVENT_CLICKED, s_id);
            if (review) {
                lv_label_set_text(status, nv_tr(inst ? NV_STR_PERM_NEW : NV_STR_PERM_REVIEW));
                lv_obj_set_style_text_color(status, th->accent, 0);
            }
        }
        if (inst && !sys_app) {   // system apps stay: only Update
            const bool armed = !strcmp(s_armed, s_id);
            lv_obj_t *b = nv_kit_button(act, nv_tr(NV_STR_STORE_UNINSTALL), armed);
            if (armed) lv_obj_set_style_bg_color(b, th->danger, 0);
            else       lv_obj_set_style_text_color(lv_obj_get_child(b, 0), th->danger, 0);
            lv_obj_add_event_cb(b, uninstall_cb, LV_EVENT_CLICKED, s_id);
            if (armed) {
                lv_label_set_text(status, nv_tr(NV_STR_STORE_CONFIRM_DEL));
                lv_obj_set_style_text_color(status, th->danger, 0);
            }
        }
    }

    // Editions: one chip each; the size shown above is the chosen edition's.
    if (in_cat && e.n_var) {
        variant_default(e);
        lv_obj_t *vr = box(parent, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(vr, NV_SP_2, 0);
        lv_obj_set_style_pad_row(vr, NV_SP_2, 0);
        lv_obj_set_flex_align(vr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        label(vr, nv_tr(NV_STR_LANGUAGE), &nv_font_14, th->text_dim);
        static char s_var_ids[NV_STORE_VARIANTS_MAX][9];
        for (int k = 0; k < e.n_var; k++) {
            snprintf(s_var_ids[k], sizeof s_var_ids[k], "%s", e.var[k].id);
            const bool sel = !strcmp(s_var_sel, e.var[k].id);
            lv_obj_t *b = nv_kit_button(vr, e.var[k].name, sel);
            if (!sel) lv_obj_set_style_text_color(lv_obj_get_child(b, 0), th->text_dim, 0);
            lv_obj_add_event_cb(b, variant_cb, LV_EVENT_CLICKED, s_var_ids[k]);
        }
    }

    if (in_cat && e.desc[0]) {
        lv_obj_t *d = label(parent, e.desc, &nv_font_20, th->text);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(d, lv_pct(100));
    }

    // Screenshots, when the store has some (games): a sideways row, filled in as they arrive.
    if (in_cat && e.shots) {
        snprintf(s_shot_id, sizeof s_shot_id, "%s", s_id);
        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), kShotH);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, NV_SP_3, 0);
        lv_obj_set_scroll_dir(row, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        const int n = e.shots > NV_STORE_SHOTS_MAX ? NV_STORE_SHOTS_MAX : e.shots;
        for (int k = 0; k < n; k++) {
            lv_obj_t *im = lv_image_create(row);
            lv_obj_set_size(im, kShotW, kShotH);
            lv_obj_set_style_bg_color(im, th->surface, 0);
            lv_obj_set_style_bg_opa(im, LV_OPA_COVER, 0);
            s_shot[k].img = im;
        }
        s_shot_n = n;
        nv_appstore_shots_want(s_id);
        shots_poll();
    }

    // What changed in this version (the store's "notes"), headed with the version and its day.
    if (in_cat && e.notes[0]) {
        lv_obj_t *wn = box(parent, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_width(wn, lv_pct(100));
        lv_obj_set_style_bg_color(wn, th->surface, 0);
        lv_obj_set_style_bg_opa(wn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(wn, NV_RAD_MD, 0);
        lv_obj_set_style_pad_all(wn, NV_SP_3, 0);
        lv_obj_set_style_pad_row(wn, NV_SP_1, 0);
        char hd[80], d[24];
        fmt_date(d, sizeof d, e.updated ? e.updated : e.added);
        snprintf(hd, sizeof hd, "%s  -  v%s%s%s", nv_tr(NV_STR_STORE_WHATS_NEW), e.version, d[0] ? "  -  " : "", d);
        label(wn, hd, &nv_font_14, th->text_dim);
        lv_obj_t *nt = label(wn, e.notes, &nv_font_20, th->text);
        lv_label_set_long_mode(nt, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(nt, lv_pct(100));
    }

    // What the app may do (sensitive permissions only), before it is installed; on an update the
    // ones the installed version didn't have are highlighted.
    if (in_cat && e.perms) perms_box(parent, e.perms, inst ? e.perms & ~inst->perms : 0u);

    // A terminal program has no window: say so before someone installs it expecting one.
    if (in_cat ? e.console : (inst && inst->console)) {
        lv_obj_t *t = box(parent, LV_FLEX_FLOW_ROW);
        lv_obj_set_width(t, lv_pct(100));
        lv_obj_set_style_bg_color(t, lv_color_hex(0x0c0e10), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(t, NV_RAD_MD, 0);
        lv_obj_set_style_pad_all(t, NV_SP_3, 0);
        lv_obj_set_style_pad_column(t, NV_SP_3, 0);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        label(t, ">_", &nv_font_20, lv_color_hex(0x4ade80));
        lv_obj_t *n = label(t, nv_tr(NV_STR_STORE_TERMINAL), &nv_font_14, lv_color_hex(0xe6e6e6));
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
        lv_obj_set_flex_grow(n, 1);
    }

    // The guide: the device has no browser, so a QR of {store}/docs/<id>.html for the phone.
    if (in_cat && e.has_doc) {
        char url[192];
        nv_appstore_get_url(url, sizeof url);
        const size_t ul = strlen(url);
        snprintf(url + ul, sizeof url - ul, "/docs/%s.html", s_id);
        lv_obj_t *g = box(parent, LV_FLEX_FLOW_ROW);
        lv_obj_set_width(g, lv_pct(100));
        lv_obj_set_style_bg_color(g, th->surface, 0);
        lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(g, NV_RAD_MD, 0);
        lv_obj_set_style_pad_all(g, NV_SP_4, 0);
        lv_obj_set_style_pad_column(g, NV_SP_5, 0);
        lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *qr = lv_qrcode_create(g);
        lv_qrcode_set_size(qr, 136);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        lv_qrcode_set_quiet_zone(qr, true);                 // white margin: scanners need it
        lv_qrcode_update(qr, url, (uint32_t)strlen(url));
        lv_obj_t *gc = box(g, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_grow(gc, 1);
        lv_obj_set_style_pad_row(gc, NV_SP_2, 0);
        label(gc, nv_tr(NV_STR_STORE_GUIDE), &nv_font_20, th->text_strong);
        lv_obj_t *h = label(gc, nv_tr(NV_STR_STORE_GUIDE_SCAN), &nv_font_14, th->text);
        lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(h, lv_pct(100));
        const char *shown = url;
        if (!strncmp(shown, "https://", 8)) shown += 8;
        else if (!strncmp(shown, "http://", 7)) shown += 7;
        lv_obj_t *u = label(gc, shown, &nv_font_14, th->text_dim);
        lv_label_set_long_mode(u, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(u, lv_pct(100));
    }

    // Credits and facts.
    lv_obj_t *facts = box(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(facts, th->surface, 0);
    lv_obj_set_style_bg_opa(facts, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(facts, NV_RAD_MD, 0);
    lv_obj_set_style_pad_all(facts, NV_SP_4, 0);
    lv_obj_set_style_pad_row(facts, NV_SP_2, 0);
    if ((in_cat && e.library) || (inst && inst->library))
        info_row(facts, nv_tr(NV_STR_STORE_COMPONENT), nv_tr(NV_STR_DEP_IS_LIBRARY));
    // Requirements: each one with where it stands on this device.
    const int nd = in_cat ? e.n_deps : (inst ? inst->n_deps : 0);
    for (int k = 0; k < nd; k++) {
        const char *did = in_cat ? e.deps[k].id : inst->deps[k].id;
        const char *want = in_cat ? e.deps[k].version : inst->deps[k].version;
        const char *sys = nv_wasm_sys_component(did);
        bool ready;
        if (sys) {
            ready = nv_wasm_version_ge(sys, want);
        } else {
            const nv_wasm_app_t *have = mgr_find(did);
            ready = have && nv_wasm_version_ge(have->version, want);
        }
        const char *name = nv_wasm_dep_name(did);
        nv_store_entry_t de;
        if (!sys && catalog_find(did, &de)) name = de.name;
        char v[112];
        snprintf(v, sizeof v, "%s %s  -  %s", name, want,
                 nv_tr(ready ? NV_STR_STORE_DEP_READY : sys ? NV_STR_STORE_DEP_SYSTEM : NV_STR_STORE_DEP_INSTALL));
        info_row(facts, k ? "" : nv_tr(NV_STR_STORE_REQUIRES), v);
    }
    if (in_cat) {
        char v[64], d[24];
        snprintf(v, sizeof v, "%s%s%s", e.version,
                 inst && strcmp(inst->version, e.version) ? "  -  " : "",
                 inst && strcmp(inst->version, e.version) ? inst->version : "");
        info_row(facts, nv_tr(NV_STR_STORE_VERSION), v);
        info_row(facts, nv_tr(NV_STR_STORE_CATEGORY), e.category_name);
        fmt_date(d, sizeof d, e.added);
        info_row(facts, nv_tr(NV_STR_STORE_ADDED), d);
        fmt_date(d, sizeof d, e.updated);
        info_row(facts, nv_tr(NV_STR_STORE_UPDATED_ON), d);
        int vo = snprintf(v, sizeof v, "%u KB", (unsigned)((e.size + e.aot_size + 1023) / 1024));
        if (e.files && vo > 0 && vo < (int)sizeof v) {
            snprintf(v + vo, sizeof v - (size_t)vo, "  -  ");
            vo = (int)strlen(v);
            snprintf(v + vo, sizeof v - (size_t)vo, nv_tr(NV_STR_STORE_FILES_FMT), (unsigned)e.files);
        }
        info_row(facts, nv_tr(NV_STR_STORE_SIZE), v);
        info_row(facts, nv_tr(NV_STR_STORE_LICENSE), e.license);
        const char *page = e.source;
        if (!strncmp(page, "https://", 8)) page += 8;
        else if (!strncmp(page, "http://", 7)) page += 7;
        info_row(facts, nv_tr(NV_STR_STORE_WEBPAGE), page);
    }
    if (inst) {
        static const struct { uint32_t bit; nv_str_id_t s; } kPerm[] = {
            { NV_WPERM_GFX, NV_STR_PERM_GFX }, { NV_WPERM_UI, NV_STR_PERM_UI },
            { NV_WPERM_LOG, NV_STR_PERM_LOG }, { NV_WPERM_NET, NV_STR_PERM_NET },
            { NV_WPERM_FS, NV_STR_PERM_FS }, { NV_WPERM_HOME, NV_STR_PERM_HOME } };
        char perms[96] = "";
        int po = 0;
        for (auto &p : kPerm)
            if ((inst->perms & p.bit) && po < (int)sizeof perms - 1) {
                const int w = snprintf(perms + po, sizeof perms - (size_t)po, "%s%s", po ? ", " : "", nv_tr(p.s));
                if (w > 0) po = po + w > (int)sizeof perms - 1 ? (int)sizeof perms - 1 : po + w;
            }
        info_row(facts, nv_tr(NV_STR_STORE_PERMS), perms[0] ? perms : nv_tr(NV_STR_PERM_NONE));
    }
    info_row(facts, "ID", s_id);
    icons_request();
}

// ---- screen
void head_build(void) {
    const NvTheme *th = nv_theme_get();
    if (!s_head) return;
    lv_obj_clean(s_head);
    s_search_ta = nullptr;
    label(s_head, "App Store", &nv_font_28, th->text_strong);
    lv_obj_t *tabs = box(s_head, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs, NV_SP_2, 0);
    const char *names[2] = { nv_tr(NV_STR_STORE_INSTALLED), nv_tr(NV_STR_STORE_STORE) };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = nv_kit_button(tabs, names[i], s_tab == i);
        lv_obj_set_flex_grow(b, 1);
        if (s_tab != i) lv_obj_set_style_text_color(lv_obj_get_child(b, 0), th->text_dim, 0);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    s_search_ta = nv_kit_textarea_ex(s_head, nv_tr(NV_STR_STORE_SEARCH_HINT), true, NV_IME_TEXT, NV_IME_RET_SEARCH);
    lv_obj_set_width(s_search_ta, lv_pct(100));
    lv_textarea_set_text(s_search_ta, s_query);
    lv_obj_add_event_cb(s_search_ta, search_cb, LV_EVENT_VALUE_CHANGED, nullptr);
}

void body_build(void) {
    if (!s_body) return;
    if (s_detail[0]) detail_page(s_body);
    else if (s_tab == 1) store_list(s_body);
    else installed_list(s_body);
}

void wasm_tile_sync(const char *id);

// An install started from this screen has finished: give the app its Home tile now. True if so.
bool store_inst_done(void) {
    if (!s_store_inst[0] || nv_appstore_state() == NV_STORE_INSTALLING) return false;
    if (nv_appstore_state() == NV_STORE_READY) wasm_tile_sync(s_store_inst);
    else nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_STORE_FAILED));
    s_store_inst[0] = 0;
    s_mgr_scanned = false;   // the Installed list changed
    return true;
}

// Poll the async store while the screen is open: a state change (catalog fetched, install done)
// rebuilds the body; a progress tick only patches the install's label; icons that arrived are
// patched into their cards.
void store_poll(lv_timer_t *) {
    if (!s_mgr_col) return;
    const nv_store_state_t st = nv_appstore_state();
    const int pr = nv_appstore_progress();
    const bool done = store_inst_done();
    // An install this screen didn't start (a system app, nv_appstore_system_start) changed it too.
    if (s_store_last == NV_STORE_INSTALLING && st != NV_STORE_INSTALLING) s_mgr_scanned = false;
    if (st != s_store_last || done) {
        s_store_last = st;
        s_store_last_prog = pr;
        body_refresh();
    } else if (st == NV_STORE_INSTALLING && pr != s_store_last_prog && s_prog_lbl) {
        s_store_last_prog = pr;
        lv_label_set_text_fmt(s_prog_lbl, "%s %d%%", nv_tr(NV_STR_STORE_INSTALLING), pr);
    }
    icons_poll();
    shots_poll();
}

void apps_deleted(lv_event_t *) {
    s_mgr_col = s_head = s_body = s_search_ta = nullptr;
    s_prog_lbl = nullptr;
    for (CardIcon &c : s_ci) c.img = nullptr;
    s_ci_n = 0;
    s_detail[0] = s_armed[0] = 0;
    nv_ui_set_back_handler(nullptr);
    if (s_store_timer) { lv_timer_delete(s_store_timer); s_store_timer = nullptr; }
    if (s_search_tmr)  { lv_timer_delete(s_search_tmr);  s_search_tmr = nullptr; }
    // The icon buffers are only referenced by this screen's (now deleted) images.
    for (CardIcon &c : s_ci) lv_image_cache_drop(&c.dsc);
    lv_image_cache_drop(&s_big_dsc);
    shots_release();
    heap_caps_free(s_ci_px);  s_ci_px = nullptr;
    heap_caps_free(s_big_px); s_big_px = nullptr;
}

void apps_build(lv_obj_t *content) {
    if (!s_mgr) {
        s_mgr = (nv_wasm_app_t *)heap_caps_calloc(kMaxWasmApps, sizeof(nv_wasm_app_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_mgr) s_mgr = (nv_wasm_app_t *)calloc(kMaxWasmApps, sizeof(nv_wasm_app_t));
    }
    if (!s_ci_px)  s_ci_px = (uint8_t *)heap_caps_aligned_alloc(64, (size_t)kPageCards * NV_STORE_ICON_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_big_px) s_big_px = (uint8_t *)heap_caps_aligned_alloc(64, (size_t)4 * NV_STORE_ICON_BYTES, MALLOC_CAP_SPIRAM);
    s_mgr_scanned = false;   // fresh scan of /sdcard/apps on each open
    s_filter[0] = 0;
    s_query[0] = 0;
    s_detail[0] = s_armed[0] = 0;
    s_page = 0;
    // Deep link: the "updates available" notification opens the Installed tab (updates first).
    if (const char *pg = nv_ui_take_page("apps"))
        if (!strcmp(pg, "updates")) s_tab = 0;
    s_store_last = nv_appstore_state();
    s_store_last_prog = -1;
    if (s_tab == 1 && (s_store_last == NV_STORE_IDLE || (s_store_last == NV_STORE_ERROR && nv_appstore_count() == 0)))
        nv_appstore_refresh();
    lv_obj_t *c = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(c, apps_deleted, LV_EVENT_DELETE, nullptr);
    s_mgr_col = c;
    s_head = box(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_head, NV_SP_3, 0);
    s_body = box(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_body, NV_SP_3, 0);
    head_build();
    body_build();
    if (!s_store_timer) s_store_timer = lv_timer_create(store_poll, 300, nullptr);
}

const NvApp kAppsApp = {"apps", "Apps", &nv_icon_apps, 1u << 20, apps_build, NV_STR_APP_APPS, nullptr};

}  // namespace

void apps_app_register(void) { nv_app_register(&kAppsApp); }

// ---- store update watch ------------------------------------------------------------------------
// The store is asked for updates of the installed apps in the background: 4 min after boot, then
// every 12 h (30 min after a failed try), only with a network, the store idle and its screen closed
// (a fetch resets the store view). The result is ONE notification (tag "store-upd") whose tap opens
// Apps > Installed, updates first. A new set of updates pops up once; the same set again (the next
// boot, the next check) only refreshes the note in the notification center, and installing them
// shrinks it quietly until it goes away. Settings > Notifications: "store_upd_check" turns it off.
namespace {
constexpr uint32_t kWatchFirstMs = 4 * 60 * 1000;
constexpr uint32_t kWatchEveryMs = 12 * 60 * 60 * 1000;
constexpr uint32_t kWatchRetryMs = 30 * 60 * 1000;
constexpr char     kUpdTag[]     = "store-upd";

uint32_t s_watch_due  = kWatchFirstMs;   // lv_tick of the next background fetch
bool     s_watch_ours = false;           // the fetch in flight is the watch's
uint32_t s_watch_gen  = 0;               // catalog generation last looked at
int      s_upd_n      = -1;              // updates in the note now (-1 = no note yet)
uint32_t s_upd_sig    = 0;

bool net_up(void) { return nv_wifi_get_state() == NV_WIFI_CONNECTED || nv_eth_get_state() == NV_ETH_UP; }

// fetched: a fresh catalog just arrived (the only time a popup is allowed).
void upd_announce(bool fetched) {
    char names[64];
    uint32_t sig = 0;
    const int n = nv_appstore_updates(names, sizeof names, &sig);
    if (n == s_upd_n && sig == s_upd_sig) return;
    s_upd_n = n;
    s_upd_sig = sig;
    if (!n) { nv_notify_remove_tag(kUpdTag); return; }
    const bool news = fetched && sig != (uint32_t)nv_config_get_int("store_upd_sig", 0);
    char m[128];
    if (n == 1) lv_snprintf(m, sizeof m, nv_tr(NV_STR_STORE_UPD_ONE), names);
    else        lv_snprintf(m, sizeof m, nv_tr(NV_STR_STORE_UPD_N), n, names);
    nv_note_opts_t o = {};
    o.tag = kUpdTag;
    o.app = "apps";
    o.page = "updates";
    o.quiet = !news;
    nv_notify_post_ex(NV_NOTE_INFO, "App Store", m, &o);
    if (news) nv_config_set_int("store_upd_sig", (int)sig);
}

void store_watch_tick(lv_timer_t *) {
    const bool on = nv_config_get_bool("store_upd_check", true);
    if (!on) {
        if (s_upd_n > 0) nv_notify_remove_tag(kUpdTag);
        s_upd_n = -1;
        return;
    }
    const uint32_t now = lv_tick_get();
    const uint32_t g = nv_appstore_catalog_gen();
    const nv_store_state_t st = nv_appstore_state();
    if (g != s_watch_gen) {                  // any fetch (ours or the store screen's) completed
        s_watch_gen = g;
        s_watch_ours = false;
        s_watch_due = now + kWatchEveryMs;
        upd_announce(true);
    } else if (s_watch_ours && st != NV_STORE_FETCHING) {   // ours ended without a catalog
        s_watch_ours = false;
        s_watch_due = now + kWatchRetryMs;
    } else if (s_upd_n > 0) {
        upd_announce(false);                 // installs shrink the set
    }
    if ((int32_t)(now - s_watch_due) < 0 || s_watch_ours) return;
    if (s_mgr_col || !net_up() || st == NV_STORE_FETCHING || st == NV_STORE_INSTALLING) return;
    NV_LOGI("apps", "background store check for app updates");
    s_watch_ours = true;
    s_watch_due = now + kWatchRetryMs;
    nv_appstore_refresh();
}
}  // namespace

void nv_apps_store_watch_start(void) {
    static lv_timer_t *t = nullptr;
    if (!t) t = lv_timer_create(store_watch_tick, 30 * 1000, nullptr);
}

namespace {

// Home tile icon shipped with a store install (/sdcard/apps/<id>/icon.z, see nv_appstore): inflated
// once into PSRAM like the built-in icons. nullptr when absent or corrupt (the compiled glyph then).
NV_PSRAM_BSS lv_image_dsc_t s_tile_icon[kMaxWasmApps];
NV_PSRAM_BSS uint8_t *s_tile_icon_px[kMaxWasmApps];

const lv_image_dsc_t *tile_icon_sd(int i, const char *id) {
    char path[80];
    snprintf(path, sizeof path, "/sdcard/apps/%s/icon.z", id);
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    constexpr size_t kCap = 32 * 1024;
    uint8_t *z = (uint8_t *)heap_caps_malloc(kCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t n = z ? fread(z, 1, kCap, f) : 0;
    fclose(f);
    if (!s_tile_icon_px[i] && n && n < kCap)
        s_tile_icon_px[i] = (uint8_t *)heap_caps_aligned_alloc(64, NV_STORE_ICON_BYTES, MALLOC_CAP_SPIRAM);
    lv_image_cache_drop(&s_tile_icon[i]);
    const bool ok = n && n < kCap && s_tile_icon_px[i] && nv_appstore_icon_inflate(z, n, s_tile_icon_px[i]);
    heap_caps_free(z);
    if (!ok) return nullptr;
    lv_image_dsc_t &d = s_tile_icon[i];
    d = {};
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = LV_COLOR_FORMAT_ARGB8888;
    d.header.w = d.header.h = NV_STORE_ICON_PX;
    d.header.stride = NV_STORE_ICON_PX * 4;
    d.data_size = NV_STORE_ICON_BYTES;
    d.data = s_tile_icon_px[i];
    return &d;
}

const lv_image_dsc_t *tile_icon(int i) {
    const nv_wasm_app_t &a = s_installed[i];
    const lv_image_dsc_t *ic = tile_icon_sd(i, a.id);
    if (ic) return ic;
    return a.console ? &nv_icon_terminal : wasm_icon_for(a.id, nv_wasm_app_is_game(&a));
}

// Home tile (+ "Open with" entry) for s_installed[i].
// Broker budget for a WASM app: its heap plus the code it loads. An app.aot stays in PSRAM as
// executable code for the whole run; bytecode is copied for the interpreter. Counting only
// ram_budget let a 15 MB AOT (ScummVM) exhaust PSRAM mid-load and hang the whole UI. Engine
// packages ("engine") point wasm_path at the engine's module, so they are counted right too.
uint32_t wasm_launch_budget(const nv_wasm_app_t &a) {
    char p[sizeof a.wasm_path];
    struct stat st;
    const size_t n = strlen(a.wasm_path);
    uint32_t code = 0;
    if (n > 5 && n < sizeof p) {
        snprintf(p, sizeof p, "%.*s.aot", (int)(n - 5), a.wasm_path);
        if (stat(p, &st) == 0 && st.st_size > 0) code = (uint32_t)st.st_size;
        else if (stat(a.wasm_path, &st) == 0) code = (uint32_t)st.st_size * 2;
    }
    return a.ram_budget + code;
}

void wasm_tile_register(int i) {
    const nv_wasm_app_t &a = s_installed[i];
    if (a.library) return;   // a package other apps require: no tile, nothing to open
    if (a.opens[0] && s_open_h && s_open_ids) {
        snprintf(s_open_ids[i], NV_OPEN_ID_MAX, "%s.open", a.id);
        s_open_h[i] = { s_open_ids[i], a.id, a.opens, -1, a.name, nullptr,
                        (uint8_t)NV_OPEN_OPENER, 0, nullptr, nullptr };
        if (!nv_open_register(&s_open_h[i])) NV_LOGW("apps", "'%s': open handler not registered", a.id);
    }
    // Per-app tile icon comes from the COMPILED set (wasm_icon_for) — flash-resident, so no SD
    // read at scan time. This replaces the old icon.argb loader (wasm_tile_icon) that boot-looped
    // in 1.1.57 loading a PSRAM ARGB dsc during the boot scan; compiled icons sidestep that path.
    s_tiles[i] = { a.id, a.name, tile_icon(i), wasm_launch_budget(a), wasm_tile_build, -1, &a,
                   (a.category[0] ? !strcmp(a.category, "games")
                                  : (nv_wasm_app_is_game(&a) || a.engine[0])) ? NV_APP_FLAG_GAME : 0u };
    nv_app_register(&s_tiles[i]);
}

}  // namespace

// Discover installed WASM apps and register one launcher tile each (called after native apps, once
// the SD is mounted + the demo app is seeded). Broker enforcement comes free via open_app.
void apps_register_wasm(void) {
    // Bring the WAMR runtime up now rather than on the first launch. Its long-lived allocations
    // (runtime tables, sound tasks) then sit with the other boot allocations. Made later, they land
    // between the big PSRAM caches created meanwhile (ANIMA mirrors), and once the broker reclaims
    // those caches the free PSRAM stays split: 22 MB free but a 12 MB largest block, and the
    // Camera's CSI driver found no room for its 4 MB frame.
    nv_wasm_init();
    s_installed = (nv_wasm_app_t *)heap_caps_calloc(kMaxWasmApps, sizeof(nv_wasm_app_t),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_installed) s_installed = (nv_wasm_app_t *)calloc(kMaxWasmApps, sizeof(nv_wasm_app_t));
    s_tiles = (NvApp *)heap_caps_calloc(kMaxWasmApps, sizeof(NvApp),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tiles) s_tiles = (NvApp *)calloc(kMaxWasmApps, sizeof(NvApp));
    if (!s_installed || !s_tiles) return;   // OOM this early means bigger problems; skip tiles
    s_open_h   = (NvOpenHandler *)heap_caps_calloc(kMaxWasmApps, sizeof(NvOpenHandler),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_open_ids = (char (*)[NV_OPEN_ID_MAX])heap_caps_calloc(kMaxWasmApps, NV_OPEN_ID_MAX,
                                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    s_installed_n = nv_wasm_scan(s_installed, kMaxWasmApps);
    for (int i = 0; i < s_installed_n; i++) {
        const nv_wasm_app_t &a = s_installed[i];
        // ABI v7: extensions the app teaches the OS first (boot-time only, before types are
        // queried), then the app itself as an opener for its declared MIME patterns.
        for (int t = 0; t < a.n_file_types && t < NV_WASM_FILE_TYPES_MAX; t++)
            nv_open_register_type(a.file_types[t].ext, a.file_types[t].mime,
                                  wasm_file_kind(a.file_types[t].kind));
        wasm_tile_register(i);
    }
}

namespace {

// A store install while the OS runs (LVGL thread): the app gets its Home tile now, where the boot
// scan would only add it at the next start. An update refreshes the record in place — the tile
// points into it. Declared file types still need a restart (they're boot-time only).
void wasm_tile_sync(const char *id) {
    if (!s_installed || !s_tiles || !id || !id[0]) return;
    auto *fresh = (nv_wasm_app_t *)heap_caps_malloc(sizeof(nv_wasm_app_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fresh) return;
    const bool ok = nv_wasm_load_manifest(id, fresh);
    int i = 0;
    while (ok && i < s_installed_n && strcmp(s_installed[i].id, id) != 0) i++;
    if (ok && i == s_installed_n && s_installed_n >= kMaxWasmApps)
        NV_LOGW("apps", "'%s': launcher full (%d apps), no Home tile", id, kMaxWasmApps);
    const bool fits = ok && i < kMaxWasmApps;
    if (fits) {
        if (i == s_installed_n) s_installed_n++;
        s_installed[i] = *fresh;
    }
    heap_caps_free(fresh);
    if (!fits) return;
    if (nv_ui_find_app(id)) {   // an update: same tile, fresh name / icon / RAM budget
        s_tiles[i].icon = tile_icon(i);
        s_tiles[i].ram_budget = wasm_launch_budget(s_installed[i]);
        return;
    }
    wasm_tile_register(i);   // new, or reinstalled after an uninstall (which dropped the tile)
}

}  // namespace

// An install made outside the Apps screen (the shell's `store install`, ANIMA): the launcher tile.
void nv_apps_store_installed(const char *id) { wasm_tile_sync(id); }
