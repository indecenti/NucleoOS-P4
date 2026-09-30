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
#include "nv_mem_attr.h"   // NV_PSRAM_BSS
#include "nv_time.h"
#include "nv_service_mgr.h"
#include "nv_memory_broker.h"
#include "nv_hal.h"
#include "nv_audio.h"
#include "nv_wifi.h"
#include "nv_eth.h"
#include "nv_sd.h"
#include "nv_usb_storage.h"   // USB drives section (Storage page)
#include "nv_bgwork.h"
#include "esp_lvgl_port.h"    // lvgl_port_lock: eject/format results come back from the bg worker
#include "nv_ota.h"
#include "nv_auth.h"          // Security page: paired web clients
#include "nv_seclog.h"        // Security page: recent security events
#include "nv_appstore.h"   // remote WASM app store: editable base URL lives on this page
#include "nv_telemetry.h" // the statistics consent switch (Security page)
#include "nv_apps.h"      // nv_setup_run_again (About page)
#include "nv_backup.h"
#include "nv_ui.h"        // nv_ui_toast
#include "nv_notify.h"    // notifications page (count / clear)
#include "nv_open.h"      // file associations (Default apps page)
#include "nv_wallpaper.h" // custom launcher wallpaper (Display page)
#include "nv_bt.h"        // Bluetooth page (BLE devices, pads, keyboards, mice)
#include "nv_pad.h"       // connected controllers (USB HID / XInput / BLE) + tester
#include "nv_mqtt.h"      // Home page (Home Assistant over MQTT)
#include "nv_wasm.h"      // Security page: app permissions (scan + revocations)
#include "esp_system.h"   // esp_restart (restore / factory reset / About)
#include "driver/i2c_master.h"  // I2C bus scan (Sensors page)

#include <cstdint>  // intptr_t (enum <-> user_data packing)
#include <cstdlib>  // atoi (Home page port field)
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

// Rebuild a page in place: `col` is the page's scroll column, `build` its category builder.
// Keeps the scroll offset so a change deep in a long list doesn't jump back to the top.
// Deletes `col` (and whatever widget fired the change) — call it only from a deferred callback.
void page_rebuild(lv_obj_t *col, void (*build)(lv_obj_t *content)) {
    lv_obj_t *content = lv_obj_get_parent(col);
    const int32_t y = lv_obj_get_scroll_y(col);
    lv_obj_clean(content);                       // fires the page's LV_EVENT_DELETE cleanup
    build(content);
    lv_obj_t *nc = lv_obj_get_child(content, 0);
    if (nc) {
        lv_obj_update_layout(nc);                // content height known before the scroll clamp
        lv_obj_scroll_to_y(nc, y, LV_ANIM_OFF);
    }
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
// KeyDeck starts/stops live on this key (nv_keydeck follows NV_EV_SETTINGS_CHANGED).
// ---- App permissions (Security page): revoke what an installed app's manifest asked for.
// Each switch carries an index into s_prow (app id + permission bit); the page is rebuilt from
// scratch on every open, so the table only has to live as long as the page.
struct PermRow { char id[32]; uint32_t bit; };
constexpr int kPermRows = 96;
NV_PSRAM_BSS PermRow s_prow[kPermRows];
int s_prow_n = 0;

nv_str_id_t perm_desc_id(uint32_t bit) {
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

void app_perm_cb(lv_event_t *e) {
    lv_obj_t *sw = lv_event_get_target_obj(e);
    const int i = (int)(intptr_t)lv_obj_get_user_data(sw);
    if (i < 0 || i >= s_prow_n) return;
    uint32_t rev = nv_wasm_perm_revoked(s_prow[i].id);
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) rev &= ~s_prow[i].bit;
    else                                        rev |= s_prow[i].bit;
    nv_wasm_perm_set_revoked(s_prow[i].id, rev);   // applies from the app's next start
}

lv_obj_t *s_sec_apps_val = nullptr;   // Security page "Store apps" status value (null when not shown)

const char *store_apps_status(bool unsigned_ok) {
    return nv_tr(unsigned_ok ? NV_STR_SEC_APPS_DEV : NV_STR_SEC_APPS_VAL);
}

void store_unsigned_cb(lv_event_t *e) {
    const bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    nv_config_set_bool("store_unsigned", on);
    if (s_sec_apps_val) lv_label_set_text(s_sec_apps_val, store_apps_status(on));
}

void app_perms_section(lv_obj_t *c) {
    section_label(c, nv_tr(NV_STR_PERM_APPS));
    s_prow_n = 0;
    constexpr int kMaxApps = 64;
    auto *apps = (nv_wasm_app_t *)heap_caps_malloc(kMaxApps * sizeof(nv_wasm_app_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const int n = (apps && nv_sd_is_mounted()) ? nv_wasm_scan(apps, kMaxApps) : 0;
    for (int a = 0; a < n; a++) {
        const uint32_t sens = apps[a].perms & NV_WPERM_SENSITIVE;
        if (!sens) continue;
        const uint32_t rev = nv_wasm_perm_revoked(apps[a].id);
        lv_obj_t *t = lv_label_create(c);
        lv_label_set_text(t, apps[a].name[0] ? apps[a].name : apps[a].id);
        lv_obj_set_style_text_color(t, nv_theme_get()->text_strong, 0);
        for (int b = 0; b < 32 && s_prow_n < kPermRows; b++) {
            const uint32_t bit = 1u << b;
            const nv_str_id_t d = perm_desc_id(bit);
            if (!(sens & bit) || d == NV_STR_COUNT) continue;
            snprintf(s_prow[s_prow_n].id, sizeof s_prow[0].id, "%s", apps[a].id);
            s_prow[s_prow_n].bit = bit;
            lv_obj_t *sw = nv_kit_switch_row(c, nv_tr(d), !(rev & bit), app_perm_cb);
            lv_obj_set_user_data(sw, (void *)(intptr_t)s_prow_n);
            s_prow_n++;
        }
    }
    free(apps);
    if (!s_prow_n) lv_label_set_text(nv_kit_info(c), nv_tr(NV_STR_PERM_APPS_NONE));
    nv_kit_switch_row(c, nv_tr(NV_STR_STORE_UNSIGNED), nv_config_get_bool("store_unsigned", false),
                      store_unsigned_cb);
}

void keydeck_en_cb(lv_event_t *e) {
    nv_config_set_bool("keydeck_en", lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}
void store_stats_cb(lv_event_t *e) {
    nv_telemetry_set_consent(lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}
void rmpin_cb(lv_event_t *e) {
    nv_config_set_str("lockpin", "");        // clear the PIN (idle lock, if on, degrades to swipe)
    lv_obj_add_flag(lv_event_get_target_obj(e), LV_OBJ_FLAG_HIDDEN);   // no stale button (no rebuild)
    nv_ui_toast(nv_tr(NV_STR_REMOVE_PIN));
}

// Wallpaper row: Remove drops the custom photo, then the page rebuilds to show the default.
// Deferred — the rebuild deletes the very button that fired; the flag coalesces a double tap.
lv_obj_t *s_disp_col     = nullptr;   // Display page scroll column (null when not shown)
bool      s_disp_pending = false;     // a deferred page rebuild is queued

void cat_display(lv_obj_t *content);
void disp_apply_async(void *) {
    s_disp_pending = false;
    if (s_disp_col) page_rebuild(s_disp_col, cat_display);   // page closed while queued: no-op
}
void wallpaper_remove_cb(lv_event_t *) {
    if (s_disp_pending || !s_disp_col) return;
    nv_wallpaper_clear();
    if (lv_async_call(disp_apply_async, nullptr) == LV_RESULT_OK) s_disp_pending = true;
}
void disp_page_deleted(lv_event_t *) {
    lv_async_call_cancel(disp_apply_async, nullptr);   // never rebuild a page that is gone
    s_disp_pending = false;
    s_disp_col = nullptr;
}

void cat_display(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    s_disp_col = c;
    lv_obj_add_event_cb(c, disp_page_deleted, LV_EVENT_DELETE, nullptr);
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

    // Wallpaper: a custom photo (set via "Set as wallpaper" in Files / Gallery) can be removed
    // here; without one the launcher shows the theme gradient.
    lv_obj_t *wp = nv_kit_row(c, nv_tr(NV_STR_WALLPAPER));
    if (nv_wallpaper_is_set()) {
        lv_obj_set_style_pad_ver(wp, NV_SP_2, 0);   // compact: the button sets the row height
        lv_obj_t *rm = nv_kit_button(wp, nv_tr(NV_STR_WALLPAPER_REMOVE), false);
        lv_obj_add_event_cb(rm, wallpaper_remove_cb, LV_EVENT_CLICKED, nullptr);
    } else {
        lv_obj_t *d = lv_label_create(wp);
        lv_label_set_text(d, nv_tr(NV_STR_WALLPAPER_DEFAULT));
        lv_obj_set_style_text_color(d, nv_theme_get()->text_dim, 0);
    }

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
    for (int i = 0; i < tzn; i++) {
        // Bounded append: lv_snprintf returns the length it WANTED, so a plain `off +=` runs past
        // the buffer once the tz table outgrows 512 B and the next size wraps (OOB write).
        const int w = lv_snprintf(opts + off, sizeof(opts) - off, "%s%s",
                                  nv_time_tz_name(i), i < tzn - 1 ? "\n" : "");
        if (w < 0 || w >= (int)sizeof(opts) - off) { off = (int)sizeof(opts) - 1; break; }
        off += w;
    }
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
uint32_t   s_sto_usb_gen = 0;
int        s_fmt_armed = -1;        // slot*2 + exfat of the format button waiting for its 2nd tap
bool       s_sto_busy  = false;     // an eject/format job is running (buttons disabled)

void sto_build_body(void);

void sto_fmt_bytes(uint64_t b, char *out, size_t n) {
    if (b >= (1ull << 30)) lv_snprintf(out, n, "%u.%u GB", (unsigned)(b >> 30), (unsigned)(((b >> 20) & 1023) * 10 >> 10));
    else lv_snprintf(out, n, "%u MB", (unsigned)(b >> 20));
}

// Eject / format run on the bg worker (SCSI + mkfs take seconds); the toast comes back locked.
struct StoJob { int slot; int op; char label[12]; };   // op: 0 eject, 1 FAT32, 2 exFAT
void sto_job(void *arg) {
    StoJob *j = (StoJob *)arg;
    const bool ok = j->op == 0 ? nv_usb_storage_eject(j->slot)
                               : nv_usb_storage_format(j->slot, j->op == 2, j->label);   // keeps the old label
    if (lvgl_port_lock(3000)) {
        s_sto_busy = false;
        if (j->op == 0) nv_ui_toast(nv_tr(ok ? NV_STR_USB_EJECTED : NV_STR_EJECT_BUSY));
        else            nv_ui_toast(nv_tr(ok ? NV_STR_FORMAT_DONE : NV_STR_FORMAT_FAILED));
        if (s_sto_col) sto_build_body();
        lvgl_port_unlock();
    }
    free(j);
}
void sto_submit(int slot, int op) {
    StoJob *j = (StoJob *)calloc(1, sizeof *j);
    if (!j) return;
    j->slot = slot;
    j->op = op;
    nv_usb_stor_info_t v;
    if (nv_usb_storage_get(slot, &v)) lv_snprintf(j->label, sizeof j->label, "%s", v.label);
    if (!nv_bgwork_submit(sto_job, j)) { free(j); return; }
    s_sto_busy = true;
    if (op) nv_ui_toast(nv_tr(NV_STR_FORMAT_BUSY));
    sto_build_body();
}
void sto_eject_cb(lv_event_t *e) {
    if (!s_sto_busy) sto_submit((int)(intptr_t)lv_event_get_user_data(e), 0);
}
void sto_format_cb(lv_event_t *e) {
    if (s_sto_busy) return;
    const int key = (int)(intptr_t)lv_event_get_user_data(e);   // slot*2 + exfat
    if (s_fmt_armed != key) {                                    // first tap arms (destructive)
        s_fmt_armed = key;
        lv_label_set_text(lv_obj_get_child(lv_event_get_target_obj(e), 0), nv_tr(NV_STR_FORMAT_CONFIRM));
        return;
    }
    s_fmt_armed = -1;
    sto_submit(key / 2, 1 + (key & 1));
}
void sto_usb_accessories_cb(lv_event_t *) {
    nv_config_set_bool("usbhost", true);   // host personality of the OTG port applies at boot
    esp_restart();
}

const char *usb_class_name(uint8_t c) {
    switch (c) {
        case 0x01: return "Audio";
        case 0x02: case 0x0A: return "CDC";
        case 0x03: return "HID";
        case 0x06: return "Image";
        case 0x07: return "Printer";
        case 0x08: return "Storage";
        case 0x09: return "Hub";
        case 0x0E: return "Video";
        case 0x11: return "Billboard";
        case 0xE0: return "Wireless";
        case 0xFF: return "Vendor";
        default:   return nullptr;
    }
}

// USB drives + the raw bus (hub tree): what is attached and why something may not work.
void sto_usb_section(void) {
    const NvTheme *th = nv_theme_get();
    section_label(s_sto_col, nv_tr(NV_STR_STORAGE_USB));
    if (!nv_config_get_bool("usbhost", true)) {   // OTG port is the PC second-screen device
        lv_obj_t *w = nv_kit_info(s_sto_col);
        lv_label_set_text(w, nv_tr(NV_STR_USB_PC_MODE));
        lv_obj_set_style_text_color(w, th->text_dim, 0);
        lv_obj_t *b = nv_kit_button(s_sto_col, nv_tr(NV_STR_USB_TO_ACCESSORIES), true);
        lv_obj_add_event_cb(b, sto_usb_accessories_cb, LV_EVENT_CLICKED, nullptr);
        return;
    }
    nv_usb_stor_info_t v[NV_USB_STOR_SLOTS];
    const int n = nv_usb_storage_list(v, NV_USB_STOR_SLOTS);
    for (int i = 0; i < n; i++) {
        const int slot = nv_usb_storage_slot_of(v[i].path);
        lv_obj_t *card = surface_card(s_sto_col);
        char name[48];
        if (v[i].label[0]) lv_snprintf(name, sizeof name, "%s", v[i].label);
        else if (v[i].product[0]) lv_snprintf(name, sizeof name, "%s", v[i].product);
        else lv_snprintf(name, sizeof name, "%s", nv_tr(NV_STR_USB_DRIVE));
        lv_obj_t *t = lv_label_create(card);
        lv_label_set_text_fmt(t, "%s  %s   %s", v[i].removable ? LV_SYMBOL_SD_CARD : LV_SYMBOL_USB, name, v[i].path);
        lv_obj_set_style_text_color(t, th->text_strong, 0);
        char line[160], a[16], b[16];
        const bool mounted = v[i].state == NV_USB_STOR_MOUNTED;
        if (mounted && v[i].free_bytes != UINT64_MAX && v[i].total_bytes) {
            usage_bar(card, (int)(100 - (v[i].free_bytes * 100) / v[i].total_bytes));
            sto_fmt_bytes(v[i].free_bytes, a, sizeof a);
            sto_fmt_bytes(v[i].total_bytes, b, sizeof b);
            char fr[64];
            lv_snprintf(fr, sizeof fr, nv_tr(NV_STR_FREE_OF_FMT), a, b);
            lv_snprintf(line, sizeof line, "%s  Â·  %s%s%s", v[i].fs, fr,
                        v[i].read_only ? "  Â·  " : "", v[i].read_only ? nv_tr(NV_STR_READ_ONLY) : "");
        } else if (mounted) {
            lv_snprintf(line, sizeof line, "%s  Â·  %s", v[i].fs, nv_tr(NV_STR_MEASURING));
        } else {
            const nv_str_id_t st = v[i].state == NV_USB_STOR_EJECTED ? NV_STR_USB_EJECTED
                                 : v[i].state == NV_USB_STOR_UNFORMATTED ? NV_STR_USB_UNFORMATTED
                                 : v[i].state == NV_USB_STOR_ERROR ? NV_STR_USB_UNREADABLE : NV_STR_USB_NO_MEDIA;
            lv_snprintf(line, sizeof line, "%s", nv_tr(st));
        }
        lv_obj_t *d = lv_label_create(card);
        lv_label_set_text(d, line);
        lv_obj_set_style_text_color(d, th->text_dim, 0);
        lv_obj_t *dv = lv_label_create(card);
        lv_label_set_text_fmt(dv, "%s %s  Â·  %04X:%04X  Â·  LUN %u  Â·  USB %s", v[i].vendor, v[i].product,
                              v[i].vid, v[i].pid, v[i].lun, v[i].speed == 2 ? "2.0 HS" : v[i].speed == 1 ? "FS" : "LS");
        lv_obj_set_style_text_font(dv, &nv_font_14, 0);
        lv_obj_set_style_text_color(dv, th->text_dim, 0);

        const bool can_fmt = !v[i].read_only && (mounted || v[i].state == NV_USB_STOR_UNFORMATTED ||
                                                  v[i].state == NV_USB_STOR_ERROR);
        if (!mounted && !can_fmt) continue;
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(row, NV_SP_2, 0);
        lv_obj_set_style_pad_row(row, NV_SP_2, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        if (mounted) {
            char eb[48];
            lv_snprintf(eb, sizeof eb, LV_SYMBOL_EJECT "  %s", nv_tr(NV_STR_EJECT));
            lv_obj_t *e = nv_kit_button(row, eb, true);
            lv_obj_add_event_cb(e, sto_eject_cb, LV_EVENT_CLICKED, (void *)(intptr_t)slot);
            if (s_sto_busy) lv_obj_add_state(e, LV_STATE_DISABLED);
        }
        if (can_fmt) {
            for (int ex = 0; ex < 2; ex++) {
                const int key = slot * 2 + ex;
                lv_obj_t *f = nv_kit_button(row, nv_tr(s_fmt_armed == key ? NV_STR_FORMAT_CONFIRM
                                                        : ex ? NV_STR_FORMAT_EXFAT : NV_STR_FORMAT_FAT32), false);
                lv_obj_add_event_cb(f, sto_format_cb, LV_EVENT_CLICKED, (void *)(intptr_t)key);
                if (s_fmt_armed == key) lv_obj_set_style_text_color(lv_obj_get_child(f, 0), th->danger, 0);
                if (s_sto_busy) lv_obj_add_state(f, LV_STATE_DISABLED);
            }
        }
    }

    // The raw bus: every device incl. hubs — explains "my keyboard behind the hub is dead" and
    // what the hub's HDMI is (a Billboard device = DisplayPort Alt Mode, impossible on the P4).
    nv_usb_bus_dev_t bus[12];
    const int nb = nv_usb_bus_list(bus, 12);
    if (nb > 0) {
        section_label(s_sto_col, nv_tr(NV_STR_USB_DEVICES));
        for (int i = 0; i < nb; i++) {
            char cls[48] = "";
            size_t l = 0;
            const char *dc = usb_class_name(bus[i].dev_class);
            if (dc) l += lv_snprintf(cls + l, sizeof cls - l, "%s", dc);
            for (int k = 0; k < 6 && bus[i].if_classes[k] != 0xFF && l < sizeof cls - 12; k++) {
                const char *ic = usb_class_name(bus[i].if_classes[k]);
                if (!ic || (dc && !strcmp(ic, dc)) || strstr(cls, ic)) continue;
                l += lv_snprintf(cls + l, sizeof cls - l, "%s%s", l ? "+" : "", ic);
            }
            char left[64], right[96];
            lv_snprintf(left, sizeof left, "%s", bus[i].product[0] ? bus[i].product : "USB");
            lv_snprintf(right, sizeof right, "%s  %04X:%04X  %s%s", cls, bus[i].vid, bus[i].pid,
                        bus[i].speed == 2 ? "HS" : bus[i].speed == 1 ? "FS" : "LS",
                        bus[i].parent_addr ? "  Â·  hub" : "");
            kv_row(s_sto_col, left, right);
            if (bus[i].parent_addr && bus[i].speed != 2 && bus[i].dev_class != 0x09) {
                lv_obj_t *w = nv_kit_info(s_sto_col);   // FS/LS behind an HS hub: no TT on the P4
                lv_label_set_text_fmt(w, "%s  %s", LV_SYMBOL_WARNING, nv_tr(NV_STR_USB_NEEDS_DIRECT));
                lv_obj_set_style_text_color(w, th->danger, 0);
            }
        }
    }
    lv_obj_t *h = nv_kit_info(s_sto_col);
    lv_label_set_text(h, nv_tr(NV_STR_USB_HINT));
    lv_obj_set_style_text_color(h, th->text_dim, 0);
}

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

    sto_usb_section();

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
    const uint32_t g = nv_sd_generation(), u = nv_usb_storage_generation();
    if (g != s_sto_gen || u != s_sto_usb_gen) {   // hot-plug: card in/out, USB drive/card swap
        s_sto_gen = g;
        s_sto_usb_gen = u;
        sto_build_body();
    }
}
void sto_page_deleted(lv_event_t *) {
    if (s_sto_timer) { lv_timer_delete(s_sto_timer); s_sto_timer = nullptr; }
    s_sto_col = nullptr;
}
void cat_storage(lv_obj_t *content) {
    s_sto_col = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(s_sto_col, sto_page_deleted, LV_EVENT_DELETE, nullptr);
    s_sto_gen = nv_sd_generation();
    s_sto_usb_gen = nv_usb_storage_generation();
    s_fmt_armed = -1;
    sto_build_body();
    s_sto_timer = lv_timer_create(sto_poll, 1000, nullptr);
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
NV_PSRAM_BSS nv_wifi_ap_t s_net_aps[24];   // snapshot backing the row click handlers (LVGL thread only)
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

// -------------------------------------------------------------- Bluetooth page
// Live page like Network: a 500 ms poll hashes nv_bt status / bonds / nv_pad slots (header) and
// the listed scan results (list), and rebuilds the body only when that changes (RSSI is left
// out). Header changes rebuild at once; list changes during a scan at most every 1.5 s, since
// every advertiser nearby is listed. Tapping a controller opens an inline tester whose own 40 ms
// timer exists only while the tester is on screen; the tester container's LV_EVENT_DELETE frees
// it (body rebuild, category switch and app close all go through it).
constexpr int kBtScanMax = 48;      // nv_bt keeps at most this many
constexpr int kBtShowMax = 20;      // rows drawn (LVGL pool): HID first, then named, then signal
constexpr int kBtPairMax = 8;
constexpr uint32_t kBtListRebuildMs = 1500;
lv_obj_t   *s_bt_col   = nullptr;   // page scroll column (body rebuilt in place)
lv_timer_t *s_bt_timer = nullptr;
bool        s_bt_pending = false;   // a deferred body rebuild is queued
uint32_t    s_bt_sig   = 0;         // header signature of what the body shows
uint32_t    s_bt_lsig  = 0;         // list signature
uint32_t    s_bt_built = 0;         // lv_tick of the last rebuild
bool        s_bt_autoscan = false;  // start one scan when the page opens and Bluetooth is ready
uint32_t    s_bt_padgen = 0;        // nv_pad generation the body was built for
NV_PSRAM_BSS nv_bt_device_t s_bt_res[kBtScanMax];    // snapshots backing the row click handlers
NV_PSRAM_BSS nv_bt_device_t s_bt_pair[kBtPairMax];   // (LVGL thread only)
NV_PSRAM_BSS nv_bt_device_t s_bt_tmp[kBtScanMax];    // poll scratch (never read by handlers)
bool        s_bt_pair_conn[kBtPairMax];
int         s_bt_resn = 0, s_bt_pairn = 0;

// Controller tester (children of the body; pointers valid only while s_pad_timer != nullptr).
constexpr int kStickD = 112, kStickDot = 22;
int         s_pad_sel   = -1;       // nv_pad index shown in the tester, -1 = closed
lv_timer_t *s_pad_timer = nullptr;
lv_obj_t   *s_pad_chip[NV_PAD_N_BUTTONS];
lv_obj_t   *s_pad_dot[2];
lv_obj_t   *s_pad_trig[2];
nv_pad_input_t s_pad_last;
bool        s_pad_force = false;    // next tick repaints every widget (tester just built)

struct PadChip { uint32_t bit; const char *label; };
const PadChip kPadChips[] = {
    {NV_PADB_A, "A"}, {NV_PADB_B, "B"}, {NV_PADB_X, "X"}, {NV_PADB_Y, "Y"},
    {NV_PADB_LB, "LB"}, {NV_PADB_RB, "RB"}, {NV_PADB_LT, "LT"}, {NV_PADB_RT, "RT"},
    {NV_PADB_UP, LV_SYMBOL_UP}, {NV_PADB_DOWN, LV_SYMBOL_DOWN},
    {NV_PADB_LEFT, LV_SYMBOL_LEFT}, {NV_PADB_RIGHT, LV_SYMBOL_RIGHT},
    {NV_PADB_BACK, "Back"}, {NV_PADB_GUIDE, LV_SYMBOL_HOME}, {NV_PADB_START, "Start"},
    {NV_PADB_LSTICK, "L3"}, {NV_PADB_RSTICK, "R3"}, {NV_PADB_MISC, "Misc"},
    {NV_PADB_TOUCHPAD, "Touch"},
};
constexpr int kPadChipN = sizeof(kPadChips) / sizeof(kPadChips[0]);

void bt_build_body(void);
void pads_section(lv_obj_t *col);

void bt_apply_async(void *) { s_bt_pending = false; if (s_bt_col) bt_build_body(); }
// Every handler below fires from a widget the rebuild deletes: always defer (see net_rebuild).
void bt_rebuild(void) {
    if (s_bt_pending || !s_bt_col) return;
    if (lv_async_call(bt_apply_async, nullptr) == LV_RESULT_OK) s_bt_pending = true;
}

uint32_t fnv(uint32_t h, const void *p, size_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

// Everything the body renders except RSSI and live pad input: the header (status, bonds, pads)
// is the return value, the scan list goes to *list.
uint32_t bt_signature(uint32_t *list) {
    uint32_t h = 2166136261u;
    const bool en = nv_bt_is_enabled();
    h = fnv(h, &en, sizeof en);
    nv_bt_status_t st;
    nv_bt_status(&st);
    const uint8_t hdr[3] = {(uint8_t)st.state, st.n_connected, st.n_paired};
    h = fnv(h, hdr, sizeof hdr);
    h = fnv(h, st.error, strnlen(st.error, sizeof st.error));
    h = fnv(h, st.busy_name, strnlen(st.busy_name, sizeof st.busy_name));
    uint32_t l = 2166136261u;
    const int ns = nv_bt_scan_results(s_bt_tmp, kBtScanMax);
    l = fnv(l, &ns, sizeof ns);
    for (int i = 0; i < ns && i < kBtShowMax; i++) {
        const nv_bt_device_t &d = s_bt_tmp[i];
        l = fnv(l, d.addr, 6);
        l = fnv(l, &d.appearance, sizeof d.appearance);
        l = fnv(l, &d.company, sizeof d.company);
        const uint8_t fl[4] = {(uint8_t)d.hid, (uint8_t)d.paired, (uint8_t)d.connectable, d.mfg_type};
        l = fnv(l, fl, 4);
        l = fnv(l, d.name, strnlen(d.name, sizeof d.name));
    }
    if (list) *list = l;
    bool conn[kBtPairMax] = {};
    const int np = nv_bt_paired(s_bt_tmp, conn, kBtPairMax);
    h = fnv(h, &np, sizeof np);
    for (int i = 0; i < np; i++) {
        h = fnv(h, s_bt_tmp[i].addr, 6);
        h = fnv(h, &conn[i], 1);
        h = fnv(h, &s_bt_tmp[i].appearance, sizeof s_bt_tmp[i].appearance);
        h = fnv(h, s_bt_tmp[i].name, strnlen(s_bt_tmp[i].name, sizeof s_bt_tmp[i].name));
    }
    const uint32_t pg = nv_pad_generation();
    h = fnv(h, &pg, sizeof pg);
    const int npad = nv_pad_count();
    for (int i = 0; i < npad; i++) {
        nv_pad_info_t inf;
        if (!nv_pad_get(i, nullptr, &inf)) continue;
        const uint8_t v[2] = {inf.battery, inf.mapped};
        h = fnv(h, v, 2);
    }
    return h;
}

// ---- small drawn glyphs (the icon fonts carry no gamepad symbol)

lv_obj_t *plain_box(lv_obj_t *parent, int w, int h, lv_color_t c, int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    no_click(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// A 30x20 controller silhouette: rounded body, D-pad dot left, two face-button dots right.
void pad_glyph(lv_obj_t *parent, lv_color_t c) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *g = plain_box(parent, 30, 20, c, 9);
    lv_obj_t *d = plain_box(g, 8, 3, th->surface, 1);
    lv_obj_align(d, LV_ALIGN_LEFT_MID, 5, 0);
    lv_obj_t *d2 = plain_box(g, 3, 8, th->surface, 1);
    lv_obj_align(d2, LV_ALIGN_LEFT_MID, 7, 0);
    lv_obj_t *b1 = plain_box(g, 4, 4, th->surface, LV_RADIUS_CIRCLE);
    lv_obj_align(b1, LV_ALIGN_RIGHT_MID, -5, -3);
    lv_obj_t *b2 = plain_box(g, 4, 4, th->surface, LV_RADIUS_CIRCLE);
    lv_obj_align(b2, LV_ALIGN_RIGHT_MID, -9, 3);
}

// Leading icon slot (fixed width so names line up): a drawn glyph (pad, mouse, laptop, watch:
// the icon fonts have none) or a font symbol, per device kind.
void dev_icon(lv_obj_t *row, nv_bt_kind_t kind, lv_color_t c) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *slot = lv_obj_create(row);
    lv_obj_remove_style_all(slot);
    no_click(slot);
    lv_obj_set_size(slot, 34, 24);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
    switch (kind) {
        case NV_BT_KIND_GAMEPAD:
            pad_glyph(slot, c);
            lv_obj_center(lv_obj_get_child(slot, 0));
            return;
        case NV_BT_KIND_MOUSE: {
            lv_obj_t *m = plain_box(slot, 16, 24, c, 8);
            lv_obj_center(m);
            lv_obj_align(plain_box(m, 2, 7, th->surface, 1), LV_ALIGN_TOP_MID, 0, 3);   // wheel
            return;
        }
        case NV_BT_KIND_COMPUTER: {
            lv_obj_t *scr = plain_box(slot, 24, 16, c, 2);
            lv_obj_align(scr, LV_ALIGN_TOP_MID, 0, 2);
            lv_obj_center(plain_box(scr, 20, 12, th->surface, 1));
            lv_obj_align(plain_box(slot, 32, 3, c, 1), LV_ALIGN_BOTTOM_MID, 0, -2);   // base
            return;
        }
        case NV_BT_KIND_WATCH: {
            lv_obj_center(plain_box(slot, 10, 24, c, 3));                            // strap
            lv_obj_t *face = plain_box(slot, 18, 18, c, LV_RADIUS_CIRCLE);
            lv_obj_center(face);
            lv_obj_center(plain_box(face, 12, 12, th->surface, LV_RADIUS_CIRCLE));
            return;
        }
        default: break;
    }
    const char *sym = kind == NV_BT_KIND_KEYBOARD ? LV_SYMBOL_KEYBOARD
                    : kind == NV_BT_KIND_PHONE    ? LV_SYMBOL_CALL
                    : kind == NV_BT_KIND_AUDIO    ? LV_SYMBOL_AUDIO
                    : kind == NV_BT_KIND_TV       ? LV_SYMBOL_VIDEO
                    : kind == NV_BT_KIND_TAG      ? LV_SYMBOL_GPS
                    : kind == NV_BT_KIND_SENSOR   ? LV_SYMBOL_TINT
                    : LV_SYMBOL_BLUETOOTH;
    lv_obj_t *l = lv_label_create(slot);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_center(l);
}

// 4 signal bars, filled up to the RSSI level.
void rssi_bars(lv_obj_t *parent, int8_t rssi) {
    const NvTheme *th = nv_theme_get();
    const int lvl = rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : 1;
    lv_obj_t *w = lv_obj_create(parent);
    lv_obj_remove_style_all(w);
    no_click(w);
    lv_obj_set_size(w, LV_SIZE_CONTENT, 18);
    lv_obj_set_flex_flow(w, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(w, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(w, 3, 0);
    lv_obj_clear_flag(w, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; i++)
        plain_box(w, 4, 6 + i * 4, i < lvl ? signal_color(rssi) : th->surface3, 1);
}

// "[icon] name / caption ........ trailing" list row; add trailing widgets to the returned
// cluster. Same card look as nv_kit_row (surface, radius SM, >= 56 px tall, pressed lift).
lv_obj_t *dev_row(lv_obj_t *col, bool clickable, nv_bt_kind_t kind, lv_color_t ic,
                  const char *name, const char *caption, lv_obj_t **row_out) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *r = lv_obj_create(col);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, 60, 0);
    lv_obj_set_style_bg_color(r, th->surface, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, th->surface2, LV_STATE_PRESSED);
    lv_obj_set_style_radius(r, NV_RAD_SM, 0);
    lv_obj_set_style_pad_hor(r, NV_SP_4, 0);
    lv_obj_set_style_pad_ver(r, NV_SP_2, 0);
    lv_obj_set_style_pad_column(r, NV_SP_3, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    if (clickable) lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    else no_click(r);

    dev_icon(r, kind, ic);

    lv_obj_t *txt = lv_obj_create(r);
    lv_obj_remove_style_all(txt);
    no_click(txt);   // flex-grows over the row: must not swallow its tap
    lv_obj_set_flex_grow(txt, 1);
    lv_obj_set_height(txt, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(txt, 2, 0);
    lv_obj_clear_flag(txt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *nm = lv_label_create(txt);
    lv_label_set_text(nm, name);
    lv_obj_set_style_text_color(nm, th->text_strong, 0);
    lv_obj_set_width(nm, lv_pct(100));
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    if (caption && caption[0]) {
        lv_obj_t *cp = lv_label_create(txt);
        lv_label_set_text(cp, caption);
        lv_obj_set_style_text_font(cp, &nv_font_14, 0);
        lv_obj_set_style_text_color(cp, th->text_dim, 0);
        lv_obj_set_width(cp, lv_pct(100));
        lv_label_set_long_mode(cp, LV_LABEL_LONG_DOT);
    }

    lv_obj_t *tr = lv_obj_create(r);
    lv_obj_remove_style_all(tr);
    lv_obj_set_size(tr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tr, NV_SP_3, 0);
    lv_obj_clear_flag(tr, LV_OBJ_FLAG_SCROLLABLE);
    no_click(tr);   // a button added to it stays clickable itself
    if (row_out) *row_out = r;
    return tr;
}

void dim_label(lv_obj_t *parent, const char *text, lv_color_t c) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &nv_font_14, 0);
    lv_obj_set_style_text_color(l, c, 0);
}

static_assert((int)NV_STR_BT_KIND_SENSOR - (int)NV_STR_BT_KIND_UNKNOWN == (int)NV_BT_KIND_SENSOR,
              "kind strings follow nv_bt_kind_t");
const char *bt_kind_label(nv_bt_kind_t k) {
    const int i = (int)k < (int)NV_BT_KIND_COUNT ? (int)k : 0;
    return nv_tr((nv_str_id_t)((int)NV_STR_BT_KIND_UNKNOWN + i));
}

// ---- handlers

void bt_toggle_cb(lv_event_t *e) {
    const bool on = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    nv_bt_set_enabled(on);
    s_bt_autoscan = on;   // look around once it is up
    bt_rebuild();
}
void bt_scan_cb(lv_event_t *) {
    nv_bt_status_t st;
    nv_bt_status(&st);
    if (st.state == NV_BT_SCANNING) nv_bt_scan_stop();
    else nv_bt_scan_start(15);
    bt_rebuild();
}
void bt_res_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_bt_resn) return;
    const nv_bt_device_t &d = s_bt_res[i];
    if (!nv_bt_can_connect(&d)) {   // phones, laptops, tags...: only input devices connect for now
        nv_ui_toast(nv_tr(d.connectable ? NV_STR_BT_ONLY_HID : NV_STR_BT_NOT_CONNECTABLE));
        return;
    }
    nv_bt_status_t st;
    nv_bt_status(&st);
    if (st.state == NV_BT_CONNECTING || st.state == NV_BT_STARTING) return;   // one at a time
    nv_bt_connect(d.addr, d.addr_type);
    bt_rebuild();
}
void bt_forget_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_bt_pairn) return;
    nv_bt_forget(s_bt_pair[i].addr, s_bt_pair[i].addr_type);
    bt_rebuild();
}
void pad_row_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    s_pad_sel = (s_pad_sel == i) ? -1 : i;   // tap again closes the tester
    bt_rebuild();
}
void pad_rumble_cb(lv_event_t *) {
    if (s_pad_sel >= 0) nv_pad_rumble(s_pad_sel, 30000, 30000, 300);
}

// ---- controller tester

void pad_tester_stop(void) {
    if (s_pad_timer) { lv_timer_delete(s_pad_timer); s_pad_timer = nullptr; }
    for (lv_obj_t *&o : s_pad_chip) o = nullptr;
    s_pad_dot[0] = s_pad_dot[1] = nullptr;
    s_pad_trig[0] = s_pad_trig[1] = nullptr;
}
void pad_tester_deleted(lv_event_t *) { pad_tester_stop(); }

int stick_px(int16_t v) {   // -32768..32767 -> dot offset inside the ring
    constexpr int kTravel = (kStickD - kStickDot) / 2 - 4;
    return (int)v * kTravel / 32768;
}

void pad_tick(lv_timer_t *) {
    if (!s_pad_dot[0]) return;
    nv_pad_input_t in;
    if (s_pad_sel < 0 || !nv_pad_get(s_pad_sel, &in, nullptr)) return;   // poll rebuilds on detach
    const NvTheme *th = nv_theme_get();
    // Touch only what moved: a 40 ms ticker re-setting identical styles would redraw forever.
    const bool force = s_pad_force;
    s_pad_force = false;
    const uint32_t changed = force ? ~0u : (in.buttons ^ s_pad_last.buttons);
    for (int i = 0; i < kPadChipN; i++) {
        if (!(changed & kPadChips[i].bit) || !s_pad_chip[i]) continue;
        const bool on = in.buttons & kPadChips[i].bit;
        nv_kit_bg_color(s_pad_chip[i], on ? th->accent : th->surface3);
        nv_kit_text_color(lv_obj_get_child(s_pad_chip[i], 0), on ? th->on_primary : th->text);
    }
    for (int s = 0; s < 2; s++) {
        const int ax = s ? NV_PADA_RX : NV_PADA_LX, ay = s ? NV_PADA_RY : NV_PADA_LY;
        const int at = s ? NV_PADA_RT : NV_PADA_LT;
        if (force || stick_px(in.axis[ax]) != stick_px(s_pad_last.axis[ax]) ||
            stick_px(in.axis[ay]) != stick_px(s_pad_last.axis[ay]))
            lv_obj_align(s_pad_dot[s], LV_ALIGN_CENTER, stick_px(in.axis[ax]), stick_px(in.axis[ay]));
        if (force || in.axis[at] / 512 != s_pad_last.axis[at] / 512)
            lv_bar_set_value(s_pad_trig[s], in.axis[at] < 0 ? 0 : in.axis[at], LV_ANIM_OFF);
    }
    s_pad_last = in;
}

lv_obj_t *pad_stick(lv_obj_t *parent, const char *label, int idx) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    no_click(wrap);
    lv_obj_set_size(wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(wrap, NV_SP_1, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ring = plain_box(wrap, kStickD, kStickD, th->surface2, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, th->divider, 0);
    lv_obj_t *hx = plain_box(ring, kStickD - 24, 1, th->divider, 0);   // crosshair
    lv_obj_center(hx);
    lv_obj_t *hy = plain_box(ring, 1, kStickD - 24, th->divider, 0);
    lv_obj_center(hy);
    s_pad_dot[idx] = plain_box(ring, kStickDot, kStickDot, th->accent, LV_RADIUS_CIRCLE);
    lv_obj_align(s_pad_dot[idx], LV_ALIGN_CENTER, 0, 0);
    dim_label(wrap, label, th->text_dim);
    return wrap;
}

lv_obj_t *pad_trigger(lv_obj_t *parent, const char *label, int idx) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *wrap = lv_obj_create(parent);
    lv_obj_remove_style_all(wrap);
    no_click(wrap);
    lv_obj_set_size(wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(wrap, NV_SP_1, 0);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bar = lv_bar_create(wrap);
    no_click(bar);
    lv_obj_set_size(bar, 22, kStickD);   // taller than wide -> LVGL draws it vertical (bottom-up)
    lv_bar_set_range(bar, 0, 32767);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, th->surface3, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, th->accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    s_pad_trig[idx] = bar;
    dim_label(wrap, label, th->text_dim);
    return wrap;
}

// Inline tester card under the selected controller row.
void pad_tester(lv_obj_t *col, const nv_pad_info_t &inf) {
    const NvTheme *th = nv_theme_get();
    pad_tester_stop();
    lv_obj_t *card = surface_card(col);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, th->accent, 0);
    lv_obj_add_event_cb(card, pad_tester_deleted, LV_EVENT_DELETE, nullptr);

    // Button chips: light up (accent) while held.
    lv_obj_t *chips = lv_obj_create(card);
    lv_obj_remove_style_all(chips);
    no_click(chips);
    lv_obj_set_size(chips, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(chips, NV_SP_2, 0);
    lv_obj_set_style_pad_column(chips, NV_SP_2, 0);
    lv_obj_clear_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < kPadChipN; i++) {
        lv_obj_t *c = plain_box(chips, LV_SIZE_CONTENT, 40, th->surface3, NV_RAD_SM - 4);
        lv_obj_set_style_min_width(c, 48, 0);
        lv_obj_set_style_pad_hor(c, NV_SP_3, 0);
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text(l, kPadChips[i].label);
        lv_obj_set_style_text_color(l, th->text, 0);
        lv_obj_center(l);
        s_pad_chip[i] = c;
    }

    // Sticks + triggers.
    lv_obj_t *axes = pick_row(card, NV_SP_5);
    lv_obj_set_flex_align(axes, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    no_click(axes);
    pad_trigger(axes, "LT", 0);
    pad_stick(axes, "L", 0);
    pad_stick(axes, "R", 1);
    pad_trigger(axes, "RT", 1);

    if (inf.rumble) {
        char rb[48];
        lv_snprintf(rb, sizeof rb, LV_SYMBOL_BELL "  %s", nv_tr(NV_STR_PAD_RUMBLE));
        lv_obj_t *b = nv_kit_button(card, rb, false);
        lv_obj_add_event_cb(b, pad_rumble_cb, LV_EVENT_CLICKED, nullptr);
    }

    memset(&s_pad_last, 0, sizeof s_pad_last);
    s_pad_force = true;   // first tick paints everything from the live state
    s_pad_timer = lv_timer_create(pad_tick, 40, nullptr);
    pad_tick(nullptr);
}

// ---- body

const char *pad_source_label(uint8_t s) {
    switch (s) {
        case NV_PAD_SRC_USB_HID: return "USB";
        case NV_PAD_SRC_XINPUT:  return "USB XInput";
        case NV_PAD_SRC_BLE:     return "Bluetooth";
        default:                 return "?";
    }
}

void bt_status_card(lv_obj_t *col, const nv_bt_status_t &st) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *card = surface_card(col);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_t *top = pick_row(card, NV_SP_3);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    no_click(top);

    const bool busy = st.state == NV_BT_STARTING || st.state == NV_BT_SCANNING ||
                      st.state == NV_BT_CONNECTING;
    if (busy) {
        lv_obj_t *sp = lv_spinner_create(top);
        lv_obj_set_size(sp, 24, 24);
    } else {
        lv_obj_t *ic = lv_label_create(top);
        lv_label_set_text(ic, LV_SYMBOL_BLUETOOTH);
        lv_obj_set_style_text_color(ic, st.state == NV_BT_ERROR ? th->danger
                                        : st.state == NV_BT_OFF ? th->text_dim : th->accent, 0);
    }
    char buf[96];
    switch (st.state) {
        case NV_BT_OFF:        lv_snprintf(buf, sizeof buf, "%s", nv_tr(NV_STR_BT_OFF)); break;
        case NV_BT_STARTING:   lv_snprintf(buf, sizeof buf, "%s", nv_tr(NV_STR_BT_STARTING)); break;
        case NV_BT_SCANNING:   lv_snprintf(buf, sizeof buf, "%s", nv_tr(NV_STR_BT_SCANNING)); break;
        case NV_BT_CONNECTING:
            lv_snprintf(buf, sizeof buf, nv_tr(NV_STR_BT_CONNECTING_FMT),
                        st.busy_name[0] ? st.busy_name : "...");
            break;
        case NV_BT_ERROR:      lv_snprintf(buf, sizeof buf, "%s", nv_tr(NV_STR_BT_ERROR)); break;
        default:
            if (st.n_connected)
                lv_snprintf(buf, sizeof buf, nv_tr(NV_STR_BT_N_CONNECTED_FMT), (int)st.n_connected);
            else
                lv_snprintf(buf, sizeof buf, "%s", nv_tr(NV_STR_BT_READY));
            break;
    }
    lv_obj_t *t = lv_label_create(top);
    lv_label_set_text(t, buf);
    lv_obj_set_style_text_color(t, st.state == NV_BT_ERROR ? th->danger : th->text_strong, 0);
    lv_obj_set_flex_grow(t, 1);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);

    if (st.error[0]) {
        lv_obj_t *er = lv_label_create(card);
        lv_label_set_text_fmt(er, LV_SYMBOL_WARNING "  %s", st.error);
        lv_obj_set_width(er, lv_pct(100));
        lv_label_set_long_mode(er, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(er, th->danger, 0);
    }
    uint8_t own[6];
    if (nv_bt_own_addr(own)) {
        char a[18];
        nv_bt_addr_str(own, a);
        lv_obj_t *ol = lv_label_create(card);
        lv_label_set_text_fmt(ol, nv_tr(NV_STR_BT_OWN_ADDR_FMT), a);
        lv_obj_set_style_text_font(ol, &nv_font_14, 0);
        lv_obj_set_style_text_color(ol, th->text_dim, 0);
    }
    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_text(hint, nv_tr(NV_STR_BT_HINT));
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint, &nv_font_14, 0);
    lv_obj_set_style_text_color(hint, th->text_dim, 0);
}

void bt_build_body(void) {
    if (!s_bt_col) return;
    const int32_t y = lv_obj_get_scroll_y(s_bt_col);
    lv_obj_clean(s_bt_col);   // fires the tester's DELETE cleanup (timer + widget refs)
    const NvTheme *th = nv_theme_get();
    s_bt_sig = bt_signature(&s_bt_lsig);
    s_bt_built = lv_tick_get();
    const uint32_t pg = nv_pad_generation();
    if (pg != s_bt_padgen) { s_bt_padgen = pg; s_pad_sel = -1; }   // indices shifted: close tester

    nv_bt_status_t st;
    nv_bt_status(&st);
    st.error[sizeof st.error - 1] = '\0';
    st.busy_name[sizeof st.busy_name - 1] = '\0';
    const bool en = nv_bt_is_enabled();

    // -- Bluetooth
    nv_kit_switch_row(s_bt_col, nv_tr(NV_STR_BT), en, bt_toggle_cb);
    bt_status_card(s_bt_col, st);

    char nb[64], cap[112], addr[18], sa[9];
    const bool up = en && st.state != NV_BT_OFF && st.state != NV_BT_ERROR;
    if (en) {   // Search / Stop: always there while Bluetooth is on, usable once the host is up
        const bool scanning = st.state == NV_BT_SCANNING;
        lv_snprintf(nb, sizeof nb, "%s  %s", scanning ? LV_SYMBOL_STOP : LV_SYMBOL_REFRESH,
                    nv_tr(scanning ? NV_STR_BT_STOP : NV_STR_BT_SCAN));
        lv_obj_t *sb = nv_kit_button(s_bt_col, nb, !scanning);
        lv_obj_add_event_cb(sb, bt_scan_cb, LV_EVENT_CLICKED, nullptr);
        if (!up || st.state == NV_BT_STARTING) lv_obj_add_state(sb, LV_STATE_DISABLED);
    }

    // -- Paired (bond store is readable even with Bluetooth off)
    s_bt_pairn = nv_bt_paired(s_bt_pair, s_bt_pair_conn, kBtPairMax);
    if (s_bt_pairn > 0) {
        section_label(s_bt_col, nv_tr(NV_STR_BT_PAIRED));
        for (int i = 0; i < s_bt_pairn; i++) {
            nv_bt_device_t &d = s_bt_pair[i];
            d.name[sizeof d.name - 1] = '\0';
            nv_bt_addr_str(d.addr, addr);
            const bool on = s_bt_pair_conn[i];
            const nv_bt_kind_t k = nv_bt_kind(&d);
            if (on) lv_snprintf(cap, sizeof cap, "%s · %s", nv_tr(NV_STR_WIFI_CONNECTED), bt_kind_label(k));
            else    lv_snprintf(cap, sizeof cap, "%s · %s", nv_tr(NV_STR_BT_NOT_CONNECTED), addr);
            lv_obj_t *tr = dev_row(s_bt_col, false, k, on ? th->accent : th->text_dim,
                                   d.name[0] ? d.name : addr, cap, nullptr);
            if (on) plain_box(tr, 10, 10, th->success_solid, LV_RADIUS_CIRCLE);   // connected dot
            lv_obj_t *fb = nv_kit_button(tr, nv_tr(NV_STR_BT_FORGET), false);
            lv_obj_add_event_cb(fb, bt_forget_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }

    // -- Available: every advertiser nearby; tapping an input device connects it
    s_bt_resn = up ? nv_bt_scan_results(s_bt_res, kBtScanMax) : 0;
    if (up && (s_bt_resn > 0 || st.state == NV_BT_SCANNING)) {
        section_label(s_bt_col, nv_tr(NV_STR_BT_FOUND));
        const bool connecting = st.state == NV_BT_CONNECTING;
        const int shown = s_bt_resn < kBtShowMax ? s_bt_resn : kBtShowMax;
        for (int i = 0; i < shown; i++) {
            nv_bt_device_t &d = s_bt_res[i];
            d.name[sizeof d.name - 1] = '\0';
            nv_bt_addr_str(d.addr, addr);
            lv_snprintf(sa, sizeof sa, "%02x:%02x:%02x", d.addr[2], d.addr[1], d.addr[0]);
            const nv_bt_kind_t k = nv_bt_kind(&d);
            const char *co = nv_bt_company_name(d.company);
            const bool can = nv_bt_can_connect(&d);
            // Name: advertised name, else what it is, else its maker, else "Unknown".
            const char *name = d.name[0] ? d.name : k != NV_BT_KIND_UNKNOWN ? bt_kind_label(k)
                             : co ? co : bt_kind_label(NV_BT_KIND_UNKNOWN);
            // Caption: kind · maker · short address · connectable · paired (what the name didn't say).
            size_t n = 0;
            cap[0] = '\0';
            auto part = [&](const char *t) {
                if (t && t[0] && n < sizeof cap)
                    n += lv_snprintf(cap + n, sizeof cap - n, "%s%s", n ? " · " : "", t);
            };
            if (d.name[0] && k != NV_BT_KIND_UNKNOWN) part(bt_kind_label(k));
            if (co && name != co) part(co);
            part(sa);
            if (can) part(nv_tr(NV_STR_BT_CONNECTABLE));
            if (d.paired) part(nv_tr(NV_STR_BT_IS_PAIRED));
            lv_obj_t *row = nullptr;
            lv_obj_t *tr = dev_row(s_bt_col, !connecting, k, can ? th->accent : th->text_dim, name, cap, &row);
            if (connecting && st.busy_name[0] && (!strcmp(d.name, st.busy_name) || !strcmp(addr, st.busy_name))) {
                lv_obj_t *sp = lv_spinner_create(tr);
                lv_obj_set_size(sp, 22, 22);
            }
            rssi_bars(tr, d.rssi);
            if (!connecting)
                lv_obj_add_event_cb(row, bt_res_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        if (s_bt_resn > shown) {
            lv_snprintf(nb, sizeof nb, nv_tr(NV_STR_BT_MORE_FMT), s_bt_resn - shown);
            lv_label_set_text(nv_kit_info(s_bt_col), nb);
        }
        if (s_bt_resn == 0) lv_label_set_text(nv_kit_info(s_bt_col), nv_tr(NV_STR_BT_NONE_FOUND));
    }

    // -- Controllers (USB and Bluetooth, nv_pad) + tester
    pads_section(s_bt_col);

    lv_obj_update_layout(s_bt_col);   // content height known before the scroll clamp
    lv_obj_scroll_to_y(s_bt_col, y, LV_ANIM_OFF);
}

// Controllers from every transport (USB HID, XInput, BLE), the selected one with its tester.
void pads_section(lv_obj_t *col) {
    const NvTheme *th = nv_theme_get();
    char cap[64], nb[32];
    section_label(col, nv_tr(NV_STR_PADS));
    const int npad = nv_pad_count();
    if (npad <= 0) {
        lv_label_set_text(nv_kit_info(col), nv_tr(NV_STR_PADS_NONE));
        s_pad_sel = -1;
        return;
    }
    if (s_pad_sel >= npad) s_pad_sel = -1;
    for (int i = 0; i < npad; i++) {
        nv_pad_info_t inf;
        if (!nv_pad_get(i, nullptr, &inf)) continue;
        inf.name[sizeof inf.name - 1] = '\0';
        lv_snprintf(cap, sizeof cap, "P%d · %s · %04x:%04x", i + 1,
                    nv_tr(inf.mapped ? NV_STR_PAD_MAPPED : NV_STR_PAD_GENERIC), inf.vid, inf.pid);
        const bool sel = i == s_pad_sel;
        lv_obj_t *row = nullptr;
        lv_obj_t *tr = dev_row(col, true, NV_BT_KIND_GAMEPAD, sel ? th->accent : th->text,
                               inf.name[0] ? inf.name : "Gamepad", cap, &row);
        if (sel) {
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(row, 4, 0);
            lv_obj_set_style_border_color(row, th->accent, 0);
        }
        lv_obj_add_event_cb(row, pad_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (inf.battery != 255) {
            const int b = inf.battery;
            lv_snprintf(nb, sizeof nb, "%s %d%%",
                        b > 80 ? LV_SYMBOL_BATTERY_FULL : b > 55 ? LV_SYMBOL_BATTERY_3
                        : b > 30 ? LV_SYMBOL_BATTERY_2 : b > 10 ? LV_SYMBOL_BATTERY_1
                        : LV_SYMBOL_BATTERY_EMPTY, b);
            dim_label(tr, nb, b <= 10 ? th->danger : th->text_dim);
        }
        // Source badge: tinted pill.
        lv_obj_t *badge = plain_box(tr, LV_SIZE_CONTENT, LV_SIZE_CONTENT, th->accent, NV_RAD_SM - 4);
        lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
        lv_obj_set_style_pad_hor(badge, NV_SP_2, 0);
        lv_obj_set_style_pad_ver(badge, 2, 0);
        dim_label(badge, pad_source_label(inf.source), th->accent);
        if (sel) pad_tester(col, inf);
    }
    if (s_pad_sel < 0) lv_label_set_text(nv_kit_info(col), nv_tr(NV_STR_PAD_TEST_HINT));
}

// One scan when the page opens, as soon as the host is ready (also after switching it on here).
void bt_autoscan(void) {
    if (!s_bt_autoscan) return;
    nv_bt_status_t st;
    nv_bt_status(&st);
    if (st.state == NV_BT_READY) { s_bt_autoscan = false; nv_bt_scan_start(15); }
}

void bt_poll(lv_timer_t *) {
    bt_autoscan();
    uint32_t list = 0;
    const uint32_t head = bt_signature(&list);
    if (head != s_bt_sig || (list != s_bt_lsig && lv_tick_elaps(s_bt_built) >= kBtListRebuildMs))
        bt_build_body();
}

void bt_page_deleted(lv_event_t *) {
    if (s_bt_timer) { lv_timer_delete(s_bt_timer); s_bt_timer = nullptr; }
    pad_tester_stop();   // children's DELETE ran already; belt and braces
    lv_async_call_cancel(bt_apply_async, nullptr);
    s_bt_pending = false;
    s_bt_col = nullptr;
}

void cat_bluetooth(lv_obj_t *content) {
    s_bt_pending = false;
    s_pad_sel = -1;
    s_bt_padgen = nv_pad_generation();
    s_bt_autoscan = nv_bt_is_enabled();
    bt_autoscan();   // already up: the first build shows the scan running
    s_bt_col = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(s_bt_col, bt_page_deleted, LV_EVENT_DELETE, nullptr);
    bt_build_body();
    s_bt_timer = lv_timer_create(bt_poll, 500, nullptr);
}

// -------------------------------------------------------------- System update (OTA) page
// Live page like Network: a 500ms poll rebuilds the body on any nv_ota state/progress change.
lv_obj_t  *s_upd_col = nullptr;
lv_timer_t *s_upd_timer = nullptr;
uint32_t   s_upd_gen = 0;
lv_obj_t  *s_upd_ta  = nullptr;         // manifest-URL field
lv_obj_t  *s_store_ta = nullptr;        // app-store base-URL field (saved on demand)
bool       s_upd_pending = false;
NV_PSRAM_BSS char s_upd_url[256];       // retained across page opens within a boot

void upd_build_body(void);
void upd_apply_async(void *) { s_upd_pending = false; if (s_upd_col) upd_build_body(); }
void upd_rebuild(void) {
    if (s_upd_pending || !s_upd_col) return;
    if (lv_async_call(upd_apply_async, nullptr) == LV_RESULT_OK) s_upd_pending = true;
}

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

    // App store base URL — the host the Apps → Store tab installs WASM apps from. Base only
    // (GitHub Pages by default, http://<PC-IP>:8090 for a local test server); the device appends
    // /store-<lang>.json and /apps/<id>/…
    lv_obj_t *sh = lv_label_create(s_upd_col);
    lv_label_set_text(sh, "App store");
    lv_obj_set_style_text_font(sh, &nv_font_14, 0);
    lv_obj_set_style_text_color(sh, th->text_dim, 0);
    char store_url[192];
    nv_appstore_get_url(store_url, sizeof store_url);
    s_store_ta = nv_kit_textarea_ex(s_upd_col, "https://host/path", true, NV_IME_URL, NV_IME_RET_DONE);
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
        // Offline: flash a firmware the user dropped on the SD card (/sdcard/nucleos-anima.bin +
        // its signed nucleos-anima.json from the release).
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
    // Preload the saved manifest URL (or the GitHub default) so the field is ready — no retyping.
    if (!s_upd_url[0]) nv_ota_get_url(s_upd_url, sizeof(s_upd_url));
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
    {   // the first-boot wizard, on demand (language, Wi-Fi, time, PIN, statistics)
        lv_obj_t *again = nv_kit_button(c, nv_tr(NV_STR_SETUP_AGAIN), false);
        lv_obj_add_event_cb(again, [](lv_event_t *) { nv_setup_run_again(); }, LV_EVENT_CLICKED, nullptr);
    }
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
// secret-storage posture: NVS encryption is on or off per nv_config_encrypted() (off only when the
// chip could not create its eFuse key) — surfaced so a gap is visible in-product, not hidden.
// Web access rows: revoking rebuilds the page, deferred (the rebuild deletes the button that fired;
// the flag coalesces a double tap).
lv_obj_t *s_sec_col     = nullptr;   // Security page scroll column (null when not shown)
bool      s_sec_pending = false;

void cat_security(lv_obj_t *content);
void sec_apply_async(void *) {
    s_sec_pending = false;
    if (s_sec_col) page_rebuild(s_sec_col, cat_security);   // page closed while queued: no-op
}
void sec_rebuild_deferred(void) {
    if (!s_sec_pending && s_sec_col && lv_async_call(sec_apply_async, nullptr) == LV_RESULT_OK)
        s_sec_pending = true;
}
void sec_page_deleted(lv_event_t *) {
    lv_async_call_cancel(sec_apply_async, nullptr);
    s_sec_pending = false;
    s_sec_col = nullptr;
    s_sec_apps_val = nullptr;
}
void web_revoke_cb(lv_event_t *e) {
    if (s_sec_pending) return;
    nv_auth_session_revoke((int)(intptr_t)lv_event_get_user_data(e));
    sec_rebuild_deferred();
}
void web_revoke_all_cb(lv_event_t *) {
    if (s_sec_pending) return;
    nv_auth_session_revoke_all();
    sec_rebuild_deferred();
}

void web_access_section(lv_obj_t *c) {
    section_label(c, nv_tr(NV_STR_WEB_ACCESS));
    lv_obj_t *hint = lv_label_create(c);
    lv_label_set_text(hint, nv_tr(NV_STR_WEB_ACCESS_HINT));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_text_color(hint, nv_theme_get()->text_dim, 0);
    const int n = nv_auth_session_count();
    if (!n) { kv_row(c, nv_tr(NV_STR_WEB_PAIRED_NONE), ""); return; }
    for (int i = 0; i < n; i++) {
        nv_auth_session_t s;
        if (!nv_auth_session_get(i, &s)) break;
        char when[24] = "-";
        if (s.created) {
            const time_t t = (time_t)s.created;
            struct tm tmv;
            localtime_r(&t, &tmv);
            strftime(when, sizeof when, "%d/%m/%Y", &tmv);
        }
        lv_obj_t *row = nv_kit_row(c, s.name);
        lv_obj_t *d = lv_label_create(row);
        lv_label_set_text(d, when);
        lv_obj_set_style_text_color(d, nv_theme_get()->text_dim, 0);
        lv_obj_t *rb = nv_kit_button(row, nv_tr(NV_STR_WEB_REVOKE), false);
        lv_obj_add_event_cb(rb, web_revoke_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_t *all = nv_kit_button(c, nv_tr(NV_STR_WEB_REVOKE_ALL), false);
    lv_obj_add_event_cb(all, web_revoke_all_cb, LV_EVENT_CLICKED, nullptr);
}

// Newest security events first (nv_seclog, RAM, since this boot). Shown as a static list: the page
// is rebuilt whenever it is reopened, which is when an owner comes looking.
void sec_events_section(lv_obj_t *c) {
    static const nv_str_id_t kLabel[NV_SEC_EVENT_COUNT] = {
        NV_STR_SEV_PAIR_WRONG, NV_STR_SEV_PAIR_LOCKED, NV_STR_SEV_PAIR_OK, NV_STR_SEV_REVOKED,
        NV_STR_SEV_FW_REFUSED, NV_STR_SEV_APP_REFUSED, NV_STR_SEV_NVS_ENC, NV_STR_SEV_NVS_PLAIN,
        NV_STR_SEV_UNLOCK_LOCKED,
    };
    section_label(c, nv_tr(NV_STR_SEC_EVENTS));
    const int n = nv_seclog_count();
    if (!n) { lv_label_set_text(nv_kit_info(c), nv_tr(NV_STR_SEC_EVENTS_NONE)); return; }
    constexpr int kShown = 12;
    for (int i = 0; i < n && i < kShown; i++) {
        nv_sec_entry_t ev;
        if (!nv_seclog_get(i, &ev) || ev.code >= NV_SEC_EVENT_COUNT) break;
        char when[24];
        if (ev.unix_time) {
            const time_t t = (time_t)ev.unix_time;
            struct tm tmv;
            localtime_r(&t, &tmv);
            strftime(when, sizeof when, "%d/%m %H:%M", &tmv);
        } else {
            snprintf(when, sizeof when, "+%lus", (unsigned long)ev.uptime_s);   // before the clock was set
        }
        char val[NV_SECLOG_DETAIL_MAX + 32];
        snprintf(val, sizeof val, ev.detail[0] ? "%s · %s" : "%s", when, ev.detail);
        kv_row(c, nv_tr(kLabel[ev.code]), val);
    }
}

void cat_security(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    s_sec_col = c;
    lv_obj_add_event_cb(c, sec_page_deleted, LV_EVENT_DELETE, nullptr);

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

    // What the firmware enforces (always on, not settings), then the encryption posture.
    section_label(c, nv_tr(NV_STR_PROTECTIONS));
    kv_row(c, nv_tr(NV_STR_SEC_FW), nv_tr(NV_STR_SEC_FW_VAL));      // nv_ota: ECDSA manifest + sha256
    s_sec_apps_val = kv_row(c, nv_tr(NV_STR_SEC_APPS),              // nv_appstore: package.sig
                            store_apps_status(nv_config_get_bool("store_unsigned", false)));
    kv_row(c, nv_tr(NV_STR_SEC_WEB), nv_tr(NV_STR_SEC_WEB_VAL));    // nv_auth: /api + /ws pairing
    kv_row(c, nv_tr(NV_STR_ENCRYPTION), nv_tr(nv_config_encrypted() ? NV_STR_ENC_ON : NV_STR_ENC_OFF));

    web_access_section(c);

    sec_events_section(c);

    // KeyDeck: an unauthenticated LAN keyboard (Cardputer companion), so off unless wanted.
    section_label(c, nv_tr(NV_STR_KEYDECK_SECTION));
    nv_kit_switch_row(c, nv_tr(NV_STR_KEYDECK_ENABLE), nv_config_get_bool("keydeck_en", false),
                      keydeck_en_cb);

    // Statistics: the one opt-in consent (nv_telemetry) — daily anonymous report and the store's
    // install counter. Asked by the setup wizard; the notice is the store's privacy.html.
    section_label(c, nv_tr(NV_STR_SETUP_STATS_T));
    nv_kit_switch_row(c, nv_tr(NV_STR_TELEMETRY_ENABLE),
                      nv_telemetry_consent() == NV_TELEMETRY_YES, store_stats_cb);
    {
        lv_obj_t *info = nv_kit_info(c);
        lv_label_set_text(info, NV_TELEMETRY_PRIVACY_URL + 8);   // without "https://"
    }

    app_perms_section(c);
}

// -------------------------------------------------------------- Accessibility page
// -------------------------------------------------------------- Home page (Home Assistant / MQTT)
// Broker settings for nv_mqtt, saved together by the button (each nv_config_set_* bumps the
// service's config generation; it reconnects once on its next loop). The password is write-only:
// never read back into the field; an empty field on save keeps the stored one.
lv_obj_t   *s_ha_host = nullptr, *s_ha_port = nullptr, *s_ha_user = nullptr, *s_ha_pass = nullptr;
lv_obj_t   *s_ha_sw = nullptr, *s_ha_status = nullptr;
lv_timer_t *s_ha_timer = nullptr;

void ha_status_text(char *buf, size_t n) {
    char det[48];
    const nv_mqtt_state_t st = nv_mqtt_status(det, sizeof det);
    nv_str_id_t id = NV_STR_HA_ST_OFF;
    switch (st) {
        case NV_MQTT_NO_BROKER:  id = NV_STR_HA_ST_NO_BROKER; break;
        case NV_MQTT_WAIT_NET:   id = NV_STR_HA_ST_WAIT_NET; break;
        case NV_MQTT_CONNECTING: id = NV_STR_HA_ST_CONNECTING; break;
        case NV_MQTT_CONNECTED:  id = NV_STR_HA_ST_CONNECTED; break;
        case NV_MQTT_ERROR:      id = NV_STR_HA_ST_ERROR; break;
        default: break;
    }
    const bool show_det = det[0] && (st == NV_MQTT_CONNECTED || st == NV_MQTT_CONNECTING ||
                                     st == NV_MQTT_ERROR);
    if (show_det) lv_snprintf(buf, n, "%s · %s", nv_tr(id), det);
    else          lv_snprintf(buf, n, "%s", nv_tr(id));
}

void ha_tick(lv_timer_t *) {
    if (!s_ha_status) return;
    char b[96];
    ha_status_text(b, sizeof b);
    nv_kit_label_set(s_ha_status, b);
}

void ha_en_cb(lv_event_t *e) {
    nv_config_set_bool("mqtt_en", lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}

void ha_save(void) {
    if (!s_ha_host) return;
    nv_config_set_str("mqtt_host", lv_textarea_get_text(s_ha_host));
    int port = atoi(lv_textarea_get_text(s_ha_port));
    if (port <= 0 || port > 65535) port = 1883;
    nv_config_set_int("mqtt_port", port);
    nv_config_set_str("mqtt_user", lv_textarea_get_text(s_ha_user));
    if (lv_textarea_get_text(s_ha_pass)[0]) {
        nv_config_set_str("mqtt_pass", lv_textarea_get_text(s_ha_pass));
        lv_textarea_set_text(s_ha_pass, "");
        lv_textarea_set_placeholder_text(s_ha_pass, nv_tr(NV_STR_HA_PASS_KEEP));
    }
    if (!nv_config_get_bool("mqtt_en", false)) {   // "Save and connect" also switches it on
        nv_config_set_bool("mqtt_en", true);
        if (s_ha_sw) lv_obj_add_state(s_ha_sw, LV_STATE_CHECKED);
    }
    nv_ime_hide();
    nv_ui_toast(nv_tr(NV_STR_SAVED));
}
void ha_save_cb(lv_event_t *) { ha_save(); }
void ha_submit_cb(lv_obj_t *, void *) { ha_save(); }   // keyboard "Go" on the password field
void ha_republish_cb(lv_event_t *) {
    nv_mqtt_republish();
    nv_ui_toast(nv_tr(NV_STR_HA_REPUBLISH));
}

lv_obj_t *s_ha_api_url = nullptr, *s_ha_api_tok = nullptr;
void ha_api_save_cb(lv_event_t *) {
    if (!s_ha_api_url) return;
    nv_config_set_str("ha_url", lv_textarea_get_text(s_ha_api_url));
    if (lv_textarea_get_text(s_ha_api_tok)[0]) {
        nv_config_set_str("ha_token", lv_textarea_get_text(s_ha_api_tok));
        lv_textarea_set_text(s_ha_api_tok, "");
        lv_textarea_set_placeholder_text(s_ha_api_tok, nv_tr(NV_STR_HA_API_TOKEN_KEEP));
    }
    nv_ime_hide();
    nv_ui_toast(nv_tr(NV_STR_SAVED));
}

void ha_page_deleted(lv_event_t *) {
    s_ha_api_url = s_ha_api_tok = nullptr;
    if (s_ha_timer) { lv_timer_delete(s_ha_timer); s_ha_timer = nullptr; }
    nv_ime_set_submit_cb(nullptr, nullptr);
    nv_ime_hide();
    s_ha_host = s_ha_port = s_ha_user = s_ha_pass = nullptr;
    s_ha_sw = s_ha_status = nullptr;
}

lv_obj_t *ha_field(lv_obj_t *c, const char *ph, nv_ime_type_t type, nv_ime_return_t ret,
                   const char *text) {
    lv_obj_t *ta = nv_kit_textarea_ex(c, ph, true, type, ret);
    lv_obj_set_width(ta, lv_pct(100));
    if (text && text[0]) lv_textarea_set_text(ta, text);
    return ta;
}

void cat_home(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    lv_obj_add_event_cb(c, ha_page_deleted, LV_EVENT_DELETE, nullptr);

    section_label(c, nv_tr(NV_STR_HA_SECTION));
    lv_label_set_text(nv_kit_info(c), nv_tr(NV_STR_HA_HINT));
    s_ha_sw = nv_kit_switch_row(c, nv_tr(NV_STR_HA_ENABLE), nv_config_get_bool("mqtt_en", false),
                                ha_en_cb);
    char b[96];
    ha_status_text(b, sizeof b);
    s_ha_status = kv_row(c, nv_tr(NV_STR_HA_STATUS), b);
    kv_row(c, nv_tr(NV_STR_HA_DEVICE_ID), nv_mqtt_node_id());

    char v[64];
    nv_config_get_str("mqtt_host", "", v, sizeof v);
    s_ha_host = ha_field(c, nv_tr(NV_STR_HA_HOST), NV_IME_URL, NV_IME_RET_NEXT, v);
    lv_snprintf(v, sizeof v, "%d", nv_config_get_int("mqtt_port", 1883));
    s_ha_port = ha_field(c, nv_tr(NV_STR_HA_PORT), NV_IME_NUMBER, NV_IME_RET_NEXT, v);
    nv_config_get_str("mqtt_user", "", v, sizeof v);
    s_ha_user = ha_field(c, nv_tr(NV_STR_HA_USER), NV_IME_EMAIL, NV_IME_RET_NEXT, v);
    nv_config_get_str("mqtt_pass", "", v, sizeof v);
    const bool has_pass = v[0] != '\0';
    memset(v, 0, sizeof v);
    s_ha_pass = ha_field(c, nv_tr(has_pass ? NV_STR_HA_PASS_KEEP : NV_STR_HA_PASS), NV_IME_PASSWORD,
                         NV_IME_RET_GO, nullptr);
    nv_ime_set_submit_cb(ha_submit_cb, nullptr);

    lv_obj_t *save = nv_kit_button(c, nv_tr(NV_STR_HA_SAVE), true);
    lv_obj_add_event_cb(save, ha_save_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *rep = nv_kit_button(c, nv_tr(NV_STR_HA_REPUBLISH), false);
    lv_obj_add_event_cb(rep, ha_republish_cb, LV_EVENT_CLICKED, nullptr);

    // Home Assistant REST/WebSocket for apps (ABI v12 nv.ha_*): URL + long-lived token, the token
    // write-only like the MQTT password.
    section_label(c, nv_tr(NV_STR_HA_API_SECTION));
    lv_label_set_text(nv_kit_info(c), nv_tr(NV_STR_HA_API_HINT));
    char hv[160];
    nv_config_get_str("ha_url", "", hv, sizeof hv);
    s_ha_api_url = ha_field(c, nv_tr(NV_STR_HA_API_URL), NV_IME_URL, NV_IME_RET_NEXT, hv);
    nv_config_get_str("ha_token", "", hv, sizeof hv);
    const bool has_tok = hv[0] != '\0';
    memset(hv, 0, sizeof hv);
    s_ha_api_tok = ha_field(c, nv_tr(has_tok ? NV_STR_HA_API_TOKEN_KEEP : NV_STR_HA_API_TOKEN),
                            NV_IME_PASSWORD, NV_IME_RET_DONE, nullptr);
    lv_textarea_set_max_length(s_ha_api_tok, 300);
    lv_obj_t *hs = nv_kit_button(c, nv_tr(NV_STR_SAVE), false);
    lv_obj_add_event_cb(hs, ha_api_save_cb, LV_EVENT_CLICKED, nullptr);

    s_ha_timer = lv_timer_create(ha_tick, 1000, nullptr);
}

void cat_access(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    section_label(c, nv_tr(NV_STR_FONT_SIZE));   // real, live: reuses the theme font-scale setter
    const nv_font_scale_t cur = nv_theme_get_font_scale();
    lv_obj_t *fonts = pick_row(c, 12);
    font_card(fonts, NV_FONT_NORMAL, &lv_font_montserrat_20, cur == NV_FONT_NORMAL);
    font_card(fonts, NV_FONT_LARGE, &lv_font_montserrat_28, cur == NV_FONT_LARGE);
}

// -------------------------------------------------------------- Default apps page
// One row per MIME type some installed app can open, grouped by kind: its extensions, the MIME,
// and the app Open would use (accent = the user's own pick). A row with 2+ openers raises the
// system chooser in "pick default" mode. Reset clears every default and rebuilds DEFERRED (the
// button is deleted by the rebuild).
constexpr int kDefMimeMax = 64;   // distinct MIMEs considered (the type table has ~60 rows)
constexpr nv_file_kind_t kDefKinds[] = {NV_FILE_TEXT, NV_FILE_IMAGE, NV_FILE_AUDIO, NV_FILE_VIDEO,
                                        NV_FILE_APP, NV_FILE_ARCHIVE, NV_FILE_OTHER};
constexpr int kDefKindN = sizeof(kDefKinds) / sizeof(kDefKinds[0]);   // last = catch-all
lv_obj_t *s_def_col     = nullptr;   // page scroll column (null when not shown)
bool      s_def_pending = false;     // a deferred page rebuild is queued

void cat_default_apps(lv_obj_t *content);

void def_apply_async(void *) {
    s_def_pending = false;
    if (s_def_col) page_rebuild(s_def_col, cat_default_apps);   // page closed while queued: no-op
}
// nv_open_pick_default() completion: nv_open runs it from its own lv_async hop, never inside
// one of this page's events, so rebuilding right here is safe. The page may be gone by then
// (category switch, app closed while the chooser was up) — s_def_col is set only while shown.
void def_picked(void *) {
    if (s_def_col) page_rebuild(s_def_col, cat_default_apps);
}
void def_row_cb(lv_event_t *e) {
    const char *mime = static_cast<const char *>(lv_event_get_user_data(e));   // type table: static
    if (mime && !s_def_pending) nv_open_pick_default(mime, def_picked, nullptr);
}
void def_reset_cb(lv_event_t *) {
    if (s_def_pending || !s_def_col) return;   // a double tap queues one rebuild, not two
    nv_open_clear_defaults();
    if (lv_async_call(def_apply_async, nullptr) == LV_RESULT_OK) s_def_pending = true;
}
void def_page_deleted(lv_event_t *) {
    lv_async_call_cancel(def_apply_async, nullptr);   // never rebuild a page that is gone
    s_def_pending = false;
    s_def_col = nullptr;
}

// "JPG, JPEG": every extension mapped to `mime`, uppercased, in table order (bounded to `n`).
void def_ext_list(const char *mime, char *out, size_t n) {
    size_t w = 0;
    out[0] = '\0';
    const int cnt = nv_open_type_count();
    for (int i = 0; i < cnt; i++) {
        const char *ext = nullptr, *m = nullptr;
        nv_file_kind_t k;
        if (!nv_open_type_at(i, &ext, &m, &k) || !ext || !m || strcmp(m, mime) != 0) continue;
        if (*ext == '.') ext++;
        if (w) {
            if (w + 3 >= n) break;   // no room for ", X"
            out[w++] = ',';
            out[w++] = ' ';
        }
        for (; *ext && w + 1 < n; ext++)
            out[w++] = (*ext >= 'a' && *ext <= 'z') ? (char)(*ext - 'a' + 'A') : *ext;
        out[w] = '\0';
    }
}

// [kind glyph]  EXTS / mime ..........  App  — tappable (chooser) only when there is a choice.
void def_row(lv_obj_t *col, const char *mime, nv_file_kind_t kind, bool choosable) {
    const NvTheme *th = nv_theme_get();
    const char *sym = nv_open_kind_symbol(kind);
    lv_obj_t *row = nv_kit_row(col, sym ? sym : LV_SYMBOL_FILE);   // the kit's label = the glyph
    lv_obj_set_style_pad_ver(row, NV_SP_2, 0);   // two text lines: stay near the kit's 56 px
    lv_obj_set_style_pad_column(row, NV_SP_3, 0);
    lv_obj_t *glyph = lv_obj_get_child(row, 0);
    lv_obj_set_style_min_width(glyph, 32, 0);    // titles line up across kinds
    lv_obj_set_style_text_align(glyph, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(glyph, th->text_dim, 0);

    // Title + MIME. no_click: this flex-grown container would otherwise swallow the row tap.
    lv_obj_t *txt = lv_obj_create(row);
    lv_obj_remove_style_all(txt);
    no_click(txt);
    lv_obj_set_flex_grow(txt, 1);
    lv_obj_set_height(txt, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(txt, 2, 0);
    lv_obj_clear_flag(txt, LV_OBJ_FLAG_SCROLLABLE);
    char exts[64];
    def_ext_list(mime, exts, sizeof exts);
    lv_obj_t *nm = lv_label_create(txt);
    lv_label_set_text(nm, exts);
    lv_obj_set_style_text_color(nm, th->text, 0);
    lv_obj_set_width(nm, lv_pct(100));
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_t *mt = lv_label_create(txt);
    lv_label_set_text(mt, mime);
    lv_obj_set_style_text_font(mt, &nv_font_14, 0);
    lv_obj_set_style_text_color(mt, th->text_dim, 0);
    lv_obj_set_width(mt, lv_pct(100));
    lv_label_set_long_mode(mt, LV_LABEL_LONG_DOT);

    // The app Open would use; accent only when that is the user's own default (not merely the
    // sole opener or a priority winner). NULL preferred = a tie: Open asks every time.
    const NvOpenHandler *pref = nv_open_preferred_mime(mime);
    const char *app = pref ? nv_open_handler_label(pref) : nullptr;
    char uid[NV_OPEN_ID_MAX];
    nv_open_get_default(mime, uid, sizeof uid);
    const bool mine = uid[0] && pref && pref->id && strcmp(pref->id, uid) == 0;
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, app ? app : nv_tr(NV_STR_ASK_EVERY_TIME));
    lv_obj_set_style_text_color(v, mine ? th->accent : th->text_dim, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_max_width(v, lv_pct(40), 0);

    if (choosable) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);   // kit row: surface2 pressed feedback
        lv_obj_add_event_cb(row, def_row_cb, LV_EVENT_CLICKED, const_cast<char *>(mime));
    } else {
        no_click(row);   // sole opener: nothing to choose, so no pressed feedback either
    }
}

void cat_default_apps(lv_obj_t *content) {
    lv_obj_t *c = nv_kit_scroll_column(content);
    s_def_col = c;
    lv_obj_add_event_cb(c, def_page_deleted, LV_EVENT_DELETE, nullptr);
    const NvTheme *th = nv_theme_get();

    lv_obj_t *hint = nv_kit_info(c);
    lv_label_set_text(hint, nv_tr(NV_STR_DEFAULT_APPS_HINT));
    lv_obj_set_style_text_color(hint, th->text_dim, 0);

    // Distinct MIMEs in table order (jpg + jpeg -> one image/jpeg row), each with its kind group
    // and opener count capped at 2 (the page only needs none / one / a choice). Fixed stack
    // array, no heap; MIMEs past the cap are simply not listed.
    struct DefMime { const char *mime; uint8_t group; uint8_t openers; };
    DefMime ms[kDefMimeMax];
    int nm = 0;
    const int cnt = nv_open_type_count();
    for (int i = 0; i < cnt && nm < kDefMimeMax; i++) {
        const char *ext = nullptr, *mime = nullptr;
        nv_file_kind_t kind = NV_FILE_OTHER;
        if (!nv_open_type_at(i, &ext, &mime, &kind) || !mime || !mime[0]) continue;
        bool seen = false;
        for (int j = 0; j < nm && !seen; j++) seen = strcmp(ms[j].mime, mime) == 0;
        if (seen) continue;
        int g = kDefKindN - 1;   // DIR / unknown kinds fall into the catch-all group
        for (int k = 0; k < kDefKindN; k++)
            if (kDefKinds[k] == kind) { g = k; break; }
        const NvOpenHandler *hs[2];
        const int no = nv_open_handlers_mime(mime, NV_OPEN_OPENER, hs, 2);
        ms[nm++] = {mime, (uint8_t)g, (uint8_t)(no < 0 ? 0 : no > 2 ? 2 : no)};
    }

    // Grouped by kind, a section label per non-empty group.
    int shown = 0;
    for (int g = 0; g < kDefKindN; g++) {
        bool header = false;
        for (int j = 0; j < nm; j++) {
            if (ms[j].group != g || ms[j].openers == 0) continue;
            if (!header) {
                const char *kl = nv_open_kind_label(kDefKinds[g]);
                section_label(c, kl ? kl : "");
                header = true;
            }
            def_row(c, ms[j].mime, kDefKinds[g], ms[j].openers >= 2);
            shown++;
        }
    }
    if (!shown) {
        lv_obj_t *e = nv_kit_info(c);
        lv_label_set_text(e, nv_tr(NV_STR_NONE));
        lv_obj_set_style_text_color(e, th->text_dim, 0);
    }

    lv_obj_t *rb = nv_kit_button(c, nv_tr(NV_STR_RESET_DEFAULTS), false);
    lv_obj_add_event_cb(rb, def_reset_cb, LV_EVENT_CLICKED, nullptr);
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
    {LV_SYMBOL_BLUETOOTH, NV_STR_SET_BLUETOOTH, NV_STR_GROUP_CONNECT, cat_bluetooth},
    {LV_SYMBOL_HOME,     NV_STR_SET_HOME,     NV_STR_GROUP_CONNECT,  cat_home},
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
    {LV_SYMBOL_FILE,     NV_STR_SET_DEFAULT_APPS, NV_STR_GROUP_PERSONAL, cat_default_apps},
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
        case NV_STR_SET_BLUETOOTH: {
            nv_bt_status_t st;
            nv_bt_status(&st);
            const int np = st.n_connected;   // BLE pads, keyboards, mice
            // State, like the other rows ("Ready", "Off"...), not the category name again.
            nv_str_id_t s = NV_STR_BT_READY;
            switch (st.state) {
            case NV_BT_OFF:      s = NV_STR_HA_ST_OFF; break;
            case NV_BT_STARTING: s = NV_STR_BT_STARTING; break;
            case NV_BT_SCANNING: s = NV_STR_BT_SCANNING; break;
            case NV_BT_ERROR:    s = NV_STR_BT_ERROR; break;
            default: break;
            }
            if (np > 0) lv_snprintf(buf, n, nv_tr(NV_STR_BT_N_CONNECTED_FMT), np);
            else lv_snprintf(buf, n, "%s", nv_tr(s));
            break;
        }
        case NV_STR_SET_HOME:
            ha_status_text(buf, n);
            break;
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
