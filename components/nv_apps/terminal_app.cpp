// terminal_app — the NucleoOS terminal: a Linux-style terminal screen for the shell in term_sh.cpp
// (POSIX-flavoured command language, GNU-style core utilities, NucleoOS built-ins) and for WASI
// terminal programs — any installed app whose manifest says "console": true (Lua, JavaScript,
// SQLite, ... from the Store) runs here with its command line as argv, what the user types as stdin
// and its output streamed to the screen.
//
// This file is the tty: an edge-to-edge dark screen in DejaVu Sans Mono, a bash-style prompt with
// the command typed inline after it, ANSI colours (recolour spans in the scrollback label), a row
// of extra keys (Tab Ctrl ^C ^D arrows symbols), Tab completion, history, and hardware-keyboard
// shortcuts (Ctrl-C/D/L/U/K/A/E/W). The shell runs on its own task and writes into a ring buffer
// drained here; programs are started here on its behalf (term_prog_run). Output text is English
// (a Unix terminal), so it adds no i18n keys; only the launcher label is translated.
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

#include "lvgl.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace {

// Look: a dark Linux terminal (GNOME Terminal / Tango palette) edge to edge, DejaVu Sans Mono.
constexpr uint32_t kBg      = 0x121417;   // terminal background
constexpr uint32_t kFg      = 0xD3D7CF;   // default text
constexpr uint32_t kFgBold  = 0xEEEEEC;   // bold with the default colour
constexpr uint32_t kUser    = 0x8AE234;   // prompt user@host
constexpr uint32_t kPath    = 0x729FCF;   // prompt path, directories in ls
constexpr uint32_t kKeyBg   = 0x202327;   // extra-keys row
constexpr uint32_t kKey     = 0x2C3035;
constexpr uint32_t kKeyDown = 0x3A3F46;
// ANSI 0..15 (Tango; black and blue lifted a little so they read on the dark background).
constexpr uint32_t kAnsi[16] = {
    0x555753, 0xCC0000, 0x4E9A06, 0xC4A000, 0x3B72C4, 0x75507B, 0x06989A, 0xD3D7CF,
    0x7F8386, 0xEF2929, 0x8AE234, 0xFCE94F, 0x729FCF, 0xAD7FA8, 0x34E2E2, 0xEEEEEC,
};

// The scrollback is label text with LVGL recolour markup ("#RRGGBB text#"): ANSI colours become
// spans, a literal '#' is written "##". Spans are closed before every newline and reopened after
// it, so dropping whole lines from the front never leaves a span half cut.
constexpr size_t kScrollCap = 24000;  // scrollback bytes; oldest whole lines drop when full
EXT_RAM_BSS_ATTR char s_scroll[kScrollCap];   // cold text buffer -> PSRAM (internal SRAM is scarce)
size_t     s_len = 0;
int32_t    s_span = -1;            // colour of the open span (0xRRGGBB), -1 = default colour
bool       s_bol = true;           // the scrollback ends at the start of a line
lv_obj_t  *s_out      = nullptr;   // wrapping label: every complete line of the scrollback
lv_obj_t  *s_tail     = nullptr;   // the open last line (shell / program prompt) left of the input
lv_obj_t  *s_scrollbox = nullptr;  // their scroll container (auto-scrolled to bottom)
lv_obj_t  *s_inrow    = nullptr;   // prompt line: s_tail + s_input
lv_obj_t  *s_input    = nullptr;   // the command line being typed (IME-bound, drawn inline)
lv_obj_t  *s_root     = nullptr;
int32_t    s_kb_h     = 0;         // on-screen keyboard height while it is up

// DejaVu Sans Mono 17 (10 px cells): ASCII, Latin-1, box drawing and block elements; anything
// else falls back to the UI font. A RAM copy, since the fallback is a field.
lv_font_t  s_mono;
bool       s_mono_ok = false;

// The terminal program run for the shell (nv_wasm runs one app at a time), and the ANSI parser
// state of the screen.
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
    uint8_t     esc    = 0;         // output filter state: 0 text, 1 after ESC, 2 in CSI, 3 in OSC
    uint8_t     col    = 0;         // column of the last output line (tab stops), mod 256
    bool        cr     = false;     // a '\r' waits: a newline follows, or the line is redrawn
    uint8_t     np     = 0;         // CSI parameters collected so far
    uint16_t    par[16] = {};
    // SGR state: foreground as an ANSI index (-1 default) or 24-bit colour; bold brightens it.
    int16_t     fg     = -1;
    bool        fg_rgb = false;
    uint32_t    rgb    = 0;
    bool        bold   = false;
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
constexpr int kHistMax = 32;
EXT_RAM_BSS_ATTR char s_hist[kHistMax][256];
int s_hist_n = 0, s_hist_pos = 0;

// ---------------------------------------------------------------- scrollback

// Complete lines go to s_out, the open last line (a prompt) to s_tail, which sits left of the
// input field, so typing continues on the prompt's line like a real terminal.
void out_flush(void) {
    if (!s_out) return;
    size_t cut = s_len;
    while (cut && s_scroll[cut - 1] != '\n') cut--;
    if (cut) {
        s_scroll[cut - 1] = '\0';                    // the label copies its text
        lv_label_set_text(s_out, s_scroll);
        s_scroll[cut - 1] = '\n';
        lv_obj_clear_flag(s_out, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_out, "");
        lv_obj_add_flag(s_out, LV_OBJ_FLAG_HIDDEN);  // no empty line above the first prompt
    }
    if (s_tail) lv_label_set_text(s_tail, s_scroll + cut);
    if (s_scrollbox) {
        // Pin the prompt line to the bottom; when everything fits (after `clear`) show it from
        // the top. A shrunken scrollback can leave the old offset past the end, so set it outright.
        lv_obj_update_layout(s_scrollbox);
        int32_t y = lv_obj_get_scroll_y(s_scrollbox) + lv_obj_get_scroll_bottom(s_scrollbox);
        if (y < 0) y = 0;
        if (y != lv_obj_get_scroll_y(s_scrollbox)) lv_obj_scroll_to_y(s_scrollbox, y, LV_ANIM_OFF);
    }
}

// Make room for n more bytes, dropping the oldest whole lines.
void make_room(size_t n) {
    if (s_len + n + 1 < kScrollCap) return;
    size_t drop = (s_len + n + 2) - kScrollCap;   // bytes we must free
    if (drop > s_len) drop = s_len;               // n close to the cap: never let memmove's length wrap
    while (drop < s_len && s_scroll[drop] != '\n') drop++;  // cut on a line boundary
    if (drop < s_len) drop++;                                // include the newline
    memmove(s_scroll, s_scroll + drop, s_len - drop);
    s_len -= drop;
}

void raw_put(const char *s, size_t n) {
    if (n >= kScrollCap) return;
    make_room(n);
    memcpy(s_scroll + s_len, s, n);
    s_len += n;
    s_scroll[s_len] = '\0';
}

void span_open(uint32_t rgb) {
    char b[10];
    lv_snprintf(b, sizeof b, "#%06X ", (unsigned)(rgb & 0xFFFFFFu));
    raw_put(b, 8);
}

// Switch the text colour (0xRRGGBB, -1 = default) for what is written next.
void set_color(int32_t rgb) {
    if (rgb == s_span) return;
    if (s_span >= 0) raw_put("#", 1);
    s_span = rgb;
    if (rgb >= 0) span_open((uint32_t)rgb);
}

void term_putc(char c) {
    if (c == '#') {                       // literal '#': "##", outside any span
        if (s_span >= 0) { raw_put("###", 3); span_open((uint32_t)s_span); }
        else raw_put("##", 2);
        s_bol = false;
    } else if (c == '\n') {
        if (s_span >= 0) { raw_put("#\n", 2); span_open((uint32_t)s_span); }
        else raw_put("\n", 1);
        s_bol = true;
    } else {
        raw_put(&c, 1);
        s_bol = false;
    }
}

void term_puts(const char *s) { while (*s) term_putc(*s++); }
void term_line(const char *s) { term_puts(s); term_putc('\n'); }

void term_clear(void) {
    s_len = 0; s_scroll[0] = '\0';
    s_span = -1;
    s_bol = true;
}

// Erase the last character of the open line (a program's backspace). Markup at the very end (a
// span that just opened or closed) is left alone: the rare case is not worth a real parser.
void term_backspace(void) {
    if (!s_len || s_scroll[s_len - 1] == '\n') return;
    if (s_span >= 0 && s_len >= 8 && s_scroll[s_len - 8] == '#' && s_scroll[s_len - 1] == ' ') return;
    if (s_scroll[s_len - 1] == '#') {
        if (s_len >= 2 && s_scroll[s_len - 2] == '#') s_len -= 2;
        else return;
    } else {
        do { s_len--; } while (s_len && ((unsigned char)s_scroll[s_len] & 0xC0) == 0x80);
    }
    s_scroll[s_len] = '\0';
}

// Redraw the open line from its start (a lone '\r': progress bars, spinners).
void term_cr(void) {
    size_t cut = s_len;
    while (cut && s_scroll[cut - 1] != '\n') cut--;
    s_len = cut;
    s_scroll[s_len] = '\0';
    if (s_span >= 0) span_open((uint32_t)s_span);
    s_bol = true;
}

// The shell prompt, bash style: user@host:dir$ (a program may have left colours on: reset).
void shell_prompt(void) {
    s_prog.fg = -1; s_prog.fg_rgb = false; s_prog.bold = false;
    set_color(-1);
    if (!s_bol) term_putc('\n');
    char dir[160];
    sh_prompt_dir(dir, sizeof dir);
    set_color((int32_t)kUser); term_puts("nucleo@anima");
    set_color(-1);             term_putc(':');
    set_color((int32_t)kPath); term_puts(dir);
    set_color(-1);             term_puts("$ ");
}

// ---------------------------------------------------------------- ANSI (program output)

uint32_t ansi256(unsigned n) {
    if (n < 16) return kAnsi[n];
    if (n < 232) {
        static const uint8_t lv[6] = {0, 95, 135, 175, 215, 255};
        n -= 16;
        return ((uint32_t)lv[n / 36] << 16) | ((uint32_t)lv[(n / 6) % 6] << 8) | lv[n % 6];
    }
    const uint32_t g = 8 + 10 * (n - 232);
    return (g << 16) | (g << 8) | g;
}

void sgr_apply(void) {
    int32_t c;
    if (s_prog.fg_rgb)     c = (int32_t)s_prog.rgb;
    else if (s_prog.fg < 0) c = s_prog.bold ? (int32_t)kFgBold : -1;
    else c = (int32_t)ansi256((unsigned)(s_prog.bold && s_prog.fg < 8 ? s_prog.fg + 8 : s_prog.fg));
    set_color(c);
}

// Select Graphic Rendition: foreground colours and bold. Background, underline and reverse have
// no rendering in a label and are skipped (with their sub-parameters).
void csi_sgr(void) {
    if (!s_prog.np) s_prog.par[s_prog.np++] = 0;   // "ESC[m" = reset
    for (unsigned i = 0; i < s_prog.np; i++) {
        const unsigned p = s_prog.par[i];
        if (p == 0) { s_prog.fg = -1; s_prog.fg_rgb = false; s_prog.bold = false; }
        else if (p == 1) s_prog.bold = true;
        else if (p == 22) s_prog.bold = false;
        else if (p >= 30 && p <= 37) { s_prog.fg = (int16_t)(p - 30); s_prog.fg_rgb = false; }
        else if (p >= 90 && p <= 97) { s_prog.fg = (int16_t)(p - 90 + 8); s_prog.fg_rgb = false; }
        else if (p == 39) { s_prog.fg = -1; s_prog.fg_rgb = false; }
        else if (p == 38 || p == 48) {
            const bool fg = p == 38;
            if (i + 2 < s_prog.np && s_prog.par[i + 1] == 5) {
                if (fg) { s_prog.fg = (int16_t)(s_prog.par[i + 2] & 0xFF); s_prog.fg_rgb = false; }
                i += 2;
            } else if (i + 4 < s_prog.np && s_prog.par[i + 1] == 2) {
                if (fg) {
                    s_prog.rgb = ((uint32_t)(s_prog.par[i + 2] & 0xFF) << 16) |
                                 ((uint32_t)(s_prog.par[i + 3] & 0xFF) << 8) | (s_prog.par[i + 4] & 0xFF);
                    s_prog.fg_rgb = true;
                }
                i += 4;
            }
        }
    }
    sgr_apply();
}

void csi_final(unsigned char f) {
    switch (f) {
        case 'm': csi_sgr(); break;
        case 'J':   // erase display: 2 / 3 = the whole screen -> behave like `clear`
            if (s_prog.np && (s_prog.par[0] == 2 || s_prog.par[0] == 3)) {
                term_clear();
                sgr_apply();
                s_prog.col = 0;
            }
            break;
        default: break;   // cursor moves, line erase, modes: no cell grid to act on
    }
}

// Program output is a byte stream written for a terminal: colours (SGR) become recolour spans,
// "clear screen" clears, other escape sequences are dropped. Tabs expand to 8-column stops,
// backspace erases, a lone carriage return redraws the line. Parser state survives chunks.
void prog_put(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        switch (s_prog.esc) {
            case 1:   // after ESC: '[' starts a CSI, ']' an OSC, anything else is a 2-byte sequence
                s_prog.esc = c == '[' ? 2 : c == ']' ? 3 : 0;
                s_prog.np = 0;
                s_prog.par[0] = 0;
                continue;
            case 2:   // CSI parameters until a final byte 0x40..0x7E
                if (c >= '0' && c <= '9') {
                    if (!s_prog.np) s_prog.np = 1;
                    uint16_t &p = s_prog.par[s_prog.np - 1];
                    p = (uint16_t)(p * 10 + (c - '0'));
                } else if (c == ';' || c == ':') {
                    if (!s_prog.np) s_prog.np = 1;   // ";5" = an empty (0) first parameter
                    if (s_prog.np < sizeof s_prog.par / sizeof s_prog.par[0]) s_prog.par[s_prog.np++] = 0;
                } else if (c >= 0x40 && c <= 0x7E) {
                    s_prog.esc = 0;
                    csi_final(c);
                }
                continue;
            case 3:   // OSC until BEL (or ESC \)
                if (c == 0x07) s_prog.esc = 0;
                else if (c == 0x1B) s_prog.esc = 1;
                continue;
            default:
                break;
        }
        if (s_prog.cr) {
            s_prog.cr = false;
            if (c != '\n') { term_cr(); s_prog.col = 0; }
        }
        if (c == 0x1B) { s_prog.esc = 1; continue; }
        if (c == '\r') { s_prog.cr = true; continue; }
        if (c == '\n') { term_putc('\n'); s_prog.col = 0; continue; }
        if (c == '\t') {
            do { term_putc(' '); s_prog.col++; } while (s_prog.col % 8);
            continue;
        }
        if (c == '\b') {
            term_backspace();
            if (s_prog.col) s_prog.col--;
            continue;
        }
        if (c < 0x20 || c == 0x7F) continue;   // BEL and other controls
        term_putc((char)c);
        if ((c & 0xC0) != 0x80) s_prog.col++;  // count UTF-8 lead bytes only
    }
}

// ---------------------------------------------------------------- tty: shell output ring

// The shell task writes here (term_tty_write); the tty timer drains it into the scrollback.
constexpr size_t kRing = 32 * 1024;
char             *s_ring = nullptr;          // PSRAM, allocated once
size_t            s_ring_head = 0, s_ring_tail = 0;   // monotonic byte counters
SemaphoreHandle_t s_ring_mtx = nullptr;
std::atomic<bool> s_tty_open{false};
std::atomic<int>  s_cols{80};
lv_timer_t       *s_tick = nullptr;
uint32_t          s_jobs_seen = 0;           // sh_jobs_done() already answered with a prompt
constexpr uint32_t kTickMs = 30;
constexpr size_t   kDrainBudget = 8192;      // bytes per tick: the screen keeps up, the UI stays fluid

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
        prog_put(chunk, k);
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

// ---------------------------------------------------------------- tty: requests from the shell

// A program the shell wants run, and a function it wants called on the LVGL thread.
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

// ---------------------------------------------------------------- terminal programs

// Move program output to its sink (the screen, a pipe buffer or a file). True if anything came.
bool prog_drain(void) {
    char chunk[512];
    size_t k;
    bool any = false;
    while ((k = nv_wasm_exec_read(chunk, sizeof chunk)) > 0) {
        if (s_prog.out) sh_sink_write(*s_prog.out, chunk, k);
        else { prog_put(chunk, k); any = true; }
    }
    return any;
}

// The program is over: answer the shell with its exit status.
void prog_finish(int status) {
    s_prog.active = false;
    s_prog.out = nullptr;
    s_prog.in = nullptr;
    s_prog.in_left = 0;
    if (s_prog.requested) {
        s_prog.requested = false;
        s_req.status = status;
        xSemaphoreGive(s_req.done);
    }
}

// Poll the running program: feed piped stdin, move output, notice the end. True if the screen changed.
bool prog_poll(void) {
    if (s_prog.in) {
        while (s_prog.in_left) {
            const size_t w = nv_wasm_exec_write_stdin(s_prog.in, s_prog.in_left);
            if (!w) break;
            s_prog.in += w;
            s_prog.in_left -= w;
        }
        if (!s_prog.in_left) { nv_wasm_exec_close_stdin(); s_prog.in = nullptr; }
    }
    bool changed = prog_drain();
    const nv_wrun_state_t st = nv_wasm_exec_state();
    if (st == NV_WRUN_DONE) {
        changed |= prog_drain();   // the tail may have landed after the first drain
        bool ok = false; uint32_t ms = 0; char err[128] = "";
        nv_wasm_exec_collect(&ok, &ms, err, sizeof err);
        if (!ok && !s_prog.aborted) {
            set_color(-1);
            if (!s_bol) term_putc('\n');
            char b[180];
            lv_snprintf(b, sizeof b, "%s: %s", s_prog.id, err[0] ? err : "failed");
            term_line(b);
            changed = true;
        }
        prog_finish(ok ? 0 : s_prog.aborted ? 130 : 1);
    } else if (st == NV_WRUN_IDLE) {   // collected elsewhere (engine reclaimed)
        prog_finish(1);
    }
    return changed;
}

void prog_stop_retry(void) {
    if (s_prog.retry) { lv_timer_delete(s_prog.retry); s_prog.retry = nullptr; }
    s_prog.wait_t0 = 0;
}

bool prog_start(void);

void prog_retry_cb(lv_timer_t *) {
    prog_start();
    out_flush();
}

// Start the requested program (s_prog.id / args). false = it cannot run (message printed, shell
// answered); true = running, or waiting for the previous app's run to wind down.
bool prog_start(void) {
    nv_wasm_app_t app;
    char b[160];
    if (!nv_wasm_load_manifest(s_prog.id, &app)) {
        prog_stop_retry();
        prog_finish(127);
        return false;
    }
    if (nv_wasm_app_is_game(&app)) {
        prog_stop_retry();
        prog_finish(126);
        return false;
    }
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
        set_color(-1);
        lv_snprintf(b, sizeof b, "%s: %s", s_prog.id, busy ? "another app is running" : err);
        term_line(b);
        prog_finish(1);
        return false;
    }
    prog_stop_retry();
    s_prog.active = true;
    s_prog.aborted = false;
    s_prog.col = 0;
    s_prog.cr = false;
    return true;
}

// The shell's program request, taken once its earlier output is on screen.
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

// ---------------------------------------------------------------- prompt / input

void hist_push(const char *line) {
    if (!line[0]) return;
    if (s_hist_n && !strcmp(s_hist[s_hist_n - 1], line)) { s_hist_pos = s_hist_n; return; }
    if (s_hist_n == kHistMax) {
        memmove(s_hist[0], s_hist[1], sizeof s_hist[0] * (kHistMax - 1));
        s_hist_n--;
    }
    snprintf(s_hist[s_hist_n++], sizeof s_hist[0], "%s", line);
    s_hist_pos = s_hist_n;
}

// A line entered at the shell prompt: echo it after the prompt and hand it to the shell.
void shell_enter(const char *line) {
    term_puts(line);
    term_putc('\n');
    hist_push(line);
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || !sh_run(line)) shell_prompt();   // empty line: straight back to the prompt
    out_flush();
}

void submit_cb(lv_event_t *) {
    if (!s_input) return;
    const char *txt = lv_textarea_get_text(s_input);
    char line[256];
    snprintf(line, sizeof line, "%s", txt ? txt : "");
    if (s_prog.active) {
        if (s_prog.piped) return;   // its input comes from the pipe
        lv_textarea_set_text(s_input, "");
        // A line for the program: echo it after its prompt (a cooked tty echoes), then send it.
        term_puts(line);
        term_putc('\n');
        s_prog.col = 0;
        hist_push(line);
        char in[260];
        const int n = snprintf(in, sizeof in, "%s\n", line);
        const size_t len = n < 0 ? 0 : ((size_t)n < sizeof in ? (size_t)n : sizeof in - 1);
        if (nv_wasm_exec_write_stdin(in, len) < len) term_line("[input dropped: program busy]");
        out_flush();
        return;
    }
    if (sh_busy() || s_req.pending || s_prog.retry) return;   // a command is running: keep the line
    lv_textarea_set_text(s_input, "");
    shell_enter(line);
}

void key_ctrl_c(void) {
    set_color(-1);
    if (s_prog.active) {
        term_line("^C");
        s_prog.aborted = true;
        sh_interrupt();         // the rest of the line (lua x; ls) stops too, as in bash
        nv_wasm_exec_abort();   // the run lands in DONE; prog_poll answers the shell with 130
    } else if (sh_busy()) {
        term_line("^C");
        sh_interrupt();
    } else {
        // At the prompt: abandon the line being typed, as bash does.
        term_puts(s_input ? lv_textarea_get_text(s_input) : "");
        term_line("^C");
        if (s_input) lv_textarea_set_text(s_input, "");
        shell_prompt();
    }
    out_flush();
}

void key_ctrl_d(void) {
    if (!s_prog.active || s_prog.piped) return;
    // Whatever is still in the field goes first, without a newline (Ctrl-D semantics).
    const char *txt = s_input ? lv_textarea_get_text(s_input) : nullptr;
    if (txt && txt[0]) {
        term_puts(txt);
        nv_wasm_exec_write_stdin(txt, strlen(txt));
        lv_textarea_set_text(s_input, "");
    }
    term_line("^D");
    s_prog.col = 0;
    out_flush();
    nv_wasm_exec_close_stdin();
}

// Ctrl-L: clear the screen, keep the line being typed.
void key_ctrl_l(void) {
    term_clear();
    if (!sh_busy() && !s_prog.active) shell_prompt();
    out_flush();
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

// Byte offset of the textarea cursor (it counts characters).
size_t cursor_byte(const char *t, uint32_t chars) {
    size_t b = 0;
    while (t[b] && chars) {
        b++;
        while (t[b] && ((unsigned char)t[b] & 0xC0) == 0x80) b++;
        chars--;
    }
    return b;
}

// Names in columns across the screen (Tab's candidate list), directories in blue.
void print_columns(const char *list) {
    int n = 0, longest = 1;
    for (const char *p = list; *p;) {
        const char *e = strchr(p, '\n');
        const int l = (int)(e ? e - p : (int)strlen(p));
        if (l > longest) longest = l;
        n++;
        p = e ? e + 1 : p + l;
    }
    const int colw = longest + 2;
    int cols = s_cols.load() / colw;
    if (cols < 1) cols = 1;
    const int rows = (n + cols - 1) / cols;
    const char *start[256];
    int k = 0;
    for (const char *p = list; *p && k < 256;) {
        start[k++] = p;
        const char *e = strchr(p, '\n');
        p = e ? e + 1 : p + strlen(p);
    }
    n = k;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const int i = c * rows + r;
            if (i >= n) break;
            const char *e = strchr(start[i], '\n');
            const int l = (int)(e ? e - start[i] : (int)strlen(start[i]));
            const bool dir = l && start[i][l - 1] == '/';
            if (dir) set_color((int32_t)kPath);
            for (int j = 0; j < l; j++) term_putc(start[i][j]);
            set_color(-1);
            if ((c + 1) * rows + r < n) for (int pad = colw - l; pad > 0; pad--) term_putc(' ');
        }
        term_putc('\n');
    }
}

void key_tab(void) {
    if (!s_input || s_prog.active || sh_busy()) return;
    const char *txt = lv_textarea_get_text(s_input);
    const size_t cur = cursor_byte(txt, lv_textarea_get_cursor_pos(s_input));
    static char ins[256];
    static char list[4096];
    const int n = sh_complete(txt, cur, ins, sizeof ins, list, sizeof list);
    if (ins[0]) lv_textarea_add_text(s_input, ins);
    if (n > 1 && list[0]) {
        // The line so far goes up with the list, then a fresh prompt with the same line.
        term_puts(lv_textarea_get_text(s_input));
        term_putc('\n');
        print_columns(list);
        shell_prompt();
        out_flush();
    }
}

// Delete from the cursor back to the start of the previous word (Ctrl-W).
void key_ctrl_w(void) {
    if (!s_input) return;
    const char *t = lv_textarea_get_text(s_input);
    uint32_t pos = lv_textarea_get_cursor_pos(s_input);
    size_t b = cursor_byte(t, pos);
    while (b > 0 && t[b - 1] == ' ') { lv_textarea_delete_char(s_input); b--; t = lv_textarea_get_text(s_input); }
    while (b > 0 && t[b - 1] != ' ') {
        lv_textarea_delete_char(s_input);
        t = lv_textarea_get_text(s_input);
        b = cursor_byte(t, lv_textarea_get_cursor_pos(s_input));
    }
}

void key_ctrl(char c) {
    if (!s_input) return;
    switch (c) {
        case 'c': key_ctrl_c(); break;
        case 'd': key_ctrl_d(); break;
        case 'l': key_ctrl_l(); break;
        case 'a': lv_textarea_set_cursor_pos(s_input, 0); break;
        case 'e': lv_textarea_set_cursor_pos(s_input, LV_TEXTAREA_CURSOR_LAST); break;
        case 'u': {   // delete back to the start of the line
            const uint32_t pos = lv_textarea_get_cursor_pos(s_input);
            for (uint32_t i = 0; i < pos; i++) lv_textarea_delete_char(s_input);
            break;
        }
        case 'k': {   // delete to the end of the line
            const char *t = lv_textarea_get_text(s_input);
            uint32_t chars = 0;
            for (const char *p = t; *p; p++) chars += ((unsigned char)*p & 0xC0) != 0x80;
            const uint32_t pos = lv_textarea_get_cursor_pos(s_input);
            for (uint32_t i = pos; i < chars; i++) lv_textarea_delete_char_forward(s_input);
            break;
        }
        case 'w': key_ctrl_w(); break;
        case 'p': key_hist(-1); break;
        case 'n': key_hist(+1); break;
        default: break;
    }
}

// Hardware / remote keys (nv_ime key hook): history, completion and Ctrl shortcuts.
bool input_key_hook(lv_obj_t *, int key, char ctrl) {
    if (ctrl) { key_ctrl(ctrl); return true; }
    switch (key) {
        case NV_IME_RK_UP:   key_hist(-1); return true;
        case NV_IME_RK_DOWN: key_hist(+1); return true;
        case NV_IME_RK_TAB:  key_tab();    return true;
        default:             return false;
    }
}

// ---------------------------------------------------------------- extra keys

enum : uint8_t { K_TAB, K_CTRL, K_CTRL_C, K_CTRL_D, K_LEFT, K_UP, K_DOWN, K_RIGHT, K_TEXT };
struct ExtraKey { const char *label; uint8_t action; const char *text; };
constexpr ExtraKey kKeys[] = {
    {"Tab", K_TAB, nullptr}, {"Ctrl", K_CTRL, nullptr},
    {"^C", K_CTRL_C, nullptr}, {"^D", K_CTRL_D, nullptr},
    {"\xE2\x86\x90", K_LEFT, nullptr},  {"\xE2\x86\x91", K_UP, nullptr},    // ← ↑
    {"\xE2\x86\x93", K_DOWN, nullptr},  {"\xE2\x86\x92", K_RIGHT, nullptr}, // ↓ →
    {"/", K_TEXT, "/"}, {"-", K_TEXT, "-"}, {"|", K_TEXT, "|"}, {"~", K_TEXT, "~"},
    {">", K_TEXT, ">"},
};
lv_obj_t *s_ctrl_key = nullptr;   // the Ctrl key: armed = the next letter typed is Ctrl+letter
bool      s_ctrl_armed = false;

void ctrl_arm(bool on) {
    s_ctrl_armed = on;
    if (s_ctrl_key) lv_obj_set_style_bg_color(s_ctrl_key, lv_color_hex(on ? kPath : kKey), 0);
    if (s_ctrl_key) lv_obj_set_style_text_color(lv_obj_get_child(s_ctrl_key, 0),
                                                lv_color_hex(on ? kBg : kFg), 0);
}

void extra_key_cb(lv_event_t *e) {
    const ExtraKey *k = (const ExtraKey *)lv_event_get_user_data(e);
    switch (k->action) {
        case K_TAB:    key_tab(); break;
        case K_CTRL:   ctrl_arm(!s_ctrl_armed); break;
        case K_CTRL_C: key_ctrl_c(); break;
        case K_CTRL_D: key_ctrl_d(); break;
        case K_UP:     key_hist(-1); break;
        case K_DOWN:   key_hist(+1); break;
        case K_LEFT:   if (s_input) lv_textarea_cursor_left(s_input);  break;
        case K_RIGHT:  if (s_input) lv_textarea_cursor_right(s_input); break;
        default:       if (s_input) lv_textarea_add_text(s_input, k->text); break;
    }
}

// On-screen keyboard text while Ctrl is armed: a letter becomes Ctrl+letter instead of text.
void input_insert_cb(lv_event_t *e) {
    if (!s_ctrl_armed) return;
    const char *t = (const char *)lv_event_get_param(e);
    if (!t || !t[0] || t[1]) return;
    const char c = (char)((t[0] >= 'A' && t[0] <= 'Z') ? t[0] + 32 : t[0]);
    if (c < 'a' || c > 'z') return;
    lv_textarea_set_insert_replace(s_input, "");
    ctrl_arm(false);
    key_ctrl(c);
}

// ---------------------------------------------------------------- tty timer

void close_cb(void *) { nv_ui_close_app(); }

void tty_tick(lv_timer_t *) {
    bool changed = ring_drain(kDrainBudget);
    if (s_ui.pending) {
        s_ui.pending = false;
        s_ui.fn(s_ui.arg);
        xSemaphoreGive(s_ui.done);
    }
    if (s_req.pending && !s_prog.active && !s_prog.retry && ring_empty()) {
        prog_take_request();
        changed = true;
    }
    if (s_prog.active) changed |= prog_poll();
    // The shell finished a line and everything it wrote is on screen: prompt again.
    if (!sh_busy() && s_jobs_seen != sh_jobs_done() && !s_req.pending && ring_empty()) {
        s_jobs_seen = sh_jobs_done();
        shell_prompt();
        changed = true;
    }
    if (changed) out_flush();
    if (s_exit_req.exchange(false)) lv_async_call(close_cb, nullptr);
}

// ---------------------------------------------------------------- screen

// Tapping anywhere on the terminal puts the caret back on the command line and raises the
// keyboard, as clicking into a terminal window does.
void focus_input(lv_event_t *) {
    if (!s_input) return;
    lv_obj_add_state(s_input, LV_STATE_FOCUSED);
    lv_obj_send_event(s_input, LV_EVENT_FOCUSED, nullptr);
}

// The keyboard's IME lifts the input's parent by padding it; here the whole terminal shrinks
// instead (the prompt line and the extra keys sit right above the keyboard).
void input_focused(lv_event_t *) {
    if (s_inrow) lv_obj_set_style_pad_bottom(s_inrow, 0, 0);
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
    if (s_inrow) lv_obj_set_style_pad_bottom(s_inrow, 0, 0);
    out_flush();   // re-pin the prompt line to the bottom of the smaller view
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
    shell_enter(line);
}

void page_deleted(lv_event_t *) {
    s_tty_open = false;                        // the shell's writes and waits give up
    sh_interrupt();
    nv_event_unsubscribe(NV_EV_IME_VISIBILITY, on_ime, nullptr);
    lv_async_call_cancel(apply_kb_pad, nullptr);
    lv_async_call_cancel(autorun_cb, nullptr);
    nv_ime_hide();
    if (s_tick) { lv_timer_delete(s_tick); s_tick = nullptr; }
    prog_stop_retry();                         // nor does a start still waiting for the engine
    if (s_prog.active) nv_wasm_exec_abort();   // a program never outlives its screen
    prog_finish(130);   // an aborted run parks in DONE; the engine auto-collects it on the next start
    if (s_ui.pending.exchange(false)) xSemaphoreGive(s_ui.done);
    s_req.pending = false;
    ring_reset();
    s_out = s_tail = nullptr;
    s_scrollbox = s_inrow = nullptr;
    s_input = nullptr;
    s_root = nullptr;
    s_ctrl_key = nullptr;
    s_ctrl_armed = false;
    s_kb_h = 0;
}

// Mono text in the terminal's colours on any object (labels, the input field).
void style_text(lv_obj_t *o) {
    lv_obj_set_style_text_font(o, &s_mono, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(kFg), 0);
    lv_obj_set_style_text_line_space(o, -1, 0);   // 21 px rows: box-drawing lines join up
}

// The command line: no box, no background, a block cursor — just text after the prompt.
void style_input(lv_obj_t *ta) {
    style_text(ta);
    static const lv_style_selector_t kStates[] = {
        LV_STATE_DEFAULT, LV_STATE_FOCUSED, LV_STATE_FOCUS_KEY, LV_STATE_EDITED, LV_STATE_PRESSED,
    };
    for (lv_style_selector_t s : kStates) {
        lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, s);
        lv_obj_set_style_border_width(ta, 0, s);
        lv_obj_set_style_outline_width(ta, 0, s);
        lv_obj_set_style_shadow_width(ta, 0, s);
    }
    lv_obj_set_style_radius(ta, 0, 0);
    lv_obj_set_style_pad_all(ta, 0, 0);
    lv_obj_set_style_min_height(ta, 0, 0);
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    // Block cursor in the text colour, the character under it drawn in the background colour.
    const lv_style_selector_t cur = (lv_style_selector_t)LV_PART_CURSOR | LV_STATE_FOCUSED;
    lv_obj_set_style_bg_color(ta, lv_color_hex(kFg), cur);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, cur);
    lv_obj_set_style_text_color(ta, lv_color_hex(kBg), cur);
    lv_obj_set_style_border_width(ta, 0, cur);
    lv_obj_set_style_pad_all(ta, 0, cur);
    lv_obj_set_style_anim_duration(ta, 530, cur);
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
        // Pressing a key must not take focus from the command line (that would drop the keyboard).
        lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(b, extra_key_cb, LV_EVENT_CLICKED, (void *)&k);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, k.label);
        style_text(l);
        lv_obj_center(l);
        if (k.action == K_CTRL) s_ctrl_key = b;
    }
}

bool tty_init_once(void) {
    if (s_ring) return true;
    s_ring = (char *)heap_caps_malloc(kRing, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ring_mtx = xSemaphoreCreateMutex();
    s_req.done = xSemaphoreCreateBinary();
    s_ui.done = xSemaphoreCreateBinary();
    if (!s_ring || !s_ring_mtx || !s_req.done || !s_ui.done) {
        heap_caps_free(s_ring);
        s_ring = nullptr;
        return false;
    }
    return true;
}

void terminal_build(lv_obj_t *content) {
    term_clear();
    s_prog = Prog{};
    s_hist_pos = s_hist_n;
    s_kb_h = 0;
    if (!s_mono_ok) {
        s_mono = nv_font_mono_17;
        s_mono.fallback = &nv_font_14;
        s_mono_ok = true;
    }
    const bool ok = tty_init_once() && sh_start();

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

    s_scrollbox = nv_kit_scroll_column(root);
    lv_obj_set_flex_grow(s_scrollbox, 1);
    lv_obj_set_style_pad_hor(s_scrollbox, 10, 0);
    lv_obj_set_style_pad_ver(s_scrollbox, 6, 0);
    lv_obj_set_style_pad_row(s_scrollbox, 0, 0);
    lv_obj_clear_flag(s_scrollbox, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(s_scrollbox, focus_input, LV_EVENT_CLICKED, nullptr);

    s_out = lv_label_create(s_scrollbox);
    lv_obj_set_width(s_out, lv_pct(100));
    lv_label_set_long_mode(s_out, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(s_out, true);
    style_text(s_out);

    // The prompt line: the open last line of the scrollback, then the command being typed.
    s_inrow = lv_obj_create(s_scrollbox);
    lv_obj_remove_style_all(s_inrow);
    lv_obj_set_size(s_inrow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_inrow, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_clear_flag(s_inrow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_inrow, LV_OBJ_FLAG_CLICKABLE);   // taps fall through to the terminal

    s_tail = lv_label_create(s_inrow);
    lv_obj_set_style_max_width(s_tail, lv_pct(100), 0);
    lv_label_set_long_mode(s_tail, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(s_tail, true);
    lv_label_set_text(s_tail, "");
    style_text(s_tail);

    // URL class: one line, no auto-capitalised first letter (commands and code are lower case).
    // RET_ENTER: Enter runs the line and the keyboard stays up for the next one.
    s_input = nv_kit_textarea_ex(s_inrow, nullptr, true, NV_IME_URL, NV_IME_RET_ENTER);
    style_input(s_input);
    lv_obj_set_width(s_input, 40);
    lv_obj_set_flex_grow(s_input, 1);
    lv_obj_add_event_cb(s_input, submit_cb, LV_EVENT_READY, nullptr);   // keyboard / hardware Enter
    lv_obj_add_event_cb(s_input, input_focused, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(s_input, input_insert_cb, LV_EVENT_INSERT, nullptr);
    nv_ime_set_key_hook(s_input, input_key_hook);

    build_keys(root);
    nv_event_subscribe(NV_EV_IME_VISIBILITY, on_ime, nullptr);

    // Terminal width in cells, for the shell's column layouts.
    lv_obj_update_layout(root);
    const int32_t cell = lv_font_get_glyph_width(&s_mono, 'M', 0);
    const int32_t w = lv_obj_get_content_width(s_scrollbox);
    s_cols = (cell > 0 && w > 0) ? (int)(w / cell) : 80;

    // Login banner, then the prompt.
    char b[120];
    lv_snprintf(b, sizeof b, "Welcome to NucleoOS Anima %s (ESP32-P4 riscv32)", nv_ota_running_version());
    term_line(b);
    term_putc('\n');
    term_line(" * Commands:  help         * Programs:  apps");
    term_line(" * Keys:      Tab completes, \xE2\x86\x91\xE2\x86\x93 history, ^C interrupts");
    term_putc('\n');
    if (!ok) {
        term_line("sh: out of memory - the shell could not start");
        out_flush();
        return;
    }
    ring_reset();
    s_jobs_seen = sh_jobs_done();
    s_tty_open = true;
    s_tick = lv_timer_create(tty_tick, kTickMs, nullptr);
    if (!sh_busy()) shell_prompt();   // else: a line from a previous visit is still unwinding
    out_flush();
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

int term_tty_cols(void) { return s_cols.load(); }

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
const char *term_hist_at(int i) { return (i >= 0 && i < s_hist_n) ? s_hist[i] : ""; }
void        term_hist_clear(void) { s_hist_n = 0; s_hist_pos = 0; }

// ================================================================= app

void terminal_app_register(void) { nv_app_register(&kTerminalApp); }

void terminal_build_with(lv_obj_t *content, const char *command) {
    snprintf(s_autorun, sizeof s_autorun, "%s", command ? command : "");
    terminal_build(content);
}
