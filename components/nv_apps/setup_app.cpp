// setup_app — the first-boot setup wizard: language, Wi-Fi, date/time/region, PIN, the statistics
// consent, and a last page with the system gestures.
//
// It is a system app, not a launcher tile: nv_setup_maybe_start() opens its descriptor directly
// right after nv_ui_start(). Being an app (and not a layer_top overlay like the lock screen) is what
// lets the shared on-screen keyboard type the Wi-Fi password — the IME is a screen child that a
// top-layer overlay would cover — and lets a language change re-run build() in place, like any
// other app. While it runs, the bottom-edge Home/Recents gesture and the shade are off
// (nv_ui_set_exit_locked) and Back goes to the previous step: the device is set up before use.
//
// Who sees what: a device that never ran NucleoOS (no "last_ver": new, or factory reset) gets the
// whole wizard; one that was already in use when the wizard appeared gets only the statistics
// question, once. "setup_done" is saved in NVS (and so in the SD backup: a restored device is
// never asked again). The statistics are OFF until the owner explicitly says yes; the two answers
// are two equal buttons, no default.
#include "nv_apps.h"
#include "nv_app.h"
#include "nv_ui.h"
#include "nv_ui_kit.h"
#include "nv_ui_scale.h"
#include "nv_ime.h"
#include "nv_i18n.h"
#include "nv_theme.h"
#include "nv_fonts.h"
#include "nv_config.h"
#include "nv_wifi.h"
#include "nv_time.h"
#include "nv_appstore.h"
#include "nv_telemetry.h"
#include "nv_log.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr char TAG[] = "setup";

enum Step { ST_LANG, ST_WIFI, ST_TIME, ST_SEC, ST_STATS, ST_DONE };
Step s_steps[6];
int  s_n = 0, s_pos = 0;
bool s_consent_only = false;

lv_obj_t   *s_content = nullptr;   // the app's content area (build())
lv_obj_t   *s_page = nullptr;      // everything drawn for the current step (rebuilt per step)
lv_obj_t   *s_body = nullptr;
lv_obj_t   *s_next_lbl = nullptr;
lv_timer_t *s_timer = nullptr;     // the step's poll (Wi-Fi list, clock, PIN state)
bool        s_render_pending = false;

// ---- deferred re-render: handlers run inside widgets that render() deletes ----------------------
void render(void);
void render_async(void *) { s_render_pending = false; if (s_content) render(); }
void request_render(void) {
    if (s_render_pending) return;
    s_render_pending = true;
    lv_async_call(render_async, nullptr);
}

void finish_async(void *) {
    nv_config_set_bool("setup_done", true);
    nv_ui_set_exit_locked(false);
    nv_ui_set_back_handler(nullptr);
    nv_ui_app_fullscreen(false);
    nv_ui_close_app();
    NV_LOGI(TAG, "setup finished");
}
void finish(void) { lv_async_call(finish_async, nullptr); }

void go(int pos) {
    if (pos < 0 || pos >= s_n) return;
    s_pos = pos;
    request_render();
}
void back_handler(void) { if (s_pos > 0) go(s_pos - 1); }
void next_cb(lv_event_t *) {
    if (s_steps[s_pos] == ST_DONE || s_pos + 1 >= s_n) finish();
    else go(s_pos + 1);
}
void back_cb(lv_event_t *) { back_handler(); }

// ---- building blocks -------------------------------------------------------------------------
lv_obj_t *label(lv_obj_t *p, const char *t, const lv_font_t *f, lv_color_t c, bool wrap = false) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    if (wrap) { lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP); lv_obj_set_width(l, lv_pct(100)); }
    return l;
}
lv_obj_t *box(lv_obj_t *p, lv_flex_flow_t flow) {
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
lv_obj_t *card(lv_obj_t *p) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *c = box(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(c, th->surface, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, NV_RAD_MD, 0);
    lv_obj_set_style_pad_all(c, NV_SP_4, 0);
    lv_obj_set_style_pad_row(c, NV_SP_2, 0);
    return c;
}

// ---- step: language --------------------------------------------------------------------------
void lang_cb(lv_event_t *e) {
    const nv_lang_t l = (nv_lang_t)(intptr_t)lv_event_get_user_data(e);
    if (l != nv_i18n_get_lang()) nv_i18n_set_lang(l);   // the whole UI (this wizard too) re-renders
}
void body_lang(lv_obj_t *b) {
    lv_obj_t *g = box(b, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(g, NV_SP_3, 0);
    lv_obj_set_style_pad_row(g, NV_SP_3, 0);
    for (int i = 0; i < NV_LANG_COUNT; i++) {
        const bool cur = i == (int)nv_i18n_get_lang();
        char t[48];
        snprintf(t, sizeof t, "%s%s", cur ? LV_SYMBOL_OK "  " : "", nv_lang_native_name((nv_lang_t)i));
        lv_obj_t *btn = nv_kit_button(g, t, cur);
        lv_obj_set_size(btn, lv_pct(48), 64);
        lv_obj_add_event_cb(btn, lang_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

// ---- step: Wi-Fi -----------------------------------------------------------------------------
constexpr int kAps = 12;
nv_wifi_ap_t s_aps[kAps];
int          s_ap_n = 0;
uint32_t     s_scan_gen = 0;
int          s_wstate = -1;
int          s_rescan = 0;
lv_obj_t    *s_wifi_list = nullptr, *s_wifi_status = nullptr;
lv_obj_t    *s_pw_modal = nullptr, *s_pw_ta = nullptr;
char         s_pw_ssid[33] = "";

void close_pw(void) {
    nv_ime_set_submit_cb(nullptr, nullptr);
    nv_ime_hide();
    if (s_pw_modal) { lv_obj_delete_async(s_pw_modal); s_pw_modal = nullptr; s_pw_ta = nullptr; }
}
void pw_connect(void) {
    if (!s_pw_ta) return;
    nv_wifi_connect(s_pw_ssid, lv_textarea_get_text(s_pw_ta));
    close_pw();
}
void pw_submit_cb(lv_obj_t *, void *) { pw_connect(); }
void pw_ok_cb(lv_event_t *) { pw_connect(); }
void pw_cancel_cb(lv_event_t *) { close_pw(); }
void pw_eye_cb(lv_event_t *e) {
    if (s_pw_ta) lv_textarea_set_password_mode(s_pw_ta, !lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}
// The password sheet: on the screen (not lv_layer_top) so the keyboard is drawn above it; a
// fixed-height card in the top part, the keyboard docks over the bottom (settings_app open_pw).
void open_pw(const char *ssid) {
    close_pw();
    snprintf(s_pw_ssid, sizeof s_pw_ssid, "%s", ssid);
    const NvTheme *th = nv_theme_get();
    s_pw_modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_pw_modal);
    lv_obj_set_size(s_pw_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_pw_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_pw_modal, LV_OPA_50, 0);
    lv_obj_add_flag(s_pw_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_pw_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *c = lv_obj_create(s_pw_modal);
    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, 460);
    lv_obj_set_height(c, 252);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(c, th->surface, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    char t[64];
    snprintf(t, sizeof t, LV_SYMBOL_WIFI "   %s", ssid);
    lv_obj_t *tl = label(c, t, &nv_font_20, th->text_strong);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(tl, lv_pct(100));
    s_pw_ta = nv_kit_textarea_ex(c, nv_tr(NV_STR_WIFI_PASSWORD), true, NV_IME_PASSWORD, NV_IME_RET_GO);
    lv_obj_set_width(s_pw_ta, lv_pct(100));
    nv_ime_set_submit_cb(pw_submit_cb, nullptr);
    lv_obj_t *show = lv_checkbox_create(c);
    lv_checkbox_set_text(show, nv_tr(NV_STR_WIFI_SHOW_PASSWORD));
    lv_obj_set_style_text_color(show, th->text_dim, 0);
    lv_obj_add_event_cb(show, pw_eye_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t *row = box(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, NV_SP_3, 0);
    lv_obj_t *cancel = nv_kit_button(row, nv_tr(NV_STR_CANCEL), false);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_add_event_cb(cancel, pw_cancel_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ok = nv_kit_button(row, nv_tr(NV_STR_WIFI_CONNECT), true);
    lv_obj_set_flex_grow(ok, 1);
    lv_obj_add_event_cb(ok, pw_ok_cb, LV_EVENT_CLICKED, nullptr);
}

void ap_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_ap_n) return;
    const nv_wifi_ap_t &a = s_aps[i];
    if (a.secured && !a.saved) open_pw(a.ssid);
    else nv_wifi_connect(a.ssid, nullptr);
}

void wifi_fill(void) {
    if (!s_wifi_list) return;
    const NvTheme *th = nv_theme_get();
    lv_obj_clean(s_wifi_list);
    char cur[33] = "", ip[20] = "";
    int8_t rssi = 0;
    const bool conn = nv_wifi_get_connected(cur, sizeof cur, ip, sizeof ip, &rssi);
    const nv_wifi_state_t st = nv_wifi_get_state();
    char m[80];
    if (conn) snprintf(m, sizeof m, nv_tr(NV_STR_SETUP_WIFI_OK_FMT), cur);
    else if (st == NV_WIFI_CONNECTING) snprintf(m, sizeof m, "%s", nv_tr(NV_STR_WIFI_CONNECTING));
    else if (st == NV_WIFI_FAILED) snprintf(m, sizeof m, "%s", nv_tr(NV_STR_WIFI_FAILED));
    else if (st == NV_WIFI_SCANNING) snprintf(m, sizeof m, "%s", nv_tr(NV_STR_WIFI_SCANNING));
    else m[0] = 0;
    if (s_wifi_status) {
        lv_label_set_text(s_wifi_status, m);
        lv_obj_set_style_text_color(s_wifi_status, conn ? th->success : st == NV_WIFI_FAILED ? th->danger : th->text_dim, 0);
    }
    if (s_next_lbl) lv_label_set_text(s_next_lbl, nv_tr(conn ? NV_STR_SETUP_NEXT : NV_STR_SETUP_SKIP));

    s_ap_n = nv_wifi_copy_aps(s_aps, kAps);
    for (int a = 1; a < s_ap_n; a++)            // strongest first
        for (int k = a; k > 0 && s_aps[k].rssi > s_aps[k - 1].rssi; k--) {
            const nv_wifi_ap_t t = s_aps[k]; s_aps[k] = s_aps[k - 1]; s_aps[k - 1] = t;
        }
    if (!s_ap_n) {
        label(s_wifi_list, nv_tr(st == NV_WIFI_SCANNING ? NV_STR_WIFI_SCANNING : NV_STR_WIFI_NO_NETWORKS),
              &nv_font_20, th->text_dim);
        return;
    }
    for (int i = 0; i < s_ap_n; i++) {
        const nv_wifi_ap_t &a = s_aps[i];
        if (!a.ssid[0]) continue;
        const bool here = conn && !strcmp(a.ssid, cur);
        lv_obj_t *row = lv_obj_create(s_wifi_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 52);
        lv_obj_set_style_bg_color(row, here ? th->surface2 : th->surface, 0);
        lv_obj_set_style_bg_color(row, th->surface3, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, NV_RAD_SM, 0);
        lv_obj_set_style_pad_hor(row, NV_SP_4, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, NV_SP_3, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, ap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        label(row, LV_SYMBOL_WIFI, &nv_font_20, a.rssi > -60 ? th->text_strong : th->text_dim);
        lv_obj_t *n = label(row, a.ssid, &nv_font_20, th->text_strong);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(n, 1);
        const char *tag = here ? nv_tr(NV_STR_WIFI_CONNECTED) : a.saved ? nv_tr(NV_STR_WIFI_SAVED)
                        : !a.secured ? nv_tr(NV_STR_WIFI_OPEN) : LV_SYMBOL_EYE_CLOSE;
        label(row, tag, &nv_font_14, here ? th->success : th->text_dim);
    }
}
void wifi_poll(lv_timer_t *) {
    const uint32_t gen = nv_wifi_scan_generation();
    const int st = (int)nv_wifi_get_state();
    if (gen != s_scan_gen || st != s_wstate) {
        s_scan_gen = gen;
        s_wstate = st;
        if (!s_pw_modal) wifi_fill();   // don't pull the list out from under the password sheet
    }
    // Keep the list fresh while not connected (a network switched on after the page opened).
    if (st != NV_WIFI_CONNECTED && st != NV_WIFI_CONNECTING && st != NV_WIFI_SCANNING && ++s_rescan >= 14) {
        s_rescan = 0;
        nv_wifi_start_scan();
    }
}
void body_wifi(lv_obj_t *b) {
    const NvTheme *th = nv_theme_get();
    if (!nv_wifi_has_radio()) {
        label(b, nv_tr(NV_STR_SETUP_WIFI_NORADIO), &nv_font_20, th->text, true);
        return;
    }
    if (!nv_wifi_is_enabled()) nv_wifi_set_enabled(true);   // also starts a scan
    else if (nv_wifi_get_state() != NV_WIFI_CONNECTED) nv_wifi_start_scan();
    s_wifi_status = label(b, "", &nv_font_20, th->text_dim);
    s_wifi_list = box(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_wifi_list, NV_SP_2, 0);
    s_scan_gen = nv_wifi_scan_generation();
    s_wstate = (int)nv_wifi_get_state();
    s_rescan = 0;
    wifi_fill();
    s_timer = lv_timer_create(wifi_poll, 700, nullptr);
}

// ---- step: date, time, region ----------------------------------------------------------------
lv_obj_t *s_clk = nullptr, *s_date = nullptr, *s_sync = nullptr;
const char *const kRegionCodes[] = { "", "*", "IT", "ES", "FR", "DE", "GB", "US", "EU" };
constexpr char kRegionOpts[] =
    "Auto\nWorldwide\nItalia (IT)\nEspaña (ES)\nFrance (FR)\nDeutschland (DE)\nUK (GB)\nUSA (US)\nEurope (EU)";

void clk_tick(lv_timer_t *) {
    const NvTheme *th = nv_theme_get();
    char b[32];
    nv_time_format(b, sizeof b, nv_time_is_24h() ? "%H:%M:%S" : "%I:%M:%S %p");
    if (s_clk) lv_label_set_text(s_clk, b);
    struct tm t;
    nv_time_now(&t);
    char d[48];
    snprintf(d, sizeof d, "%d %s %d", t.tm_mday, nv_i18n_month_short(t.tm_mon), t.tm_year + 1900);
    if (s_date) lv_label_set_text(s_date, d);
    const bool ok = nv_time_is_synced();
    if (s_sync) {
        lv_label_set_text(s_sync, nv_tr(ok ? NV_STR_SETUP_SYNCED : NV_STR_SETUP_NOT_SYNCED));
        lv_obj_set_style_text_color(s_sync, ok ? th->success : th->text_dim, 0);
    }
}
void tz_cb(lv_event_t *e) { nv_time_set_tz((int)lv_dropdown_get_selected(lv_event_get_target_obj(e))); clk_tick(nullptr); }
void h24_cb(lv_event_t *e) { nv_time_set_24h(lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED)); clk_tick(nullptr); }
void region_cb(lv_event_t *e) {
    const uint32_t i = lv_dropdown_get_selected(lv_event_get_target_obj(e));
    if (i < sizeof kRegionCodes / sizeof kRegionCodes[0]) nv_appstore_set_region(kRegionCodes[i]);
}
lv_obj_t *field_row(lv_obj_t *p, const char *name) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *r = box(p, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(r, name, &nv_font_20, th->text);
    return r;
}
void body_time(lv_obj_t *b) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *c = card(b);
    s_clk = label(c, "", &nv_font_28, th->text_strong);
    s_date = label(c, "", &nv_font_20, th->text);
    s_sync = label(c, "", &nv_font_14, th->text_dim);

    static char opts[1024];
    size_t o = 0;
    opts[0] = 0;
    for (int i = 0; i < nv_time_tz_count() && o < sizeof opts - 64; i++)
        o += (size_t)snprintf(opts + o, sizeof opts - o, "%s%s", i ? "\n" : "", nv_time_tz_name(i));
    lv_obj_t *r = field_row(b, nv_tr(NV_STR_TIMEZONE));
    lv_obj_t *dd = lv_dropdown_create(r);
    lv_obj_set_width(dd, 360);
    lv_dropdown_set_options(dd, opts);
    lv_dropdown_set_selected(dd, (uint32_t)nv_time_get_tz());
    lv_obj_add_event_cb(dd, tz_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    nv_kit_switch_row(b, nv_tr(NV_STR_TIME_24H), nv_time_is_24h(), h24_cb);

    r = field_row(b, nv_tr(NV_STR_STORE_REGION));
    lv_obj_t *rd = lv_dropdown_create(r);
    lv_obj_set_width(rd, 360);
    lv_dropdown_set_options(rd, kRegionOpts);
    char cur[16];
    nv_appstore_get_region(cur, sizeof cur);
    for (uint32_t i = 0; i < sizeof kRegionCodes / sizeof kRegionCodes[0]; i++)
        if (!strcmp(cur, kRegionCodes[i])) { lv_dropdown_set_selected(rd, i); break; }
    lv_obj_add_event_cb(rd, region_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    clk_tick(nullptr);
    s_timer = lv_timer_create(clk_tick, 1000, nullptr);
}

// ---- step: PIN -------------------------------------------------------------------------------
bool s_had_pin = false;
bool has_pin(void) {
    char pin[8];
    nv_config_get_str("lockpin", "", pin, sizeof pin);
    return pin[0] != 0;
}
void pin_cb(lv_event_t *) { nv_ui_set_pin_flow(); }   // the system PIN pad (set + confirm)
void boot_lock_cb(lv_event_t *e) {
    nv_config_set_bool("lock_boot", lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}
void pin_poll(lv_timer_t *) { if (has_pin() != s_had_pin) request_render(); }
void body_sec(lv_obj_t *b) {
    const NvTheme *th = nv_theme_get();
    s_had_pin = has_pin();
    lv_obj_t *r = box(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, NV_SP_3, 0);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *btn = nv_kit_button(r, nv_tr(NV_STR_SET_PIN), !s_had_pin);
    lv_obj_add_event_cb(btn, pin_cb, LV_EVENT_CLICKED, nullptr);
    if (s_had_pin) label(r, LV_SYMBOL_OK "  " , &nv_font_20, th->success);
    if (s_had_pin) label(r, nv_tr(NV_STR_SETUP_PIN_OK), &nv_font_20, th->success);
    lv_obj_t *sw = nv_kit_switch_row(b, nv_tr(NV_STR_LOCK_ON_BOOT), s_had_pin && nv_config_get_bool("lock_boot", false),
                                     boot_lock_cb);
    if (!s_had_pin && sw) lv_obj_add_state(sw, LV_STATE_DISABLED);
    if (s_next_lbl) lv_label_set_text(s_next_lbl, nv_tr(s_had_pin ? NV_STR_SETUP_NEXT : NV_STR_SETUP_SKIP));
    s_timer = lv_timer_create(pin_poll, 500, nullptr);
}

// ---- step: statistics consent ----------------------------------------------------------------
void consent_cb(lv_event_t *e) {
    const bool yes = lv_event_get_user_data(e) != nullptr;
    nv_telemetry_set_consent(yes);
    if (s_consent_only || s_pos + 1 >= s_n) finish();
    else go(s_pos + 1);
}
void body_stats(lv_obj_t *b) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *row = box(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, NV_SP_5, 0);
    lv_obj_t *txt = box(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(txt, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(txt, 1);
    lv_obj_set_style_pad_row(txt, NV_SP_3, 0);
    label(txt, nv_tr(NV_STR_SETUP_STATS_SENT), &nv_font_20, th->text, true);
    label(txt, nv_tr(NV_STR_SETUP_STATS_NEVER), &nv_font_20, th->text, true);
    lv_obj_t *q = box(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(q, 190);
    lv_obj_set_style_pad_row(q, NV_SP_2, 0);
    lv_obj_t *qr = lv_qrcode_create(q);
    lv_qrcode_set_size(qr, 150);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_qrcode_update(qr, NV_TELEMETRY_PRIVACY_URL, (uint32_t)strlen(NV_TELEMETRY_PRIVACY_URL));
    label(q, nv_tr(NV_STR_SETUP_STATS_QR), &nv_font_14, th->text_dim, true);
}

// ---- step: done ------------------------------------------------------------------------------
void body_done(lv_obj_t *b) {
    const NvTheme *th = nv_theme_get();
    static const struct { const char *sym; nv_str_id_t s; } kTips[] = {
        { LV_SYMBOL_UP, NV_STR_SETUP_TIP_HOME }, { LV_SYMBOL_DOWN, NV_STR_SETUP_TIP_SHADE },
        { LV_SYMBOL_RIGHT, NV_STR_SETUP_TIP_BACK }, { LV_SYMBOL_PLUS, NV_STR_SETUP_TIP_STORE } };
    for (const auto &t : kTips) {
        lv_obj_t *r = box(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(r, NV_SP_4, 0);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *ic = label(r, t.sym, &nv_font_20, th->primary);
        lv_obj_set_width(ic, 28);
        lv_obj_t *l = label(r, nv_tr(t.s), &nv_font_20, th->text);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_flex_grow(l, 1);
    }
}

// ---- page ------------------------------------------------------------------------------------
void page_deleted(lv_event_t *) {
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    close_pw();
    s_page = s_body = s_next_lbl = nullptr;
    s_wifi_list = s_wifi_status = nullptr;
    s_clk = s_date = s_sync = nullptr;
}

void render(void) {
    if (!s_content) return;
    if (s_page) lv_obj_delete(s_page);           // page_deleted() stops the step's timer
    const NvTheme *th = nv_theme_get();
    const Step st = s_steps[s_pos];

    s_page = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_page, th->bg, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_page, 56, 0);
    lv_obj_set_style_pad_ver(s_page, 28, 0);
    lv_obj_set_style_pad_row(s_page, NV_SP_3, 0);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_page, page_deleted, LV_EVENT_DELETE, nullptr);

    // Header: product name, step count and a segmented progress bar.
    lv_obj_t *hd = box(s_page, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hd, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(hd, "NucleoOS", &nv_font_14, th->text_dim);
    if (s_n > 1) {
        char t[40];
        snprintf(t, sizeof t, nv_tr(NV_STR_SETUP_STEP_FMT), s_pos + 1, s_n);
        label(hd, t, &nv_font_14, th->text_dim);
        lv_obj_t *bar = box(s_page, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(bar, NV_SP_2, 0);
        for (int i = 0; i < s_n; i++) {
            lv_obj_t *seg = lv_obj_create(bar);
            lv_obj_remove_style_all(seg);
            lv_obj_set_height(seg, 4);
            lv_obj_set_flex_grow(seg, 1);
            lv_obj_set_style_radius(seg, 2, 0);
            lv_obj_set_style_bg_color(seg, i <= s_pos ? th->primary : th->surface3, 0);
            lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        }
    }

    static const struct { nv_str_id_t title, sub; } kText[] = {
        { NV_STR_SETUP_WELCOME, NV_STR_SETUP_LANG_SUB },  { NV_STR_SETUP_WIFI_T, NV_STR_SETUP_WIFI_SUB },
        { NV_STR_SETUP_TIME_T, NV_STR_SETUP_TIME_SUB },   { NV_STR_SETUP_SEC_T, NV_STR_SETUP_SEC_SUB },
        { NV_STR_SETUP_STATS_T, NV_STR_SETUP_STATS_SUB }, { NV_STR_SETUP_DONE_T, NV_STR_SETUP_DONE_SUB } };
    lv_obj_t *tt = label(s_page, nv_tr(kText[st].title), &nv_font_28, th->text_strong, true);
    lv_obj_set_style_pad_top(tt, NV_SP_3, 0);
    label(s_page, nv_tr(kText[st].sub), &nv_font_20, th->text_dim, true);

    s_body = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_width(s_body, lv_pct(100));
    lv_obj_set_flex_grow(s_body, 1);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_body, NV_SP_3, 0);
    lv_obj_set_style_pad_top(s_body, NV_SP_2, 0);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_body, LV_SCROLLBAR_MODE_AUTO);

    // Footer: Back on the left; Next (or the two equal consent answers) on the right.
    lv_obj_t *ft = box(s_page, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ft, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ft, NV_SP_3, 0);
    if (s_pos > 0) {
        char t[40];
        snprintf(t, sizeof t, LV_SYMBOL_LEFT "  %s", nv_tr(NV_STR_BACK));
        lv_obj_t *bk = nv_kit_button(ft, t, false);
        lv_obj_set_height(bk, 56);
        lv_obj_add_event_cb(bk, back_cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *sp = lv_obj_create(ft);
    lv_obj_remove_style_all(sp);
    lv_obj_set_height(sp, 1);
    lv_obj_set_flex_grow(sp, 1);
    s_next_lbl = nullptr;
    if (st == ST_STATS) {
        // No default and no "recommended" styling: both answers look and weigh the same.
        lv_obj_t *no = nv_kit_button(ft, nv_tr(NV_STR_SETUP_STATS_NO), false);
        lv_obj_set_size(no, 300, 56);
        lv_obj_add_event_cb(no, consent_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *yes = nv_kit_button(ft, nv_tr(NV_STR_SETUP_STATS_YES), false);
        lv_obj_set_size(yes, 300, 56);
        lv_obj_add_event_cb(yes, consent_cb, LV_EVENT_CLICKED, (void *)1);
    } else {
        char t[40];
        snprintf(t, sizeof t, "%s  " LV_SYMBOL_RIGHT, nv_tr(st == ST_DONE ? NV_STR_SETUP_START : NV_STR_SETUP_NEXT));
        lv_obj_t *nx = nv_kit_button(ft, t, true);
        lv_obj_set_size(nx, 220, 56);
        lv_obj_add_event_cb(nx, next_cb, LV_EVENT_CLICKED, nullptr);
        s_next_lbl = lv_obj_get_child(nx, 0);
    }

    switch (st) {
        case ST_LANG:  body_lang(s_body); break;
        case ST_WIFI:  body_wifi(s_body); break;
        case ST_TIME:  body_time(s_body); break;
        case ST_SEC:   body_sec(s_body); break;
        case ST_STATS: body_stats(s_body); break;
        case ST_DONE:  body_done(s_body); break;
    }
}

// build(): at open and again after a language/theme change (the page is rebuilt in place).
void setup_build(lv_obj_t *content) {
    s_content = content;
    s_page = nullptr;
    nv_ui_app_fullscreen(true);
    nv_ui_set_exit_locked(true);
    nv_ui_set_back_handler(back_handler);
    // Closed by any path (finish, or a remote /api/ui/home while testing): never leave the
    // Home gesture and the shade switched off.
    lv_obj_add_event_cb(content, [](lv_event_t *) {
        s_content = nullptr;
        s_page = nullptr;
        nv_ui_set_exit_locked(false);
    }, LV_EVENT_DELETE, nullptr);
    render();
}

const NvApp kSetupApp = {
    "setup", "Setup", nullptr, 64 * 1024, setup_build, -1, nullptr,
};

void full_steps(void) {
    s_consent_only = false;
    s_n = 0;
    s_pos = 0;
    s_steps[s_n++] = ST_LANG;
    s_steps[s_n++] = ST_WIFI;
    s_steps[s_n++] = ST_TIME;
    s_steps[s_n++] = ST_SEC;
    s_steps[s_n++] = ST_STATS;
    s_steps[s_n++] = ST_DONE;
}
void run_again_async(void *) { full_steps(); nv_ui_open_app(&kSetupApp); }

}  // namespace

extern "C" void nv_setup_run_again(void) {
    // Deferred: the caller is a button inside the app that opening the wizard closes.
    lv_async_call(run_again_async, nullptr);
}

extern "C" void nv_setup_maybe_start(bool first_boot) {
    const bool done = nv_config_get_bool("setup_done", false);
    const bool asked = nv_telemetry_consent() != NV_TELEMETRY_UNASKED;
    s_n = 0;
    s_pos = 0;
    if (!done && first_boot) {
        full_steps();
    } else {
        // Already in use before the wizard existed (or restored from a backup): it counts as set
        // up; only a statistics question that was never answered is still asked.
        if (!done) nv_config_set_bool("setup_done", true);
        if (asked) return;
        s_consent_only = true;
        s_steps[s_n++] = ST_STATS;
    }
    NV_LOGI(TAG, "opening the setup wizard (%s)", s_consent_only ? "statistics question" : "first boot");
    nv_ui_open_app(&kSetupApp);
}
