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
#include "nv_open.h"       // ABI v7: installed apps as "Open with" targets + launch-file grant
#include "nv_appstore.h"   // remote catalog: install/update apps over Wi-Fi
#include "nv_hal.h"   // nv_hal_touch_points — feed the game canvas full multi-touch
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
    int bl = nv_wasm_gfx_take_backlight();   // ABI v4: apply the guest's backlight request on THIS thread
    if (bl >= 0) { nv_hal_backlight_set(bl); s_gv.bl_touched = true; }
    int dx = 0, dy = 0, dw = 0, dh = 0;
    uint16_t *fr = nv_wasm_gfx_take_frame_ex(&dx, &dy, &dw, &dh);
    if (fr && s_gv.canvas) {
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
        // Error: no guest is left to consume gv_back's back_req, so restore the default Back
        // (close the app) — the home pill is hidden in fullscreen and Back was dead. Hide the
        // canvas too: the engine is IDLE with its buffers reclaimable (wasm_reclaim / a bigger
        // gfx_open frees them), and a visible canvas would keep reading freed PSRAM on redraw.
        nv_ui_set_back_handler(nullptr);
        if (s_gv.canvas) lv_obj_add_flag(s_gv.canvas, LV_OBJ_FLAG_HIDDEN);
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
    if (s_gv.bl_touched) {   // ABI v4: a backlight app (torch) ran -> restore the user's brightness
        nv_hal_backlight_set(nv_config_get_int("brightness", 90));
        s_gv.bl_touched = false;
    }
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
    // Launcher tile / nv_open launch: grant the file the game was opened on (ABI v7), if any.
    const NvIntent *in = nv_open_intent();
    nv_wasm_exec_set_launch_file(in && in->verb == NV_INTENT_OPEN ? in->path : nullptr);
    if (!nv_wasm_exec_start(app, err, sizeof err)) {
        lv_obj_t *msg = lv_label_create(root);
        lv_label_set_text(msg, !strcmp(err, "busy") ? nv_tr(NV_STR_WASM_BUSY) : err);
        lv_obj_set_style_text_color(msg, th->danger, 0);
        return;
    }
    s_gv.active = true;
    s_gv.bl_touched = false;                    // fresh run: no backlight change yet
    s_gv.last_fb = nullptr;                      // ABI v6: force a set_buffer on the first frame
    s_gv.hb_seq  = nv_wasm_gfx_present_seq();   // seed the wedge watchdog from "now", not from 0
    s_gv.hb_tick = lv_tick_get();
    nv_ui_set_back_handler(gv_back);   // Back navigates inside the game, not straight out

    s_gv.canvas = lv_canvas_create(root);
    uint16_t *buf = nv_wasm_gfx_current();
    // Bind with the SAME clamp gfx_open applies to the allocation (16..1024 x 16..600): a store
    // manifest saying 4096x4096 made LVGL read 32 MB from a 1.2 MB buffer on the first render.
    int cw = (int)app->canvas_w, ch = (int)app->canvas_h;
    if (cw < 16) cw = 16; if (cw > 1024) cw = 1024;
    if (ch < 16) ch = 16; if (ch > 600)  ch = 600;
    if (buf) lv_canvas_set_buffer(s_gv.canvas, buf, cw, ch, LV_COLOR_FORMAT_RGB565);
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
char           s_store_inst[32] = "";        // id being installed: gets its Home tile when done
nv_store_state_t s_store_last = NV_STORE_IDLE;
int            s_store_last_prog = -1;
lv_obj_t      *s_prog_lbl   = nullptr;       // label showing the running install's %, patched in place
NV_PSRAM_BSS char s_ids[NV_STORE_MAX][32];   // stable id strings for event user data
NV_PSRAM_BSS char s_cats[NV_STORE_MAX][24];  // stable category ids for the chips
NV_PSRAM_BSS char s_filter[24];              // chip: "" = all, "\x01" = featured, else a category id

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

void body_build(void);
void head_build(void);
void body_refresh(void) {
    s_prog_lbl = nullptr;
    for (CardIcon &c : s_ci) c.img = nullptr;
    s_ci_n = 0;
    if (s_body) { lv_obj_clean(s_body); body_build(); }
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
    if (s_filter[0] == '\x01' && !e->featured) return false;
    if (s_filter[0] && s_filter[0] != '\x01' && strcmp(e->category, s_filter) != 0) return false;
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
void install_cb(lv_event_t *e) {
    const char *id = (const char *)lv_event_get_user_data(e);
    if (nv_appstore_install(id)) snprintf(s_store_inst, sizeof s_store_inst, "%s", id);
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
    char err[64] = "";
    if (nv_wasm_uninstall(id, err, sizeof err)) {
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

// ---- Store tab
void store_chips(lv_obj_t *parent, int n) {
    const NvTheme *th = nv_theme_get();
    static char s_all[1] = "";
    static char s_feat[2] = "\x01";
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
    for (int i = 0; i < n && i < NV_STORE_MAX; i++) {
        nv_store_entry_t e;
        if (!nv_appstore_get(i, &e)) continue;
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
    lv_obj_t *sel = chip(nv_tr(NV_STR_STORE_ALL), n, s_all, s_filter[0] == 0);
    if (featured) {
        lv_obj_t *b = chip(nv_tr(NV_STR_STORE_FEATURED), -1, s_feat, s_filter[0] == '\x01');
        if (s_filter[0] == '\x01') sel = b;
    }
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
        empty_state(parent, st == NV_STORE_ERROR ? nv_tr(NV_STR_STORE_UNREACHABLE) : nv_tr(NV_STR_STORE_EMPTY),
                    st == NV_STORE_ERROR ? th->danger : th->text_dim);
        char url[192];
        nv_appstore_get_url(url, sizeof url);
        label(parent, url, &nv_font_14, th->text_dim);
        lv_obj_t *rb = nv_kit_button(parent, nv_tr(NV_STR_STORE_RETRY), true);
        lv_obj_add_event_cb(rb, retry_cb, LV_EVENT_CLICKED, nullptr);
        return;
    }
    store_chips(parent, n);

    int total = 0;
    for (int i = 0; i < n && i < NV_STORE_MAX; i++) {
        nv_store_entry_t e;
        if (nv_appstore_get(i, &e) && store_match(&e)) total++;
    }
    if (!total) { empty_state(parent, nv_tr(NV_STR_STORE_NO_RESULTS), th->text_dim); return; }
    page_clamp(total);
    pager(parent, total);
    lv_obj_t *g = grid(parent);
    const char *busy_id = nv_appstore_installing_id();
    for (int i = 0, v = 0; i < n && i < NV_STORE_MAX; i++) {
        nv_store_entry_t e;
        if (!nv_appstore_get(i, &e) || !store_match(&e)) continue;
        if (v++ / kPageCards != s_page) continue;
        snprintf(s_ids[i], sizeof s_ids[i], "%s", e.id);
        const bool installed = mgr_find(e.id) != nullptr;
        const bool busy = !strcmp(busy_id, e.id);
        const bool too_new = e.abi > (uint32_t)NV_WASM_ABI;
        char sub[64] = "";
        if (e.author[0]) snprintf(sub, sizeof sub, nv_tr(NV_STR_STORE_BY_FMT), e.author);
        else             snprintf(sub, sizeof sub, "%s", e.category_name);
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
            action = nv_tr(NV_STR_STORE_INSTALL); cb = install_cb;
        }
        if (busy) cb = nullptr;
        card(g, s_ids[i], e.name, sub, status, sc, e.is_game, e.icon_z > 0, action, cb, primary);
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
    for (int i = 0, v = 0; i < s_mgr_n; i++) {
        const nv_wasm_app_t &a = s_mgr[i];
        if (!ci_has(a.name, s_query)) continue;
        if (v++ / kPageCards != s_page) continue;
        nv_store_entry_t e;
        const bool in_cat = catalog_find(a.id, &e);
        char sub[64] = "";
        if (in_cat && e.author[0]) snprintf(sub, sizeof sub, nv_tr(NV_STR_STORE_BY_FMT), e.author);
        char status[48];
        snprintf(status, sizeof status, "v%s  -  %ld KB", a.version, installed_kb(&a));
        const bool tile = nv_ui_find_app(a.id) != nullptr;
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
    lv_obj_t *kl = label(row, k, &nv_font_14, th->text_dim);
    lv_obj_set_width(kl, 150);
    lv_obj_t *vl = label(row, v, &nv_font_14, th->text);
    lv_obj_set_flex_grow(vl, 1);
    lv_label_set_long_mode(vl, LV_LABEL_LONG_WRAP);
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
    add("   %ld KB", inst ? installed_kb(inst) : (long)((e.size + e.aot_size + 1023) / 1024));
    if (in_cat && e.rating10) add("   %u.%u/5", (unsigned)(e.rating10 / 10), (unsigned)(e.rating10 % 10));
    label(col, meta, &nv_font_14, th->text_dim);

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
            lv_obj_t *b = nv_kit_button(act, nv_tr(inst ? NV_STR_STORE_UPDATE : NV_STR_STORE_INSTALL), !inst);
            lv_obj_add_event_cb(b, install_cb, LV_EVENT_CLICKED, s_id);
        }
        if (inst) {
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

    if (in_cat && e.desc[0]) {
        lv_obj_t *d = label(parent, e.desc, &nv_font_20, th->text);
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(d, lv_pct(100));
    }

    // Credits and facts.
    lv_obj_t *facts = box(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(facts, th->surface, 0);
    lv_obj_set_style_bg_opa(facts, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(facts, NV_RAD_MD, 0);
    lv_obj_set_style_pad_all(facts, NV_SP_4, 0);
    lv_obj_set_style_pad_row(facts, NV_SP_2, 0);
    if (in_cat) {
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
    if (st != s_store_last || done) {
        s_store_last = st;
        s_store_last_prog = pr;
        body_refresh();
    } else if (st == NV_STORE_INSTALLING && pr != s_store_last_prog && s_prog_lbl) {
        s_store_last_prog = pr;
        lv_label_set_text_fmt(s_prog_lbl, "%s %d%%", nv_tr(NV_STR_STORE_INSTALLING), pr);
    }
    icons_poll();
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
void wasm_tile_register(int i) {
    const nv_wasm_app_t &a = s_installed[i];
    if (a.opens[0] && s_open_h && s_open_ids) {
        snprintf(s_open_ids[i], NV_OPEN_ID_MAX, "%s.open", a.id);
        s_open_h[i] = { s_open_ids[i], a.id, a.opens, -1, a.name, nullptr,
                        (uint8_t)NV_OPEN_OPENER, 0, nullptr, nullptr };
        if (!nv_open_register(&s_open_h[i])) NV_LOGW("apps", "'%s': open handler not registered", a.id);
    }
    // Per-app tile icon comes from the COMPILED set (wasm_icon_for) — flash-resident, so no SD
    // read at scan time. This replaces the old icon.argb loader (wasm_tile_icon) that boot-looped
    // in 1.1.57 loading a PSRAM ARGB dsc during the boot scan; compiled icons sidestep that path.
    s_tiles[i] = { a.id, a.name, tile_icon(i), a.ram_budget, wasm_tile_build, -1, &a };
    nv_app_register(&s_tiles[i]);
}

}  // namespace

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
        s_tiles[i].ram_budget = s_installed[i].ram_budget;
        return;
    }
    wasm_tile_register(i);   // new, or reinstalled after an uninstall (which dropped the tile)
}

}  // namespace
