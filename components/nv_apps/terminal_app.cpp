// terminal_app — a local command console for NucleoOS Anima. Not a POSIX shell: a small set of
// built-in introspection commands (heap, services, log ring, i2c scan, VFS ls/cat, reboot) that
// mirror what the serial monitor / web console expose, but on the device itself — plus a launcher
// for terminal programs: any installed WASI app whose manifest says "console": true (Lua,
// JavaScript, SQLite, ... from the Store) runs here with its command line as argv, what the user
// types as stdin and its stdout/stderr streamed into the scrollback. Output text is intentionally
// hard-coded English (a dev console), so it adds no i18n keys; only the launcher label is
// translated. A fixed scrollback buffer feeds one wrapping label inside a scroll box.
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

namespace {

constexpr size_t kScrollCap = 8000;   // scrollback bytes; oldest whole lines drop when full
EXT_RAM_BSS_ATTR char s_scroll[kScrollCap];   // cold text buffer -> PSRAM (internal SRAM is scarce)
size_t     s_len = 0;
lv_obj_t  *s_out      = nullptr;   // wrapping label holding the scrollback
lv_obj_t  *s_scrollbox = nullptr;  // its scroll container (auto-scrolled to bottom)
lv_obj_t  *s_input    = nullptr;   // one-line command entry (IME-bound)
lv_obj_t  *s_eof_btn  = nullptr;   // program running: end of input (Ctrl-D)
lv_obj_t  *s_stop_btn = nullptr;   // program running: kill it (Ctrl-C)

// Monospace output (columns line up: tables, prompts): unscii 16 covers ASCII; anything beyond
// (accented Latin-1 letters) falls back to the UI font. A RAM copy, since the fallback is a field.
lv_font_t  s_mono;
bool       s_mono_ok = false;

// The terminal program started from this screen (nv_wasm runs one app at a time).
struct Prog {
    bool        active = false;
    char        id[32] = "";
    lv_timer_t *timer  = nullptr;
    uint8_t     esc    = 0;         // output filter state: 0 text, 1 after ESC, 2 in CSI, 3 in OSC
    uint8_t     col    = 0;         // column of the last output line (tab stops), mod 256
    uint8_t     box[2] = {0, 0};    // pending bytes of a UTF-8 box-drawing character (E2 94/95 ..)
    uint8_t     box_n  = 0;
};
Prog s_prog;
constexpr uint32_t kProgPollMs = 50;

// A command to run as soon as the screen is built (a console app's Home tile / Store "Open").
char s_autorun[48] = "";

// Command history (newest last), recalled with the up button.
constexpr int kHistMax = 16;
EXT_RAM_BSS_ATTR char s_hist[kHistMax][160];
int s_hist_n = 0, s_hist_pos = 0;

// ---------------------------------------------------------------- scrollback

void out_flush(void) {
    if (!s_out) return;
    lv_label_set_text(s_out, s_scroll);
    if (s_scrollbox) {
        lv_obj_update_layout(s_scrollbox);
        int32_t bottom = lv_obj_get_scroll_bottom(s_scrollbox);
        if (bottom > 0) lv_obj_scroll_by(s_scrollbox, 0, -bottom, LV_ANIM_OFF);
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

void term_puts(const char *s) {
    size_t n = strlen(s);
    if (n >= kScrollCap) { s += (n - (kScrollCap - 1)); n = kScrollCap - 1; }
    make_room(n);
    memcpy(s_scroll + s_len, s, n);
    s_len += n;
    s_scroll[s_len] = '\0';
}

void term_line(const char *s) { term_puts(s); term_puts("\n"); }

void term_putc(char c) {
    make_room(1);
    s_scroll[s_len++] = c;
    s_scroll[s_len] = '\0';
}

// Box-drawing characters U+2500..U+257F (tables: SQLite's box mode, tree listings) have no
// glyph in the terminal fonts; draw them with ASCII so columns still line up.
char box_ascii(unsigned cp) {
    static const uint16_t kVert[] = { 0x2502, 0x2503, 0x2506, 0x2507, 0x250A, 0x250B, 0x2551,
                                      0x254E, 0x254F, 0x2575, 0x2577, 0x2579, 0x257B, 0x257D, 0x257F };
    static const uint16_t kHorz[] = { 0x2500, 0x2501, 0x2504, 0x2505, 0x2508, 0x2509, 0x2550,
                                      0x254C, 0x254D, 0x2574, 0x2576, 0x2578, 0x257A, 0x257C, 0x257E };
    for (uint16_t v : kVert) if (v == cp) return '|';
    for (uint16_t h : kHorz) if (h == cp) return '-';
    return '+';   // corners, tees, crosses
}

// Program output is a byte stream written for a terminal: drop what a label can't show (ANSI
// escape sequences — colours, cursor moves — carriage returns, bells), expand tabs to 8-column
// stops, apply backspaces and turn box-drawing characters into ASCII. The escape and UTF-8
// states survive chunk boundaries.
void prog_put(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (s_prog.box_n == 1) {                        // after E2: 94/95 starts a box character
            if (c == 0x94 || c == 0x95) { s_prog.box[1] = c; s_prog.box_n = 2; continue; }
            term_putc((char)0xE2);                      // another E2 xx sequence: pass it through
            s_prog.box_n = 0;
        } else if (s_prog.box_n == 2) {
            s_prog.box_n = 0;
            if ((c & 0xC0) == 0x80) {
                term_putc(box_ascii(0x2500u + ((s_prog.box[1] - 0x94u) << 6) + (c & 0x3Fu)));
                s_prog.col++;
                continue;
            }
            term_putc((char)0xE2);                      // malformed: keep the bytes as they were
            term_putc((char)s_prog.box[1]);
        }
        switch (s_prog.esc) {
            case 1:   // after ESC: '[' starts a CSI, ']' an OSC, anything else is a 2-byte sequence
                s_prog.esc = c == '[' ? 2 : c == ']' ? 3 : 0;
                continue;
            case 2:   // CSI parameters until a final byte 0x40..0x7E
                if (c >= 0x40 && c <= 0x7E) s_prog.esc = 0;
                continue;
            case 3:   // OSC until BEL (or ESC \)
                if (c == 0x07) s_prog.esc = 0;
                else if (c == 0x1B) s_prog.esc = 1;
                continue;
            default:
                break;
        }
        if (c == 0x1B) { s_prog.esc = 1; continue; }
        if (c == 0xE2) { s_prog.box_n = 1; continue; }
        if (c == '\n') { term_putc('\n'); s_prog.col = 0; continue; }
        if (c == '\t') {
            do { term_putc(' '); s_prog.col++; } while (s_prog.col % 8);
            continue;
        }
        if (c == '\b') {
            if (s_len && s_scroll[s_len - 1] != '\n') { s_scroll[--s_len] = '\0'; s_prog.col--; }
            continue;
        }
        if (c < 0x20 || c == 0x7F) continue;   // \r, BEL and other controls
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
    term_line("                    input goes to the program; EOF ends input, STOP kills it");
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

void cmd_ls(const char *path) {
    if (!path || !path[0]) path = "/sdcard";
    DIR *d = opendir(path);
    if (!d) { term_line("ls: cannot open"); return; }
    struct dirent *e;
    int c = 0;
    char b[300];
    while ((e = readdir(d)) != nullptr) {
        const bool dir = (e->d_type == DT_DIR);
        lv_snprintf(b, sizeof b, "  %s%s", e->d_name, dir ? "/" : "");
        term_line(b);
        if (++c >= 200) { term_line("  ..."); break; }
    }
    closedir(d);
    if (c == 0) term_line("  (empty)");
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

void set_prog_ui(bool running) {
    if (s_eof_btn)  running ? lv_obj_clear_flag(s_eof_btn, LV_OBJ_FLAG_HIDDEN)
                            : lv_obj_add_flag(s_eof_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_stop_btn) running ? lv_obj_clear_flag(s_stop_btn, LV_OBJ_FLAG_HIDDEN)
                            : lv_obj_add_flag(s_stop_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_input) {
        char ph[64];
        if (running) lv_snprintf(ph, sizeof ph, "input for %s", s_prog.id);
        lv_textarea_set_placeholder_text(s_input, running ? ph : "type a command  (help)");
    }
}

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
    set_prog_ui(false);
}

void prog_poll(lv_timer_t *) {
    bool changed = prog_drain();
    if (nv_wasm_exec_state() == NV_WRUN_DONE) {
        changed |= prog_drain();   // the tail may have landed after the first drain
        bool ok = false; uint32_t ms = 0; char err[128] = "";
        nv_wasm_exec_collect(&ok, &ms, err, sizeof err);
        if (s_len && s_scroll[s_len - 1] != '\n') term_putc('\n');   // a prompt left mid-line
        if (!ok) {
            char b[180];
            lv_snprintf(b, sizeof b, "[%s: %s]", s_prog.id, err[0] ? err : "failed");
            term_line(b);
        }
        prog_end();
        changed = true;
    } else if (nv_wasm_exec_state() == NV_WRUN_IDLE) {   // collected elsewhere (engine reclaimed)
        prog_end();
    }
    if (changed) out_flush();
}

// Start an installed WASI app as a terminal program. false if `cmd` names no installed app.
bool prog_start(const char *cmd, const char *args) {
    nv_wasm_app_t app;
    if (!nv_wasm_load_manifest(cmd, &app)) return false;
    char b[160];
    if (nv_wasm_app_is_game(&app)) {
        lv_snprintf(b, sizeof b, "%s: graphical app - open it from Home", cmd);
        term_line(b);
        return true;
    }
    char err[96] = "";
    nv_wasm_exec_set_console(args);
    if (!nv_wasm_exec_start(&app, err, sizeof err)) {
        lv_snprintf(b, sizeof b, "%s: %s", cmd, !strcmp(err, "busy") ? "another app is running" : err);
        term_line(b);
        return true;
    }
    snprintf(s_prog.id, sizeof s_prog.id, "%s", app.id);
    s_prog.active = true;
    s_prog.esc = 0;
    s_prog.col = 0;
    if (!s_prog.timer) s_prog.timer = lv_timer_create(prog_poll, kProgPollMs, nullptr);
    set_prog_ui(true);
    return true;
}

void eof_cb(lv_event_t *) {
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

void stop_cb(lv_event_t *) {
    if (!s_prog.active) return;
    term_line("^C");
    out_flush();
    nv_wasm_exec_abort();   // the run lands in DONE; prog_poll reports "terminated by user"
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

// Split "cmd arg arg" -> cmd token + pointer to the (trimmed) remainder.
void run_command(const char *line) {
    while (*line == ' ') line++;
    if (!*line) return;
    hist_push(line);

    // "clear" replaces the screen instead of appending under a prompt echo.
    if (strcmp(line, "clear") == 0 || strcmp(line, "cls") == 0) {
        s_len = 0; s_scroll[0] = '\0'; out_flush();
        return;
    }

    // Echo the prompt line, then dispatch.
    char echo[180];
    lv_snprintf(echo, sizeof echo, "$ %s", line);
    term_line(echo);

    char cmd[160];
    strncpy(cmd, line, sizeof cmd - 1);
    cmd[sizeof cmd - 1] = '\0';
    char *sp = strchr(cmd, ' ');
    const char *arg = "";
    if (sp) { *sp = '\0'; arg = sp + 1; while (*arg == ' ') arg++; }

    if      (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) cmd_help();
    else if (strcmp(cmd, "ver") == 0 || strcmp(cmd, "version") == 0) cmd_ver();
    else if (strcmp(cmd, "uptime") == 0) cmd_uptime();
    else if (strcmp(cmd, "date") == 0) cmd_date();
    else if (strcmp(cmd, "temp") == 0) cmd_temp();
    else if (strcmp(cmd, "mem") == 0 || strcmp(cmd, "free") == 0) cmd_mem();
    else if (strcmp(cmd, "df") == 0) cmd_df();
    else if (strcmp(cmd, "ps") == 0 || strcmp(cmd, "services") == 0) cmd_ps();
    else if (strcmp(cmd, "wifi") == 0) cmd_wifi();
    else if (strcmp(cmd, "log") == 0) cmd_log();
    else if (strcmp(cmd, "i2c") == 0 || strcmp(cmd, "i2cdetect") == 0) cmd_i2c();
    else if (strcmp(cmd, "usb") == 0) cmd_usb(arg);
    else if (strcmp(cmd, "bl") == 0) cmd_bl(arg);
    else if (strcmp(cmd, "ls") == 0) cmd_ls(arg);
    else if (strcmp(cmd, "cat") == 0) cmd_cat(arg);
    else if (strcmp(cmd, "echo") == 0) term_line(arg);
    else if (strcmp(cmd, "apps") == 0 || strcmp(cmd, "programs") == 0) cmd_apps();
    else if (strcmp(cmd, "reboot") == 0 || strcmp(cmd, "restart") == 0) {
        term_line("rebooting in 1s...");
        out_flush();
        lv_timer_create(reboot_timer, 1000, nullptr);
        return;
    }
    else if (!prog_start(cmd, arg)) {
        char b[96];
        lv_snprintf(b, sizeof b, "unknown: %s  (try 'help' or 'apps')", cmd);
        term_line(b);
    }
    out_flush();
}

void submit_cb(lv_event_t *) {
    if (!s_input) return;
    const char *txt = lv_textarea_get_text(s_input);
    if (s_prog.active) {
        // A line for the program: echo it after its prompt (a cooked tty echoes), then send it.
        const char *t = txt ? txt : "";
        term_puts(t);
        term_putc('\n');
        s_prog.col = 0;
        if (t[0]) hist_push(t);
        char line[260];
        const int n = snprintf(line, sizeof line, "%s\n", t);
        const size_t len = n < 0 ? 0 : ((size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
        if (nv_wasm_exec_write_stdin(line, len) < len) term_line("[input dropped: program busy]");
        lv_textarea_set_text(s_input, "");
        out_flush();
        return;   // keyboard stays up: the program is waiting for more
    }
    if (!txt || !txt[0]) return;
    char line[160];
    snprintf(line, sizeof line, "%s", txt);
    lv_textarea_set_text(s_input, "");
    run_command(line);
    if (!s_prog.active) nv_ime_hide();   // reveal output; user taps the field again for the next command
}

void hist_cb(lv_event_t *) {
    if (!s_input || !s_hist_n) return;
    s_hist_pos = s_hist_pos > 0 ? s_hist_pos - 1 : s_hist_n - 1;   // wrap to the newest
    lv_textarea_set_text(s_input, s_hist[s_hist_pos]);
}

void autorun_cb(void *) {
    if (!s_input || !s_autorun[0]) return;
    char line[sizeof s_autorun];
    snprintf(line, sizeof line, "%s", s_autorun);
    s_autorun[0] = '\0';
    run_command(line);
}

void page_deleted(lv_event_t *) {
    nv_ime_hide();
    if (s_prog.active) nv_wasm_exec_abort();   // a program never outlives its screen
    prog_end();   // an aborted run parks in DONE; the engine auto-collects it on the next start
    s_out = nullptr;
    s_scrollbox = nullptr;
    s_input = nullptr;
    s_eof_btn = s_stop_btn = nullptr;
}

lv_obj_t *bar_button(lv_obj_t *bar, const char *label, bool primary, lv_event_cb_t cb) {
    lv_obj_t *b = nv_kit_button(bar, label, primary);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    return b;
}

void terminal_build(lv_obj_t *content) {
    s_len = 0; s_scroll[0] = '\0';
    s_prog = Prog{};
    const NvTheme *th = nv_theme_get();
    if (!s_mono_ok) {
        s_mono = lv_font_unscii_16;
        s_mono.fallback = &nv_font_14;
        s_mono_ok = true;
    }

    lv_obj_t *root = lv_obj_create(content);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 12, 0);
    lv_obj_set_style_pad_row(root, 10, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, page_deleted, LV_EVENT_DELETE, nullptr);

    // Output console: a dark-ish surface holding one wrapping monospace label.
    s_scrollbox = nv_kit_scroll_column(root);
    lv_obj_set_flex_grow(s_scrollbox, 1);
    lv_obj_set_style_bg_color(s_scrollbox, th->surface, 0);
    lv_obj_set_style_bg_opa(s_scrollbox, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_scrollbox, 10, 0);

    s_out = lv_label_create(s_scrollbox);
    lv_obj_set_width(s_out, lv_pct(100));
    lv_label_set_long_mode(s_out, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_out, &s_mono, 0);
    lv_obj_set_style_text_color(s_out, th->success, 0);   // console-green on surface

    // Command bar: one-line entry (IME return = GO) + history, Run, and — while a program runs —
    // EOF / STOP.
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // URL class: one line, no auto-capitalised first letter (commands and code are lower case).
    s_input = nv_kit_textarea_ex(bar, "type a command  (help)", true, NV_IME_URL, NV_IME_RET_GO);
    lv_obj_set_flex_grow(s_input, 1);
    lv_obj_add_event_cb(s_input, submit_cb, LV_EVENT_READY, nullptr);   // IME GO / Enter key

    bar_button(bar, LV_SYMBOL_UP, false, hist_cb);
    s_eof_btn  = bar_button(bar, "EOF", false, eof_cb);
    s_stop_btn = bar_button(bar, LV_SYMBOL_STOP, false, stop_cb);
    bar_button(bar, LV_SYMBOL_RIGHT, true, submit_cb);
    set_prog_ui(false);

    term_line("NucleoOS Anima terminal");
    term_line("type 'help' for commands, 'apps' for programs");
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
