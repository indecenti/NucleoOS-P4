// terminal_app — a local command console for NucleoOS Anima. Not a POSIX shell: a small set of
// built-in introspection commands (heap, services, log ring, i2c scan, VFS ls/cat, reboot) that
// mirror what the serial monitor / web console expose, but on the device itself. Output text is
// intentionally hard-coded English (a dev console), so it adds no i18n keys; only the launcher
// label is translated. A fixed scrollback buffer feeds one wrapping label inside a scroll box.
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

constexpr size_t kScrollCap = 6000;   // scrollback bytes; oldest whole lines drop when full
EXT_RAM_BSS_ATTR char s_scroll[kScrollCap];   // cold text buffer -> PSRAM (internal SRAM is scarce)
size_t     s_len = 0;
lv_obj_t  *s_out      = nullptr;   // wrapping label holding the scrollback
lv_obj_t  *s_scrollbox = nullptr;  // its scroll container (auto-scrolled to bottom)
lv_obj_t  *s_input    = nullptr;   // one-line command entry (IME-bound)

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

void term_puts(const char *s) {
    size_t n = strlen(s);
    if (n >= kScrollCap) { s += (n - (kScrollCap - 1)); n = kScrollCap - 1; }
    if (s_len + n + 1 >= kScrollCap) {
        size_t drop = (s_len + n + 2) - kScrollCap;   // bytes we must free
        while (drop < s_len && s_scroll[drop] != '\n') drop++;  // cut on a line boundary
        if (drop < s_len) drop++;                                // include the newline
        memmove(s_scroll, s_scroll + drop, s_len - drop);
        s_len -= drop;
    }
    memcpy(s_scroll + s_len, s, n);
    s_len += n;
    s_scroll[s_len] = '\0';
}

void term_line(const char *s) { term_puts(s); term_puts("\n"); }

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
}

void cmd_ver(void) {
    char b[96];
    lv_snprintf(b, sizeof b, "NucleoOS Anima  v%s", nv_ota_running_version());
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

// ---------------------------------------------------------------- dispatch

void reboot_timer(lv_timer_t *t) { lv_timer_delete(t); esp_restart(); }

// Split "cmd arg arg" -> cmd token + pointer to the (trimmed) remainder.
void run_command(const char *line) {
    while (*line == ' ') line++;
    if (!*line) return;

    // "clear" replaces the screen instead of appending under a prompt echo.
    if (strcmp(line, "clear") == 0 || strcmp(line, "cls") == 0) {
        s_len = 0; s_scroll[0] = '\0'; out_flush();
        return;
    }

    // Echo the prompt line, then dispatch.
    char echo[160];
    lv_snprintf(echo, sizeof echo, "> %s", line);
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
    else if (strcmp(cmd, "reboot") == 0 || strcmp(cmd, "restart") == 0) {
        term_line("rebooting in 1s...");
        out_flush();
        lv_timer_create(reboot_timer, 1000, nullptr);
        return;
    }
    else {
        char b[96];
        lv_snprintf(b, sizeof b, "unknown: %s  (try 'help')", cmd);
        term_line(b);
    }
    out_flush();
}

void submit_cb(lv_event_t *) {
    if (!s_input) return;
    const char *txt = lv_textarea_get_text(s_input);
    if (!txt || !txt[0]) return;
    run_command(txt);
    lv_textarea_set_text(s_input, "");
    nv_ime_hide();   // reveal output; user taps the field again for the next command
}

void page_deleted(lv_event_t *) {
    nv_ime_hide();
    s_out = nullptr;
    s_scrollbox = nullptr;
    s_input = nullptr;
}

void terminal_build(lv_obj_t *content) {
    s_len = 0; s_scroll[0] = '\0';
    const NvTheme *th = nv_theme_get();

    lv_obj_t *root = lv_obj_create(content);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(root, 12, 0);
    lv_obj_set_style_pad_row(root, 10, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, page_deleted, LV_EVENT_DELETE, nullptr);

    // Output console: a dark-ish surface holding one wrapping monospace-ish label.
    s_scrollbox = nv_kit_scroll_column(root);
    lv_obj_set_flex_grow(s_scrollbox, 1);
    lv_obj_set_style_bg_color(s_scrollbox, th->surface, 0);
    lv_obj_set_style_bg_opa(s_scrollbox, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_scrollbox, 10, 0);

    s_out = lv_label_create(s_scrollbox);
    lv_obj_set_width(s_out, lv_pct(100));
    lv_label_set_long_mode(s_out, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_out, &nv_font_14, 0);
    lv_obj_set_style_text_color(s_out, th->success, 0);   // console-green on surface

    // Command bar: one-line entry (IME return = GO) + Run button.
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_input = nv_kit_textarea_ex(bar, "type a command  (help)", true, NV_IME_TEXT, NV_IME_RET_GO);
    lv_obj_set_flex_grow(s_input, 1);
    lv_obj_add_event_cb(s_input, submit_cb, LV_EVENT_READY, nullptr);   // IME GO key

    lv_obj_t *run = nv_kit_button(bar, LV_SYMBOL_RIGHT, true);
    lv_obj_add_event_cb(run, submit_cb, LV_EVENT_CLICKED, nullptr);

    term_line("NucleoOS Anima terminal");
    term_line("type 'help' for commands");
    out_flush();
}

const NvApp kTerminalApp = {"terminal", "Terminal", &nv_icon_terminal, 1u << 20, terminal_build,
                            NV_STR_APP_TERMINAL, nullptr};

}  // namespace

void terminal_app_register(void) { nv_app_register(&kTerminalApp); }
