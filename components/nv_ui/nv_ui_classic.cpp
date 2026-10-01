// nv_ui_classic — the classic desktop shell: desktop icons, taskbar, Start menu, window title bar,
// right-click menus. It recalls the Windows 95 desktop (bevelled buttons, taskbar, Start) but
// stays NucleoOS: colours, accent, fonts and icons come from the active theme, and apps are never
// restyled — only the shell around them changes. See docs/CLASSIC_UI_PLAN.md.
//
// Lives next to the tablet launcher (nv_ui.cpp), which stays built and hidden while this is on.
// Desktop = a screen child at the bottom of the z-order (apps open above it); taskbar, Start menu,
// context menus and tooltips live on the top layer. All state is here, in PSRAM.
#include "nv_ui_internal.h"

#include "nv_ui.h"
#include "nv_ui_focus.h"
#include "nv_theme.h"
#include "nv_i18n.h"
#include "nv_fonts.h"
#include "nv_config.h"
#include "nv_time.h"
#include "nv_wifi.h"
#include "nv_audio.h"
#include "nv_sd.h"
#include "nv_usb_storage.h"
#include "nv_notify.h"
#include "nv_hid_host.h"
#include "nv_mem_attr.h"
#include "nv_ui_kit.h"
#include "nv_ime.h"
#include "nv_open.h"
#include "nv_event_bus.h"
#include "esp_app_desc.h"
#include "generated/nv_logo.h"   // the NucleoOS crystal nucleus (tools/gen_logo.py)

#include "lvgl.h"

#include <ctype.h>
#include <string.h>

namespace {

constexpr int kCellW = 108, kCellH = 92, kIconPx = 48, kMaxDesk = 64, kMaxTasks = 5, kMaxPins = 18;
constexpr int kRowH = 38, kMenuW = 250;
constexpr uint32_t kDoubleClickMs = 450;

struct MenuItem { const char *sym; const char *text; void (*fn)(const NvApp *); const NvApp *app; bool sep; };

struct State {
    bool       on;
    lv_obj_t  *desk, *grid;            // desktop plane + icon grid
    lv_obj_t  *bar, *start_btn, *tasks, *tray;
    lv_obj_t  *t_bell, *t_usb, *t_sd, *t_wifi, *t_vol, *t_clock, *t_date;
    bool       fs, ime_up;             // a fullscreen app / the on-screen keyboard hides the taskbar
    lv_obj_t  *start, *start_panel, *start_col, *start_body, *start_search;   // Start menu
    int32_t    ime_h;                  // docked height of the on-screen keyboard (0 = down)
    int        view;                   // StartView
    const NvApp *first_app;            // best search match (Enter opens it)
    char       first_path[256];
    lv_obj_t  *menu;                   // context menu scrim
    lv_obj_t  *tip;                    // tooltip label
    lv_obj_t  *preview;                // task hover preview (last screen)
    const NvApp *run[8]; int nrun;     // the session's tasks: open + suspended, in opening order
    lv_obj_t  *title_hdr;              // the open app's title bar
    lv_timer_t *tick;
    lv_obj_t  *last_click; uint32_t last_click_ms;
    MenuItem   items[10]; int n_items;
    char       tip_buf[48];
};
NV_PSRAM_BSS State S;

// Desktop palettes (Settings > Display > Desktop colours, "cls_pal"): they repaint the shell only
// (taskbar, Start, windows, menus); apps keep the system theme. 0 = the NucleoOS theme itself.
struct Pal { uint32_t bg, surface, surface2, surface3, text, dim, accent, primary, on_primary; };
const Pal kPals[] = {
    {},                                                                              // NucleoOS
    {0x020503, 0x07100A, 0x0C1A10, 0x173A22, 0xC8FFD8, 0x5FA874, 0x39FF6A, 0x00B84A, 0x001A08},   // Cyberdeck
    {0x070400, 0x120C04, 0x1C1306, 0x3A2A10, 0xFFE0B0, 0xB08040, 0xFFB000, 0xD98A00, 0x1A1000},   // Amber
    {0x010605, 0x061210, 0x0B1E1B, 0x15403A, 0xC8FFF6, 0x5AA89C, 0x00E5C8, 0x00A890, 0x00201C},   // Teal
};
NV_PSRAM_BSS NvTheme s_pal;
NV_PSRAM_BSS int s_pal_id;

void pal_refresh(void) {
    s_pal = *nv_theme_get();
    s_pal_id = nv_config_get_int("cls_pal", 1);                  // default: Cyberdeck
    if (s_pal_id <= 0 || s_pal_id >= (int)(sizeof kPals / sizeof *kPals)) { s_pal_id = 0; return; }
    const Pal &p = kPals[s_pal_id];
    s_pal.bg = lv_color_hex(p.bg);
    s_pal.surface = s_pal.header = s_pal.shade_bg = lv_color_hex(p.surface);
    s_pal.surface2 = s_pal.control_alt = lv_color_hex(p.surface2);
    s_pal.surface3 = s_pal.divider = lv_color_hex(p.surface3);
    s_pal.text = s_pal.text_strong = lv_color_hex(p.text);
    s_pal.text_dim = s_pal.text_disabled = lv_color_hex(p.dim);
    s_pal.accent = s_pal.success = s_pal.success_solid = lv_color_hex(p.accent);
    s_pal.primary = lv_color_hex(p.primary);
    s_pal.on_primary = lv_color_hex(p.on_primary);
}
const NvTheme *th(void) { return &s_pal; }
int32_t scr_w(void) { return LV_HOR_RES; }
int32_t scr_h(void) { return LV_VER_RES; }

// ---------------------------------------------------------------- look: bevels, rows, tooltips

// Classic 3D edge drawn as four 1 px lines after the object (no shadow, no layer: safe on the
// P4 software renderer). Raised by default; sunken while pressed / checked, or when user_data = 1.
void bevel_cb(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const bool sunk = lv_event_get_user_data(e) != nullptr ||
                      lv_obj_has_state(o, LV_STATE_PRESSED) || lv_obj_has_state(o, LV_STATE_CHECKED);
    const lv_color_t base = th()->surface2;
    const lv_color_t hi = lv_color_mix(lv_color_white(), base, 110);
    const lv_color_t lo = lv_color_mix(lv_color_black(), base, 120);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.width = 1;
    d.opa = LV_OPA_COVER;
    auto line = [&](int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
        d.p1.x = x1; d.p1.y = y1; d.p2.x = x2; d.p2.y = y2;
        lv_draw_line(layer, &d);
    };
    d.color = sunk ? lo : hi;
    line(a.x1, a.y1, a.x2, a.y1);
    line(a.x1, a.y1, a.x1, a.y2);
    d.color = sunk ? hi : lo;
    line(a.x1, a.y2, a.x2, a.y2);
    line(a.x2, a.y1, a.x2, a.y2);
}
// The running app's taskbar button: an accent bar inside the sunken button, clear of its edges.
void active_mark_cb(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = th()->accent;
    d.bg_opa = LV_OPA_COVER;
    d.radius = 1;
    const lv_area_t bar = {a.x1 + 4, a.y2 - 4, a.x2 - 4, a.y2 - 3};
    lv_draw_rect(lv_event_get_layer(e), &d, &bar);
}

void bevel(lv_obj_t *o, bool sunken = false) {
    lv_obj_add_event_cb(o, bevel_cb, LV_EVENT_DRAW_POST, sunken ? (void *)1 : nullptr);
}

lv_obj_t *box(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return o;
}

lv_obj_t *text(lv_obj_t *parent, const char *t, lv_color_t c, const lv_font_t *f = nullptr) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_color(l, c, 0);
    if (f) lv_obj_set_style_text_font(l, f, 0);
    return l;
}

// A bevelled push button (taskbar, title bar).
lv_obj_t *button(lv_obj_t *parent, int32_t w, int32_t h, lv_event_cb_t cb, void *ud) {
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(b, th()->surface2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, th()->surface3, LV_STATE_HOVERED);
    lv_obj_set_style_bg_color(b, th()->surface3, LV_STATE_CHECKED);
    lv_obj_set_style_pad_hor(b, 8, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 6, 0);
    bevel(b);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

void tip_hide(void) { if (S.tip) lv_obj_add_flag(S.tip, LV_OBJ_FLAG_HIDDEN); }

// Tooltip above a hovered taskbar item: the app name, or the full date on the clock.
void tip_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_HOVER_OVER) { tip_hide(); return; }
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    const char *t = (const char *)lv_event_get_user_data(e);
    if (o == S.t_clock || o == lv_obj_get_parent(S.t_clock)) {
        struct tm tmv;
        nv_time_now(&tmv);
        lv_snprintf(S.tip_buf, sizeof S.tip_buf, "%s %d %s %d", nv_i18n_wday_short(tmv.tm_wday),
                    tmv.tm_mday, nv_i18n_month_short(tmv.tm_mon), tmv.tm_year + 1900);
        t = S.tip_buf;
    }
    if (!t || !t[0]) return;
    if (!S.tip) {
        S.tip = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_font(S.tip, th()->font_default, 0);
        lv_obj_set_style_bg_opa(S.tip, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(S.tip, 8, 0);
        lv_obj_set_style_pad_ver(S.tip, 4, 0);
        lv_obj_set_style_border_width(S.tip, 1, 0);
        lv_obj_clear_flag(S.tip, LV_OBJ_FLAG_CLICKABLE);
        nv_focus_skip(S.tip);
    }
    lv_obj_set_style_bg_color(S.tip, th()->surface, 0);
    lv_obj_set_style_border_color(S.tip, th()->text_dim, 0);
    lv_obj_set_style_text_color(S.tip, th()->text_strong, 0);
    lv_label_set_text(S.tip, t);
    lv_obj_clear_flag(S.tip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(S.tip);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t x = a.x1, w = lv_obj_get_width(S.tip);
    if (x + w > scr_w() - 2) x = scr_w() - 2 - w;
    lv_obj_set_pos(S.tip, x, a.y1 - lv_obj_get_height(S.tip) - 4);
    lv_obj_move_foreground(S.tip);
}
void tooltip(lv_obj_t *o, const char *t) {
    lv_obj_add_event_cb(o, tip_cb, LV_EVENT_HOVER_OVER, (void *)t);
    lv_obj_add_event_cb(o, tip_cb, LV_EVENT_HOVER_LEAVE, nullptr);
    lv_obj_add_event_cb(o, tip_cb, LV_EVENT_PRESSED, nullptr);
}

// Menu / list row: [icon or symbol] text. Hover and press paint it in the accent.
lv_obj_t *row(lv_obj_t *parent, const NvApp *app, const char *sym, const char *t, lv_event_cb_t cb, void *ud) {
    lv_obj_t *r = box(parent);
    lv_obj_set_size(r, lv_pct(100), kRowH);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_hor(r, 10, 0);
    lv_obj_set_style_pad_column(r, 10, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(r, th()->accent, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_HOVERED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_text_color(r, th()->text_strong, 0);
    lv_obj_set_style_text_color(r, th()->on_primary, LV_STATE_HOVERED);
    lv_obj_set_style_text_color(r, th()->on_primary, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(r, th()->on_primary, LV_STATE_FOCUS_KEY);
    if (app) {
        lv_obj_t *img = lv_image_create(r);
        lv_image_set_src(img, nvui::icon(app, 24));
    } else {
        lv_obj_t *s = lv_label_create(r);
        lv_label_set_text(s, sym ? sym : "");
        lv_obj_set_width(s, 24);
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, t);
    lv_obj_set_height(l, lv_font_get_line_height(th()->font_default));   // one line, dots
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(l, 1);
    if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
    return r;
}

lv_obj_t *section(lv_obj_t *parent, const char *t) {
    lv_obj_t *l = text(parent, t, th()->text_dim);
    lv_obj_set_style_pad_left(l, 10, 0);
    lv_obj_set_style_pad_top(l, 6, 0);
    return l;
}

lv_obj_t *hline(lv_obj_t *parent) {
    lv_obj_t *l = box(parent);
    lv_obj_set_size(l, lv_pct(100), 2);
    bevel(l, true);
    return l;
}

// A floating panel (menus): theme surface, bevelled, content-sized column.
lv_obj_t *panel(lv_obj_t *parent, int32_t w) {
    lv_obj_t *p = box(parent);
    lv_obj_set_width(p, w);
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);   // taps inside never reach the dismiss scrim
    lv_obj_set_style_bg_color(p, th()->surface, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(p, 3, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    // One even hairline: a bevel's dark edges vanish on the dark theme and leave the panel open.
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, th()->surface3, 0);
    return p;
}

// Full-screen transparent catcher on the top layer: a click outside the menu closes it (and Esc,
// through nvclassic::escape).
lv_obj_t *scrim(lv_event_cb_t close_cb, lv_obj_t *parent = nullptr) {
    lv_obj_t *s = box(parent ? parent : lv_layer_top());
    lv_obj_set_size(s, scr_w(), scr_h());
    lv_obj_set_style_text_font(s, th()->font_default, 0);   // top layer: no inherited Latin-1 font
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s, close_cb, LV_EVENT_CLICKED, nullptr);
    return s;
}

// ---------------------------------------------------------------- app lists (persisted)

// Desktop icons ("cdesk") and Start pins ("cpin") live in nv_config as comma-separated app ids:
// "" = the defaults, "," = emptied by the user.
struct ListDef { const char *key; const char *const *first; int nfirst; bool rest_builtin; int max; };
const char *const kDeskFirst[] = {"files", "apps", "settings"};
const char *const kPinFirst[] = {"files", "settings", "apps", "anima", "gallery", "camera",
                                 "music", "video", "notes", "calc", "terminal", "sysmon"};
const ListDef kDeskList = {"cdesk", kDeskFirst, 3, true, kMaxDesk};
const ListDef kPinList  = {"cpin", kPinFirst, 12, false, kMaxPins};

int list_load(const ListDef &d, const NvApp **out) {
    char *buf = (char *)lv_malloc(1024);
    if (!buf) return 0;
    nv_config_get_str(d.key, "", buf, 1024);
    int n = 0;
    if (!buf[0]) {
        for (int i = 0; i < d.nfirst && n < d.max; i++)
            if (const NvApp *a = nv_ui_find_app(d.first[i])) out[n++] = a;
        for (int i = 0; d.rest_builtin && i < nv_app_count() && n < d.max; i++) {
            const NvApp *a = nv_app_at(i);
            bool dup = false;
            for (int k = 0; k < n; k++) dup = dup || out[k] == a;
            if (!dup && a->user == nullptr) out[n++] = a;   // built-in apps
        }
    } else {
        for (char *p = buf; *p && n < d.max;) {
            char *c = strchr(p, ',');
            if (c) *c = 0;
            if (*p) if (const NvApp *a = nv_ui_find_app(p)) out[n++] = a;
            if (!c) break;
            p = c + 1;
        }
    }
    lv_free(buf);
    return n;
}

void list_save(const ListDef &d, const NvApp *const *v, int n) {
    char *buf = (char *)lv_malloc(1024);
    if (!buf) return;
    size_t w = 0;
    for (int i = 0; i < n; i++) {
        const size_t l = strlen(v[i]->id);
        if (w + l + 2 >= 1024) break;
        memcpy(buf + w, v[i]->id, l);
        w += l;
        buf[w++] = ',';
    }
    if (!w) buf[w++] = ',';
    buf[w] = 0;
    nv_config_set_str(d.key, buf);
    lv_free(buf);
}

bool list_has(const ListDef &d, const NvApp *a) {
    const NvApp *v[kMaxDesk];
    const int n = list_load(d, v);
    for (int i = 0; i < n; i++) if (v[i] == a) return true;
    return false;
}

void desk_build_icons(void);   // fwd
void start_refresh(void);      // fwd: redraw the open Start menu's current view

void lists_changed(const ListDef &d) {
    if (&d == &kDeskList) desk_build_icons();
    else start_refresh();
}

void list_add(const ListDef &d, const NvApp *a) {
    const NvApp *v[kMaxDesk + 1];
    int n = list_load(d, v);
    for (int i = 0; i < n; i++) if (v[i] == a) return;
    if (n < d.max) v[n++] = a;
    list_save(d, v, n);
    lists_changed(d);
}
void list_remove(const ListDef &d, const NvApp *a) {
    const NvApp *v[kMaxDesk];
    int n = list_load(d, v), w = 0;
    for (int i = 0; i < n; i++) if (v[i] != a) v[w++] = v[i];
    list_save(d, v, w);
    lists_changed(d);
}

void desk_arrange(const NvApp *) {
    const NvApp *v[kMaxDesk];
    const int n = list_load(kDeskList, v);
    for (int i = 1; i < n; i++)                      // insertion sort by visible name
        for (int j = i; j > 0 && strcmp(nvui::label(v[j - 1]), nvui::label(v[j])) > 0; j--) {
            const NvApp *t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
        }
    list_save(kDeskList, v, n);
    desk_build_icons();
}
void desk_reset(const NvApp *) {
    nv_config_set_str(kDeskList.key, "");
    desk_build_icons();
}

// ---------------------------------------------------------------- context menus

void menu_close(void) {
    if (S.menu) { lv_obj_delete(S.menu); S.menu = nullptr; }
}
void menu_close_async(void *) { menu_close(); }
void menu_scrim_cb(lv_event_t *) { lv_async_call(menu_close_async, nullptr); }

void menu_item_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= S.n_items) return;
    static MenuItem it;                              // survives the menu's deletion
    it = S.items[i];
    lv_async_call([](void *) {
        menu_close();
        if (it.fn) it.fn(it.app);
    }, nullptr);
}

void menu_open(lv_point_t p, const MenuItem *items, int n) {
    menu_close();
    tip_hide();
    if (n > (int)(sizeof S.items / sizeof *S.items)) n = sizeof S.items / sizeof *S.items;
    memcpy(S.items, items, n * sizeof *items);
    S.n_items = n;
    S.menu = scrim(menu_scrim_cb);
    lv_obj_t *pn = panel(S.menu, kMenuW);
    lv_obj_t *first = nullptr;
    for (int i = 0; i < n; i++) {
        if (items[i].sep) hline(pn);
        lv_obj_t *r = row(pn, nullptr, items[i].sym, items[i].text, menu_item_cb, (void *)(intptr_t)i);
        if (!first) first = r;
    }
    nv_focus_prefer(first);
    lv_obj_update_layout(pn);
    const int32_t w = lv_obj_get_width(pn), h = lv_obj_get_height(pn);
    int32_t x = p.x, y = p.y;
    if (x + w > scr_w() - 2) x = scr_w() - 2 - w;
    if (y + h > scr_h() - 2) y = LV_MAX(2, p.y - h);
    lv_obj_set_pos(pn, LV_MAX(2, x), y);
}

void open_app_fn(const NvApp *a) { if (a) nv_ui_open_app(a); }
void open_id_fn(const char *id) { nv_ui_open_app_id(id); }
void close_fn(const NvApp *) { nv_ui_close_app(); }
void min_fn(const NvApp *) { nvui::minimize(); }
void back_fn(const NvApp *) { nvui::back(); }
void display_fn(const NvApp *) { nv_ui_open_app_id("settings"); }
void sysmon_fn(const NvApp *) { nv_ui_open_app_id("sysmon"); }
void tasks_refresh(void);   // fwd
void close_task_fn(const NvApp *a);   // fwd
void desk_add_fn(const NvApp *a) { list_add(kDeskList, a); }
void desk_remove_fn(const NvApp *a) { list_remove(kDeskList, a); }
void pin_add_fn(const NvApp *a) { list_add(kPinList, a); }
void pin_remove_fn(const NvApp *a) { list_remove(kPinList, a); }

lv_point_t center(lv_obj_t *o) {
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
}

void menu_for_app(lv_point_t p, const NvApp *a, bool running) {
    MenuItem m[5];
    int n = 0;
    m[n++] = {LV_SYMBOL_PLAY, nv_tr(NV_STR_OPEN), open_app_fn, a, false};
    if (running) m[n++] = {LV_SYMBOL_CLOSE, nv_tr(NV_STR_CLOSE), close_task_fn, a, false};
    if (list_has(kPinList, a)) m[n++] = {LV_SYMBOL_MINUS, nv_tr(NV_STR_UNPIN_START), pin_remove_fn, a, true};
    else                       m[n++] = {LV_SYMBOL_PLUS, nv_tr(NV_STR_PIN_START), pin_add_fn, a, true};
    if (list_has(kDeskList, a)) m[n++] = {LV_SYMBOL_MINUS, nv_tr(NV_STR_DESK_REMOVE), desk_remove_fn, a, false};
    else                        m[n++] = {LV_SYMBOL_PLUS, nv_tr(NV_STR_DESK_ADD), desk_add_fn, a, false};
    menu_open(p, m, n);
}

void menu_for_desktop(lv_point_t p) {
    const MenuItem m[] = {
        {LV_SYMBOL_LIST, nv_tr(NV_STR_DESK_ARRANGE), desk_arrange, nullptr, false},
        {LV_SYMBOL_REFRESH, nv_tr(NV_STR_DESK_RESET), desk_reset, nullptr, false},
        {LV_SYMBOL_IMAGE, nv_tr(NV_STR_DISPLAY_SETTINGS), display_fn, nullptr, true},
    };
    menu_open(p, m, 3);
}

void menu_for_taskbar(lv_point_t p) {
    const MenuItem m[] = {
        {LV_SYMBOL_HOME, nv_tr(NV_STR_SHOW_DESKTOP), min_fn, nullptr, false},
        {LV_SYMBOL_SETTINGS, nvui::label(nv_ui_find_app("sysmon")), sysmon_fn, nullptr, false},
    };
    menu_open(p, m, 2);
}

// ---------------------------------------------------------------- desktop

void icon_click_cb(lv_event_t *e) {
    lv_obj_t *cell = lv_event_get_current_target_obj(e);
    const NvApp *a = (const NvApp *)lv_event_get_user_data(e);
    lv_indev_t *ind = lv_indev_active();
    const bool mouse = ind && ind == (lv_indev_t *)nv_hid_host_mouse_indev();
    // Mouse: click selects, double click opens. Finger or keyboard: open straight away.
    if (mouse) {
        const uint32_t now = lv_tick_get();
        const bool dbl = S.last_click == cell && now - S.last_click_ms < kDoubleClickMs;
        S.last_click = cell;
        S.last_click_ms = now;
        const uint32_t n = lv_obj_get_child_count(S.grid);
        for (uint32_t i = 0; i < n; i++) lv_obj_remove_state(lv_obj_get_child(S.grid, (int32_t)i), LV_STATE_CHECKED);
        lv_obj_add_state(cell, LV_STATE_CHECKED);
        if (!dbl) return;
        S.last_click = nullptr;
    }
    nv_ui_open_app(a);
}

void icon_menu_cb(lv_event_t *e) {   // long press (finger) / Menu key
    lv_obj_t *cell = lv_event_get_current_target_obj(e);
    menu_for_app(center(cell), (const NvApp *)lv_event_get_user_data(e), false);
}

void desk_bg_cb(lv_event_t *) {       // click on empty desktop: drop the selection
    if (!S.grid) return;
    const uint32_t n = lv_obj_get_child_count(S.grid);
    for (uint32_t i = 0; i < n; i++) lv_obj_remove_state(lv_obj_get_child(S.grid, (int32_t)i), LV_STATE_CHECKED);
}
void desk_menu_cb(lv_event_t *e) {    // long press on empty desktop
    lv_indev_t *ind = lv_indev_active();
    lv_point_t p = {scr_w() / 2, scr_h() / 2};
    if (ind && lv_indev_get_type(ind) == LV_INDEV_TYPE_POINTER) lv_indev_get_point(ind, &p);
    (void)e;
    menu_for_desktop(p);
}

void desk_build_icons(void) {
    if (!S.grid) return;
    lv_obj_clean(S.grid);
    S.last_click = nullptr;
    const NvApp *v[kMaxDesk];
    const int n = list_load(kDeskList, v);
    const lv_font_t *f = th()->font_default;
    const int32_t lh = lv_font_get_line_height(f);
    for (int i = 0; i < n; i++) {
        lv_obj_t *c = box(S.grid);
        lv_obj_set_size(c, kCellW, kCellH);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(c, 6, 0);
        lv_obj_set_style_pad_top(c, 6, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(c, 4, 0);
        lv_obj_set_style_bg_color(c, th()->text_strong, LV_STATE_HOVERED);
        lv_obj_set_style_bg_opa(c, LV_OPA_10, LV_STATE_HOVERED);
        lv_obj_set_style_bg_color(c, th()->accent, LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(c, LV_OPA_30, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(c, th()->accent, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(c, LV_OPA_30, LV_STATE_PRESSED);
        lv_obj_add_event_cb(c, icon_click_cb, LV_EVENT_CLICKED, (void *)v[i]);
        lv_obj_add_event_cb(c, icon_menu_cb, LV_EVENT_LONG_PRESSED, (void *)v[i]);
        lv_obj_set_user_data(c, (void *)v[i]);

        lv_obj_t *img = lv_image_create(c);
        lv_image_set_src(img, nvui::icon(v[i], kIconPx));
        // Name on a soft plate: readable on any wallpaper without a text shadow (banned: layer).
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text(l, nvui::label(v[i]));
        lv_obj_set_width(l, kCellW - 4);
        lv_obj_set_style_max_height(l, 2 * lh, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(l, th()->text_strong, 0);
        lv_obj_set_style_bg_color(l, th()->bg, 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_50, 0);
        lv_obj_set_style_radius(l, 4, 0);
        lv_obj_set_style_bg_color(l, th()->accent, LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(l, th()->on_primary, LV_STATE_CHECKED);
        lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    }
}

void desk_build(void) {
    S.desk = box(lv_screen_active());
    lv_obj_set_size(S.desk, scr_w(), scr_h() - nvclassic::kTaskH);
    lv_obj_set_pos(S.desk, 0, 0);
    nvui::wallpaper(S.desk);
    lv_obj_add_flag(S.desk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(S.desk, desk_bg_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(S.desk, desk_menu_cb, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_move_to_index(S.desk, 0);                 // under everything: apps open above it

    // Icons fill columns top to bottom, then left to right (the classic desktop order).
    S.grid = box(S.desk);
    lv_obj_set_size(S.grid, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(S.grid, 10, 0);
    lv_obj_set_style_pad_row(S.grid, 6, 0);
    lv_obj_set_style_pad_column(S.grid, 10, 0);
    lv_obj_set_flex_flow(S.grid, LV_FLEX_FLOW_COLUMN_WRAP);
    lv_obj_add_flag(S.grid, LV_OBJ_FLAG_EVENT_BUBBLE);   // empty-grid clicks reach the desktop
    desk_build_icons();
}

// ---------------------------------------------------------------- Start menu
//
//  ┌─────────────────────────────────────────────┐
//  │ [ Search apps and files                   ] │   search view: Apps + Files
//  │ Pinned                          All apps ›  │   home view: pinned grid +
//  │ [] [] [] [] [] []                           │   recommended (recent / most used)
//  │ Recommended                                 │   all view: A-Z list with letters
//  │ (o) App        (o) App                      │
//  │ NucleoOS 1.1.x              ⚙  ◐  ⏻        │
//  └─────────────────────────────────────────────┘
//
// It lives on the screen (not the top layer) so the on-screen keyboard can rise above the search
// field; the taskbar steps aside while the keyboard is up.

enum StartView { SV_HOME, SV_ALL, SV_SEARCH };
constexpr int32_t kStartW = 572, kStartH = 500, kTileW = 88, kTileH = 80;   // 6 tiles + padding

void start_close(void) {
    if (S.start) {
        lv_obj_delete(S.start);
        S.start = nullptr;
        S.start_panel = S.start_body = S.start_search = nullptr;
    }
    S.view = SV_HOME;
    if (S.start_btn) lv_obj_remove_state(S.start_btn, LV_STATE_CHECKED);
}
void start_close_async(void *) { start_close(); }
void start_scrim_cb(lv_event_t *) { lv_async_call(start_close_async, nullptr); }

// Every Start action closes the menu first, then acts (the control that fired is inside it).
void start_app_cb(lv_event_t *e) {
    static const NvApp *a;
    a = (const NvApp *)lv_event_get_user_data(e);
    lv_async_call([](void *) { start_close(); nv_ui_open_app(a); }, nullptr);
}
void start_app_menu_cb(lv_event_t *e) {
    lv_obj_t *r = lv_event_get_current_target_obj(e);
    menu_for_app(center(r), (const NvApp *)lv_event_get_user_data(e), false);
}
NV_PSRAM_BSS char s_open_path[256];   // the file to open once the menu has closed

void start_file_cb(lv_event_t *e) {
    char *path = s_open_path;
    lv_strlcpy(path, (const char *)lv_event_get_user_data(e), sizeof s_open_path);
    lv_async_call([](void *) { start_close(); nv_open_file(s_open_path); }, nullptr);
}
void free_ud_cb(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

void start_act_cb(lv_event_t *e) {
    static intptr_t act;
    act = (intptr_t)lv_event_get_user_data(e);
    lv_async_call([](void *) {
        start_close();
        switch (act) {
            case 1: nvui::lock(); break;
            case 2: nvui::sleep_now(); break;
            case 3: nv_ui_open_app_id("settings"); break;
            default: break;
        }
    }, nullptr);
}

// Hover / press / keyboard-focus wash for clickable cells of the menu.
void cell_states(lv_obj_t *o) {
    lv_obj_set_style_radius(o, 4, 0);
    lv_obj_set_style_bg_color(o, th()->text_strong, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_10, LV_STATE_HOVERED);
    lv_obj_set_style_bg_opa(o, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(o, LV_OPA_10, LV_STATE_FOCUS_KEY);
}

lv_obj_t *app_row(lv_obj_t *parent, const NvApp *a) {
    lv_obj_t *r = row(parent, a, nullptr, nvui::label(a), start_app_cb, (void *)a);
    lv_obj_add_event_cb(r, start_app_menu_cb, LV_EVENT_LONG_PRESSED, (void *)a);
    lv_obj_set_user_data(r, (void *)a);
    return r;
}

// Section title with an optional link on the right ("All apps ›", "‹ Back").
lv_obj_t *start_header(lv_obj_t *parent, const char *title, const char *link, lv_event_cb_t cb) {
    lv_obj_t *h = box(parent);
    lv_obj_set_size(h, lv_pct(100), 32);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(h, 4, 0);
    text(h, title, th()->text_strong);
    if (link) {                                      // a flat link, washed on hover / focus
        lv_obj_t *b = box(h);
        lv_obj_set_size(b, LV_SIZE_CONTENT, 28);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        cell_states(b);
        lv_obj_set_style_pad_hor(b, 8, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(b, 6, 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
        text(b, link, th()->accent);
    }
    return h;
}

void show_all_cb(lv_event_t *);
void show_home_cb(lv_event_t *);

void start_view_home(void) {
    lv_obj_t *b = S.start_body;
    char all[48];
    lv_snprintf(all, sizeof all, "%s  " LV_SYMBOL_RIGHT, nv_tr(NV_STR_ALL_APPS));
    start_header(b, nv_tr(NV_STR_PINNED), all, show_all_cb);
    lv_obj_t *grid = box(b);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 4, 0);
    lv_obj_set_style_pad_row(grid, 4, 0);
    const NvApp *pins[kMaxPins];
    const int np = list_load(kPinList, pins);
    for (int i = 0; i < np; i++) {
        lv_obj_t *t = box(grid);
        lv_obj_set_size(t, kTileW, kTileH);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        cell_states(t);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(t, 4, 0);
        lv_obj_add_event_cb(t, start_app_cb, LV_EVENT_CLICKED, (void *)pins[i]);
        lv_obj_add_event_cb(t, start_app_menu_cb, LV_EVENT_LONG_PRESSED, (void *)pins[i]);
        lv_obj_set_user_data(t, (void *)pins[i]);
        lv_obj_t *img = lv_image_create(t);
        lv_image_set_src(img, nvui::icon(pins[i], 40));
        lv_obj_t *l = text(t, nvui::label(pins[i]), th()->text_strong);
        lv_obj_set_size(l, kTileW - 6, lv_font_get_line_height(th()->font_default));   // one line…
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);                             // …with dots
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        if (i == 0) nv_focus_prefer(t);
    }

    // Recommended: the recent apps, then the most used ones not already listed.
    const NvApp *rec[6];
    int nr = 0;
    const NvApp *tmp[8];
    int n = nvui::recents(tmp, 8);
    for (int i = 0; i < n && nr < 6; i++) rec[nr++] = tmp[i];
    n = nvui::most_used(tmp, 8);
    for (int i = 0; i < n && nr < 6; i++) {
        bool dup = false;
        for (int k = 0; k < nr; k++) dup = dup || rec[k] == tmp[i];
        if (!dup) rec[nr++] = tmp[i];
    }
    if (!nr) return;
    start_header(b, nv_tr(NV_STR_RECOMMENDED), nullptr, nullptr);
    lv_obj_t *two = box(b);
    lv_obj_set_size(two, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(two, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(two, 8, 0);
    for (int i = 0; i < nr; i++) {
        lv_obj_t *r = app_row(two, rec[i]);
        lv_obj_set_width(r, lv_pct(48));
    }
}

void start_view_all(void) {
    lv_obj_t *b = S.start_body;
    char back[48];
    lv_snprintf(back, sizeof back, LV_SYMBOL_LEFT "  %s", nv_tr(NV_STR_BACK));
    start_header(b, nv_tr(NV_STR_ALL_APPS), back, show_home_cb);
    const int n = nv_app_count();
    const NvApp **v = (const NvApp **)lv_malloc(sizeof(NvApp *) * (n ? n : 1));
    if (!v) return;
    for (int i = 0; i < n; i++) v[i] = nv_app_at(i);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && lv_strcmp(nvui::label(v[j - 1]), nvui::label(v[j])) > 0; j--) {
            const NvApp *t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
        }
    char letter = 0;
    for (int i = 0; i < n; i++) {
        const char c = (char)toupper((unsigned char)nvui::label(v[i])[0]);
        if (c != letter) {                           // A, B, C... like the classic program list
            letter = c;
            const char s[2] = {c, 0};
            lv_obj_t *l = text(b, s, th()->accent);
            lv_obj_set_style_pad_left(l, 8, 0);
            lv_obj_set_style_pad_top(l, 4, 0);
        }
        lv_obj_t *r = app_row(b, v[i]);
        if (i == 0) nv_focus_prefer(r);
    }
    lv_free(v);
}

const char *file_symbol(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return LV_SYMBOL_FILE;
    char e[8] = {};
    for (int i = 0; i < 7 && dot[1 + i]; i++) e[i] = (char)tolower((unsigned char)dot[1 + i]);
    static const char *const kImg[] = {"jpg", "jpeg", "png", "bmp", "gif"};
    static const char *const kAud[] = {"mp3", "wav", "flac", "aac", "m4a", "ogg"};
    static const char *const kVid[] = {"mp4", "avi", "mpg", "mpeg", "mjpeg", "mjpg", "mkv"};
    for (const char *x : kImg) if (!strcmp(e, x)) return LV_SYMBOL_IMAGE;
    for (const char *x : kAud) if (!strcmp(e, x)) return LV_SYMBOL_AUDIO;
    for (const char *x : kVid) if (!strcmp(e, x)) return LV_SYMBOL_VIDEO;
    return LV_SYMBOL_FILE;
}

// Case-insensitive (ASCII) "contains" for app names.
bool contains_ci(const char *hay, const char *needle) {
    const size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t k = 0;
        while (k < nl && hay[k] && tolower((unsigned char)hay[k]) == tolower((unsigned char)needle[k])) k++;
        if (k == nl) return true;
    }
    return false;
}

void start_view_search(const char *q) {
    lv_obj_t *b = S.start_body;
    S.first_app = nullptr;
    S.first_path[0] = 0;
    int found = 0;
    // Apps: name or id.
    int na = 0;
    for (int i = 0; i < nv_app_count() && na < 6; i++) {
        const NvApp *a = nv_app_at(i);
        if (!contains_ci(nvui::label(a), q) && !contains_ci(a->id, q)) continue;
        if (!na) start_header(b, nv_tr(NV_STR_APPS_SECTION), nullptr, nullptr);
        lv_obj_t *r = app_row(b, a);
        if (!S.first_app) { S.first_app = a; nv_focus_prefer(r); }
        na++;
    }
    found += na;
    // Files on the SD card (name match).
    const char *paths[8];
    const int nf = nvsearch::find(q, paths, 8);
    if (nf) start_header(b, nv_tr(NV_STR_FILES_SECTION), nullptr, nullptr);
    for (int i = 0; i < nf; i++) {
        const char *base = strrchr(paths[i], '/');
        base = base ? base + 1 : paths[i];
        char *own = (char *)lv_malloc(strlen(paths[i]) + 1);
        if (!own) break;
        strcpy(own, paths[i]);
        lv_obj_t *r = row(b, nullptr, file_symbol(own), base, start_file_cb, own);
        lv_obj_add_event_cb(r, free_ud_cb, LV_EVENT_DELETE, own);
        // Where it is, dimmed, after the name.
        char dir[96];
        const size_t dl = (size_t)(base - own) > 1 ? (size_t)(base - own) - 1 : 0;
        lv_snprintf(dir, sizeof dir, "%.*s", (int)LV_MIN(dl, sizeof dir - 1), own);
        lv_obj_t *d = lv_label_create(r);
        lv_label_set_text(d, dir + (strncmp(dir, "/sdcard", 7) ? 0 : 7));
        lv_obj_set_style_text_color(d, th()->text_dim, 0);
        lv_obj_set_style_text_color(d, th()->on_primary, LV_STATE_HOVERED);
        if (!S.first_app && !S.first_path[0]) lv_strlcpy(S.first_path, own, sizeof S.first_path);
    }
    found += nf;
    if (!found) {
        lv_obj_t *l = text(b, nvsearch::ready() ? nv_tr(NV_STR_NO_RESULTS) : nv_tr(NV_STR_INDEXING),
                           th()->text_dim);
        lv_obj_set_style_pad_all(l, 12, 0);
    }
}

void start_render(void) {
    if (!S.start_body) return;
    lv_obj_clean(S.start_body);
    lv_obj_scroll_to_y(S.start_body, 0, LV_ANIM_OFF);
    const char *q = S.start_search ? lv_textarea_get_text(S.start_search) : "";
    if (q && q[0]) { S.view = SV_SEARCH; start_view_search(q); }
    else if (S.view == SV_ALL) start_view_all();
    else { S.view = SV_HOME; start_view_home(); }
}
void start_refresh(void) { if (S.start) start_render(); }

void show_all_cb(lv_event_t *)  { S.view = SV_ALL;  lv_async_call([](void *) { start_render(); }, nullptr); }
void show_home_cb(lv_event_t *) { S.view = SV_HOME; lv_async_call([](void *) { start_render(); }, nullptr); }

void search_changed_cb(lv_event_t *) {
    if (S.view == SV_SEARCH || lv_textarea_get_text(S.start_search)[0]) start_render();
}
void search_ready_cb(lv_event_t *) {                 // Enter: open the best match
    static const NvApp *a;
    a = S.first_app;
    if (a) { lv_async_call([](void *) { start_close(); nv_ui_open_app(a); }, nullptr); return; }
    if (S.first_path[0]) {
        lv_strlcpy(s_open_path, S.first_path, sizeof s_open_path);
        lv_async_call([](void *) { start_close(); nv_open_file(s_open_path); }, nullptr);
    }
}

// Above the taskbar normally; with the on-screen keyboard up, at the top and only as tall as the
// space above the keyboard, so search results stay readable while typing.
void start_place(void) {
    if (!S.start_panel) return;
    const int32_t bottom = S.ime_h > 0 ? scr_h() - S.ime_h : scr_h() - nvclassic::kTaskH;
    const int32_t h = LV_MIN(kStartH, bottom - 4);
    lv_obj_set_height(S.start_panel, h);
    lv_obj_set_height(S.start_col, h);
    lv_obj_set_pos(S.start_panel, 2, bottom - h - 2);
}

lv_obj_t *icon_button(lv_obj_t *parent, const char *sym, intptr_t act, const char *tip) {
    lv_obj_t *b = button(parent, 40, 34, start_act_cb, (void *)act);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(b, 0, 0);
    text(b, sym, th()->text_strong);
    tooltip(b, tip);
    return b;
}

bool nvclassic_start_open(void) {
    if (S.start) return false;
    menu_close();
    tip_hide();
    nvsearch::refresh();                             // file index, in the background if stale
    S.view = SV_HOME;
    S.start = scrim(start_scrim_cb, lv_screen_active());
    lv_obj_move_foreground(S.start);
    if (S.start_btn) lv_obj_add_state(S.start_btn, LV_STATE_CHECKED);

    lv_obj_t *p = box(S.start);
    S.start_panel = p;
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(p, kStartW, kStartH);
    lv_obj_set_pos(p, 2, scr_h() - nvclassic::kTaskH - kStartH - 2);
    lv_obj_set_style_bg_color(p, th()->surface, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, th()->surface3, 0);

    lv_obj_t *col = box(p);
    S.start_col = col;
    lv_obj_set_size(col, kStartW, kStartH);
    lv_obj_set_pos(col, 0, 0);
    lv_obj_set_style_pad_all(col, 10, 0);
    lv_obj_set_style_pad_row(col, 6, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);

    // The field sits in a fixed-size holder: the IME pads a field's parent by the keyboard height,
    // which here would squash the menu. The menu makes room itself (start_place).
    lv_obj_t *holder = box(col);
    lv_obj_set_size(holder, lv_pct(100), 44);
    S.start_search = nv_kit_textarea_ex(holder, nv_tr(NV_STR_SEARCH_HINT), true, NV_IME_TEXT, NV_IME_RET_SEARCH);
    lv_obj_set_width(S.start_search, lv_pct(100));
    lv_obj_add_event_cb(S.start_search, search_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(S.start_search, search_ready_cb, LV_EVENT_READY, nullptr);

    S.start_body = box(col);
    lv_obj_set_width(S.start_body, lv_pct(100));
    lv_obj_set_flex_grow(S.start_body, 1);
    lv_obj_add_flag(S.start_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(S.start_body, LV_DIR_VER);
    lv_obj_set_flex_flow(S.start_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(S.start_body, 2, 0);

    // Footer: who we are on the left, the power-ish actions on the right.
    hline(col);
    lv_obj_t *foot = box(col);
    lv_obj_set_size(foot, lv_pct(100), 40);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(foot, 8, 0);
    char who[40];
    lv_snprintf(who, sizeof who, "NucleoOS %s", esp_app_get_description()->version);
    lv_obj_t *ver = text(foot, who, th()->text_dim);
    lv_obj_set_flex_grow(ver, 1);
    icon_button(foot, LV_SYMBOL_SETTINGS, 3, nvui::label(nv_ui_find_app("settings")));
    icon_button(foot, LV_SYMBOL_EYE_CLOSE, 1, nv_tr(NV_STR_LOCK_NOW));
    icon_button(foot, LV_SYMBOL_POWER, 2, nv_tr(NV_STR_SCREEN_OFF));

    start_place();
    start_render();
    // A physical keyboard types straight into the search (like the Start key + typing).
    if (nv_hid_host_keyboard_present()) nv_focus_set(S.start_search);
    return true;
}

void start_btn_cb(lv_event_t *) {
    if (S.start) lv_async_call(start_close_async, nullptr);
    else nvclassic_start_open();
}

// ---------------------------------------------------------------- taskbar

// Like the classic taskbar: the running app's button minimizes it / brings it back; another
// app's button switches to it (one app runs at a time: the current one closes).
// ---- tasks: one app runs at a time (RAM), the others are suspended. A suspended task keeps its
// button and its last-screen preview; clicking it brings the app back. Only Close ends a task.
void run_add(const NvApp *a) {
    for (int i = 0; i < S.nrun; i++) if (S.run[i] == a) return;
    if (S.nrun == 8) { memmove(S.run, S.run + 1, 7 * sizeof *S.run); S.nrun--; }
    S.run[S.nrun++] = a;
}
void run_remove(const NvApp *a) {
    int w = 0;
    for (int i = 0; i < S.nrun; i++) if (S.run[i] != a) S.run[w++] = S.run[i];
    S.nrun = w;
}

void preview_hide(void) { if (S.preview) lv_obj_add_flag(S.preview, LV_OBJ_FLAG_HIDDEN); }

void preview_cb(lv_event_t *e) {
    const NvApp *a = (const NvApp *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) != LV_EVENT_HOVER_OVER) { preview_hide(); return; }
    const lv_image_dsc_t *img = (a == nv_ui_current_app() && !nvui::minimized()) ? nullptr : nvui::thumb(a);
    if (!img) return;                                // the tooltip names it
    tip_hide();
    if (!S.preview) {                                // just the picture, in a thin frame
        S.preview = lv_image_create(lv_layer_top());
        lv_obj_set_style_border_width(S.preview, 1, 0);
        lv_obj_set_style_border_color(S.preview, th()->surface3, 0);
        lv_obj_set_style_pad_all(S.preview, 3, 0);
        lv_obj_set_style_bg_color(S.preview, th()->surface, 0);
        lv_obj_set_style_bg_opa(S.preview, LV_OPA_COVER, 0);
        lv_obj_clear_flag(S.preview, LV_OBJ_FLAG_CLICKABLE);
        nv_focus_skip(S.preview);
    }
    lv_image_set_src(S.preview, img);
    lv_obj_clear_flag(S.preview, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(S.preview);
    lv_area_t r;
    lv_obj_get_coords(lv_event_get_current_target_obj(e), &r);
    int32_t x = LV_MIN(r.x1, scr_w() - 2 - lv_obj_get_width(S.preview));
    lv_obj_set_pos(S.preview, x, r.y1 - lv_obj_get_height(S.preview) - 6);
    lv_obj_move_foreground(S.preview);
}

void close_task_fn(const NvApp *a) {
    run_remove(a);
    if (a == nv_ui_current_app()) nv_ui_close_app();   // the close callback refreshes the bar
    else tasks_refresh();
}

void task_click_cb(lv_event_t *e) {
    const NvApp *a = (const NvApp *)lv_event_get_user_data(e);
    if (!a) return;
    if (a != nv_ui_current_app()) { nv_ui_open_app(a); return; }
    if (nvui::minimized()) nvui::restore();
    else nvui::minimize();
}
void task_menu_cb(lv_event_t *e) {
    lv_obj_t *b = lv_event_get_current_target_obj(e);
    const NvApp *a = (const NvApp *)lv_event_get_user_data(e);
    menu_for_app(center(b), a, true);              // every task can be closed from its button
}

void tasks_refresh(void) {
    if (!S.tasks) return;
    tip_hide();
    lv_obj_clean(S.tasks);
    const NvApp *cur = nv_ui_current_app();
    preview_hide();
    if (cur) run_add(cur);
    const NvApp *const *v = S.run;
    const int n = S.nrun;
    // Buttons share the free width, 170 px at most (they narrow as more tasks are listed).
    lv_obj_update_layout(S.bar);
    int32_t bw = n ? (lv_obj_get_content_width(S.tasks) - 4 * (n - 1)) / n : 170;
    if (bw > 170) bw = 170;
    if (bw < 60) bw = 60;
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = button(S.tasks, bw, nvclassic::kTaskH - 8, task_click_cb, (void *)v[i]);
        lv_obj_add_event_cb(b, task_menu_cb, LV_EVENT_LONG_PRESSED, (void *)v[i]);
        lv_obj_set_user_data(b, (void *)v[i]);
        lv_obj_t *img = lv_image_create(b);
        lv_image_set_src(img, nvui::icon(v[i], 20));
        lv_obj_t *l = text(b, nvui::label(v[i]), th()->text_strong);
        lv_obj_set_height(l, lv_font_get_line_height(th()->font_default));   // one line, dots
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_flex_grow(l, 1);
        if (v[i] != cur) lv_obj_set_style_text_opa(b, LV_OPA_60, 0);   // suspended: dimmed
        lv_obj_add_event_cb(b, preview_cb, LV_EVENT_HOVER_OVER, (void *)v[i]);
        lv_obj_add_event_cb(b, preview_cb, LV_EVENT_HOVER_LEAVE, (void *)v[i]);
        lv_obj_add_event_cb(b, preview_cb, LV_EVENT_PRESSED, (void *)v[i]);
        if (v[i] == cur && !nvui::minimized()) {     // the window on screen: pressed in, accent mark
            lv_obj_add_state(b, LV_STATE_CHECKED);
            lv_obj_add_event_cb(b, active_mark_cb, LV_EVENT_DRAW_POST, nullptr);
        }
        // The name shows only when there's no picture to show (the running window).
        if (!(v[i] != cur || nvui::minimized()) || !nvui::thumb(v[i])) tooltip(b, nvui::label(v[i]));
    }
}

void tray_click_cb(lv_event_t *) { start_close(); menu_close(); nvui::open_shade(); }

void tray_tick(lv_timer_t *) {
    if (!S.bar || nvui::asleep()) return;
    char b[24];
    nv_time_format(b, sizeof b, nv_time_is_24h() ? "%H:%M" : "%I:%M %p");
    lv_label_set_text(S.t_clock, b);
    struct tm tmv;
    nv_time_now(&tmv);
    lv_snprintf(b, sizeof b, "%02d/%02d/%04d", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900);
    lv_label_set_text(S.t_date, b);

    const int unread = nv_notify_count();
    if (unread > 0) {
        lv_label_set_text_fmt(S.t_bell, LV_SYMBOL_BELL " %d", unread);
        lv_obj_set_style_text_color(S.t_bell, th()->accent, 0);
    } else {
        lv_label_set_text(S.t_bell, LV_SYMBOL_BELL);
        lv_obj_set_style_text_color(S.t_bell, th()->text_dim, 0);
    }
    if (nv_sd_is_mounted()) lv_obj_clear_flag(S.t_sd, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(S.t_sd, LV_OBJ_FLAG_HIDDEN);
    if (nv_usb_storage_mounted_count() > 0) lv_obj_clear_flag(S.t_usb, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(S.t_usb, LV_OBJ_FLAG_HIDDEN);
    const nv_wifi_state_t st = nv_wifi_is_enabled() ? nv_wifi_get_state() : NV_WIFI_DISABLED;
    lv_color_t c = th()->text_dim;
    if (st == NV_WIFI_CONNECTED) c = nv_time_is_synced() ? th()->success_solid : th()->accent;
    else if (st == NV_WIFI_FAILED) c = th()->danger;
    else if (st == NV_WIFI_CONNECTING || st == NV_WIFI_SCANNING) c = th()->accent;
    lv_obj_set_style_text_color(S.t_wifi, c, 0);
    const bool mute = nv_config_get_bool("mute", false);
    const int vol = nv_config_get_int("volume", 60);
    lv_label_set_text(S.t_vol, mute || vol == 0 ? LV_SYMBOL_MUTE : vol < 50 ? LV_SYMBOL_VOLUME_MID : LV_SYMBOL_VOLUME_MAX);
}

void bar_visibility(void) {
    if (!S.bar) return;
    if (S.fs || S.ime_up) lv_obj_add_flag(S.bar, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(S.bar, LV_OBJ_FLAG_HIDDEN);
}

// The on-screen keyboard docks at the bottom: the taskbar (top layer) would cover its last row.
void on_ime(nv_event_t, const void *data, void *) {
    const nv_ime_visibility_t *v = (const nv_ime_visibility_t *)data;
    S.ime_up = v && v->visible;
    S.ime_h = S.ime_up ? v->height : 0;
    bar_visibility();
    start_place();
}

// ---- tray popups: calendar (clock), volume, network. One at a time, in the context-menu slot
// (S.menu), so a click outside, Esc or another popup closes it.

// A panel anchored above a tray item, right-aligned with it, inside the screen.
lv_obj_t *tray_popup(lv_obj_t *anchor, int32_t w) {
    menu_close();
    tip_hide();
    S.n_items = 0;
    S.menu = scrim(menu_scrim_cb);
    lv_obj_t *pn = panel(S.menu, w);
    lv_obj_set_style_pad_all(pn, 10, 0);
    lv_obj_set_style_pad_row(pn, 8, 0);
    lv_area_t a;
    lv_obj_get_coords(anchor, &a);
    lv_obj_set_user_data(pn, (void *)(intptr_t)a.x2);   // right edge to align to, after layout
    return pn;
}
void tray_popup_place(lv_obj_t *pn) {
    lv_obj_update_layout(pn);
    int32_t x = (int32_t)(intptr_t)lv_obj_get_user_data(pn) - lv_obj_get_width(pn);
    x = LV_MAX(2, LV_MIN(x, scr_w() - 2 - lv_obj_get_width(pn)));
    lv_obj_set_pos(pn, x, scr_h() - nvclassic::kTaskH - lv_obj_get_height(pn) - 4);
}

// 1. Clock -> today's full date over a month calendar, in the UI language (LVGL's own header
// only knows English month names, so the month bar is ours).
NV_PSRAM_BSS int s_cal_y, s_cal_m;
NV_PSRAM_BSS lv_obj_t *s_cal;
NV_PSRAM_BSS lv_obj_t *s_cal_title;
void cal_show(void) {
    lv_calendar_set_month_shown(s_cal, s_cal_y, s_cal_m);
    lv_label_set_text_fmt(s_cal_title, "%s %d", nv_i18n_month_short(s_cal_m - 1), s_cal_y);
}
void cal_step_cb(lv_event_t *e) {
    s_cal_m += (int)(intptr_t)lv_event_get_user_data(e);
    if (s_cal_m < 1)  { s_cal_m = 12; s_cal_y--; }
    if (s_cal_m > 12) { s_cal_m = 1;  s_cal_y++; }
    cal_show();
}
void clock_popup_cb(lv_event_t *e) {
    lv_obj_t *pn = tray_popup(lv_event_get_current_target_obj(e), 320);
    struct tm t;
    nv_time_now(&t);
    char b[48];
    lv_snprintf(b, sizeof b, "%s %d %s %d", nv_i18n_wday_short(t.tm_wday), t.tm_mday,
                nv_i18n_month_short(t.tm_mon), t.tm_year + 1900);
    text(pn, b, th()->text_strong, &nv_font_20);

    lv_obj_t *bar = box(pn);
    lv_obj_set_size(bar, lv_pct(100), 32);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *prev = button(bar, 34, 28, cal_step_cb, (void *)(intptr_t)-1);
    lv_obj_set_flex_align(prev, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    text(prev, LV_SYMBOL_LEFT, th()->text_strong);
    s_cal_title = text(bar, "", th()->text_strong);
    lv_obj_t *next = button(bar, 34, 28, cal_step_cb, (void *)(intptr_t)1);
    lv_obj_set_flex_align(next, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    text(next, LV_SYMBOL_RIGHT, th()->text_strong);

    s_cal = lv_calendar_create(pn);
    lv_obj_set_size(s_cal, 300, 230);
    static const char *days[7];
    for (int i = 0; i < 7; i++) days[i] = nv_i18n_wday_short(i);   // Sunday first, like LVGL
    lv_calendar_set_day_names(s_cal, days);
    lv_calendar_set_today_date(s_cal, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    lv_obj_set_style_bg_opa(s_cal, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cal, 0, 0);
    lv_obj_set_style_pad_all(s_cal, 0, 0);
    // Flat day cells: no boxes; other months dimmed by LVGL; today in the accent.
    lv_obj_t *m = lv_calendar_get_btnmatrix(s_cal);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_border_width(m, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(m, 3, LV_PART_ITEMS);   // ~invisible (LVGL skips fills under 2), but keeps a fill to recolour
    lv_obj_set_style_text_color(m, th()->text_strong, LV_PART_ITEMS);
    lv_obj_set_style_radius(m, 4, LV_PART_ITEMS);
    // Today: an accent disc (LVGL draws highlighted dates in the CHECKED state).
    static lv_calendar_date_t today;
    today = {(uint16_t)(t.tm_year + 1900), (uint8_t)(t.tm_mon + 1), (uint8_t)t.tm_mday};
    lv_calendar_set_highlighted_dates(s_cal, &today, 1);
    // Runs after LVGL's own handler (added later): the highlighted day in the NucleoOS accent.
    lv_obj_add_event_cb(m, [](lv_event_t *e) {
        lv_draw_task_t *dt = lv_event_get_draw_task(e);
        lv_draw_dsc_base_t *base = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(dt);
        if (base->part != LV_PART_ITEMS) return;
        lv_obj_t *mo = lv_event_get_current_target_obj(e);
        if (!lv_buttonmatrix_has_button_ctrl(mo, base->id1, LV_BUTTONMATRIX_CTRL_CUSTOM_2)) return;
        if (lv_draw_fill_dsc_t *f = lv_draw_task_get_fill_dsc(dt)) { f->color = th()->accent; f->opa = LV_OPA_COVER; }
        if (lv_draw_label_dsc_t *l = lv_draw_task_get_label_dsc(dt)) l->color = th()->on_primary;
        if (lv_draw_border_dsc_t *b = lv_draw_task_get_border_dsc(dt)) b->opa = LV_OPA_TRANSP;
    }, LV_EVENT_DRAW_TASK_ADDED, nullptr);
    s_cal_y = t.tm_year + 1900;
    s_cal_m = t.tm_mon + 1;
    cal_show();
    nv_focus_prefer(next);
    tray_popup_place(pn);
}

// 2. Volume -> live slider (saved on release) + mute.
void vol_slider_cb(lv_event_t *e) {
    lv_obj_t *sl = lv_event_get_target_obj(e);
    const int v = (int)lv_slider_get_value(sl);
    nv_audio_set_volume(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        nv_config_set_int("volume", v);
        nv_audio_click();                            // a sample of the new level
    }
}
void vol_mute_cb(lv_event_t *e) {
    const bool m = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    nv_audio_set_mute(m);
    nv_config_set_bool("mute", m);
}
void volume_popup_cb(lv_event_t *e) {
    lv_obj_t *pn = tray_popup(lv_event_get_current_target_obj(e), 280);
    text(pn, nv_tr(NV_STR_VOLUME), th()->text_strong);
    lv_obj_t *sl = lv_slider_create(pn);
    lv_obj_set_width(sl, lv_pct(100));
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, nv_config_get_int("volume", 60), LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, vol_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(sl, vol_slider_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_set_style_margin_ver(sl, 8, 0);
    lv_obj_t *mrow = box(pn);
    lv_obj_set_size(mrow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mrow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    text(mrow, LV_SYMBOL_MUTE, th()->text_strong);
    lv_obj_t *sw = lv_switch_create(mrow);
    if (nv_config_get_bool("mute", false)) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, vol_mute_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    nv_focus_prefer(sl);
    tray_popup_place(pn);
}

// 3. Wi-Fi -> network, address and a way into the network settings.
void wifi_popup_cb(lv_event_t *e) {
    lv_obj_t *pn = tray_popup(lv_event_get_current_target_obj(e), 300);
    char ssid[33] = "", ip[16] = "";
    const bool on = nv_wifi_is_enabled();
    const bool up = on && nv_wifi_get_state() == NV_WIFI_CONNECTED &&
                    nv_wifi_get_connected(ssid, sizeof ssid, ip, sizeof ip, nullptr);
    lv_obj_t *h = box(pn);
    lv_obj_set_size(h, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(h, 10, 0);
    text(h, LV_SYMBOL_WIFI, up ? th()->success_solid : th()->text_dim, &nv_font_20);
    text(h, up ? ssid : "Wi-Fi", th()->text_strong, &nv_font_20);
    if (up) {
        char b[40];
        lv_snprintf(b, sizeof b, "IP %s", ip);
        text(pn, b, th()->text_dim);
    } else {
        text(pn, on ? "—" : nv_tr(NV_STR_WIFI_OFF), th()->text_dim);
    }
    lv_obj_t *go = row(pn, nullptr, LV_SYMBOL_SETTINGS, nv_tr(NV_STR_NET_SETTINGS), [](lv_event_t *) {
        lv_async_call([](void *) { menu_close(); nv_ui_open_app_id("settings"); }, nullptr);
    }, nullptr);
    nv_focus_prefer(go);
    tray_popup_place(pn);
}

lv_obj_t *tray_icon(const char *sym, const char *tip, lv_event_cb_t cb = nullptr) {
    lv_obj_t *l = text(S.tray, sym, th()->text_dim);
    lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(l, cb ? cb : tray_click_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_ext_click_area(l, 8);
    if (tip) tooltip(l, tip);
    return l;
}

void bar_bg_menu_cb(lv_event_t *e) {
    lv_obj_t *o = lv_event_get_current_target_obj(e);
    menu_for_taskbar(center(o));
}

void bar_build(void) {
    const int32_t h = nvclassic::kTaskH;
    S.bar = box(lv_layer_top());
    lv_obj_set_size(S.bar, scr_w(), h);
    lv_obj_set_style_text_font(S.bar, th()->font_default, 0);   // top layer: no inherited Latin-1 font
    lv_obj_set_pos(S.bar, 0, scr_h() - h);
    lv_obj_add_flag(S.bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(S.bar, bar_bg_menu_cb, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_set_style_bg_color(S.bar, th()->surface, 0);
    lv_obj_set_style_bg_opa(S.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(S.bar, 4, 0);
    lv_obj_set_style_pad_column(S.bar, 4, 0);
    lv_obj_set_flex_flow(S.bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(S.bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // A full 1 px light frame (the classic taskbar highlight), on every side.
    lv_obj_set_style_border_width(S.bar, 1, 0);
    lv_obj_set_style_border_side(S.bar, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_border_color(S.bar, lv_color_mix(lv_color_white(), th()->surface2, 110), 0);
    // The taskbar is always on screen: keep it out of keyboard layers (Win opens Start, Alt+Tab
    // the tasks), or every key would land on it instead of the app.
    nv_focus_skip(S.bar);
    lv_obj_move_to_index(S.bar, 0);   // under every top-layer overlay (lock screen, Recents, menus)

    S.start_btn = button(S.bar, 104, h - 8, start_btn_cb, nullptr);
    lv_obj_t *logo = lv_image_create(S.start_btn);
    lv_image_set_src(logo, &nv_logo_22);
    text(S.start_btn, nv_tr(NV_STR_START), th()->text_strong, &nv_font_20);

    lv_obj_t *sep = box(S.bar);
    lv_obj_set_size(sep, 2, h - 10);
    bevel(sep, true);

    S.tasks = box(S.bar);
    lv_obj_set_flex_grow(S.tasks, 1);
    lv_obj_set_height(S.tasks, h);
    lv_obj_set_flex_flow(S.tasks, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(S.tasks, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(S.tasks, 4, 0);

    // Tray: notifications, drives, Wi-Fi, clock + date. Any of them opens the notification shade
    // (quick settings live there).
    S.tray = box(S.bar);
    lv_obj_set_height(S.tray, h - 8);
    lv_obj_set_width(S.tray, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(S.tray, 10, 0);
    lv_obj_set_style_pad_column(S.tray, 12, 0);
    lv_obj_set_flex_flow(S.tray, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(S.tray, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Notification area: a quiet inset well (darker fill, one even hairline), so it never stacks a
    // second bevel against the taskbar's own frame.
    lv_obj_set_style_bg_color(S.tray, th()->bg, 0);
    lv_obj_set_style_bg_opa(S.tray, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(S.tray, 1, 0);
    lv_obj_set_style_border_color(S.tray, th()->surface3, 0);
    lv_obj_set_style_margin_right(S.tray, 4, 0);
    S.t_bell = tray_icon(LV_SYMBOL_BELL, nv_tr(NV_STR_NOTIFICATIONS));
    S.t_usb  = tray_icon(LV_SYMBOL_USB, nullptr);
    S.t_sd   = tray_icon(LV_SYMBOL_SD_CARD, nullptr);
    S.t_wifi = tray_icon(LV_SYMBOL_WIFI, "Wi-Fi", wifi_popup_cb);
    S.t_vol  = tray_icon(LV_SYMBOL_VOLUME_MAX, nv_tr(NV_STR_VOLUME), volume_popup_cb);
    lv_obj_t *clk = box(S.tray);
    lv_obj_set_size(clk, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(clk, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(clk, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_add_flag(clk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clk, clock_popup_cb, LV_EVENT_CLICKED, nullptr);
    S.t_clock = text(clk, "", th()->text_strong);
    S.t_date = text(clk, "", th()->text_dim, &nv_font_14);
    tooltip(clk, "");

    // "Show desktop": the thin button in the far corner.
    lv_obj_t *peek = button(S.bar, 10, h - 8, [](lv_event_t *) {
        lv_async_call([](void *) { nvui::minimize(); }, nullptr);
    }, nullptr);
    lv_obj_set_style_pad_hor(peek, 0, 0);
    tooltip(peek, nv_tr(NV_STR_SHOW_DESKTOP));

    tasks_refresh();
    tray_tick(nullptr);
    S.tick = lv_timer_create(tray_tick, 1000, nullptr);
}

// ---------------------------------------------------------------- title bar

void title_back_cb(lv_event_t *) { nvui::back(); }
void title_min_cb(lv_event_t *) { lv_async_call([](void *) { nvui::minimize(); }, nullptr); }
void title_close_cb(lv_event_t *) { lv_async_call([](void *) { nv_ui_close_app(); }, nullptr); }

void title_menu_cb(lv_event_t *e) {
    lv_obj_t *h = lv_event_get_current_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(h, &a);
    const MenuItem m[] = {
        {LV_SYMBOL_LEFT, nv_tr(NV_STR_BACK), back_fn, nullptr, false},
        {LV_SYMBOL_CLOSE, nv_tr(NV_STR_CLOSE), close_fn, nullptr, true},
    };
    menu_open({a.x1 + 40, a.y2}, m, 2);
}

lv_obj_t *title_button(lv_obj_t *hdr, const char *sym, lv_event_cb_t cb, const char *tip) {
    lv_obj_t *b = button(hdr, 34, nvclassic::kTitleH - 10, cb, nullptr);
    lv_obj_set_style_pad_hor(b, 0, 0);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    text(b, sym, th()->text_strong);
    if (tip) tooltip(b, tip);
    return b;
}

}  // namespace

namespace nvclassic {

void enable(bool on) {
    if (on == S.on) return;
    if (on) {
        pal_refresh();
        static bool subscribed = false;
        if (!subscribed) subscribed = nv_event_subscribe(NV_EV_IME_VISIBILITY, on_ime, nullptr);
        S.on = true;
        desk_build();
        bar_build();
    } else {
        menu_close();
        start_close();
        if (S.tip)  { lv_obj_delete(S.tip); S.tip = nullptr; }
        if (S.preview) { lv_obj_delete(S.preview); S.preview = nullptr; }
        if (S.tick) { lv_timer_delete(S.tick); S.tick = nullptr; }
        if (S.bar)  { lv_obj_delete(S.bar); }
        if (S.desk) { lv_obj_delete(S.desk); }
        const NvApp *run[8]; const int nrun = S.nrun;
        memcpy(run, S.run, sizeof run);
        S = State{};
        memcpy(S.run, run, sizeof run);
        S.nrun = nrun;
    }
}

void rebuild(void) {
    if (!S.on) return;
    enable(false);
    enable(true);
}

lv_obj_t *frame_header(lv_obj_t *hdr, const NvApp *a) {
    pal_refresh();
    S.title_hdr = hdr;
    // Title bar in the NucleoOS accent (active window), bevelled buttons on the right.
    lv_obj_set_style_bg_color(hdr, th()->accent, 0);
    lv_obj_set_style_bg_grad_color(hdr, th()->primary, 0);
    lv_obj_set_style_bg_grad_dir(hdr, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(hdr, 6, 0);
    lv_obj_set_style_pad_column(hdr, 8, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hdr, title_menu_cb, LV_EVENT_LONG_PRESSED, nullptr);

    title_button(hdr, LV_SYMBOL_LEFT, title_back_cb, nv_tr(NV_STR_BACK));
    if (a) {
        lv_obj_t *img = lv_image_create(hdr);
        lv_image_set_src(img, nvui::icon(a, 24));
    }
    lv_obj_t *t = text(hdr, a ? nvui::label(a) : "", th()->on_primary, &nv_font_20);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(t, 1);
    title_button(hdr, LV_SYMBOL_MINUS, title_min_cb, nv_tr(NV_STR_MINIMIZE));
    title_button(hdr, LV_SYMBOL_CLOSE, title_close_cb, nv_tr(NV_STR_CLOSE));
    return t;
}

void on_app_closed(const NvApp *a, bool switching) {
    if (!S.on) return;
    if (a && !switching) run_remove(a);             // closed for real: the task ends
    on_app_changed();
}

void task_activate(int n) {
    if (!S.on || n < 0 || n >= S.nrun) return;
    const NvApp *a = S.run[n];
    if (a == nv_ui_current_app()) { if (nvui::minimized()) nvui::restore(); }
    else nv_ui_open_app(a);
}

void on_app_changed(void) {
    if (!S.on) return;
    start_close();
    menu_close();
    if (!nv_ui_current_app()) S.title_hdr = nullptr;
    tasks_refresh();
}

void set_fullscreen(bool on) {
    S.fs = on;
    if (on) { start_close(); menu_close(); tip_hide(); }
    bar_visibility();
}

lv_color_t icon_color(void) {
    if (!S.on) return nv_theme_get()->accent;
    pal_refresh();
    return s_pal.accent;
}

bool start_toggle(void) {
    if (!S.on) return false;
    if (S.start) start_close();
    else nvclassic_start_open();
    return true;
}

bool escape(void) {
    if (!S.on) return false;
    if (S.menu)  { menu_close(); return true; }
    if (S.start) {
        // Esc steps back: clear the search, then leave "All apps", then close.
        if (S.start_search && lv_textarea_get_text(S.start_search)[0]) {
            lv_textarea_set_text(S.start_search, "");
            S.view = SV_HOME;
            start_render();
        } else if (S.view != SV_HOME) {
            S.view = SV_HOME;
            start_render();
        } else {
            start_close();
        }
        return true;
    }
    return false;
}

static bool inside(lv_obj_t *o, lv_obj_t *root) {
    for (; o; o = lv_obj_get_parent(o)) if (o == root) return true;
    return false;
}

bool context_at(lv_point_t p) {
    if (!S.on) return false;
    tip_hide();
    if (S.menu) { menu_close(); return true; }
    lv_obj_t *o = lv_indev_search_obj(lv_layer_top(), &p);
    if (!o && S.start) o = lv_indev_search_obj(lv_screen_active(), &p);
    if (S.start && inside(o, S.start)) {
        // An app tile / row: its menu at the pointer (open, Start pin, desktop icon).
        for (lv_obj_t *r = o; r && r != S.start; r = lv_obj_get_parent(r)) {
            const uint32_t n = lv_obj_get_event_count(r);
            for (uint32_t i = 0; i < n; i++)
                if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(r, i)) == start_app_menu_cb) {
                    menu_for_app(p, (const NvApp *)lv_obj_get_user_data(r), false);
                    return true;
                }
        }
        return true;
    }
    if (S.bar && inside(o, S.bar)) {
        for (lv_obj_t *b = o; b && b != S.bar; b = lv_obj_get_parent(b))
            if (lv_obj_get_parent(b) == S.tasks) {
                menu_for_app(p, (const NvApp *)lv_obj_get_user_data(b), true);
                return true;
            }
        menu_for_taskbar(p);
        return true;
    }
    if (o) return false;                              // another top-layer surface (lock, Recents)
    o = lv_indev_search_obj(lv_screen_active(), &p);
    if (S.title_hdr && inside(o, S.title_hdr)) {
        const MenuItem m[] = {
            {LV_SYMBOL_LEFT, nv_tr(NV_STR_BACK), back_fn, nullptr, false},
            {LV_SYMBOL_CLOSE, nv_tr(NV_STR_CLOSE), close_fn, nullptr, true},
        };
        menu_open(p, m, 2);
        return true;
    }
    if (S.desk && inside(o, S.desk)) {
        for (lv_obj_t *c = o; c && c != S.desk; c = lv_obj_get_parent(c))
            if (lv_obj_get_parent(c) == S.grid) {
                desk_bg_cb(nullptr);
                lv_obj_add_state(c, LV_STATE_CHECKED);
                menu_for_app(p, (const NvApp *)lv_obj_get_user_data(c), false);
                return true;
            }
        menu_for_desktop(p);
        return true;
    }
    return false;
}

}  // namespace nvclassic
