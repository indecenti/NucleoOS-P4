// calculator_app — immediate-execution calculator (no operator precedence) with a
// Standard keypad and an optional Scientific mode (memory + sqrt / x^2 / 1/x) that
// the user toggles from an in-app pill. All labels are plain ASCII so they render in
// the built-in Montserrat font (the extended math glyphs × ÷ √ are not in it).
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_fonts.h"
#include "nv_theme.h"

#include "lvgl.h"
#include "nv_config.h"
#include <cstdlib>  // strtod
#include <cmath>    // sqrt
#include <cstdio>   // snprintf — newlib (full float fmt); lv_snprintf lacks %g support

namespace {

// Palette is read from the theme engine at build time (can't be constexpr: it depends on the
// live mode/accent). Chrome tokens: bg, surface (card), surface2 (num keys), surface3 (fn keys).
// Functional key colors: operator -> primary, equals -> success_solid, clear ink -> danger,
// key ink -> text_strong, ink-on-fill -> on_primary, dim expr line -> text_dim.

// ---- engine state ----------------------------------------------------------
lv_obj_t *s_expr = nullptr;    // small top line: running expression
lv_obj_t *s_out  = nullptr;    // big bottom line: current entry / result
lv_obj_t *s_keys = nullptr;    // keypad container (rebuilt on mode switch)
double s_acc   = 0;
double s_mem   = 0;
char   s_op    = 0;
char   s_buf[32] = "0";
bool   s_fresh = true;
bool   s_sci   = false;        // scientific mode
char   s_line[48] = "";

const char *op_glyph(char op) {
    switch (op) {
        case '+': return "+";
        case '-': return "-";
        case '*': return "*";
        case '/': return "/";
    }
    return "";
}

void render(void) {
    if (s_out)  lv_label_set_text(s_out, s_buf);
    if (s_expr) lv_label_set_text(s_expr, s_line);
}

void set_expr(const char *s) {
    lv_snprintf(s_line, sizeof(s_line), "%s%s", (s_mem != 0 ? "M  " : ""), s);
}

void apply(void) {
    const double x = strtod(s_buf, nullptr);
    switch (s_op) {
        case '+': s_acc += x; break;
        case '-': s_acc -= x; break;
        case '*': s_acc *= x; break;
        case '/': s_acc = (x != 0) ? s_acc / x : 0; break;
        default:  s_acc = x; break;
    }
    snprintf(s_buf, sizeof(s_buf), "%.10g", s_acc);   // trims trailing zeros
}

void reset(void) {
    s_acc = 0; s_op = 0; s_fresh = true;
    lv_strcpy(s_buf, "0");
    set_expr("");
}

void put_num(double v) {
    snprintf(s_buf, sizeof(s_buf), "%.10g", v);
    s_fresh = true;
}

void key_cb(lv_event_t *e) {
    const char *cmd = static_cast<const char *>(lv_event_get_user_data(e));
    if (!cmd) return;
    const size_t len = lv_strlen(cmd);

    if (len == 1 && cmd[0] >= '0' && cmd[0] <= '9') {          // digit
        if (s_fresh) { lv_strcpy(s_buf, cmd); s_fresh = false; }
        else if (lv_strlen(s_buf) < sizeof(s_buf) - 1) {
            if (lv_strcmp(s_buf, "0") == 0) lv_strcpy(s_buf, cmd);
            else { size_t n = lv_strlen(s_buf); s_buf[n] = cmd[0]; s_buf[n + 1] = 0; }
        }
    } else if (lv_strcmp(cmd, ".") == 0) {                     // decimal point
        if (s_fresh) { lv_strcpy(s_buf, "0."); s_fresh = false; }
        else if (!lv_strchr(s_buf, '.') && lv_strlen(s_buf) < sizeof(s_buf) - 1) {
            size_t n = lv_strlen(s_buf); s_buf[n] = '.'; s_buf[n + 1] = 0;
        }
    } else if (lv_strcmp(cmd, "C") == 0) {                     // clear all
        reset();
    } else if (lv_strcmp(cmd, "DEL") == 0) {                   // backspace
        if (!s_fresh) {
            size_t n = lv_strlen(s_buf);
            if (n > 0) s_buf[n - 1] = 0;
            if (s_buf[0] == 0 || lv_strcmp(s_buf, "-") == 0) { lv_strcpy(s_buf, "0"); s_fresh = true; }
        }
    } else if (lv_strcmp(cmd, "NEG") == 0) {                   // +/-
        if (s_buf[0] == '-') lv_strcpy(s_buf, s_buf + 1);
        else if (lv_strcmp(s_buf, "0") != 0) {
            char t[32]; lv_snprintf(t, sizeof(t), "-%s", s_buf); lv_strcpy(s_buf, t);
        }
    } else if (lv_strcmp(cmd, "%") == 0) {                     // percent
        put_num(strtod(s_buf, nullptr) / 100.0);
    } else if (lv_strcmp(cmd, "SQRT") == 0) {                  // square root
        double x = strtod(s_buf, nullptr);
        char t[40]; lv_snprintf(t, sizeof(t), "sqrt(%s)", s_buf); set_expr(t);
        put_num(x >= 0 ? sqrt(x) : 0);
    } else if (lv_strcmp(cmd, "SQR") == 0) {                   // x^2
        double x = strtod(s_buf, nullptr);
        char t[40]; lv_snprintf(t, sizeof(t), "sqr(%s)", s_buf); set_expr(t);
        put_num(x * x);
    } else if (lv_strcmp(cmd, "INV") == 0) {                   // 1/x
        double x = strtod(s_buf, nullptr);
        char t[40]; lv_snprintf(t, sizeof(t), "1/(%s)", s_buf); set_expr(t);
        put_num(x != 0 ? 1.0 / x : 0);
    } else if (lv_strcmp(cmd, "MC") == 0) {                    // memory clear
        s_mem = 0; set_expr("");
    } else if (lv_strcmp(cmd, "MR") == 0) {                    // memory recall
        put_num(s_mem);
    } else if (lv_strcmp(cmd, "M+") == 0) {                    // memory add
        s_mem += strtod(s_buf, nullptr); s_fresh = true;
    } else if (lv_strcmp(cmd, "M-") == 0) {                    // memory subtract
        s_mem -= strtod(s_buf, nullptr); s_fresh = true;
    } else if (lv_strcmp(cmd, "=") == 0) {                     // evaluate
        if (s_op) {
            double lhs = s_acc; char opc = s_op; char rhs[32]; lv_strcpy(rhs, s_buf);
            apply(); s_op = 0;
            char t[80]; snprintf(t, sizeof(t), "%.10g %s %s =", lhs, op_glyph(opc), rhs);
            set_expr(t);
        }
        s_fresh = true;
    } else {                                                  // operator + - * /
        if (s_op && !s_fresh) apply();
        else s_acc = strtod(s_buf, nullptr);
        s_op = cmd[0];
        s_fresh = true;
        char t[80]; snprintf(t, sizeof(t), "%.10g %s", s_acc, op_glyph(s_op));
        set_expr(t);
    }
    render();
}

// ---- keypad construction ---------------------------------------------------
enum Kind { K_NUM, K_FN, K_OP, K_EQ, K_CLR };

void make_key(lv_obj_t *grid, const char *label, const char *cmd, Kind kind,
              int col, int colspan, int row) {
    lv_obj_t *b = lv_button_create(grid);
    lv_obj_set_grid_cell(b, LV_GRID_ALIGN_STRETCH, col, colspan,
                            LV_GRID_ALIGN_STRETCH, row, 1);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);

    const NvTheme *th = nv_theme_get();
    lv_color_t bg = th->surface2, ink = th->text_strong;
    switch (kind) {
        case K_NUM: bg = th->surface2;      ink = th->text_strong; break;
        case K_FN:  bg = th->surface3;      ink = th->text_dim;    break;
        case K_OP:  bg = th->primary;       ink = th->on_primary;  break;
        case K_EQ:  bg = th->success_solid; ink = th->on_primary;  break;
        case K_CLR: bg = th->surface3;      ink = th->danger;      break;
    }
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, LV_STATE_PRESSED);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &nv_font_20, 0);  // pinned: keys sized for 20px glyphs
    lv_obj_set_style_text_color(l, ink, 0);
    lv_obj_center(l);

    lv_obj_add_event_cb(b, key_cb, LV_EVENT_CLICKED, const_cast<char *>(cmd));
}

// Build the keypad into s_keys for the current mode. Standard = 5 rows; Scientific
// prepends a memory row and a sqrt/x^2/1/x row (7 rows total).
void build_keypad(void) {
    lv_obj_clean(s_keys);

    static int32_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                                LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t row5[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                             LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t row7[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                             LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
                             LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

    lv_obj_t *g = lv_obj_create(s_keys);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_column(g, 8, 0);
    lv_obj_set_style_pad_row(g, 8, 0);
    lv_obj_set_grid_dsc_array(g, col_dsc, s_sci ? row7 : row5);
    lv_obj_set_layout(g, LV_LAYOUT_GRID);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);

    int r = 0;
    if (s_sci) {
        // memory row
        make_key(g, "MC", "MC", K_FN, 0, 1, r);
        make_key(g, "MR", "MR", K_FN, 1, 1, r);
        make_key(g, "M+", "M+", K_FN, 2, 1, r);
        make_key(g, "M-", "M-", K_FN, 3, 1, r);
        r++;
        // scientific row (1/x spans two cells to fill the row)
        make_key(g, "sqrt", "SQRT", K_FN, 0, 1, r);
        make_key(g, "x^2",  "SQR",  K_FN, 1, 1, r);
        make_key(g, "1/x",  "INV",  K_FN, 2, 2, r);
        r++;
    }
    // clear / edit / percent / divide
    make_key(g, "C",  "C",   K_CLR, 0, 1, r);
    make_key(g, LV_SYMBOL_BACKSPACE, "DEL", K_FN, 1, 1, r);
    make_key(g, "%",  "%",   K_FN, 2, 1, r);
    make_key(g, "/",  "/",   K_OP, 3, 1, r); r++;
    make_key(g, "7", "7", K_NUM, 0, 1, r);
    make_key(g, "8", "8", K_NUM, 1, 1, r);
    make_key(g, "9", "9", K_NUM, 2, 1, r);
    make_key(g, "*", "*", K_OP, 3, 1, r); r++;
    make_key(g, "4", "4", K_NUM, 0, 1, r);
    make_key(g, "5", "5", K_NUM, 1, 1, r);
    make_key(g, "6", "6", K_NUM, 2, 1, r);
    make_key(g, "-", "-", K_OP, 3, 1, r); r++;
    make_key(g, "1", "1", K_NUM, 0, 1, r);
    make_key(g, "2", "2", K_NUM, 1, 1, r);
    make_key(g, "3", "3", K_NUM, 2, 1, r);
    make_key(g, "+", "+", K_OP, 3, 1, r); r++;
    make_key(g, "+/-", "NEG", K_FN, 0, 1, r);
    make_key(g, "0",   "0",   K_NUM, 1, 1, r);
    make_key(g, ".",   ".",   K_NUM, 2, 1, r);
    make_key(g, "=",   "=",   K_EQ,  3, 1, r);
}

void mode_toggle_cb(lv_event_t *e) {
    s_sci = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    nv_config_set_bool("calc_sci", s_sci);
    build_keypad();
}

void page_deleted(lv_event_t *) { s_expr = s_out = s_keys = nullptr; }

void calc_build(lv_obj_t *content) {
    reset();
    s_mem = 0;   // clear stored memory on a fresh app open (C/reset intentionally keeps M)
    s_sci = nv_config_get_bool("calc_sci", false);
    const NvTheme *th = nv_theme_get();
    lv_obj_set_style_bg_color(content, th->bg, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);

    lv_obj_t *col = lv_obj_create(content);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(col, 14, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(col, page_deleted, LV_EVENT_DELETE, nullptr);

    // ---- top bar: Scientific-mode pill (right-aligned) ----
    lv_obj_t *bar = lv_obj_create(col);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pill = lv_button_create(bar);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_height(pill, 38);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(pill, 0, 0);
    lv_obj_set_style_bg_color(pill, th->surface3, 0);
    lv_obj_set_style_bg_color(pill, th->primary, LV_STATE_CHECKED);
    if (s_sci) lv_obj_add_state(pill, LV_STATE_CHECKED);
    lv_obj_add_event_cb(pill, mode_toggle_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t *pl = lv_label_create(pill);
    lv_label_set_text(pl, nv_tr(NV_STR_SCIENTIFIC));
    lv_obj_set_style_text_color(pl, th->text_strong, 0);
    lv_obj_center(pl);

    // ---- display card ----
    lv_obj_t *disp = lv_obj_create(col);
    lv_obj_remove_style_all(disp);
    lv_obj_set_size(disp, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(disp, th->surface, 0);
    lv_obj_set_style_bg_opa(disp, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(disp, 14, 0);
    lv_obj_set_style_pad_all(disp, 16, 0);
    lv_obj_set_style_pad_row(disp, 4, 0);
    lv_obj_set_flex_flow(disp, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(disp, LV_OBJ_FLAG_SCROLLABLE);

    s_expr = lv_label_create(disp);
    lv_obj_set_width(s_expr, lv_pct(100));
    lv_obj_set_style_text_color(s_expr, th->text_dim, 0);
    lv_obj_set_style_text_align(s_expr, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_expr, LV_LABEL_LONG_DOT);

    s_out = lv_label_create(disp);
    lv_obj_set_width(s_out, lv_pct(100));
    lv_obj_set_style_text_font(s_out, &nv_font_28, 0);  // pinned: display face, fixed card
    lv_obj_set_style_text_color(s_out, th->text_strong, 0);
    lv_obj_set_style_text_align(s_out, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_out, LV_LABEL_LONG_SCROLL);

    // ---- keypad ----
    s_keys = lv_obj_create(col);
    lv_obj_remove_style_all(s_keys);
    lv_obj_set_width(s_keys, lv_pct(100));
    lv_obj_set_flex_grow(s_keys, 1);
    lv_obj_clear_flag(s_keys, LV_OBJ_FLAG_SCROLLABLE);
    build_keypad();

    render();
}

const NvApp kCalcApp = {"calc", "Calculator", &nv_icon_calc, 512u << 10, calc_build,
                        NV_STR_APP_CALC, nullptr};

}  // namespace

void calculator_app_register(void) { nv_app_register(&kCalcApp); }
