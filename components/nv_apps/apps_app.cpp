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

#include "nv_app.h"
#include "nv_ui.h"
#include "nv_ui_kit.h"
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_theme.h"
#include "nv_fonts.h"
#include "nv_notify.h"
#include "nv_wasm.h"
#include "nv_appstore.h"   // remote catalog: install/update apps over Wi-Fi
#include "nv_hal.h"   // nv_hal_touch_points — feed the game canvas full multi-touch
#include "nv_sd.h"

#include "esp_heap_caps.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace {

// Installed apps discovered once at boot. PSRAM-backed and stable for the whole session: the
// launcher NvApp tiles point their .user here, so the records must outlive registration.
constexpr int  kMaxWasmApps = 20;   // registry holds 32 total; leave room for the native apps
nv_wasm_app_t *s_installed  = nullptr;
int            s_installed_n = 0;
NvApp         *s_tiles       = nullptr;   // one launcher descriptor per installed app

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
Runner s_run;
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
};
GameView s_gv;
constexpr uint32_t kGameWedgeMs = 8000;   // generous: a legit frame never takes 8 s

void gv_input_cb(lv_event_t *e) {
    if (!s_gv.canvas) return;
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

// Back gesture while a game is fullscreen -> forward to the game (it pops its own screen and exits
// on its own when at root); the app is NOT closed here.
void gv_back(void) { nv_wasm_gfx_request_back(); }

void gv_poll(lv_timer_t *) {
    uint16_t *fr = nv_wasm_gfx_take_frame();
    if (fr && s_gv.canvas) {
        int w = 0, h = 0; nv_wasm_gfx_size(&w, &h);
        lv_canvas_set_buffer(s_gv.canvas, fr, w, h, LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(s_gv.canvas);
    }
    // Feed the guest the FULL multi-touch set (ABI v3), mapped panel -> canvas the same way the
    // single-pointer path (gv_input_cb) maps finger 0. Games are full-screen landscape (canvas at the
    // panel origin, 1:1), so panel coords line up; still offset/clamp by the canvas rect for safety.
    if (s_gv.active && s_gv.canvas) {
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

void gv_deleted(lv_event_t *) {
    gv_stop_timer();
    if (s_gv.active) { nv_wasm_exec_abort(); s_gv.active = false; }
    s_gv.canvas = s_gv.overlay = nullptr;
    nv_ui_set_back_handler(nullptr);
    nv_ui_app_fullscreen(false);   // restore the status bar / chrome for the launcher
}

void game_view_build(lv_obj_t *content, const nv_wasm_app_t *app) {
    const NvTheme *th = nv_theme_get();
    nv_ui_app_fullscreen(true);   // games own the whole panel — expand content before sizing the canvas
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

    char err[96] = "";
    if (!nv_wasm_exec_start(app, err, sizeof err)) {
        lv_obj_t *msg = lv_label_create(root);
        lv_label_set_text(msg, !strcmp(err, "busy") ? nv_tr(NV_STR_WASM_BUSY) : err);
        lv_obj_set_style_text_color(msg, th->danger, 0);
        return;
    }
    s_gv.active = true;
    s_gv.hb_seq  = nv_wasm_gfx_present_seq();   // seed the wedge watchdog from "now", not from 0
    s_gv.hb_tick = lv_tick_get();
    nv_ui_set_back_handler(gv_back);   // Back navigates inside the game, not straight out

    s_gv.canvas = lv_canvas_create(root);
    uint16_t *buf = nv_wasm_gfx_current();
    if (buf) lv_canvas_set_buffer(s_gv.canvas, buf, (int)app->canvas_w, (int)app->canvas_h,
                                  LV_COLOR_FORMAT_RGB565);
    lv_obj_set_style_radius(s_gv.canvas, 10, 0);
    lv_obj_set_style_clip_corner(s_gv.canvas, false, 0);
    lv_obj_add_flag(s_gv.canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_gv.canvas, gv_input_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(s_gv.canvas, gv_input_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_gv.canvas, gv_input_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_gv.canvas, gv_input_cb, LV_EVENT_PRESS_LOST, nullptr);

    s_gv.overlay = lv_label_create(root);
    lv_label_set_text(s_gv.overlay, "");
    lv_obj_add_flag(s_gv.overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(s_gv.overlay, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_gv.overlay, th->text, 0);

    s_gv.timer = lv_timer_create(gv_poll, 16, nullptr);
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
}

// ---------------------------------------------------------------- Apps store (manager)
// A proper store front: a fresh scan of /sdcard/apps rendered as info cards (icon, name, version,
// GAME/APP badge, size, ABI/RAM/timeout, permission pills) each with an Uninstall action (two-tap
// confirm). Launching is from the Home tiles (solo-mode forbids opening a 2nd app from within one).
nv_wasm_app_t *s_mgr    = nullptr;   // store's own scan (independent of the boot snapshot)
int            s_mgr_n  = 0;
lv_obj_t      *s_mgr_col = nullptr;   // the column we rebuild on refresh
char           s_armed[32] = "";      // id whose Uninstall is armed for confirmation

// Two tabs: 0 = Installed (local /sdcard/apps), 1 = Store (remote catalog over Wi-Fi).
int              s_tab            = 0;
lv_timer_t      *s_store_timer    = nullptr;   // polls the async nv_appstore state while on the Store tab
nv_store_state_t s_store_last     = NV_STORE_IDLE;
int              s_store_last_prog = -1;
lv_obj_t        *s_store_prog_lbl = nullptr;   // installing card's status label — patched in place each tick

long wasm_size(const nv_wasm_app_t *a) {
    struct stat st;
    return stat(a->wasm_path, &st) == 0 ? (long)st.st_size : 0;
}

void mgr_build(lv_obj_t *col);
void mgr_refresh(void) {
    s_store_prog_lbl = nullptr;   // freed by the clean below; rebuilt cards re-register it
    if (s_mgr_col) { lv_obj_clean(s_mgr_col); mgr_build(s_mgr_col); }
}

void mgr_arm_cb(lv_event_t *e) {
    snprintf(s_armed, sizeof s_armed, "%s", (const char *)lv_event_get_user_data(e));
    mgr_refresh();
}
void mgr_cancel_cb(lv_event_t *) { s_armed[0] = 0; mgr_refresh(); }
void mgr_delete_cb(lv_event_t *e) {
    const char *id = (const char *)lv_event_get_user_data(e);
    char err[64] = "";
    if (nv_wasm_uninstall(id, err, sizeof err)) {
        nv_app_unregister(id);                 // remove the Home tile live (no reboot needed)
        nv_toast(NV_NOTE_OK, "Uninstalled");
    } else {
        nv_toast(NV_NOTE_ERROR, err[0] ? err : "uninstall failed");
    }
    s_armed[0] = 0;
    mgr_refresh();
}

lv_obj_t *pill(lv_obj_t *parent, const char *txt, lv_color_t bg, lv_color_t ink) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(p, bg, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(p, 8, 0);
    lv_obj_set_style_pad_ver(p, 3, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &nv_font_14, 0);
    lv_obj_set_style_text_color(l, ink, 0);
    return p;
}

void mgr_card(lv_obj_t *col, const nv_wasm_app_t *a) {
    const NvTheme *th = nv_theme_get();
    const bool game = nv_wasm_app_is_game(a);

    lv_obj_t *card = lv_obj_create(col);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, th->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, NV_RAD_MD, 0);
    lv_obj_set_style_pad_all(card, NV_SP_4, 0);
    lv_obj_set_style_pad_row(card, NV_SP_2, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, th->divider, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // header: letter badge · name/meta · type pill
    lv_obj_t *hr = lv_obj_create(card);
    lv_obj_remove_style_all(hr);
    lv_obj_set_size(hr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hr, NV_SP_4, 0);
    lv_obj_clear_flag(hr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = lv_obj_create(hr);
    lv_obj_remove_style_all(ic);
    lv_obj_set_size(ic, 46, 46);
    lv_obj_set_style_radius(ic, 12, 0);
    lv_obj_set_style_bg_color(ic, game ? th->accent : th->primary, 0);
    lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);
    char letter[2] = { (char)((a->name[0] >= 'a' && a->name[0] <= 'z') ? a->name[0] - 32 : a->name[0]), 0 };
    lv_obj_t *il = lv_label_create(ic);
    lv_label_set_text(il, letter);
    lv_obj_set_style_text_font(il, &nv_font_28, 0);
    lv_obj_set_style_text_color(il, th->on_primary, 0);
    lv_obj_center(il);

    lv_obj_t *tcol = lv_obj_create(hr);
    lv_obj_remove_style_all(tcol);
    lv_obj_set_flex_grow(tcol, 1);
    lv_obj_set_height(tcol, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(tcol, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *nm = lv_label_create(tcol);
    lv_label_set_text(nm, a->name);
    lv_obj_set_style_text_font(nm, &nv_font_20, 0);
    lv_obj_set_style_text_color(nm, th->text_strong, 0);
    lv_obj_t *sub = lv_label_create(tcol);
    lv_label_set_text_fmt(sub, "%s  ·  v%s  ·  %ld KB", a->id, a->version, (wasm_size(a) + 512) / 1024);
    lv_obj_set_style_text_font(sub, &nv_font_14, 0);
    lv_obj_set_style_text_color(sub, th->text_dim, 0);

    pill(hr, game ? "GAME" : "APP", game ? th->accent : th->primary, th->on_primary);

    // spec + permission pills
    lv_obj_t *spec = lv_label_create(card);
    lv_label_set_text_fmt(spec, "ABI v%u   ·   RAM %u KB   ·   stack %u KB   ·   timeout %us",
                          (unsigned)a->abi, (unsigned)(a->ram_budget / 1024),
                          (unsigned)a->stack_kb, (unsigned)(a->timeout_ms / 1000));
    lv_obj_set_style_text_font(spec, &nv_font_14, 0);
    lv_obj_set_style_text_color(spec, th->text_dim, 0);

    lv_obj_t *pr = lv_obj_create(card);
    lv_obj_remove_style_all(pr);
    lv_obj_set_size(pr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(pr, NV_SP_2, 0);
    lv_obj_set_style_pad_row(pr, NV_SP_2, 0);
    lv_obj_clear_flag(pr, LV_OBJ_FLAG_SCROLLABLE);
    const struct { uint32_t bit; const char *nm; } PERMS[] = {
        { NV_WPERM_GFX, "graphics" }, { NV_WPERM_UI, "ui" }, { NV_WPERM_LOG, "log" },
        { NV_WPERM_NET, "net" }, { NV_WPERM_FS, "files" } };
    bool any = false;
    for (auto &pp : PERMS) if (a->perms & pp.bit) { pill(pr, pp.nm, th->surface3, th->text); any = true; }
    if (!any) pill(pr, "no permissions", th->surface3, th->text_dim);

    // actions
    lv_obj_t *ar = lv_obj_create(card);
    lv_obj_remove_style_all(ar);
    lv_obj_set_size(ar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ar, NV_SP_2, 0);
    lv_obj_set_style_pad_top(ar, NV_SP_2, 0);
    lv_obj_clear_flag(ar, LV_OBJ_FLAG_SCROLLABLE);

    if (strcmp(s_armed, a->id) != 0) {
        lv_obj_t *hint = lv_label_create(ar);
        lv_label_set_text(hint, game ? "Open from Home to play" : "Open from Home to run");
        lv_obj_set_style_text_font(hint, &nv_font_14, 0);
        lv_obj_set_style_text_color(hint, th->text_dim, 0);
        lv_obj_set_flex_grow(hint, 1);
        lv_obj_t *ub = nv_kit_button(ar, "UNINSTALL", false);
        lv_obj_set_style_text_color(lv_obj_get_child(ub, 0), th->danger, 0);
        lv_obj_add_event_cb(ub, mgr_arm_cb, LV_EVENT_CLICKED, (void *)a->id);
    } else {
        lv_obj_t *warn = lv_label_create(ar);
        lv_label_set_text_fmt(warn, "Delete %s?", a->name);
        lv_obj_set_style_text_color(warn, th->danger, 0);
        lv_obj_set_flex_grow(warn, 1);
        lv_obj_t *no = nv_kit_button(ar, "CANCEL", false);
        lv_obj_add_event_cb(no, mgr_cancel_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *yes = nv_kit_button(ar, "DELETE", true);
        lv_obj_set_style_bg_color(yes, th->danger, 0);
        lv_obj_add_event_cb(yes, mgr_delete_cb, LV_EVENT_CLICKED, (void *)a->id);
    }
}

// ---------------------------------------------------------------- Store (remote catalog)
// The Store tab lists apps from a remote HTTP server (Settings → Update source shares its host).
// nv_appstore fetches/installs on a worker task; a poll timer rebuilds this column on state changes.
char s_store_ids[NV_STORE_MAX][32];    // stable id strings for the Install button callbacks
char s_store_cats[NV_STORE_MAX][24];   // stable category-id strings for the filter chips
char s_store_filter[24] = "";          // active category filter: "" = All, "\x01" = Featured, else a category id

void store_install_cb(lv_event_t *e) {
    const char *id = (const char *)lv_event_get_user_data(e);
    if (nv_appstore_install(id)) nv_toast(NV_NOTE_INFO, nv_tr(NV_STR_STORE_INSTALLING));
    else                         nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_WASM_BUSY));
    mgr_refresh();
}
void store_retry_cb(lv_event_t *) { nv_appstore_refresh(); mgr_refresh(); }
void store_chip_cb(lv_event_t *e) {
    snprintf(s_store_filter, sizeof s_store_filter, "%s", (const char *)lv_event_get_user_data(e));
    mgr_refresh();
}

// True when entry `e` passes the active category filter.
bool store_visible(const nv_store_entry_t *e) {
    if (!s_store_filter[0])         return true;         // All
    if (s_store_filter[0] == '\x01') return e->featured;  // Featured
    return strcmp(e->category, s_store_filter) == 0;
}

// Stable, distinct badge colour per category (hash into a small pleasant palette).
lv_color_t store_cat_color(const char *cat) {
    static const uint32_t pal[] = { 0x5A9CFF, 0xE0556C, 0x38C878, 0xB07CFF,
                                    0xFF9F40, 0x2EC4C6, 0xF25C9A, 0x8AA0B4 };
    uint32_t h = 0;
    for (const char *p = cat; p && *p; ++p) h = h * 31u + (uint8_t)*p;
    return lv_color_hex(pal[h % (sizeof(pal) / sizeof(pal[0]))]);
}

void store_card(lv_obj_t *col, const nv_store_entry_t *e, const char *id_slot) {
    const NvTheme *th = nv_theme_get();
    const bool too_new  = e->abi > (uint32_t)NV_WASM_ABI;
    const bool busy     = !strcmp(nv_appstore_installing_id(), e->id);

    lv_obj_t *card = lv_obj_create(col);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, th->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, NV_RAD_MD, 0);
    lv_obj_set_style_pad_all(card, NV_SP_4, 0);
    lv_obj_set_style_pad_row(card, NV_SP_2, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, th->divider, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // header: badge · name/meta · type pill
    lv_obj_t *hr = lv_obj_create(card);
    lv_obj_remove_style_all(hr);
    lv_obj_set_size(hr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hr, NV_SP_4, 0);
    lv_obj_clear_flag(hr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = lv_obj_create(hr);
    lv_obj_remove_style_all(ic);
    lv_obj_set_size(ic, 46, 46);
    lv_obj_set_style_radius(ic, 12, 0);
    lv_obj_set_style_bg_color(ic, e->category[0] ? store_cat_color(e->category)
                                                 : (e->is_game ? th->accent : th->primary), 0);
    lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ic, LV_OBJ_FLAG_SCROLLABLE);
    char letter[2] = { (char)((e->name[0] >= 'a' && e->name[0] <= 'z') ? e->name[0] - 32 : e->name[0]), 0 };
    lv_obj_t *il = lv_label_create(ic);
    lv_label_set_text(il, letter);
    lv_obj_set_style_text_font(il, &nv_font_28, 0);
    lv_obj_set_style_text_color(il, th->on_primary, 0);
    lv_obj_center(il);

    lv_obj_t *tcol = lv_obj_create(hr);
    lv_obj_remove_style_all(tcol);
    lv_obj_set_flex_grow(tcol, 1);
    lv_obj_set_height(tcol, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tcol, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(tcol, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *nm = lv_label_create(tcol);
    lv_label_set_text(nm, e->name);
    lv_obj_set_style_text_font(nm, &nv_font_20, 0);
    lv_obj_set_style_text_color(nm, th->text_strong, 0);
    lv_obj_t *sub = lv_label_create(tcol);
    char meta[96];
    int mo = 0;
    if (e->category_name[0]) mo += snprintf(meta + mo, sizeof meta - mo, "%s   ·   ", e->category_name);
    mo += snprintf(meta + mo, sizeof meta - mo, "v%s", e->version);
    if (e->size)     mo += snprintf(meta + mo, sizeof meta - mo, "   ·   %u KB", (unsigned)((e->size + 512) / 1024));
    if (e->rating10) mo += snprintf(meta + mo, sizeof meta - mo, "   ·   %u.%u/5",
                                    (unsigned)(e->rating10 / 10), (unsigned)(e->rating10 % 10));
    lv_label_set_text(sub, meta);
    lv_obj_set_style_text_font(sub, &nv_font_14, 0);
    lv_obj_set_style_text_color(sub, th->text_dim, 0);

    if (e->featured) pill(hr, nv_tr(NV_STR_STORE_FEATURED), th->accent, th->on_primary);
    pill(hr, e->is_game ? "GAME" : "APP", e->is_game ? th->accent : th->primary, th->on_primary);

    if (e->desc[0]) {
        lv_obj_t *ds = lv_label_create(card);
        lv_label_set_text(ds, e->desc);
        lv_label_set_long_mode(ds, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(ds, lv_pct(100));
        lv_obj_set_style_text_font(ds, &nv_font_14, 0);
        lv_obj_set_style_text_color(ds, th->text, 0);
    }

    // actions: status text on the left, install/update button on the right
    lv_obj_t *ar = lv_obj_create(card);
    lv_obj_remove_style_all(ar);
    lv_obj_set_size(ar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ar, NV_SP_2, 0);
    lv_obj_set_style_pad_top(ar, NV_SP_2, 0);
    lv_obj_clear_flag(ar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(ar);
    lv_obj_set_flex_grow(lbl, 1);
    lv_obj_set_style_text_font(lbl, &nv_font_14, 0);

    if (busy) {
        lv_label_set_text_fmt(lbl, "%s  %d%%", nv_tr(NV_STR_STORE_INSTALLING), nv_appstore_progress());
        lv_obj_set_style_text_color(lbl, th->primary, 0);
        s_store_prog_lbl = lbl;   // let the poll patch just this label as % climbs (no full rebuild)
    } else if (too_new) {
        lv_label_set_text_fmt(lbl, "%s (ABI v%u)", nv_tr(NV_STR_STORE_NEEDS_OS), (unsigned)e->abi);
        lv_obj_set_style_text_color(lbl, th->danger, 0);
    } else if (e->installed && !e->update) {
        lv_label_set_text(lbl, nv_tr(NV_STR_STORE_INSTALLED));
        lv_obj_set_style_text_color(lbl, th->text_dim, 0);
    } else {
        lv_label_set_text(lbl, nv_tr(e->update ? NV_STR_STORE_UPDATE_AVAIL : NV_STR_STORE_NOT_INSTALLED));
        lv_obj_set_style_text_color(lbl, e->update ? th->accent : th->text_dim, 0);
        lv_obj_t *b = nv_kit_button(ar, nv_tr(e->update ? NV_STR_STORE_UPDATE : NV_STR_STORE_INSTALL), true);
        lv_obj_add_event_cb(b, store_install_cb, LV_EVENT_CLICKED, (void *)id_slot);
    }
}

// A horizontal wrap of filter chips: All · Featured (if any) · each distinct category. The selected
// chip is filled; tapping one sets s_store_filter and rebuilds. Category id strings are cached in
// s_store_cats[] so the chip callbacks get a stable pointer.
void store_chips(lv_obj_t *col, int n) {
    const NvTheme *th = nv_theme_get();
    static char s_all[1] = "";
    static char s_feat[2] = "\x01";

    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);        // single row, scrolls sideways when it overflows
    lv_obj_set_style_pad_column(row, NV_SP_2, 0);
    lv_obj_set_scroll_dir(row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

    auto add_chip = [&](const char *label, const char *key, bool sel) {
        lv_obj_t *b = nv_kit_button(row, label, sel);
        if (!sel) lv_obj_set_style_text_color(lv_obj_get_child(b, 0), th->text_dim, 0);
        lv_obj_add_event_cb(b, store_chip_cb, LV_EVENT_CLICKED, (void *)key);
    };

    add_chip(nv_tr(NV_STR_STORE_ALL), s_all, s_store_filter[0] == 0);

    bool any_featured = false;
    for (int i = 0; i < n && i < NV_STORE_MAX; i++) {
        nv_store_entry_t e;
        if (nv_appstore_get(i, &e) && e.featured) { any_featured = true; break; }
    }
    if (any_featured) add_chip(nv_tr(NV_STR_STORE_FEATURED), s_feat, s_store_filter[0] == '\x01');

    // distinct categories, in first-seen order, cached for stable chip pointers
    int cn = 0;
    for (int i = 0; i < n && i < NV_STORE_MAX; i++) {
        nv_store_entry_t e;
        if (!nv_appstore_get(i, &e) || !e.category[0]) continue;
        bool seen = false;
        for (int k = 0; k < cn; k++) if (!strcmp(s_store_cats[k], e.category)) { seen = true; break; }
        if (seen || cn >= NV_STORE_MAX) continue;
        snprintf(s_store_cats[cn], sizeof s_store_cats[cn], "%s", e.category);
        const char *label = e.category_name[0] ? e.category_name : e.category;
        add_chip(label, s_store_cats[cn], !strcmp(s_store_filter, e.category));
        cn++;
    }
}

void store_build(lv_obj_t *col) {
    const NvTheme *th = nv_theme_get();
    char url[192], region[16];
    nv_appstore_get_url(url, sizeof url);
    nv_appstore_get_region(region, sizeof region);
    const nv_store_state_t st = nv_appstore_state();

    lv_obj_t *src = lv_label_create(col);
    lv_label_set_text_fmt(src, "%s:  %s   ·   %s: %s", nv_tr(NV_STR_STORE_SOURCE), url,
                          nv_tr(NV_STR_STORE_REGION), region[0] ? region : "*");
    lv_label_set_long_mode(src, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(src, lv_pct(100));
    lv_obj_set_style_text_font(src, &nv_font_14, 0);
    lv_obj_set_style_text_color(src, th->text_dim, 0);

    if (!nv_sd_is_mounted()) {
        lv_obj_t *info = nv_kit_info(col);
        lv_label_set_text(info, nv_tr(NV_STR_STORE_NO_SD));
        lv_obj_set_style_text_color(info, th->text_dim, 0);
        return;
    }

    const int n = nv_appstore_count();
    if (st == NV_STORE_FETCHING && n == 0) {
        lv_obj_t *sp = lv_spinner_create(col);
        lv_obj_set_size(sp, 40, 40);
        lv_obj_t *info = nv_kit_info(col);
        lv_label_set_text(info, nv_tr(NV_STR_STORE_CONTACTING));
        lv_obj_set_style_text_color(info, th->text_dim, 0);
        return;
    }
    if (st == NV_STORE_ERROR && n == 0) {
        lv_obj_t *info = nv_kit_info(col);
        lv_label_set_text(info, nv_appstore_message());
        lv_obj_set_style_text_color(info, th->danger, 0);
        lv_obj_t *rb = nv_kit_button(col, nv_tr(NV_STR_STORE_RETRY), true);
        lv_obj_add_event_cb(rb, store_retry_cb, LV_EVENT_CLICKED, nullptr);
        return;
    }
    if (n == 0) {
        lv_obj_t *info = nv_kit_info(col);
        lv_label_set_text(info, nv_tr(NV_STR_STORE_EMPTY));
        lv_obj_set_style_text_color(info, th->text_dim, 0);
        lv_obj_t *rb = nv_kit_button(col, nv_tr(NV_STR_STORE_REFRESH), true);
        lv_obj_add_event_cb(rb, store_retry_cb, LV_EVENT_CLICKED, nullptr);
        return;
    }

    store_chips(col, n);   // All · Featured · categories

    int shown = 0;
    for (int i = 0; i < n && i < NV_STORE_MAX; i++) {
        nv_store_entry_t e;
        if (!nv_appstore_get(i, &e)) continue;
        if (!store_visible(&e)) continue;
        snprintf(s_store_ids[i], sizeof s_store_ids[i], "%s", e.id);
        store_card(col, &e, s_store_ids[i]);
        shown++;
    }
    if (!shown) {   // filter matched nothing (e.g. category emptied after an uninstall)
        lv_obj_t *info = nv_kit_info(col);
        lv_label_set_text(info, nv_tr(NV_STR_STORE_EMPTY));
        lv_obj_set_style_text_color(info, th->text_dim, 0);
    }
}

// Segmented [ Installed | Store ] header — the tab picker rebuilt with the column.
void tab_cb(lv_event_t *e) {
    const int tab = (int)(intptr_t)lv_event_get_user_data(e);
    if (tab == s_tab) return;
    s_tab = tab;
    s_armed[0] = 0;
    if (s_tab == 1 && nv_appstore_state() == NV_STORE_IDLE) nv_appstore_refresh();   // first visit → fetch
    mgr_refresh();
}

void tabs_header(lv_obj_t *col) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, NV_SP_2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    const char *names[2] = { nv_tr(NV_STR_STORE_INSTALLED), nv_tr(NV_STR_STORE_STORE) };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = nv_kit_button(row, names[i], s_tab == i);
        lv_obj_set_flex_grow(b, 1);
        if (s_tab != i) lv_obj_set_style_text_color(lv_obj_get_child(b, 0), th->text_dim, 0);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

void mgr_build(lv_obj_t *col) {
    const NvTheme *th = nv_theme_get();

    lv_obj_t *head = lv_label_create(col);
    lv_label_set_text(head, "App Store");
    lv_obj_set_style_text_font(head, &nv_font_28, 0);
    lv_obj_set_style_text_color(head, th->text_strong, 0);

    tabs_header(col);

    if (s_tab == 1) { store_build(col); return; }

    // ---- Installed tab: a fresh scan of /sdcard/apps ----
    s_mgr_n = (s_mgr && nv_sd_is_mounted()) ? nv_wasm_scan(s_mgr, kMaxWasmApps) : 0;
    lv_obj_t *cnt = lv_label_create(col);
    lv_label_set_text_fmt(cnt, "%d installed   ·   /sdcard/apps", s_mgr_n);
    lv_obj_set_style_text_font(cnt, &nv_font_14, 0);
    lv_obj_set_style_text_color(cnt, th->text_dim, 0);

    if (s_mgr_n == 0) {
        lv_obj_t *info = nv_kit_info(col);
        lv_label_set_text(info, nv_sd_is_mounted()
            ? "No apps installed yet.\n\nBrowse the Store tab to install over Wi-Fi, or put\n"
              "manifest.json + app.wasm in /sdcard/apps/<id>/ and restart.\n"
              "Games declare \"abi\": 2 and the \"gfx\" permission."
            : "Insert an SD card to install apps.");
        lv_obj_set_style_text_color(info, th->text_dim, 0);
        return;
    }
    for (int i = 0; i < s_mgr_n; i++) mgr_card(col, &s_mgr[i]);
}

// Poll the async store while the Store tab is open. A state change (fetch done, install finished)
// rebuilds the list; a bare progress tick only patches the installing card's label — no rebuild,
// so an install counts up smoothly without the whole page flickering.
void store_poll(lv_timer_t *) {
    if (s_tab != 1 || !s_mgr_col) return;
    const nv_store_state_t st = nv_appstore_state();
    const int pr = nv_appstore_progress();
    if (st != s_store_last) {
        s_store_last = st;
        s_store_last_prog = pr;
        mgr_refresh();
    } else if (st == NV_STORE_INSTALLING && pr != s_store_last_prog && s_store_prog_lbl) {
        s_store_last_prog = pr;
        lv_label_set_text_fmt(s_store_prog_lbl, "%s  %d%%", nv_tr(NV_STR_STORE_INSTALLING), pr);
    }
}

void apps_deleted(lv_event_t *) {
    s_mgr_col = nullptr;
    s_armed[0] = 0;
    if (s_store_timer) { lv_timer_del(s_store_timer); s_store_timer = nullptr; }
}

void apps_build(lv_obj_t *content) {
    if (!s_mgr) {
        s_mgr = (nv_wasm_app_t *)heap_caps_calloc(kMaxWasmApps, sizeof(nv_wasm_app_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_mgr) s_mgr = (nv_wasm_app_t *)calloc(kMaxWasmApps, sizeof(nv_wasm_app_t));
    }
    s_armed[0] = 0;
    s_store_filter[0] = 0;   // reset category filter to All on each open
    s_store_last = NV_STORE_IDLE;
    s_store_last_prog = -1;
    lv_obj_t *c = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(c, apps_deleted, LV_EVENT_DELETE, nullptr);
    s_mgr_col = c;
    if (!s_store_timer) s_store_timer = lv_timer_create(store_poll, 300, nullptr);
    mgr_build(c);
}

const NvApp kAppsApp = {"apps", "Apps", &nv_icon_apps, 1u << 20, apps_build, NV_STR_APP_APPS, nullptr};

}  // namespace

void apps_app_register(void) { nv_app_register(&kAppsApp); }

// Optional per-app launcher icon: /sdcard/apps/<id>/icon.argb — raw 80×80 ARGB8888 (25600 B),
// produced by tools/make_app_icon.py. Loaded once into PSRAM at scan time; fallback = the generic
// puzzle tile (which every WASM app shared before — indistinguishable side by side).
static const lv_image_dsc_t *wasm_tile_icon(const char *id) {
    char p[160];
    snprintf(p, sizeof p, "/sdcard/apps/%s/icon.argb", id);
    FILE *f = fopen(p, "rb");
    if (!f) return &nv_icon_wasm;
    const size_t bytes = 80 * 80 * 4;
    // 64-byte aligned like every other buffer a draw engine may read (PPA/DMA2D want cache-line
    // alignment on the P4; the flash-resident icons get it for free from the linker).
    uint8_t *px = (uint8_t *)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_image_dsc_t *d = (lv_image_dsc_t *)heap_caps_calloc(1, sizeof(lv_image_dsc_t),
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!px || !d || fread(px, 1, bytes, f) != bytes) {
        if (px) heap_caps_free(px);
        if (d) heap_caps_free(d);
        fclose(f);
        return &nv_icon_wasm;
    }
    fclose(f);
    d->header.magic  = LV_IMAGE_HEADER_MAGIC;
    d->header.cf     = LV_COLOR_FORMAT_ARGB8888;
    d->header.w      = 80;
    d->header.h      = 80;
    d->header.stride = 320;
    d->data_size     = bytes;
    d->data          = px;
    return d;
}

// Discover installed WASM apps and register one launcher tile each (called after native apps, once
// the SD is mounted + the demo app is seeded). Broker enforcement comes free via open_app.
void apps_register_wasm(void) {
    s_installed = (nv_wasm_app_t *)heap_caps_calloc(kMaxWasmApps, sizeof(nv_wasm_app_t),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_installed) s_installed = (nv_wasm_app_t *)calloc(kMaxWasmApps, sizeof(nv_wasm_app_t));
    s_tiles = (NvApp *)heap_caps_calloc(kMaxWasmApps, sizeof(NvApp),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tiles) s_tiles = (NvApp *)calloc(kMaxWasmApps, sizeof(NvApp));
    if (!s_installed || !s_tiles) return;   // OOM this early means bigger problems; skip tiles

    s_installed_n = nv_wasm_scan(s_installed, kMaxWasmApps);
    for (int i = 0; i < s_installed_n; i++) {
        // TODO(icons): wasm_tile_icon(id) shipped in 1.1.57 boot-looped the board (crash before
        // the web server, marked-valid so no rollback — recovered via a served 1.1.58). The dsc
        // is now byte-equivalent to the flash icons (same ARGB8888/80×80/stride 320 header) and
        // the pixel buffer is 64B-aligned, so the two code-level suspects are closed; the actual
        // panic is still unconfirmed. Re-land only with the board on USB serial — and note the
        // deferred mark-valid in nv_ota.cpp now auto-rolls-back a boot-looping image, so a repeat
        // can no longer brick.
        s_tiles[i] = { s_installed[i].id, s_installed[i].name, &nv_icon_wasm,
                       s_installed[i].ram_budget, wasm_tile_build, -1, &s_installed[i] };
        nv_app_register(&s_tiles[i]);
    }
}
