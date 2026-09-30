// terminal_app — a local command console for NucleoOS Anima. Not a POSIX shell: a small set of
// built-in introspection commands (heap, services, log ring, i2c scan, VFS ls/cat, reboot) that
// mirror what the serial monitor / web console expose, but on the device itself — plus a launcher
// for terminal programs: any installed WASI app whose manifest says "console": true (Lua,
// JavaScript, SQLite, ... from the Store) runs here with its command line as argv, what the user
// types as stdin and its stdout/stderr streamed into the scrollback. Output text is intentionally
// hard-coded English (a dev console), so it adds no i18n keys; only the launcher label is
// translated. It looks and behaves like a Linux terminal: edge-to-edge dark screen, DejaVu Sans
// Mono, a bash-style prompt with the command typed inline after it, ANSI colours from programs
// (recolour spans in the scrollback label) and a row of extra keys (^C ^D arrows symbols).
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_ui_kit.h"   // nv_kit_* + (transitively) nv_ime_hide
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_theme.h"
#include "nv_fonts.h"

#include "nv_service_mgr.h"
#include "nv_memory_broker.h"
#include "nv_log.h"
#include "esp_heap_caps.h"
#include "nv_hal.h"       // nv_hal_i2c_bus() / nv_hal_temp_read() / nv_hal_backlight_set()
#include "nv_ota.h"       // nv_ota_running_version()
#include "nv_time.h"      // nv_time_format() / nv_time_is_synced()
#include "nv_wifi.h"      // nv_wifi_get_link() (read-only status)
#include "nv_sd.h"        // nv_sd_info() (free/total)
#include "nv_config.h"    // usb host/device mode flag
#include "nv_usb_audio.h" // nv_usb_audio_present() (usb status line)
#include "nv_hid_host.h"   // keyboard/mouse presence (usb status line)
#include "nv_wasm.h"      // terminal programs (console WASI apps)
#include "nv_event_bus.h" // NV_EV_IME_VISIBILITY (keyboard up -> terminal shrinks)

#include "lvgl.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_system.h"   // esp_restart()

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>   // atoi
#include <dirent.h>
#include <strings.h>  // strcasecmp

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
constexpr size_t kScrollCap = 16000;  // scrollback bytes; oldest whole lines drop when full
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

// The terminal program started from this screen (nv_wasm runs one app at a time).
struct Prog {
    bool        active = false;
    char        id[32] = "";
    lv_timer_t *timer  = nullptr;
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
    // Start retry (see prog_start): the command waiting for the previous app's run to wind down.
    lv_timer_t *retry  = nullptr;
    uint32_t    wait_t0 = 0;
    char        wait_cmd[32]  = "";
    char        wait_args[256] = "";
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

// The shell prompt, bash style: user@host:path$
void shell_prompt(void) {
    if (!s_bol) term_putc('\n');
    set_color((int32_t)kUser); term_puts("nucleo@anima");
    set_color(-1);             term_putc(':');
    set_color((int32_t)kPath); term_putc('~');
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

// ---------------------------------------------------------------- commands

const char *svc_state_str(nv_service_state_t st) {
    switch (st) {
        case NV_SVC_RUNNING:   return "running";
        case NV_SVC_SUSPENDED: return "suspended";
        default:               return "stopped";
    }
}

void cmd_help(void) {
    term_line("commands:");
    term_line("  help              this list");
    term_line("  ver               firmware / chip");
    term_line("  uptime            time since boot");
    term_line("  date              wall clock (ntp)");
    term_line("  temp              on-die chip temperature");
    term_line("  mem               heap (internal / psram)");
    term_line("  df                SD free / total");
    term_line("  ps                services + state");
    term_line("  wifi              Wi-Fi link status");
    term_line("  log               kernel log ring");
    term_line("  i2c               scan internal I2C bus");
    term_line("  bl <0-100>        set backlight %");
    term_line("  usb [host|device] OTG mode: USB speaker vs second screen");
    term_line("  ls [path]         list dir (default /sdcard)");
    term_line("  cat <file>        print a file");
    term_line("  echo <text>       print text");
    term_line("  clear             wipe the screen");
    term_line("  reboot            restart the device");
    term_line("programs:");
    term_line("  apps              list installed terminal programs");
    term_line("  <program> [args]  run one, e.g. 'lua', 'js -e \"1+1\"', 'sqlite3 notes.db'");
    term_line("                    input goes to the program; ^D ends input, ^C kills it");
    term_line("                    programs see /sdcard/home as '/' (their files live there)");
}

void cmd_ver(void) {
    char b[96];
    lv_snprintf(b, sizeof b, "NucleoOS Anima  v%s  (WASM ABI v%d)", nv_ota_running_version(), NV_WASM_ABI);
    term_line(b);
    term_line("chip: ESP32-P4  RISC-V dual @360MHz  32MB PSRAM");
}

void cmd_uptime(void) {
    uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
    char b[64];
    lv_snprintf(b, sizeof b, "uptime: %ud %02u:%02u:%02u",
                s / 86400u, (s / 3600u) % 24u, (s / 60u) % 60u, s % 60u);
    term_line(b);
}

void cmd_mem(void) {
    char b[80];
    lv_snprintf(b, sizeof b, "internal: %u KB free (largest %u KB)",
                (unsigned)(nv_mem_free_internal() / 1024),
                (unsigned)(nv_mem_largest_internal() / 1024));
    term_line(b);
    lv_snprintf(b, sizeof b, "psram:    %u KB free",
                (unsigned)(nv_mem_free_psram() / 1024));
    term_line(b);
}

void cmd_ps(void) {
    const int n = nv_service_count();
    char b[96];
    lv_snprintf(b, sizeof b, "%d services:", n);
    term_line(b);
    for (int id = 0; id < n; id++) {
        const char *nm = nv_service_name(id);
        if (!nm) continue;
        lv_snprintf(b, sizeof b, "  [%d] %-14s %s", id, nm,
                    svc_state_str(nv_service_state(id)));
        term_line(b);
    }
}

void cmd_log(void) {
    // Transient: allocate in PSRAM only while dumping, not a resident 4 KB in internal .bss.
    constexpr size_t kSnap = 4096;
    char *snap = (char *)heap_caps_malloc(kSnap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snap) { term_line("(oom)"); return; }
    size_t k = nv_log_snapshot(snap, kSnap);
    if (k) term_puts(snap);        // already newline-terminated per entry
    else   term_line("(log ring empty)");
    heap_caps_free(snap);
}

void cmd_i2c(void) {
    i2c_master_bus_handle_t bus = nv_hal_i2c_bus();
    if (!bus) { term_line("i2c: no bus"); return; }
    term_line("i2c scan (0x08-0x77):");
    char b[32];
    int found = 0;
    for (uint16_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(bus, a, 20) == ESP_OK) {
            lv_snprintf(b, sizeof b, "  0x%02X", (unsigned)a);
            term_line(b);
            found++;
        }
    }
    lv_snprintf(b, sizeof b, "%d device(s)", found);
    term_line(b);
}

// Characters (not bytes) in a UTF-8 string: terminal columns for a name.
int utf8_len(const char *s) {
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

// GNU ls style: names sorted, laid out down columns across the terminal width, directories in
// blue. The name table is transient PSRAM.
void cmd_ls(const char *path) {
    if (!path || !path[0]) path = "/sdcard";
    DIR *d = opendir(path);
    if (!d) {
        char b[200];
        lv_snprintf(b, sizeof b, "ls: cannot access '%s': No such file or directory", path);
        term_line(b);
        return;
    }
    constexpr int kMax = 256, kName = 256;   // FAT long names are up to 255 bytes
    struct Ent { char name[kName]; bool dir; };
    auto *ents = (Ent *)heap_caps_malloc(sizeof(Ent) * kMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ents) { closedir(d); term_line("ls: out of memory"); return; }
    int n = 0;
    bool more = false;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (n == kMax) { more = true; break; }
        snprintf(ents[n].name, kName, "%s", e->d_name);
        ents[n].dir = (e->d_type == DT_DIR);
        n++;
    }
    closedir(d);
    qsort(ents, n, sizeof(Ent), [](const void *a, const void *b) {
        return strcasecmp(((const Ent *)a)->name, ((const Ent *)b)->name);
    });
    // Columns from the real terminal width (10 px cells).
    int width = 80;
    if (s_out) {
        const int32_t cell = lv_font_get_glyph_width(&s_mono, 'M', 0);
        const int32_t w = lv_obj_get_content_width(s_out);
        if (cell > 0 && w > 0) width = (int)(w / cell);
    }
    int longest = 1;
    for (int i = 0; i < n; i++) {
        const int l = utf8_len(ents[i].name);
        if (l > longest) longest = l;
    }
    const int colw = longest + 2;
    int cols = width / colw;
    if (cols < 1) cols = 1;
    const int rows = (n + cols - 1) / cols;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const int i = c * rows + r;
            if (i >= n) break;
            if (ents[i].dir) set_color((int32_t)kPath);
            term_puts(ents[i].name);
            set_color(-1);
            const int last = (c + 1) * rows + r >= n;   // no padding after a row's last name
            if (!last) {
                for (int pad = colw - utf8_len(ents[i].name); pad > 0; pad--)
                    term_putc(' ');
            }
        }
        term_putc('\n');
    }
    if (more) term_line("...");
    heap_caps_free(ents);
}

void cmd_cat(const char *path) {
    if (!path || !path[0]) { term_line("cat: need a file"); return; }
    FILE *f = fopen(path, "rb");
    if (!f) { term_line("cat: cannot open"); return; }
    char buf[513];
    size_t total = 0, r;
    while ((r = fread(buf, 1, sizeof buf - 1, f)) > 0) {
        buf[r] = '\0';
        term_puts(buf);
        total += r;
        if (total >= 4096) { term_puts("\n...(truncated)"); break; }
    }
    fclose(f);
    term_puts("\n");
}

void cmd_temp(void) {
    float c;
    char b[48];
    if (nv_hal_temp_read(&c)) snprintf(b, sizeof b, "chip temp: %.1f C", (double)c);
    else                      snprintf(b, sizeof b, "temp: unavailable");
    term_line(b);
}

void cmd_date(void) {
    char t[40];
    nv_time_format(t, sizeof t, "%Y-%m-%d %H:%M:%S");
    char b[80];
    lv_snprintf(b, sizeof b, "%s  (%s)", t,
                nv_time_is_synced() ? "ntp-synced" : "not synced");
    term_line(b);
}

void cmd_df(void) {
    uint64_t total = 0, free = 0;
    if (!nv_sd_info(&total, &free)) { term_line("df: no card mounted"); return; }
    const double tot_mb = (double)total / (1024.0 * 1024.0);
    const double free_mb = (double)free / (1024.0 * 1024.0);
    const int used_pct = total ? (int)(((total - free) * 100ULL) / total) : 0;
    char b[96];
    snprintf(b, sizeof b, "%s: %.0f MB free / %.0f MB  (%d%% used)",
             nv_sd_mount_point(), free_mb, tot_mb, used_pct);
    term_line(b);
}

void cmd_wifi(void) {
    if (!nv_wifi_is_enabled()) { term_line("wifi: off"); return; }
    nv_wifi_link_t lk;
    if (!nv_wifi_get_link(&lk)) { term_line("wifi: on, not connected"); return; }
    char b[96];
    lv_snprintf(b, sizeof b, "ssid:  %s", lk.ssid);            term_line(b);
    lv_snprintf(b, sizeof b, "ip:    %s", lk.ip);              term_line(b);
    lv_snprintf(b, sizeof b, "rssi:  %d dBm  ch %u  %s", (int)lk.rssi,
                (unsigned)lk.channel, nv_wifi_gen_label(lk.gen));
    term_line(b);
}

void cmd_usb(const char *arg) {
    const bool host = nv_config_get_bool("usbhost", true);
    if (!arg || !arg[0]) {
        char b[96];
        lv_snprintf(b, sizeof b, "usb mode: %s", host ? "host (audio)" : "device (second screen)");
        term_line(b);
        if (host) {
            lv_snprintf(b, sizeof b, "bus: %d device(s), UAC speaker: %s",
                        nv_usb_audio_bus_devices(), nv_usb_audio_present() ? "connected" : "none");
            term_line(b);
            lv_snprintf(b, sizeof b, "keyboard: %s, mouse: %s",
                        nv_hid_host_keyboard_present() ? "yes" : "no",
                        nv_hid_host_mouse_present() ? "yes" : "no");
            term_line(b);
            if (nv_usb_audio_bus_devices() == 0)
                term_line("0 devices = no data link: wrong port or charge-only adapter");
        }
        term_line("usage: usb host | usb device   (reboot applies)");
        return;
    }
    if (strcmp(arg, "host") == 0)        nv_config_set_bool("usbhost", true);
    else if (strcmp(arg, "device") == 0) nv_config_set_bool("usbhost", false);
    else { term_line("usb: 'host' or 'device'"); return; }
    term_line("saved. 'reboot' to apply.");
}

void cmd_bl(const char *arg) {
    if (!arg || !arg[0]) { term_line("bl: usage 'bl 0-100'"); return; }
    int pct = atoi(arg);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    nv_hal_backlight_set(pct);
    char b[40];
    lv_snprintf(b, sizeof b, "backlight -> %d%%", pct);
    term_line(b);
}

// Installed terminal programs (manifest "console": true). The scan buffer is transient PSRAM.
void cmd_apps(void) {
    constexpr int kMax = 64;
    auto *apps = (nv_wasm_app_t *)heap_caps_malloc(sizeof(nv_wasm_app_t) * kMax,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!apps) { term_line("(oom)"); return; }
    const int n = nv_wasm_scan(apps, kMax);
    int shown = 0;
    char b[96];
    for (int i = 0; i < n; i++) {
        if (!apps[i].console) continue;
        if (!shown++) term_line("terminal programs:");
        lv_snprintf(b, sizeof b, "  %-12s %s  v%s", apps[i].id, apps[i].name, apps[i].version);
        term_line(b);
    }
    if (!shown) term_line("no terminal programs installed - get Lua, JavaScript or SQLite from the Store");
    heap_caps_free(apps);
}

// ---------------------------------------------------------------- terminal programs

// Move program output into the scrollback. Returns true if anything arrived.
bool prog_drain(void) {
    char chunk[512];
    size_t k;
    bool any = false;
    while ((k = nv_wasm_exec_read(chunk, sizeof chunk)) > 0) {
        prog_put(chunk, k);
        any = true;
    }
    return any;
}

void prog_end(void) {
    if (s_prog.timer) { lv_timer_delete(s_prog.timer); s_prog.timer = nullptr; }
    s_prog.active = false;
}

// Back at the shell: plain colour, then a fresh prompt.
void prog_finish(void) {
    prog_end();
    set_color(-1);
    shell_prompt();
}

void prog_poll(lv_timer_t *) {
    bool changed = prog_drain();
    if (nv_wasm_exec_state() == NV_WRUN_DONE) {
        changed |= prog_drain();   // the tail may have landed after the first drain
        bool ok = false; uint32_t ms = 0; char err[128] = "";
        nv_wasm_exec_collect(&ok, &ms, err, sizeof err);
        set_color(-1);
        if (!s_bol) term_putc('\n');   // a prompt left mid-line
        if (!ok) {
            char b[180];
            lv_snprintf(b, sizeof b, "%s: %s", s_prog.id, err[0] ? err : "failed");
            term_line(b);
        }
        prog_finish();
        changed = true;
    } else if (nv_wasm_exec_state() == NV_WRUN_IDLE) {   // collected elsewhere (engine reclaimed)
        prog_finish();
        changed = true;
    }
    if (changed) out_flush();
}

void prog_stop_retry(void) {
    if (s_prog.retry) { lv_timer_delete(s_prog.retry); s_prog.retry = nullptr; }
    s_prog.wait_t0 = 0;
}

bool prog_start(const char *cmd, const char *args);

void prog_retry_cb(lv_timer_t *) {
    char cmd[sizeof s_prog.wait_cmd], args[sizeof s_prog.wait_args];
    snprintf(cmd, sizeof cmd, "%s", s_prog.wait_cmd);
    snprintf(args, sizeof args, "%s", s_prog.wait_args);
    prog_start(cmd, args);
    if (!s_prog.active && !s_prog.retry) shell_prompt();   // gave up: back to the shell
    out_flush();
}

// Start an installed WASI app as a terminal program. false if `cmd` names no installed app.
bool prog_start(const char *cmd, const char *args) {
    nv_wasm_app_t app;
    if (!nv_wasm_load_manifest(cmd, &app)) { prog_stop_retry(); return false; }
    char b[160];
    if (nv_wasm_app_is_game(&app)) {
        prog_stop_retry();
        lv_snprintf(b, sizeof b, "%s: graphical app - open it from Home", cmd);
        term_line(b);
        return true;
    }
    char err[96] = "";
    nv_wasm_exec_set_console(args);
    if (!nv_wasm_exec_start(&app, err, sizeof err)) {
        const bool busy = !strcmp(err, "busy");
        // Opened from Home straight out of another WASM app: that app's run was aborted by its
        // teardown a moment ago and is still unwinding. Retry quietly until it lands rather than
        // failing a console tile with "another app is running".
        if (busy && nv_wasm_exec_stopping()) {
            if (!s_prog.wait_t0) s_prog.wait_t0 = lv_tick_get();
            if (lv_tick_elaps(s_prog.wait_t0) < kProgStartWaitMs) {
                snprintf(s_prog.wait_cmd, sizeof s_prog.wait_cmd, "%s", cmd);
                snprintf(s_prog.wait_args, sizeof s_prog.wait_args, "%s", args ? args : "");
                if (!s_prog.retry) s_prog.retry = lv_timer_create(prog_retry_cb, kProgPollMs, nullptr);
                return true;
            }
        }
        if (s_prog.retry) NV_LOGE("term", "'%s': previous run did not stop within %u ms", cmd,
                                  (unsigned)kProgStartWaitMs);
        prog_stop_retry();
        lv_snprintf(b, sizeof b, "%s: %s", cmd, busy ? "another app is running" : err);
        term_line(b);
        return true;
    }
    prog_stop_retry();
    snprintf(s_prog.id, sizeof s_prog.id, "%s", app.id);
    s_prog.active = true;
    s_prog.esc = 0;
    s_prog.col = 0;
    s_prog.cr = false;
    s_prog.fg = -1; s_prog.fg_rgb = false; s_prog.bold = false;
    if (!s_prog.timer) s_prog.timer = lv_timer_create(prog_poll, kProgPollMs, nullptr);
    return true;
}

// ---------------------------------------------------------------- dispatch

void reboot_timer(lv_timer_t *t) { lv_timer_delete(t); esp_restart(); }

void hist_push(const char *line) {
    if (s_hist_n && !strcmp(s_hist[s_hist_n - 1], line)) { s_hist_pos = s_hist_n; return; }
    if (s_hist_n == kHistMax) {
        memmove(s_hist[0], s_hist[1], sizeof s_hist[0] * (kHistMax - 1));
        s_hist_n--;
    }
    snprintf(s_hist[s_hist_n++], sizeof s_hist[0], "%s", line);
    s_hist_pos = s_hist_n;
}

// Run one command line typed at the shell prompt (already echoed after it).
// Split "cmd arg arg" -> cmd token + pointer to the (trimmed) remainder.
void run_command(const char *line) {
    while (*line == ' ') line++;
    if (!*line) return;
    hist_push(line);

    if (strcmp(line, "clear") == 0 || strcmp(line, "cls") == 0) {
        term_clear();
        return;
    }

    char cmd[256];
    strncpy(cmd, line, sizeof cmd - 1);
    cmd[sizeof cmd - 1] = '\0';
    char *sp = strchr(cmd, ' ');
    const char *arg = "";
    if (sp) { *sp = '\0'; arg = sp + 1; while (*arg == ' ') arg++; }

    if      (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) cmd_help();
    else if (strcmp(cmd, "ver") == 0 || strcmp(cmd, "version") == 0 || strcmp(cmd, "uname") == 0) cmd_ver();
    else if (strcmp(cmd, "uptime") == 0) cmd_uptime();
    else if (strcmp(cmd, "date") == 0) cmd_date();
    else if (strcmp(cmd, "temp") == 0) cmd_temp();
    else if (strcmp(cmd, "mem") == 0 || strcmp(cmd, "free") == 0) cmd_mem();
    else if (strcmp(cmd, "df") == 0) cmd_df();
    else if (strcmp(cmd, "ps") == 0 || strcmp(cmd, "services") == 0) cmd_ps();
    else if (strcmp(cmd, "wifi") == 0) cmd_wifi();
    else if (strcmp(cmd, "log") == 0 || strcmp(cmd, "dmesg") == 0) cmd_log();
    else if (strcmp(cmd, "i2c") == 0 || strcmp(cmd, "i2cdetect") == 0) cmd_i2c();
    else if (strcmp(cmd, "usb") == 0) cmd_usb(arg);
    else if (strcmp(cmd, "bl") == 0) cmd_bl(arg);
    else if (strcmp(cmd, "ls") == 0) cmd_ls(arg);
    else if (strcmp(cmd, "cat") == 0) cmd_cat(arg);
    else if (strcmp(cmd, "echo") == 0) term_line(arg);
    else if (strcmp(cmd, "apps") == 0 || strcmp(cmd, "programs") == 0) cmd_apps();
    else if (strcmp(cmd, "reboot") == 0 || strcmp(cmd, "restart") == 0) {
        term_line("rebooting in 1s...");
        lv_timer_create(reboot_timer, 1000, nullptr);
    }
    else if (!prog_start(cmd, arg)) {
        char b[96];
        lv_snprintf(b, sizeof b, "%s: command not found", cmd);
        term_line(b);
    }
}

// A line entered at the shell prompt: echo it after the prompt, run it, prompt again unless a
// program took over the terminal.
void shell_enter(const char *line) {
    term_puts(line);
    term_putc('\n');
    run_command(line);
    if (!s_prog.active && !s_prog.retry) shell_prompt();
    out_flush();
}

void submit_cb(lv_event_t *) {
    if (!s_input) return;
    const char *txt = lv_textarea_get_text(s_input);
    char line[256];
    snprintf(line, sizeof line, "%s", txt ? txt : "");
    lv_textarea_set_text(s_input, "");
    if (s_prog.active) {
        // A line for the program: echo it after its prompt (a cooked tty echoes), then send it.
        term_puts(line);
        term_putc('\n');
        s_prog.col = 0;
        if (line[0]) hist_push(line);
        char in[260];
        const int n = snprintf(in, sizeof in, "%s\n", line);
        const size_t len = n < 0 ? 0 : ((size_t)n < sizeof in ? (size_t)n : sizeof in - 1);
        if (nv_wasm_exec_write_stdin(in, len) < len) term_line("[input dropped: program busy]");
        out_flush();
        return;
    }
    if (s_prog.retry) return;   // a program is about to start
    shell_enter(line);
}

// ---------------------------------------------------------------- extra keys

void key_ctrl_c(void) {
    if (s_prog.active) {
        term_line("^C");
        out_flush();
        nv_wasm_exec_abort();   // the run lands in DONE; prog_poll reports "terminated by user"
        return;
    }
    // At the shell: abandon the line being typed, as bash does.
    term_puts(s_input ? lv_textarea_get_text(s_input) : "");
    term_line("^C");
    if (s_input) lv_textarea_set_text(s_input, "");
    shell_prompt();
    out_flush();
}

void key_ctrl_d(void) {
    if (!s_prog.active) return;
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

enum : uint8_t { K_CTRL_C, K_CTRL_D, K_LEFT, K_UP, K_DOWN, K_RIGHT, K_TEXT };
struct ExtraKey { const char *label; uint8_t action; const char *text; };
constexpr ExtraKey kKeys[] = {
    {"^C", K_CTRL_C, nullptr}, {"^D", K_CTRL_D, nullptr},
    {"\xE2\x86\x90", K_LEFT, nullptr},  {"\xE2\x86\x91", K_UP, nullptr},    // ← ↑
    {"\xE2\x86\x93", K_DOWN, nullptr},  {"\xE2\x86\x92", K_RIGHT, nullptr}, // ↓ →
    {"/", K_TEXT, "/"}, {"-", K_TEXT, "-"}, {"|", K_TEXT, "|"}, {"\"", K_TEXT, "\""},
    {"~", K_TEXT, "~"}, {"*", K_TEXT, "*"},
};

void extra_key_cb(lv_event_t *e) {
    const ExtraKey *k = (const ExtraKey *)lv_event_get_user_data(e);
    switch (k->action) {
        case K_CTRL_C: key_ctrl_c(); break;
        case K_CTRL_D: key_ctrl_d(); break;
        case K_UP:     key_hist(-1); break;
        case K_DOWN:   key_hist(+1); break;
        case K_LEFT:   if (s_input) lv_textarea_cursor_left(s_input);  break;
        case K_RIGHT:  if (s_input) lv_textarea_cursor_right(s_input); break;
        default:       if (s_input) lv_textarea_add_text(s_input, k->text); break;
    }
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
    char line[sizeof s_autorun];
    snprintf(line, sizeof line, "%s", s_autorun);
    s_autorun[0] = '\0';
    shell_enter(line);
}

void page_deleted(lv_event_t *) {
    nv_event_unsubscribe(NV_EV_IME_VISIBILITY, on_ime, nullptr);
    lv_async_call_cancel(apply_kb_pad, nullptr);
    nv_ime_hide();
    prog_stop_retry();                         // nor does a start still waiting for the engine
    if (s_prog.active) nv_wasm_exec_abort();   // a program never outlives its screen
    prog_end();   // an aborted run parks in DONE; the engine auto-collects it on the next start
    s_out = s_tail = nullptr;
    s_scrollbox = s_inrow = nullptr;
    s_input = nullptr;
    s_root = nullptr;
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
    }
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

    build_keys(root);
    nv_event_subscribe(NV_EV_IME_VISIBILITY, on_ime, nullptr);

    // Login banner, then the prompt.
    char b[120];
    lv_snprintf(b, sizeof b, "Welcome to NucleoOS Anima %s (ESP32-P4 riscv32)", nv_ota_running_version());
    term_line(b);
    term_putc('\n');
    term_line(" * Commands:  help");
    term_line(" * Programs:  apps   (Lua, JavaScript, SQLite, BASIC, Zork, ...)");
    term_putc('\n');
    shell_prompt();
    out_flush();
    // A console app's tile: run it once the screen is up (after the open animation's first frame).
    if (s_autorun[0]) lv_async_call(autorun_cb, nullptr);
}

const NvApp kTerminalApp = {"terminal", "Terminal", &nv_icon_terminal, 1u << 20, terminal_build,
                            NV_STR_APP_TERMINAL, nullptr};

}  // namespace

void terminal_app_register(void) { nv_app_register(&kTerminalApp); }

void terminal_build_with(lv_obj_t *content, const char *command) {
    snprintf(s_autorun, sizeof s_autorun, "%s", command ? command : "");
    terminal_build(content);
}
