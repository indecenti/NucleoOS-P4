// dmd_panel — the DMD app's settings panel. See dmd_panel.h. LVGL thread.
#include "dmd_panel.h"

#include "dmd_ui.h"
#include "nv_fonts.h"
#include "nv_i18n.h"

#include <cstdio>
#include <cstring>

using namespace dmdui;

namespace {

// Two columns on the panel; everything fits its 600 rows (no scrolling to find the Wi-Fi address).
constexpr int kW = kScrW, kPad = 24, kColW = (kScrW - 3 * kPad) / 2, kRightX = kPad + kColW + kPad;
constexpr int kTop = 104;          // where the sections start, under the header
constexpr uint32_t kWarn = 0xFF9A6A;   // a problem to fix (the port), not an error dialog

struct Panel {
    const DmdPanelIo *io = nullptr;
    lv_obj_t *status = nullptr, *address = nullptr, *port_note = nullptr;
    lv_obj_t *resume = nullptr, *test = nullptr, *exit = nullptr;
    lv_obj_t *fname = nullptr, *fdesc = nullptr, *ftable = nullptr, *fnote = nullptr;
    lv_obj_t *theme[2] = {}, *color[8] = {}, *shape[2] = {}, *rot[4] = {};
    lv_obj_t *color_cap = nullptr, *bright_val = nullptr;
};
Panel s_p;   // the objects are LVGL's (deleted with the app's page); this only points at them

// "a\nb\nc": the n-th line into out
void line_of(const char *lines, int n, char *out, size_t cap) {
    out[0] = 0;
    const char *q = lines;
    for (int i = 0; q && i < n; i++) {
        q = strchr(q, '\n');
        if (q) q++;
    }
    if (!q) return;
    const char *e = strchr(q, '\n');
    const size_t len = e ? (size_t)(e - q) : strlen(q);
    snprintf(out, cap, "%.*s", (int)(len < cap ? len : cap - 1), q);
}

lv_obj_t *section(lv_obj_t *p, const char *t, int x, int y) { return label(p, &nv_font_20, kInk, t, x, y); }
lv_obj_t *caption(lv_obj_t *p, const char *t, int x, int y) { return label(p, &nv_font_14, kDim, t, x, y); }

// A label of at most `lines` lines, w wide, cut with "..." (never running into what follows it).
void clip_lines(lv_obj_t *l, int w, int lines) {
    lv_obj_set_size(l, w, lv_font_get_line_height(lv_obj_get_style_text_font(l, LV_PART_MAIN)) * lines);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
}

// A row of pills from "a\nb\n..." at x, y, each w wide (0: as its name); out[i] the pills, n at most.
int pill_row(lv_obj_t *p, const char *names, int x, int y, int w, lv_event_cb_t cb, lv_obj_t **out, int n) {
    char t[64];
    int i = 0, cx = x;
    for (; i < n; i++) {
        line_of(names, i, t, sizeof t);
        if (!t[0]) break;
        out[i] = pill(p, t, cx, y, w, cb, (void *)(intptr_t)i);
        lv_obj_update_layout(out[i]);
        cx += lv_obj_get_width(out[i]) + 8;
    }
    return i;
}

// An on / off row: its name and a switch at its right end (x .. x + w). The whole row is the control, so a
// tap on the name toggles it too and the keyboard stops on it once.
void toggle_cb(lv_event_t *e) {
    lv_obj_t *row = lv_event_get_current_target_obj(e);
    lv_obj_t *s = lv_obj_get_child(row, 1);
    const bool on = !lv_obj_has_state(s, LV_STATE_CHECKED);
    if (on) lv_obj_add_state(s, LV_STATE_CHECKED);
    else lv_obj_remove_state(s, LV_STATE_CHECKED);
    void (*fn)(bool) = (void (*)(bool))lv_event_get_user_data(e);
    if (fn) fn(on);
}
lv_obj_t *toggle(lv_obj_t *p, const char *t, int x, int y, int w, bool on, void (*fn)(bool)) {
    lv_obj_t *row = box(p);
    lv_obj_set_pos(row, x - 8, y);
    lv_obj_set_size(row, w + 8, 44);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_bg_color(row, hx(kCard), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, toggle_cb, LV_EVENT_CLICKED, (void *)fn);
    lv_obj_t *l = label(row, &nv_font_20, kInk, t, 8, 9);
    lv_obj_set_width(l, w - 90);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_t *s = lv_switch_create(row);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);   // the row takes the taps
    lv_obj_set_pos(s, w + 8 - 72, 4);
    lv_obj_set_size(s, 72, 36);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, hx(kCard), LV_PART_MAIN);
    lv_obj_set_style_border_width(s, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s, hx(kEdge), LV_PART_MAIN);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, (lv_style_selector_t)LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s, hx(kOn), (lv_style_selector_t)LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s, hx(kDim), LV_PART_KNOB);
    lv_obj_set_style_bg_color(s, hx(kAccent), (lv_style_selector_t)LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_pad_all(s, -4, LV_PART_KNOB);
    if (on) lv_obj_add_state(s, LV_STATE_CHECKED);
    return row;
}

int index_of(lv_event_t *e) { return (int)(intptr_t)lv_event_get_user_data(e); }

void resume_cb(lv_event_t *) { if (s_p.io->resume) s_p.io->resume(); }
void test_cb(lv_event_t *) { if (s_p.io->test) s_p.io->test(); }
void exit_cb(lv_event_t *) { if (s_p.io->exit) s_p.io->exit(); }
void layout_cb(lv_event_t *) { if (s_p.io->layout) s_p.io->layout(); }
void theme_cb(lv_event_t *e) {
    pills_mark(s_p.theme, 2, index_of(e));
    if (s_p.io->set_theme) s_p.io->set_theme(index_of(e));
}
// "Colour · Orange": the swatches carry no names, the caption says the chosen one
void color_caption(int i) {
    if (!s_p.color_cap) return;
    char n[48];
    line_of(nv_tr(NV_STR_DMD_COLORS), i, n, sizeof n);
    lv_label_set_text_fmt(s_p.color_cap, "%s  \xC2\xB7  %s", nv_tr(NV_STR_DMD_COLOR), n);
}
void color_cb(lv_event_t *e) {
    pills_mark(s_p.color, 8, index_of(e));
    color_caption(index_of(e));
    if (s_p.io->set_color) s_p.io->set_color(index_of(e));
}
void shape_cb(lv_event_t *e) {
    pills_mark(s_p.shape, 2, index_of(e));
    if (s_p.io->set_shape) s_p.io->set_shape(index_of(e));
}
void rot_cb(lv_event_t *e) {
    pills_mark(s_p.rot, 4, index_of(e));
    if (s_p.io->set_rot) s_p.io->set_rot(index_of(e));
}
void set_glow(bool on) { if (s_p.io->set_glow) s_p.io->set_glow(on); }
void set_clock(bool on) { if (s_p.io->set_clock) s_p.io->set_clock(on); }
void set_boot(bool on) { if (s_p.io->set_boot) s_p.io->set_boot(on); }
void bright_cb(lv_event_t *e) {
    const int v = (int)lv_slider_get_value(lv_event_get_target_obj(e));
    if (s_p.bright_val) lv_label_set_text_fmt(s_p.bright_val, "%d %%", v);
    if (s_p.io->set_bright) s_p.io->set_bright(v, lv_event_get_code(e) == LV_EVENT_RELEASED);
}

// The three actions at the header's right end, right-aligned (their names change length with the language
// and with the first one's name).
void place_actions(void) {
    if (!s_p.exit) return;
    lv_obj_update_layout(s_p.exit);
    int x = kW - kPad - lv_obj_get_width(s_p.exit);
    lv_obj_set_x(s_p.exit, x);
    x -= 8 + lv_obj_get_width(s_p.test);
    lv_obj_set_x(s_p.test, x);
    x -= 8 + lv_obj_get_width(s_p.resume);
    lv_obj_set_x(s_p.resume, x);
}

}  // namespace

lv_obj_t *dmd_panel_build(lv_obj_t *parent, const DmdPanelIo *io, lv_obj_t **resume) {
    s_p = Panel{};
    s_p.io = io;
    lv_obj_t *p = box(parent);
    lv_obj_set_size(p, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(p, hx(kBg), 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_SCROLLABLE);   // (only if a language's texts ever need more room)
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_style_pad_bottom(p, 8, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);

    // the header: the title, the three actions; under them the link and the Wi-Fi address (what the PC's
    // DmdDevice.ini needs, always in view)
    label(p, &nv_font_28, kAccent, nv_tr(NV_STR_APP_DMD), kPad, 14);
    s_p.resume = pill(p, nv_tr(NV_STR_DMD_RESUME), 0, 18, 0, resume_cb, nullptr);
    s_p.test = pill(p, nv_tr(NV_STR_DMD_TEST), 0, 18, 0, test_cb, nullptr);
    s_p.exit = pill(p, nv_tr(NV_STR_DMD_EXIT), 0, 18, 0, exit_cb, nullptr);
    pill_mark(s_p.resume, true);
    place_actions();
    if (resume) *resume = s_p.resume;
    s_p.status = caption(p, "", kPad, 62);
    clip_lines(s_p.status, kW - 2 * kPad, 1);
    s_p.address = label(p, &nv_font_14, kInk, "", kPad, 80);
    clip_lines(s_p.address, kW - 2 * kPad, 1);

    // ---- left: the screen, the menu
    int y = kTop;
    section(p, nv_tr(NV_STR_DMD_SEC_SCREEN), kPad, y);
    // the layout: what it is, what it does, how to change it. The whole card is the button (one stop for
    // the keyboard); the pill only says where it leads.
    constexpr int kCardH = 206;
    lv_obj_t *card = box(p);
    lv_obj_set_pos(card, kPad, y + 30);
    lv_obj_set_size(card, kColW, kCardH);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, hx(kCard), 0);
    lv_obj_set_style_bg_color(card, hx(0x0E3440), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, hx(kEdge), 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, layout_cb, LV_EVENT_CLICKED, nullptr);
    caption(card, nv_tr(NV_STR_DMD_LAYOUT), 16, 10);
    s_p.fname = label(card, &nv_font_28, kInk, "", 16, 28);
    clip_lines(s_p.fname, kColW - 32, 1);
    s_p.fdesc = caption(card, "", 16, 66);   // two lines: the whole text is on the composition screen
    clip_lines(s_p.fdesc, kColW - 32, 2);
    s_p.ftable = label(card, &nv_font_14, kAccent, "", 16, 104);
    clip_lines(s_p.ftable, kColW - 32, 1);
    s_p.fnote = label(card, &nv_font_14, kWarn, "", 16, 122);
    clip_lines(s_p.fnote, kColW - 32, 1);
    lv_obj_t *pick = pill(card, nv_tr(NV_STR_DMD_LAYOUT_PICK), 16, kCardH - 54, 0, nullptr, nullptr);
    lv_obj_clear_flag(pick, LV_OBJ_FLAG_CLICKABLE);
    pill_mark(pick, true);

    y += 30 + kCardH + 14;
    caption(p, nv_tr(NV_STR_DMD_ROTATION), kPad, y);
    pill_row(p, "0°\n90°\n180°\n270°", kPad, y + 20, (kColW - 24) / 4, rot_cb, s_p.rot, 4);
    pills_mark(s_p.rot, 4, io->rot);

    y += 20 + 40 + 22;
    section(p, nv_tr(NV_STR_DMD_SEC_MENU), kPad, y);
    caption(p, nv_tr(NV_STR_DMD_THEME), kPad, y + 30);
    pill_row(p, nv_tr(NV_STR_DMD_THEMES), kPad, y + 50, 160, theme_cb, s_p.theme, 2);
    pills_mark(s_p.theme, 2, io->theme);

    // ---- right: the dots, the system
    y = kTop;
    section(p, nv_tr(NV_STR_DMD_SEC_DOTS), kRightX, y);
    s_p.color_cap = caption(p, "", kRightX, y + 30);
    color_caption(io->color);
    for (int i = 0; i < io->ncolors && i < 8; i++) {   // one row of swatches: the caption names the chosen one
        lv_obj_t *b = box(p);
        lv_obj_set_pos(b, kRightX + i * 52, y + 50);
        lv_obj_set_size(b, 44, 44);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 3, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, color_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *sw = box(b);
        lv_obj_set_size(sw, 24, 24);
        lv_obj_center(sw);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        if (io->colors[i] >= 0) {
            lv_obj_set_style_bg_color(sw, hx((uint32_t)io->colors[i]), 0);
        } else {   // as the PC sends them: a ring
            lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(sw, 2, 0);
            lv_obj_set_style_border_color(sw, hx(kInk), 0);
        }
        s_p.color[i] = b;
    }
    pills_mark(s_p.color, 8, io->color);
    y += 50 + 44 + 14;
    caption(p, nv_tr(NV_STR_DMD_BRIGHT), kRightX, y);
    lv_obj_t *sl = lv_slider_create(p);
    lv_obj_set_pos(sl, kRightX + 12, y + 30);
    lv_obj_set_size(sl, kColW - 110, 10);
    lv_slider_set_range(sl, 10, 100);
    lv_slider_set_value(sl, io->bright, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, hx(kEdge), LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, hx(kAccent), LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sl, hx(kInk), LV_PART_KNOB);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 9, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sl, 16);
    lv_obj_add_event_cb(sl, bright_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(sl, bright_cb, LV_EVENT_RELEASED, nullptr);
    char b[16];
    snprintf(b, sizeof b, "%d %%", io->bright);
    s_p.bright_val = label(p, &nv_font_20, kInk, b, kRightX + kColW - 70, y + 22);

    y += 30 + 32;
    caption(p, nv_tr(NV_STR_DMD_SHAPE), kRightX, y);
    pill_row(p, nv_tr(NV_STR_DMD_SHAPES), kRightX, y + 20, 140, shape_cb, s_p.shape, 2);
    pills_mark(s_p.shape, 2, io->shape);
    toggle(p, nv_tr(NV_STR_DMD_GLOW), kRightX + 312, y + 18, kColW - 312, io->glow, set_glow);

    y += 20 + 40 + 20;
    section(p, nv_tr(NV_STR_DMD_SEC_SYSTEM), kRightX, y);
    toggle(p, nv_tr(NV_STR_DMD_CLOCK), kRightX, y + 28, kColW, io->clock, set_clock);
    toggle(p, nv_tr(NV_STR_DMD_BOOT), kRightX, y + 74, kColW, io->boot, set_boot);
    label(p, &nv_font_20, kInk, nv_tr(NV_STR_DMD_PORT), kRightX, y + 132);
    lv_obj_t *port = box(p);
    lv_obj_set_pos(port, kRightX + kColW - 150, y + 124);
    lv_obj_set_size(port, 150, 44);
    if (io->make_port) io->make_port(port);
    s_p.port_note = label(p, &nv_font_14, kWarn, "", kRightX, y + 172);
    clip_lines(s_p.port_note, kColW, 1);
    return p;
}

void dmd_panel_status(const char *line, const char *address) {
    if (s_p.status) lv_label_set_text(s_p.status, line);
    if (s_p.address) lv_label_set_text(s_p.address, address);
}

void dmd_panel_resume_label(const char *text) {
    if (!s_p.resume || !lv_obj_get_child_count(s_p.resume)) return;
    lv_obj_t *l = lv_obj_get_child(s_p.resume, 0);
    if (!strcmp(lv_label_get_text(l), text)) return;
    lv_label_set_text(l, text);
    place_actions();
}

void dmd_panel_port_note(const char *text) {
    if (s_p.port_note && strcmp(lv_label_get_text(s_p.port_note), text ? text : ""))
        lv_label_set_text(s_p.port_note, text ? text : "");
}

void dmd_panel_layout(int fit_all, int table_fit, const char *note) {
    if (!s_p.fname) return;
    char t[400];
    line_of(nv_tr(NV_STR_DMDQ_FIT_NAMES), fit_all, t, sizeof t);
    lv_label_set_text(s_p.fname, t);
    line_of(nv_tr(NV_STR_DMDC_FIT_DESCS), fit_all, t, sizeof t);
    lv_label_set_text(s_p.fdesc, t);
    if (table_fit >= 0) {
        char n[64];
        line_of(nv_tr(NV_STR_DMDQ_FIT_NAMES), table_fit, n, sizeof n);
        snprintf(t, sizeof t, nv_tr(NV_STR_DMD_LAYOUT_TABLE_FMT), n);
        lv_label_set_text(s_p.ftable, t);
    } else {
        lv_label_set_text(s_p.ftable, "");
    }
    lv_label_set_text(s_p.fnote, note ? note : "");
}

void dmd_panel_destroyed(void) { s_p = Panel{}; }

namespace {
struct Ask {
    void (*stay)(void), (*ok)(void);
};
Ask s_ask;
void ask_stay_cb(lv_event_t *) { if (s_ask.stay) s_ask.stay(); }
void ask_ok_cb(lv_event_t *) { if (s_ask.ok) s_ask.ok(); }
}  // namespace

lv_obj_t *dmd_panel_ask(lv_obj_t *parent, const char *title, const char *body, const char *stay, const char *ok,
                        void (*on_stay)(void), void (*on_ok)(void), lv_obj_t **first) {
    s_ask = Ask{on_stay, on_ok};
    lv_obj_t *veil = box(parent);   // takes every tap outside the card (nothing under it reacts)
    if (!veil) return nullptr;
    lv_obj_set_size(veil, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(veil, hx(0x000000), 0);
    lv_obj_set_style_bg_opa(veil, LV_OPA_70, 0);   // a background opacity, not an opa layer
    lv_obj_add_flag(veil, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(veil);
    constexpr int kAskW = 600;
    lv_obj_t *card = box(veil);
    lv_obj_set_width(card, kAskW);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, hx(kCard), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, hx(kEdge), 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 28, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 14, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);   // a tap on the card is not a tap on the veil
    lv_obj_t *t = lv_label_create(card);
    lv_obj_set_style_text_font(t, &nv_font_28, 0);
    lv_obj_set_style_text_color(t, hx(kAccent), 0);
    lv_label_set_text(t, title);
    lv_obj_t *m = lv_label_create(card);
    lv_obj_set_style_text_font(m, &nv_font_20, 0);
    lv_obj_set_style_text_color(m, hx(kInk), 0);
    lv_obj_set_width(m, kAskW - 56);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_label_set_text(m, body);
    lv_obj_t *row = box(card);   // the buttons at the right end: "close" first in line, "stay" last (the main one)
    lv_obj_set_size(row, kAskW - 56, 52);
    lv_obj_t *s = pill(row, stay, 0, 12, 0, ask_stay_cb, nullptr);   // created first: the first stop for Tab
    lv_obj_t *o = pill(row, ok, 0, 12, 0, ask_ok_cb, nullptr);
    pill_mark(s, true);
    lv_obj_update_layout(row);
    const int sx = kAskW - 56 - lv_obj_get_width(s);
    lv_obj_set_x(s, sx);
    lv_obj_set_x(o, sx - 12 - lv_obj_get_width(o));
    lv_obj_center(card);
    if (first) *first = s;
    return veil;
}
