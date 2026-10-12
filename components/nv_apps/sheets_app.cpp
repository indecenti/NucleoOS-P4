// sheets_app.cpp — Fogli, the spreadsheet: a native grid over sc-im running as its engine.
//
// This file draws the sheet and takes touch, mouse and keyboard; sc-im (system app "sc-im",
// ports/tui/scim/nv_engine.c) holds the document: values, formulas, undo, sheets, and the file
// formats (xlsx xls ods csv tsv in, xlsx csv tsv out). They talk over the console pipe of nv_wasm's
// asynchronous runner, one command per line; the engine answers tab-separated records and ends each
// batch with "\x1e<id>". Nothing here blocks the LVGL thread: a 30 ms timer pumps the pipe.
//
// What people see is a spreadsheet as they know it: rows from 1, formulas in Excel's notation and
// their own language (=SOMMA(B2:B8) in Italian, ';' and the decimal comma), a cell edited in place
// with a real caret, tap a cell while writing a formula to put its reference in, long press or
// right click for the context menu, drag the corner handles to stretch a selection, the sum of the
// selection in the status bar, and ANIMA reading the selected cells to explain, check or fill them.
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <atomic>
#include <cstdlib>
#include <initializer_list>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "apps_internal.h"
#include "nucleo_anima.h"
#include "nucleo_anima_conv.h"
#include "nv_app.h"
#include "nv_clipboard.h"
#include "nv_config.h"
#include "nv_event_bus.h"
#include "nv_fonts.h"
#include "nv_hid_host.h"
#include "nv_i18n.h"
#include "nv_icons.h"
#include "nv_ime.h"
#include "nv_log.h"
#include "nv_mem_attr.h"
#include "nv_notify.h"
#include "nv_open.h"
#include "nv_theme.h"
#include "nv_ui.h"
#include "nv_ui_focus.h"
#include "nv_ui_host.h"
#include "nv_ui_kit.h"
#include "nv_ui_select.h"
#include "nv_wasm.h"
#include "nv_bgwork.h"

void recents_add(const char *path);
void back_handler(void);
void ai_send(void);

extern "C" {
extern const lv_font_t nv_font_sheet_18;   // toolbar icons (FontAwesome) + Σ
extern const lv_font_t nv_font_sheet_14;   // € and typographic marks, behind the grid font
}

namespace {

#include "sheets_strings.h"
#include "sheets_templates.h"

const char *TAG = "sheets";

// ---------------------------------------------------------------- look and limits
constexpr int32_t kRowH = 30;          // grid row
constexpr int32_t kHdrH = 26;          // column letters
constexpr int32_t kRowHdrW = 56;       // row numbers
constexpr int32_t kCharPx = 8;         // sc-im widths are characters
constexpr int32_t kPadX = 6;
constexpr int32_t kHandleR = 9;        // selection handle (touch)
constexpr int kMaxCol = 701;           // ZZ
constexpr int kMaxRow = 65535;
constexpr int kCR = 160, kCC = 48;     // cached region of cells
constexpr int kPool = 128 * 1024;
constexpr int kTickMs = 30;
constexpr int kOut = 32 * 1024;        // to the engine
constexpr int kIn = 16 * 1024;         // one record
constexpr int kTsvMax = 6 * 1024;      // data ANIMA reads
constexpr int kAiOut = 6 * 1024;
constexpr int kMaxFiles = 300;
constexpr const char *kRecoveryDir = "/sdcard/home/.fogli";
constexpr const char *kRecovery = "/sdcard/home/.fogli/ripristino.sc";
constexpr const char *kDocsDir = "/sdcard/home/Documenti";

// FontAwesome code points in nv_font_sheet_18
#define I_SEARCH   "\xEF\x80\x82"
#define I_CHECK    "\xEF\x80\x8C"
#define I_TIMES    "\xEF\x80\x8D"
#define I_REDO     "\xEF\x80\x9E"
#define I_BOLD     "\xEF\x80\xB2"
#define I_ITALIC   "\xEF\x80\xB3"
#define I_WIDTH    "\xEF\x80\xB5"
#define I_PLUS     "\xEF\x81\xA7"
#define I_DOWN     "\xEF\x81\xB8"
#define I_FOLDER   "\xEF\x81\xBC"
#define I_CUT      "\xEF\x83\x84"
#define I_COPY     "\xEF\x83\x85"
#define I_SAVE     "\xEF\x83\x87"
#define I_UNDERL   "\xEF\x83\x8D"
#define I_MAGIC    "\xEF\x83\x90"
#define I_UNDO     "\xEF\x83\xA2"
#define I_PASTE    "\xEF\x83\xAA"
#define I_ERASER   "\xEF\x84\xAD"
#define I_MORE     "\xEF\x85\x82"
#define I_FILE     "\xEF\x85\x9B"
#define I_SORTDN   "\xEF\x85\xA0"
#define I_SORTUP   "\xEF\x85\xA1"
#define I_XLS      "\xEF\x87\x83"
#define I_SEND     "\xEF\x87\x98"
#define I_HISTORY  "\xEF\x87\x9A"
#define I_HASH     "\xEF\x8A\x92"
#define I_PERCENT  "\xEF\x8A\x95"
#define I_SIGMA    "\xCE\xA3"

const char *tr(T id) {
    int l = (int)nv_i18n_get_lang();
    if (l < 0 || l > 4) l = 0;
    const char *s = kT[id][l];
    return s && *s ? s : kT[id][0];
}
int lang_ix(void) {
    int l = (int)nv_i18n_get_lang();
    return l < 0 || l > 4 ? 0 : l;
}
const char *lang_code(void) {
    static const char *const k[] = {"en", "it", "es", "fr", "de"};
    return k[lang_ix()];
}

// ---------------------------------------------------------------- state
struct Cell { int32_t text; uint16_t len; char kind, align; uint8_t style; };
struct Cache {
    int r0, c0;
    int n;
    int32_t pool_n;
    int32_t idx[kCR * kCC];      // -1: empty
    Cell cells[kCR * kCC];
    char pool[kPool];
};

enum Sync : uint8_t { S_NONE, S_READY, S_VIEW_BEGIN, S_VIEW_END, S_TSV_CLIP, S_TSV_AI, S_FIND, S_CHECK, S_PLAIN };
enum Mode : uint8_t { M_NONE, M_PAN, M_SELECT, M_EXTEND, M_COLSEL, M_ROWSEL, M_RESIZE, M_POINT };

struct State {
    // ---- engine link
    bool run;                 // our run is in the runner
    bool ready;               // it answered the first sync
    bool want_start;          // start on the next tick (the runner may still be busy)
    bool stopping;            // EOF sent: waiting for DONE before the next start
    int64_t stop_t;
    char start_path[256];     // the file to start on ("" = blank)
    int start_tpl;            // template to fill once ready (-1 none)
    bool start_recover;       // started on the recovery copy: name it after the original
    char recover_name[256];
    char out[kOut]; int out_n;
    char in[kIn]; int in_n;
    uint32_t sync_next;
    uint8_t sync_kind[64];
    int32_t sync_a[64], sync_b[64];
    uint32_t view_inflight;   // id of the last VIEW_END asked (0: none)
    int view_r0, view_c0;     // region of that request
    bool view_dirty;          // the document changed: the cached cells are stale
    // ---- document
    char path[256];           // "" until saved / opened
    bool modified;
    bool read_only_fmt;       // opened from xls/ods: saving makes an .xlsx
    int nsheets, cursheet, maxrow, maxcol;
    char sheet[24][40];
    uint8_t colw[kMaxCol + 1];
    int32_t colx[kMaxCol + 2];
    // ---- view and selection
    int32_t sx, sy;
    float vx, vy;
    int ar, ac, cr, cc;       // anchor, active
    // ---- active cell text (formula bar), from "X" records
    int xr, xc;
    char xtext[1024];
    // ---- editing
    bool editing, edit_bar, edit_typed;
    int er, ec;
    char edit_orig[1024];
    int pt_pos, pt_len;       // the reference being pointed at (inside the edited text)
    int pt_r0, pt_c0;
    // ---- status
    char status[200];
    // ---- autosave
    uint32_t edits, edits_backed;
    int64_t backup_t;
    // ---- find
    char find_text[128];
    int find_n, find_i;
    int find_r[200], find_c[200];
    // ---- tsv (clipboard / ANIMA)
    char tsv[kTsvMax + 256];
    int tsv_n;
    int tsv_c0;
    uint32_t own_clip_seq;
    // ---- press
    Mode mode;
    bool long_pressed, moved, mouse;
    int32_t px, py, drag_x, drag_y;
    int pr, pc;
    int64_t last_tap_t;
    int last_tap_r, last_tap_c;
    int resize_col;
    int32_t resize_x0, resize_w0;
    // ---- scratch (heap, not static memory: the firmware's static budgets are tight)
    char untitled[64];        // what an unsaved document is called (a template's name, "New sheet")
    char new_dir[200];        // a new document from Files: the folder it is saved in ("" = Documenti)
    char last_intent[300];    // handler + path of the intent already acted on (rebuilds repeat it)
    bool anima_on_ready;      // "Analizza con ANIMA": open the panel and summarize once loaded
    bool anima_after_view;    // ... after the first view (the sheet's size is known then)
    bool big_heap;            // the engine ran out of memory on this file: the next start gets the maximum
    char rl[8][256];          // recent files
    char rbuf[2100];
    char tmp_path[300];
    char tmp_path2[300];
    char ai_q[600];
};

State *S = nullptr;           // PSRAM, kept across rebuilds (theme / language / a new intent)
Cache *s_live = nullptr, *s_stage = nullptr;

// widgets (rebuilt with the page)
lv_obj_t *s_root, *s_tool, *s_fbar, *s_name, *s_fx, *s_grid, *s_tabs, *s_tabrow, *s_status;
lv_obj_t *s_editor, *s_start, *s_modal, *s_modal_ta, *s_ai, *s_ai_ta, *s_ai_out, *s_ai_apply;
lv_obj_t *s_findbar, *s_find_ta, *s_loading;
lv_timer_t *s_tick;
lv_font_t s_font;             // nv_font_14 with € and friends behind it
bool s_font_ok;
bool s_syncing_text;          // copying text between the in-cell editor and the formula bar
bool s_pointing;              // point mode is rewriting the edited text
lv_font_t s_icons;            // toolbar icons, with Montserrat 20 behind them for the letters

// ---------------------------------------------------------------- small helpers
int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int sel_r0() { return S->ar < S->cr ? S->ar : S->cr; }
int used_r1();
int used_c1();
int sel_r1() { return S->ar > S->cr ? S->ar : S->cr; }
int sel_c0() { return S->ac < S->cc ? S->ac : S->cc; }
int sel_c1() { return S->ac > S->cc ? S->ac : S->cc; }
bool sel_single() { return S->ar == S->cr && S->ac == S->cc; }
// the selection's end, kept inside the used area (a whole row or column is selected)
int used_r1() { const int r1 = sel_r1(); return r1 > S->maxrow ? (S->maxrow > sel_r0() ? S->maxrow : sel_r0()) : r1; }
int used_c1() { const int c1 = sel_c1(); return c1 > S->maxcol ? (S->maxcol > sel_c0() ? S->maxcol : sel_c0()) : c1; }

void col_name(int c, char *b) {
    if (c < 26) { b[0] = (char)('A' + c); b[1] = 0; }
    else { b[0] = (char)('A' + c / 26 - 1); b[1] = (char)('A' + c % 26); b[2] = 0; }
}
void cell_name(int r, int c, char *b, size_t n) {
    char cn[4];
    col_name(c, cn);
    snprintf(b, n, "%s%d", cn, r + 1);
}

int32_t col_w(int c) { return S->colw[c] * kCharPx + 2 * kPadX; }
void colx_rebuild(void) {
    int32_t x = 0;
    for (int c = 0; c <= kMaxCol; c++) { S->colx[c] = x; x += col_w(c); }
    S->colx[kMaxCol + 1] = x;
}
int col_at(int32_t x) {                  // content x -> column
    int lo = 0, hi = kMaxCol;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (S->colx[mid] <= x) lo = mid; else hi = mid - 1;
    }
    return lo;
}

const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
bool has_ext(const char *p, const char *ext) {
    const char *d = strrchr(p, '.');
    return d && !strcasecmp(d + 1, ext);
}
bool writable_fmt(const char *p) {
    return has_ext(p, "xlsx") || has_ext(p, "csv") || has_ext(p, "tsv") || has_ext(p, "sc");
}
bool sheet_file(const char *name) {
    static const char *const k[] = {"xlsx", "xls", "ods", "csv", "tsv", "sc"};
    for (const char *e : k) if (has_ext(name, e)) return true;
    return false;
}

// ---------------------------------------------------------------- engine link
void send(const char *fmt, ...) {
    char b[1400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = sizeof b - 2;
    for (int i = 0; i < n; i++) if (b[i] == '\n' || b[i] == '\r') b[i] = ' ';
    b[n++] = '\n';
    if (S->out_n + n > kOut) { NV_LOGW(TAG, "engine queue full: command dropped"); return; }
    memcpy(S->out + S->out_n, b, n);
    S->out_n += n;
}

uint32_t sync(Sync kind, int32_t a = 0, int32_t b = 0) {
    const uint32_t id = ++S->sync_next;
    S->sync_kind[id % 64] = kind;
    S->sync_a[id % 64] = a;
    S->sync_b[id % 64] = b;
    send("nv sync %u", (unsigned)id);
    return id;
}

void request_view(bool force);
void refresh(void);
void grid_invalidate(void) { if (s_grid) lv_obj_invalidate(s_grid); }
void update_title(void);
void update_tabs(void);
void update_namebox(void);
void ask_cell(void);
void ask_stats(void);
void on_ready(void);
void on_line(char *l);
void show_loading(bool on);
void engine_dead(void);
void note_for_anima(void);
void ai_checked(int i, bool ok);
void ai_check_done(void);
void ai_sync_chips(void);
void ai_tick(void);
void ai_reset_topic(void);
void ai_draw_preview(lv_layer_t *layer, const lv_area_t &a, int32_t x0, int32_t y0);
void ai_open_and_summarize(void);
void ai_give_up(T why);
void skill_install(void *);

// Clear and fill a cache region
void cache_reset(Cache *c, int r0, int c0) {
    c->r0 = r0;
    c->c0 = c0;
    c->n = 0;
    c->pool_n = 0;
    memset(c->idx, 0xff, sizeof c->idx);
}
const Cell *cache_get(const Cache *c, int r, int col, const char **text) {
    if (!c) return nullptr;
    const int rr = r - c->r0, cc = col - c->c0;
    if (rr < 0 || cc < 0 || rr >= kCR || cc >= kCC) return nullptr;
    const int32_t i = c->idx[rr * kCC + cc];
    if (i < 0) return nullptr;
    if (text) *text = c->pool + c->cells[i].text;
    return &c->cells[i];
}
bool cache_has(const Cache *c, int r, int col) { return cache_get(c, r, col, nullptr) != nullptr; }

// ---- start / stop
bool engine_start_now(void) {
    auto *app = (nv_wasm_app_t *)heap_caps_malloc(sizeof(nv_wasm_app_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!app) return false;
    if (!nv_wasm_load_manifest("sc-im", app)) {
        heap_caps_free(app);
        S->want_start = false;
        show_loading(false);
        nv_toast(NV_NOTE_ERROR, tr(T_ENGINE_MISSING));
        return false;
    }
    // Memory for the engine: what this file needs, not the manifest's maximum (16 MB would leave the
    // rest of the system, ANIMA's network calls included, with under 1 MB of PSRAM). A compressed
    // workbook unpacks to ~10x its size; text formats to ~4x.
    {
        size_t heap = 6u << 20;
        struct stat st;
        if (S->start_path[0] && stat(S->start_path, &st) == 0) {
            const size_t sz = (size_t)st.st_size;
            const bool packed = has_ext(S->start_path, "xlsx") || has_ext(S->start_path, "ods");
            heap += sz * (packed ? 12 : has_ext(S->start_path, "xls") ? 3 : 4);
        }
        if (S->big_heap || heap > app->ram_budget) heap = app->ram_budget;
        app->ram_budget = (uint32_t)heap;
        NV_LOGI(TAG, "engine heap %u KB for %s", (unsigned)(heap >> 10), S->start_path[0] ? S->start_path : "a new sheet");
    }
    char args[400];
    if (S->start_path[0]) snprintf(args, sizeof args, "--nocurses --nv_engine \"%s\"", S->start_path);
    else snprintf(args, sizeof args, "--nocurses --nv_engine");
    // the launch grant opens the file's folder to the engine; a new document from Files gets the
    // folder it will be saved in (a name inside it: the grant covers its directory)
    char grant[256] = "";
    if (S->start_path[0]) snprintf(grant, sizeof grant, "%s", S->start_path);
    else if (S->new_dir[0]) snprintf(grant, sizeof grant, "%s/.new", S->new_dir);
    nv_wasm_exec_set_launch_file(grant[0] ? grant : nullptr);
    nv_wasm_exec_set_console(args);
    nv_wasm_exec_set_console_cwd("/sdcard/home");
    nv_wasm_exec_set_console_size(0, 0);
    char err[160] = "";
    const bool ok = nv_wasm_exec_start(app, err, sizeof err);
    heap_caps_free(app);
    if (!ok) {
        if (nv_wasm_exec_stopping()) return false;           // the previous run is unwinding: next tick
        S->want_start = false;
        show_loading(false);
        nv_toast(NV_NOTE_ERROR, err[0] ? err : tr(T_ENGINE_BUSY));
        return false;
    }
    S->want_start = false;
    S->run = true;
    S->ready = false;
    S->out_n = S->in_n = 0;
    S->sync_next = 0;                                        // the engine's first record is "\x1e0"
    memset(S->sync_kind, 0, sizeof S->sync_kind);
    S->view_inflight = 0;
    return true;
}

// Ask the engine to finish (EOF on its input): the next start waits until it has.
void engine_stop(void) {
    if (!S->run) return;
    nv_wasm_exec_end_stdin();
    S->stopping = true;
    S->stop_t = esp_timer_get_time();
}

// Open `path` ("" = a blank document, tpl >= 0 = that template) in a fresh engine.
void open_doc(const char *path, int tpl, bool recover = false, const char *recover_name = nullptr) {
    snprintf(S->start_path, sizeof S->start_path, "%s", path ? path : "");
    S->start_tpl = tpl;
    static const T kTplName[3] = {T_TPL_BUDGET, T_TPL_SHOP, T_TPL_HOURS};
    snprintf(S->untitled, sizeof S->untitled, "%s", tpl >= 0 && tpl < 3 ? tr(kTplName[tpl]) : tr(T_NEW_SHEET));
    S->start_recover = recover;
    snprintf(S->recover_name, sizeof S->recover_name, "%s", recover_name ? recover_name : "");
    S->path[0] = '\0';
    S->read_only_fmt = false;
    if (path && path[0]) S->new_dir[0] = '\0';
    if (path && path[0] && !recover) {
        snprintf(S->path, sizeof S->path, "%s", path);
        S->read_only_fmt = !writable_fmt(path);
    }
    S->modified = false;
    S->big_heap = false;
    S->edits = S->edits_backed = 0;
    S->ar = S->ac = S->cr = S->cc = 0;
    S->sx = S->sy = 0;
    S->vx = S->vy = 0;
    S->nsheets = 1;
    S->cursheet = 0;
    S->maxrow = S->maxcol = 0;
    snprintf(S->sheet[0], sizeof S->sheet[0], "%s 1", tr(T_SHEET));
    S->xtext[0] = '\0';
    S->status[0] = '\0';
    memset(S->colw, 10, sizeof S->colw);
    colx_rebuild();
    if (s_live) cache_reset(s_live, 0, 0);
    if (s_start) { lv_obj_delete(s_start); s_start = nullptr; }
    ai_reset_topic();
    show_loading(true);
    update_title();
    update_tabs();
    if (S->run) engine_stop();
    S->want_start = true;
}

void pump(void) {
    // outgoing: as much as the pipe takes
    if (S->run && S->out_n) {
        const size_t n = nv_wasm_exec_write_stdin(S->out, S->out_n);
        if (n) {
            memmove(S->out, S->out + n, S->out_n - n);
            S->out_n -= (int)n;
        }
    }
    // incoming: whole lines
    char buf[1024];
    for (int budget = 0; budget < 64; budget++) {
        const size_t n = nv_wasm_exec_read(buf, sizeof buf);
        if (!n) break;
        for (size_t i = 0; i < n; i++) {
            const char ch = buf[i];
            if (ch == '\n') {
                S->in[S->in_n] = '\0';
                on_line(S->in);
                S->in_n = 0;
                if (!S) return;
            } else if (S->in_n < kIn - 1) {
                S->in[S->in_n++] = ch;
            }
        }
    }
}

// ---------------------------------------------------------------- engine records
int split(char *l, char **f, int max) {
    int n = 0;
    f[n++] = l;
    for (char *p = l; *p && n < max; p++)
        if (*p == '\t') { *p = '\0'; f[n++] = p + 1; }
    return n;
}

void stage_cell(char **f, int n) {
    if (n < 7 || !s_stage) return;
    const int r = atoi(f[1]), c = atoi(f[2]);
    const int rr = r - s_stage->r0, cc = c - s_stage->c0;
    if (rr < 0 || cc < 0 || rr >= kCR || cc >= kCC || s_stage->n >= kCR * kCC) return;
    const int len = (int)strlen(f[6]);
    if (s_stage->pool_n + len + 1 > kPool) return;
    Cell &x = s_stage->cells[s_stage->n];
    x.text = s_stage->pool_n;
    x.len = (uint16_t)len;
    x.kind = f[3][0];
    x.align = f[4][0];
    x.style = (uint8_t)atoi(f[5]);
    memcpy(s_stage->pool + s_stage->pool_n, f[6], len + 1);
    s_stage->pool_n += len + 1;
    s_stage->idx[rr * kCC + cc] = s_stage->n++;
}

// "L <row> values..." from nv tsv: kept with the row number (ANIMA reads it; the clipboard drops it)
void tsv_line(char **f, int n) {
    for (int i = 1; i < n && S->tsv_n < kTsvMax; i++) {
        const int l = (int)strlen(f[i]);
        if (S->tsv_n + l + 2 > kTsvMax) { S->tsv_n = kTsvMax; return; }
        if (i > 1) S->tsv[S->tsv_n++] = '\t';
        memcpy(S->tsv + S->tsv_n, f[i], l);
        S->tsv_n += l;
    }
    if (S->tsv_n < kTsvMax) S->tsv[S->tsv_n++] = '\n';
}

void error_toast(const char *msg) {
    if (!strcmp(msg, "formula") || strstr(msg, "syntax error") || strstr(msg, "Illegal"))
        nv_toast(NV_NOTE_ERROR, tr(T_BAD_FORMULA));
    else if (!strcmp(msg, "locked") || strstr(msg, "Locked"))
        nv_toast(NV_NOTE_ERROR, tr(T_LOCKED));
    else if (!strcmp(msg, "sheet name"))
        nv_toast(NV_NOTE_ERROR, tr(T_EXISTS));
    else if (strstr(msg, "No UNDO") || strstr(msg, "No REDO") || !strcmp(msg, "unknown command"))
        ;                                                  // nothing to undo: no noise
    else
        nv_toast(NV_NOTE_ERROR, msg);
}

void on_sync(uint32_t id);
void tsv_line(char **f, int n);
void found(int r, int c);
void find_next(void);

void on_line(char *l) {
    if ((unsigned char)l[0] == 0x1e) { on_sync((uint32_t)strtoul(l + 1, nullptr, 10)); return; }
    char *f[12];
    const int n = split(l, f, 12);
    switch (f[0][0]) {
        case 'C': if (!f[0][1]) stage_cell(f, n); break;
        case 'D':
            if (n >= 7 && !f[0][1]) {
                if (f[1][0]) snprintf(S->path, sizeof S->path, "%s", f[1]);
                S->modified = atoi(f[2]) != 0;
                S->nsheets = clampi(atoi(f[3]), 1, 24);
                S->cursheet = clampi(atoi(f[4]), 0, S->nsheets - 1);
                S->maxrow = atoi(f[5]);
                S->maxcol = atoi(f[6]);
            }
            break;
        case 'S':
            if (n >= 3 && !f[0][1]) {
                const int i = atoi(f[1]);
                if (i >= 0 && i < 24) snprintf(S->sheet[i], sizeof S->sheet[i], "%s", f[2]);
            }
            break;
        case 'W':
            if (n >= 5 && !f[0][1]) {
                const int c = atoi(f[1]);
                if (c >= 0 && c <= kMaxCol) S->colw[c] = (uint8_t)clampi(atoi(f[2]), 1, 60);
            }
            break;
        case 'X':
            if (n >= 4 && !f[0][1]) {
                S->xr = atoi(f[1]);
                S->xc = atoi(f[2]);
                snprintf(S->xtext, sizeof S->xtext, "%s", f[3]);
                if (!S->editing && s_fx && S->xr == S->cr && S->xc == S->cc) {
                    s_syncing_text = true;
                    lv_textarea_set_text(s_fx, S->xtext);
                    s_syncing_text = false;
                }
                ai_sync_chips();
            }
            break;
        case 'T':
            if (n >= 7 && !f[0][1]) {
                const long cnt = atol(f[1]), nums = atol(f[2]);
                if (sel_single()) S->status[0] = '\0';
                else if (nums)
                    snprintf(S->status, sizeof S->status, "%s %s   %s %s   %s %ld", tr(T_SUM), f[3], tr(T_AVG), f[4],
                             tr(T_COUNT), cnt);
                else snprintf(S->status, sizeof S->status, "%s %ld", tr(T_COUNT), cnt);
                if (s_status) lv_label_set_text(s_status, S->status);
            }
            break;
        case 'L': if (!f[0][1]) tsv_line(f, n); break;
        case 'F': if (n >= 3 && !f[0][1]) found(atoi(f[1]), atoi(f[2])); break;
        case 'K': if (n >= 3 && !f[0][1]) ai_checked(atoi(f[1]), atoi(f[2]) != 0); break;
        case 'E': if (n >= 2 && !f[0][1]) error_toast(f[1]); break;
        case 'O':
            if (n >= 2 && !strcmp(f[0], "OK")) {
                if (!strcmp(f[1], "save")) {
                    S->modified = false;
                    S->read_only_fmt = false;
                    S->edits_backed = S->edits;
                    remove(kRecovery);
                    nv_config_set_str("sheet_rec", "");
                    nv_toast(NV_NOTE_OK, tr(T_SAVED));
                    recents_add(S->path);
                    note_for_anima();
                    update_title();
                } else if (!strcmp(f[1], "backup")) {
                    nv_config_set_str("sheet_rec", S->path[0] ? S->path : "-");
                }
            }
            break;
        default: break;
    }
}

void on_sync(uint32_t id) {
    const Sync k = (Sync)S->sync_kind[id % 64];
    const int32_t a = S->sync_a[id % 64], b = S->sync_b[id % 64];
    S->sync_kind[id % 64] = S_NONE;
    if (id == 0) { on_ready(); return; }
    switch (k) {
        case S_VIEW_BEGIN:
            cache_reset(s_stage, a, b);
            break;
        case S_VIEW_END: {
            Cache *t = s_live;
            s_live = s_stage;
            s_stage = t;
            if (S->view_inflight == id) S->view_inflight = 0;
            colx_rebuild();
            update_title();
            update_tabs();
            grid_invalidate();
            if (S->anima_after_view) {
                S->anima_after_view = false;
                ai_open_and_summarize();
            }
            break;
        }
        case S_TSV_CLIP: {
            // the rows without their numbers, as every spreadsheet puts cells on the clipboard
            S->tsv[S->tsv_n] = '\0';
            int k = 0;
            for (char *p = S->tsv; *p;) {
                char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
                if (!nl) nl = p + strlen(p);
                char *from = tab && tab < nl ? tab + 1 : nl;
                const int l = (int)(nl - from);
                memmove(S->tsv + k, from, l);
                k += l;
                if (*nl) S->tsv[k++] = '\n';
                p = *nl ? nl + 1 : nl;
            }
            S->tsv[k] = '\0';
            if (k) {
                nv_clip_set_text(S->tsv, "sheets");
                S->own_clip_seq = nv_clip_seq();
            }
            break;
        }
        case S_TSV_AI:
            ai_send();
            break;
        case S_CHECK:
            ai_check_done();
            break;
        case S_FIND:
            if (!S->find_n) nv_toast(NV_NOTE_INFO, tr(T_NOT_FOUND));
            else { S->find_i = -1; find_next(); }
            break;
        default: break;
    }
}

// ---------------------------------------------------------------- requests
// The cells around what is on screen (a region of kCR x kCC), when the view left the cached one.
void request_view(bool force) {
    if (!S->ready || !s_grid) return;
    const int32_t w = lv_obj_get_width(s_grid) - kRowHdrW, h = lv_obj_get_height(s_grid) - kHdrH;
    const int rv0 = S->sy / kRowH, rv1 = (S->sy + h) / kRowH + 1;
    const int cv0 = col_at(S->sx), cv1 = col_at(S->sx + w) + 1;
    if (!force && !S->view_dirty && s_live && rv0 >= s_live->r0 && rv1 < s_live->r0 + kCR &&
        cv0 >= s_live->c0 && cv1 < s_live->c0 + kCC)
        return;
    const int r0 = clampi(rv0 - (kCR - (rv1 - rv0)) / 3, 0, kMaxRow);
    const int c0 = clampi(cv0 - 4, 0, kMaxCol);
    if (!force && S->view_inflight && S->view_r0 == r0 && S->view_c0 == c0) return;
    S->view_dirty = false;
    sync(S_VIEW_BEGIN, r0, c0);
    send("nv view %d %d %d %d", r0, c0, r0 + kCR - 1, c0 + kCC - 1);
    S->view_r0 = r0;
    S->view_c0 = c0;
    S->view_inflight = sync(S_VIEW_END);
}

void ask_cell(void) {
    if (!S->ready) return;
    send("nv cell %d %d", S->cr, S->cc);
}
void ask_stats(void) {
    if (!S->ready) return;
    if (sel_single()) {
        S->status[0] = '\0';
        if (s_status) lv_label_set_text(s_status, "");
        return;
    }
    send("nv stats %d %d %d %d", sel_r0(), sel_c0(), sel_r1(), sel_c1());
}

// After a change: the document's facts, the cells on screen, the formula bar, the totals.
void refresh(void) {
    if (!S->ready) return;
    send("nv info");
    send("nv cols 0 %d", clampi(S->maxcol + 40, 40, kMaxCol));
    S->view_dirty = true;
    request_view(true);
    ask_cell();
    ask_stats();
}

// A command that changes the document
void change(const char *fmt, ...) {
    char b[1300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    send("%s", b);
    S->edits++;
    S->modified = true;
}

void apply_template(int tpl) {
    const char *t = kTpl[tpl][lang_ix()];
    int r = 0, c = 0;
    char cell[256];
    size_t k = 0;
    for (const char *p = t;; p++) {
        if (*p == '|' || *p == '\n' || !*p) {
            cell[k] = '\0';
            if (k) send("nv set %d %d %s", r, c, cell);
            k = 0;
            if (*p == '|') c++;
            else { r++; c = 0; }
            if (!*p) break;
        } else if (k < sizeof cell - 1) {
            cell[k++] = *p;
        }
    }
    send("nv bold 0 0 0 3 1");
    send("nv autofit 0 4");
    send("nv fmt 1 1 %d 3 #,##0.00", r);
    S->modified = false;   // a fresh template is not "changed" until the user touches it
}

// The system assistant's "current file": a later "in questo file..." in the ANIMA app means this one
void note_for_anima(void) {
    if (!S->path[0]) return;
    nucleo_anima_note_file(!strncmp(S->path, "/sdcard/", 8) ? S->path + 7 : S->path);
}

void on_ready(void) {
    S->ready = true;
    note_for_anima();
    show_loading(false);
    send("nv lang %s", lang_code());
    const bool fresh = !S->start_path[0];
    if (fresh) send("nv rensheet %s 1", tr(T_SHEET));
    if (S->start_tpl >= 0) { apply_template(S->start_tpl); S->start_tpl = -1; }
    if (fresh) send("nv saved");
    if (S->start_recover) {
        S->start_recover = false;
        if (S->recover_name[0]) send("nv name %s", S->recover_name);
        S->edits++;
    }
    if (S->read_only_fmt) nv_toast(NV_NOTE_INFO, tr(T_READ_ONLY_FMT));
    refresh();
    if (S->anima_on_ready) {
        S->anima_on_ready = false;
        S->anima_after_view = true;
    }
}

void engine_dead(void) {
    S->run = false;
    S->ready = false;
    show_loading(false);
    nv_toast(NV_NOTE_ERROR, tr(T_ENGINE_FAIL));
}

// ---------------------------------------------------------------- recents
void recents_get(char out[8][256], int *n) {
    char *buf = S->rbuf;
    nv_config_get_str("sheet_recent", "", buf, sizeof S->rbuf);
    *n = 0;
    char *save = nullptr;
    for (char *t = strtok_r(buf, "\n", &save); t && *n < 8; t = strtok_r(nullptr, "\n", &save))
        snprintf(out[(*n)++], 256, "%s", t);
}
}  // namespace

void recents_add(const char *path) {
    if (!path || !path[0] || !S) return;
    char (*list)[256] = S->rl;
    char *buf = S->rbuf;
    const size_t cap = sizeof S->rbuf;
    int n = 0;
    recents_get(list, &n);                                   // (done with rbuf before it is reused)
    char first[256];
    snprintf(first, sizeof first, "%s", path);
    size_t k = (size_t)snprintf(buf, cap, "%s", first);
    for (int i = 0; i < n && k < cap - 260; i++) {
        if (!strcmp(list[i], first)) continue;
        struct stat st;
        if (stat(list[i], &st) != 0) continue;            // gone from the card
        k += (size_t)snprintf(buf + k, cap - k, "\n%s", list[i]);
        if (i >= 6) break;
    }
    nv_config_set_str("sheet_recent", buf);
}

namespace {

// ---------------------------------------------------------------- title, tabs, name box
void update_title(void) {
    char t[160];
    if (!S->run && !S->want_start && !S->path[0]) { nv_ui_set_title(nv_tr(NV_STR_APP_SHEETS)); return; }
    const char *name = S->path[0] ? base_name(S->path) : S->untitled[0] ? S->untitled : tr(T_NEW_SHEET);
    snprintf(t, sizeof t, "%s%s", S->modified ? "\xE2\x80\xA2 " : "", name);
    nv_ui_set_title(t);
}

void update_namebox(void) {
    if (!s_name) return;
    char a[16], b[16], t[40];
    cell_name(sel_r0(), sel_c0(), a, sizeof a);
    if (sel_single()) snprintf(t, sizeof t, "%s", a);
    else {
        cell_name(sel_r1(), sel_c1(), b, sizeof b);
        snprintf(t, sizeof t, "%s:%s", a, b);
    }
    lv_label_set_text(s_name, t);
}

void tab_cb(lv_event_t *e);
void tab_ctx_cb(lv_event_t *e);
void tab_add_cb(lv_event_t *e);

void update_tabs(void) {
    if (!s_tabrow) return;
    lv_obj_clean(s_tabrow);
    const NvTheme *th = nv_theme_get();
    for (int i = 0; i < S->nsheets; i++) {
        lv_obj_t *b = lv_obj_create(s_tabrow);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, LV_SIZE_CONTENT, 30);
        lv_obj_set_style_pad_hor(b, 14, 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, i == S->cursheet ? th->primary : th->surface2, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, tab_ctx_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, tab_ctx_cb, (lv_event_code_t)nv_ui_event_context(), (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, S->sheet[i]);
        lv_obj_set_style_text_font(l, &nv_font_14, 0);
        lv_obj_set_style_text_color(l, i == S->cursheet ? th->on_primary : th->text, 0);
        lv_obj_center(l);
    }
    lv_obj_t *add = lv_obj_create(s_tabrow);
    lv_obj_remove_style_all(add);
    lv_obj_set_size(add, 36, 30);
    lv_obj_set_style_radius(add, 8, 0);
    lv_obj_set_style_bg_opa(add, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(add, th->surface2, 0);
    lv_obj_add_flag(add, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(add, tab_add_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(add);
    lv_label_set_text(l, I_PLUS);
    lv_obj_set_style_text_font(l, &s_icons, 0);
    lv_obj_set_style_text_color(l, th->text, 0);
    lv_obj_center(l);
}

// ---------------------------------------------------------------- geometry and drawing
int32_t grid_w(void) { return s_grid ? lv_obj_get_width(s_grid) - kRowHdrW : 0; }
int32_t grid_h(void) { return s_grid ? lv_obj_get_height(s_grid) - kHdrH : 0; }

void clamp_scroll(void) {
    const int32_t maxy = (int32_t)(LV_MAX(S->maxrow + 200, 400)) * kRowH;
    const int32_t maxx = S->colx[clampi(S->maxcol + 30, 30, kMaxCol)];
    S->sx = clampi(S->sx, 0, LV_MAX(0, maxx - grid_w() / 2));
    S->sy = clampi(S->sy, 0, LV_MAX(0, maxy));
}

// Scroll so that cell (r, c) is on screen
void reveal(int r, int c) {
    if (!s_grid) return;
    const int32_t w = grid_w(), h = grid_h();
    const int32_t y = r * kRowH, x = S->colx[c], cw = col_w(c);
    if (y < S->sy) S->sy = y;
    else if (y + kRowH > S->sy + h) S->sy = y + kRowH - h;
    if (x < S->sx) S->sx = x;
    else if (x + cw > S->sx + w) S->sx = LV_MIN(x, x + cw - w);
    clamp_scroll();
    request_view(false);
    grid_invalidate();
}

bool area_and(lv_area_t *res, const lv_area_t *a, const lv_area_t *b);

void fill(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c, lv_opa_t opa = LV_OPA_COVER) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = opa;
    lv_area_t a = {x1, y1, x2, y2};
    lv_draw_rect(layer, &d, &a);
}

void frame(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, lv_color_t c, int32_t w) {
    fill(layer, x1, y1, x2, y1 + w - 1, c);
    fill(layer, x1, y2 - w + 1, x2, y2, c);
    fill(layer, x1, y1, x1 + w - 1, y2, c);
    fill(layer, x2 - w + 1, y1, x2, y2, c);
}

void dot(lv_layer_t *layer, int32_t cx, int32_t cy, lv_color_t c, lv_color_t ring) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = LV_OPA_COVER;
    d.radius = LV_RADIUS_CIRCLE;
    d.border_color = ring;
    d.border_width = 2;
    d.border_opa = LV_OPA_COVER;
    lv_area_t a = {cx - kHandleR + 3, cy - kHandleR + 3, cx + kHandleR - 3, cy + kHandleR - 3};
    lv_draw_rect(layer, &d, &a);
}

void label(lv_layer_t *layer, const lv_area_t &area, const char *t, const lv_font_t *font, lv_color_t col,
           lv_text_align_t align, bool underline = false) {
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = font;
    d.color = col;
    d.text = t;
    d.text_local = 1;
    d.align = align;
    d.decor = underline ? LV_TEXT_DECOR_UNDERLINE : LV_TEXT_DECOR_NONE;
    lv_draw_label(layer, &d, &area);
}

// Text inside [x1, x2] of a row, clipped there (labels do not clip by themselves)
void cell_text(lv_layer_t *layer, int32_t x1, int32_t x2, int32_t y, const char *t, lv_color_t col,
               lv_text_align_t align, uint8_t style) {
    const lv_area_t saved = layer->_clip_area;
    lv_area_t clip = {x1, y, x2, y + kRowH - 1};
    if (!area_and(&layer->_clip_area, &saved, &clip)) { layer->_clip_area = saved; return; }
    const int32_t ty = y + (kRowH - lv_font_get_line_height(&s_font)) / 2;
    lv_area_t a = {x1 + kPadX, ty, x2 - kPadX, ty + lv_font_get_line_height(&s_font)};
    if (align == LV_TEXT_ALIGN_LEFT) a.x2 = x1 + 4000;     // overflow to the right stays possible
    label(layer, a, t, &s_font, col, align, style & 4);
    if (style & 1) { a.x1 += 1; a.x2 += 1; label(layer, a, t, &s_font, col, align, false); }   // bold
    layer->_clip_area = saved;
}

lv_color_t mix(lv_color_t a, lv_color_t b, uint8_t ratio) { return lv_color_mix(a, b, ratio); }

bool area_and(lv_area_t *res, const lv_area_t *a, const lv_area_t *b) {
    res->x1 = LV_MAX(a->x1, b->x1);
    res->y1 = LV_MAX(a->y1, b->y1);
    res->x2 = LV_MIN(a->x2, b->x2);
    res->y2 = LV_MIN(a->y2, b->y2);
    return res->x1 <= res->x2 && res->y1 <= res->y2;
}

int32_t text_w(const char *t) {
    lv_point_t sz;
    lv_text_get_size(&sz, t, &s_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}

void grid_draw(lv_event_t *e) {
    if (!S || !s_grid) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(s_grid, &a);
    const NvTheme *th = nv_theme_get();
    const lv_color_t bg = th->bg, line = mix(th->text_dim, th->bg, 40), hdr = th->surface;
    const lv_color_t acc = th->primary;
    const int32_t x0 = a.x1 + kRowHdrW, y0 = a.y1 + kHdrH;
    fill(layer, x0, y0, a.x2, a.y2, bg);

    const int rfirst = S->sy / kRowH;
    const int32_t yoff = y0 - S->sy % kRowH;
    const int cfirst = col_at(S->sx);
    const int32_t xoff = x0 + S->colx[cfirst] - S->sx;
    int rlast = rfirst, clast = cfirst;
    for (int32_t y = yoff; y <= a.y2 && rlast <= kMaxRow; y += kRowH) rlast++;
    for (int32_t x = xoff; x <= a.x2 && clast <= kMaxCol; x += col_w(clast)) clast++;
    const int r0 = sel_r0(), r1 = sel_r1(), c0 = sel_c0(), c1 = sel_c1();

    // selection tint (under the text)
    {
        const int32_t sx1 = x0 + S->colx[c0] - S->sx, sx2 = x0 + S->colx[clampi(c1 + 1, 0, kMaxCol + 1)] - S->sx - 1;
        const int32_t sy1 = y0 + r0 * kRowH - S->sy, sy2 = y0 + (r1 + 1) * kRowH - S->sy - 1;
        if (!sel_single()) fill(layer, LV_MAX(sx1, x0), LV_MAX(sy1, y0), sx2, sy2, acc, LV_OPA_20);
    }
    // grid lines
    for (int r = rfirst; r < rlast; r++) {
        const int32_t y = yoff + (r - rfirst) * kRowH;
        fill(layer, x0, y + kRowH - 1, a.x2, y + kRowH - 1, line);
    }
    for (int c = cfirst; c < clast; c++) {
        const int32_t x = x0 + S->colx[c + 1] - S->sx - 1;
        if (x >= x0) fill(layer, x, y0, x, a.y2, line);
    }
    // cells
    for (int r = rfirst; r < rlast; r++) {
        const int32_t y = yoff + (r - rfirst) * kRowH;
        for (int c = cfirst; c < clast; c++) {
            const char *t = nullptr;
            const Cell *cl = cache_get(s_live, r, c, &t);
            if (!cl || !cl->len) continue;
            if (S->editing && !S->edit_bar && r == S->er && c == S->ec) continue;
            const int32_t cx1 = x0 + S->colx[c] - S->sx, cx2 = x0 + S->colx[c + 1] - S->sx - 2;
            lv_color_t col = th->text;
            if (cl->kind == 'e') col = th->danger;
            const bool text = cl->kind == 's' || cl->kind == 'g';
            lv_text_align_t al = cl->align == 'l' ? LV_TEXT_ALIGN_LEFT : cl->align == 'c' ? LV_TEXT_ALIGN_CENTER
                                                                                          : LV_TEXT_ALIGN_RIGHT;
            if (!text) al = LV_TEXT_ALIGN_RIGHT;
            int32_t xr = cx2;
            const int32_t tw = text_w(t);
            if (text && al == LV_TEXT_ALIGN_LEFT) {
                // Excel: text runs on over the empty cells to its right
                int k = c + 1;
                while (tw + 2 * kPadX > xr - cx1 && k <= kMaxCol && !cache_has(s_live, r, k)) {
                    xr = x0 + S->colx[k + 1] - S->sx - 2;
                    k++;
                    if (xr > a.x2) break;
                }
                if (tw + 2 * kPadX > xr - cx1 + 1) fill(layer, cx1 + 1, y, xr, y + kRowH - 2, bg);
                else if (k > c + 1) fill(layer, cx2 + 1, y, xr, y + kRowH - 2, bg);
            }
            if (!text && tw + 2 * kPadX > cx2 - cx1) {                 // a number that does not fit
                cell_text(layer, LV_MAX(cx1, x0), cx2, y, "###", col, LV_TEXT_ALIGN_RIGHT, 0);
                continue;
            }
            cell_text(layer, LV_MAX(cx1, x0), LV_MIN(xr, a.x2), y, t, col, al, cl->style);
        }
    }
    ai_draw_preview(layer, a, x0, y0);
    // selection frame, active cell, handles
    {
        const int32_t sx1 = x0 + S->colx[c0] - S->sx, sx2 = x0 + S->colx[clampi(c1 + 1, 0, kMaxCol + 1)] - S->sx - 1;
        const int32_t sy1 = y0 + r0 * kRowH - S->sy, sy2 = y0 + (r1 + 1) * kRowH - S->sy - 1;
        const lv_area_t saved = layer->_clip_area;
        lv_area_t clip = {x0, y0, a.x2, a.y2};
        if (area_and(&layer->_clip_area, &saved, &clip)) {
            frame(layer, sx1 - 1, sy1 - 1, sx2 + 1, sy2 + 1, acc, 2);
            if (!sel_single()) {
                const int32_t ax1 = x0 + S->colx[S->cc] - S->sx, ax2 = x0 + S->colx[S->cc + 1] - S->sx - 1;
                const int32_t ay1 = y0 + S->cr * kRowH - S->sy, ay2 = ay1 + kRowH - 1;
                fill(layer, ax1 + 1, ay1 + 1, ax2 - 1, ay2 - 1, bg);
                const char *t = nullptr;
                const Cell *cl = cache_get(s_live, S->cr, S->cc, &t);
                if (cl && cl->len)
                    cell_text(layer, ax1, ax2, ay1, t, cl->kind == 'e' ? th->danger : th->text,
                              cl->kind == 's' || cl->kind == 'g' ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, cl->style);
            }
            if (!S->editing) {
                dot(layer, sx1, sy1, bg, acc);
                dot(layer, sx2, sy2, bg, acc);
            }
        }
        layer->_clip_area = saved;
    }
    // headers on top
    fill(layer, a.x1, a.y1, a.x2, y0 - 1, hdr);
    fill(layer, a.x1, y0, x0 - 1, a.y2, hdr);
    char b[12];
    for (int c = cfirst; c < clast; c++) {
        const int32_t x1 = x0 + S->colx[c] - S->sx, x2 = x0 + S->colx[c + 1] - S->sx - 1;
        if (x2 < x0) continue;
        const bool on = c >= c0 && c <= c1;
        if (on) fill(layer, LV_MAX(x1, x0), a.y1, x2, y0 - 1, mix(acc, hdr, 90));
        fill(layer, x2, a.y1 + 4, x2, y0 - 5, line);
        col_name(c, b);
        const lv_area_t saved = layer->_clip_area;
        lv_area_t clip = {x0, a.y1, a.x2, y0 - 1};
        if (area_and(&layer->_clip_area, &saved, &clip)) {
            lv_area_t la = {x1, a.y1 + (kHdrH - 16) / 2, x2, a.y1 + kHdrH};
            label(layer, la, b, &nv_font_14, on ? th->text_strong : th->text_dim, LV_TEXT_ALIGN_CENTER);
        }
        layer->_clip_area = saved;
    }
    for (int r = rfirst; r < rlast; r++) {
        const int32_t y = yoff + (r - rfirst) * kRowH;
        const bool on = r >= r0 && r <= r1;
        if (on) fill(layer, a.x1, LV_MAX(y, y0), x0 - 1, y + kRowH - 1, mix(acc, hdr, 90));
        fill(layer, a.x1 + 4, y + kRowH - 1, x0 - 5, y + kRowH - 1, line);
        snprintf(b, sizeof b, "%d", r + 1);
        const lv_area_t saved = layer->_clip_area;
        lv_area_t clip = {a.x1, y0, x0 - 1, a.y2};
        if (area_and(&layer->_clip_area, &saved, &clip)) {
            lv_area_t la = {a.x1, y + (kRowH - 16) / 2, x0 - 6, y + kRowH};
            label(layer, la, b, &nv_font_14, on ? th->text_strong : th->text_dim, LV_TEXT_ALIGN_RIGHT);
        }
        layer->_clip_area = saved;
    }
    fill(layer, a.x1, a.y1, x0 - 1, y0 - 1, mix(th->text_dim, hdr, 60));
    fill(layer, a.x1, y0 - 1, a.x2, y0 - 1, line);
    fill(layer, x0 - 1, a.y1, x0 - 1, a.y2, line);
}

// ---------------------------------------------------------------- hit testing
enum Hit { H_NONE, H_CORNER, H_COLHDR, H_COLEDGE, H_ROWHDR, H_CELL, H_HANDLE_TL, H_HANDLE_BR };

Hit hit(int32_t px, int32_t py, int *r, int *c) {
    lv_area_t a;
    lv_obj_get_coords(s_grid, &a);
    const int32_t x = px - a.x1, y = py - a.y1;
    *r = clampi((S->sy + y - kHdrH) / kRowH, 0, kMaxRow);
    *c = clampi(col_at(S->sx + x - kRowHdrW), 0, kMaxCol);
    if (x < kRowHdrW && y < kHdrH) return H_CORNER;
    if (y < kHdrH) {
        const int32_t cx = S->sx + x - kRowHdrW;
        const int cc = col_at(cx);
        const int32_t right = S->colx[cc + 1];
        if (right - cx <= 8) { *c = cc; return H_COLEDGE; }
        if (cx - S->colx[cc] <= 6 && cc > 0) { *c = cc - 1; return H_COLEDGE; }
        return H_COLHDR;
    }
    if (x < kRowHdrW) return H_ROWHDR;
    if (!S->editing) {
        const int32_t sx1 = kRowHdrW + S->colx[sel_c0()] - S->sx, sy1 = kHdrH + sel_r0() * kRowH - S->sy;
        const int32_t sx2 = kRowHdrW + S->colx[sel_c1() + 1] - S->sx - 1, sy2 = kHdrH + (sel_r1() + 1) * kRowH - S->sy - 1;
        const int32_t tol = kHandleR + 8;
        if (LV_ABS(x - sx2) <= tol && LV_ABS(y - sy2) <= tol) return H_HANDLE_BR;
        if (LV_ABS(x - sx1) <= tol && LV_ABS(y - sy1) <= tol) return H_HANDLE_TL;
    }
    return H_CELL;
}

// ---------------------------------------------------------------- selection
void selection_changed(bool cell_moved) {
    update_namebox();
    if (cell_moved) ask_cell();
    ask_stats();
    grid_invalidate();
}

void select_cell(int r, int c, bool extend) {
    r = clampi(r, 0, kMaxRow);
    c = clampi(c, 0, kMaxCol);
    const bool moved = r != S->cr || c != S->cc || !extend;
    if (!extend) { S->ar = r; S->ac = c; }
    S->cr = r;
    S->cc = c;
    reveal(r, c);
    selection_changed(moved);
}

// ---------------------------------------------------------------- editing
void edit_commit(int dr, int dc);
void edit_cancel(void);
void point_insert(int r0, int c0, int r1, int c1, bool replace);

bool point_mode(void) {
    if (!S->editing) return false;
    lv_obj_t *ta = S->edit_bar ? s_fx : s_editor;
    if (!ta) return false;
    const char *t = lv_textarea_get_text(ta);
    if (t[0] != '=') return false;
    const uint32_t pos = lv_textarea_get_cursor_pos(ta);
    if (pos == 0) return false;
    // the character before the caret (ASCII is enough: the operators are)
    uint32_t i = 0, ch = 0;
    for (const char *p = t; *p && i < pos; p++) if (((unsigned char)*p & 0xC0) != 0x80) { ch = (unsigned char)*p; i++; }
    if (S->pt_len && (int)pos == S->pt_pos + S->pt_len) return true;   // replacing the last pointed ref
    return strchr("=+-*/^(;,:<>&", (int)ch) != nullptr;
}

// The in-cell editor grows with its text, over the cells to its right (Excel does the same)
void editor_fit(void) {
    if (!s_editor || !s_grid) return;
    const int32_t tw = text_w(lv_textarea_get_text(s_editor));
    const int32_t w = LV_MAX(col_w(S->ec) + 2, tw + 2 * kPadX + 12);
    lv_obj_set_width(s_editor, LV_MIN(w, lv_obj_get_width(s_grid) - lv_obj_get_x(s_editor) - 2));
}

void editor_sync(lv_event_t *e) {
    if (s_syncing_text || s_pointing || !S->editing) return;
    lv_obj_t *src = lv_event_get_target_obj(e);
    lv_obj_t *dst = src == s_editor ? s_fx : s_editor;
    if (dst) {
        s_syncing_text = true;
        lv_textarea_set_text(dst, lv_textarea_get_text(src));
        s_syncing_text = false;
    }
    editor_fit();
    if (src == s_editor || src == s_fx) {
        const uint32_t pos = lv_textarea_get_cursor_pos(src);
        if (S->pt_len && (int)pos != S->pt_pos + S->pt_len) S->pt_len = 0;
    }
}

void editor_ready(lv_event_t *) { edit_commit(1, 0); }

bool editor_key(lv_obj_t *, int key, char ctrl) {
    if (ctrl) return false;
    switch (key) {
        case NV_IME_RK_TAB: edit_commit(0, 1); return true;
        case NV_IME_RK_ESC: edit_cancel(); return true;
        case NV_IME_RK_UP: if (S->edit_typed) { edit_commit(-1, 0); return true; } return false;
        case NV_IME_RK_DOWN: if (S->edit_typed) { edit_commit(1, 0); return true; } return false;
        default: return false;
    }
}

void style_editor(lv_obj_t *ta) {
    const NvTheme *th = nv_theme_get();
    lv_obj_set_style_text_font(ta, &s_font, 0);
    lv_obj_set_style_bg_color(ta, th->surface, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, th->primary, 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_radius(ta, 0, 0);
    lv_obj_set_style_pad_hor(ta, kPadX - 2, 0);
    lv_obj_set_style_pad_ver(ta, (kRowH - 4 - lv_font_get_line_height(&s_font)) / 2, 0);
    lv_obj_set_style_text_color(ta, th->text_strong, 0);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
}

void field_font(lv_obj_t *ta) {
    lv_obj_set_style_text_font(ta, &s_font, 0);
    lv_obj_set_style_text_font(ta, &s_font, LV_PART_TEXTAREA_PLACEHOLDER);
}

void focus_ta(lv_obj_t *ta) {
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_obj_send_event(ta, LV_EVENT_FOCUSED, nullptr);
}

// Start editing the active cell: `typed` = the first character typed over it (it replaces the
// content, as in Excel); in_bar = edit in the formula bar instead of the cell.
void edit_start(const char *typed, bool in_bar) {
    if (!S->ready || !s_grid) return;
    if (S->editing) return;
    S->ar = S->cr;
    S->ac = S->cc;
    S->er = S->cr;
    S->ec = S->cc;
    reveal(S->er, S->ec);
    S->editing = true;
    S->edit_bar = in_bar;
    S->edit_typed = typed != nullptr;
    S->pt_len = 0;
    const char *cur = S->xr == S->er && S->xc == S->ec ? S->xtext : "";
    const char *start = typed ? typed : cur;
    snprintf(S->edit_orig, sizeof S->edit_orig, "%s", start);

    s_editor = nv_kit_textarea_ex(s_grid, nullptr, true, NV_IME_TEXT, NV_IME_RET_ENTER);
    style_editor(s_editor);
    lv_obj_add_flag(s_editor, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(s_editor, kRowHdrW + S->colx[S->ec] - S->sx - 1, kHdrH + S->er * kRowH - S->sy - 1);
    lv_obj_set_size(s_editor, col_w(S->ec) + 2, kRowH + 1);
    lv_textarea_set_text(s_editor, start);
    if (typed && !typed[0]) {                         // replace mode: the old value as a hint
        const char *old = nullptr;
        if (cache_get(s_live, S->er, S->ec, &old) && old) lv_textarea_set_placeholder_text(s_editor, old);
    }
    lv_obj_add_event_cb(s_editor, editor_sync, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_editor, editor_ready, LV_EVENT_READY, nullptr);
    nv_ime_set_key_hook(s_editor, editor_key);
    if (s_fx) {
        s_syncing_text = true;
        lv_textarea_set_text(s_fx, start);
        s_syncing_text = false;
    }
    lv_obj_t *ta = in_bar && s_fx ? s_fx : s_editor;
    lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST);
    focus_ta(ta);
    grid_invalidate();
}

void edit_close(void) {
    S->editing = false;
    S->pt_len = 0;
    if (s_editor) { lv_obj_delete(s_editor); s_editor = nullptr; }
    if (s_fx) lv_obj_remove_state(s_fx, LV_STATE_FOCUSED);
    grid_invalidate();
}

void edit_commit(int dr, int dc) {
    if (!S->editing) return;
    lv_obj_t *ta = S->edit_bar && s_fx ? s_fx : s_editor;
    const char *t = ta ? lv_textarea_get_text(ta) : "";
    if (strcmp(t, S->edit_orig)) change("nv set %d %d %s", S->er, S->ec, t);
    const int r = S->er, c = S->ec;
    const bool in_cell = !S->edit_bar;
    edit_close();
    S->ar = S->cr = clampi(r + dr, 0, kMaxRow);
    S->ac = S->cc = clampi(c + dc, 0, kMaxCol);
    refresh();
    reveal(S->cr, S->cc);
    update_namebox();
    update_title();
    // Enter / Tab in the cell: straight on with the next one, keyboard up (typing a column of values)
    if ((dr || dc) && in_cell) edit_start("", false);
    else nv_ime_hide();
}

void edit_cancel(void) {
    if (!S->editing) return;
    edit_close();
    if (s_fx) {
        s_syncing_text = true;
        lv_textarea_set_text(s_fx, S->xtext);
        s_syncing_text = false;
    }
    nv_ime_hide();
}

// Put the reference of the pointed cells into the formula being written (Excel's point mode)
void point_insert(int r0, int c0, int r1, int c1, bool replace) {
    lv_obj_t *ta = S->edit_bar && s_fx ? s_fx : s_editor;
    if (!ta) return;
    char ref[24], a[12], b[12];
    if (r0 > r1) { int t = r0; r0 = r1; r1 = t; }
    if (c0 > c1) { int t = c0; c0 = c1; c1 = t; }
    cell_name(r0, c0, a, sizeof a);
    if (r0 == r1 && c0 == c1) snprintf(ref, sizeof ref, "%s", a);
    else { cell_name(r1, c1, b, sizeof b); snprintf(ref, sizeof ref, "%s:%s", a, b); }
    s_pointing = true;                       // editor_sync must not drop pt_len halfway
    if (replace && S->pt_len) {
        const int n = S->pt_len;
        lv_textarea_set_cursor_pos(ta, S->pt_pos + n);
        for (int i = 0; i < n; i++) lv_textarea_delete_char(ta);
    }
    S->pt_pos = (int)lv_textarea_get_cursor_pos(ta);
    lv_textarea_add_text(ta, ref);
    S->pt_len = (int)strlen(ref);
    s_pointing = false;
    editor_fit();
    // the other field mirrors the result once
    lv_obj_t *dst = ta == s_editor ? s_fx : s_editor;
    if (dst) {
        s_syncing_text = true;
        lv_textarea_set_text(dst, lv_textarea_get_text(ta));
        s_syncing_text = false;
    }
    grid_invalidate();
}

// ---------------------------------------------------------------- commands
void cmd_undo(void) { change("nv undo"); refresh(); }
void cmd_redo(void) { change("nv redo"); refresh(); }

void cmd_copy_to_clip(void) {
    S->tsv_n = 0;
    S->tsv_c0 = sel_c0();
    send("nv tsv %d %d %d %d", sel_r0(), sel_c0(), LV_MIN(used_r1(), sel_r0() + 2000), used_c1());
    sync(S_TSV_CLIP);
}
void cmd_copy(void) {
    send("nv copy %d %d %d %d", sel_r0(), sel_c0(), used_r1(), used_c1());
    cmd_copy_to_clip();
    nv_toast(NV_NOTE_OK, tr(T_COPIED));
}
void cmd_cut(void) {
    cmd_copy_to_clip();
    change("nv cut %d %d %d %d", sel_r0(), sel_c0(), used_r1(), used_c1());
    refresh();
}
void paste_text(const char *t) {
    int r = sel_r0(), c = sel_c0(), n = 0;
    char cell[512];
    size_t k = 0;
    for (const char *p = t;; p++) {
        if (*p == '\t' || *p == '\n' || !*p) {
            cell[k] = '\0';
            while (k && (cell[k - 1] == '\r' || cell[k - 1] == ' ')) cell[--k] = '\0';
            if (k) { change("nv set %d %d %s", r, c, cell); n++; }
            k = 0;
            if (*p == '\t') c++;
            else { r++; c = sel_c0(); }
            if (!*p || n > 3000) break;
        } else if (k < sizeof cell - 1) {
            cell[k++] = *p;
        }
    }
}
void cmd_paste(bool values) {
    if (nv_clip_seq() != S->own_clip_seq && nv_clip_has_text()) {   // copied in another app
        char *t = nv_clip_get_text();
        if (t) { paste_text(t); free(t); }
    } else {
        change("nv paste %d %d%s", sel_r0(), sel_c0(), values ? " values" : "");
    }
    refresh();
}
void cmd_clear(void) { change("nv clear %d %d %d %d", sel_r0(), sel_c0(), used_r1(), used_c1()); refresh(); }

void cmd_style(const char *what, uint8_t bit) {
    const Cell *cl = cache_get(s_live, S->cr, S->cc, nullptr);
    const int on = cl && (cl->style & bit) ? 0 : 1;
    change("nv %s %d %d %d %d %d", what, sel_r0(), sel_c0(), used_r1(), used_c1(), on);
    refresh();
}

void cmd_fmt(const char *f) { change("nv fmt %d %d %d %d %s", sel_r0(), sel_c0(), sel_r1(), sel_c1(), f); refresh(); }

// The rows to sort: the selection, or the table around the active cell without its header row
void cmd_sort(bool desc) {
    int r0 = sel_r0(), r1 = used_r1(), c0 = sel_c0(), c1 = used_c1();
    const bool whole = r0 == r1;
    if (whole) {                       // the table around: the engine keeps its header row in place
        r0 = 0;
        r1 = S->maxrow;
        c0 = 0;
        c1 = S->maxcol;
    }
    if (r1 <= r0) return;
    change("nv sort %d %d %d %d %d %s%s", r0, c0, r1, c1, S->cc, desc ? "desc" : "asc", whole ? " auto" : "");
    refresh();
}

void cmd_rows(bool insert) {
    const int n = sel_r1() - sel_r0() + 1;
    if (insert) change("nv insrow %d %d", sel_r0(), LV_MIN(n, 500));
    else change("nv delrow %d %d", sel_r0(), LV_MIN(n, 5000));
    refresh();
}
void cmd_cols(bool insert) {
    const int n = sel_c1() - sel_c0() + 1;
    if (insert) change("nv inscol %d %d", sel_c0(), LV_MIN(n, 100));
    else change("nv delcol %d %d", sel_c0(), LV_MIN(n, 300));
    refresh();
}
void cmd_fit(void) { change("nv autofit %d %d", sel_c0(), sel_c1()); refresh(); }

// Σ: one cell sums the numbers above it (or to its left); a range gets its column totals below
void cmd_autosum(void) {
    static const char *const kSum[5] = {"SUM", "SOMMA", "SUMA", "SOMME", "SUMME"};
    const char *fn = kSum[lang_ix()];
    char a[12], b[12], t[64];
    if (sel_single()) {
        const int r = S->cr, c = S->cc;
        int top = r;
        while (top > 0) {
            const Cell *x = cache_get(s_live, top - 1, c, nullptr);
            if (!x || x->kind == 's' || x->kind == 'g') break;
            top--;
        }
        if (top < r) {
            cell_name(top, c, a, sizeof a);
            cell_name(r - 1, c, b, sizeof b);
        } else {
            int left = c;
            while (left > 0) {
                const Cell *x = cache_get(s_live, r, left - 1, nullptr);
                if (!x || x->kind == 's' || x->kind == 'g') break;
                left--;
            }
            if (left == c) { snprintf(t, sizeof t, "=%s()", fn); edit_start(t, false); return; }
            cell_name(r, left, a, sizeof a);
            cell_name(r, c - 1, b, sizeof b);
        }
        snprintf(t, sizeof t, "=%s(%s:%s)", fn, a, b);
        edit_start(t, false);
        return;
    }
    const int r0 = sel_r0(), r1 = sel_r1();
    for (int c = sel_c0(); c <= sel_c1(); c++) {
        cell_name(r0, c, a, sizeof a);
        cell_name(r1, c, b, sizeof b);
        change("nv set %d %d =SUM(%s:%s)", r1 + 1, c, a, b);
    }
    refresh();
}

// ---------------------------------------------------------------- dialogs
using Fn = void (*)(void);
struct DlgBtn { const char *text; Fn fn; bool primary; };
Fn s_dlg_fn[3];

void modal_close_async(void *) {
    nv_ime_set_submit_cb(nullptr, nullptr);
    if (s_modal) { lv_obj_delete(s_modal); s_modal = nullptr; s_modal_ta = nullptr; }
    nv_ui_set_back_handler(back_handler);
}
void modal_close(void) { lv_async_call(modal_close_async, nullptr); }

void dlg_btn_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    Fn fn = i >= 0 && i < 3 ? s_dlg_fn[i] : nullptr;
    modal_close();
    if (fn) fn();
}

lv_obj_t *modal_card(const char *title, int32_t w, int32_t top = 60) {
    const NvTheme *th = nv_theme_get();
    if (s_modal) { lv_obj_delete(s_modal); s_modal = nullptr; s_modal_ta = nullptr; }
    nv_ui_set_back_handler(modal_close);
    s_modal = lv_obj_create(lv_screen_active());      // not lv_layer_top: the keyboard must stay above
    lv_obj_remove_style_all(s_modal);
    lv_obj_set_size(s_modal, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_50, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *card = lv_obj_create(s_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_set_style_bg_color(card, th->surface, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, th->surface3, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_style_pad_row(card, 14, 0);
    lv_obj_set_style_text_font(card, &s_font, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &nv_font_20, 0);
    lv_obj_set_style_text_color(t, th->text_strong, 0);
    return card;
}

void modal_buttons(lv_obj_t *card, const DlgBtn *b, int n) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < n && i < 3; i++) {
        s_dlg_fn[i] = b[i].fn;
        lv_obj_t *bt = lv_button_create(row);
        lv_obj_set_flex_grow(bt, 1);
        lv_obj_set_height(bt, 46);
        lv_obj_set_style_bg_color(bt, b[i].primary ? th->primary : th->surface3, 0);
        lv_obj_add_event_cb(bt, dlg_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(bt);
        lv_label_set_text(l, b[i].text);
        lv_obj_set_style_text_color(l, b[i].primary ? th->on_primary : th->text_strong, 0);
        lv_obj_center(l);
    }
}

void dialog(const char *title, const char *msg, const DlgBtn *b, int n) {
    lv_obj_t *card = modal_card(title, 520);
    if (msg && msg[0]) {
        lv_obj_t *m = lv_label_create(card);
        lv_label_set_text(m, msg);
        lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(m, lv_pct(100));
        lv_obj_set_style_text_color(m, nv_theme_get()->text, 0);
    }
    modal_buttons(card, b, n);
}

// Ask for a name: `done` receives the text
using TextFn = void (*)(const char *);
TextFn s_text_done;
void text_ok(void) {
    if (!s_modal_ta) return;
    static NV_PSRAM_BSS char t[256];
    snprintf(t, sizeof t, "%s", lv_textarea_get_text(s_modal_ta));
    TextFn fn = s_text_done;
    modal_close();
    if (fn) fn(t);
}
void text_submit(lv_obj_t *, void *) { text_ok(); }
void text_dialog(const char *title, const char *initial, TextFn done) {
    lv_obj_t *card = modal_card(title, 520);
    s_text_done = done;
    s_modal_ta = nv_kit_textarea_ex(card, nullptr, true, NV_IME_TEXT, NV_IME_RET_GO);
    lv_obj_set_width(s_modal_ta, lv_pct(100));
    lv_textarea_set_text(s_modal_ta, initial);
    nv_ime_set_submit_cb(text_submit, nullptr);
    const DlgBtn b[] = {{tr(T_CANCEL), nullptr, false}, {"OK", text_ok, true}};
    modal_buttons(card, b, 2);
    focus_ta(s_modal_ta);
}

// ---------------------------------------------------------------- unsaved changes
Fn s_after_save;               // what to do once the document is saved / discarded
void save_then(Fn next);

void ask_unsaved(Fn next) {
    if (!S->modified || !S->ready) { next(); return; }
    s_after_save = next;
    static Fn s_next;
    s_next = next;
    struct L {
        static void save(void) { save_then(s_next); }
        static void discard(void) {
            remove(kRecovery);
            nv_config_set_str("sheet_rec", "");
            S->modified = false;
            s_next();
        }
    };
    const DlgBtn b[] = {{tr(T_CANCEL), nullptr, false}, {tr(T_DONT_SAVE), L::discard, false}, {tr(T_SAVE), L::save, true}};
    dialog(tr(T_UNSAVED_Q), tr(T_UNSAVED_MSG), b, 3);
}

// ---------------------------------------------------------------- save / save as
struct SaveAs { char name[128]; int fmt; int folder; };   // fmt: 0 xlsx 1 csv 2 tsv; folder: 0 Documents 1 same
SaveAs s_sa;
lv_obj_t *s_sa_fmt[3], *s_sa_dir[2];

void make_dirs(const char *dir) {
    char p[256];
    snprintf(p, sizeof p, "%s", dir);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = '\0'; mkdir(p, 0777); *s = '/'; }
    mkdir(p, 0777);
}

void do_save(const char *path) {
    snprintf(S->path, sizeof S->path, "%s", path);
    send("nv save %s", path);
    send("nv info");
}

void sa_paint(void) {
    const NvTheme *th = nv_theme_get();
    for (int i = 0; i < 3; i++)
        if (s_sa_fmt[i]) lv_obj_set_style_bg_color(s_sa_fmt[i], i == s_sa.fmt ? th->primary : th->surface3, 0);
    for (int i = 0; i < 2; i++)
        if (s_sa_dir[i]) lv_obj_set_style_bg_color(s_sa_dir[i], i == s_sa.folder ? th->primary : th->surface3, 0);
}
void sa_fmt_cb(lv_event_t *e) { s_sa.fmt = (int)(intptr_t)lv_event_get_user_data(e); sa_paint(); }
void sa_dir_cb(lv_event_t *e) { s_sa.folder = (int)(intptr_t)lv_event_get_user_data(e); sa_paint(); }

void sa_target(char *out, size_t n) {
    static const char *const kExt[3] = {"xlsx", "csv", "tsv"};
    char name[160];
    snprintf(name, sizeof name, "%s", s_modal_ta ? lv_textarea_get_text(s_modal_ta) : s_sa.name);
    char *dot = strrchr(name, '.');
    if (dot && (sheet_file(name))) *dot = '\0';
    char dir[256];
    if (s_sa.folder == 1 && S->path[0]) {
        snprintf(dir, sizeof dir, "%s", S->path);
        char *sl = strrchr(dir, '/');
        if (sl) *sl = '\0';
    } else if (s_sa.folder == 1 && S->new_dir[0]) {
        snprintf(dir, sizeof dir, "%s", S->new_dir);
    } else {
        snprintf(dir, sizeof dir, "%s", kDocsDir);
    }
    snprintf(out, n, "%s/%s.%s", dir, name, kExt[s_sa.fmt]);
}

void sa_ok(void) {
    if (!s_modal_ta) return;
    const char *nm = lv_textarea_get_text(s_modal_ta);
    if (!nm[0] || strchr(nm, '/') || strchr(nm, '"')) { nv_toast(NV_NOTE_ERROR, tr(T_BAD_NAME)); return; }
    char *target = S->tmp_path2;
    sa_target(target, sizeof S->tmp_path2);
    modal_close();
    char dir[256];
    snprintf(dir, sizeof dir, "%s", target);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = '\0'; make_dirs(dir); }
    struct stat st;
    if (stat(target, &st) == 0 && strcmp(target, S->path)) {
        struct L { static void yes(void) { do_save(S->tmp_path2); if (s_after_save) { Fn f = s_after_save; s_after_save = nullptr; f(); } } };
        const DlgBtn b[] = {{tr(T_CANCEL), nullptr, false}, {tr(T_SAVE), L::yes, true}};
        dialog(tr(T_OVERWRITE_Q), base_name(target), b, 2);
        return;
    }
    do_save(target);
    if (s_after_save) { Fn f = s_after_save; s_after_save = nullptr; f(); }
}
void sa_submit(lv_obj_t *, void *) { sa_ok(); }

lv_obj_t *chip(lv_obj_t *parent, const char *text, lv_event_cb_t cb, int ud) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 40);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

void save_as(void) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *card = modal_card(tr(T_SAVE_AS), 640, 12);   // above the keyboard, buttons included
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    if (S->path[0]) {
        snprintf(s_sa.name, sizeof s_sa.name, "%s", base_name(S->path));
        char *dot = strrchr(s_sa.name, '.');
        if (dot) *dot = '\0';
    } else {
        snprintf(s_sa.name, sizeof s_sa.name, "%s", S->untitled[0] ? S->untitled : tr(T_SHEET));
    }
    s_sa.fmt = S->path[0] && has_ext(S->path, "csv") ? 1 : S->path[0] && has_ext(S->path, "tsv") ? 2 : 0;
    const bool same_ok = (S->path[0] && strncmp(S->path, kDocsDir, strlen(kDocsDir)) && !S->read_only_fmt) ||
                         (!S->path[0] && S->new_dir[0] && strcmp(S->new_dir, kDocsDir));
    s_sa.folder = same_ok ? 1 : 0;
    s_modal_ta = nv_kit_textarea_ex(card, tr(T_FILE_NAME), true, NV_IME_TEXT, NV_IME_RET_GO);
    lv_obj_set_width(s_modal_ta, lv_pct(100));
    lv_textarea_set_text(s_modal_ta, s_sa.name);
    nv_ime_set_submit_cb(sa_submit, nullptr);
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);
    s_sa_fmt[0] = chip(row, "Excel .xlsx", sa_fmt_cb, 0);
    s_sa_fmt[1] = chip(row, "CSV .csv", sa_fmt_cb, 1);
    s_sa_fmt[2] = chip(row, "TSV .tsv", sa_fmt_cb, 2);
    lv_obj_t *row2 = lv_obj_create(card);
    lv_obj_remove_style_all(row2);
    lv_obj_set_size(row2, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row2, 8, 0);
    lv_obj_t *in = lv_label_create(row2);
    lv_label_set_text(in, tr(T_SAVE_IN));
    lv_obj_set_style_text_color(in, th->text_dim, 0);
    lv_obj_set_style_align(in, LV_ALIGN_LEFT_MID, 0);
    lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // folders as the card shows them in Files (the path under /sdcard)
    static char home_docs[64], folder[160];
    snprintf(home_docs, sizeof home_docs, "%s", kDocsDir + 8);          // "home/Documenti"
    s_sa_dir[0] = chip(row2, home_docs, sa_dir_cb, 0);
    s_sa_dir[1] = nullptr;
    if (same_ok) {
        char d[256];
        snprintf(d, sizeof d, "%s", S->path[0] ? S->path : S->new_dir);
        char *sl = S->path[0] ? strrchr(d, '/') : nullptr;
        if (sl) *sl = '\0';
        snprintf(folder, sizeof folder, "%s", !strncmp(d, "/sdcard/", 8) ? d + 8 : d);
        s_sa_dir[1] = chip(row2, folder, sa_dir_cb, 1);
    }
    sa_paint();
    const DlgBtn b[] = {{tr(T_CANCEL), nullptr, false}, {tr(T_SAVE), sa_ok, true}};
    modal_buttons(card, b, 2);
    focus_ta(s_modal_ta);
}

void save_then(Fn next) {
    s_after_save = next;
    if (S->path[0] && writable_fmt(S->path) && !S->read_only_fmt) {
        do_save(S->path);
        s_after_save = nullptr;
        if (next) next();
    } else {
        save_as();
    }
}
void cmd_save(void) { save_then(nullptr); }

// ---------------------------------------------------------------- open: file list
struct FileEnt { char path[200]; int64_t mtime; int32_t size; };
FileEnt *s_files;
int s_nfiles;

void scan_dir(const char *dir, int depth) {
    static const char *const kSkip[] = {"apps", "system", "web", "data", "nvupd", ".thumbs", "DCIM", "Music",
                                        "Video", "Photos", "Screenshots", "Recordings", "System Volume Information",
                                        "NUCLEOS", "APPS", "nucleo", "tmp", ".fogli", ".config", ".cache"};
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && s_nfiles < kMaxFiles) {
        if (de->d_name[0] == '.' && strcmp(de->d_name, ".fogli")) continue;
        char p[256];
        if (snprintf(p, sizeof p, "%s/%s", dir, de->d_name) >= (int)sizeof p) continue;
        if (de->d_type == DT_DIR) {
            bool skip = false;
            for (const char *k : kSkip) if (!strcasecmp(de->d_name, k)) skip = true;
            if (!skip && depth > 0) scan_dir(p, depth - 1);
            continue;
        }
        if (!sheet_file(de->d_name) || strlen(p) >= sizeof s_files[0].path) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        FileEnt &f = s_files[s_nfiles++];
        snprintf(f.path, sizeof f.path, "%s", p);
        f.mtime = st.st_mtime;
        f.size = (int32_t)st.st_size;
    }
    closedir(d);
}

void open_path_now(const char *p) {
    snprintf(S->tmp_path, sizeof S->tmp_path, "%s", p);
    struct L {
        static void go(void) {
            char path[300];
            snprintf(path, sizeof path, "%s", S->tmp_path);
            recents_add(path);
            open_doc(path, -1);
        }
    };
    ask_unsaved(L::go);
}

void file_pick_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_nfiles) return;
    char p[200];
    snprintf(p, sizeof p, "%s", s_files[i].path);
    modal_close();
    open_path_now(p);
}

void when_text(int64_t t, char *b, size_t n) {
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(b, n, lang_ix() == 0 ? "%m/%d/%Y %H:%M" : "%d/%m/%Y %H:%M", &tm);
}

lv_obj_t *list_row(lv_obj_t *list, const char *icon, const char *title, const char *sub, lv_event_cb_t cb, int ud) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 58);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_bg_color(row, th->surface2, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, th->surface3, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, (void *)(intptr_t)ud);
    lv_obj_t *ic = lv_label_create(row);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, &s_icons, 0);
    lv_obj_set_style_text_color(ic, th->primary, 0);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, title);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(t, lv_pct(88));
    lv_obj_set_style_text_color(t, th->text_strong, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 34, 9);
    lv_obj_t *s = lv_label_create(row);
    lv_label_set_text(s, sub);
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s, lv_pct(88));
    lv_obj_set_style_text_font(s, &nv_font_14, 0);
    lv_obj_set_style_text_color(s, th->text_dim, 0);
    lv_obj_align(s, LV_ALIGN_TOP_LEFT, 34, 32);
    return row;
}

void open_dialog(void) {
    if (!s_files) s_files = (FileEnt *)heap_caps_malloc(sizeof(FileEnt) * kMaxFiles, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_files) return;
    s_nfiles = 0;
    scan_dir("/sdcard/home", 4);
    scan_dir("/sdcard", 2);
    // /sdcard/home was seen twice (through /sdcard): keep the first
    for (int i = 0; i < s_nfiles; i++)
        for (int j = i + 1; j < s_nfiles; j++)
            if (!strcmp(s_files[i].path, s_files[j].path)) { s_files[j] = s_files[--s_nfiles]; j--; }
    std::qsort(s_files, s_nfiles, sizeof(FileEnt), [](const void *a, const void *b) {
        const int64_t x = ((const FileEnt *)a)->mtime, y = ((const FileEnt *)b)->mtime;
        return x < y ? 1 : x > y ? -1 : 0;
    });
    lv_obj_t *card = modal_card(tr(T_OPEN), 760);
    lv_obj_t *list = lv_obj_create(card);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), 360);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    if (!s_nfiles) {
        lv_obj_t *l = lv_label_create(list);
        lv_label_set_text(l, tr(T_NO_FILES));
        lv_obj_set_style_text_color(l, nv_theme_get()->text_dim, 0);
    }
    for (int i = 0; i < s_nfiles && i < 120; i++) {
        char sub[200], when[40], dir[160];
        snprintf(dir, sizeof dir, "%s", s_files[i].path + 7);             // after "/sdcard"
        char *sl = strrchr(dir, '/');
        if (sl) *sl = '\0';
        when_text(s_files[i].mtime, when, sizeof when);
        snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s  \xC2\xB7  %ld KB", dir[0] ? dir : "/", when,
                 (long)((s_files[i].size + 1023) / 1024));
        list_row(list, I_XLS, base_name(s_files[i].path), sub, file_pick_cb, i);
    }
    const DlgBtn b[] = {{tr(T_CANCEL), nullptr, false}};
    modal_buttons(card, b, 1);
}
void cmd_open(void) { open_dialog(); }
void cmd_new(void) {
    struct L { static void go(void) { open_doc("", -1); } };
    ask_unsaved(L::go);
}

// ---------------------------------------------------------------- start page
void start_pick_cb(lv_event_t *e);

void start_page(void) {
    if (s_start || !s_root) return;
    const NvTheme *th = nv_theme_get();
    s_start = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_start);
    lv_obj_add_flag(s_start, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_start, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_start, th->bg, 0);
    lv_obj_set_style_bg_opa(s_start, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_start, 24, 0);
    lv_obj_set_style_pad_column(s_start, 24, 0);
    lv_obj_set_flex_flow(s_start, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(s_start, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left = lv_obj_create(s_start);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, lv_pct(48), lv_pct(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 8, 0);
    lv_obj_t *h = lv_label_create(left);
    lv_label_set_text(h, tr(T_TEMPLATES));
    lv_obj_set_style_text_font(h, &nv_font_20, 0);
    lv_obj_set_style_text_color(h, th->text_strong, 0);
    // recovered work first: it is the one thing that cannot wait
    char *rec = S->tmp_path;
    nv_config_get_str("sheet_rec", "", rec, sizeof S->tmp_path);
    struct stat st;
    if (rec[0] && stat(kRecovery, &st) == 0) {
        char *sub = S->tmp_path2;
        snprintf(sub, sizeof S->tmp_path2, "%s  \xC2\xB7  %s", strcmp(rec, "-") ? base_name(rec) : tr(T_BLANK), tr(T_RECOVER_SUB));
        list_row(left, I_HISTORY, tr(T_RECOVER), sub, start_pick_cb, 100);
    }
    list_row(left, I_FILE, tr(T_BLANK), tr(T_BLANK_SUB), start_pick_cb, 0);
    list_row(left, I_SIGMA, tr(T_TPL_BUDGET), tr(T_TPL_BUDGET_SUB), start_pick_cb, 1);
    list_row(left, I_CHECK, tr(T_TPL_SHOP), tr(T_TPL_SHOP_SUB), start_pick_cb, 2);
    list_row(left, I_HISTORY, tr(T_TPL_HOURS), tr(T_TPL_HOURS_SUB), start_pick_cb, 3);
    list_row(left, I_FOLDER, tr(T_BROWSE), tr(T_BROWSE_SUB), start_pick_cb, 4);

    lv_obj_t *right = lv_obj_create(s_start);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, lv_pct(48), lv_pct(100));
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 8, 0);
    h = lv_label_create(right);
    lv_label_set_text(h, tr(T_RECENT));
    lv_obj_set_style_text_font(h, &nv_font_20, 0);
    lv_obj_set_style_text_color(h, th->text_strong, 0);
    char (*rl)[256] = S->rl;
    int n = 0;
    recents_get(rl, &n);
    int shown = 0;
    for (int i = 0; i < n; i++) {
        if (stat(rl[i], &st) != 0) continue;
        char sub[200], when[40], dir[160];
        snprintf(dir, sizeof dir, "%s", rl[i] + (strncmp(rl[i], "/sdcard", 7) ? 0 : 7));
        char *sl = strrchr(dir, '/');
        if (sl) *sl = '\0';
        when_text(st.st_mtime, when, sizeof when);
        snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", dir[0] ? dir : "/", when);
        list_row(right, I_XLS, base_name(rl[i]), sub, start_pick_cb, 10 + i);
        shown++;
    }
    if (!shown) {
        lv_obj_t *l = lv_label_create(right);
        lv_label_set_text(l, "\xE2\x80\x94");
        lv_obj_set_style_text_color(l, th->text_dim, 0);
    }
}

void start_pick_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == 0) open_doc("", -1);
    else if (i >= 1 && i <= 3) open_doc("", i - 1);
    else if (i == 4) open_dialog();
    else if (i == 100) {
        char rec[256];
        nv_config_get_str("sheet_rec", "", rec, sizeof rec);
        open_doc(kRecovery, -1, true, strcmp(rec, "-") ? rec : "");
    } else if (i >= 10 && i < 18) {
        int n = 0;
        recents_get(S->rl, &n);
        if (i - 10 < n) {
            char p[256];
            snprintf(p, sizeof p, "%s", S->rl[i - 10]);
            open_path_now(p);
        }
    }
}

// ---------------------------------------------------------------- sheets (tabs)
void tab_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == S->cursheet || !S->ready) return;
    if (S->editing) edit_commit(0, 0);
    send("nv sheet %d", i);
    S->cursheet = i;
    S->ar = S->ac = S->cr = S->cc = 0;
    S->sx = S->sy = 0;
    refresh();
    update_tabs();
    update_namebox();
}
void tab_add_cb(lv_event_t *) {
    if (!S->ready) return;
    char name[48];
    for (int k = S->nsheets + 1; k < 99; k++) {
        snprintf(name, sizeof name, "%s %d", tr(T_SHEET), k);
        bool used = false;
        for (int i = 0; i < S->nsheets; i++) if (!strcmp(S->sheet[i], name)) used = true;
        if (!used) break;
    }
    change("nv newsheet %s", name);
    S->ar = S->ac = S->cr = S->cc = 0;
    S->sx = S->sy = 0;
    refresh();
}
int s_tab_menu;
void tab_rename_done(const char *t) {
    if (!t[0] || strchr(t, '"')) { nv_toast(NV_NOTE_ERROR, tr(T_BAD_NAME)); return; }
    if (s_tab_menu != S->cursheet) send("nv sheet %d", s_tab_menu);
    change("nv rensheet %s", t);
    refresh();
}
void tab_rename(void *) { text_dialog(tr(T_RENAME_SHEET), S->sheet[s_tab_menu], tab_rename_done); }
void tab_delete(void *) {
    struct L {
        static void yes(void) {
            if (s_tab_menu != S->cursheet) send("nv sheet %d", s_tab_menu);
            change("nv delsheet");
            S->ar = S->ac = S->cr = S->cc = 0;
            refresh();
        }
    };
    const DlgBtn b[] = {{tr(T_CANCEL), nullptr, false}, {tr(T_DELETE), L::yes, true}};
    dialog(tr(T_DEL_SHEET), tr(T_DEL_SHEET_Q), b, 2);
}
void tab_ctx_cb(lv_event_t *e) {
    s_tab_menu = (int)(intptr_t)lv_event_get_user_data(e);
    lv_point_t p = {400, 500};
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        if (lv_indev_t *in = lv_indev_active()) lv_indev_get_point(in, &p);
    } else if (const lv_point_t *pp = (const lv_point_t *)lv_event_get_param(e)) {
        p = *pp;
    }
    const nv_menu_item_t m[] = {
        {LV_SYMBOL_EDIT, tr(T_RENAME_SHEET), nullptr, tab_rename, nullptr, false, false},
        {LV_SYMBOL_TRASH, tr(T_DEL_SHEET), nullptr, S->nsheets > 1 ? tab_delete : nullptr, nullptr, false, false},
    };
    nv_ui_menu_open(p.x, p.y, m, 2);
}

// ---------------------------------------------------------------- find
void find_next(void) {
    if (!S->find_n) return;
    S->find_i = (S->find_i + 1) % S->find_n;
    S->ar = S->cr = S->find_r[S->find_i];
    S->ac = S->cc = S->find_c[S->find_i];
    reveal(S->cr, S->cc);
    selection_changed(true);
}
void found(int r, int c) {
    if (S->find_n < 200) { S->find_r[S->find_n] = r; S->find_c[S->find_n] = c; S->find_n++; }
}
void find_submit(lv_obj_t *, void *) {
    if (!s_find_ta) return;
    const char *t = lv_textarea_get_text(s_find_ta);
    if (!t[0]) return;
    if (strcmp(t, S->find_text) || !S->find_n) {
        snprintf(S->find_text, sizeof S->find_text, "%s", t);
        S->find_n = 0;
        send("nv find %s", t);
        sync(S_FIND);
    } else {
        find_next();
    }
}
void find_close(void) {
    if (!s_findbar) return;
    nv_ime_set_submit_cb(nullptr, nullptr);
    lv_obj_add_flag(s_findbar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_fbar, LV_OBJ_FLAG_HIDDEN);
    nv_ime_hide();
}
void find_close_cb(lv_event_t *) { find_close(); }
void cmd_find(void) {
    if (!s_findbar) return;
    lv_obj_add_flag(s_fbar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_findbar, LV_OBJ_FLAG_HIDDEN);
    nv_ime_set_submit_cb(find_submit, nullptr);
    focus_ta(s_find_ta);
}

// ---------------------------------------------------------------- ANIMA
// ANIMA reads the selected cells (or the whole table when one cell is selected), with the formulas,
// and answers in the UI language. When the request asks for a change it also proposes actions:
//   SET <cell> <content>     a value or an Excel formula
//   FORMAT <range> <kind>    number | thousands | currency | percent | general
//   BOLD <range>
// Each proposed formula is checked by the engine before it is offered; the proposals are drawn on the
// grid (tinted cells with the new value) until the user applies or dismisses them. A follow-up
// question carries the previous exchange. Without a reachable model the device still answers what it
// can compute itself (a summary per column).
//
// One worker for the app's lifetime (like the ANIMA app's): the request travels in PSRAM buffers,
// the answer comes back the same way, matched by generation so a closed panel ignores it.
struct Ai {
    TaskHandle_t task;
    std::atomic<uint32_t> want, done;
    int rc;                                 // > 0 reply length, -1 failed, -2 no model, -3 busy
    char sys[3200];
    char user[kTsvMax + 2400];
    char out[kAiOut];
    char note[160];
};
enum ActKind : uint8_t { A_SET, A_FORMAT, A_BOLD };
struct Act { uint8_t kind; bool ok; int r0, c0, r1, c1; char text[200]; };
struct AiUi {
    Act acts[40];
    int nacts;
    bool checking;
    char text[kAiOut];          // what the panel shows
    char hist_q[400];           // the previous exchange, for a follow-up question
    char hist_a[900];
    char scope[96];             // "Legge A1:E9 · 45 celle"
    int dots;                   // "thinking" animation
    bool pending;
    int64_t t_start;            // when the pending request started
};
Ai *s_aw;                                   // never freed (the worker may still be using it)
AiUi *s_au;                                 // the panel's state (heap, freed with the document)
uint32_t s_ai_gen;
lv_obj_t *s_ai_scope;

void ai_task(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint32_t gen = s_aw->want.load();
        const int64_t t0 = esp_timer_get_time();
        NV_LOGI(TAG, "ANIMA request %u: start", (unsigned)gen);
        bool locked = false;
        for (int i = 0; i < 60 && !(locked = nucleo_anima_try_lock()); i++) vTaskDelay(pdMS_TO_TICKS(100));
        int rc;
        s_aw->note[0] = '\0';
        if (!locked) rc = -3;
        else {
            nucleo_anima_cancel_reset();
            nucleo_anima_online_turn_begin();
            const bool usable = nucleo_anima_model_usable();
            NV_LOGI(TAG, "ANIMA request %u: model %s (%d ms)", (unsigned)gen, usable ? "usable" : "not usable",
                    (int)((esp_timer_get_time() - t0) / 1000));
            if (!usable) rc = -2;
            else {
                NV_LOGI(TAG, "ANIMA request %u: asking the model (%u + %u bytes)", (unsigned)gen,
                         (unsigned)strlen(s_aw->sys), (unsigned)strlen(s_aw->user));
                rc = nucleo_anima_teacher_complete(s_aw->sys, s_aw->user, s_aw->out, kAiOut);
                if (rc < 0)
                    snprintf(s_aw->note, sizeof s_aw->note, "%s",
                             nucleo_anima_online_fail_note(nv_i18n_get_lang() == NV_LANG_EN));
            }
            nucleo_anima_unlock();
        }
        s_aw->rc = rc;
        NV_LOGI(TAG, "ANIMA request %u: %s in %d ms", (unsigned)gen,
                 rc > 0 ? "answered" : rc == -2 ? "no model" : rc == -3 ? "busy" : "failed",
                 (int)((esp_timer_get_time() - t0) / 1000));
        s_aw->done.store(gen);
    }
}

void ai_set_text(const char *t) {
    if (s_ai_out) lv_label_set_text(s_ai_out, t);
}

// A1 / A1:C9 -> rows and columns from 0; false when it is not a reference
bool parse_ref(const char *s, int *r0, int *c0, int *r1, int *c1) {
    auto one = [](const char *&p, int *r, int *c) {
        int col = 0, n = 0;
        if (*p == '$') p++;
        while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) { col = col * 26 + ((*p & ~0x20) - 'A' + 1); p++; n++; }
        if (*p == '$') p++;
        if (!n || n > 3 || *p < '1' || *p > '9') return false;
        const int row = atoi(p);
        while (*p >= '0' && *p <= '9') p++;
        *r = row - 1;
        *c = col - 1;
        return *r >= 0 && *r <= kMaxRow && *c >= 0 && *c <= kMaxCol;
    };
    const char *p = s;
    if (!one(p, r0, c0)) return false;
    *r1 = *r0;
    *c1 = *c0;
    if (*p == ':') { p++; if (!one(p, r1, c1)) return false; }
    if (*r0 > *r1) { const int t = *r0; *r0 = *r1; *r1 = t; }
    if (*c0 > *c1) { const int t = *c0; *c0 = *c1; *c1 = t; }
    return *p == '\0' || *p == ' ';
}

const char *fmt_code(const char *kind) {
    if (!strncasecmp(kind, "currency", 8)) return "#,##0.00 \xE2\x82\xAC";
    if (!strncasecmp(kind, "thousands", 9)) return "#,##0.00";
    if (!strncasecmp(kind, "percent", 7)) return "0%";
    if (!strncasecmp(kind, "number", 6)) return "0.00";
    if (!strncasecmp(kind, "general", 7)) return "";
    return nullptr;
}

// Without a model: what the device can say on its own, column by column
void ai_local_summary(char *out, size_t n, T why = T_ANIMA_OFFLINE) {
    size_t k = (size_t)snprintf(out, n, "%s\n\n%s:\n", tr(why), tr(T_ANIMA_LOCAL));
    double sum[40] = {0}, mn[40] = {0}, mx[40] = {0};
    int cnt[40] = {0};
    char head[40][32] = {};
    int ncols = 0, row = 0;
    const char *p = S->tsv;
    while (*p && k + 200 < n) {
        const char *e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        int col = -1;                                   // field 0 is the row number
        const char *q = p;
        while (q < e) {
            const char *t = q;
            while (t < e && *t != '\t') t++;
            if (col >= 0 && col < 40) {
                char v[64];
                const size_t l = LV_MIN((size_t)(t - q), sizeof v - 1);
                memcpy(v, q, l);
                v[l] = '\0';
                char *br = strstr(v, " [");             // a formula: a total of other cells, not data
                if (br) v[0] = '\0';
                char *end;
                const double d = strtod(v, &end);
                if (v[0] && !*end) {
                    if (!cnt[col] || d < mn[col]) mn[col] = d;
                    if (!cnt[col] || d > mx[col]) mx[col] = d;
                    sum[col] += d;
                    cnt[col]++;
                } else if (row == 0 && v[0]) {
                    snprintf(head[col], sizeof head[col], "%s", v);
                }
                if (col + 1 > ncols) ncols = col + 1;
            }
            col++;
            q = t < e ? t + 1 : e;
        }
        row++;
        p = *e ? e + 1 : e;
    }
    int shown = 0;
    for (int c = 0; c < ncols && k + 200 < n; c++) {
        if (!cnt[c]) continue;
        char cn[4];
        col_name(S->tsv_c0 + c, cn);
        char a[40], b[40], m1[40], m2[40];
        snprintf(a, sizeof a, "%.10g", sum[c]);
        snprintf(b, sizeof b, "%.6g", sum[c] / cnt[c]);
        snprintf(m1, sizeof m1, "%.10g", mn[c]);
        snprintf(m2, sizeof m2, "%.10g", mx[c]);
        if (lang_ix()) for (char *x : {a, b, m1, m2}) for (char *y = x; *y; y++) if (*y == '.') *y = ',';
        // U+2212 and no-break spaces: a label never wraps between a word and its number
        char *xs[4] = {a, b, m1, m2};
        for (char *x : xs) {
            if (x[0] == '-') { memmove(x + 3, x + 1, strlen(x)); memcpy(x, "\xE2\x88\x92", 3); }
        }
        k += (size_t)snprintf(out + k, n - k,
                              "\xE2\x80\xA2 %s%s%s\n   %s\xC2\xA0%s \xC2\xB7 %s\xC2\xA0%s \xC2\xB7 %s\xC2\xA0%d \xC2\xB7 %s\xC2\xA0%s \xC2\xB7 %s\xC2\xA0%s\n",
                              cn, head[c][0] ? "  " : "", head[c], tr(T_SUM), a, tr(T_AVG), b, tr(T_COUNT), cnt[c],
                              tr(T_MIN), m1, tr(T_MAX), m2);
        shown++;
    }
    if (!shown && k + 4 < n) snprintf(out + k, n - k, "\xE2\x80\x94\n");
}

void ai_apply_label(void) {
    if (!s_ai_apply || !s_au) return;
    lv_obj_set_style_bg_color(s_ai_apply, nv_theme_get()->primary, 0);
    int ok = 0, bad = 0;
    for (int i = 0; i < s_au->nacts; i++) (s_au->acts[i].ok ? ok : bad)++;
    if (!ok || s_au->checking) { lv_obj_add_flag(s_ai_apply, LV_OBJ_FLAG_HIDDEN); return; }
    char b[120], x[60] = "";
    if (bad) { snprintf(x, sizeof x, tr(T_ANIMA_INVALID), bad); }
    snprintf(b, sizeof b, tr(T_ANIMA_APPLY), ok);
    if (bad) { const size_t l = strlen(b); snprintf(b + l, sizeof b - l, "  \xC2\xB7  %s", x); }
    lv_label_set_text(lv_obj_get_child(s_ai_apply, 0), b);
    lv_obj_clear_flag(s_ai_apply, LV_OBJ_FLAG_HIDDEN);
}

// The proposals as a list under the explanation: ✓ usable, ✗ left out
void ai_render(const char *expl) {
    if (!s_au) return;
    char *t = s_au->text;
    const size_t cap = sizeof s_au->text;
    size_t k = (size_t)snprintf(t, cap, "%s", expl);
    if (s_au->nacts && k + 80 < cap) k += (size_t)snprintf(t + k, cap - k, "\n\n%s\n", tr(T_ANIMA_PREVIEW));
    for (int i = 0; i < s_au->nacts && k + 260 < cap; i++) {
        const Act &a = s_au->acts[i];
        char r0[12], r1[12];
        cell_name(a.r0, a.c0, r0, sizeof r0);
        cell_name(a.r1, a.c1, r1, sizeof r1);
        const char *mark = s_au->checking ? "\xE2\x80\xA6" : a.ok ? "\xE2\x9C\x93" : "\xE2\x9C\x97";
        if (a.kind == A_SET) k += (size_t)snprintf(t + k, cap - k, "%s %s  %s\n", mark, r0, a.text);
        else if (a.r0 == a.r1 && a.c0 == a.c1)
            k += (size_t)snprintf(t + k, cap - k, "%s %s  %s\n", mark, r0, a.kind == A_BOLD ? tr(T_BOLD) : a.text);
        else k += (size_t)snprintf(t + k, cap - k, "%s %s:%s  %s\n", mark, r0, r1, a.kind == A_BOLD ? tr(T_BOLD) : a.text);
    }
    if (k && t[k - 1] == '\n') t[k - 1] = '\0';
    ai_set_text(t);
    ai_apply_label();
    grid_invalidate();
}

// The explanation as it arrived (kept for the re-render after the checks)
char *s_ai_expl;

void ai_show_result(void) {
    if (!s_ai || !s_au) return;
    s_au->pending = false;
    if (s_ai_apply) lv_obj_add_flag(s_ai_apply, LV_OBJ_FLAG_HIDDEN);
    s_au->nacts = 0;
    if (s_aw->rc == -3) { ai_give_up(T_ANIMA_BUSY); return; }
    if (s_aw->rc < 0) {
        char *t = s_au->text;
        ai_local_summary(t, sizeof s_au->text);
        if (s_aw->rc == -1 && s_aw->note[0]) {
            const size_t l = strlen(t);
            snprintf(t + l, sizeof s_au->text - l, "\n(%s)", s_aw->note);
        }
        ai_set_text(t);
        return;
    }
    // remember the exchange for a follow-up
    snprintf(s_au->hist_q, sizeof s_au->hist_q, "%s", S->ai_q);
    snprintf(s_au->hist_a, sizeof s_au->hist_a, "%s", s_aw->out);
    if (!s_ai_expl) s_ai_expl = (char *)heap_caps_malloc(kAiOut, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ai_expl) return;
    size_t k = 0;
    char *save = nullptr;
    for (char *l = strtok_r(s_aw->out, "\n", &save); l; l = strtok_r(nullptr, "\n", &save)) {
        char *x = l;
        while (*x == ' ' || *x == '\t' || *x == '-' || *x == '*' || *x == '`') x++;
        if (!strncmp(l, "```", 3)) continue;
        Act a{};
        bool act = false;
        if (s_au->nacts < 40) {
            if (!strncasecmp(x, "SET ", 4)) {
                char *ref = x + 4;
                while (*ref == ' ') ref++;
                char *sp = strchr(ref, ' ');
                if (sp) {
                    *sp = '\0';
                    char *val = sp + 1;
                    while (*val == ' ') val++;
                    size_t vl = strlen(val);
                    while (vl && (val[vl - 1] == ' ' || val[vl - 1] == '`')) val[--vl] = '\0';
                    if (parse_ref(ref, &a.r0, &a.c0, &a.r1, &a.c1) && a.r0 == a.r1 && a.c0 == a.c1) {
                        a.kind = A_SET;
                        snprintf(a.text, sizeof a.text, "%s", val);
                        act = true;
                    } else *sp = ' ';
                }
            } else if (!strncasecmp(x, "FORMAT ", 7) || !strncasecmp(x, "BOLD ", 5)) {
                const bool bold = (x[0] & ~0x20) == 'B';
                char *ref = x + (bold ? 5 : 7);
                while (*ref == ' ') ref++;
                char *sp = strchr(ref, ' ');
                if (sp) *sp = '\0';
                const char *kind = sp ? sp + 1 : "";
                while (*kind == ' ') kind++;
                if (parse_ref(ref, &a.r0, &a.c0, &a.r1, &a.c1) && (bold || fmt_code(kind))) {
                    a.kind = bold ? A_BOLD : A_FORMAT;
                    snprintf(a.text, sizeof a.text, "%s", bold ? "" : kind);
                    act = true;
                } else if (sp) *sp = ' ';
            }
        }
        if (act) {
            a.ok = true;
            s_au->acts[s_au->nacts++] = a;
            continue;
        }
        if (k + 4 < (size_t)kAiOut) k += (size_t)snprintf(s_ai_expl + k, kAiOut - k, "%s\n", l);
    }
    if (k && s_ai_expl[k - 1] == '\n') s_ai_expl[--k] = '\0';
    // the engine checks every proposed formula before it is offered
    bool any = false;
    for (int i = 0; i < s_au->nacts; i++)
        if (s_au->acts[i].kind == A_SET && s_au->acts[i].text[0] == '=') {
            send("nv lang en");
            send("nv check %d %s", i, s_au->acts[i].text);
            any = true;
        }
    if (any) {
        send("nv lang %s", lang_code());
        s_au->checking = true;
        sync(S_CHECK);
    }
    ai_render(s_ai_expl);
}

void ai_reset_topic(void) {
    if (s_au) { s_au->hist_q[0] = s_au->hist_a[0] = '\0'; s_au->nacts = 0; s_au->text[0] = '\0'; }
}

void ai_checked(int i, bool ok) {
    if (s_au && i >= 0 && i < s_au->nacts) s_au->acts[i].ok = ok;
}
void ai_check_done(void) {
    if (!s_au) return;
    s_au->checking = false;
    if (s_ai && s_ai_expl) ai_render(s_ai_expl);
}

// The proposals on the grid: tinted cells, the new value in the accent color
void ai_draw_preview(lv_layer_t *layer, const lv_area_t &a, int32_t x0, int32_t y0) {
    if (!s_ai || !s_au || !s_au->nacts || s_au->pending) return;
    const NvTheme *th = nv_theme_get();
    const lv_area_t saved = layer->_clip_area;
    lv_area_t clip = {x0, y0, a.x2, a.y2};
    if (!area_and(&layer->_clip_area, &saved, &clip)) { layer->_clip_area = saved; return; }
    for (int i = 0; i < s_au->nacts; i++) {
        const Act &x = s_au->acts[i];
        if (!x.ok) continue;
        const int32_t x1 = x0 + S->colx[x.c0] - S->sx, x2 = x0 + S->colx[x.c1 + 1] - S->sx - 2;
        const int32_t y1 = y0 + x.r0 * kRowH - S->sy, y2 = y0 + (x.r1 + 1) * kRowH - S->sy - 2;
        if (x2 < x0 || y2 < y0 || x1 > a.x2 || y1 > a.y2) continue;
        fill(layer, x1, y1, x2, y2, th->primary, LV_OPA_30);
        frame(layer, x1, y1, x2, y2, th->primary, 1);
        if (x.kind == A_SET) {
            const bool text = !(x.text[0] == '=' || (x.text[0] >= '0' && x.text[0] <= '9') || x.text[0] == '-');
            cell_text(layer, x1, x2, y1, x.text, th->text_strong, text ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
        }
    }
    layer->_clip_area = saved;
}

void ai_apply_cb(lv_event_t *) {
    if (s_au && s_au->pending) {                          // "Cancel": stop the call in flight
        nucleo_anima_cancel();
        ai_give_up(T_ANIMA_STOPPED);
        return;
    }
    if (!s_au || s_au->checking) return;
    const Act *a = s_au->acts;
    const int n = s_au->nacts;
    int first = -1, last = -1;
    send("nv lang en");                       // ANIMA writes English notation: '.' decimals, ',' arguments
    for (int i = 0; i < n; i++) {
        if (!a[i].ok) continue;
        if (a[i].kind == A_SET) change("nv set %d %d %s", a[i].r0, a[i].c0, a[i].text);
        else if (a[i].kind == A_BOLD) change("nv bold %d %d %d %d 1", a[i].r0, a[i].c0, a[i].r1, a[i].c1);
        else if (const char *f = fmt_code(a[i].text)) change("nv fmt %d %d %d %d %s", a[i].r0, a[i].c0, a[i].r1, a[i].c1, f);
        if (first < 0) first = i;
        last = i;
    }
    send("nv lang %s", lang_code());
    if (first >= 0) {
        S->ar = a[first].r0; S->ac = a[first].c0;
        S->cr = a[last].r1; S->cc = a[last].c1;
        if (S->ar > S->cr) { const int t = S->ar; S->ar = S->cr; S->cr = t; }
        if (S->ac > S->cc) { const int t = S->ac; S->ac = S->cc; S->cc = t; }
    }
    s_au->nacts = 0;
    if (s_ai_apply) lv_obj_add_flag(s_ai_apply, LV_OBJ_FLAG_HIDDEN);
    if (s_ai_expl) ai_set_text(s_ai_expl);
    refresh();
    reveal(S->cr, S->cc);
    selection_changed(true);
    update_title();
    nv_toast(NV_NOTE_OK, tr(T_ANIMA_APPLIED));
}

}  // namespace

// The selected data arrived (S_TSV_AI): build the prompt and wake the worker
void ai_send(void) {
    if (!s_aw || !s_au) return;
    // the worker still reads the previous request (a panel closed and reopened meanwhile)
    if (s_aw->task && s_aw->want.load() != s_aw->done.load()) {
        ai_give_up(T_ANIMA_BUSY);
        return;
    }
    static const char *const kLang[5] = {"English", "Italian", "Spanish", "French", "German"};
    char used[24], first[4], ua[12], ub[12];
    cell_name(0, 0, ua, sizeof ua);
    cell_name(S->maxrow, S->maxcol, ub, sizeof ub);
    snprintf(used, sizeof used, "%s:%s", ua, ub);
    col_name(S->tsv_c0, first);
    snprintf(s_aw->sys, sizeof s_aw->sys,
             "You are ANIMA, the assistant inside Fogli, the spreadsheet app of NucleoOS. Reply in %s, clearly "
             "and briefly: at most 120 words of explanation, plain text, no markdown tables or headings.\n"
             "You receive cells of the user's sheet as tab-separated rows. The first field of each row is the "
             "row number; the other fields are the columns in order, starting at column %s. A formula cell shows "
             "its value followed by its formula in brackets, like: 2250 [=SUM(B2:D2)]. Base every statement on "
             "these data only and never invent values; when the data do not answer the question, say so.\n"
             "When the user asks to compute, fill, fix, format or create something in the sheet, after the "
             "explanation add the changes, one per line, exactly in these forms:\n"
             "SET <cell> <content>\n"
             "FORMAT <cell or range> <number|thousands|currency|percent|general>\n"
             "BOLD <cell or range>\n"
             "Examples: SET E2 =SUM(B2:D2)   SET A10 Total   FORMAT B2:E9 currency   BOLD A1:E1\n"
             "Contents: a value, or an Excel formula starting with '=' that uses English function names, ',' "
             "between arguments and '.' for decimals; rows count from 1. Prefer formulas that reference the "
             "cells, so results update when the data change. Only these functions exist: SUM AVERAGE COUNT MAX "
             "MIN PRODUCT ROUND ABS SQRT POWER IF STDEV INT EXP LN LOG10 NOW YEAR MONTH DAY HOUR MINUTE SECOND "
             "UPPER LOWER LEN; IF compares with = <> < > <= >=, & joins text. Put new results in empty cells "
             "right next to or below the data (the sheet's used area is %s), with a short label in %s in the "
             "cell before, and never overwrite filled cells unless the user asks to change them. At most 40 "
             "changes. Explaining or checking needs no changes at all.",
             kLang[lang_ix()], first, used, kLang[lang_ix()]);
    char a[12], b[12];
    cell_name(sel_r0(), sel_c0(), a, sizeof a);
    cell_name(LV_MIN(sel_r1(), kMaxRow), LV_MIN(sel_c1(), kMaxCol), b, sizeof b);
    size_t k = (size_t)snprintf(s_aw->user, sizeof s_aw->user, "File: %s. Sheet: %s. Used area: %s. Selection: %s%s%s.\n",
                                S->path[0] ? base_name(S->path) : "(not saved yet)", S->sheet[S->cursheet], used, a,
                                sel_single() ? "" : ":", sel_single() ? "" : b);
    if (s_au->hist_q[0] && k < sizeof s_aw->user - 1400)
        k += (size_t)snprintf(s_aw->user + k, sizeof s_aw->user - k,
                              "Previous question: %s\nYour previous answer: %s\n", s_au->hist_q, s_au->hist_a);
    snprintf(s_aw->user + k, sizeof s_aw->user - k, "Data:\n%.*s\nRequest: %s", kTsvMax, S->tsv, S->ai_q);
    s_ai_gen = s_aw->want.fetch_add(1) + 1;
    if (!s_aw->task)
        xTaskCreateWithCaps(ai_task, "sheet_ai", 32 * 1024, nullptr, 3, &s_aw->task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_aw->task) xTaskNotifyGive(s_aw->task);
}

namespace {

void ai_ask(const char *q) {
    if (!S->ready || !s_ai) return;
    if (!s_aw) {
        s_aw = (Ai *)heap_caps_calloc(1, sizeof(Ai), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_aw) return;
        new (&s_aw->want) std::atomic<uint32_t>(0);
        new (&s_aw->done) std::atomic<uint32_t>(0);
    }
    if (!s_au) s_au = (AiUi *)heap_caps_calloc(1, sizeof(AiUi), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_au || s_au->pending) return;
    snprintf(S->ai_q, sizeof S->ai_q, "%s", q);
    s_au->nacts = 0;
    s_au->pending = true;
    s_au->dots = 0;
    s_au->t_start = esp_timer_get_time();
    char thinking[160];
    snprintf(thinking, sizeof thinking, "%s...", tr(T_ANIMA_THINKING));
    ai_set_text(thinking);
    if (s_ai_apply) lv_obj_add_flag(s_ai_apply, LV_OBJ_FLAG_HIDDEN);
    grid_invalidate();
    // the selection, or the whole used table when one cell is selected
    int r0 = sel_r0(), c0 = sel_c0(), r1 = LV_MIN(sel_r1(), S->maxrow), c1 = LV_MIN(sel_c1(), S->maxcol);
    if (sel_single() || r1 < r0 || c1 < c0) { r0 = 0; c0 = 0; r1 = S->maxrow; c1 = S->maxcol; }
    r1 = LV_MIN(r1, r0 + 150);
    c1 = LV_MIN(c1, c0 + 25);
    char ra[12], rb[12], range[28];
    cell_name(r0, c0, ra, sizeof ra);
    cell_name(r1, c1, rb, sizeof rb);
    snprintf(range, sizeof range, "%s:%s", ra, rb);
    snprintf(s_au->scope, sizeof s_au->scope, tr(T_ANIMA_SCOPE), range, (r1 - r0 + 1) * (c1 - c0 + 1));
    if (s_ai_scope) lv_label_set_text(s_ai_scope, s_au->scope);
    S->tsv_n = 0;
    S->tsv_c0 = c0;
    send("nv tsv %d %d %d %d fx", r0, c0, r1, c1);
    sync(S_TSV_AI);
}

void ai_chip_cb(lv_event_t *e) {
    static const char *const kAsk[3] = {
        "Summarize these data: what they are about, the totals and averages of the numeric columns, and the "
        "highest and lowest values worth noting.",
        "Check these data for mistakes: formulas that leave out rows or point to the wrong cells, totals that "
        "do not add up, missing values, outliers, text where numbers are expected. List each problem with its "
        "cell and propose the fix as changes.",
        "Explain the formula of the active cell step by step, in plain words.",
    };
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == 3) {                                        // a formula: the user says what it must do
        if (s_ai_ta) { lv_textarea_set_text(s_ai_ta, ""); focus_ta(s_ai_ta); }
        return;
    }
    char q[800];
    if (i == 2) {
        char a[12];
        cell_name(S->cr, S->cc, a, sizeof a);
        snprintf(q, sizeof q, "%s The active cell is %s and contains %s", kAsk[2], a, S->xtext);
    } else {
        snprintf(q, sizeof q, "%s", kAsk[i]);
    }
    if (s_au) s_au->hist_q[0] = '\0';                    // a quick action starts a new topic
    ai_ask(q);
}
void ai_submit(lv_obj_t *, void *) {
    if (!s_ai_ta) return;
    const char *t = lv_textarea_get_text(s_ai_ta);
    if (!t[0]) return;
    char q[600];
    char a[12];
    cell_name(S->cr, S->cc, a, sizeof a);
    snprintf(q, sizeof q, "%s (active cell: %s)", t, a);
    lv_textarea_set_text(s_ai_ta, "");
    nv_ime_hide();
    ai_ask(q);
}
void ai_send_cb(lv_event_t *) { ai_submit(nullptr, nullptr); }
void ai_new_cb(lv_event_t *) {
    if (!s_au || s_au->pending) return;
    s_au->hist_q[0] = s_au->hist_a[0] = '\0';
    s_au->nacts = 0;
    ai_set_text("");
    ai_apply_label();
    grid_invalidate();
}

void ai_close(void) {
    if (!s_ai) return;
    nv_ime_set_submit_cb(nullptr, nullptr);
    lv_obj_delete(s_ai);
    s_ai = s_ai_ta = s_ai_out = s_ai_apply = s_ai_scope = nullptr;
    s_ai_gen = 0;
    if (s_au) { s_au->nacts = 0; s_au->pending = false; }
    grid_invalidate();
}
void ai_close_cb(lv_event_t *) { ai_close(); }

lv_obj_t *s_ai_explain;

// Called on every active-cell change: "Explain the formula" only means something on a formula
void ai_sync_chips(void) {
    if (!s_ai_explain) return;
    const bool f = S->xr == S->cr && S->xc == S->cc && S->xtext[0] == '=';
    if (f) lv_obj_remove_state(s_ai_explain, LV_STATE_DISABLED);
    else lv_obj_add_state(s_ai_explain, LV_STATE_DISABLED);
}

void cmd_anima(void) {
    if (s_ai) { ai_close(); return; }
    if (!s_root) return;
    const NvTheme *th = nv_theme_get();
    s_ai = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_ai);
    lv_obj_add_flag(s_ai, LV_OBJ_FLAG_FLOATING);
    lv_obj_update_layout(s_root);
    const int32_t top = s_tool ? lv_obj_get_height(s_tool) : 0;
    lv_obj_set_size(s_ai, 420, lv_obj_get_height(s_root) - top);
    lv_obj_align(s_ai, LV_ALIGN_TOP_RIGHT, 0, top);
    lv_obj_set_style_bg_color(s_ai, th->surface, 0);
    lv_obj_set_style_bg_opa(s_ai, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_ai, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(s_ai, 1, 0);
    lv_obj_set_style_border_color(s_ai, th->surface3, 0);
    lv_obj_set_style_pad_all(s_ai, 14, 0);
    lv_obj_set_style_pad_row(s_ai, 10, 0);
    lv_obj_set_flex_flow(s_ai, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_ai, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head = lv_obj_create(s_ai);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), 32);
    lv_obj_t *t = lv_label_create(head);
    lv_label_set_text(t, I_MAGIC "  ANIMA");
    lv_obj_set_style_text_font(t, &s_icons, 0);
    lv_obj_set_style_text_color(t, th->primary, 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *nw = lv_label_create(head);
    lv_label_set_text(nw, tr(T_ANIMA_NEW_Q));
    lv_obj_set_style_text_font(nw, &nv_font_14, 0);
    lv_obj_set_style_text_color(nw, th->text_dim, 0);
    lv_obj_align(nw, LV_ALIGN_RIGHT_MID, -44, 0);
    lv_obj_add_flag(nw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(nw, 10);
    lv_obj_add_event_cb(nw, ai_new_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *x = lv_label_create(head);
    lv_label_set_text(x, I_TIMES);
    lv_obj_set_style_text_font(x, &s_icons, 0);
    lv_obj_set_style_text_color(x, th->text_dim, 0);
    lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(x, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(x, 14);
    lv_obj_add_event_cb(x, ai_close_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *chips = lv_obj_create(s_ai);
    lv_obj_remove_style_all(chips);
    lv_obj_set_size(chips, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(chips, 6, 0);
    lv_obj_set_style_pad_row(chips, 6, 0);
    const T kChips[4] = {T_ANIMA_SUMMARY, T_ANIMA_CHECK, T_ANIMA_EXPLAIN, T_ANIMA_FORMULA};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = lv_button_create(chips);
        lv_obj_set_height(c, 36);
        lv_obj_set_style_bg_color(c, th->surface3, 0);
        lv_obj_set_style_radius(c, 18, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_40, LV_STATE_DISABLED);
        lv_obj_set_style_text_color(c, th->text_strong, 0);
        lv_obj_set_style_text_color(c, th->text_disabled, LV_STATE_DISABLED);
        lv_obj_add_event_cb(c, ai_chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text(l, tr(kChips[i]));
        lv_obj_set_style_text_font(l, &nv_font_14, 0);
        lv_obj_center(l);
        if (i == 2) s_ai_explain = c;
    }
    ai_sync_chips();

    lv_obj_t *in = lv_obj_create(s_ai);
    lv_obj_remove_style_all(in);
    lv_obj_set_size(in, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(in, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(in, 6, 0);
    s_ai_ta = nv_kit_textarea_ex(in, tr(T_ANIMA_HINT), true, NV_IME_TEXT, NV_IME_RET_SEND);
    lv_obj_set_flex_grow(s_ai_ta, 1);
    field_font(s_ai_ta);
    nv_ime_set_submit_cb(ai_submit, nullptr);
    lv_obj_t *send_b = lv_button_create(in);
    lv_obj_set_size(send_b, 44, 44);
    lv_obj_set_style_bg_color(send_b, th->primary, 0);
    lv_obj_add_event_cb(send_b, ai_send_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sl = lv_label_create(send_b);
    lv_label_set_text(sl, I_SEND);
    lv_obj_set_style_text_font(sl, &s_icons, 0);
    lv_obj_set_style_text_color(sl, th->on_primary, 0);
    lv_obj_center(sl);

    s_ai_scope = lv_label_create(s_ai);
    lv_obj_set_width(s_ai_scope, lv_pct(100));
    lv_label_set_long_mode(s_ai_scope, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_ai_scope, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_ai_scope, th->text_dim, 0);
    lv_label_set_text(s_ai_scope, tr(T_ANIMA_PRIVACY));

    lv_obj_t *scroll = lv_obj_create(s_ai);
    lv_obj_remove_style_all(scroll);
    lv_obj_set_width(scroll, lv_pct(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    s_ai_out = lv_label_create(scroll);
    lv_label_set_long_mode(s_ai_out, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_ai_out, lv_pct(100));
    lv_obj_set_style_text_color(s_ai_out, th->text, 0);
    lv_obj_set_style_text_font(s_ai_out, &s_font, 0);
    lv_label_set_text(s_ai_out, s_au && s_au->text[0] && !s_au->pending ? s_au->text : "");

    s_ai_apply = lv_button_create(s_ai);
    lv_obj_set_size(s_ai_apply, lv_pct(100), 44);
    lv_obj_set_style_bg_color(s_ai_apply, th->primary, 0);
    lv_obj_add_event_cb(s_ai_apply, ai_apply_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *al = lv_label_create(s_ai_apply);
    lv_obj_set_style_text_color(al, th->on_primary, 0);
    lv_obj_center(al);
    lv_obj_add_flag(s_ai_apply, LV_OBJ_FLAG_HIDDEN);
}

// "Analizza con ANIMA" from Files: the panel, and the summary of the whole table
void ai_open_and_summarize(void) {
    if (!s_ai) cmd_anima();
    if (!s_ai) return;
    if (s_au) s_au->hist_q[0] = '\0';
    static const char *const kSummary =
        "Summarize these data: what they are about, the totals and averages of the numeric columns, and the "
        "highest and lowest values worth noting.";
    ai_ask(kSummary);
}

// The request is left to finish on its own; the panel shows the device's own summary now
void ai_give_up(T why) {
    if (!s_au) return;
    s_ai_gen = 0;
    s_au->pending = false;
    s_au->nacts = 0;
    if (S->tsv_n) ai_local_summary(s_au->text, sizeof s_au->text, why);
    else snprintf(s_au->text, sizeof s_au->text, "%s", tr(why));
    ai_set_text(s_au->text);
    if (s_ai_apply) lv_obj_add_flag(s_ai_apply, LV_OBJ_FLAG_HIDDEN);
    grid_invalidate();
}

// "ANIMA is looking at the data" with moving dots while the worker runs
void ai_tick(void) {
    if (!s_au || !s_ai || !s_au->pending || !s_ai_out) return;
    if (esp_timer_get_time() - s_au->t_start > 90LL * 1000000) { ai_give_up(T_ANIMA_TIMEOUT); return; }
    if (s_ai_apply && lv_obj_has_flag(s_ai_apply, LV_OBJ_FLAG_HIDDEN)) {   // the button is "Cancel" meanwhile
        lv_label_set_text(lv_obj_get_child(s_ai_apply, 0), tr(T_CANCEL));
        lv_obj_set_style_bg_color(s_ai_apply, nv_theme_get()->surface3, 0);
        lv_obj_clear_flag(s_ai_apply, LV_OBJ_FLAG_HIDDEN);
    }
    if (++s_au->dots % 12) return;
    static const char *const kDots[4] = {"", ".", "..", "..."};
    char b[160];
    snprintf(b, sizeof b, "%s%s", tr(T_ANIMA_THINKING), kDots[(s_au->dots / 12) % 4]);
    lv_label_set_text(s_ai_out, b);
}

// ---------------------------------------------------------------- context menus
lv_point_t s_ctx_pt;
void m_cut(void *) { cmd_cut(); }
void m_copy(void *) { cmd_copy(); }
void m_paste(void *) { cmd_paste(false); }
void m_paste_v(void *) { cmd_paste(true); }
void m_clear(void *) { cmd_clear(); }
void m_ins_row(void *) { cmd_rows(true); }
void m_ins_col(void *) { cmd_cols(true); }
void m_del_row(void *) { cmd_rows(false); }
void m_del_col(void *) { cmd_cols(false); }
void m_sort_a(void *) { cmd_sort(false); }
void m_sort_d(void *) { cmd_sort(true); }
void m_fit(void *) { cmd_fit(); }
void m_edit(void *) { edit_start(nullptr, false); }
void m_anima(void *) { if (!s_ai) cmd_anima(); }

void cell_menu(int32_t x, int32_t y) {
    const nv_menu_item_t m[] = {
        {LV_SYMBOL_EDIT, tr(T_EDIT_CELL), "F2", m_edit, nullptr, false, false},
        {LV_SYMBOL_CUT, tr(T_CUT), "Ctrl+X", m_cut, nullptr, true, false},
        {LV_SYMBOL_COPY, tr(T_COPY), "Ctrl+C", m_copy, nullptr, false, false},
        {LV_SYMBOL_PASTE, tr(T_PASTE), "Ctrl+V", m_paste, nullptr, false, false},
        {LV_SYMBOL_PASTE, tr(T_PASTE_VALUES), nullptr, m_paste_v, nullptr, false, false},
        {LV_SYMBOL_TRASH, tr(T_CLEAR), "Canc", m_clear, nullptr, false, false},
        {LV_SYMBOL_PLUS, tr(T_INS_ROW), nullptr, m_ins_row, nullptr, true, false},
        {LV_SYMBOL_PLUS, tr(T_INS_COL), nullptr, m_ins_col, nullptr, false, false},
        {LV_SYMBOL_MINUS, tr(T_DEL_ROW), nullptr, m_del_row, nullptr, false, false},
        {LV_SYMBOL_MINUS, tr(T_DEL_COL), nullptr, m_del_col, nullptr, false, false},
        {LV_SYMBOL_DOWN, tr(T_SORT_ASC), nullptr, m_sort_a, nullptr, true, false},
        {LV_SYMBOL_IMAGE, tr(T_ASK_ANIMA), nullptr, m_anima, nullptr, false, false},
    };
    nv_ui_menu_open(x, y, m, 12);
}
void col_menu(int32_t x, int32_t y) {
    const nv_menu_item_t m[] = {
        {LV_SYMBOL_PLUS, tr(T_INS_COL), nullptr, m_ins_col, nullptr, false, false},
        {LV_SYMBOL_MINUS, tr(T_DEL_COL), nullptr, m_del_col, nullptr, false, false},
        {LV_SYMBOL_SHUFFLE, tr(T_FIT_WIDTH), nullptr, m_fit, nullptr, true, false},
        {LV_SYMBOL_DOWN, tr(T_SORT_ASC), nullptr, m_sort_a, nullptr, true, false},
        {LV_SYMBOL_UP, tr(T_SORT_DESC), nullptr, m_sort_d, nullptr, false, false},
        {LV_SYMBOL_TRASH, tr(T_CLEAR), nullptr, m_clear, nullptr, true, false},
    };
    nv_ui_menu_open(x, y, m, 6);
}
void row_menu(int32_t x, int32_t y) {
    const nv_menu_item_t m[] = {
        {LV_SYMBOL_PLUS, tr(T_INS_ROW), nullptr, m_ins_row, nullptr, false, false},
        {LV_SYMBOL_MINUS, tr(T_DEL_ROW), nullptr, m_del_row, nullptr, false, false},
        {LV_SYMBOL_TRASH, tr(T_CLEAR), nullptr, m_clear, nullptr, true, false},
    };
    nv_ui_menu_open(x, y, m, 3);
}

bool in_selection(int r, int c) { return r >= sel_r0() && r <= sel_r1() && c >= sel_c0() && c <= sel_c1(); }

void context_at(int32_t x, int32_t y) {
    if (!S->ready) return;
    if (S->editing) edit_commit(0, 0);
    int r, c;
    const Hit h = hit(x, y, &r, &c);
    if (h == H_COLHDR || h == H_COLEDGE) {
        if (!(c >= sel_c0() && c <= sel_c1() && sel_r0() == 0 && sel_r1() == kMaxRow)) {
            S->ar = 0; S->cr = kMaxRow; S->ac = S->cc = c;
            selection_changed(true);
        }
        col_menu(x, y);
    } else if (h == H_ROWHDR) {
        if (!(r >= sel_r0() && r <= sel_r1() && sel_c0() == 0 && sel_c1() == kMaxCol)) {
            S->ac = 0; S->cc = kMaxCol; S->ar = S->cr = r;
            selection_changed(true);
        }
        row_menu(x, y);
    } else if (h != H_NONE && h != H_CORNER) {
        if (!in_selection(r, c)) { S->ar = S->cr = r; S->ac = S->cc = c; selection_changed(true); }
        cell_menu(x, y);
    }
}

void grid_ctx_cb(lv_event_t *e) {
    if (const lv_point_t *p = (const lv_point_t *)lv_event_get_param(e)) context_at(p->x, p->y);
}

// ---------------------------------------------------------------- pointer
void autoscroll(int32_t px, int32_t py) {
    lv_area_t a;
    lv_obj_get_coords(s_grid, &a);
    const int32_t m = 24;
    if (py > a.y2 - m) S->sy += kRowH / 2;
    else if (py < a.y1 + kHdrH + m) S->sy -= kRowH / 2;
    if (px > a.x2 - m) S->sx += 24;
    else if (px < a.x1 + kRowHdrW + m) S->sx -= 24;
    clamp_scroll();
}

void grid_event(lv_event_t *e) {
    if (!S) return;
    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = {0, 0};
    if (in) lv_indev_get_point(in, &p);
    int r, c;
    switch (code) {
        case LV_EVENT_PRESSED: {
            S->px = S->drag_x = p.x;
            S->py = S->drag_y = p.y;
            S->moved = S->long_pressed = false;
            S->mouse = nv_sel_mouse();
            S->vx = S->vy = 0;
            const Hit h = hit(p.x, p.y, &r, &c);
            S->pr = r;
            S->pc = c;
            S->mode = M_NONE;
            if (!S->ready) { S->mode = M_PAN; break; }
            if (S->editing && h == H_CELL && point_mode()) {
                S->mode = M_POINT;
                S->pt_r0 = r;
                S->pt_c0 = c;
                point_insert(r, c, r, c, S->pt_len != 0);
                break;
            }
            if (h == H_COLEDGE) {
                S->mode = M_RESIZE;
                S->resize_col = c;
                S->resize_x0 = p.x;
                S->resize_w0 = col_w(c);
            } else if (h == H_COLHDR) {
                if (S->editing) edit_commit(0, 0);
                S->mode = M_COLSEL;
                S->ar = 0; S->cr = kMaxRow; S->ac = S->cc = c;
                selection_changed(true);
            } else if (h == H_ROWHDR) {
                if (S->editing) edit_commit(0, 0);
                S->mode = M_ROWSEL;
                S->ac = 0; S->cc = kMaxCol; S->ar = S->cr = r;
                selection_changed(true);
            } else if (h == H_CORNER) {
                if (S->editing) edit_commit(0, 0);
                S->ar = S->ac = 0;
                S->cr = LV_MAX(S->maxrow, 0);
                S->cc = LV_MAX(S->maxcol, 0);
                selection_changed(true);
            } else if (h == H_HANDLE_BR || h == H_HANDLE_TL) {
                S->mode = M_EXTEND;
                if (h == H_HANDLE_TL) {          // the opposite corner stays put
                    const int r1 = sel_r1(), c1 = sel_c1();
                    S->cr = sel_r0(); S->cc = sel_c0(); S->ar = r1; S->ac = c1;
                } else {
                    const int r0 = sel_r0(), c0 = sel_c0();
                    S->cr = sel_r1(); S->cc = sel_c1(); S->ar = r0; S->ac = c0;
                }
            } else if (S->mouse) {
                if (S->editing) edit_commit(0, 0);
                S->mode = M_SELECT;
                S->ar = S->cr = r;
                S->ac = S->cc = c;
                selection_changed(true);
            } else {
                S->mode = M_PAN;
            }
            break;
        }
        case LV_EVENT_PRESSING: {
            if (LV_ABS(p.x - S->px) > 8 || LV_ABS(p.y - S->py) > 8) S->moved = true;
            const int32_t dx = p.x - S->drag_x, dy = p.y - S->drag_y;
            S->drag_x = p.x;
            S->drag_y = p.y;
            if (S->mode == M_PAN && S->moved && !S->long_pressed) {
                S->sx -= dx;
                S->sy -= dy;
                S->vx = (float)-dx;
                S->vy = (float)-dy;
                clamp_scroll();
                request_view(false);
                grid_invalidate();
            } else if (S->mode == M_RESIZE) {
                const int32_t w = LV_MAX(20, S->resize_w0 + p.x - S->resize_x0);
                S->colw[S->resize_col] = (uint8_t)clampi((w - 2 * kPadX + kCharPx / 2) / kCharPx, 1, 60);
                colx_rebuild();
                grid_invalidate();
            } else if (S->mode == M_SELECT || S->mode == M_EXTEND || S->mode == M_COLSEL || S->mode == M_ROWSEL ||
                       S->mode == M_POINT || (S->mode == M_PAN && S->long_pressed)) {
                autoscroll(p.x, p.y);
                hit(p.x, p.y, &r, &c);
                if (S->mode == M_POINT) {
                    point_insert(S->pt_r0, S->pt_c0, r, c, true);
                } else {
                    if (S->mode == M_COLSEL) r = kMaxRow;
                    if (S->mode == M_ROWSEL) c = kMaxCol;
                    if (r != S->cr || c != S->cc) {
                        S->cr = r;
                        S->cc = c;
                        update_namebox();
                        grid_invalidate();
                    }
                }
                request_view(false);
            }
            break;
        }
        case LV_EVENT_LONG_PRESSED:
            if (S->mode == M_PAN && !S->moved && !S->mouse && S->ready) {
                S->long_pressed = true;
                if (S->editing) edit_commit(0, 0);
                hit(p.x, p.y, &r, &c);
                // a long press selects the cell and arms dragging the selection; let go for the menu
                if (!in_selection(r, c)) { S->ar = S->cr = r; S->ac = S->cc = c; selection_changed(true); }
            }
            break;
        case LV_EVENT_RELEASED: {
            const Mode m = S->mode;
            S->mode = M_NONE;
            if (m == M_RESIZE) {
                if (S->colw[S->resize_col] * kCharPx + 2 * kPadX != S->resize_w0) {
                    change("nv width %d %d", S->resize_col, S->colw[S->resize_col]);
                    refresh();
                }
                break;
            }
            if (m == M_PAN && S->long_pressed) {
                if (!S->moved) context_at(p.x, p.y);
                else { selection_changed(false); }
                break;
            }
            if (m == M_PAN && !S->moved) {
                hit(p.x, p.y, &r, &c);
                const int64_t now = esp_timer_get_time();
                const bool again = sel_single() && r == S->cr && c == S->cc && !S->editing;
                const bool dbl = now - S->last_tap_t < 400000 && r == S->last_tap_r && c == S->last_tap_c;
                S->last_tap_t = now;
                S->last_tap_r = r;
                S->last_tap_c = c;
                if (S->editing) edit_commit(0, 0);
                if (again || dbl) { S->ar = S->cr = r; S->ac = S->cc = c; edit_start(nullptr, false); break; }
                S->ar = S->cr = r;
                S->ac = S->cc = c;
                selection_changed(true);
                break;
            }
            if (m == M_SELECT && !S->moved) {
                const int64_t now = esp_timer_get_time();
                if (now - S->last_tap_t < 400000 && S->pr == S->last_tap_r && S->pc == S->last_tap_c) edit_start(nullptr, false);
                S->last_tap_t = now;
                S->last_tap_r = S->pr;
                S->last_tap_c = S->pc;
                break;
            }
            if (m == M_SELECT || m == M_EXTEND || m == M_COLSEL || m == M_ROWSEL) selection_changed(false);
            if (m == M_POINT && s_editor && !S->edit_bar) focus_ta(s_editor);
            break;
        }
        case LV_EVENT_SIZE_CHANGED:
            if (S->editing) reveal(S->er, S->ec);
            request_view(false);
            break;
        default: break;
    }
}

// ---------------------------------------------------------------- keyboard (physical)
bool grid_key(uint32_t key, uint8_t usage, uint8_t mods) {
    if (!S || !S->ready || s_modal || s_start) return false;
    const bool ctrl = mods & 0x11, shift = mods & 0x22;
    if (ctrl) {
        switch (usage) {
            case 0x06: cmd_copy(); return true;              // C
            case 0x19: cmd_paste(false); return true;        // V
            case 0x1B: cmd_cut(); return true;               // X
            case 0x1D: cmd_undo(); return true;              // Z
            case 0x1C: cmd_redo(); return true;              // Y
            case 0x16: cmd_save(); return true;              // S
            case 0x12: cmd_open(); return true;              // O
            case 0x11: cmd_new(); return true;               // N
            case 0x09: cmd_find(); return true;              // F
            case 0x05: cmd_style("bold", 1); return true;    // B
            case 0x0C: cmd_style("italic", 2); return true;  // I
            case 0x18: cmd_style("underline", 4); return true;   // U
            case 0x04:                                       // A
                S->ar = S->ac = 0; S->cr = S->maxrow; S->cc = S->maxcol;
                selection_changed(true);
                return true;
            default: return false;
        }
    }
    int dr = 0, dc = 0;
    switch (key) {
        case LV_KEY_UP: dr = -1; break;
        case LV_KEY_DOWN: dr = 1; break;
        case LV_KEY_LEFT: dc = -1; break;
        case LV_KEY_RIGHT: dc = 1; break;
        case LV_KEY_ENTER: edit_start(nullptr, false); return true;
        case LV_KEY_DEL:
        case LV_KEY_BACKSPACE: cmd_clear(); return true;
        case LV_KEY_HOME: select_cell(S->cr, 0, shift); return true;
        case LV_KEY_END: select_cell(S->cr, S->maxcol, shift); return true;
        default: break;
    }
    if (usage == 0x3B) { edit_start(nullptr, false); return true; }                 // F2
    if (usage == 0x4B) { select_cell(S->cr - grid_h() / kRowH, S->cc, shift); return true; }   // PgUp
    if (usage == 0x4E) { select_cell(S->cr + grid_h() / kRowH, S->cc, shift); return true; }   // PgDn
    if (dr || dc) { select_cell(S->cr + dr, S->cc + dc, shift); return true; }
    if (key >= 0x20 && key != 0x7F) {                      // typing over the cell, as in Excel
        char t[8];
        uint32_t cp = key;
        int n = 0;
        if (cp < 0x80) t[n++] = (char)cp;
        else if (cp < 0x800) { t[n++] = (char)(0xC0 | cp >> 6); t[n++] = (char)(0x80 | (cp & 0x3F)); }
        else { t[n++] = (char)(0xE0 | cp >> 12); t[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[n++] = (char)(0x80 | (cp & 0x3F)); }
        t[n] = '\0';
        edit_start(t, false);
        return true;
    }
    return false;
}

const nv_shortcut_t kShortcuts[] = {
    {"F2 / Enter", "Modifica la cella", "Edit the cell"},
    {"Ctrl+C / X / V", "Copia / Taglia / Incolla", "Copy / Cut / Paste"},
    {"Ctrl+Z / Y", "Annulla / Ripeti", "Undo / Redo"},
    {"Ctrl+B / I / U", "Grassetto / Corsivo / Sottolineato", "Bold / Italic / Underline"},
    {"Ctrl+S", "Salva", "Save"},
    {"Ctrl+O / N", "Apri / Nuovo", "Open / New"},
    {"Ctrl+F", "Trova", "Find"},
    {"Ctrl+A", "Seleziona tutto", "Select all"},
    {"Shift+frecce", "Estendi la selezione", "Extend the selection"},
    {"Canc", "Cancella il contenuto", "Clear the contents"},
};

// ---------------------------------------------------------------- toolbar
void tb_cb(lv_event_t *e);

enum Tb { TB_OPEN, TB_SAVE, TB_UNDO, TB_REDO, TB_CUT, TB_COPY, TB_PASTE, TB_BOLD, TB_ITALIC, TB_UNDERL,
          TB_FMT, TB_SUM, TB_SORTA, TB_SORTD, TB_FIND, TB_MORE, TB_ANIMA };

lv_obj_t *tb_button(lv_obj_t *bar, const char *icon, Tb id, bool wide = false) {
    const NvTheme *th = nv_theme_get();
    lv_obj_t *b = lv_obj_create(bar);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, wide ? LV_SIZE_CONTENT : 42, 40);
    if (wide) lv_obj_set_style_pad_hor(b, 12, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_bg_opa(b, wide ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(b, wide ? th->primary : th->surface3, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, th->surface3, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);    // keeps the editor's keyboard up
    lv_obj_add_event_cb(b, tb_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, icon);
    lv_obj_set_style_text_font(l, &s_icons, 0);
    lv_obj_set_style_text_color(l, wide ? th->on_primary : th->text, 0);
    lv_obj_center(l);
    return b;
}
void tb_sep(lv_obj_t *bar) {
    lv_obj_t *s = lv_obj_create(bar);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, 1, 26);
    lv_obj_set_style_bg_color(s, nv_theme_get()->surface3, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
}

void f_general(void *) { cmd_fmt(""); }
void f_number(void *) { cmd_fmt("0.00"); }
void f_thousands(void *) { cmd_fmt("#,##0.00"); }
void f_currency(void *) { cmd_fmt("#,##0.00 \xE2\x82\xAC"); }
void f_percent(void *) { cmd_fmt("0%"); }
void m_new(void *) { cmd_new(); }
void m_save_as(void *) { save_as(); }
void m_shortcuts(void *) { nv_ui_shortcuts_sheet(); }

void tb_cb(lv_event_t *e) {
    const Tb id = (Tb)(intptr_t)lv_event_get_user_data(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target_obj(e), &a);
    if (!S->ready && id != TB_OPEN && id != TB_MORE) return;
    if (S->editing && id != TB_SUM && id != TB_ANIMA) edit_commit(0, 0);
    switch (id) {
        case TB_OPEN: cmd_open(); break;
        case TB_SAVE: cmd_save(); break;
        case TB_UNDO: cmd_undo(); break;
        case TB_REDO: cmd_redo(); break;
        case TB_CUT: cmd_cut(); break;
        case TB_COPY: cmd_copy(); break;
        case TB_PASTE: cmd_paste(false); break;
        case TB_BOLD: cmd_style("bold", 1); break;
        case TB_ITALIC: cmd_style("italic", 2); break;
        case TB_UNDERL: cmd_style("underline", 4); break;
        case TB_SUM: if (!S->editing) cmd_autosum(); break;
        case TB_SORTA: cmd_sort(false); break;
        case TB_SORTD: cmd_sort(true); break;
        case TB_FIND: cmd_find(); break;
        case TB_ANIMA: cmd_anima(); break;
        case TB_FMT: {
            const nv_menu_item_t m[] = {
                {nullptr, tr(T_FMT_GENERAL), nullptr, f_general, nullptr, false, false},
                {nullptr, tr(T_FMT_NUMBER), nullptr, f_number, nullptr, false, false},
                {nullptr, tr(T_FMT_THOUSANDS), nullptr, f_thousands, nullptr, false, false},
                {nullptr, tr(T_FMT_CURRENCY), nullptr, f_currency, nullptr, false, false},
                {nullptr, tr(T_FMT_PERCENT), nullptr, f_percent, nullptr, false, false},
            };
            nv_ui_menu_open(a.x1, a.y2 + 4, m, 5);
            break;
        }
        case TB_MORE: {
            const nv_menu_item_t m[] = {
                {LV_SYMBOL_FILE, tr(T_NEW), "Ctrl+N", m_new, nullptr, false, false},
                {LV_SYMBOL_SAVE, tr(T_SAVE_AS), nullptr, S->ready ? m_save_as : nullptr, nullptr, false, false},
                {LV_SYMBOL_PLUS, tr(T_INS_ROW), nullptr, S->ready ? m_ins_row : nullptr, nullptr, true, false},
                {LV_SYMBOL_PLUS, tr(T_INS_COL), nullptr, S->ready ? m_ins_col : nullptr, nullptr, false, false},
                {LV_SYMBOL_MINUS, tr(T_DEL_ROW), nullptr, S->ready ? m_del_row : nullptr, nullptr, false, false},
                {LV_SYMBOL_MINUS, tr(T_DEL_COL), nullptr, S->ready ? m_del_col : nullptr, nullptr, false, false},
                {LV_SYMBOL_SHUFFLE, tr(T_FIT_WIDTH), nullptr, S->ready ? m_fit : nullptr, nullptr, true, false},
                {LV_SYMBOL_KEYBOARD, tr(T_SHORTCUTS), "F1", m_shortcuts, nullptr, true, false},
            };
            nv_ui_menu_open(a.x2 - 300, a.y2 + 4, m, 8);
            break;
        }
    }
}

// ---------------------------------------------------------------- formula bar
void fx_focus_cb(lv_event_t *e) {
    if (!S->ready || s_syncing_text) return;
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED && !S->editing) {
        S->editing = false;
        edit_start(nullptr, true);
    }
}

// ---------------------------------------------------------------- loading veil
void show_loading(bool on) {
    if (!s_root) return;
    if (!on) { if (s_loading) { lv_obj_delete(s_loading); s_loading = nullptr; } return; }
    if (s_loading) return;
    const NvTheme *th = nv_theme_get();
    s_loading = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_loading);
    lv_obj_add_flag(s_loading, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_loading, 240, 56);
    lv_obj_center(s_loading);
    lv_obj_set_style_bg_color(s_loading, th->surface2, 0);
    lv_obj_set_style_bg_opa(s_loading, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_loading, 14, 0);
    lv_obj_t *l = lv_label_create(s_loading);
    lv_label_set_text(l, tr(T_LOADING));
    lv_obj_set_style_text_color(l, th->text_strong, 0);
    lv_obj_center(l);
}

// ---------------------------------------------------------------- tick
void tick_cb(lv_timer_t *) {
    if (!S) return;
    // the engine: start, pump, collect
    const nv_wrun_state_t st = nv_wasm_exec_state();
    if (S->run && st == NV_WRUN_DONE && !strcmp(nv_wasm_exec_app_id(), "sc-im")) {
        bool ok;
        uint32_t ms;
        char err[96];
        nv_wasm_exec_collect(&ok, &ms, err, sizeof err);
        const bool expected = S->stopping;
        const bool loading = !S->ready;
        S->run = S->stopping = false;
        S->ready = false;
        if (!expected && loading && !S->big_heap) {            // out of memory on a big file: once more
            NV_LOGW(TAG, "engine stopped while loading (%s): retrying with the full heap", err);
            S->big_heap = true;
            S->want_start = true;
        } else if (!expected) {
            engine_dead();
        }
    }
    if (S->stopping && esp_timer_get_time() - S->stop_t > 2000000 && st == NV_WRUN_RUNNING) {
        nv_wasm_exec_abort();                              // it did not finish by itself
        S->stop_t = esp_timer_get_time();
    }
    if (S->want_start && !S->run && !S->stopping) engine_start_now();
    if (S->run) pump();
    if (!S) return;
    // kinetic scrolling after a fling
    if (S->mode == M_NONE && (fabsf(S->vx) > 0.5f || fabsf(S->vy) > 0.5f)) {
        S->sx += (int32_t)S->vx;
        S->sy += (int32_t)S->vy;
        S->vx *= 0.88f;
        S->vy *= 0.88f;
        clamp_scroll();
        request_view(false);
        grid_invalidate();
    }
    // mouse wheel over the grid
    if (s_grid && nv_hid_host_mouse_present()) {
        int32_t dx, dy, wh;
        uint8_t btn;
        if (nv_hid_host_mouse_take(&dx, &dy, &wh, &btn) && wh) {
            int x = -1, y = -1;
            uint8_t b = 0;
            lv_area_t a;
            lv_obj_get_coords(s_grid, &a);
            nv_hid_host_mouse_state(&x, &y, &b);
            if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2 && !s_modal) {
                S->sy -= wh * 3 * kRowH;
                clamp_scroll();
                request_view(false);
                grid_invalidate();
            }
        }
    }
    // a recovery copy while there are unsaved changes (every 30 s at most)
    const int64_t now = esp_timer_get_time();
    if (S->ready && S->edits != S->edits_backed && S->modified && now - S->backup_t > 30000000) {
        S->backup_t = now;
        mkdir(kRecoveryDir, 0777);
        send("nv backup %s", kRecovery);
        S->edits_backed = S->edits;
    }
    // ANIMA's answer
    ai_tick();
    if (s_aw && s_ai && s_ai_gen && s_aw->done.load() == s_ai_gen) {
        s_ai_gen = 0;
        ai_show_result();
    }
}

}  // namespace

void back_handler(void) {
    if (!S) { nv_ui_close_app(); return; }
    if (S->editing) { edit_cancel(); return; }
    if (s_ai) { ai_close(); return; }
    if (s_findbar && !lv_obj_has_flag(s_findbar, LV_OBJ_FLAG_HIDDEN)) { find_close(); return; }
    struct L {
        static void close(void) {
            const NvIntent *in = nv_open_intent();
            if (in && in->caller[0]) nv_open_finish();     // back where the user came from
            else nv_ui_close_app();
        }
    };
    ask_unsaved(L::close);
}

namespace {

// ---------------------------------------------------------------- build / teardown
void teardown_async(void *) {
    // a rebuild (theme, language, a file opened while we are in front) already made a new page:
    // the engine and the document stay
    if (s_root) return;
    if (!S) return;
    if (S->run) nv_wasm_exec_abort();
    heap_caps_free(s_live);
    heap_caps_free(s_stage);
    s_live = s_stage = nullptr;
    heap_caps_free(S);
    S = nullptr;
    if (s_files) { heap_caps_free(s_files); s_files = nullptr; }
    if (s_au) { heap_caps_free(s_au); s_au = nullptr; }
}

void page_deleted(lv_event_t *) {
    if (s_tick) { lv_timer_delete(s_tick); s_tick = nullptr; }
    nv_ime_set_submit_cb(nullptr, nullptr);
    if (s_modal) { lv_obj_delete(s_modal); s_modal = nullptr; }
    s_root = s_tool = s_fbar = s_name = s_fx = s_grid = s_tabs = s_tabrow = s_status = nullptr;
    s_editor = s_start = s_modal_ta = s_ai = s_ai_ta = s_ai_out = s_ai_apply = s_ai_scope = s_ai_explain = nullptr;
    s_findbar = s_find_ta = s_loading = nullptr;
    if (S) { S->editing = false; S->mode = M_NONE; }
    lv_async_call(teardown_async, nullptr);
}

void build_ui(lv_obj_t *content) {
    const NvTheme *th = nv_theme_get();
    s_root = lv_obj_create(content);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_root, th->bg, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_root, &s_font, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_root, page_deleted, LV_EVENT_DELETE, nullptr);

    // toolbar
    s_tool = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_tool);
    lv_obj_set_size(s_tool, lv_pct(100), 48);
    lv_obj_set_style_bg_color(s_tool, th->surface, 0);
    lv_obj_set_style_bg_opa(s_tool, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_tool, 8, 0);
    lv_obj_set_style_pad_column(s_tool, 4, 0);
    lv_obj_set_flex_flow(s_tool, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_tool, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_tool, LV_OBJ_FLAG_SCROLLABLE);
    tb_button(s_tool, I_FOLDER, TB_OPEN);
    tb_button(s_tool, I_SAVE, TB_SAVE);
    tb_sep(s_tool);
    tb_button(s_tool, I_UNDO, TB_UNDO);
    tb_button(s_tool, I_REDO, TB_REDO);
    tb_sep(s_tool);
    tb_button(s_tool, I_CUT, TB_CUT);
    tb_button(s_tool, I_COPY, TB_COPY);
    tb_button(s_tool, I_PASTE, TB_PASTE);
    tb_sep(s_tool);
    tb_button(s_tool, I_BOLD, TB_BOLD);
    tb_button(s_tool, I_ITALIC, TB_ITALIC);
    tb_button(s_tool, I_UNDERL, TB_UNDERL);
    tb_sep(s_tool);
    tb_button(s_tool, I_HASH, TB_FMT);
    tb_button(s_tool, I_SIGMA, TB_SUM);
    tb_button(s_tool, I_SORTDN, TB_SORTA);
    tb_button(s_tool, I_SORTUP, TB_SORTD);
    tb_button(s_tool, I_SEARCH, TB_FIND);
    lv_obj_t *sp = lv_obj_create(s_tool);
    lv_obj_remove_style_all(sp);
    lv_obj_set_height(sp, 1);
    lv_obj_set_flex_grow(sp, 1);
    tb_button(s_tool, I_MAGIC "  ANIMA", TB_ANIMA, true);
    tb_button(s_tool, I_MORE, TB_MORE);

    // formula bar
    s_fbar = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_fbar);
    lv_obj_set_size(s_fbar, lv_pct(100), 42);
    lv_obj_set_style_pad_hor(s_fbar, 8, 0);
    lv_obj_set_style_pad_column(s_fbar, 8, 0);
    lv_obj_set_flex_flow(s_fbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_fbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_fbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *nb = lv_obj_create(s_fbar);
    lv_obj_remove_style_all(nb);
    lv_obj_set_size(nb, 110, 34);
    lv_obj_set_style_radius(nb, 8, 0);
    lv_obj_set_style_bg_color(nb, th->surface2, 0);
    lv_obj_set_style_bg_opa(nb, LV_OPA_COVER, 0);
    s_name = lv_label_create(nb);
    lv_obj_set_style_text_color(s_name, th->text_strong, 0);
    lv_obj_center(s_name);
    lv_obj_t *fx = lv_label_create(s_fbar);
    lv_label_set_text(fx, "fx");
    lv_obj_set_style_text_color(fx, th->text_dim, 0);
    s_fx = nv_kit_textarea_ex(s_fbar, nullptr, true, NV_IME_TEXT, NV_IME_RET_ENTER);
    lv_obj_set_flex_grow(s_fx, 1);
    field_font(s_fx);
    lv_obj_set_height(s_fx, 36);
    lv_obj_set_style_text_font(s_fx, &s_font, 0);
    lv_obj_set_style_pad_ver(s_fx, 6, 0);
    lv_obj_add_event_cb(s_fx, fx_focus_cb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(s_fx, editor_sync, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(s_fx, editor_ready, LV_EVENT_READY, nullptr);
    nv_ime_set_key_hook(s_fx, editor_key);

    // find bar (in place of the formula bar while searching)
    s_findbar = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_findbar);
    lv_obj_set_size(s_findbar, lv_pct(100), 42);
    lv_obj_set_style_pad_hor(s_findbar, 8, 0);
    lv_obj_set_style_pad_column(s_findbar, 8, 0);
    lv_obj_set_flex_flow(s_findbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_findbar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_findbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *fi = lv_label_create(s_findbar);
    lv_label_set_text(fi, I_SEARCH);
    lv_obj_set_style_text_font(fi, &s_icons, 0);
    lv_obj_set_style_text_color(fi, th->text_dim, 0);
    s_find_ta = nv_kit_textarea_ex(s_findbar, tr(T_FIND_HINT), true, NV_IME_TEXT, NV_IME_RET_SEARCH);
    lv_obj_set_flex_grow(s_find_ta, 1);
    field_font(s_find_ta);
    lv_obj_set_height(s_find_ta, 36);
    lv_obj_set_style_pad_ver(s_find_ta, 6, 0);
    lv_obj_t *fc = lv_label_create(s_findbar);
    lv_label_set_text(fc, I_TIMES);
    lv_obj_set_style_text_font(fc, &s_icons, 0);
    lv_obj_set_style_text_color(fc, th->text_dim, 0);
    lv_obj_add_flag(fc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(fc, 12);
    lv_obj_add_event_cb(fc, find_close_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_findbar, LV_OBJ_FLAG_HIDDEN);

    // the grid
    s_grid = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_width(s_grid, lv_pct(100));
    lv_obj_set_flex_grow(s_grid, 1);
    lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_flag(s_grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_grid, grid_draw, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(s_grid, grid_event, LV_EVENT_ALL, nullptr);
    lv_obj_add_event_cb(s_grid, grid_ctx_cb, (lv_event_code_t)nv_ui_event_context(), nullptr);

    // sheets + status
    s_tabs = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_tabs);
    lv_obj_set_size(s_tabs, lv_pct(100), 40);
    lv_obj_set_style_bg_color(s_tabs, th->surface, 0);
    lv_obj_set_style_bg_opa(s_tabs, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_tabs, 8, 0);
    lv_obj_clear_flag(s_tabs, LV_OBJ_FLAG_SCROLLABLE);
    s_tabrow = lv_obj_create(s_tabs);
    lv_obj_remove_style_all(s_tabrow);
    lv_obj_set_size(s_tabrow, lv_pct(55), 40);
    lv_obj_set_flex_flow(s_tabrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_tabrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_tabrow, 6, 0);
    lv_obj_set_scroll_dir(s_tabrow, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_tabrow, LV_SCROLLBAR_MODE_OFF);
    s_status = lv_label_create(s_tabs);
    lv_obj_set_width(s_status, lv_pct(44));
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_status, th->text_dim, 0);
    lv_obj_set_style_text_font(s_status, &s_font, 0);
    lv_obj_align(s_status, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text(s_status, S->status);
}

void sheets_build(lv_obj_t *content) {
    if (!s_font_ok) {
        s_font = nv_font_14;
        s_font.fallback = &nv_font_sheet_14;
        s_icons = nv_font_sheet_18;
        s_icons.fallback = &nv_font_20;
        s_font_ok = true;
    }
    if (!S) {
        S = (State *)heap_caps_calloc(1, sizeof(State), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_live = (Cache *)heap_caps_malloc(sizeof(Cache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_stage = (Cache *)heap_caps_malloc(sizeof(Cache), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!S || !s_live || !s_stage) {
            heap_caps_free(S); heap_caps_free(s_live); heap_caps_free(s_stage);
            S = nullptr; s_live = s_stage = nullptr;
            lv_obj_t *l = lv_label_create(content);
            lv_label_set_text(l, "out of memory");
            return;
        }
        cache_reset(s_live, 0, 0);
        cache_reset(s_stage, 0, 0);
        memset(S->colw, 10, sizeof S->colw);
        colx_rebuild();
        S->start_tpl = -1;
    }
    build_ui(content);
    static bool s_skill_checked;
    if (!s_skill_checked) s_skill_checked = nv_bgwork_submit(skill_install, nullptr);   // (boot may have been early)
    nv_ui_set_back_handler(back_handler);
    nv_ui_set_key_handler(grid_key);
    nv_ui_set_shortcuts(kShortcuts, (int)(sizeof kShortcuts / sizeof kShortcuts[0]));
    s_tick = lv_timer_create(tick_cb, kTickMs, nullptr);

    // opened on a file (Files, "Open with", ANIMA): that one; else what was open, else the start page
    const NvIntent *in = nv_open_intent();
    char key[sizeof S->last_intent];
    if (in && in->verb == NV_INTENT_OPEN) snprintf(key, sizeof key, "%s|%s", in->handler, in->path);
    if (in && in->verb == NV_INTENT_OPEN && in->path[0] && strcmp(key, S->last_intent)) {
        snprintf(S->last_intent, sizeof S->last_intent, "%s", key);
        char p[256];
        snprintf(p, sizeof p, "%s", in->path);
        if (!strcmp(in->handler, "sheets.new")) {               // Files: a new spreadsheet in this folder
            struct L { static void go(void) { open_doc("", -1); } };
            snprintf(S->new_dir, sizeof S->new_dir, "%s", p);
            if (S->run && S->modified) ask_unsaved(L::go); else L::go();
        } else {
            const bool same = !strcmp(p, S->path) && (S->run || S->want_start);
            if (!strcmp(in->handler, "sheets.anima")) {         // "Analizza con ANIMA"
                if (same && S->ready) ai_open_and_summarize();
                else S->anima_on_ready = true;
            }
            if (!same) {
                if (S->run && S->modified) open_path_now(p);
                else { recents_add(p); open_doc(p, -1); }
            }
        }
    } else if (!S->run && !S->want_start) {
        start_page();
    }
    update_title();
    update_tabs();
    update_namebox();
    if (S->ready) { s_syncing_text = true; lv_textarea_set_text(s_fx, S->xtext); s_syncing_text = false; refresh(); }
    else if (S->want_start || S->run) show_loading(true);
}

const NvApp kSheetsApp = {
    "sheets", "Fogli", &nv_icon_sheets, 2u << 20, sheets_build, NV_STR_APP_SHEETS, nullptr, 0,
    "excel xlsx xls ods csv spreadsheet sheets calc tabella tabelle foglio fogli calcolo hoja hojas "
    "tableur classeur feuille tabellenkalkulation tabellen",
};

const NvOpenHandler kSheetsOpen = {
    "sheets.edit", "sheets",
    "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet application/vnd.ms-excel "
    "application/vnd.oasis.opendocument.spreadsheet text/csv text/tab-separated-values application/x-sc",
    -1, nullptr, LV_SYMBOL_EDIT, NV_OPEN_OPENER, 50, nullptr, nullptr,
};

// Reachable by id only (their types match no file): Files' "New spreadsheet" (the path is the
// folder) and the action below
const NvOpenHandler kSheetsNew = {
    "sheets.new", "sheets", "application/x-nv-sheets-new", -1, nullptr, nullptr, NV_OPEN_OPENER, 0, nullptr, nullptr,
};
const NvOpenHandler kSheetsAnima = {
    "sheets.anima", "sheets", "application/x-nv-sheets-anima", -1, nullptr, nullptr, NV_OPEN_OPENER, 0, nullptr, nullptr,
};
// A button in Files' details of a spreadsheet: Fogli opens it and ANIMA summarizes it
bool analyze_run(const char *path, void *) { return nv_open_run_id("sheets.anima", path); }
const NvOpenHandler kSheetsAnalyze = {
    "sheets.analyze", "sheets",
    "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet application/vnd.ms-excel "
    "application/vnd.oasis.opendocument.spreadsheet text/csv text/tab-separated-values",
    NV_STR_SHEETS_ANALYZE, nullptr, LV_SYMBOL_EYE_OPEN, NV_OPEN_ACTION, 0, analyze_run, nullptr,
};

#include "sheets_skill.h"

// ANIMA learns what Fogli does from a skill on the card (data/anima/skills): written by the app so
// it is there from the first boot of this firmware, refreshed when its text changes.
void skill_install(void *) {
    const char *dir = "/sdcard/data/anima/skills";
    const char *path = "/sdcard/data/anima/skills/fogli.md";
    struct stat st;
    if (stat(dir, &st) != 0) return;                      // no ANIMA content on this card
    FILE *f = fopen(path, "r");
    if (f) {
        char *cur = (char *)heap_caps_malloc(sizeof kFogliSkill + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        bool same = false;
        if (cur) {
            const size_t n = fread(cur, 1, sizeof kFogliSkill + 15, f);
            cur[n] = '\0';
            same = !strcmp(cur, kFogliSkill);
            heap_caps_free(cur);
        }
        fclose(f);
        if (same) return;
    }
    f = fopen(path, "w");
    if (!f) return;
    fputs(kFogliSkill, f);
    fclose(f);
    NV_LOGI(TAG, "ANIMA skill %s written", path);
}

}  // namespace

void sheets_app_register(void) {
    nv_app_register(&kSheetsApp);
    nv_open_register(&kSheetsOpen);
    nv_open_register(&kSheetsNew);
    nv_open_register(&kSheetsAnima);
    nv_open_register(&kSheetsAnalyze);
    nv_bgwork_submit(skill_install, nullptr);
}
