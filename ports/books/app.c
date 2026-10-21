// app.c — the Libri app: the loop, the themes, the widgets every screen uses (buttons, rows, pills,
// segmented controls, sheets, the on-screen keyboard, toasts), keyboard and touch input dispatched
// to the screens. Immediate-mode drawing: a screen draws itself whole and registers its controls
// (touch targets and keyboard focus); the reader keeps its pages in surfaces (reader.c).
#include "app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nucleo_sdk.h"

#include "icons.h"

app_t A;

const icon_t g_icons[IC_COUNT] = {
    {ICON_BACK, ICON_BACK_PX}, {ICON_SEARCH, ICON_SEARCH_PX}, {ICON_TOC, ICON_TOC_PX}, {ICON_BOOKMARK, ICON_BOOKMARK_PX},
    {ICON_BOOKMARK_ON, ICON_BOOKMARK_ON_PX}, {ICON_MORE, ICON_MORE_PX}, {ICON_LIBRARY, ICON_LIBRARY_PX},
    {ICON_INFO, ICON_INFO_PX}, {ICON_CLOSE, ICON_CLOSE_PX}, {ICON_TEXTSIZE, ICON_TEXTSIZE_PX}, {ICON_CHECK, ICON_CHECK_PX},
    {ICON_BACKSPACE, ICON_BACKSPACE_PX}, {ICON_ENTER, ICON_ENTER_PX}, {ICON_SHIFT, ICON_SHIFT_PX},
    {ICON_KEYBOARD, ICON_KEYBOARD_PX}, {ICON_BOOK, ICON_BOOK_PX}, {ICON_DOWNLOAD, ICON_DOWNLOAD_PX},
    {ICON_STORE, ICON_STORE_PX}, {ICON_SORT, ICON_SORT_PX}, {ICON_TRASH, ICON_TRASH_PX}, {ICON_PREV, ICON_PREV_PX},
    {ICON_NEXT, ICON_NEXT_PX}, {ICON_ROTATE, ICON_ROTATE_PX},
};

void icon_draw(int ic, int x, int y, uint16_t c) { gfx_mask(g_icons[ic].m, g_icons[ic].px, g_icons[ic].px, x, y, c); }

// ------------------------------------------------------------------------------------- themes

uint16_t g_col[C_COUNT];

int theme_dark(int t) { return t == TH_DARK || t == TH_BLACK; }

static uint16_t grey(int level) { const int v = level * 17; return rgb565(v, v, v); }   // 16 levels: what e-ink shows

void theme_set(int t) {
    uint32_t c[C_COUNT];
    switch (t) {
    case TH_SEPIA: {
        static const uint32_t k[C_COUNT] = {0xF4ECD8, 0x3B2E1E, 0x857055, 0x8A4B12, 0x2E2214, 0xD9CBA9, 0xEADFC4, 0xEFE5CB,
                                            0x3B2E1E, 0x9A5A1C, 0xFFFFFF, 0xEADFC4, 0xE6D8B8, 0x9A5A1C, 0xDCCDA8, 0x000000,
                                            0xC0392B, 0xE8C878};
        memcpy(c, k, sizeof c);
        break;
    }
    case TH_DARK: {
        static const uint32_t k[C_COUNT] = {0x1E2024, 0xD6D8DC, 0x8D939C, 0x8AB4F8, 0xEDEFF2, 0x3A3E46, 0x2A2D33, 0x25282D,
                                            0xE6E8EB, 0x8AB4F8, 0x10141C, 0x2A2E35, 0x2C3038, 0x8AB4F8, 0x3A3F48, 0x000000,
                                            0xF28B82, 0x5A5020};
        memcpy(c, k, sizeof c);
        break;
    }
    case TH_BLACK: {   // OLED-style: pure black, softer white (no glare at night)
        static const uint32_t k[C_COUNT] = {0x000000, 0xB4B8BE, 0x6E737B, 0x7FA7E8, 0xC9CCD1, 0x2A2C30, 0x141518, 0x0C0C0E,
                                            0xC9CCD1, 0x7FA7E8, 0x000000, 0x16171A, 0x1A1B1F, 0x7FA7E8, 0x26282C, 0x000000,
                                            0xE57373, 0x403818};
        memcpy(c, k, sizeof c);
        break;
    }
    case TH_EINK: case TH_MONO: {
        const int m = t == TH_MONO;
        for (int i = 0; i < C_COUNT; i++) c[i] = 0;
        const uint16_t paper = m ? grey(15) : grey(14), ink = m ? grey(0) : grey(1);
        g_col[C_BG] = paper;
        g_col[C_TEXT] = ink;
        g_col[C_DIM] = m ? ink : grey(6);
        g_col[C_LINK] = m ? ink : grey(3);
        g_col[C_HEAD] = grey(0);
        g_col[C_RULE] = m ? ink : grey(10);
        g_col[C_CARD] = m ? paper : grey(13);
        g_col[C_BAR] = paper;
        g_col[C_BAR_TEXT] = ink;
        g_col[C_ACCENT] = ink;
        g_col[C_ON_ACCENT] = paper;
        g_col[C_CODE_BG] = m ? paper : grey(13);
        g_col[C_CELL_BG] = m ? paper : grey(12);
        g_col[C_FOCUS] = ink;
        g_col[C_PRESS] = m ? grey(0) : grey(11);
        g_col[C_SCRIM] = grey(0);
        g_col[C_MARK] = ink;
        g_col[C_SEL] = m ? grey(0) : grey(11);
        return;
    }
    default: {
        static const uint32_t k[C_COUNT] = {0xFFFFFF, 0x1E1F22, 0x666B73, 0x1A5CC8, 0x111215, 0xDADDE2, 0xF2F3F5, 0xF7F7F8,
                                            0x1E1F22, 0x1A5CC8, 0xFFFFFF, 0xF1F3F5, 0xEAEEF3, 0x1A5CC8, 0xE2E6EC, 0x000000,
                                            0xC62828, 0xFFE69A};
        memcpy(c, k, sizeof c);
        break;
    }
    }
    for (int i = 0; i < C_COUNT; i++) g_col[i] = rgb565((int)(c[i] >> 16), (int)(c[i] >> 8 & 255), (int)(c[i] & 255));
}

// The theme of the settings, and the render mode that goes with it.
void theme_apply(void) {
    const int mode = g_set.theme == TH_EINK ? RM_GREY : g_set.theme == TH_MONO ? RM_MONO : RM_COLOR;
    if (mode != g_rmode) {
        g_rmode = mode;
        font_cache_clear();   // the cached glyphs are plain coverage: kept; the pictures differ per mode (img.c)
    }
    theme_set(g_set.theme);
}

// ------------------------------------------------------------------------------------- basics

void perf(const char *fmt, ...) {   // timings in the system log (GET /api/logs)
    char b[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    nv_log(2, b);
}

void toast(const char *fmt, const char *arg) {
    snprintf(A.toast, sizeof A.toast, fmt, arg ? arg : "");
    A.toast_until = A.now + 3000;
    A.redraw |= R_ALL;
}

int pump(void) {   // one frame shown now (long work keeps the screen and the watchdog alive)
    gfx_flush();
    if (!nv_gfx_present()) { A.quit = 1; return 0; }
    return 1;
}

// Input that comes while a page turns is kept for the main loop (a key pressed and let go, a tap, would
// be lost otherwise) and tells the turn to end now: reading fast never drops a page.
static kev_t s_pend[8];
static int s_npend, s_ptouch, s_px, s_py;   // s_ptouch: 1 pressed, 2 pressed and let go

int input_during_turn(void) {
    kev_t k[8];
    const int n = kbd_poll(k, 8, nv_millis(), i18n_italian_keys());
    for (int i = 0; i < n && s_npend < 8; i++) s_pend[s_npend++] = k[i];
    int x, y;
    const int down = nv_touch(&x, &y) != 0;
    if (down && !s_ptouch) {
        gfx_touch_map(&x, &y);
        s_ptouch = 1;
        s_px = x;
        s_py = y;
    } else if (!down && s_ptouch == 1) {
        s_ptouch = 2;
    }
    return s_npend > 0 || s_ptouch;
}

int reg_add(int x, int y, int w, int h, int act, int arg) {
    if (A.nregs >= REG_MAX) return -1;
    reg_t *r = &A.regs[A.nregs];
    r->x = x; r->y = y; r->w = w; r->h = h; r->act = act; r->arg = arg;
    return A.nregs++;
}

static int reg_at(int x, int y) {
    for (int i = A.nregs - 1; i >= 0; i--) {   // the last drawn is on top
        const reg_t *r = &A.regs[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) return i;
    }
    for (int i = A.nregs - 1; i >= 0; i--) {   // a little slack around small buttons
        const reg_t *r = &A.regs[i];
        if (r->w <= BTN && x >= r->x - 10 && x < r->x + r->w + 10 && y >= r->y - 10 && y < r->y + r->h + 10) return i;
    }
    return -1;
}

int reg_state(int idx) { return idx >= 0 && idx == A.pressed ? 2 : idx >= 0 && idx == A.focus ? 1 : 0; }

void button(int x, int y, int ic, int act, int arg, int enabled, uint16_t fg) {
    const int r = enabled ? reg_add(x, y, BTN, BTN, act, arg) : -1;
    const int st = reg_state(r);
    if (st == 2) gfx_rrect(x + 2, y + 2, BTN - 4, BTN - 4, 14, g_col[C_PRESS]);
    if (st == 1) gfx_ring(x + 2, y + 2, BTN - 4, BTN - 4, 14, 2, g_col[C_FOCUS]);
    const int px = g_icons[ic].px;
    icon_draw(ic, x + (BTN - px) / 2, y + (BTN - px) / 2, enabled ? fg : g_col[C_RULE]);
}

int row_box(int x, int y, int w, int h, int act, int arg, uint16_t bg) {
    const int r = reg_add(x, y, w, h, act, arg);
    const int st = reg_state(r);
    gfx_rrect(x, y, w, h, 12, st == 2 ? g_col[C_PRESS] : bg);
    if (st == 1) gfx_ring(x, y, w, h, 12, 2, g_col[C_FOCUS]);
    return r;
}

int pill(int x, int y, const char *lbl, int act, int arg, int on) {
    const int px = 17, w = text_width(F_SANS, px, lbl, (int)strlen(lbl)) + 32, h = 40;
    const int r = reg_add(x, y, w, h, act, arg);
    const int st = reg_state(r);
    if (on) gfx_rrect(x, y, w, h, h / 2, st == 2 ? g_col[C_PRESS] : g_col[C_ACCENT]);
    else {
        gfx_rrect(x, y, w, h, h / 2, st == 2 ? g_col[C_PRESS] : g_col[C_CARD]);
        gfx_ring(x, y, w, h, h / 2, 1, g_col[C_RULE]);
    }
    if (st == 1) gfx_ring(x - 3, y - 3, w + 6, h + 6, h / 2 + 3, 2, g_col[C_FOCUS]);
    text_draw(on ? F_BOLD : F_SANS, px, x + 16, y + 26, lbl, (int)strlen(lbl), on ? g_col[C_ON_ACCENT] : g_col[C_TEXT]);
    return w;
}

// A row of choices, one selected (arg = the index).
void segmented(int x, int y, int w, const char *const *labels, int n, int sel, int act) {
    const int h = 44, cw = w / n;
    gfx_rrect(x, y, w, h, 12, g_col[C_CARD]);
    gfx_ring(x, y, w, h, 12, 1, g_col[C_RULE]);
    for (int i = 0; i < n; i++) {
        const int cx = x + i * cw;
        const int r = reg_add(cx, y, cw, h, act, i);
        const int st = reg_state(r);
        if (i == sel) gfx_rrect(cx + 3, y + 3, cw - 6, h - 6, 9, g_col[C_ACCENT]);
        else if (st == 2) gfx_rrect(cx + 3, y + 3, cw - 6, h - 6, 9, g_col[C_PRESS]);
        if (st == 1) gfx_ring(cx + 1, y + 1, cw - 2, h - 2, 11, 2, g_col[C_FOCUS]);
        const char *s = labels[i];
        const int px = 16, tw = text_width(i == sel ? F_BOLD : F_SANS, px, s, (int)strlen(s));
        text_draw_fit(i == sel ? F_BOLD : F_SANS, px, cx + (cw - (tw < cw - 12 ? tw : cw - 12)) / 2, y + 28, s, (int)strlen(s),
                      cw - 12, i == sel ? g_col[C_ON_ACCENT] : g_col[C_TEXT]);
    }
}

void bar_bg(void) {
    gfx_fill(0, 0, g_w, BAR_H, g_col[C_BAR]);
    gfx_fill(0, BAR_H - 1, g_w, 1, g_col[C_RULE]);
}

void scrim(void) { gfx_blend_rect(0, 0, g_w, g_h, g_col[C_SCRIM], theme_dark(g_set.theme) ? 150 : 110); }

void sheet_frame(int x, int y, int w, int h) {
    if (g_rmode == RM_COLOR) gfx_blend_rect(x + 2, y + 4, w, h, 0, 40);   // a soft drop shadow
    gfx_rrect(x, y, w, h, 18, g_col[C_BG]);
    gfx_ring(x, y, w, h, 18, 1, g_col[C_RULE]);
}

void draw_toast(void) {
    if (!A.toast[0] || A.now > A.toast_until) return;
    const int px = 17, maxw = g_w - 160;
    int w = text_width(F_SANS, px, A.toast, (int)strlen(A.toast)) + 40;
    if (w > maxw) w = maxw;
    const int x = (g_w - w) / 2, y = g_h - 96;
    gfx_rrect(x, y, w, 48, 24, g_col[C_TEXT]);
    text_draw_fit(F_SANS, px, x + 20, y + 30, A.toast, (int)strlen(A.toast), w - 40, g_col[C_BG]);
}

// Text wrapped at spaces in a box; at most max_lines lines, the last cut with an ellipsis. Returns the lines drawn.
int wrap_text(int f, int px, int x, int y, int w, int lh, const char *s, int max_lines, uint16_t c) {
    int n = (int)strlen(s), line = 0;
    while (n > 0 && line < max_lines) {
        while (n > 0 && *s == ' ') { s++; n--; }
        int k = text_fit(f, px, s, n, w);
        if (k < n && line + 1 < max_lines) {   // back to a space
            int b = k;
            while (b > 0 && s[b] != ' ') b--;
            if (b > 0) k = b;
        }
        if (line + 1 == max_lines && k < n) { text_draw_fit(f, px, x, y, s, n, w, c); return max_lines; }
        if (k <= 0) k = 1;
        text_draw(f, px, x, y, s, k, c);
        s += k;
        n -= k;
        y += lh;
        line++;
    }
    return line;
}

// A cover drawn for a book without a picture: a muted colour from its name, a frame, the title and
// the author set like a classic paperback.
void draw_typo_cover(const char *title, const char *author, uint32_t hue, int x, int y, int w, int h) {
    static const uint32_t kPal[8] = {0x2F4858, 0x5B3A29, 0x33503F, 0x4A3B5C, 0x6B2E2E, 0x2E4A6B, 0x5C5A2E, 0x3D3D46};
    uint32_t c = kPal[hue % 8];
    uint16_t bg = rgb565((int)(c >> 16), (int)(c >> 8 & 255), (int)(c & 255)), fg = rgb565(0xF5, 0xF0, 0xE6);
    if (g_rmode != RM_COLOR) { bg = g_col[C_TEXT]; fg = g_col[C_BG]; }
    gfx_rrect(x, y, w, h, 6, bg);
    gfx_ring(x + 7, y + 7, w - 14, h - 14, 3, 1, fg);
    const int px = w >= 120 ? 17 : 13;
    wrap_text(F_SERIF_BOLD, px, x + 14, y + 22 + px, w - 28, px * 125 / 100, title, 5, fg);
    if (author && *author) wrap_text(F_SERIF_ITALIC, px - 3, x + 14, y + h - 30, w - 28, px, author, 1, fg);
}

void draw_cover(int i, int x, int y, int w, int h) {
    if (i < 0 || i >= g_nlib) return;
    const entry_t *e = &g_lib[i];
    const uint16_t *px = A.covers ? A.covers[i] : NULL;
    if (!px) { draw_typo_cover(e->title, e->author, e->hue, x, y, w, h); return; }
    // the cached picture is COVER_W x COVER_H: drawn scaled (nearest) to w x h, toned to the render mode
    for (int j = 0; j < h; j++) {
        const int sy = y + j;
        if (sy < 0 || sy >= g_h) continue;
        const uint16_t *src = px + (size_t)(j * COVER_H / h) * COVER_W;
        uint16_t *d = g_rows[sy];
        for (int k = 0; k < w; k++) {
            const int sx = x + k;
            if (sx < 0 || sx >= g_w) continue;
            uint16_t c = src[k * COVER_W / w];
            if (g_rmode != RM_COLOR) {
                int l = lum565(c);
                if (g_rmode == RM_MONO) {   // ordered dither (4x4 Bayer): stable, no worms
                    static const uint8_t kB[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
                    l = l * 16 / 256 > kB[(sy & 3) * 4 + (sx & 3)] ? 255 : 0;
                } else {
                    l = ((l * 15 + 127) / 255) * 17;
                }
                c = rgb565(l, l, l);
            }
            d[sx] = c;
        }
    }
    gfx_dirty(y, y + h);
    gfx_ring(x, y, w, h, 2, 1, g_col[C_RULE]);
}

// ------------------------------------------------------------------- the on-screen keyboard

typedef struct { const char *lbl; int w; int code; } okey_t;   // code: char, or < 0 special
enum { OK_SHIFT = -1, OK_BS = -2, OK_LAYER = -3, OK_SPACE = -4, OK_ENTER = -5, OK_HIDE = -6 };
static const okey_t kOsk[2][4][11] = {
    {{{"q", 10, 'q'}, {"w", 10, 'w'}, {"e", 10, 'e'}, {"r", 10, 'r'}, {"t", 10, 't'}, {"y", 10, 'y'}, {"u", 10, 'u'},
      {"i", 10, 'i'}, {"o", 10, 'o'}, {"p", 10, 'p'}, {0, 0, 0}},
     {{"a", 10, 'a'}, {"s", 10, 's'}, {"d", 10, 'd'}, {"f", 10, 'f'}, {"g", 10, 'g'}, {"h", 10, 'h'}, {"j", 10, 'j'},
      {"k", 10, 'k'}, {"l", 10, 'l'}, {"'", 10, '\''}, {0, 0, 0}},
     {{"", 15, OK_SHIFT}, {"z", 10, 'z'}, {"x", 10, 'x'}, {"c", 10, 'c'}, {"v", 10, 'v'}, {"b", 10, 'b'}, {"n", 10, 'n'},
      {"m", 10, 'm'}, {"", 15, OK_BS}, {0, 0, 0}},
     {{"?123", 15, OK_LAYER}, {"", 10, OK_HIDE}, {"-", 10, '-'}, {" ", 40, OK_SPACE}, {".", 10, '.'}, {"", 15, OK_ENTER},
      {0, 0, 0}}},
    {{{"1", 10, '1'}, {"2", 10, '2'}, {"3", 10, '3'}, {"4", 10, '4'}, {"5", 10, '5'}, {"6", 10, '6'}, {"7", 10, '7'},
      {"8", 10, '8'}, {"9", 10, '9'}, {"0", 10, '0'}, {0, 0, 0}},
     {{"\xC3\xA0", 10, 0xE0}, {"\xC3\xA8", 10, 0xE8}, {"\xC3\xA9", 10, 0xE9}, {"\xC3\xAC", 10, 0xEC}, {"\xC3\xB2", 10, 0xF2},
      {"\xC3\xB9", 10, 0xF9}, {"\xC3\xA7", 10, 0xE7}, {"\xC3\xB1", 10, 0xF1}, {"\xC3\xBC", 10, 0xFC}, {"\xC3\xB6", 10, 0xF6}, {0, 0, 0}},
     {{"(", 10, '('}, {")", 10, ')'}, {"/", 10, '/'}, {":", 10, ':'}, {",", 10, ','}, {"&", 10, '&'}, {"+", 10, '+'},
      {"\xC3\xA4", 10, 0xE4}, {"\xC3\x9F", 10, 0xDF}, {"", 10, OK_BS}, {0, 0, 0}},
     {{"abc", 15, OK_LAYER}, {"", 10, OK_HIDE}, {"\"", 10, '"'}, {" ", 40, OK_SPACE}, {"?", 10, '?'}, {"", 15, OK_ENTER},
      {0, 0, 0}}},
};

void draw_osk(void) {
    const int y0 = g_h - OSK_H, dark = theme_dark(g_set.theme);
    gfx_fill(0, y0, g_w, OSK_H, g_rmode != RM_COLOR ? g_col[C_CARD] : dark ? rgb565(0x1B, 0x1E, 0x23) : rgb565(0xE4, 0xE7, 0xEC));
    gfx_fill(0, y0, g_w, 1, g_col[C_RULE]);
    const int kh = 52, gapy = 8, unit = (g_w - 24) / 100;
    for (int r = 0; r < 4; r++) {
        int tot = 0;
        for (int k = 0; kOsk[A.osk_layer][r][k].lbl; k++) tot += kOsk[A.osk_layer][r][k].w;
        int x = 12 + (100 - tot) * unit / 2;
        const int y = y0 + 10 + r * (kh + gapy);
        for (int k = 0; kOsk[A.osk_layer][r][k].lbl; k++) {
            const okey_t *o = &kOsk[A.osk_layer][r][k];
            const int w = o->w * unit - 6;
            const int special = o->code < 0 && o->code != OK_SPACE;
            const int idx = reg_add(x, y, w, kh, A_OSK, r * 16 + k);
            uint16_t bg = g_rmode != RM_COLOR ? g_col[C_BG]
                          : special ? (dark ? rgb565(0x2B, 0x2F, 0x36) : rgb565(0xCD, 0xD2, 0xDA))
                                    : (dark ? rgb565(0x3A, 0x3F, 0x48) : rgb565(0xFF, 0xFF, 0xFF));
            if (o->code == OK_ENTER || (o->code == OK_SHIFT && A.osk_shift)) bg = g_col[C_ACCENT];
            if (idx == A.pressed) bg = g_col[C_PRESS];
            gfx_rrect(x, y, w, kh, 8, bg);
            if (g_rmode != RM_COLOR) gfx_ring(x, y, w, kh, 8, 1, g_col[C_RULE]);
            const uint16_t fg = (o->code == OK_ENTER || (o->code == OK_SHIFT && A.osk_shift)) ? g_col[C_ON_ACCENT] : g_col[C_TEXT];
            int ic = o->code == OK_SHIFT ? IC_SHIFT : o->code == OK_BS ? IC_BACKSPACE : o->code == OK_ENTER ? IC_SEARCH :
                     o->code == OK_HIDE ? IC_KEYBOARD : -1;
            if (ic >= 0) {
                icon_draw(ic, x + (w - 24) / 2, y + (kh - 24) / 2, fg);
            } else {
                char lbl[8];
                snprintf(lbl, sizeof lbl, "%s", o->lbl);
                if (A.osk_shift && lbl[0] >= 'a' && lbl[0] <= 'z' && !lbl[1]) lbl[0] = (char)(lbl[0] - 32);
                const int px = o->code == OK_LAYER ? 17 : 22;
                const int tw = text_width(F_SANS, px, lbl, (int)strlen(lbl));
                text_draw(F_SANS, px, x + (w - tw) / 2, y + kh / 2 + px * 36 / 100, lbl, (int)strlen(lbl), fg);
            }
            x += o->w * unit;
        }
    }
}

static void osk_press(int code) {
    const okey_t *o = &kOsk[A.osk_layer][code / 16][code % 16];
    A.redraw |= R_ALL;
    switch (o->code) {
    case OK_SHIFT: A.osk_shift = !A.osk_shift; return;
    case OK_BS: q_backspace(); return;
    case OK_LAYER: A.osk_layer = !A.osk_layer; return;
    case OK_SPACE: q_append(' '); return;
    case OK_ENTER:
        A.osk_on = 0;
        if (A.screen == SCR_READER) reader_search_run();
        else if (A.screen == SCR_CATALOG) cat_action(A_CAT_SEARCH, 1);
        return;
    case OK_HIDE: A.osk_on = 0; return;
    default: break;
    }
    uint32_t c = (uint32_t)o->code;
    if (A.osk_shift && c >= 'a' && c <= 'z') c -= 32;
    else if (A.osk_shift && c >= 0xE0 && c <= 0xFE && c != 0xF7) c -= 32;
    q_append(c);
    A.osk_shift = 0;
}

void q_append(uint32_t c) {
    char b[4];
    const int n = utf8_put(c, b);
    const size_t l = strlen(A.q);
    if (l + (size_t)n + 1 >= sizeof A.q) return;
    memcpy(A.q + l, b, (size_t)n);
    A.q[l + (size_t)n] = 0;
    A.searched = 0;
    A.redraw |= R_ALL;
    if (A.screen == SCR_CATALOG) cat_action(A_CAT_SEARCH, 0);   // the catalog filters as you type
}

void q_backspace(void) {
    size_t l = strlen(A.q);
    while (l > 0) {
        l--;
        if (((unsigned char)A.q[l] & 0xC0) != 0x80) break;
    }
    A.q[l] = 0;
    A.searched = 0;
    A.redraw |= R_ALL;
    if (A.screen == SCR_CATALOG) cat_action(A_CAT_SEARCH, 0);
}

void draw_field(int x, int y, int w, const char *ph) {
    const int r = reg_add(x, y, w, 44, A_FIELD, 0);
    gfx_rrect(x, y, w, 44, 22, g_col[C_CARD]);
    gfx_ring(x, y, w, 44, 22, reg_state(r) ? 2 : 1, reg_state(r) ? g_col[C_FOCUS] : g_col[C_RULE]);
    icon_draw(IC_SEARCH, x + 14, y + 10, g_col[C_DIM]);
    const int tx = x + 48, tw = w - 64;
    if (!A.q[0]) {
        text_draw_fit(F_SANS, 18, tx, y + 29, ph, (int)strlen(ph), tw, g_col[C_DIM]);
        gfx_fill(tx - 2, y + 11, 2, 22, g_col[C_ACCENT]);
        return;
    }
    const char *q = A.q;
    int n = (int)strlen(q);
    while (n > 0 && text_width(F_SANS, 18, q, n) > tw - 6) {   // the end of a long query
        utf8_next(&q, A.q + strlen(A.q));
        n = (int)strlen(q);
    }
    const int w2 = text_draw(F_SANS, 18, tx, y + 29, q, n, g_col[C_TEXT]);
    gfx_fill(tx + w2 + 1, y + 11, 2, 22, g_col[C_ACCENT]);
}

// ------------------------------------------------------------------------------------ input

static void go_back(void) {
    if (A.osk_on) { A.osk_on = 0; A.redraw |= R_ALL; return; }
    if (A.sheet != SH_NONE) {
        if (A.screen == SCR_READER) reader_action(A_CLOSE, 0);
        else { A.sheet = SH_NONE; A.redraw |= R_ALL; }
        return;
    }
    if (A.screen == SCR_READER) { reader_close(); return; }
    if (A.screen == SCR_CATALOG) { A.screen = SCR_LIBRARY; lib_order(); A.redraw |= R_ALL; return; }
    A.quit = 1;
}

static void do_action(int act, int arg) {
    A.redraw |= R_ALL;
    if (act != A_DELETE) A.del_armed = 0;
    switch (act) {
    case A_BACK: go_back(); return;
    case A_OSK: osk_press(arg); return;
    case A_FIELD: A.osk_on = !kbd_present() || !A.osk_on; return;
    case A_ORIENT:   // portrait <-> landscape: the screen is made again at its new shape
        g_set.portrait = !g_set.portrait;
        settings_save();
        if (gfx_set_portrait(g_set.portrait) < 0) { g_set.portrait = !g_set.portrait; gfx_set_portrait(g_set.portrait); }
        A.lib_scroll = A.list_scroll = 0;
        A.sheet = SH_NONE;
        if (A.screen == SCR_READER) reader_reshape();
        return;
    default: break;
    }
    if (A.screen == SCR_READER && reader_action(act, arg)) return;
    if (A.screen == SCR_CATALOG && cat_action(act, arg)) return;
    switch (act) {
    case A_OPEN: case A_CONTINUE:
        if (reader_open(arg) < 0) A.screen = SCR_LIBRARY;
        return;
    case A_BOOK_INFO: A.book_sel = arg; A.sheet = SH_BOOK; return;
    case A_SORT: A.sheet = A.sheet == SH_SORT ? SH_NONE : SH_SORT; return;
    case A_SORT_SET: g_set.sort = arg; settings_save(); lib_order(); A.sheet = SH_NONE; A.lib_scroll = 0; return;
    case A_CATALOG: cat_open(); return;
    case A_ABOUT: A.sheet = SH_ABOUT; A.list_scroll = 0; return;
    case A_CLOSE: A.sheet = SH_NONE; return;
    case A_DELETE:
        if (!A.del_armed) { A.del_armed = 1; toast(tr(S_DELETE_SURE), NULL); return; }
        A.del_armed = 0;
        if (A.covers && arg >= 0 && arg < g_nlib) {
            free(A.covers[arg]);
            memmove(&A.covers[arg], &A.covers[arg + 1], sizeof(uint16_t *) * (size_t)(g_nlib - arg - 1));
            memmove(&A.cover_tried[arg], &A.cover_tried[arg + 1], (size_t)(g_nlib - arg - 1));
        }
        if (lib_remove(arg) == 0) toast(tr(S_DELETED), NULL);
        A.sheet = SH_NONE;
        lib_order();
        return;
    default: return;
    }
}

static void on_key(const kev_t *k) {
    A.idle_since = A.now;
    if (k->key == K_ESC || (k->key == K_LEFT && (k->mods & M_ALT))) { go_back(); return; }
    // typing into a query field
    if ((A.sheet == SH_SEARCH || (A.screen == SCR_CATALOG && A.sheet == SH_NONE)) &&
        (k->key == K_CHAR || k->key == K_SPACE || k->key == K_BACKSPACE) && !(k->mods & (M_CTRL | M_GUI))) {
        if (k->key == K_BACKSPACE) q_backspace();
        else q_append(k->key == K_SPACE ? ' ' : k->ch);
        return;
    }
    if (k->key == K_TAB) {   // focus moves through the controls
        if (!A.nregs) return;
        int f = A.focus;
        for (int i = 0; i < A.nregs; i++) {
            f = (k->mods & M_SHIFT) ? (f <= 0 ? A.nregs - 1 : f - 1) : (f + 1) % A.nregs;
            if (A.regs[f].act != A_OSK && A.regs[f].act != A_PAGE_TAP) break;
        }
        A.focus = f;
        A.redraw |= R_ALL;
        return;
    }
    if ((k->key == K_ENTER || k->key == K_SPACE) && A.focus >= 0 && A.focus < A.nregs) {
        const reg_t r = A.regs[A.focus];
        do_action(r.act, r.arg);
        return;
    }
    if (k->key == K_CHAR && (k->ch == 'o' || k->ch == 'O') && (k->mods & M_CTRL)) { do_action(A_ORIENT, 0); return; }
    if (A.screen == SCR_READER) { reader_key(k); return; }
    if (A.screen == SCR_CATALOG) { cat_key(k); return; }
    // the library
    const int32_t step = 300;
    switch (k->key) {
    case K_DOWN: case K_PGDN: A.lib_scroll += k->key == K_PGDN ? g_h - BAR_H - 60 : step / 3; break;
    case K_UP: case K_PGUP: A.lib_scroll -= k->key == K_PGUP ? g_h - BAR_H - 60 : step / 3; break;
    case K_HOME: A.lib_scroll = 0; break;
    case K_END: A.lib_scroll = 1 << 30; break;
    case K_CHAR:
        if (k->ch == 'c' || k->ch == 'C') { do_action(A_CATALOG, 0); return; }
        if (k->ch == 's' || k->ch == 'S') { do_action(A_SORT, 0); return; }
        return;
    default: return;
    }
    const int32_t m = lib_height() - (g_h - BAR_H);
    if (A.lib_scroll > m) A.lib_scroll = m > 0 ? m : 0;
    if (A.lib_scroll < 0) A.lib_scroll = 0;
    A.redraw |= R_ALL;
}

static int list_scrollable(void) {
    return A.sheet == SH_TOC || A.sheet == SH_SEARCH || A.sheet == SH_CAT_LANGS || A.sheet == SH_CAT_DLS || A.sheet == SH_INFO ||
           A.sheet == SH_BOOK || A.sheet == SH_CAT_BOOK || A.sheet == SH_CAT_CATS || A.sheet == SH_ABOUT;
}

static void on_touch(int down, int x, int y) {
    // the reader's page has its own gestures (turning, pinch, tap zones)
    if (A.screen == SCR_READER && (A.sheet == SH_NONE || A.sheet == SH_VIEWER)) { reader_touch(down, x, y); return; }
    if (down && !A.touch_down) {   // press
        A.touch_down = 1;
        A.tx0 = A.tx = x;
        A.ty0 = A.ty = y;
        A.t0 = A.now;
        A.moved = A.long_fired = 0;
        A.vel = 0;
        A.nvel = 0;
        A.drag_scroll0 = list_scrollable() ? A.list_scroll : A.screen == SCR_LIBRARY ? A.lib_scroll
                         : A.screen == SCR_CATALOG ? *cat_scroll() : A.list_scroll;
        A.pressed = reg_at(x, y);
        if (A.pressed >= 0 && A.regs[A.pressed].act == A_SLIDER) { A.slider_drag = 1; do_action(A_SLIDER, x); }
        A.focus = -1;
        A.redraw |= R_ALL;
        return;
    }
    if (down) {   // move
        A.tx = x;
        A.ty = y;
        if (A.slider_drag) { reader_action(A_SLIDER, x); return; }
        if (!A.moved && (abs(y - A.ty0) > 12 || abs(x - A.tx0) > 12)) {
            A.moved = 1;
            if (A.pressed >= 0 && A.regs[A.pressed].act != A_OSK) { A.pressed = -1; A.redraw |= R_ALL; }
        }
        if (A.moved && A.pressed < 0) {
            const int32_t s = A.drag_scroll0 - (y - A.ty0);
            if (list_scrollable()) A.list_scroll = s < 0 ? 0 : s;
            else if (A.screen == SCR_CATALOG) *cat_scroll() = s < 0 ? 0 : s;
            else if (A.screen != SCR_LIBRARY) A.list_scroll = s < 0 ? 0 : s;
            else {
                const int32_t m = lib_height() - (g_h - BAR_H);
                A.lib_scroll = s > m ? (m > 0 ? m : 0) : s < 0 ? 0 : s;
            }
            if (A.nvel == 6) { memmove(A.vx_t, A.vx_t + 1, 5 * sizeof(int)); memmove(A.vy_y, A.vy_y + 1, 5 * sizeof(int)); A.nvel--; }
            A.vx_t[A.nvel] = A.now;
            A.vy_y[A.nvel] = y;
            A.nvel++;
            A.redraw |= R_ALL;
        }
        if (!A.moved && !A.long_fired && A.now - A.t0 > 600 && A.pressed >= 0 && A.screen == SCR_LIBRARY &&
            A.regs[A.pressed].act == A_OPEN) {   // a long press on a book: its information
            A.long_fired = 1;
            const int i = A.regs[A.pressed].arg;
            A.pressed = -1;
            do_action(A_BOOK_INFO, i);
        }
        return;
    }
    // release
    x = A.tx;
    y = A.ty;
    A.touch_down = 0;
    const int was = A.pressed;
    A.pressed = -1;
    if (A.slider_drag) { A.slider_drag = 0; reader_action(A_SLIDER, -1); A.redraw |= R_ALL; return; }
    if (A.long_fired) { A.redraw |= R_ALL; return; }
    if (A.moved) {
        if (A.nvel >= 2) {
            const int dt = A.vx_t[A.nvel - 1] - A.vx_t[0];
            if (dt > 0 && A.now - A.vx_t[A.nvel - 1] < 80) A.vel = -(float)(A.vy_y[A.nvel - 1] - A.vy_y[0]) / (float)dt;
        }
        if (was >= 0 && A.regs[was].act == A_OSK) do_action(A_OSK, A.regs[was].arg);
        A.redraw |= R_ALL;
        return;
    }
    if (was >= 0) { do_action(A.regs[was].act, A.regs[was].arg); return; }
    if (A.sheet != SH_NONE && !(A.screen == SCR_READER && A.sheet == SH_SEARCH && A.osk_on)) go_back();   // a tap outside a panel closes it
    A.redraw |= R_ALL;
}

// ----------------------------------------------------------------------------------- drawing

static void draw_all(void) {
    A.nregs = 0;
    gfx_noclip();
    if (A.screen == SCR_READER) reader_draw();
    else if (A.screen == SCR_CATALOG) cat_draw();
    else {
        lib_draw();
        if (A.sheet == SH_BOOK || A.sheet == SH_SORT) book_sheet_draw();
        else if (A.sheet == SH_ABOUT) about_draw();
    }
    if (A.osk_on && (A.sheet == SH_SEARCH || A.screen == SCR_CATALOG)) draw_osk();
    draw_toast();
    if (A.focus >= A.nregs) A.focus = -1;
}

static int scan_progress(void *ctx, int done, int total) {
    (void)ctx;
    char msg[120];
    snprintf(msg, sizeof msg, tr(S_SCANNING), done + 1, total);
    gfx_fill(0, 0, g_w, g_h, g_col[C_BG]);
    text_draw(F_SANS, 20, (g_w - text_width(F_SANS, 20, msg, (int)strlen(msg))) / 2, g_h / 2, msg, (int)strlen(msg), g_col[C_TEXT]);
    return !pump();
}

// Opened with a file (Files > Open with): a book in ~/books opens; one elsewhere is copied there
// first (the app reads only its own folders), then opened.
static void opened_with_file(void) {
    char p[400];
    if (nv_open_path(p, sizeof p) <= 0) return;
    const char *name = strrchr(p, '/');
    name = name ? name + 1 : p;
    int i = lib_find(name);
    if (i < 0) {   // copy it in through the OS
        const int size = nv_open_size();
        char dst[600];
        snprintf(dst, sizeof dst, "%s/%s", books_dir(), name);
        FILE *f = size > 0 && size < (64 << 20) ? fopen(dst, "wb") : NULL;
        char *buf = f ? (char *)malloc(65536) : NULL;
        int ok = f && buf;
        for (int off = 0; ok && off < size;) {
            const int n = nv_open_read(off, buf, 65536);
            if (n <= 0 || fwrite(buf, 1, (size_t)n, f) != (size_t)n) ok = 0;
            off += n > 0 ? n : 0;
        }
        free(buf);
        if (f) fclose(f);
        if (!ok) { remove(dst); toast(tr(S_OPEN_FAILED), NULL); return; }
        lib_scan(NULL, NULL);
        free(A.covers);
        free(A.cover_tried);
        A.covers = (uint16_t **)calloc(LIB_MAX, sizeof(uint16_t *));
        A.cover_tried = (uint8_t *)calloc(LIB_MAX, 1);
        i = lib_find(name);
    }
    if (i >= 0) reader_open(i);
}

// Tests on the board: /appdata/shot.req (any content) asks for the screen as the app drew it,
// written to /appdata/shot.565 (gfx_shot) and the request removed; tools: ports/books/tools/shot.py.
// The system's own capture needs 2.4 MB of free PSRAM in one piece, which a reading app leaves no room for.
static void shot_poll(void) {
    static int next;
    if (A.now - next < 0) return;
    next = A.now + 1500;
    char req[300], out[300];
    snprintf(req, sizeof req, "%s/appdata/shot.req", bk_root());
    FILE *f = fopen(req, "rb");
    if (!f) return;
    fclose(f);
    snprintf(out, sizeof out, "%s/appdata/shot.565", bk_root());
    const int t0 = nv_millis();
    const int r = gfx_shot(out);
    remove(req);
    perf("shot: %s in %d ms", r ? "failed" : "written", nv_millis() - t0);
}

// The runtime grows the app's linear memory by reallocating it whole in PSRAM; later, with the
// system's allocations around it, a grow can fail. Right after the start the block the app was
// loaded into is still free: grow to nearly the whole budget once (malloc keeps what it got).
static void *volatile s_grown;

static void pregrow(void) {
    for (size_t mb = 10; mb >= 3; mb--) {
        char *p = (char *)malloc(mb << 20);
        if (!p) continue;
        ((volatile char *)p)[(mb << 20) - 1] = 0;
        s_grown = p;
        free(p);
        return;
    }
}

NV_EXPORT("run")
void run(void) {
#ifndef BK_NO_PREGROW
    pregrow();
#endif
    memset(&A, 0, sizeof A);
    A.focus = A.pressed = A.focus_link = -1;
    A.lib_idx = A.book_sel = -1;
    A.side_chap = -1;
    if (gfx_init() < 0 || font_init() < 0) return;
    i18n_init();
    settings_load();
    if (g_set.portrait && gfx_set_portrait(1) < 0) { g_set.portrait = 0; gfx_set_portrait(0); }
    theme_apply();
    A.now = nv_millis();
    const int t0 = nv_millis();
    lib_scan(scan_progress, NULL);
    A.covers = (uint16_t **)calloc(LIB_MAX, sizeof(uint16_t *));
    A.cover_tried = (uint8_t *)calloc(LIB_MAX, 1);
    if (!A.covers || !A.cover_tried) return;
    lib_order();
    perf("library: %d books in %d ms", g_nlib, nv_millis() - t0);
    opened_with_file();
    A.redraw = R_ALL;
    int last = nv_millis();
    while (!A.quit) {
        A.now = nv_millis();
        const int dt = A.now - last;
        last = A.now;
        if (nv_gfx_back()) go_back();
        kev_t keys[8];
        const int nk = kbd_poll(keys, 8, A.now, i18n_italian_keys());
        for (int i = 0; i < s_npend && !A.quit; i++) on_key(&s_pend[i]);   // came during a turn
        s_npend = 0;
        for (int i = 0; i < nk && !A.quit; i++) on_key(&keys[i]);
        int x, y;
        const int down = nv_touch(&x, &y) != 0;
        gfx_touch_map(&x, &y);
        if (s_ptouch && !A.quit) {   // a touch that came during a turn
            const int let_go = s_ptouch == 2;
            s_ptouch = 0;
            on_touch(1, s_px, s_py);
            if (let_go && !down && !A.quit) on_touch(0, s_px, s_py);
        }
        if (down || A.touch_down) on_touch(down, x, y);
        if (A.quit) break;
        if (A.vel != 0 && !A.touch_down) {   // fling of the lists
            int32_t *s = list_scrollable() ? &A.list_scroll : A.screen == SCR_LIBRARY ? &A.lib_scroll
                       : A.screen == SCR_CATALOG ? cat_scroll() : &A.list_scroll;
            const int32_t before = *s;
            *s += (int32_t)(A.vel * (float)dt);
            if (*s < 0) *s = 0;
            if (A.screen == SCR_LIBRARY && s == &A.lib_scroll) {
                const int32_t m = lib_height() - (g_h - BAR_H);
                if (*s > m) *s = m > 0 ? m : 0;
            }
            A.vel *= 0.94f;
            if ((A.vel > -0.03f && A.vel < 0.03f) || *s == before) A.vel = 0;
            A.redraw |= R_ALL;
        }
        if (A.toast[0] && A.now > A.toast_until) { A.toast[0] = 0; A.redraw |= R_ALL; }
        cat_bg_poll();
        if (A.screen == SCR_READER) reader_frame();
        else if (A.screen == SCR_CATALOG) cat_frame();
        else if (!A.touch_down && A.vel == 0) lib_idle();
        if (A.redraw) {
            draw_all();
            A.redraw = 0;
        }
        shot_poll();
        gfx_flush();
        if (!nv_gfx_present()) break;
    }
    if (A.book) reader_save();
    settings_save();
}
