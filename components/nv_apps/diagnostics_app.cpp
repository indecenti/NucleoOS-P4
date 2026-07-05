// diagnostics_app — live memory/service telemetry. Owns its 1 Hz timer and frees it
// on LV_EVENT_DELETE (app close).
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_ui_kit.h"
#include "nv_icons.h"
#include "nv_i18n.h"

#include "nv_service_mgr.h"
#include "nv_memory_broker.h"
#include "nv_theme.h"
#include "nv_wasm.h"
#include "nv_crash.h"
#include "nv_fonts.h"
#include "nv_ui.h"     // nv_ui_toast

#include <cstdio>

namespace {

lv_timer_t *s_timer = nullptr;
lv_obj_t *s_label = nullptr;
lv_obj_t *s_crash_label = nullptr;

// Run the bundled WASM "app" (imports env.host_log, computes 6*7, calls the host). Blocks briefly
// on the worker pthread, then reports on-screen. Proves the WASM host-import path from the UI.
void run_wasm_cb(lv_event_t *) {
    char err[128] = "";
    nv_ui_toast(nv_wasm_run_app(err, sizeof err) ? "WASM app: host_log(42) OK"
                                                 : (err[0] ? err : "WASM app failed"));
}

void tick(lv_timer_t *) {
    if (!s_label) return;
    lv_label_set_text_fmt(s_label,
                          nv_tr(NV_STR_MEM_STATS),
                          (unsigned)(nv_mem_free_internal() / 1024),
                          (unsigned)(nv_mem_largest_internal() / 1024),
                          (unsigned)(nv_mem_free_psram() / 1024),
                          nv_service_count());
}
void page_deleted(lv_event_t *) {
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    s_label = nullptr;
    s_crash_label = nullptr;
}

void crash_label_refresh(void) {
    if (!s_crash_label) return;
    nv_crash_info_t ci;
    if (nv_crash_get(&ci)) {
        lv_label_set_text_fmt(s_crash_label, nv_tr(NV_STR_CRASH_INFO_FMT),
                              ci.task, (unsigned)ci.pc, (unsigned)(ci.size / 1024));
        lv_obj_set_style_text_color(s_crash_label, nv_theme_get()->danger, 0);
    } else {
        lv_label_set_text(s_crash_label, nv_tr(NV_STR_NO_CRASH));
        lv_obj_set_style_text_color(s_crash_label, nv_theme_get()->text_dim, 0);
    }
}

void crash_clear_cb(lv_event_t *) {
    nv_crash_erase();
    crash_label_refresh();
}

void diag_build(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(c, page_deleted, LV_EVENT_DELETE, nullptr);
    s_label = nv_kit_info(c);
    lv_obj_set_style_text_color(s_label, nv_theme_get()->success, 0);
    tick(nullptr);
    s_timer = lv_timer_create(tick, 1000, nullptr);

    lv_obj_t *wb = nv_kit_button(c, "Run WASM app", true);
    lv_obj_add_event_cb(wb, run_wasm_cb, LV_EVENT_CLICKED, nullptr);

    // ---- last-boot crash (core dump read back from the coredump flash partition) ----
    lv_obj_t *ct = lv_label_create(c);
    lv_label_set_text(ct, nv_tr(NV_STR_LAST_CRASH));
    lv_obj_set_style_text_font(ct, &nv_font_20, 0);
    lv_obj_set_style_text_color(ct, nv_theme_get()->text_strong, 0);
    lv_obj_set_style_pad_top(ct, NV_SP_3, 0);

    s_crash_label = nv_kit_info(c);
    crash_label_refresh();

    if (nv_crash_get(nullptr)) {
        lv_obj_t *cb = nv_kit_button(c, nv_tr(NV_STR_CLEAR), false);
        lv_obj_add_event_cb(cb, crash_clear_cb, LV_EVENT_CLICKED, nullptr);
    }
}

const NvApp kDiagApp = {"diag", "Diagnostics", &nv_icon_diag, 512u << 10, diag_build,
                        NV_STR_APP_DIAG, nullptr};

}  // namespace

void diagnostics_app_register(void) { nv_app_register(&kDiagApp); }
