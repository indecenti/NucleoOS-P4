// terminal_app — the NucleoOS terminal: an xterm-compatible terminal screen (libvterm) for the shell
// in term_sh.cpp (POSIX-flavoured command language, GNU-style core utilities, NucleoOS built-ins)
// and for WASI terminal programs — any installed app whose manifest says "console": true (Lua,
// JavaScript, SQLite, ... from the Store) runs here with its command line as argv, what the user
// types as stdin and its output drawn on the screen.
//
// This file is the tty:
//  - libvterm keeps the cell grid (colours, bold, underline, reverse, cursor addressing, scroll
//    regions, the alternate screen, DSR/CPR replies); a custom LVGL object draws it in DejaVu Sans
//    Mono, row by row, only the rows libvterm reports damaged. Lines scrolled off the top go to a
//    1000-line scrollback in PSRAM, browsed by dragging the screen.
//  - Cooked mode (the default) is the line discipline: the line being typed lives in a hidden IME
//    textarea and is echoed into the grid at the cursor; Enter hands it to the shell / program.
//    Tab completion, history, and Ctrl shortcuts (C D L U K A E W P N) work on it.
//  - Raw mode (a program on the alternate screen, or a shell built-in such as edit / less / top
//    asking for it) sends every key straight through as the xterm byte sequence.
// The shell runs on its own task and writes into a ring buffer drained here; programs are started
// here on its behalf (term_prog_run). Output text is English (a Unix terminal), so it adds no i18n
// keys; only the launcher label is translated.
#include "apps_internal.h"
#include "term_sh.h"

#include "nv_app.h"
#include "nv_ui.h"        // nv_ui_close_app()
#include "nv_ui_kit.h"    // nv_kit_* + (transitively) nv_ime_*
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_theme.h"
#include "nv_fonts.h"
#include "nv_ota.h"       // nv_ota_running_version()
#include "nv_wasm.h"      // terminal programs (console WASI apps)
#include "nv_log.h"
#include "nv_event_bus.h" // NV_EV_IME_VISIBILITY (keyboard up -> terminal shrinks)

#include "vterm.h"

#include "lvgl.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace {

// ---------------------------------------------------------------- look

// A dark Linux terminal (GNOME Terminal / Tango palette), edge to edge, DejaVu Sans Mono.
constexpr uint32_t kBg      = 0x121417;   // terminal background
constexpr uint32_t kFg      = 0xD3D7CF;   // default text
constexpr uint32_t kFgBold  = 0xEEEEEC;   // bold with the default colour
constexpr uint32_t kSel     = 0x729FCF;   // accents: the armed Ctrl key
constexpr uint32_t kKeyBg   = 0x202327;   // extra-keys row
constexpr uint32_t kKey     = 0x2C3035;
constexpr uint32_t kKeyDown = 0x3A3F46;
// ANSI 0..15 (Tango; black and blue lifted a little so they read on the dark background).
constexpr uint32_t kAnsi[16] = {
    0x555753, 0xCC0000, 0x4E9A06, 0xC4A000, 0x3B72C4, 0x75507B, 0x06989A, 0xD3D7CF,
    0x7F8386, 0xEF2929, 0x8AE234, 0xFCE94F, 0x729FCF, 0xAD7FA8, 0x34E2E2, 0xEEEEEC,
};

constexpr int kCellH   = 21;    // row pitch: font line height 22 - 1, so box drawing joins up
constexpr int kMaxCols = 160;
constexpr int kMaxRows = 64;
constexpr int kPadX = 8, kPadY = 4;

// DejaVu Sans Mono 17: ASCII, Latin-1, box drawing and block elements; anything else falls back to
// the UI font. A RAM copy, since the fallback is a field.
lv_font_t s_mono;
bool      s_mono_ok = false;
int       s_cell_w  = 10;

// ---------------------------------------------------------------- state

lv_obj_t *s_root   = nullptr;
lv_obj_t *s_view   = nullptr;   // the cell grid
lv_obj_t *s_input  = nullptr;   // hidden IME textarea: the line being typed (cooked) / key source (raw)
lv_obj_t *s_ctrl_key = nullptr; // the Ctrl key: armed = the next letter typed is Ctrl+letter
bool      s_ctrl_armed = false;
int32_t   s_kb_h   = 0;         // on-screen keyboard height while it is up

VTerm       *s_vt = nullptr;
VTermScreen *s_vs = nullptr;
VTermState  *s_vst = nullptr;
int  s_rows = 24, s_cols = 80;
VTermPos s_cur = {0, 0};
bool s_cur_vis = true;          // DECTCEM
bool s_blink_on = true;
bool s_altscreen = false;
lv_timer_t *s_blink = nullptr;

// Scrollback: lines pushed off the top of the primary screen, newest last, as compact cells.
struct SbCell {
    uint32_t ch;       // first code point (0 = blank)
    uint32_t fg;       // 0xRRGGBB, bit 24 = default fg
    uint32_t bg;       // 0xRRGGBB, bit 24 = default bg
    uint8_t  attrs;    // bit0 bold, bit1 underline, bit2 reverse, bit3 strike
    uint8_t  width;
};
constexpr int kSbLines = 1000;
SbCell  *s_sb = nullptr;        // kSbLines x kMaxCols (PSRAM)
uint8_t *s_sb_cols = nullptr;   // columns stored per line
int s_sb_head = 0, s_sb_count = 0;
int s_view_off = 0;             // lines scrolled back into the scrollback (0 = live screen)
int32_t s_drag_acc = 0;
bool s_dragged = false;

// Line discipline (cooked mode): where the typed line starts on the grid.
bool s_org_valid = false;
int  s_org_row = 0, s_org_col = 0;
bool s_quiet = false;           // programmatic textarea edits: no echo
uint32_t s_last_cur = 0;        // textarea cursor last echoed
bool s_raw = false;             // keys go straight through
const char kRawSentinel[] = " "; // raw mode keeps one char in the textarea to catch Backspace

// The terminal program run for the shell (nv_wasm runs one app at a time).
struct Prog {
    bool        active = false;
    bool        requested = false;  // started for the shell: it waits for the exit status
    bool        aborted = false;    // ^C
    bool        piped = false;      // stdin comes from a pipe / file, not the keyboard
    char        id[32] = "";
    char        args[256] = "";
    const char *in = nullptr;       // piped stdin still to feed
    size_t      in_left = 0;
    const ShSink *out = nullptr;    // nullptr = the screen
    // Start retry (see prog_start): waiting for the previous app's run to wind down.
    lv_timer_t *retry  = nullptr;
    uint32_t    wait_t0 = 0;
};
Prog s_prog;
constexpr uint32_t kProgPollMs = 50;
// Longest wait for an aborted run to wind down before a start gives up with "another app is
// running": a guest inside nv.http_get only sees the abort when that call returns (10 s timeout).
constexpr uint32_t kProgStartWaitMs = 12000;

// A command to run as soon as the screen is built (a console app's Home tile / Store "Open").
char s_autorun[48] = "";

// Command history (newest last), walked with the up / down keys.
constexpr int kHistMax = 64;
char (*s_hist)[256] = nullptr;   // PSRAM, allocated once
int s_hist_n = 0, s_hist_pos = 0;

// Shell side of the tty.
constexpr size_t kRing = 32 * 1024;
char             *s_ring = nullptr;          // PSRAM, allocated once
size_t            s_ring_head = 0, s_ring_tail = 0;   // monotonic byte counters
SemaphoreHandle_t s_ring_mtx = nullptr;
StreamBufferHandle_t s_keys = nullptr;       // raw key bytes for shell built-ins
std::atomic<bool> s_tty_open{false};
std::atomic<bool> s_sh_raw{false};           // a shell built-in asked for raw keys
std::atomic<int>  s_cols_pub{80}, s_rows_pub{24};
lv_timer_t       *s_tick = nullptr;
uint32_t          s_jobs_seen = 0;           // sh_jobs_done() already answered with a prompt
constexpr uint32_t kTickMs = 30;
constexpr size_t   kDrainBudget = 16384;     // bytes per tick: the screen keeps up, the UI stays fluid

struct ProgReq {
    std::atomic<bool> pending{false};
    char           id[32];
    char           args[256];
    const char    *in;
    size_t         in_len;
    const ShSink  *out;
    int            status;
    SemaphoreHandle_t done;
};
ProgReq s_req;

struct UiReq {
    std::atomic<bool> pending{false};
    void (*fn)(void *);
    void *arg;
    SemaphoreHandle_t done;
};
UiReq s_ui;
std::atomic<bool> s_exit_req{false};

// ---------------------------------------------------------------- memory for libvterm

void *vt_malloc(size_t n, void *) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void  vt_free(void *p, void *)    { heap_caps_free(p); }
VTermAllocatorFunctions kVtAlloc = {vt_malloc, vt_free};

// ---------------------------------------------------------------- grid -> screen

uint32_t color_rgb(VTermColor c, bool fg) {
    if (fg && VTERM_COLOR_IS_DEFAULT_FG(&c)) return (1u << 24) | kFg;
    if (!fg && VTERM_COLOR_IS_DEFAULT_BG(&c)) return (1u << 24) | kBg;
    vterm_screen_convert_color_to_rgb(s_vs, &c);
    return ((uint32_t)c.rgb.red << 16) | ((uint32_t)c.rgb.green << 8) | c.rgb.blue;
}

SbCell to_sb(const VTermScreenCell &c) {
    SbCell s;
    s.ch = c.chars[0];
    s.fg = color_rgb(c.fg, true);
    s.bg = color_rgb(c.bg, false);
    s.attrs = (uint8_t)((c.attrs.bold ? 1 : 0) | (c.attrs.underline ? 2 : 0) |
                        (c.attrs.reverse ? 4 : 0) | (c.attrs.strike ? 8 : 0));
    s.width = (uint8_t)(c.width > 0 ? c.width : 1);
    return s;
}

void invalidate_rows(int r0, int r1) {
    if (!s_view) return;
    if (s_view_off) { lv_obj_invalidate(s_view); return; }
    lv_area_t a;
    lv_obj_get_coords(s_view, &a);
    const int32_t top = a.y1 + kPadY;
    lv_area_t d = {a.x1, top + r0 * kCellH, a.x2, top + r1 * kCellH + kCellH - 1};
    if (r0 == 0) d.y1 = a.y1;
    lv_obj_invalidate_area(s_view, &d);
}

int cb_damage(VTermRect r, void *) { invalidate_rows(r.start_row, r.end_row - 1); return 1; }

int cb_movecursor(VTermPos pos, VTermPos old, int visible, void *) {
    s_cur = pos;
    s_cur_vis = visible;
    s_blink_on = true;
    invalidate_rows(old.row, old.row);
    invalidate_rows(pos.row, pos.row);
    return 1;
}

int cb_settermprop(VTermProp prop, VTermValue *v, void *) {
    switch (prop) {
        case VTERM_PROP_CURSORVISIBLE: s_cur_vis = v->boolean; break;
        case VTERM_PROP_ALTSCREEN:     s_altscreen = v->boolean; s_view_off = 0; break;
        default: break;
    }
    if (s_view) lv_obj_invalidate(s_view);
    return 1;
}

int cb_bell(void *) { return 1; }

int cb_sb_pushline(int cols, const VTermScreenCell *cells, void *) {
    if (!s_sb) return 0;
    const int slot = (s_sb_head + s_sb_count) % kSbLines;
    const int n = cols < kMaxCols ? cols : kMaxCols;
    SbCell *dst = s_sb + (size_t)slot * kMaxCols;
    for (int i = 0; i < n; i++) dst[i] = to_sb(cells[i]);
    s_sb_cols[slot] = (uint8_t)n;
    if (s_sb_count < kSbLines) s_sb_count++;
    else s_sb_head = (s_sb_head + 1) % kSbLines;
    if (s_view_off) s_view_off = s_view_off < s_sb_count ? s_view_off + 1 : s_sb_count;   // hold the view still
    return 1;
}

int cb_sb_popline(int cols, VTermScreenCell *cells, void *) {
    if (!s_sb || !s_sb_count) return 0;
    const int slot = (s_sb_head + s_sb_count - 1) % kSbLines;
    const SbCell *src = s_sb + (size_t)slot * kMaxCols;
    for (int i = 0; i < cols; i++) {
        VTermScreenCell &c = cells[i];
        memset(&c, 0, sizeof c);
        c.width = 1;
        if (i < s_sb_cols[slot]) {
            const SbCell &s = src[i];
            c.chars[0] = s.ch;
            c.width = (char)s.width;
            c.attrs.bold = s.attrs & 1;
            c.attrs.underline = (s.attrs & 2) ? 1 : 0;
            c.attrs.reverse = (s.attrs & 4) ? 1 : 0;
            c.attrs.strike = (s.attrs & 8) ? 1 : 0;
            if (s.fg & (1u << 24)) { c.fg.type = VTERM_COLOR_DEFAULT_FG; }
            else vterm_color_rgb(&c.fg, (uint8_t)(s.fg >> 16), (uint8_t)(s.fg >> 8), (uint8_t)s.fg);
            if (s.bg & (1u << 24)) { c.bg.type = VTERM_COLOR_DEFAULT_BG; }
            else vterm_color_rgb(&c.bg, (uint8_t)(s.bg >> 16), (uint8_t)(s.bg >> 8), (uint8_t)s.bg);
        } else {
            c.fg.type = VTERM_COLOR_DEFAULT_FG;
            c.bg.type = VTERM_COLOR_DEFAULT_BG;
        }
    }
    s_sb_count--;
    return 1;
}

int cb_sb_clear(void *) {
    s_sb_count = 0;
    s_view_off = 0;
    if (s_view) lv_obj_invalidate(s_view);
    return 1;
}

const VTermScreenCallbacks kScreenCb = {
    cb_damage, nullptr, cb_movecursor, cb_settermprop, cb_bell, nullptr,
    cb_sb_pushline, cb_sb_popline, cb_sb_clear,
};

void prog_stdin(const char *s, size_t n);

// Bytes libvterm sends back to the "host": keys in raw mode and replies to queries (DSR, DA).
void cb_output(const char *s, size_t n, void *) {
    if (s_prog.active && !s_prog.piped) prog_stdin(s, n);
    else if (s_sh_raw.load() && s_keys) xStreamBufferSend(s_keys, s, n, 0);
}

void vt_write(const char *s, size_t n) {
    if (!s_vt || !n) return;
    vterm_input_write(s_vt, s, n);
}
void vt_puts(const char *s) { vt_write(s, strlen(s)); }

// ---------------------------------------------------------------- drawing

struct Cell { uint32_t ch; uint32_t fg; uint32_t bg; uint8_t attrs; uint8_t width; };

// Cells of view row r (the live screen, or a scrollback line when scrolled back).
int row_cells(int r, Cell *out) {
    const int v = r - s_view_off;
    if (v >= 0) {
        for (int c = 0; c < s_cols; c++) {
            VTermScreenCell sc;
            VTermPos p = {v, c};
            if (!vterm_screen_get_cell(s_vs, p, &sc)) { out[c] = {0, (1u << 24) | kFg, (1u << 24) | kBg, 0, 1}; continue; }
            const SbCell s = to_sb(sc);
            out[c] = {s.ch, s.fg, s.bg, s.attrs, (uint8_t)((signed char)sc.width < 0 ? 0 : sc.width)};
        }
        return s_cols;
    }
    const int back = -v;   // 1 = newest scrollback line
    if (back > s_sb_count) { for (int c = 0; c < s_cols; c++) out[c] = {0, (1u << 24) | kFg, (1u << 24) | kBg, 0, 1}; return s_cols; }
    const int slot = (s_sb_head + s_sb_count - back) % kSbLines;
    const SbCell *src = s_sb + (size_t)slot * kMaxCols;
    for (int c = 0; c < s_cols; c++) {
        if (c < s_sb_cols[slot]) out[c] = {src[c].ch, src[c].fg, src[c].bg, src[c].attrs, src[c].width};
        else out[c] = {0, (1u << 24) | kFg, (1u << 24) | kBg, 0, 1};
    }
    return s_cols;
}

int utf8_put(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

void fill(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t rgb) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(rgb & 0xFFFFFF);
    d.bg_opa = LV_OPA_COVER;
    d.radius = 0;
    lv_area_t a = {x1, y1, x2, y2};
    lv_draw_rect(layer, &d, &a);
}

void text(lv_layer_t *layer, int32_t x, int32_t y, const char *s, uint32_t rgb) {
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = &s_mono;
    d.color = lv_color_hex(rgb & 0xFFFFFF);
    d.opa = LV_OPA_COVER;
    d.text = s;
    d.text_local = 1;   // copied: the buffer is on the stack
    lv_area_t a = {x, y, x + 4000, y + kCellH + 4};
    lv_draw_label(layer, &d, &a);
}

void draw_row(lv_layer_t *layer, int32_t x0, int32_t y, int r) {
    static Cell cells[kMaxCols];
    const int n = row_cells(r, cells);
    const bool cursor_row = !s_view_off && r == s_cur.row && s_cur_vis;
    const bool focused = s_input && lv_obj_has_state(s_input, LV_STATE_FOCUSED);
    char buf[kMaxCols * 4 + 4];
    int c = 0;
    while (c < n) {
        Cell &k = cells[c];
        uint32_t fg = k.fg, bg = k.bg;
        if ((k.attrs & 1) && (fg & (1u << 24))) fg = kFgBold;
        if (k.attrs & 4) { const uint32_t t = fg; fg = bg; bg = t; }
        // A run: same colours and attributes, plain ASCII (other glyphs are drawn one per cell so
        // a proportional fallback glyph can never shift the columns after it).
        int e = c + 1;
        const bool ascii = k.ch < 0x80;
        if (ascii)
            while (e < n && cells[e].ch < 0x80 && cells[e].fg == k.fg && cells[e].bg == k.bg &&
                   cells[e].attrs == k.attrs) e++;
        const int32_t x1 = x0 + c * s_cell_w, x2 = x0 + e * s_cell_w - 1;
        if (!(bg & (1u << 24)) || (k.attrs & 4)) fill(layer, x1, y, x2, y + kCellH - 1, bg);
        int len = 0;
        bool any = false;
        for (int i = c; i < e; i++) {
            const uint32_t ch = cells[i].ch;
            if (ch > ' ') any = true;
            len += utf8_put(ch ? ch : ' ', buf + len);
        }
        buf[len] = '\0';
        if (any) text(layer, x1, y, buf, fg);
        if (k.attrs & 2) fill(layer, x1, y + kCellH - 3, x2, y + kCellH - 2, fg);
        if (k.attrs & 8) fill(layer, x1, y + kCellH / 2, x2, y + kCellH / 2, fg);
        c = e + (!ascii && k.width > 1 ? k.width - 1 : 0);
    }
    // The cursor: a block when the terminal has the keyboard, an outline when it doesn't.
    if (cursor_row && s_cur.col < n) {
        const int32_t x1 = x0 + s_cur.col * s_cell_w;
        const Cell &k = cells[s_cur.col];
        if (focused && s_blink_on) {
            fill(layer, x1, y, x1 + s_cell_w - 1, y + kCellH - 1, kFg);
            if (k.ch > ' ') {
                const int l = utf8_put(k.ch, buf);
                buf[l] = '\0';
                text(layer, x1, y, buf, kBg);
            }
        } else if (!focused) {
            fill(layer, x1, y, x1 + s_cell_w - 1, y, kFg);
            fill(layer, x1, y + kCellH - 1, x1 + s_cell_w - 1, y + kCellH - 1, kFg);
            fill(layer, x1, y, x1, y + kCellH - 1, kFg);
            fill(layer, x1 + s_cell_w - 1, y, x1 + s_cell_w - 1, y + kCellH - 1, kFg);
        }
    }
}

void view_draw(lv_event_t *e) {
    if (!s_vt) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(s_view, &a);
    const lv_area_t &clip = layer->_clip_area;
    const int32_t x0 = a.x1 + kPadX, top = a.y1 + kPadY;
    for (int r = 0; r < s_rows; r++) {
        const int32_t y = top + r * kCellH;
        if (y + kCellH <= clip.y1 || y > clip.y2) continue;
        draw_row(layer, x0, y, r);
    }
    // Scrolled back: a slim position bar on the right edge.
    if (s_view_off && s_sb_count) {
        const int32_t h = a.y2 - a.y1;
        const int total = s_sb_count + s_rows;
        const int32_t bh = h * s_rows / total > 24 ? h * s_rows / total : 24;
        const int32_t by = a.y1 + (h - bh) * (s_sb_count - s_view_off) / (s_sb_count ? s_sb_count : 1);
        fill(layer, a.x2 - 3, by, a.x2, by + bh, 0x5A5F66);
    }
}

// ---------------------------------------------------------------- geometry

void grid_resize(void) {
    if (!s_view || !s_vt) return;
    lv_obj_update_layout(s_view);
    const int32_t w = lv_obj_get_width(s_view) - 2 * kPadX;
    const int32_t h = lv_obj_get_height(s_view) - 2 * kPadY;
    int cols = w / s_cell_w, rows = h / kCellH;
    cols = cols < 20 ? 20 : cols > kMaxCols ? kMaxCols : cols;
    rows = rows < 4 ? 4 : rows > kMaxRows ? kMaxRows : rows;
    if (rows == s_rows && cols == s_cols) return;
    const int cur_before = s_cur.row;
    vterm_set_size(s_vt, rows, cols);
    s_rows = rows;
    s_cols = cols;
    s_cols_pub = cols;
    s_rows_pub = rows;
    VTermPos p;
    vterm_state_get_cursorpos(s_vst, &p);
    s_cur = p;
    if (s_org_valid) s_org_row += p.row - cur_before;   // the grid shifted with the cursor
    s_view_off = 0;
    lv_obj_invalidate(s_view);
}

// ---------------------------------------------------------------- line discipline (cooked mode)

uint32_t text_chars(const char *t) {
    uint32_t n = 0;
    for (; *t; t++) n += ((unsigned char)*t & 0xC0) != 0x80;
    return n;
}

// Echo the typed line into the grid at its origin, then put the grid cursor where the textarea
// cursor is. The line may wrap; if it runs past the bottom the grid scrolls and the origin moves.
void edit_render(void) {
    if (!s_input || s_raw || !s_vt) return;
    const char *t = lv_textarea_get_text(s_input);
    const int n = (int)text_chars(t);
    const int idx = (int)lv_textarea_get_cursor_pos(s_input);
    if (!s_org_valid) {
        VTermPos p;
        vterm_state_get_cursorpos(s_vst, &p);
        s_org_row = p.row;
        s_org_col = p.col;
        s_org_valid = true;
    }
    char esc[48];
    snprintf(esc, sizeof esc, "\x1b[0m\x1b[%d;%dH\x1b[J", s_org_row + 1, s_org_col + 1);
    vt_puts(esc);
    vt_puts(t);
    if (n) {
        const int end_row = s_org_row + (s_org_col + n - 1) / s_cols;
        if (end_row > s_rows - 1) s_org_row -= end_row - (s_rows - 1);
    }
    if (s_org_row < 0) s_org_row = 0;
    const int p = s_org_col + idx;
    int r = s_org_row + p / s_cols, c = p % s_cols;
    if (r > s_rows - 1) { r = s_rows - 1; c = s_cols - 1; }
    snprintf(esc, sizeof esc, "\x1b[%d;%dH", r + 1, c + 1);
    vt_puts(esc);
    s_last_cur = (uint32_t)idx;
    s_view_off = 0;
}

// Set the typed line without echoing (the caller redraws or moves on).
void input_set_quiet(const char *t) {
    if (!s_input) return;
    s_quiet = true;
    lv_textarea_set_text(s_input, t);
    s_quiet = false;
}

// Enter: leave the cursor after the line, on a new row. The line is handed on by the caller.
void edit_commit(void) {
    if (!s_input) return;
    lv_textarea_set_cursor_pos(s_input, LV_TEXTAREA_CURSOR_LAST);
    edit_render();
    vt_puts("\r\n");
    s_org_valid = false;
}

// The shell prompt, bash style: user@host:dir$
void shell_prompt(void) {
    VTermPos p;
    vterm_state_get_cursorpos(s_vst, &p);
    vt_puts("\x1b[0m");
    if (p.col) vt_puts("\r\n");   // output that didn't end its line: the prompt starts a fresh one
    char dir[160], b[256];
    sh_prompt_dir(dir, sizeof dir);
    snprintf(b, sizeof b, "\x1b[1;32m%s@%s\x1b[0m:\x1b[1;34m%s\x1b[0m$ ", "nucleo", "anima", dir);
    vt_puts(b);
    s_org_valid = false;
    if (s_input && lv_textarea_get_text(s_input)[0]) edit_render();   // typed ahead
}

// ---------------------------------------------------------------- raw mode

void raw_update(void) {
    const bool raw = s_sh_raw.load() || (s_prog.active && !s_prog.piped && s_altscreen);
    if (raw == s_raw) return;
    s_raw = raw;
    input_set_quiet(raw ? kRawSentinel : "");
    if (!raw) s_org_valid = false;
}

VTermModifier raw_mod(void) {
    if (!s_ctrl_armed) return VTERM_MOD_NONE;
    s_ctrl_armed = false;
    if (s_ctrl_key) {
        lv_obj_set_style_bg_color(s_ctrl_key, lv_color_hex(kKey), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_ctrl_key, 0), lv_color_hex(kFg), 0);
    }
    return VTERM_MOD_CTRL;
}

void raw_key(VTermKey k) { if (s_vt) vterm_keyboard_key(s_vt, k, raw_mod()); }
void raw_char(uint32_t cp, VTermModifier mod) { if (s_vt) vterm_keyboard_unichar(s_vt, cp, mod); }

void raw_text(const char *t) {
    const VTermModifier mod = raw_mod();
    while (*t) {
        uint32_t cp = (unsigned char)*t++;
        int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        if (more) cp &= 0x3F >> more;
        while (more-- && *t) cp = (cp << 6) | ((unsigned char)*t++ & 0x3F);
        raw_char(cp, mod);
    }
}

// ---------------------------------------------------------------- shell output ring

bool ring_empty(void) {
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    const bool e = s_ring_head == s_ring_tail;
    xSemaphoreGive(s_ring_mtx);
    return e;
}

bool ring_drain(size_t budget) {
    static char chunk[1024];
    bool any = false;
    while (budget) {
        xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
        size_t k = s_ring_head - s_ring_tail;
        if (k > sizeof chunk) k = sizeof chunk;
        if (k > budget) k = budget;
        for (size_t i = 0; i < k; i++) chunk[i] = s_ring[(s_ring_tail + i) % kRing];
        s_ring_tail += k;
        xSemaphoreGive(s_ring_mtx);
        if (!k) break;
        vt_write(chunk, k);
        budget -= k;
        any = true;
    }
    return any;
}

void ring_reset(void) {
    xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
    s_ring_head = s_ring_tail = 0;
    xSemaphoreGive(s_ring_mtx);
}

// ---------------------------------------------------------------- terminal programs

void prog_stdin(const char *s, size_t n) {
    if (nv_wasm_exec_write_stdin(s, n) < n) vt_puts("\r\n[input dropped: program busy]\r\n");
}

// Move program output to its sink (the screen, a pipe buffer or a file).
bool prog_drain(void) {
    char chunk[512];
    size_t k;
    bool any = false;
    while ((k = nv_wasm_exec_read(chunk, sizeof chunk)) > 0) {
        if (s_prog.out) sh_sink_write(*s_prog.out, chunk, k);
        else { vt_write(chunk, k); any = true; }
    }
    return any;
}

// The program is over: answer the shell with its exit status.
void prog_finish(int status) {
    const bool was = s_prog.active;
    s_prog.active = false;
    s_prog.out = nullptr;
    s_prog.in = nullptr;
    s_prog.in_left = 0;
    if (was && s_altscreen) vt_puts("\x1b[?1049l");   // a program that died on the alternate screen
    if (s_prog.requested) {
        s_prog.requested = false;
        s_req.status = status;
        xSemaphoreGive(s_req.done);
    }
    raw_update();
}

void prog_poll(void) {
    if (s_prog.in) {
        while (s_prog.in_left) {
            const size_t w = nv_wasm_exec_write_stdin(s_prog.in, s_prog.in_left);
            if (!w) break;
            s_prog.in += w;
            s_prog.in_left -= w;
        }
        if (!s_prog.in_left) { nv_wasm_exec_close_stdin(); s_prog.in = nullptr; }
    }
    prog_drain();
    raw_update();   // the program may have switched to / from the alternate screen
    const nv_wrun_state_t st = nv_wasm_exec_state();
    if (st == NV_WRUN_DONE) {
        prog_drain();   // the tail may have landed after the first drain
        bool ok = false; uint32_t ms = 0; char err[128] = "";
        nv_wasm_exec_collect(&ok, &ms, err, sizeof err);
        if (!ok && !s_prog.aborted) {
            char b[200];
            snprintf(b, sizeof b, "\x1b[0m\r\n%s: %s\r\n", s_prog.id, err[0] ? err : "failed");
            vt_puts(b);
        }
        prog_finish(ok ? 0 : s_prog.aborted ? 130 : 1);
    } else if (st == NV_WRUN_IDLE) {   // collected elsewhere (engine reclaimed)
        prog_finish(1);
    }
}

void prog_stop_retry(void) {
    if (s_prog.retry) { lv_timer_delete(s_prog.retry); s_prog.retry = nullptr; }
    s_prog.wait_t0 = 0;
}

bool prog_start(void);
void prog_retry_cb(lv_timer_t *) { prog_start(); }

// Start the requested program (s_prog.id / args). false = it cannot run (shell answered).
bool prog_start(void) {
    nv_wasm_app_t app;
    if (!nv_wasm_load_manifest(s_prog.id, &app)) { prog_stop_retry(); prog_finish(127); return false; }
    if (nv_wasm_app_is_game(&app)) { prog_stop_retry(); prog_finish(126); return false; }
    char err[96] = "";
    nv_wasm_exec_set_console(s_prog.args);
    if (!nv_wasm_exec_start(&app, err, sizeof err)) {
        const bool busy = !strcmp(err, "busy");
        // Opened from Home straight out of another WASM app: that app's run was aborted by its
        // teardown a moment ago and is still unwinding. Retry quietly until it lands rather than
        // failing a console tile with "another app is running".
        if (busy && nv_wasm_exec_stopping()) {
            if (!s_prog.wait_t0) s_prog.wait_t0 = lv_tick_get();
            if (lv_tick_elaps(s_prog.wait_t0) < kProgStartWaitMs) {
                if (!s_prog.retry) s_prog.retry = lv_timer_create(prog_retry_cb, kProgPollMs, nullptr);
                return true;
            }
        }
        if (s_prog.retry) NV_LOGE("term", "'%s': previous run did not stop within %u ms", s_prog.id,
                                  (unsigned)kProgStartWaitMs);
        prog_stop_retry();
        char b[200];
        snprintf(b, sizeof b, "%s: %s\r\n", s_prog.id, busy ? "another app is running" : err);
        vt_puts(b);
        prog_finish(1);
        return false;
    }
    prog_stop_retry();
    s_prog.active = true;
    s_prog.aborted = false;
    s_org_valid = false;
    raw_update();
    return true;
}

void prog_take_request(void) {
    s_req.pending = false;
    snprintf(s_prog.id, sizeof s_prog.id, "%s", s_req.id);
    snprintf(s_prog.args, sizeof s_prog.args, "%s", s_req.args);
    s_prog.in = s_req.in;
    s_prog.in_left = s_req.in ? s_req.in_len : 0;
    s_prog.piped = s_req.in != nullptr;
    s_prog.out = s_req.out;
    s_prog.requested = true;
    prog_start();
}

// ---------------------------------------------------------------- input

void hist_push(const char *line) {
    if (!line[0] || !s_hist) return;
    if (s_hist_n && !strcmp(s_hist[s_hist_n - 1], line)) { s_hist_pos = s_hist_n; return; }
    if (s_hist_n == kHistMax) {
        memmove(s_hist[0], s_hist[1], sizeof s_hist[0] * (kHistMax - 1));
        s_hist_n--;
    }
    snprintf(s_hist[s_hist_n++], sizeof s_hist[0], "%s", line);
    s_hist_pos = s_hist_n;
}

// Enter in cooked mode.
void submit_cb(lv_event_t *) {
    if (!s_input) return;
    if (s_raw) { raw_key(VTERM_KEY_ENTER); return; }
    char line[256];
    snprintf(line, sizeof line, "%s", lv_textarea_get_text(s_input));
    if (s_prog.active) {
        if (s_prog.piped) return;   // its input comes from the pipe
        edit_commit();
        input_set_quiet("");
        hist_push(line);
        char in[260];
        const int n = snprintf(in, sizeof in, "%s\n", line);
        prog_stdin(in, (size_t)(n < 0 ? 0 : n < (int)sizeof in ? n : (int)sizeof in - 1));
        return;
    }
    if (sh_busy() || s_req.pending || s_prog.retry) return;   // a command is running: type ahead
    edit_commit();
    input_set_quiet("");
    hist_push(line);
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || !sh_run(line)) shell_prompt();   // empty line: straight back to the prompt
}

void key_ctrl_c(void) {
    if (s_raw) { raw_char('c', VTERM_MOD_CTRL); return; }   // full-screen programs handle ^C themselves
    if (s_prog.active) {
        edit_commit();
        input_set_quiet("");
        vt_puts("^C\r\n");
        s_prog.aborted = true;
        sh_interrupt();         // the rest of the line (lua x; ls) stops too, as in bash
        nv_wasm_exec_abort();   // the run lands in DONE; prog_poll answers the shell with 130
    } else if (sh_busy()) {
        vt_puts("^C");
        sh_interrupt();
    } else {
        // At the prompt: abandon the line being typed, as bash does.
        lv_textarea_set_cursor_pos(s_input, LV_TEXTAREA_CURSOR_LAST);
        edit_render();
        vt_puts("^C\r\n");
        input_set_quiet("");
        shell_prompt();
    }
}

void key_ctrl_d(void) {
    if (s_raw) { raw_char('d', VTERM_MOD_CTRL); return; }
    if (!s_prog.active || s_prog.piped) return;
    // Whatever is still typed goes first, without a newline (Ctrl-D semantics).
    const char *t = lv_textarea_get_text(s_input);
    if (t[0]) {
        nv_wasm_exec_write_stdin(t, strlen(t));
        lv_textarea_set_cursor_pos(s_input, LV_TEXTAREA_CURSOR_LAST);
        edit_render();
        input_set_quiet("");
    }
    s_org_valid = false;
    nv_wasm_exec_close_stdin();
}

// Ctrl-L: clear the screen, keep the line being typed.
void key_ctrl_l(void) {
    vt_puts("\x1b[H\x1b[2J");
    s_org_valid = false;
    if (!sh_busy() && !s_prog.active) shell_prompt();
    else edit_render();
}

void key_hist(int dir) {
    if (!s_input || !s_hist_n) return;
    s_hist_pos += dir;
    if (s_hist_pos < 0) s_hist_pos = 0;
    if (s_hist_pos >= s_hist_n) {   // past the newest: an empty line again
        s_hist_pos = s_hist_n;
        lv_textarea_set_text(s_input, "");
        return;
    }
    lv_textarea_set_text(s_input, s_hist[s_hist_pos]);
}

// Byte offset of a character index.
size_t char_byte(const char *t, uint32_t chars) {
    size_t b = 0;
    while (t[b] && chars) {
        b++;
        while (t[b] && ((unsigned char)t[b] & 0xC0) == 0x80) b++;
        chars--;
    }
    return b;
}

// Candidates in columns under the line, directories in blue.
void print_columns(const char *list) {
    const char *start[256];
    int n = 0, longest = 1;
    for (const char *p = list; *p && n < 256;) {
        start[n++] = p;
        const char *e = strchr(p, '\n');
        const int l = (int)(e ? e - p : (int)strlen(p));
        if (l > longest) longest = l;
        p = e ? e + 1 : p + l;
    }
    const int colw = longest + 2;
    int cols = s_cols / colw;
    if (cols < 1) cols = 1;
    const int rows = (n + cols - 1) / cols;
    char line[kMaxCols * 2 + 64];
    for (int r = 0; r < rows; r++) {
        int o = 0;
        for (int c = 0; c < cols; c++) {
            const int i = c * rows + r;
            if (i >= n) break;
            const char *e = strchr(start[i], '\n');
            const int l = (int)(e ? e - start[i] : (int)strlen(start[i]));
            const bool dir = l && start[i][l - 1] == '/';
            if (dir) o += snprintf(line + o, sizeof line - o, "\x1b[1;34m");
            o += snprintf(line + o, sizeof line - o, "%.*s", l, start[i]);
            if (dir) o += snprintf(line + o, sizeof line - o, "\x1b[0m");
            if ((c + 1) * rows + r < n)
                for (int pad = colw - l; pad > 0 && o < (int)sizeof line - 3; pad--) line[o++] = ' ';
            if (o >= (int)sizeof line - 8) break;
        }
        line[o] = '\0';
        vt_puts(line);
        vt_puts("\r\n");
    }
}

void key_tab(void) {
    if (s_raw) { raw_key(VTERM_KEY_TAB); return; }
    if (!s_input || s_prog.active || sh_busy()) return;
    const char *t = lv_textarea_get_text(s_input);
    const size_t cur = char_byte(t, lv_textarea_get_cursor_pos(s_input));
    static char ins[256];
    static char list[4096];
    const int n = sh_complete(t, cur, ins, sizeof ins, list, sizeof list);
    if (ins[0]) lv_textarea_add_text(s_input, ins);
    if (n > 1 && list[0]) {
        // The line so far stays up with the list under it, then a fresh prompt with the same line.
        const uint32_t pos = lv_textarea_get_cursor_pos(s_input);
        lv_textarea_set_cursor_pos(s_input, LV_TEXTAREA_CURSOR_LAST);
        edit_render();
        vt_puts("\r\n");
        print_columns(list);
        lv_textarea_set_cursor_pos(s_input, pos);
        shell_prompt();
        edit_render();
    }
}

void key_ctrl(char c) {
    if (s_raw) { raw_char((uint32_t)c, VTERM_MOD_CTRL); return; }
    switch (c) {
        case 'c': key_ctrl_c(); return;
        case 'd': key_ctrl_d(); return;
        case 'l': key_ctrl_l(); return;
        case 'a': lv_textarea_set_cursor_pos(s_input, 0); break;
        case 'e': lv_textarea_set_cursor_pos(s_input, LV_TEXTAREA_CURSOR_LAST); break;
        case 'u': {   // delete back to the start of the line
            const uint32_t pos = lv_textarea_get_cursor_pos(s_input);
            for (uint32_t i = 0; i < pos; i++) lv_textarea_delete_char(s_input);
            break;
        }
        case 'k': {   // delete to the end of the line
            const uint32_t chars = text_chars(lv_textarea_get_text(s_input));
            const uint32_t pos = lv_textarea_get_cursor_pos(s_input);
            for (uint32_t i = pos; i < chars; i++) lv_textarea_delete_char_forward(s_input);
            break;
        }
        case 'w': {   // delete the word before the cursor
            const char *t = lv_textarea_get_text(s_input);
            size_t b = char_byte(t, lv_textarea_get_cursor_pos(s_input));
            while (b > 0 && t[b - 1] == ' ') { lv_textarea_delete_char(s_input); t = lv_textarea_get_text(s_input); b--; }
            while (b > 0 && t[b - 1] != ' ') {
                lv_textarea_delete_char(s_input);
                t = lv_textarea_get_text(s_input);
                b = char_byte(t, lv_textarea_get_cursor_pos(s_input));
            }
            break;
        }
        case 'p': key_hist(-1); return;
        case 'n': key_hist(+1); return;
        default: return;
    }
    edit_render();
}

// Hardware / remote keys (nv_ime key hook).
bool input_key_hook(lv_obj_t *, int key, char ctrl) {
    if (ctrl) { key_ctrl(ctrl); return true; }
    if (s_raw) {
        switch (key) {
            case NV_IME_RK_ENTER:     raw_key(VTERM_KEY_ENTER); return true;
            case NV_IME_RK_ESC:       raw_key(VTERM_KEY_ESCAPE); return true;
            case NV_IME_RK_BACKSPACE: raw_key(VTERM_KEY_BACKSPACE); return true;
            case NV_IME_RK_DELETE:    raw_key(VTERM_KEY_DEL); return true;
            case NV_IME_RK_TAB:       raw_key(VTERM_KEY_TAB); return true;
            case NV_IME_RK_LEFT:      raw_key(VTERM_KEY_LEFT); return true;
            case NV_IME_RK_RIGHT:     raw_key(VTERM_KEY_RIGHT); return true;
            case NV_IME_RK_UP:        raw_key(VTERM_KEY_UP); return true;
            case NV_IME_RK_DOWN:      raw_key(VTERM_KEY_DOWN); return true;
            case NV_IME_RK_HOME:      raw_key(VTERM_KEY_HOME); return true;
            case NV_IME_RK_END:       raw_key(VTERM_KEY_END); return true;
            default:                  return false;
        }
    }
    switch (key) {
        case NV_IME_RK_UP:   key_hist(-1); return true;
        case NV_IME_RK_DOWN: key_hist(+1); return true;
        case NV_IME_RK_TAB:  key_tab();    return true;
        default:             return false;   // editing keys act on the textarea; echo follows
    }
}

// Text going into the textarea: raw mode sends it on instead; an armed Ctrl makes a letter Ctrl+letter.
void input_insert_cb(lv_event_t *e) {
    if (s_quiet) return;
    const char *t = (const char *)lv_event_get_param(e);
    if (!t || !t[0]) return;
    if (s_raw) {
        lv_textarea_set_insert_replace(s_input, "");
        if (!strcmp(t, "\n")) raw_key(VTERM_KEY_ENTER);
        else raw_text(t);
        return;
    }
    if (!s_ctrl_armed || t[1]) return;
    const char c = (char)((t[0] >= 'A' && t[0] <= 'Z') ? t[0] + 32 : t[0]);
    if (c < 'a' || c > 'z') return;
    lv_textarea_set_insert_replace(s_input, "");
    raw_mod();   // disarm
    key_ctrl(c);
}

void input_changed_cb(lv_event_t *) {
    if (s_quiet || !s_input) return;
    if (s_raw) {   // the sentinel shrank: Backspace
        if (!lv_textarea_get_text(s_input)[0]) {
            raw_key(VTERM_KEY_BACKSPACE);
            input_set_quiet(kRawSentinel);
        }
        return;
    }
    edit_render();
}

// ---------------------------------------------------------------- extra keys

enum : uint8_t { K_ESC, K_TAB, K_CTRL, K_CTRL_C, K_CTRL_D, K_LEFT, K_UP, K_DOWN, K_RIGHT, K_TEXT };
struct ExtraKey { const char *label; uint8_t action; const char *text; };
constexpr ExtraKey kKeys[] = {
    {"Esc", K_ESC, nullptr}, {"Tab", K_TAB, nullptr}, {"Ctrl", K_CTRL, nullptr},
    {"^C", K_CTRL_C, nullptr}, {"^D", K_CTRL_D, nullptr},
    {"\xE2\x86\x90", K_LEFT, nullptr},  {"\xE2\x86\x91", K_UP, nullptr},    // ← ↑
    {"\xE2\x86\x93", K_DOWN, nullptr},  {"\xE2\x86\x92", K_RIGHT, nullptr}, // ↓ →
    {"/", K_TEXT, "/"}, {"-", K_TEXT, "-"}, {"|", K_TEXT, "|"}, {"~", K_TEXT, "~"},
};

void ctrl_arm(bool on) {
    s_ctrl_armed = on;
    if (!s_ctrl_key) return;
    lv_obj_set_style_bg_color(s_ctrl_key, lv_color_hex(on ? kSel : kKey), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s_ctrl_key, 0), lv_color_hex(on ? kBg : kFg), 0);
}

void extra_key_cb(lv_event_t *e) {
    const ExtraKey *k = (const ExtraKey *)lv_event_get_user_data(e);
    if (!s_input) return;
    s_view_off = 0;
    switch (k->action) {
        case K_ESC:    if (s_raw) raw_key(VTERM_KEY_ESCAPE); break;
        case K_TAB:    key_tab(); break;
        case K_CTRL:   ctrl_arm(!s_ctrl_armed); break;
        case K_CTRL_C: key_ctrl_c(); break;
        case K_CTRL_D: key_ctrl_d(); break;
        case K_UP:     if (s_raw) raw_key(VTERM_KEY_UP); else key_hist(-1); break;
        case K_DOWN:   if (s_raw) raw_key(VTERM_KEY_DOWN); else key_hist(+1); break;
        case K_LEFT:
            if (s_raw) raw_key(VTERM_KEY_LEFT);
            else { lv_textarea_cursor_left(s_input); edit_render(); }
            break;
        case K_RIGHT:
            if (s_raw) raw_key(VTERM_KEY_RIGHT);
            else { lv_textarea_cursor_right(s_input); edit_render(); }
            break;
        default:
            if (s_raw) raw_text(k->text);
            else lv_textarea_add_text(s_input, k->text);
            break;
    }
    lv_obj_invalidate(s_view);
}

// ---------------------------------------------------------------- tty timer

void close_cb(void *) { nv_ui_close_app(); }

void tty_tick(lv_timer_t *) {
    ring_drain(kDrainBudget);
    if (s_ui.pending) {
        s_ui.pending = false;
        s_ui.fn(s_ui.arg);
        xSemaphoreGive(s_ui.done);
    }
    raw_update();
    if (s_req.pending && !s_prog.active && !s_prog.retry && ring_empty()) prog_take_request();
    if (s_prog.active) prog_poll();
    // The shell finished a line and everything it wrote is on screen: prompt again.
    if (!sh_busy() && s_jobs_seen != sh_jobs_done() && !s_req.pending && ring_empty()) {
        s_jobs_seen = sh_jobs_done();
        shell_prompt();
    }
    // The on-screen keyboard's own arrow keys move the textarea cursor without an event.
    if (!s_raw && s_input && lv_textarea_get_cursor_pos(s_input) != s_last_cur &&
        (lv_textarea_get_text(s_input)[0] || s_org_valid))
        edit_render();
    if (s_vs) vterm_screen_flush_damage(s_vs);
    if (s_exit_req.exchange(false)) lv_async_call(close_cb, nullptr);
}

void blink_cb(lv_timer_t *) {
    s_blink_on = !s_blink_on;
    if (!s_view_off) invalidate_rows(s_cur.row, s_cur.row);
}

// ---------------------------------------------------------------- screen

// Drag scrolls back through the scrollback; a tap gives the terminal the keyboard.
void view_event(lv_event_t *e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_drag_acc = 0;
        s_dragged = false;
    } else if (code == LV_EVENT_PRESSING) {
        lv_point_t v;
        lv_indev_get_vect(lv_indev_active(), &v);
        s_drag_acc += v.y;
        if (s_drag_acc > 12 || s_drag_acc < -12) s_dragged = true;
        const int lines = s_drag_acc / kCellH;
        if (lines && !s_altscreen) {
            s_drag_acc -= lines * kCellH;
            int off = s_view_off + lines;
            off = off < 0 ? 0 : off > s_sb_count ? s_sb_count : off;
            if (off != s_view_off) { s_view_off = off; lv_obj_invalidate(s_view); }
        }
    } else if (code == LV_EVENT_CLICKED && !s_dragged && s_input) {
        lv_obj_add_state(s_input, LV_STATE_FOCUSED);
        lv_obj_send_event(s_input, LV_EVENT_FOCUSED, nullptr);
        lv_obj_invalidate(s_view);
    } else if (code == LV_EVENT_SIZE_CHANGED) {
        grid_resize();
    }
}

void apply_kb_pad(void *);

// Focus: the cursor turns solid / hollow. On a re-focus with the keyboard already up the IME pads
// the root again: set the padding back to what the keyboard really covers.
void input_focus_changed(lv_event_t *e) {
    if (s_view) invalidate_rows(s_cur.row, s_cur.row);
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED && s_kb_h) lv_async_call(apply_kb_pad, nullptr);
}

void apply_kb_pad(void *) {
    if (!s_root) return;
    int32_t pad = 0;
    if (s_kb_h > 0) {
        lv_area_t a;
        lv_obj_get_coords(s_root, &a);
        const int32_t below = lv_display_get_vertical_resolution(nullptr) - (a.y2 + 1);
        pad = s_kb_h - below;
        if (pad < 0) pad = 0;
    }
    lv_obj_set_style_pad_bottom(s_root, pad, 0);
    grid_resize();
}

// NV_EV_IME_VISIBILITY comes from the keyboard code on the LVGL thread: relayout once its own
// focus handling has finished.
void on_ime(nv_event_t, const void *d, void *) {
    const auto *v = static_cast<const nv_ime_visibility_t *>(d);
    s_kb_h = (v && v->visible) ? v->height : 0;
    lv_async_call(apply_kb_pad, nullptr);
}

void autorun_cb(void *) {
    if (!s_input || !s_autorun[0]) return;
    if (sh_busy()) { lv_async_call(autorun_cb, nullptr); return; }   // an old line is winding down
    char line[sizeof s_autorun];
    snprintf(line, sizeof line, "%s", s_autorun);
    s_autorun[0] = '\0';
    input_set_quiet(line);
    edit_commit();
    input_set_quiet("");
    hist_push(line);
    if (!sh_run(line)) shell_prompt();
}

void page_deleted(lv_event_t *) {
    s_tty_open = false;                        // the shell's writes and waits give up
    s_sh_raw = false;
    s_input = nullptr;                         // children may already be gone: no widget access
    s_view = nullptr;
    s_raw = false;
    sh_interrupt();
    nv_event_unsubscribe(NV_EV_IME_VISIBILITY, on_ime, nullptr);
    lv_async_call_cancel(apply_kb_pad, nullptr);
    lv_async_call_cancel(autorun_cb, nullptr);
    nv_ime_hide();
    if (s_tick) { lv_timer_delete(s_tick); s_tick = nullptr; }
    if (s_blink) { lv_timer_delete(s_blink); s_blink = nullptr; }
    prog_stop_retry();                         // nor does a start still waiting for the engine
    if (s_prog.active) nv_wasm_exec_abort();   // a program never outlives its screen
    prog_finish(130);   // an aborted run parks in DONE; the engine auto-collects it on the next start
    if (s_ui.pending.exchange(false)) xSemaphoreGive(s_ui.done);
    s_req.pending = false;
    ring_reset();
    if (s_vt) { vterm_free(s_vt); s_vt = nullptr; s_vs = nullptr; s_vst = nullptr; }
    // The scrollback goes with the screen (solo mode: the next app gets the PSRAM back).
    heap_caps_free(s_sb); s_sb = nullptr;
    heap_caps_free(s_sb_cols); s_sb_cols = nullptr;
    s_sb_count = s_sb_head = s_view_off = 0;
    s_input = nullptr;
    s_root = nullptr;
    s_ctrl_key = nullptr;
    s_ctrl_armed = false;
    s_raw = false;
    s_kb_h = 0;
}

void build_keys(lv_obj_t *root) {
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bar, lv_color_hex(kKeyBg), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bar, 6, 0);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    for (const ExtraKey &k : kKeys) {
        lv_obj_t *b = lv_obj_create(bar);
        lv_obj_remove_style_all(b);
        lv_obj_set_height(b, 40);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(kKey), 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(kKeyDown), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        // Pressing a key must not take focus from the terminal (that would drop the keyboard).
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, extra_key_cb, LV_EVENT_CLICKED, (void *)&k);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, k.label);
        lv_obj_set_style_text_font(l, &s_mono, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(kFg), 0);
        lv_obj_center(l);
        if (k.action == K_CTRL) s_ctrl_key = b;
    }
}

// The IME's sink: a one-line textarea nobody sees (no background, border, text or cursor drawn).
void style_hidden_input(lv_obj_t *ta) {
    static const lv_style_selector_t kStates[] = {
        LV_STATE_DEFAULT, LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY, LV_STATE_EDITED, LV_STATE_PRESSED,
    };
    for (lv_style_selector_t s : kStates) {
        lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, s);
        lv_obj_set_style_border_width(ta, 0, s);
        lv_obj_set_style_outline_width(ta, 0, s);
        lv_obj_set_style_shadow_width(ta, 0, s);
        lv_obj_set_style_text_opa(ta, LV_OPA_TRANSP, s);
    }
    const lv_style_selector_t cur = (lv_style_selector_t)LV_PART_CURSOR | LV_STATE_FOCUSED;
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, cur);
    lv_obj_set_style_border_width(ta, 0, cur);
    lv_obj_set_style_pad_all(ta, 0, 0);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(ta, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(ta, 4, 4);
    lv_obj_set_pos(ta, 0, 0);
}

bool tty_init_once(void) {
    if (s_ring) return true;
    s_ring = (char *)heap_caps_malloc(kRing, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_hist = (char (*)[256])heap_caps_calloc(kHistMax, 256, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ring_mtx = xSemaphoreCreateMutex();
    s_req.done = xSemaphoreCreateBinary();
    s_ui.done = xSemaphoreCreateBinary();
    s_keys = xStreamBufferCreate(1024, 1);
    if (!s_ring || !s_hist || !s_ring_mtx || !s_req.done || !s_ui.done || !s_keys) {
        heap_caps_free(s_ring);
        s_ring = nullptr;
        return false;
    }
    return true;
}

bool vt_create(void) {
    s_sb = (SbCell *)heap_caps_malloc(sizeof(SbCell) * kSbLines * kMaxCols, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_sb_cols = (uint8_t *)heap_caps_calloc(kSbLines, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    VTermBuilder b = {};
    b.rows = s_rows;
    b.cols = s_cols;
    b.allocator = &kVtAlloc;
    s_vt = vterm_build(&b);
    if (!s_vt || !s_sb || !s_sb_cols) return false;
    vterm_set_utf8(s_vt, 1);
    vterm_output_set_callback(s_vt, cb_output, nullptr);
    s_vs = vterm_obtain_screen(s_vt);
    s_vst = vterm_obtain_state(s_vt);
    vterm_screen_set_callbacks(s_vs, &kScreenCb, nullptr);
    vterm_screen_set_damage_merge(s_vs, VTERM_DAMAGE_ROW);
    vterm_screen_enable_altscreen(s_vs, 1);
    vterm_screen_enable_reflow(s_vs, true);
    vterm_screen_reset(s_vs, 1);
    for (int i = 0; i < 16; i++) {
        VTermColor c;
        vterm_color_rgb(&c, (uint8_t)(kAnsi[i] >> 16), (uint8_t)(kAnsi[i] >> 8), (uint8_t)kAnsi[i]);
        vterm_state_set_palette_color(s_vst, i, &c);
    }
    vterm_state_set_bold_highbright(s_vst, 1);
    s_sb_head = s_sb_count = s_view_off = 0;
    return true;
}

void terminal_build(lv_obj_t *content) {
    s_prog = Prog{};
    s_hist_pos = s_hist_n;
    s_kb_h = 0;
    s_org_valid = false;
    s_raw = false;
    s_altscreen = false;
    s_sh_raw = false;
    if (!s_mono_ok) {
        s_mono = nv_font_mono_17;
        s_mono.fallback = &nv_font_14;
        s_mono_ok = true;
        const int32_t w = lv_font_get_glyph_width(&s_mono, 'M', 0);
        s_cell_w = w > 0 ? w : 10;
    }

    // Edge to edge: the terminal is the whole app area, no card, no margins.
    s_root = lv_obj_create(content);
    lv_obj_t *root = s_root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(root, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, page_deleted, LV_EVENT_DELETE, nullptr);

    s_view = lv_obj_create(root);
    lv_obj_remove_style_all(s_view);
    lv_obj_set_width(s_view, lv_pct(100));
    lv_obj_set_flex_grow(s_view, 1);
    lv_obj_clear_flag(s_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_view, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(s_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_view, view_draw, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(s_view, view_event, LV_EVENT_ALL, nullptr);

    // URL class: one line, no auto-capitalised first letter (commands and code are lower case).
    // RET_ENTER: Enter hands the line on and the keyboard stays up for the next one.
    s_input = nv_kit_textarea_ex(root, nullptr, true, NV_IME_URL, NV_IME_RET_ENTER);
    style_hidden_input(s_input);
    lv_obj_add_event_cb(s_input, submit_cb, LV_EVENT_READY, nullptr);   // keyboard / hardware Enter
    lv_obj_add_event_cb(s_input, input_insert_cb, LV_EVENT_INSERT, nullptr);
    lv_obj_add_event_cb(s_input, input_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_input, input_focus_changed, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(s_input, input_focus_changed, LV_EVENT_DEFOCUSED, nullptr);
    nv_ime_set_key_hook(s_input, input_key_hook);

    build_keys(root);
    nv_event_subscribe(NV_EV_IME_VISIBILITY, on_ime, nullptr);

    // Grid size from the view's size; the terminal follows it (keyboard up = fewer rows).
    lv_obj_update_layout(root);
    s_cols = (int)((lv_obj_get_width(s_view) - 2 * kPadX) / s_cell_w);
    s_rows = (int)((lv_obj_get_height(s_view) - 2 * kPadY) / kCellH);
    s_cols = s_cols < 20 ? 20 : s_cols > kMaxCols ? kMaxCols : s_cols;
    s_rows = s_rows < 4 ? 4 : s_rows > kMaxRows ? kMaxRows : s_rows;
    s_cols_pub = s_cols;
    s_rows_pub = s_rows;

    const bool ok = tty_init_once() && vt_create() && sh_start();
    if (!s_vt) { s_view = nullptr; return; }

    // Login banner, then the prompt.
    char b[200];
    snprintf(b, sizeof b, "Welcome to NucleoOS Anima %s (ESP32-P4 riscv32)\r\n\r\n", nv_ota_running_version());
    vt_puts(b);
    vt_puts(" * Commands:  help         * Programs:  apps\r\n");
    vt_puts(" * Keys:      Tab completes, \xE2\x86\x91\xE2\x86\x93 history, ^C interrupts, drag to scroll back\r\n\r\n");
    if (!ok) {
        vt_puts("sh: out of memory - the shell could not start\r\n");
        return;
    }
    ring_reset();
    xStreamBufferReset(s_keys);
    s_jobs_seen = sh_jobs_done();
    s_tty_open = true;
    s_tick = lv_timer_create(tty_tick, kTickMs, nullptr);
    s_blink = lv_timer_create(blink_cb, 530, nullptr);
    if (!sh_busy()) shell_prompt();   // else: a line from a previous visit is still unwinding
    // A console app's tile: run it once the screen is up (after the open animation's first frame).
    if (s_autorun[0]) lv_async_call(autorun_cb, nullptr);
}

const NvApp kTerminalApp = {"terminal", "Terminal", &nv_icon_terminal, 1u << 20, terminal_build,
                            NV_STR_APP_TERMINAL, nullptr};

}  // namespace

// ================================================================= tty contract (term_sh.h)

void term_tty_write(const char *s, size_t n) {
    while (n) {
        if (!s_tty_open.load()) return;   // screen gone: output is dropped
        xSemaphoreTake(s_ring_mtx, portMAX_DELAY);
        const size_t room = kRing - (s_ring_head - s_ring_tail);
        const size_t k = n < room ? n : room;
        for (size_t i = 0; i < k; i++) s_ring[(s_ring_head + i) % kRing] = s[i];
        s_ring_head += k;
        xSemaphoreGive(s_ring_mtx);
        s += k;
        n -= k;
        if (n) vTaskDelay(pdMS_TO_TICKS(10));   // full: wait for the screen to catch up
    }
}

int term_tty_cols(void) { return s_cols_pub.load(); }
int term_tty_rows(void) { return s_rows_pub.load(); }

void term_tty_raw(bool on) {
    if (on && s_keys) xStreamBufferReset(s_keys);
    s_sh_raw = on;
}

int term_tty_read(char *buf, size_t n, int timeout_ms) {
    if (!s_keys || !s_tty_open.load()) return -1;
    return (int)xStreamBufferReceive(s_keys, buf, n, pdMS_TO_TICKS(timeout_ms));
}

int term_prog_run(const char *id, const char *args, const char *in, size_t in_len, const ShSink *out) {
    if (!s_tty_open.load()) return 130;
    xSemaphoreTake(s_req.done, 0);   // no stale answer
    snprintf(s_req.id, sizeof s_req.id, "%s", id);
    snprintf(s_req.args, sizeof s_req.args, "%s", args ? args : "");
    s_req.in = in;
    s_req.in_len = in_len;
    s_req.out = out;
    s_req.status = 1;
    s_req.pending = true;
    while (xSemaphoreTake(s_req.done, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (!s_tty_open.load()) { s_req.pending = false; return 130; }
    }
    return s_req.status;
}

bool term_ui_call(void (*fn)(void *), void *arg) {
    if (!s_tty_open.load()) return false;
    xSemaphoreTake(s_ui.done, 0);
    s_ui.fn = fn;
    s_ui.arg = arg;
    s_ui.pending = true;
    while (xSemaphoreTake(s_ui.done, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (!s_tty_open.load()) { s_ui.pending = false; return false; }
    }
    return true;
}

void term_request_exit(void) { s_exit_req = true; }

int         term_hist_count(void) { return s_hist_n; }
const char *term_hist_at(int i) { return (s_hist && i >= 0 && i < s_hist_n) ? s_hist[i] : ""; }
void        term_hist_clear(void) { s_hist_n = 0; s_hist_pos = 0; }

// ================================================================= app

void terminal_app_register(void) { nv_app_register(&kTerminalApp); }

void terminal_build_with(lv_obj_t *content, const char *command) {
    snprintf(s_autorun, sizeof s_autorun, "%s", command ? command : "");
    terminal_build(content);
}
