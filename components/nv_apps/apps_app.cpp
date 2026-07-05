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
};
GameView s_gv;

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

long wasm_size(const nv_wasm_app_t *a) {
    struct stat st;
    return stat(a->wasm_path, &st) == 0 ? (long)st.st_size : 0;
}

void mgr_build(lv_obj_t *col);
void mgr_refresh(void) { if (s_mgr_col) { lv_obj_clean(s_mgr_col); mgr_build(s_mgr_col); } }

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

void mgr_build(lv_obj_t *col) {
    const NvTheme *th = nv_theme_get();
    s_mgr_n = (s_mgr && nv_sd_is_mounted()) ? nv_wasm_scan(s_mgr, kMaxWasmApps) : 0;

    lv_obj_t *head = lv_label_create(col);
    lv_label_set_text(head, "App Store");
    lv_obj_set_style_text_font(head, &nv_font_28, 0);
    lv_obj_set_style_text_color(head, th->text_strong, 0);
    lv_obj_t *cnt = lv_label_create(col);
    lv_label_set_text_fmt(cnt, "%d installed   ·   /sdcard/apps", s_mgr_n);
    lv_obj_set_style_text_font(cnt, &nv_font_14, 0);
    lv_obj_set_style_text_color(cnt, th->text_dim, 0);

    if (s_mgr_n == 0) {
        lv_obj_t *info = nv_kit_info(col);
        lv_label_set_text(info, nv_sd_is_mounted()
            ? "No apps installed yet.\n\nTo add one: put manifest.json + app.wasm in\n"
              "/sdcard/apps/<id>/  then restart the device.\n"
              "Games declare \"abi\": 2 and the \"gfx\" permission."
            : "Insert an SD card to install apps.");
        lv_obj_set_style_text_color(info, th->text_dim, 0);
        return;
    }
    for (int i = 0; i < s_mgr_n; i++) mgr_card(col, &s_mgr[i]);
}

void apps_deleted(lv_event_t *) { s_mgr_col = nullptr; s_armed[0] = 0; }

void apps_build(lv_obj_t *content) {
    if (!s_mgr) {
        s_mgr = (nv_wasm_app_t *)heap_caps_calloc(kMaxWasmApps, sizeof(nv_wasm_app_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_mgr) s_mgr = (nv_wasm_app_t *)calloc(kMaxWasmApps, sizeof(nv_wasm_app_t));
    }
    s_armed[0] = 0;
    lv_obj_t *c = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(c, apps_deleted, LV_EVENT_DELETE, nullptr);
    s_mgr_col = c;
    mgr_build(c);
}

const NvApp kAppsApp = {"apps", "Apps", &nv_icon_apps, 1u << 20, apps_build, NV_STR_APP_APPS, nullptr};

}  // namespace

void apps_app_register(void) { nv_app_register(&kAppsApp); }

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
        s_tiles[i] = { s_installed[i].id, s_installed[i].name, &nv_icon_wasm,
                       s_installed[i].ram_budget, wasm_tile_build, -1, &s_installed[i] };
        nv_app_register(&s_tiles[i]);
    }
}
