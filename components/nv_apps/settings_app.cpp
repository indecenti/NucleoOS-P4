// settings_app — tablet-style split-view Settings for the 1024x600 landscape panel:
// a grouped category rail (icon badge + title + live subtitle) on the left and the active
// category's page on the right. Selecting a category defers the rail/detail rebuild via
// lv_async_call (the clicked row is deleted by the rebuild, so it must unwind first).
// Live pages (Network / Update / Storage / Date & time / Memory / About-uptime) own an LVGL
// timer each and free it on LV_EVENT_DELETE (category switch, theme/lang rebuild, app close).
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_ui_kit.h"
#include "nv_ui_host.h"
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_theme.h"
#include "nv_fonts.h"

#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nv_log.h"
#include "nv_config.h"
#include "nv_time.h"
#include "nv_service_mgr.h"
#include "nv_memory_broker.h"
#include "nv_hal.h"
#include "nv_audio.h"
#include "nv_wifi.h"
#include "nv_eth.h"
#include "nv_sd.h"
#include "nv_ota.h"
#include "nv_appstore.h"   // remote WASM app store: editable base URL lives on this page
#include "nv_backup.h"
#include "nv_ui.h"        // nv_ui_toast
#include "nv_notify.h"    // notifications page (count / clear)
#include "esp_system.h"   // esp_restart (restore / factory reset / About)
#include "driver/i2c_master.h"  // I2C bus scan (Sensors page)

#include <cstdint>  // intptr_t (enum <-> user_data packing)
#include <cstring>  // strcmp

namespace {

// -------------------------------------------------------------- shared page helpers

// Muted section header inside a settings scroll column — small, dim, letter-spaced, with room
// above so it reads as a group separator (professional settings-list rhythm).
void section_label(lv_obj_t *col, const char *text) {
    lv_obj_t *l = lv_label_create(col);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &nv_font_14, 0);
    lv_obj_set_style_text_color(l, nv_theme_get()->text_dim, 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_set_style_pad_top(l, NV_SP_3, 0);
    lv_obj_set_style_pad_left(l, NV_SP_1, 0);
    lv_obj_set_style_pad_bottom(l, 2, 0);
}

// LVGL 9 gotcha: every lv_obj container is born CLICKABLE (lv_obj.c constructor) — only labels
// clear it. A plain container INSIDE a clickable row therefore swallows taps that should hit the
// row. Call this on every decorative child of a tappable surface.
void no_click(lv_obj_t *o) { lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE); }

// A full-width horizontal container for side-by-side preview cards / chips.
lv_obj_t *pick_row(lv_obj_t *col, int gap) {
    lv_obj_t *r = lv_obj_create(col);
    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, lv_pct(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(r, gap, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

// Live "NN%" readout appended to a slider's row — updates on every drag tick (label text
// only: no layout, no NVS). Fixed min-width so the row doesn't jitter between 5% and 100%.
void pct_badge(lv_obj_t *slider) {
    lv_obj_t *v = lv_label_create(lv_obj_get_parent(slider));
    lv_obj_set_style_text_color(v, nv_theme_get()->text_dim, 0);
    lv_obj_set_style_min_width(v, 56, 0);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text_fmt(v, "%d%%", (int)lv_slider_get_value(slider));
    lv_obj_add_event_cb(slider, [](lv_event_t *e) {
        lv_label_set_text_fmt(static_cast<lv_obj_t *>(lv_event_get_user_data(e)), "%d%%",
                              (int)lv_slider_get_value(lv_event_get_target_obj(e)));
    }, LV_EVENT_VALUE_CHANGED, v);
}

// "[label ............ value]" info row: a standard kit row + right-aligned dim value.
// Returns the value label so live pages can update it in place.
lv_obj_t *kv_row(lv_obj_t *col, const char *label, const char *value) {
    lv_obj_t *row = nv_kit_row(col, label);
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_color(v, nv_theme_get()->text_dim, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(v, lv_pct(60), 0);
    return v;
}

// Thin usage bar (0..100%); indicator turns danger past 90%. Returns the bar.
lv_obj_t *usage_bar(lv_obj_t *col, int pct) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *bar = lv_bar_create(col);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, 10);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, th->surface3, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, pct > 90 ? th->danger : th->accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    return bar;
}

// A padded surface card column (radius MD) — hero blocks and grouped stats.
lv_obj_t *surface_card(lv_obj_t *col) {
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
    return card;
}

// -------------------------------------------------------------- live-apply callbacks
// Sliders: hardware applies live on every drag tick, but NVS persists only on RELEASED —
// a drag fires dozens of VALUE_CHANGED and committing each one would wear flash and stutter
// the frame loop (same split the shade sliders use).
void brightness_cb(lv_event_t *e) {
    nv_hal_backlight_set(lv_slider_get_value(lv_event_get_target_obj(e)));   // live (LEDC)
}
void brightness_done_cb(lv_event_t *e) {
    nv_config_set_int("brightness",
                      lv_slider_get_value(lv_event_get_target_obj(e)));   // persist once
}
void volume_cb(lv_event_t *e) {
    nv_audio_set_volume(lv_slider_get_value(lv_event_get_target_obj(e)));   // live (codec)
}
void volume_done_cb(lv_event_t *e) {
    nv_config_set_int("volume", lv_slider_get_value(lv_event_get_target_obj(e)));
    nv_audio_tone(1000, 60);   // audible sample of the level just set (no-op when muted)
}
void mode_pick_cb(lv_event_t *e) {   // theme-mode preview card tapped
    nv_theme_set_mode((nv_theme_mode_t)(intptr_t)lv_event_get_user_data(e));
}
void mute_cb(lv_event_t *e) {
    const bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    nv_audio_set_mute(on);                 // live apply
    nv_config_set_bool("mute", on);
}
void keyclick_cb(lv_event_t *e) {
    const bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    nv_audio_set_key_click(on);            // live apply (IME tick gate)
    nv_config_set_bool("keyclick", on);
}
void chime_cb(lv_event_t *e) {
    nv_config_set_bool("chime", lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}
void testsound_cb(lv_event_t *) { nv_audio_chime(); }
void dumplog_cb(lv_event_t *) { nv_log_dump(); }

// -------------------------------------------------------------- Appearance pickers
// Both fire a theme setter, which publishes NV_EV_THEME_CHANGED -> SystemUI rebuilds the whole
// UI live (this Settings page included, re-reading the new marks). Deferred rebuild is safe
// from these click handlers (the async rebuild frees this page after the handler unwinds).
void accent_row_cb(lv_event_t *e) {
    nv_theme_set_accent((nv_accent_t)(intptr_t)lv_event_get_user_data(e));
}
void font_row_cb(lv_event_t *e) {
    nv_theme_set_font_scale((nv_font_scale_t)(intptr_t)lv_event_get_user_data(e));
}

// A preset accent's hue in the given mode — mirrors nv_theme.c's accent table so a chip/dot
// previews exactly what the engine would apply.
lv_color_t accent_hue(int a, bool light) {
    switch (a) {
        case NV_ACCENT_GREEN:  return light ? lv_color_hex(0x1A7F37) : lv_color_hex(0x2EA043);
        case NV_ACCENT_PURPLE: return light ? lv_color_hex(0x8250DF) : lv_color_hex(0x8957E5);
        case NV_ACCENT_ORANGE: return light ? lv_color_hex(0xBC4C00) : lv_color_hex(0xE36209);
        default:               return light ? lv_color_hex(0x0969DA) : lv_color_hex(0x1F6FEB);  // Blue
    }
}

// Localized accent color name for the rail subtitle + (future) labels.
nv_str_id_t accent_name_id(nv_accent_t a) {
    switch (a) {
        case NV_ACCENT_GREEN:  return NV_STR_ACC_GREEN;
        case NV_ACCENT_PURPLE: return NV_STR_ACC_PURPLE;
        case NV_ACCENT_ORANGE: return NV_STR_ACC_ORANGE;
        default:               return NV_STR_ACC_BLUE;
    }
}

// A small rounded bar used inside the theme mockups (fake text line).
void mock_bar(lv_obj_t *parent, int w, lv_color_t col) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, 6);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_bg_color(b, col, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
}

// Light/Dark preview card: a mini mockup (surface + accent dot + text lines) rendered in that
// mode's own palette, with a name + checkmark footer. Tapping applies the mode.
void mode_card(lv_obj_t *row, bool dark, bool selected) {
    const NvTheme *th = nv_theme_get();
    const lv_color_t bg   = dark ? lv_color_hex(0x0E1116) : lv_color_hex(0xF6F8FA);
    const lv_color_t surf = dark ? lv_color_hex(0x1B222B) : lv_color_hex(0xFFFFFF);
    const lv_color_t txt  = dark ? lv_color_hex(0xE6EDF3) : lv_color_hex(0x1F2328);
    const lv_color_t dim  = dark ? lv_color_hex(0x6E7781) : lv_color_hex(0xAFB8C1);
    const lv_color_t acc  = accent_hue((int)nv_theme_get_accent(), !dark);

    lv_obj_t *card = lv_obj_create(row);
    lv_obj_remove_style_all(card);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(card, 132);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, bg, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? th->accent : th->surface3, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, mode_pick_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)(dark ? NV_THEME_DARK : NV_THEME_LIGHT));

    lv_obj_t *m = lv_obj_create(card);   // mock surface (decoration: must not eat the card tap)
    lv_obj_remove_style_all(m);
    no_click(m);
    lv_obj_set_width(m, lv_pct(100));
    lv_obj_set_flex_grow(m, 1);
    lv_obj_set_style_radius(m, 8, 0);
    lv_obj_set_style_bg_color(m, surf, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(m, 10, 0);
    lv_obj_set_style_pad_column(m, 10, 0);
    lv_obj_set_flex_flow(m, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(m, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *dot = lv_obj_create(m);
    lv_obj_remove_style_all(dot);
    no_click(dot);
    lv_obj_set_size(dot, 18, 18);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, acc, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_t *bars = lv_obj_create(m);
    lv_obj_remove_style_all(bars);
    no_click(bars);
    lv_obj_set_flex_grow(bars, 1);
    lv_obj_set_height(bars, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bars, 7, 0);
    lv_obj_clear_flag(bars, LV_OBJ_FLAG_SCROLLABLE);
    mock_bar(bars, 72, txt);
    mock_bar(bars, 46, dim);

    lv_obj_t *foot = lv_obj_create(card);   // name + check
    lv_obj_remove_style_all(foot);
    no_click(foot);
    lv_obj_set_width(foot, lv_pct(100));
    lv_obj_set_height(foot, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *nm = lv_label_create(foot);
    lv_label_set_text(nm, nv_tr(dark ? NV_STR_DARK : NV_STR_LIGHT));
    lv_obj_set_style_text_color(nm, txt, 0);
    if (selected) {
        lv_obj_t *ok = lv_label_create(foot);
        lv_label_set_text(ok, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(ok, th->accent, 0);
    }
}

// Circular accent color chip; selected gets an outline ring + check. Tapping applies it.
void accent_chip(lv_obj_t *row, int a, bool selected) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *chip = lv_obj_create(row);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, 50, 50);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(chip, accent_hue(a, nv_theme_get_mode() == NV_THEME_LIGHT), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(chip, accent_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)a);
    if (selected) {
        lv_obj_set_style_outline_width(chip, 3, 0);
        lv_obj_set_style_outline_color(chip, th->accent, 0);
        lv_obj_set_style_outline_pad(chip, 3, 0);
        lv_obj_t *ok = lv_label_create(chip);
        lv_label_set_text(ok, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(ok, lv_color_white(), 0);
        lv_obj_center(ok);
    }
}

// Font-size preview card: a big "Aa" sample at the preset's scale + its name. Tap applies it.
void font_card(lv_obj_t *row, nv_font_scale_t scale, const lv_font_t *font, bool selected) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *card = lv_obj_create(row);
    lv_obj_remove_style_all(card);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(card, 96);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, th->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, selected ? 3 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? th->accent : th->surface3, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(card, font_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)scale);
    lv_obj_t *aa = lv_label_create(card);
    lv_label_set_text(aa, "Aa");
    lv_obj_set_style_text_font(aa, font, 0);
    lv_obj_set_style_text_color(aa, th->text_strong, 0);
    lv_obj_t *nm = lv_label_create(card);
    lv_label_set_text(nm, nv_tr(scale == NV_FONT_NORMAL ? NV_STR_FONT_NORMAL : NV_STR_FONT_LARGE));
    lv_obj_set_style_text_color(nm, th->text_dim, 0);
}

// -------------------------------------------------------------- Display page (+ screen sleep)
// Screen-sleep choices (seconds; 0 = never). Pills restyle in place — no rebuild, no deletion.
constexpr int kSleepOpts[] = {0, 15, 30, 60, 300};
constexpr int kSleepN = sizeof(kSleepOpts) / sizeof(kSleepOpts[0]);

void sleep_pill_label(char *buf, size_t n, int secs) {
    if (secs == 0)       lv_snprintf(buf, n, "%s", nv_tr(NV_STR_SLEEP_NEVER));
    else if (secs < 60)  lv_snprintf(buf, n, "%d s", secs);
    else                 lv_snprintf(buf, n, "%d min", secs / 60);
}

void restyle_sleep_pills(lv_obj_t *row, int sel_idx) {
    const NvTheme *th = nv_theme_get();
    for (int i = 0; i < (int)lv_obj_get_child_count(row) && i < kSleepN; i++) {
        lv_obj_t *pill = lv_obj_get_child(row, i);
        const bool sel = (i == sel_idx);
        lv_obj_set_style_bg_color(pill, sel ? th->primary : th->surface3, 0);
        lv_obj_t *lbl = lv_obj_get_child(pill, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, sel ? th->on_primary : th->text_strong, 0);
    }
}

void sleep_pick_cb(lv_event_t *e) {
    lv_obj_t *pill = lv_event_get_target_obj(e);
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    nv_config_set_int("scr_timeout", kSleepOpts[idx]);   // SystemUI re-caches via the event
    restyle_sleep_pills(lv_obj_get_parent(pill), idx);   // in-place restyle: nothing is deleted
}

void lock_en_cb(lv_event_t *e) {
    nv_config_set_bool("lock_en", lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}
void setpin_cb(lv_event_t *) { nv_ui_set_pin_flow(); }   // opens the numeric keypad (SystemUI)
void lockboot_cb(lv_event_t *e) {
    nv_config_set_bool("lock_boot", lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}
void rmpin_cb(lv_event_t *e) {
    nv_config_set_str("lockpin", "");        // clear the PIN (idle lock, if on, degrades to swipe)
    lv_obj_add_flag(lv_event_get_target_obj(e), LV_OBJ_FLAG_HIDDEN);   // no stale button (no rebuild)
    nv_ui_toast(nv_tr(NV_STR_REMOVE_PIN));
}

void cat_display(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    lv_obj_t *bsl = nv_kit_slider_row(c, nv_tr(NV_STR_BRIGHTNESS),
                                      nv_config_get_int("brightness", 90), 5, 100, brightness_cb);
    lv_obj_add_event_cb(bsl, brightness_done_cb, LV_EVENT_RELEASED, nullptr);
    pct_badge(bsl);

    // Screen sleep: pill choices, current persisted value marked.
    section_label(c, nv_tr(NV_STR_SCREEN_SLEEP));
    const int cur_to = nv_config_get_int("scr_timeout", 0);
    lv_obj_t *pills = pick_row(c, NV_SP_2);
    for (int i = 0; i < kSleepN; i++) {
        lv_obj_t *pill = lv_obj_create(pills);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, LV_SIZE_CONTENT, NV_TOUCH_MIN);
        lv_obj_set_style_pad_hor(pill, NV_SP_4, 0);
        lv_obj_set_style_radius(pill, NV_TOUCH_MIN / 2, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_80, LV_STATE_PRESSED);   // press dip, layer-free
        lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(pill, sleep_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char lb[16];
        sleep_pill_label(lb, sizeof lb, kSleepOpts[i]);
        lv_obj_t *l = lv_label_create(pill);
        lv_label_set_text(l, lb);
        lv_obj_center(l);
    }
    int sel = 0;
    for (int i = 0; i < kSleepN; i++)
        if (kSleepOpts[i] == cur_to) { sel = i; break; }
    restyle_sleep_pills(pills, sel);

    // (Screen lock + PIN moved to the dedicated Security category.)

    // Theme: Light / Dark preview cards (each rendered in its own palette).
    section_label(c, nv_tr(NV_STR_THEME));
    const bool dark = nv_theme_get_mode() == NV_THEME_DARK;
    lv_obj_t *modes = pick_row(c, 12);
    mode_card(modes, false, !dark);
    mode_card(modes, true, dark);

    // Accent color: circular swatches, selected ringed.
    section_label(c, nv_tr(NV_STR_ACCENT_COLOR));
    lv_obj_t *accents = pick_row(c, 16);
    lv_obj_set_flex_align(accents, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(accents, 4, 0);
    const nv_accent_t cur_acc = nv_theme_get_accent();
    for (int a = 0; a < NV_ACCENT_COUNT; a++) accent_chip(accents, a, a == cur_acc);

    // Font size: Normal / Large preview cards with an "Aa" sample.
    section_label(c, nv_tr(NV_STR_FONT_SIZE));
    const nv_font_scale_t cur_f = nv_theme_get_font_scale();
    lv_obj_t *fonts = pick_row(c, 12);
    font_card(fonts, NV_FONT_NORMAL, &lv_font_montserrat_20, cur_f == NV_FONT_NORMAL);
    font_card(fonts, NV_FONT_LARGE, &lv_font_montserrat_28, cur_f == NV_FONT_LARGE);
}

// -------------------------------------------------------------- Sound page
// mic section: live level meter + record/playback test (on-board MIC1 via ES7210)
lv_obj_t   *s_mic_bar    = nullptr;
lv_obj_t   *s_mic_status = nullptr;
lv_timer_t *s_mic_timer  = nullptr;

void mic_tick(lv_timer_t *) {
    if (!s_mic_bar) return;
    lv_bar_set_value(s_mic_bar, nv_audio_mic_level(), LV_ANIM_OFF);
    if (s_mic_status) {
        switch (nv_audio_mic_state()) {
            case NV_MIC_REC:  lv_label_set_text(s_mic_status, nv_tr(NV_STR_RECORDING)); break;
            case NV_MIC_PLAY: lv_label_set_text(s_mic_status, nv_tr(NV_STR_PLAYING));   break;
            default:          lv_label_set_text(s_mic_status, "");                      break;
        }
    }
    // a finished test leaves the worker idle; resume live metering for the visible bar
    if (nv_audio_mic_state() == NV_MIC_IDLE) nv_audio_mic_meter_start();
}

void mic_test_cb(lv_event_t *) { nv_audio_mic_test_start(3000); }

void sound_page_deleted(lv_event_t *) {
    if (s_mic_timer) { lv_timer_delete(s_mic_timer); s_mic_timer = nullptr; }
    nv_audio_mic_meter_stop();
    s_mic_bar = nullptr;
    s_mic_status = nullptr;
}

void cat_sound(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(c, sound_page_deleted, LV_EVENT_DELETE, nullptr);
    lv_obj_t *vsl = nv_kit_slider_row(c, nv_tr(NV_STR_VOLUME),
                                      nv_config_get_int("volume", 60), 0, 100, volume_cb);
    lv_obj_add_event_cb(vsl, volume_done_cb, LV_EVENT_RELEASED, nullptr);
    pct_badge(vsl);
    nv_kit_switch_row(c, nv_tr(NV_STR_MUTE), nv_config_get_bool("mute", false), mute_cb);
    nv_kit_switch_row(c, nv_tr(NV_STR_KEY_CLICK), nv_config_get_bool("keyclick", true),
                      keyclick_cb);
    nv_kit_switch_row(c, nv_tr(NV_STR_STARTUP_CHIME), nv_config_get_bool("chime", true), chime_cb);

    lv_obj_t *test = nv_kit_button(c, nv_tr(NV_STR_TEST_SOUND), false);
    lv_obj_add_event_cb(test, testsound_cb, LV_EVENT_CLICKED, nullptr);

    const NvTheme *th = nv_theme_get();
    section_label(c, nv_tr(NV_STR_MICROPHONE));

    if (!nv_audio_mic_ready()) {
        lv_obj_t *info = nv_kit_info(c);
        lv_label_set_text(info, nv_tr(NV_STR_MIC_MISSING));
        lv_obj_set_style_text_color(info, th->text_dim, 0);
        return;
    }

    s_mic_bar = lv_bar_create(c);
    lv_obj_set_size(s_mic_bar, lv_pct(100), 12);
    lv_bar_set_range(s_mic_bar, 0, 100);
    lv_obj_set_style_bg_color(s_mic_bar, th->surface3, 0);
    lv_obj_set_style_bg_opa(s_mic_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_mic_bar, NV_RAD_MD, 0);
    lv_obj_set_style_bg_color(s_mic_bar, th->accent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_mic_bar, NV_RAD_MD, LV_PART_INDICATOR);

    s_mic_status = nv_kit_info(c);
    lv_label_set_text(s_mic_status, "");
    lv_obj_set_style_text_color(s_mic_status, th->text_dim, 0);

    lv_obj_t *mtest = nv_kit_button(c, nv_tr(NV_STR_MIC_TEST), false);
    lv_obj_add_event_cb(mtest, mic_test_cb, LV_EVENT_CLICKED, nullptr);

    nv_audio_mic_meter_start();
    s_mic_timer = lv_timer_create(mic_tick, 120, nullptr);
}

// -------------------------------------------------------------- Date & time page (live)
lv_obj_t  *s_clk_time  = nullptr;
lv_obj_t  *s_clk_date  = nullptr;
lv_obj_t  *s_clk_sync  = nullptr;
lv_timer_t *s_clk_timer = nullptr;
bool       s_clk_synced = false;

void clk_tick(lv_timer_t *) {
    if (!s_clk_time) return;
    char b[48];
    nv_time_format(b, sizeof b, nv_time_is_24h() ? "%H:%M:%S" : "%I:%M:%S %p");
    lv_label_set_text(s_clk_time, b);

    struct tm t;
    nv_time_now(&t);
    lv_label_set_text_fmt(s_clk_date, "%s %d %s %d", nv_i18n_wday_short(t.tm_wday), t.tm_mday,
                          nv_i18n_month_short(t.tm_mon), t.tm_year + 1900);

    const bool synced = nv_time_is_synced();
    if (synced != s_clk_synced || lv_label_get_text(s_clk_sync)[0] == '\0') {
        s_clk_synced = synced;
        const NvTheme *th = nv_theme_get();
        lv_label_set_text_fmt(s_clk_sync, "%s  %s",
                              synced ? LV_SYMBOL_OK : LV_SYMBOL_REFRESH,
                              nv_tr(synced ? NV_STR_TIME_SYNCED : NV_STR_TIME_WAIT_SYNC));
        lv_obj_set_style_text_color(s_clk_sync, synced ? th->success : th->text_dim, 0);
    }
}
void clk_page_deleted(lv_event_t *) {
    if (s_clk_timer) { lv_timer_delete(s_clk_timer); s_clk_timer = nullptr; }
    s_clk_time = s_clk_date = s_clk_sync = nullptr;
}

void h24_cb(lv_event_t *e) {
    nv_time_set_24h(lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
    clk_tick(nullptr);   // reformat immediately, not at the next second
}

void tz_dd_cb(lv_event_t *e) {
    const int idx = (int)lv_dropdown_get_selected(lv_event_get_target_obj(e));
    nv_time_set_tz(idx);   // live setenv+tzset + persist
    clk_tick(nullptr);     // clock reflects the new zone immediately
}

void cat_datetime(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(c, clk_page_deleted, LV_EVENT_DELETE, nullptr);
    const NvTheme *th = nv_theme_get();

    // Hero clock card: live HH:MM:SS + localized date line + sync status.
    lv_obj_t *card = surface_card(c);
    s_clk_time = lv_label_create(card);
    lv_obj_set_style_text_font(s_clk_time, &nv_font_28, 0);
    lv_obj_set_style_text_color(s_clk_time, th->text_strong, 0);
    lv_label_set_text(s_clk_time, "");
    s_clk_date = lv_label_create(card);
    lv_obj_set_style_text_color(s_clk_date, th->text, 0);
    lv_label_set_text(s_clk_date, "");
    s_clk_sync = lv_label_create(card);
    lv_label_set_text(s_clk_sync, "");
    s_clk_synced = !nv_time_is_synced();   // force the first tick to style the sync line

    nv_kit_switch_row(c, nv_tr(NV_STR_TIME_24H), nv_time_is_24h(), h24_cb);

    // Time zone: a single dropdown instead of a 15-row list. One widget vs ~45 objects — keeps
    // this page the lightest detail in the app (the old list was the object-heaviest page and
    // pushed the fixed 64 KB LVGL pool to failure with the IME/launcher/shade resident).
    section_label(c, nv_tr(NV_STR_TIMEZONE));
    char opts[512];
    int off = 0;
    const int tzn = nv_time_tz_count();
    for (int i = 0; i < tzn; i++)
        off += lv_snprintf(opts + off, sizeof(opts) - off, "%s%s",
                           nv_time_tz_name(i), i < tzn - 1 ? "\n" : "");
    lv_obj_t *dd = lv_dropdown_create(c);
    lv_dropdown_set_options(dd, opts);
    lv_dropdown_set_selected(dd, nv_time_get_tz());
    lv_obj_set_width(dd, lv_pct(100));
    lv_obj_set_style_bg_color(dd, th->surface, 0);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dd, th->surface3, 0);
    lv_obj_set_style_text_color(dd, th->text, 0);
    lv_obj_set_style_radius(dd, NV_RAD_SM, 0);
    // Style the pop-up list too (default-clickable safe; no shadow/transform — P4-renderer safe).
    lv_obj_t *ddlist = lv_dropdown_get_list(dd);
    lv_obj_set_style_bg_color(ddlist, th->surface, 0);
    lv_obj_set_style_bg_opa(ddlist, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ddlist, th->surface3, 0);
    lv_obj_set_style_text_color(ddlist, th->text, 0);
    lv_obj_set_style_bg_color(ddlist, th->accent,
                              (uint32_t)LV_PART_SELECTED | (uint32_t)LV_STATE_CHECKED);
    lv_obj_add_event_cb(dd, tz_dd_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    clk_tick(nullptr);
    s_clk_timer = lv_timer_create(clk_tick, 1000, nullptr);
}

// -------------------------------------------------------------- Storage page (live, hot-plug)
lv_obj_t  *s_sto_col   = nullptr;
lv_timer_t *s_sto_timer = nullptr;
uint32_t   s_sto_gen   = 0;

void sto_build_body(void) {
    if (!s_sto_col) return;
    lv_obj_clean(s_sto_col);
    const NvTheme *th = nv_theme_get();

    // microSD: capacity bar when mounted, hint when not.
    section_label(s_sto_col, nv_tr(NV_STR_STORAGE_SD));
    uint64_t sd_total = 0, sd_free = 0;
    if (nv_sd_is_mounted() && nv_sd_info(&sd_total, &sd_free) && sd_total > 0) {
        lv_obj_t *card = surface_card(s_sto_col);
        const unsigned tot_mb  = (unsigned)(sd_total / (1024 * 1024));
        const unsigned free_mb = (unsigned)(sd_free / (1024 * 1024));
        const int used_pct = (int)(100 - (sd_free * 100) / sd_total);
        lv_obj_t *t = lv_label_create(card);
        lv_label_set_text_fmt(t, LV_SYMBOL_SD_CARD "  %s", nv_sd_mount_point());
        lv_obj_set_style_text_color(t, th->text_strong, 0);
        usage_bar(card, used_pct);
        lv_obj_t *d = lv_label_create(card);
        lv_label_set_text_fmt(d, nv_tr(NV_STR_MB_FREE_OF), free_mb, tot_mb);
        lv_obj_set_style_text_color(d, th->text_dim, 0);
    } else {
        lv_obj_t *w = nv_kit_info(s_sto_col);
        lv_label_set_text_fmt(w, "%s  %s", LV_SYMBOL_WARNING, nv_tr(NV_STR_SD_MISSING));
        lv_obj_set_style_text_color(w, th->danger, 0);
        lv_obj_t *h = nv_kit_info(s_sto_col);
        lv_label_set_text(h, nv_tr(NV_STR_SD_HINT));
        lv_obj_set_style_text_color(h, th->text_dim, 0);
    }

    // Internal flash: total size + the running firmware slot.
    section_label(s_sto_col, nv_tr(NV_STR_STORAGE_FLASH));
    uint32_t flash_sz = 0;
    esp_flash_get_size(nullptr, &flash_sz);
    const esp_partition_t *run = esp_ota_get_running_partition();
    char fv[64];
    lv_snprintf(fv, sizeof fv, "%u MB", (unsigned)(flash_sz / (1024 * 1024)));
    kv_row(s_sto_col, "Flash", fv);
    if (run) {
        lv_snprintf(fv, sizeof fv, "%s  ·  %u MB", run->label,
                    (unsigned)(run->size / (1024 * 1024)));
        kv_row(s_sto_col, "Firmware", fv);
    }

    // Preferences store (NVS) usage.
    nvs_stats_t st;
    if (nvs_get_stats(nullptr, &st) == ESP_OK && st.total_entries > 0) {
        lv_obj_t *n = nv_kit_info(s_sto_col);
        lv_label_set_text_fmt(n, nv_tr(NV_STR_NVS_USAGE),
                              (unsigned)st.used_entries, (unsigned)st.total_entries);
        lv_obj_set_style_text_color(n, th->text_dim, 0);
        usage_bar(s_sto_col, (int)((st.used_entries * 100) / st.total_entries));
    }
}
void sto_poll(lv_timer_t *) {
    const uint32_t g = nv_sd_generation();
    if (g != s_sto_gen) { s_sto_gen = g; sto_build_body(); }   // hot-plug: card in/out
}
void sto_page_deleted(lv_event_t *) {
    if (s_sto_timer) { lv_timer_delete(s_sto_timer); s_sto_timer = nullptr; }
    s_sto_col = nullptr;
}
void cat_storage(lv_obj_t *content) {
    s_sto_col = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(s_sto_col, sto_page_deleted, LV_EVENT_DELETE, nullptr);
    s_sto_gen = nv_sd_generation();
    sto_build_body();
    s_sto_timer = lv_timer_create(sto_poll, 2000, nullptr);
}

// -------------------------------------------------------------- Memory page (live bars)
lv_obj_t  *s_mem_sram_bar  = nullptr;
lv_obj_t  *s_mem_sram_lbl  = nullptr;
lv_obj_t  *s_mem_psram_bar = nullptr;
lv_obj_t  *s_mem_psram_lbl = nullptr;
lv_obj_t  *s_mem_label     = nullptr;
lv_timer_t *s_mem_timer    = nullptr;

void mem_fill(lv_obj_t *bar, lv_obj_t *lbl, size_t total, size_t freeb) {
    if (!bar || !lbl || total == 0) return;
    const NvTheme *th = nv_theme_get();
    const int pct = (int)(100 - (freeb * 100) / total);
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, pct > 90 ? th->danger : th->accent, LV_PART_INDICATOR);
    char b[48];
    lv_snprintf(b, sizeof b, nv_tr(NV_STR_MB_FREE_OF),
                (unsigned)(freeb / (1024 * 1024)), (unsigned)(total / (1024 * 1024)));
    // SRAM is sub-MB granular — show KB there instead.
    if (total < 4 * 1024 * 1024)
        lv_snprintf(b, sizeof b, nv_tr(NV_STR_KB_FREE), (unsigned)(freeb / 1024));
    lv_label_set_text(lbl, b);
}

void mem_tick(lv_timer_t *) {
    if (!s_mem_label) return;
    mem_fill(s_mem_sram_bar, s_mem_sram_lbl,
             heap_caps_get_total_size(MALLOC_CAP_INTERNAL), nv_mem_free_internal());
    mem_fill(s_mem_psram_bar, s_mem_psram_lbl,
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM), nv_mem_free_psram());
    lv_label_set_text_fmt(s_mem_label,
                          nv_tr(NV_STR_MEM_STATS),
                          (unsigned)(nv_mem_free_internal() / 1024),
                          (unsigned)(nv_mem_largest_internal() / 1024),
                          (unsigned)(nv_mem_free_psram() / 1024),
                          nv_service_count());
}
void mem_page_deleted(lv_event_t *) {
    if (s_mem_timer) { lv_timer_delete(s_mem_timer); s_mem_timer = nullptr; }
    s_mem_label = nullptr;
    s_mem_sram_bar = s_mem_sram_lbl = s_mem_psram_bar = s_mem_psram_lbl = nullptr;
}
void cat_memory(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(c, mem_page_deleted, LV_EVENT_DELETE, nullptr);
    const NvTheme *th = nv_theme_get();

    lv_obj_t *card = surface_card(c);
    lv_obj_t *t1 = lv_label_create(card);
    lv_label_set_text(t1, "SRAM");
    lv_obj_set_style_text_color(t1, th->text_strong, 0);
    s_mem_sram_bar = usage_bar(card, 0);
    s_mem_sram_lbl = lv_label_create(card);
    lv_obj_set_style_text_color(s_mem_sram_lbl, th->text_dim, 0);

    lv_obj_t *t2 = lv_label_create(card);
    lv_label_set_text(t2, "PSRAM");
    lv_obj_set_style_text_color(t2, th->text_strong, 0);
    lv_obj_set_style_pad_top(t2, NV_SP_2, 0);
    s_mem_psram_bar = usage_bar(card, 0);
    s_mem_psram_lbl = lv_label_create(card);
    lv_obj_set_style_text_color(s_mem_psram_lbl, th->text_dim, 0);

    s_mem_label = nv_kit_info(c);
    mem_tick(nullptr);
    s_mem_timer = lv_timer_create(mem_tick, 1000, nullptr);

    lv_obj_t *btn = nv_kit_button(c, nv_tr(NV_STR_DUMP_LOG), true);
    lv_obj_add_event_cb(btn, dumplog_cb, LV_EVENT_CLICKED, nullptr);
}

// -------------------------------------------------------------- Anima page (branded preview)
void cat_anima(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    const NvTheme *th = nv_theme_get();

    lv_obj_t *hero = surface_card(c);
    lv_obj_set_style_pad_all(hero, NV_SP_5, 0);
    lv_obj_t *badge = lv_obj_create(hero);   // accent-tinted round emblem
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 64, 64);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, th->accent, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
    lv_obj_t *g = lv_label_create(badge);
    lv_label_set_text(g, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(g, &nv_font_28, 0);
    lv_obj_set_style_text_color(g, th->accent, 0);
    lv_obj_center(g);
    lv_obj_t *ttl = lv_label_create(hero);
    lv_label_set_text(ttl, "Anima");
    lv_obj_set_style_text_font(ttl, &nv_font_28, 0);
    lv_obj_set_style_text_color(ttl, th->text_strong, 0);
    lv_obj_t *tag = lv_label_create(hero);
    lv_label_set_text(tag, nv_tr(NV_STR_ANIMA_TAGLINE));
    lv_obj_set_style_text_color(tag, th->accent, 0);
    lv_obj_t *desc = lv_label_create(hero);
    lv_label_set_text(desc, nv_tr(NV_STR_ANIMA_DESC));
    lv_obj_set_width(desc, lv_pct(100));
    lv_label_set_long_mode(desc, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(desc, th->text, 0);
    lv_obj_set_style_pad_top(desc, NV_SP_2, 0);

    lv_obj_t *soon = nv_kit_info(c);
    lv_label_set_text_fmt(soon, LV_SYMBOL_CHARGE "  %s", nv_tr(NV_STR_ANIMA_SOON));
    lv_obj_set_style_text_color(soon, th->text_dim, 0);
}

// -------------------------------------------------------------- Language & Region page
void lang_row_cb(lv_event_t *e) {
    const nv_lang_t l = (nv_lang_t)(intptr_t)lv_event_get_user_data(e);
    nv_i18n_set_lang(l);  // persists + publishes NV_EV_LANG_CHANGED (SystemUI re-renders live)
}
void cat_language(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    const NvTheme *th = nv_theme_get();
    const nv_lang_t cur = nv_i18n_get_lang();
    for (int l = 0; l < NV_LANG_COUNT; l++) {
        lv_obj_t *row = nv_kit_row(c, nv_lang_native_name((nv_lang_t)l));
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, lang_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)l);
        if (l == cur) {   // accent-tinted active row (rebuilt via LANG_CHANGED on switch)
            lv_obj_set_style_bg_color(row, th->accent, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_20, 0);
        }
        lv_obj_t *ck = lv_label_create(row);          // checkmark marks the active language
        lv_label_set_text(ck, l == cur ? LV_SYMBOL_OK : "");
        lv_obj_set_style_text_color(ck, th->accent, 0);
    }
}

// -------------------------------------------------------------- Network (Wi-Fi) page
// Live page: a 700ms LVGL-thread poll rebuilds the body whenever nv_wifi's state or scan
// generation changes (nv_wifi mutates its state off-thread; we only ever read it here).
lv_obj_t  *s_net_col   = nullptr;   // body container (rebuilt in place)
lv_timer_t *s_net_timer = nullptr;
uint32_t   s_net_gen   = 0;
int        s_net_state = -1;
nv_wifi_ap_t s_net_aps[24];         // snapshot backing the row click handlers
int        s_net_apn   = 0;
char       s_net_conn[33] = "";     // persistent copy of the connected SSID (Forget target)
lv_obj_t  *s_pw_modal  = nullptr;   // password sheet (on the top layer)
lv_obj_t  *s_pw_ta     = nullptr;
char       s_pw_ssid[33] = "";
bool       s_net_pending = false;   // a deferred body rebuild is queued
bool       s_pw_pending  = false;   // a deferred password-sheet close is queued

void net_build_body(void);

void close_pw(void) {
    nv_ime_set_submit_cb(nullptr, nullptr);   // drop the keyboard-return hook for this sheet
    nv_ime_hide();                            // slide the on-screen keyboard away with the sheet
    if (s_pw_modal) { lv_obj_delete(s_pw_modal); s_pw_modal = nullptr; s_pw_ta = nullptr; }
}

// Every Wi-Fi handler runs while the very row/button that fired it is still on the stack, and
// net_build_body()'s first act is lv_obj_clean(s_net_col) — deleting that widget mid-event. So
// defer the rebuild (and the sheet close) to the next LVGL loop, after the event has unwound.
// Flags coalesce bursts and self-heal if scheduling fails.
void net_apply_async(void *) { s_net_pending = false; if (s_net_col) net_build_body(); }
void net_rebuild(void) {
    if (s_net_pending || !s_net_col) return;
    if (lv_async_call(net_apply_async, nullptr) == LV_RESULT_OK) s_net_pending = true;
}
void pw_close_async(void *) { s_pw_pending = false; close_pw(); if (s_net_col) net_build_body(); }
void pw_close_deferred(void) {
    if (s_pw_pending) return;
    if (lv_async_call(pw_close_async, nullptr) == LV_RESULT_OK) s_pw_pending = true;
}

lv_color_t signal_color(int8_t rssi) {
    const NvTheme *th = nv_theme_get();
    if (rssi >= -60) return th->success_solid;
    if (rssi >= -72) return th->accent;
    return th->text_dim;
}

void wifi_toggle_cb(lv_event_t *e) {
    nv_wifi_set_enabled(lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
    net_rebuild();
}
void scan_cb(lv_event_t *) { nv_wifi_start_scan(); net_rebuild(); }
void disconnect_cb(lv_event_t *) { nv_wifi_disconnect(); net_rebuild(); }
void forget_cb(lv_event_t *e) {
    nv_wifi_forget(static_cast<const char *>(lv_event_get_user_data(e)));
    net_rebuild();
}

void do_pw_connect(void) {
    const char *pass = s_pw_ta ? lv_textarea_get_text(s_pw_ta) : "";
    nv_wifi_connect(s_pw_ssid, pass);   // copies the password synchronously (safe before close)
    pw_close_deferred();                // defer the sheet delete off the button's own event
}
void pw_connect_cb(lv_event_t *) { do_pw_connect(); }
void pw_submit_cb(lv_obj_t *, void *) { do_pw_connect(); }  // keyboard "Go" return key
void pw_eye_cb(lv_event_t *e) {
    if (!s_pw_ta) return;
    // Switch is "Show password": ON -> reveal (password mode OFF), OFF -> mask.
    const bool show = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    lv_textarea_set_password_mode(s_pw_ta, !show);
}

void open_pw(const char *ssid) {
    lv_strcpy(s_pw_ssid, ssid);
    close_pw();
    const NvTheme *th = nv_theme_get();

    // Parent on the active screen (NOT lv_layer_top): the shared IME keyboard is a screen child
    // that raises itself with move_foreground, so a top-layer sheet would sit above the keyboard
    // and the password field would be untypable. On-screen, the keyboard overlays the sheet.
    s_pw_modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_pw_modal);
    lv_obj_set_size(s_pw_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_pw_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_pw_modal, LV_OPA_50, 0);
    lv_obj_clear_flag(s_pw_modal, LV_OBJ_FLAG_SCROLLABLE);

    // Compact, FIXED-height card in the top region (the IME docks over the bottom ~42%). The
    // fixed height matters: the keyboard shifts the focused field's parent by its own height;
    // a content-sized card ballooned into a big empty surface block behind the keyboard. A
    // fixed height absorbs the shift with no visual change, and the content packs from the top.
    lv_obj_t *card = lv_obj_create(s_pw_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(88));
    lv_obj_set_style_max_width(card, 460, 0);
    lv_obj_set_height(card, 252);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(card, th->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, th->surface3, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_style_pad_row(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    // Title: centered Wi-Fi glyph + SSID.
    lv_obj_t *ttl = lv_label_create(card);
    lv_label_set_text_fmt(ttl, LV_SYMBOL_WIFI "   %s", ssid);
    // nv_font_20, NOT montserrat: SSIDs are arbitrary bytes and montserrat is ASCII-only —
    // a Latin-1 SSID ("Café_5G") must render the same here as in the list rows.
    lv_obj_set_style_text_font(ttl, &nv_font_20, 0);
    lv_obj_set_style_text_color(ttl, th->text_strong, 0);
    lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ttl, lv_pct(100));
    lv_obj_set_style_text_align(ttl, LV_TEXT_ALIGN_CENTER, 0);

    // Password field — direct child of the fixed-height card, so the IME parent-shift is a no-op.
    // Masked, single-line; the keyboard "Go" return key connects directly.
    s_pw_ta = nv_kit_textarea_ex(card, nv_tr(NV_STR_WIFI_PASSWORD), true,
                                 NV_IME_PASSWORD, NV_IME_RET_GO);
    lv_obj_set_width(s_pw_ta, lv_pct(100));
    nv_ime_set_submit_cb(pw_submit_cb, nullptr);

    // Show-password: a compact checkbox (no heavy settings-row card).
    lv_obj_t *show = lv_checkbox_create(card);
    lv_checkbox_set_text(show, nv_tr(NV_STR_WIFI_SHOW_PASSWORD));
    lv_obj_set_style_text_color(show, th->text_dim, 0);
    lv_obj_add_event_cb(show, pw_eye_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Actions: two equal, full-width buttons — Cancel (surface) + Connect (primary).
    lv_obj_t *btns = lv_obj_create(card);
    lv_obj_remove_style_all(btns);
    lv_obj_set_width(btns, lv_pct(100));
    lv_obj_set_height(btns, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btns, 12, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = lv_button_create(btns);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_set_height(cancel, 46);
    lv_obj_set_style_bg_color(cancel, th->surface3, 0);
    lv_obj_add_event_cb(cancel, [](lv_event_t *) { pw_close_deferred(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, nv_tr(NV_STR_CANCEL));
    lv_obj_set_style_text_color(cl, th->text_strong, 0);
    lv_obj_center(cl);

    lv_obj_t *ok = lv_button_create(btns);
    lv_obj_set_flex_grow(ok, 1);
    lv_obj_set_height(ok, 46);
    lv_obj_set_style_bg_color(ok, th->primary, 0);
    lv_obj_add_event_cb(ok, pw_connect_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ol = lv_label_create(ok);
    lv_label_set_text(ol, nv_tr(NV_STR_WIFI_CONNECT));
    lv_obj_set_style_text_color(ol, th->on_primary, 0);
    lv_obj_center(ol);
}

void ap_click_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_net_apn) return;
    const nv_wifi_ap_t &a = s_net_aps[i];
    if (a.secured && !a.saved) {
        open_pw(a.ssid);          // opens the sheet on-screen; the clicked row is left intact
        return;                   // no rebuild here (would delete the row mid-click)
    }
    nv_wifi_connect(a.ssid, nullptr);   // open or already-saved -> straight connect
    net_rebuild();
}

// One network row: [wifi icon]  SSID .......  [WPA2/Open + saved dot]
void ap_row(lv_obj_t *col, const nv_wifi_ap_t &a, int index, bool connected) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *row = nv_kit_row(col, a.ssid);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, ap_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    // leading wifi glyph is faked by recoloring: prepend an icon before the label is awkward,
    // so we add trailing status instead — a compact group pushed to the right edge.
    lv_obj_t *tr = lv_obj_create(row);
    lv_obj_remove_style_all(tr);
    no_click(tr);   // status cluster must not eat the row tap
    lv_obj_set_size(tr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tr, 10, 0);
    lv_obj_clear_flag(tr, LV_OBJ_FLAG_SCROLLABLE);

    if (connected) {
        lv_obj_t *ok = lv_label_create(tr);
        lv_label_set_text(ok, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(ok, th->success_solid, 0);
    } else if (a.saved) {
        lv_obj_t *sv = lv_label_create(tr);
        lv_label_set_text(sv, nv_tr(NV_STR_WIFI_SAVED));
        lv_obj_set_style_text_color(sv, th->text_dim, 0);
    }

    // Generation badge (Wi-Fi 6 / 4) in the accent color, when the scan reported it.
    const char *genl = nv_wifi_gen_label(a.gen);
    if (genl[0]) {
        lv_obj_t *g = lv_label_create(tr);
        lv_label_set_text(g, genl);
        lv_obj_set_style_text_color(g, th->accent, 0);
    }

    lv_obj_t *sec = lv_label_create(tr);
    lv_label_set_text(sec, a.secured ? nv_wifi_auth_label(a.auth) : nv_tr(NV_STR_WIFI_OPEN));
    lv_obj_set_style_text_color(sec, th->text_dim, 0);

    lv_obj_t *ic = lv_label_create(tr);
    lv_label_set_text(ic, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(ic, signal_color(a.rssi), 0);
}

void net_build_body(void) {
    if (!s_net_col) return;
    lv_obj_clean(s_net_col);
    const NvTheme *th = nv_theme_get();

    // Wired Ethernet first: zero-config — the card just reflects cable/DHCP state live.
    if (nv_eth_available()) {
        section_label(s_net_col, "Ethernet");
        const nv_eth_state_t est = nv_eth_get_state();
        if (est == NV_ETH_UP) {
            lv_obj_t *card = surface_card(s_net_col);
            lv_obj_set_style_pad_row(card, 6, 0);
            char ip[16], mac[18];
            nv_eth_get_ip(ip, sizeof ip);
            nv_eth_get_mac(mac, sizeof mac);
            lv_obj_t *t1 = lv_label_create(card);
            lv_label_set_text_fmt(t1, LV_SYMBOL_OK "  %s", nv_tr(NV_STR_WIFI_CONNECTED));
            lv_obj_set_style_text_color(t1, th->success, 0);
            lv_obj_t *t2 = lv_label_create(card);
            lv_label_set_text_fmt(t2, "IP %s   ·   %d Mbps", ip, nv_eth_speed_mbps());
            lv_obj_set_style_text_color(t2, th->text, 0);
            if (mac[0]) {
                lv_obj_t *t3 = lv_label_create(card);
                lv_label_set_text_fmt(t3, "MAC %s", mac);
                lv_obj_set_style_text_color(t3, th->text_dim, 0);
            }
        } else if (est == NV_ETH_LINK) {
            lv_obj_t *l = nv_kit_info(s_net_col);
            lv_label_set_text(l, nv_tr(NV_STR_WIFI_CONNECTING));
            lv_obj_set_style_text_color(l, th->text_dim, 0);
        } else {
            lv_obj_t *l = nv_kit_info(s_net_col);
            lv_label_set_text(l, nv_tr(NV_STR_ETH_DOWN));
            lv_obj_set_style_text_color(l, th->text_dim, 0);
        }
        section_label(s_net_col, "Wi-Fi");
    }

    nv_kit_switch_row(s_net_col, nv_tr(NV_STR_WIFI), nv_wifi_is_enabled(), wifi_toggle_cb);

    if (!nv_wifi_has_radio()) {   // simulated backend banner (no C6 slave present)
        lv_obj_t *b = nv_kit_info(s_net_col);
        lv_label_set_text(b, nv_tr(NV_STR_WIFI_DEMO));
        lv_obj_set_style_text_color(b, th->text_dim, 0);
    }

    if (!nv_wifi_is_enabled()) {
        lv_label_set_text(nv_kit_info(s_net_col), nv_tr(NV_STR_WIFI_OFF));
        return;
    }

    const nv_wifi_state_t st = nv_wifi_get_state();

    // Connected card (SSID + IP + Disconnect/Forget)
    char cs[33], ip[16]; int8_t rs = 0;
    const bool have_conn = nv_wifi_get_connected(cs, sizeof(cs), ip, sizeof(ip), &rs);
    if (have_conn) {
        lv_obj_t *card = surface_card(s_net_col);
        lv_obj_set_style_pad_row(card, 6, 0);

        lv_obj_t *t1 = lv_label_create(card);
        lv_label_set_text_fmt(t1, "%s  %s", LV_SYMBOL_WIFI, cs);
        lv_obj_set_style_text_color(t1, th->text_strong, 0);
        lv_obj_t *t2 = lv_label_create(card);
        lv_label_set_text_fmt(t2, "%s  -  IP %s", nv_tr(NV_STR_WIFI_CONNECTED), ip);
        lv_obj_set_style_text_color(t2, th->text_dim, 0);

        // Link detail: negotiated generation (Wi-Fi 6/4) · security · channel · signal, then the
        // full IP config (gateway / mask / DNS / station MAC) for diagnostics.
        nv_wifi_link_t lk;
        if (nv_wifi_get_link(&lk)) {
            const char *g = nv_wifi_gen_label(lk.gen);
            lv_obj_t *t3 = lv_label_create(card);
            lv_label_set_text_fmt(t3, "%s%s%s   ch %u   %d dBm",
                                  g, g[0] ? "   " : "", nv_wifi_auth_label(lk.auth),
                                  (unsigned)lk.channel, (int)lk.rssi);
            lv_obj_set_style_text_color(t3, th->text_dim, 0);

            if (lk.gateway[0]) {
                lv_obj_t *t4 = lv_label_create(card);
                lv_label_set_text_fmt(t4, "GW %s   Mask %s", lk.gateway, lk.netmask);
                lv_obj_set_style_text_color(t4, th->text_dim, 0);
            }
            if (lk.dns[0]) {
                lv_obj_t *t5 = lv_label_create(card);
                lv_label_set_text_fmt(t5, "DNS %s", lk.dns);
                lv_obj_set_style_text_color(t5, th->text_dim, 0);
            }
            if (lk.mac[0]) {
                lv_obj_t *t6 = lv_label_create(card);
                lv_label_set_text_fmt(t6, "MAC %s", lk.mac);
                lv_obj_set_style_text_color(t6, th->text_dim, 0);
            }
        }

        lv_obj_t *br = lv_obj_create(card);
        lv_obj_remove_style_all(br);
        lv_obj_set_size(br, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(br, 10, 0);
        lv_obj_clear_flag(br, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *dc = nv_kit_button(br, nv_tr(NV_STR_WIFI_DISCONNECT), false);
        lv_obj_add_event_cb(dc, disconnect_cb, LV_EVENT_CLICKED, nullptr);
        lv_strcpy(s_net_conn, cs);   // persistent Forget target (cs is a stack buffer)
        lv_obj_t *fg = nv_kit_button(br, nv_tr(NV_STR_WIFI_FORGET), false);
        lv_obj_add_event_cb(fg, forget_cb, LV_EVENT_CLICKED, (void *)s_net_conn);
    }

    // Scan control / progress
    if (st == NV_WIFI_SCANNING || st == NV_WIFI_CONNECTING) {
        lv_obj_t *r = lv_obj_create(s_net_col);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 12, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *sp = lv_spinner_create(r);
        lv_obj_set_size(sp, 28, 28);
        lv_obj_t *lb = lv_label_create(r);
        lv_label_set_text(lb, nv_tr(st == NV_WIFI_SCANNING ? NV_STR_WIFI_SCANNING
                                                           : NV_STR_WIFI_CONNECTING));
        lv_obj_set_style_text_color(lb, th->text_dim, 0);
    } else {
        char sb[48];
        lv_snprintf(sb, sizeof sb, LV_SYMBOL_REFRESH "  %s", nv_tr(NV_STR_WIFI_SCAN));
        lv_obj_t *scan = nv_kit_button(s_net_col, sb, false);
        lv_obj_add_event_cb(scan, scan_cb, LV_EVENT_CLICKED, nullptr);
    }

    if (st == NV_WIFI_FAILED) {
        lv_obj_t *w = nv_kit_info(s_net_col);
        lv_label_set_text_fmt(w, "%s  %s", LV_SYMBOL_WARNING, nv_tr(NV_STR_WIFI_FAILED));
        lv_obj_set_style_text_color(w, th->danger, 0);
    }

    // Available networks
    lv_label_set_text(nv_kit_info(s_net_col), nv_tr(NV_STR_WIFI_AVAILABLE));
    s_net_apn = nv_wifi_copy_aps(s_net_aps, 24);
    for (int i = 0; i < s_net_apn; i++) {
        const bool is_conn = have_conn && strcmp(s_net_aps[i].ssid, cs) == 0;
        ap_row(s_net_col, s_net_aps[i], i, is_conn);
    }
    if (s_net_apn == 0)
        lv_label_set_text(nv_kit_info(s_net_col), nv_tr(NV_STR_WIFI_NO_NETWORKS));
}

uint32_t s_net_eth_gen = 0;   // ethernet state generation (cable/DHCP changes)

void net_poll(lv_timer_t *) {
    const uint32_t g  = nv_wifi_scan_generation();
    const uint32_t eg = nv_eth_generation();
    const int st = (int)nv_wifi_get_state();
    if (g != s_net_gen || st != s_net_state || eg != s_net_eth_gen) {
        s_net_gen = g; s_net_state = st; s_net_eth_gen = eg;
        net_build_body();
    }
}

void net_page_deleted(lv_event_t *) {
    if (s_net_timer) { lv_timer_delete(s_net_timer); s_net_timer = nullptr; }
    close_pw();
    s_net_col = nullptr;
}

void cat_network(lv_obj_t *content) {
    s_net_pending = false; s_pw_pending = false;   // clear stale deferral flags from a prior page
    s_net_col = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(s_net_col, net_page_deleted, LV_EVENT_DELETE, nullptr);
    s_net_gen = nv_wifi_scan_generation();
    s_net_state = (int)nv_wifi_get_state();
    s_net_eth_gen = nv_eth_generation();
    net_build_body();
    s_net_timer = lv_timer_create(net_poll, 700, nullptr);
}

// -------------------------------------------------------------- System update (OTA) page
// Live page like Network: a 500ms poll rebuilds the body on any nv_ota state/progress change.
lv_obj_t  *s_upd_col = nullptr;
lv_timer_t *s_upd_timer = nullptr;
uint32_t   s_upd_gen = 0;
lv_obj_t  *s_upd_ta  = nullptr;         // manifest-URL field
lv_obj_t  *s_store_ta = nullptr;        // app-store base-URL field (saved on demand)
bool       s_upd_pending = false;
char       s_upd_url[256] = "";         // retained across page opens within a boot

void upd_build_body(void);
void upd_apply_async(void *) { s_upd_pending = false; if (s_upd_col) upd_build_body(); }
void upd_rebuild(void) {
    if (s_upd_pending || !s_upd_col) return;
    if (lv_async_call(upd_apply_async, nullptr) == LV_RESULT_OK) s_upd_pending = true;
}

// Default manifest URL baked in for convenience — editable and then persisted, so it only needs
// typing once (or never, when the local update server is at this address).
constexpr char kDefaultOtaUrl[] = "http://192.168.0.216:8080/manifest.json";

void do_upd_check(void) {
    if (s_upd_ta) lv_snprintf(s_upd_url, sizeof(s_upd_url), "%s", lv_textarea_get_text(s_upd_ta));
    nv_config_set_str("ota_url", s_upd_url);   // remember across reboots (no retyping on a keyboard)
    nv_ota_check(s_upd_url);
    upd_rebuild();
}
void upd_check_cb(lv_event_t *) { do_upd_check(); }
void store_save_cb(lv_event_t *) {
    if (!s_store_ta) return;
    const char *u = lv_textarea_get_text(s_store_ta);
    nv_appstore_set_url(u);
    nv_appstore_refresh();        // re-fetch the catalog from the new host
    nv_ui_toast(nv_tr(NV_STR_SAVED));
}

// Store region picker — the device sends this as ?region= so the store geolocates the catalog.
const char *const kRegionCodes[] = { "", "*", "IT", "ES", "FR", "DE", "GB", "US", "EU" };
constexpr char kRegionOpts[] =
    "Auto\nWorldwide\nItaly (IT)\nSpain (ES)\nFrance (FR)\nGermany (DE)\nUK (GB)\nUSA (US)\nEurope (EU)";
void store_region_cb(lv_event_t *e) {
    const uint32_t i = lv_dropdown_get_selected(lv_event_get_target_obj(e));
    if (i < sizeof(kRegionCodes) / sizeof(kRegionCodes[0])) {
        nv_appstore_set_region(kRegionCodes[i]);
        nv_appstore_refresh();
    }
}
void upd_submit_cb(lv_obj_t *, void *) { do_upd_check(); }   // keyboard "Go" on the URL field
void upd_install_cb(lv_event_t *) { nv_ota_update(); upd_rebuild(); }
void upd_sd_cb(lv_event_t *) { nv_ota_install_sd(nullptr); upd_rebuild(); }
void upd_restart_cb(lv_event_t *) { nv_ota_reboot(); }

void upd_build_body(void) {
    if (!s_upd_col) return;
    lv_obj_clean(s_upd_col);
    const NvTheme *th = nv_theme_get();
    const nv_ota_state_t st = nv_ota_state();
    const bool busy = (st == NV_OTA_CHECKING || st == NV_OTA_DOWNLOADING);

    lv_label_set_text_fmt(nv_kit_info(s_upd_col), "%s:  v%s",
                          nv_tr(NV_STR_UPDATE_CURRENT), nv_ota_running_version());

    // Manifest URL field (keyboard "Go" triggers the check).
    s_upd_ta = nv_kit_textarea_ex(s_upd_col, nv_tr(NV_STR_UPDATE_URL), true,
                                  NV_IME_URL, NV_IME_RET_GO);
    lv_obj_set_width(s_upd_ta, lv_pct(100));
    if (s_upd_url[0]) lv_textarea_set_text(s_upd_ta, s_upd_url);
    nv_ime_set_submit_cb(upd_submit_cb, nullptr);

    // App store base URL — the host the Apps → Store tab installs WASM apps from. Host only
    // (e.g. http://192.168.0.216:8090); the device appends /store.json and /apps/<id>/…
    lv_obj_t *sh = lv_label_create(s_upd_col);
    lv_label_set_text(sh, "App store");
    lv_obj_set_style_text_font(sh, &nv_font_14, 0);
    lv_obj_set_style_text_color(sh, th->text_dim, 0);
    char store_url[192];
    nv_appstore_get_url(store_url, sizeof store_url);
    s_store_ta = nv_kit_textarea_ex(s_upd_col, "http://host:8090", true, NV_IME_URL, NV_IME_RET_DONE);
    lv_obj_set_width(s_store_ta, lv_pct(100));
    lv_textarea_set_text(s_store_ta, store_url);
    lv_obj_t *ssave = nv_kit_button(s_upd_col, "SAVE STORE URL", false);
    lv_obj_add_event_cb(ssave, store_save_cb, LV_EVENT_CLICKED, nullptr);

    // Region — geolocates the store catalog (?region=). Auto lets the server infer from language.
    lv_obj_t *rl = lv_label_create(s_upd_col);
    lv_label_set_text_fmt(rl, "%s", nv_tr(NV_STR_STORE_REGION));
    lv_obj_set_style_text_font(rl, &nv_font_14, 0);
    lv_obj_set_style_text_color(rl, th->text_dim, 0);
    lv_obj_t *rdd = lv_dropdown_create(s_upd_col);
    lv_dropdown_set_options(rdd, kRegionOpts);
    lv_obj_set_width(rdd, lv_pct(100));
    char cur_region[16];
    nv_appstore_get_region(cur_region, sizeof cur_region);
    for (uint32_t i = 0; i < sizeof(kRegionCodes) / sizeof(kRegionCodes[0]); i++)
        if (!strcmp(cur_region, kRegionCodes[i])) { lv_dropdown_set_selected(rdd, i); break; }
    lv_obj_add_event_cb(rdd, store_region_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Status line (from the service; colored by state).
    if (nv_ota_message()[0]) {
        lv_obj_t *msg = nv_kit_info(s_upd_col);
        lv_label_set_text(msg, nv_ota_message());
        lv_color_t c = th->text_dim;
        if (st == NV_OTA_FAILED)       c = th->danger;
        else if (st == NV_OTA_AVAILABLE) c = th->accent;
        else if (st == NV_OTA_SUCCESS)   c = th->success;
        lv_obj_set_style_text_color(msg, c, 0);
    }

    // Progress bar while downloading.
    if (st == NV_OTA_DOWNLOADING) {
        lv_obj_t *bar = lv_bar_create(s_upd_col);
        lv_obj_set_width(bar, lv_pct(100));
        lv_obj_set_height(bar, 12);
        lv_bar_set_value(bar, nv_ota_progress(), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(bar, th->surface3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, th->primary, LV_PART_INDICATOR);
    }
    if (busy) {  // spinner while checking/downloading
        lv_obj_t *sp = lv_spinner_create(s_upd_col);
        lv_obj_set_size(sp, 28, 28);
    }

    // Actions.
    if (!busy) {
        lv_obj_t *chk = nv_kit_button(s_upd_col, nv_tr(NV_STR_UPDATE_CHECK), false);
        lv_obj_add_event_cb(chk, upd_check_cb, LV_EVENT_CLICKED, nullptr);
        // Offline: flash a firmware the user dropped on the SD card (/sdcard/nucleos-anima.bin).
        if (nv_sd_is_mounted()) {
            lv_obj_t *sd = nv_kit_button(s_upd_col, nv_tr(NV_STR_UPDATE_FROM_SD), false);
            lv_obj_add_event_cb(sd, upd_sd_cb, LV_EVENT_CLICKED, nullptr);
        }
    }
    if (st == NV_OTA_AVAILABLE) {
        lv_obj_t *ins = nv_kit_button(s_upd_col, nv_tr(NV_STR_UPDATE_INSTALL), true);
        lv_obj_add_event_cb(ins, upd_install_cb, LV_EVENT_CLICKED, nullptr);
    }
    if (st == NV_OTA_SUCCESS) {
        lv_obj_t *rb = nv_kit_button(s_upd_col, nv_tr(NV_STR_UPDATE_RESTART), true);
        lv_obj_add_event_cb(rb, upd_restart_cb, LV_EVENT_CLICKED, nullptr);
    }
}

void upd_poll(lv_timer_t *) {
    const uint32_t g = nv_ota_generation();
    if (g != s_upd_gen) { s_upd_gen = g; upd_build_body(); }
}
void upd_page_deleted(lv_event_t *) {
    if (s_upd_timer) { lv_timer_delete(s_upd_timer); s_upd_timer = nullptr; }
    nv_ime_set_submit_cb(nullptr, nullptr);
    nv_ime_hide();
    s_upd_col = nullptr;
    s_upd_ta = nullptr;
    s_store_ta = nullptr;
}
void cat_update(lv_obj_t *content) {
    s_upd_pending = false;
    // Preload the saved manifest URL (or the baked default) so the field is ready — no retyping.
    if (!s_upd_url[0])
        nv_config_get_str("ota_url", kDefaultOtaUrl, s_upd_url, sizeof(s_upd_url));
    s_upd_col = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(s_upd_col, upd_page_deleted, LV_EVENT_DELETE, nullptr);
    s_upd_gen = nv_ota_generation();   // seed current: the explicit build below is the initial one
    upd_build_body();
    s_upd_timer = lv_timer_create(upd_poll, 500, nullptr);
}

// -------------------------------------------------------------- Backup & restore (+ factory reset)
lv_obj_t *s_rst_modal = nullptr;
bool      s_rst_pending = false;

void backup_now_cb(lv_event_t *) {
    nv_ui_toast(nv_backup_export() ? nv_tr(NV_STR_SAVED) : nv_tr(NV_STR_SD_MISSING));
}
void backup_restore_cb(lv_event_t *) {
    if (nv_backup_import()) esp_restart();          // reboot so every restored pref applies cleanly
    else nv_ui_toast(nv_tr(NV_STR_SD_MISSING));
}

void rst_close_async(void *) {
    s_rst_pending = false;
    if (s_rst_modal) { lv_obj_delete(s_rst_modal); s_rst_modal = nullptr; }
}
void rst_close_deferred(void) {   // Cancel is a child of the modal — defer its deletion
    if (s_rst_pending) return;
    if (lv_async_call(rst_close_async, nullptr) == LV_RESULT_OK) s_rst_pending = true;
}
void rst_go_cb(lv_event_t *) {
    // Drop the SD mirror first — else nv_backup's restore-if-empty resurrects every wiped
    // setting (Wi-Fi credentials included) at the next boot, silently undoing the reset.
    // If the mirror provably survives (remove() failed on a mounted card), ABORT.
    if (!nv_backup_delete()) {
        nv_ui_toast(nv_tr(NV_STR_RESET_FAILED));
        rst_close_deferred();
        return;
    }
    // Point of no return. esp_restart() never returns.
    nvs_flash_deinit();
    nvs_flash_erase();
    esp_restart();
}

void open_factory_confirm(lv_event_t *) {
    if (s_rst_modal) return;
    const NvTheme *th = nv_theme_get();

    s_rst_modal = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_rst_modal);
    lv_obj_set_size(s_rst_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_rst_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_rst_modal, LV_OPA_50, 0);
    lv_obj_clear_flag(s_rst_modal, LV_OBJ_FLAG_SCROLLABLE);
    // Tapping the scrim (outside the card) cancels.
    lv_obj_add_flag(s_rst_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_rst_modal, [](lv_event_t *e) {
        if (lv_event_get_target_obj(e) == lv_event_get_current_target_obj(e))
            rst_close_deferred();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *card = lv_obj_create(s_rst_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(70));
    lv_obj_set_style_max_width(card, 520, 0);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, th->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, th->surface3, 0);
    lv_obj_set_style_pad_all(card, NV_SP_5, 0);
    lv_obj_set_style_pad_row(card, NV_SP_4, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = lv_label_create(card);
    lv_label_set_text_fmt(ttl, LV_SYMBOL_WARNING "  %s", nv_tr(NV_STR_ERASE_CONFIRM));
    lv_obj_set_style_text_font(ttl, &nv_font_20, 0);
    lv_obj_set_style_text_color(ttl, th->danger, 0);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body, nv_tr(NV_STR_FACTORY_INFO));
    lv_obj_set_width(body, lv_pct(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body, th->text, 0);

    lv_obj_t *btns = lv_obj_create(card);
    lv_obj_remove_style_all(btns);
    lv_obj_set_width(btns, lv_pct(100));
    lv_obj_set_height(btns, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 12, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = nv_kit_button(btns, nv_tr(NV_STR_CANCEL), false);
    lv_obj_set_flex_grow(cancel, 1);
    lv_obj_add_event_cb(cancel, [](lv_event_t *) { rst_close_deferred(); },
                        LV_EVENT_CLICKED, nullptr);

    lv_obj_t *go = nv_kit_button(btns, nv_tr(NV_STR_ERASE_BTN), true);
    lv_obj_set_flex_grow(go, 1);
    lv_obj_set_style_bg_color(go, th->danger, 0);         // destructive: danger fill
    lv_obj_set_style_text_color(go, lv_color_white(), 0);
    lv_obj_add_event_cb(go, rst_go_cb, LV_EVENT_CLICKED, nullptr);
}

void bak_page_deleted(lv_event_t *) {
    s_rst_pending = false;
    if (s_rst_modal) { lv_obj_delete(s_rst_modal); s_rst_modal = nullptr; }
}

void cat_backup(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(c, bak_page_deleted, LV_EVENT_DELETE, nullptr);
    const NvTheme *th = nv_theme_get();

    lv_obj_t *info = nv_kit_info(c);
    lv_label_set_text(info, nv_tr(NV_STR_BACKUP_INFO));
    lv_obj_set_style_text_color(info, th->text_dim, 0);

    if (!nv_sd_is_mounted()) {
        lv_obj_t *w = nv_kit_info(c);
        lv_label_set_text_fmt(w, "%s  %s", LV_SYMBOL_WARNING, nv_tr(NV_STR_SD_MISSING));
        lv_obj_set_style_text_color(w, th->danger, 0);
    } else {
        lv_obj_t *b1 = nv_kit_button(c, nv_tr(NV_STR_BACKUP_NOW), true);
        lv_obj_add_event_cb(b1, backup_now_cb, LV_EVENT_CLICKED, nullptr);
        if (nv_backup_available()) {
            lv_obj_t *b2 = nv_kit_button(c, nv_tr(NV_STR_BACKUP_RESTORE), false);
            lv_obj_add_event_cb(b2, backup_restore_cb, LV_EVENT_CLICKED, nullptr);
        }
    }

    // Danger zone: factory reset (confirm sheet; erases NVS + the SD mirror).
    section_label(c, nv_tr(NV_STR_FACTORY_RESET));
    lv_obj_t *fi = nv_kit_info(c);
    lv_label_set_text(fi, nv_tr(NV_STR_FACTORY_INFO));
    lv_obj_set_style_text_color(fi, th->text_dim, 0);
    lv_obj_t *fr = nv_kit_button(c, nv_tr(NV_STR_FACTORY_RESET), false);
    lv_obj_set_style_text_color(fr, th->danger, 0);       // destructive ink on neutral fill
    lv_obj_add_event_cb(fr, open_factory_confirm, LV_EVENT_CLICKED, nullptr);
}

// -------------------------------------------------------------- About page (device identity)
lv_obj_t  *s_up_label = nullptr;
lv_obj_t  *s_temp_label = nullptr;
lv_timer_t *s_up_timer = nullptr;

void up_tick(lv_timer_t *) {
    if (!s_up_label) return;
    const uint64_t secs = (uint64_t)(esp_timer_get_time() / 1000000LL);
    lv_label_set_text_fmt(s_up_label, nv_tr(NV_STR_UPTIME_FMT),
                          (unsigned)(secs / 86400), (unsigned)((secs / 3600) % 24),
                          (unsigned)((secs / 60) % 60));
    float tc;
    if (s_temp_label && nv_hal_temp_read(&tc)) {   // on-die temp, same 1s cadence
        // Explicit sign: -0.5 would otherwise print as "0.5" (integer -0 has no sign).
        const int t10 = (int)(tc * 10.0f + (tc >= 0 ? 0.5f : -0.5f));
        const int a = t10 < 0 ? -t10 : t10;
        lv_label_set_text_fmt(s_temp_label, "%s%d.%d °C", t10 < 0 ? "-" : "", a / 10, a % 10);
    }
}
void about_page_deleted(lv_event_t *) {
    if (s_up_timer) { lv_timer_delete(s_up_timer); s_up_timer = nullptr; }
    s_up_label = nullptr;
    s_temp_label = nullptr;
}
void restart_cb(lv_event_t *) { esp_restart(); }

void cat_about(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(c, about_page_deleted, LV_EVENT_DELETE, nullptr);
    const NvTheme *th = nv_theme_get();
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    // Hero: OS identity.
    lv_obj_t *hero = surface_card(c);
    lv_obj_set_style_pad_all(hero, NV_SP_5, 0);
    lv_obj_t *ttl = lv_label_create(hero);
    lv_label_set_text(ttl, "NucleoOS Anima");
    lv_obj_set_style_text_font(ttl, &nv_font_28, 0);
    lv_obj_set_style_text_color(ttl, th->text_strong, 0);
    lv_obj_t *ver = lv_label_create(hero);
    lv_label_set_text_fmt(ver, "v%s", nv_ota_running_version());
    lv_obj_set_style_text_color(ver, th->accent, 0);

    char v[64];

    // Software.
    lv_snprintf(v, sizeof v, "%s %s", app->date, app->time);
    kv_row(c, nv_tr(NV_STR_ABOUT_BUILD), v);
    kv_row(c, "ESP-IDF", app->idf_ver);

    // Hardware.
    lv_snprintf(v, sizeof v, "ESP32-P4 · %d core · v%d.%d",
                chip.cores, chip.revision / 100, chip.revision % 100);
    kv_row(c, "SoC", v);
    const size_t sram_kb  = heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024;
    const size_t psram_mb = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024 * 1024);
    lv_snprintf(v, sizeof v, "%u KB SRAM  +  %u MB PSRAM",
                (unsigned)sram_kb, (unsigned)psram_mb);
    kv_row(c, "RAM", v);
    uint32_t flash_sz = 0;
    esp_flash_get_size(nullptr, &flash_sz);
    lv_snprintf(v, sizeof v, "%u MB", (unsigned)(flash_sz / (1024 * 1024)));
    kv_row(c, "Flash", v);
    lv_snprintf(v, sizeof v, "%d × %d",
                (int)lv_display_get_horizontal_resolution(nullptr),
                (int)lv_display_get_vertical_resolution(nullptr));
    kv_row(c, nv_tr(NV_STR_SET_DISPLAY), v);
    kv_row(c, "Wireless", "ESP32-C6 · Wi-Fi 6 · BLE 5");

    // Live uptime + on-die temperature (one shared 1s timer; hidden row if no sensor).
    s_up_label = kv_row(c, nv_tr(NV_STR_ABOUT_UPTIME), "");
    float tc;
    s_temp_label = nv_hal_temp_read(&tc) ? kv_row(c, nv_tr(NV_STR_TEMPERATURE), "") : nullptr;
    up_tick(nullptr);
    s_up_timer = lv_timer_create(up_tick, 1000, nullptr);

    lv_obj_t *rb = nv_kit_button(c, nv_tr(NV_STR_RESTART_DEVICE), false);
    lv_obj_add_event_cb(rb, restart_cb, LV_EVENT_CLICKED, nullptr);
}

// -------------------------------------------------------------- Sensors page (live)
lv_obj_t  *s_sens_temp = nullptr;
lv_obj_t  *s_sens_time = nullptr;
lv_obj_t  *s_sens_i2c  = nullptr;
lv_timer_t *s_sens_timer = nullptr;

const char *i2c_label(uint8_t a) {   // known on-board devices on the shared internal bus
    switch (a) {
        case 0x14: return "RX8130 RTC";
        case 0x18: return "ES8311 codec";
        case 0x40: return "ES7210 ADC";
        case 0x5D: return "GT911 touch";
        default:   return "\xC2\xB7";   // middle dot (U+00B7) for an unlabeled responder
    }
}
void sens_scan(void) {
    if (!s_sens_i2c) return;
    lv_obj_clean(s_sens_i2c);
    const NvTheme *th = nv_theme_get();
    i2c_master_bus_handle_t bus = nv_hal_i2c_bus();
    int found = 0;
    if (bus) {
        for (uint8_t a = 0x08; a <= 0x77; a++) {   // one-shot address poll (10ms/addr, ~bounded)
            if (i2c_master_probe(bus, a, 10) != ESP_OK) continue;
            char buf[8];
            lv_snprintf(buf, sizeof buf, "0x%02X", a);
            kv_row(s_sens_i2c, buf, i2c_label(a));
            found++;
        }
    }
    if (!found) {
        lv_obj_t *e = nv_kit_info(s_sens_i2c);
        lv_label_set_text(e, nv_tr(NV_STR_NONE));
        lv_obj_set_style_text_color(e, th->text_dim, 0);
    }
}
void sens_tick(lv_timer_t *) {
    if (s_sens_temp) {
        float tc;
        if (nv_hal_temp_read(&tc)) {
            const int t10 = (int)(tc * 10.0f + (tc >= 0 ? 0.5f : -0.5f));
            const int a = t10 < 0 ? -t10 : t10;
            lv_label_set_text_fmt(s_sens_temp, "%s%d.%d °C", t10 < 0 ? "-" : "", a / 10, a % 10);
        }
    }
    if (s_sens_time) {
        char b[24];
        nv_time_format(b, sizeof b, nv_time_is_24h() ? "%H:%M:%S" : "%I:%M:%S %p");
        lv_label_set_text(s_sens_time, b);
    }
}
void sens_page_deleted(lv_event_t *) {
    if (s_sens_timer) { lv_timer_delete(s_sens_timer); s_sens_timer = nullptr; }
    s_sens_temp = s_sens_time = s_sens_i2c = nullptr;
}
void sens_rescan_cb(lv_event_t *) { sens_scan(); }
void cat_sensors(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(c, sens_page_deleted, LV_EVENT_DELETE, nullptr);
    s_sens_temp = kv_row(c, nv_tr(NV_STR_TEMPERATURE), "");
    s_sens_time = kv_row(c, nv_tr(NV_STR_SET_DATETIME), "");

    section_label(c, nv_tr(NV_STR_I2C_DEVICES));
    s_sens_i2c = lv_obj_create(c);
    lv_obj_remove_style_all(s_sens_i2c);
    lv_obj_set_size(s_sens_i2c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_sens_i2c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_sens_i2c, NV_SP_2, 0);
    lv_obj_clear_flag(s_sens_i2c, LV_OBJ_FLAG_SCROLLABLE);
    sens_scan();
    lv_obj_t *rb = nv_kit_button(c, nv_tr(NV_STR_RESCAN), false);
    lv_obj_add_event_cb(rb, sens_rescan_cb, LV_EVENT_CLICKED, nullptr);

    sens_tick(nullptr);
    s_sens_timer = lv_timer_create(sens_tick, 1000, nullptr);
}

// -------------------------------------------------------------- Notifications page
void dnd_cb(lv_event_t *e) {
    nv_config_set_bool("qs_dnd", lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}
void notif_clearall_cb(lv_event_t *) { nv_notify_clear(); nv_ui_toast(nv_tr(NV_STR_CLEAR_ALL)); }
void cat_notifications(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    const NvTheme *th = nv_theme_get();
    nv_kit_switch_row(c, nv_tr(NV_STR_DND), nv_config_get_bool("qs_dnd", false), dnd_cb);
    lv_obj_t *info = nv_kit_info(c);
    lv_label_set_text_fmt(info, "%s:  %d", nv_tr(NV_STR_NOTIFICATIONS), nv_notify_count());
    lv_obj_set_style_text_color(info, th->text_dim, 0);
    lv_obj_t *cl = nv_kit_button(c, nv_tr(NV_STR_CLEAR_ALL), false);
    lv_obj_add_event_cb(cl, notif_clearall_cb, LV_EVENT_CLICKED, nullptr);
}

// -------------------------------------------------------------- Security page
// Screen lock (idle privacy screen), unlock PIN, lock-on-boot, and an honest read of the
// secret-storage posture. NVS is currently plaintext (no flash/NVS encryption yet) — surfaced
// here so the gap is visible in-product rather than hidden.
void cat_security(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);

    section_label(c, nv_tr(NV_STR_SCREEN_LOCK));
    nv_kit_switch_row(c, nv_tr(NV_STR_SCREEN_LOCK), nv_config_get_bool("lock_en", false), lock_en_cb);
    char pinbuf[6];
    nv_config_get_str("lockpin", "", pinbuf, sizeof pinbuf);
    lv_obj_t *setb = nv_kit_button(c, nv_tr(NV_STR_SET_PIN), false);   // set or replace the PIN
    lv_obj_add_event_cb(setb, setpin_cb, LV_EVENT_CLICKED, nullptr);
    if (pinbuf[0]) {
        lv_obj_t *rmb = nv_kit_button(c, nv_tr(NV_STR_REMOVE_PIN), false);
        lv_obj_add_event_cb(rmb, rmpin_cb, LV_EVENT_CLICKED, nullptr);
    }
    // Require the PIN at every startup (not just after idle-sleep).
    nv_kit_switch_row(c, nv_tr(NV_STR_LOCK_ON_BOOT), nv_config_get_bool("lock_boot", false),
                      lockboot_cb);

    // Encryption posture (honest; NVS is plaintext until flash+NVS encryption is enabled).
    section_label(c, nv_tr(NV_STR_ENCRYPTION));
    kv_row(c, nv_tr(NV_STR_ENCRYPTION), nv_tr(NV_STR_ENC_OFF));
}

// -------------------------------------------------------------- Accessibility page
void cat_access(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    section_label(c, nv_tr(NV_STR_FONT_SIZE));   // real, live: reuses the theme font-scale setter
    const nv_font_scale_t cur = nv_theme_get_font_scale();
    lv_obj_t *fonts = pick_row(c, 12);
    font_card(fonts, NV_FONT_NORMAL, &lv_font_montserrat_20, cur == NV_FONT_NORMAL);
    font_card(fonts, NV_FONT_LARGE, &lv_font_montserrat_28, cur == NV_FONT_LARGE);
}

// -------------------------------------------------------------- split view: rail + detail
struct Category {
    const char *symbol;
    nv_str_id_t name_id;
    nv_str_id_t group_id;              // rail section header (emitted on change)
    void (*build)(lv_obj_t *content);
};
const Category kCats[] = {
    {LV_SYMBOL_WIFI,     NV_STR_SET_NETWORK,  NV_STR_GROUP_CONNECT,  cat_network},
    {LV_SYMBOL_IMAGE,    NV_STR_SET_DISPLAY,  NV_STR_GROUP_DEVICE,   cat_display},
    {LV_SYMBOL_AUDIO,    NV_STR_SET_SOUND,    NV_STR_GROUP_DEVICE,   cat_sound},
    {LV_SYMBOL_BELL,     NV_STR_NOTIFICATIONS, NV_STR_GROUP_DEVICE,  cat_notifications},
    {LV_SYMBOL_REFRESH,  NV_STR_SET_DATETIME, NV_STR_GROUP_DEVICE,   cat_datetime},
    {LV_SYMBOL_SD_CARD,  NV_STR_SET_STORAGE,  NV_STR_GROUP_DEVICE,   cat_storage},
    {LV_SYMBOL_GPS,      NV_STR_SET_SENSORS,  NV_STR_GROUP_DEVICE,   cat_sensors},
    {LV_SYMBOL_LIST,     NV_STR_SET_MEMORY,   NV_STR_GROUP_DEVICE,   cat_memory},
    {LV_SYMBOL_CHARGE,   NV_STR_SET_ANIMA,    NV_STR_GROUP_PERSONAL, cat_anima},
    {LV_SYMBOL_KEYBOARD, NV_STR_SET_LANGUAGE, NV_STR_GROUP_PERSONAL, cat_language},
    {LV_SYMBOL_EYE_OPEN, NV_STR_SET_ACCESS,   NV_STR_GROUP_PERSONAL, cat_access},
    {LV_SYMBOL_DOWNLOAD, NV_STR_SET_UPDATE,   NV_STR_GROUP_SYSTEM,   cat_update},
    {LV_SYMBOL_SAVE,     NV_STR_SET_BACKUP,   NV_STR_GROUP_SYSTEM,   cat_backup},
    {LV_SYMBOL_EYE_CLOSE, NV_STR_SET_SECURITY, NV_STR_GROUP_SYSTEM,  cat_security},
    {LV_SYMBOL_SETTINGS, NV_STR_SET_ABOUT,    NV_STR_GROUP_SYSTEM,   cat_about},
};
constexpr int kCatN = sizeof(kCats) / sizeof(kCats[0]);

int        s_sel = 0;                 // remembered across opens within a boot
lv_obj_t  *s_rail   = nullptr;
lv_obj_t  *s_detail = nullptr;
bool       s_sel_pending = false;     // a deferred selection apply is queued
int        s_sel_next = 0;

// Live one-line status under each rail title (computed at rail build time).
void cat_subtitle(const Category &cat, char *buf, size_t n) {
    buf[0] = '\0';
    switch (cat.name_id) {
        case NV_STR_SET_NETWORK: {
            // Order mirrors lwIP's actual default route: the Wi-Fi STA netif outranks the
            // ETH netif (route_prio 100 vs 50), so show what traffic really uses.
            char ssid[33];
            if (nv_wifi_get_connected(ssid, sizeof ssid, nullptr, 0, nullptr))
                lv_snprintf(buf, n, "%s", ssid);
            else if (nv_eth_get_state() == NV_ETH_UP) {
                char ip[16];
                nv_eth_get_ip(ip, sizeof ip);
                lv_snprintf(buf, n, "Ethernet · %s", ip);
            } else
                lv_snprintf(buf, n, "%s",
                            nv_wifi_is_enabled() ? nv_tr(NV_STR_WIFI) : nv_tr(NV_STR_WIFI_OFF));
            break;
        }
        case NV_STR_SET_DISPLAY:
            lv_snprintf(buf, n, "%s · %s",
                        nv_tr(nv_theme_get_mode() == NV_THEME_DARK ? NV_STR_DARK : NV_STR_LIGHT),
                        nv_tr(accent_name_id(nv_theme_get_accent())));
            break;
        case NV_STR_SET_SOUND:
            if (nv_config_get_bool("mute", false))
                lv_snprintf(buf, n, "%s", nv_tr(NV_STR_MUTED));
            else
                lv_snprintf(buf, n, "%s %d%%", nv_tr(NV_STR_VOLUME),
                            nv_config_get_int("volume", 60));
            break;
        case NV_STR_SET_DATETIME: {
            char t[24];
            nv_time_format(t, sizeof t, nv_time_is_24h() ? "%H:%M" : "%I:%M %p");
            lv_snprintf(buf, n, "%s", t);
            break;
        }
        case NV_STR_SET_STORAGE: {
            uint64_t tot = 0, freeb = 0;
            if (nv_sd_is_mounted() && nv_sd_info(&tot, &freeb))
                lv_snprintf(buf, n, nv_tr(NV_STR_MB_FREE), (unsigned)(freeb / (1024 * 1024)));
            else
                lv_snprintf(buf, n, "%s", nv_tr(NV_STR_SD_MISSING));
            break;
        }
        case NV_STR_SET_MEMORY:
            lv_snprintf(buf, n, nv_tr(NV_STR_KB_FREE), (unsigned)(nv_mem_free_internal() / 1024));
            break;
        case NV_STR_NOTIFICATIONS: {
            const int nn = nv_notify_count();
            if (nn > 0) lv_snprintf(buf, n, "%d", nn);
            break;
        }
        case NV_STR_SET_SENSORS: {
            float tc;
            if (nv_hal_temp_read(&tc)) {
                const int t10 = (int)(tc * 10.0f + (tc >= 0 ? 0.5f : -0.5f));
                const int a = t10 < 0 ? -t10 : t10;
                lv_snprintf(buf, n, "%s%d.%d °C", t10 < 0 ? "-" : "", a / 10, a % 10);
            }
            break;
        }
        case NV_STR_SET_ACCESS:
            lv_snprintf(buf, n, "%s", nv_tr(nv_theme_get_font_scale() == NV_FONT_NORMAL
                                             ? NV_STR_FONT_NORMAL : NV_STR_FONT_LARGE));
            break;
        case NV_STR_SET_ANIMA:
            lv_snprintf(buf, n, "%s", nv_tr(NV_STR_COMING_SOON));
            break;
        case NV_STR_SET_LANGUAGE:
            lv_snprintf(buf, n, "%s", nv_lang_native_name(nv_i18n_get_lang()));
            break;
        case NV_STR_SET_UPDATE:
            lv_snprintf(buf, n, "v%s", nv_ota_running_version());
            break;
        case NV_STR_SET_BACKUP:
            if (!nv_sd_is_mounted())            lv_snprintf(buf, n, "%s", nv_tr(NV_STR_SD_MISSING));
            else if (nv_backup_available())     lv_snprintf(buf, n, "%s", nv_tr(NV_STR_SAVED));
            break;
        case NV_STR_SET_ABOUT:
            lv_snprintf(buf, n, "NucleoOS v%s", nv_ota_running_version());
            break;
        default: break;
    }
}

void build_rail(void);

// The detail panel = a fixed page header (icon badge + title, hairline underline) above the
// category's scrollable content. Gives every page a clear identity; pages build into `content`
// exactly as before (they still call nv_kit_scroll_column on it).
void build_detail(void) {
    if (!s_detail) return;
    lv_obj_clean(s_detail);          // fires the previous page's LV_EVENT_DELETE cleanups
    const NvTheme *th = nv_theme_get();
    const Category &cat = kCats[s_sel];
    lv_obj_set_flex_flow(s_detail, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *hdr = lv_obj_create(s_detail);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(hdr, NV_SP_4, 0);
    lv_obj_set_style_pad_column(hdr, NV_SP_3, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_set_style_border_color(hdr, th->divider, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *badge = lv_obj_create(hdr);
    lv_obj_remove_style_all(badge);
    no_click(badge);
    lv_obj_set_size(badge, 40, 40);
    lv_obj_set_style_radius(badge, NV_RAD_SM - 2, 0);
    lv_obj_set_style_bg_color(badge, th->accent, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bic = lv_label_create(badge);
    lv_label_set_text(bic, cat.symbol);
    lv_obj_set_style_text_color(bic, th->accent, 0);
    lv_obj_center(bic);

    lv_obj_t *ttl = lv_label_create(hdr);
    lv_label_set_text(ttl, nv_tr(cat.name_id));
    lv_obj_set_style_text_font(ttl, &nv_font_28, 0);
    lv_obj_set_style_text_color(ttl, th->text_strong, 0);

    lv_obj_t *content = lv_obj_create(s_detail);   // page target (flex-grows below the header)
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    cat.build(content);
}

void sel_apply_async(void *) {
    s_sel_pending = false;
    if (!s_rail || !s_detail) return;   // app closed / rebuilt while queued
    s_sel = s_sel_next;
    build_rail();                        // refresh selection highlight + subtitles
    build_detail();                      // header + page (cleans s_detail -> prior page cleanups)
}
void rail_row_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == s_sel || s_sel_pending) return;
    s_sel_next = i;
    // The rebuild deletes the clicked row itself — defer until this event unwinds.
    if (lv_async_call(sel_apply_async, nullptr) == LV_RESULT_OK) s_sel_pending = true;
}

void build_rail(void) {
    if (!s_rail) return;
    lv_obj_clean(s_rail);
    const NvTheme *th = nv_theme_get();

    nv_str_id_t last_group = NV_STR_COUNT;
    for (int i = 0; i < kCatN; i++) {
        const Category &cat = kCats[i];

        if (cat.group_id != last_group) {   // section header on group change
            last_group = cat.group_id;
            lv_obj_t *g = lv_label_create(s_rail);
            lv_label_set_text(g, nv_tr(cat.group_id));
            lv_obj_set_style_text_font(g, &nv_font_14, 0);
            lv_obj_set_style_text_color(g, th->text_dim, 0);
            lv_obj_set_style_pad_top(g, i == 0 ? 0 : NV_SP_3, 0);
            lv_obj_set_style_pad_left(g, NV_SP_2, 0);
        }

        const bool sel = (i == s_sel);
        lv_obj_t *row = lv_obj_create(s_rail);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, 60, 0);
        lv_obj_set_style_radius(row, NV_RAD_SM, 0);
        lv_obj_set_style_pad_hor(row, NV_SP_3, 0);
        lv_obj_set_style_pad_ver(row, NV_SP_2, 0);
        lv_obj_set_style_pad_column(row, NV_SP_3, 0);
        lv_obj_set_style_bg_color(row, sel ? th->accent : th->surface, 0);
        lv_obj_set_style_bg_opa(row, sel ? LV_OPA_20 : LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, th->surface2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);   // accent bar on the selected row
        lv_obj_set_style_border_width(row, sel ? 4 : 0, 0);
        lv_obj_set_style_border_color(row, th->accent, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, rail_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // Icon badge: accent-tinted rounded square, accent glyph.
        lv_obj_t *badge = lv_obj_create(row);
        lv_obj_remove_style_all(badge);
        no_click(badge);   // decoration — the tap belongs to the row
        lv_obj_set_size(badge, 40, 40);
        lv_obj_set_style_radius(badge, NV_RAD_SM - 2, 0);
        lv_obj_set_style_bg_color(badge, th->accent, 0);
        lv_obj_set_style_bg_opa(badge, sel ? LV_OPA_COVER : LV_OPA_20, 0);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *ic = lv_label_create(badge);
        lv_label_set_text(ic, cat.symbol);
        lv_obj_set_style_text_color(ic, sel ? th->on_primary : th->accent, 0);
        lv_obj_center(ic);

        // Title + live subtitle. no_click is CRITICAL here: this container flex-grows over
        // most of the row and (LVGL default) would swallow the tap, leaving only the thin
        // padding strips clickable — the "rail taps barely work" bug.
        lv_obj_t *txt = lv_obj_create(row);
        lv_obj_remove_style_all(txt);
        no_click(txt);
        lv_obj_set_flex_grow(txt, 1);
        lv_obj_set_height(txt, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(txt, 2, 0);
        lv_obj_clear_flag(txt, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *nm = lv_label_create(txt);
        lv_label_set_text(nm, nv_tr(cat.name_id));
        lv_obj_set_style_text_color(nm, sel ? th->text_strong : th->text, 0);
        lv_obj_set_width(nm, lv_pct(100));
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        char sub[48];
        cat_subtitle(cat, sub, sizeof sub);
        if (sub[0]) {
            lv_obj_t *sb = lv_label_create(txt);
            lv_label_set_text(sb, sub);
            lv_obj_set_style_text_font(sb, &nv_font_14, 0);
            lv_obj_set_style_text_color(sb, th->text_dim, 0);
            lv_obj_set_width(sb, lv_pct(100));
            lv_label_set_long_mode(sb, LV_LABEL_LONG_DOT);
        }
    }
}

void split_deleted(lv_event_t *) {
    // App close or SystemUI rebuild: drop the container refs; a queued sel_apply_async bails out.
    s_rail = nullptr;
    s_detail = nullptr;
}

void settings_build(lv_obj_t *content) {
    nv_ui_set_back(nullptr);   // split view has no page stack — Back always closes the app
    const NvTheme *th = nv_theme_get();

    lv_obj_t *root = lv_obj_create(content);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, split_deleted, LV_EVENT_DELETE, nullptr);

    // Left: category rail (scrolls independently).
    s_rail = lv_obj_create(root);
    lv_obj_remove_style_all(s_rail);
    lv_obj_set_size(s_rail, 336, lv_pct(100));
    lv_obj_set_style_pad_all(s_rail, NV_SP_3, 0);
    lv_obj_set_style_pad_row(s_rail, 6, 0);
    lv_obj_set_flex_flow(s_rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_rail, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_rail, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(s_rail, th->text_dim, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_rail, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_rail, 4, LV_PART_SCROLLBAR);

    // Hairline divider between rail and detail.
    lv_obj_t *div = lv_obj_create(root);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 1, lv_pct(100));
    lv_obj_set_style_bg_color(div, th->divider, 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);

    // Right: the active category's page.
    s_detail = lv_obj_create(root);
    lv_obj_remove_style_all(s_detail);
    lv_obj_set_flex_grow(s_detail, 1);
    lv_obj_set_height(s_detail, lv_pct(100));
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);

    if (s_sel < 0 || s_sel >= kCatN) s_sel = 0;
    build_rail();
    build_detail();   // header + page
}

const NvApp kSettingsApp = {"settings", "Settings", &nv_icon_settings, 1u << 20, settings_build,
                            NV_STR_APP_SETTINGS, nullptr};

}  // namespace

void settings_app_register(void) { nv_app_register(&kSettingsApp); }
