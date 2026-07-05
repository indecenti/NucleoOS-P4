// nv_keydeck — remote keyboard + telemetry service. Protocol/threading contract in the header.
//
// One plain-TCP server task: waits for Wi-Fi, advertises _keydeck._tcp over mDNS, accepts a
// single client (latest wins), parses LF-terminated command lines, and pushes a STAT line
// every second. All LVGL work (key injection, toasts) goes through lvgl_port_lock() — the
// task itself owns no LVGL objects. No TLS, no auth (v1, LAN-only): worst case someone on
// the LAN types into a focused field; nothing here reads data back off the device.
#include "nv_keydeck.h"
#include "nv_cpu_load.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "mdns.h"

#include "nv_log.h"
#include "nv_wifi.h"
#include "nv_memory_broker.h"
#include "nv_config.h"
#include "nv_ui.h"
#include "nv_ime.h"

static const char *TAG = "keydeck";

enum {
    KD_PORT       = 5588,
    KD_LINE_MAX   = 160,    // protocol cap; longer lines are dropped, not split
    KD_IDLE_TO_MS = 8000,   // drop a client silent for this long (it PINGs every ~2 s)
    KD_STAT_MS    = 1000,
};

static TaskHandle_t  s_task      = nullptr;
static volatile bool s_client_up = false;

// Optional PIN pairing — DISABLED by default. Set nv_config "keydeck_pin" (numeric string)
// to require `HELLO v1 <name> PIN=<pin>` before any TXT/KEY/PING is honored; wrong or
// missing PIN gets `ERR badpin` and the connection is dropped. Empty (default) = open LAN
// behavior, protocol unchanged.
static char s_pin[16]  = "";
static bool s_authed   = true;   // true whenever no PIN is configured

// ---------------------------------------------------------------- LVGL bridge
// Short lock timeouts: with the LVGL task busy (animation storm), dropping one remote key
// beats stalling the whole server loop.
static void ui_toast(const char *msg)
{
    if (!lvgl_port_lock(200)) return;
    nv_ui_toast(msg);
    lvgl_port_unlock();
}

static bool inject_text(const char *utf8)
{
    if (!lvgl_port_lock(200)) return false;
    const bool ok = nv_ime_inject_text(utf8);
    lvgl_port_unlock();
    return ok;
}

static bool inject_key(nv_ime_remote_key_t key)
{
    if (!lvgl_port_lock(200)) return false;
    const bool ok = nv_ime_inject_key(key);
    lvgl_port_unlock();
    return ok;
}

// ---------------------------------------------------------------- protocol
static void tx(int fd, const char *fmt, ...)
{
    char out[KD_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, sizeof out, fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof out) return;  // truncated = lost '\n' = merged lines: never send
    send(fd, out, (size_t)n, 0);
}

static const struct { const char *name; nv_ime_remote_key_t key; } kKeyNames[] = {
    { "ENTER",     NV_IME_RK_ENTER },
    { "ESC",       NV_IME_RK_ESC },
    { "BACKSPACE", NV_IME_RK_BACKSPACE },
    { "DELETE",    NV_IME_RK_DELETE },
    { "TAB",       NV_IME_RK_TAB },
    { "LEFT",      NV_IME_RK_LEFT },
    { "RIGHT",     NV_IME_RK_RIGHT },
    { "UP",        NV_IME_RK_UP },
    { "DOWN",      NV_IME_RK_DOWN },
};

// Returns false when the client must be dropped (failed PIN handshake).
static bool handle_line(int fd, char *line)
{
    if (!line[0]) return true;

    if (strncmp(line, "HELLO ", 6) == 0 || strcmp(line, "HELLO") == 0) {
        if (s_pin[0]) {
            const char *p = strstr(line, " PIN=");
            if (!p || strcmp(p + 5, s_pin) != 0) {
                tx(fd, "ERR badpin\n");
                NV_LOGW(TAG, "client rejected: bad/missing PIN");
                return false;
            }
            if (!s_authed) ui_toast("KeyDeck keyboard connected");  // paired: announce now
            s_authed = true;
        }
        tx(fd, "WELCOME v1 NucleoOS-Anima nucleov2\n");
        return true;
    }
    if (!s_authed) {           // PIN configured and no valid HELLO yet: nothing else is honored
        tx(fd, "ERR badpin\n");
        return false;
    }

    if (strncmp(line, "TXT ", 4) == 0) {
        if (!inject_text(line + 4)) tx(fd, "ERR nofocus\n");
        return true;
    }
    if (strncmp(line, "KEY ", 4) == 0) {
        char *name = line + 4;
        if (char *sp = strchr(name, ' ')) *sp = '\0';  // v1 ignores the trailing mods field
        for (const auto &k : kKeyNames) {
            if (strcmp(name, k.name) == 0) {
                if (!inject_key(k.key)) tx(fd, "ERR nofocus\n");
                return true;
            }
        }
        tx(fd, "ERR badkey\n");
        return true;
    }
    if (strcmp(line, "PING") == 0) {
        tx(fd, "PONG\n");
        return true;
    }
    tx(fd, "ERR badcmd\n");
    return true;
}

static bool send_stat(int fd)
{
    int cpu0, cpu1;
    nv_cpu_load_get(&cpu0, &cpu1);
    char out[KD_LINE_MAX];
    const int n = snprintf(out, sizeof out,
                           "STAT ps_free=%u ps_total=%u sram_free=%u cpu0=%d cpu1=%d up=%u\n",
                           (unsigned)nv_mem_free_psram(),
                           (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                           (unsigned)nv_mem_free_internal(),
                           cpu0, cpu1,
                           (unsigned)(esp_timer_get_time() / 1000000));
    if (n <= 0 || n >= (int)sizeof out) return false;   // truncated STAT would merge lines
    return send(fd, out, (size_t)n, 0) == n;
}

// ---------------------------------------------------------------- mDNS
static bool mdns_start(void)
{
    if (mdns_init() != ESP_OK) {
        NV_LOGW(TAG, "mDNS init failed — manual-IP connections still work");
        return false;
    }
    mdns_hostname_set("nucleov2");
    mdns_instance_name_set("NucleoOS Anima");
    mdns_txt_item_t txt[] = { { "v", "1" } };
    mdns_service_add("NucleoV2 KeyDeck", "_keydeck", "_tcp", KD_PORT, txt, 1);
    NV_LOGI(TAG, "mDNS: _keydeck._tcp on port %d (host nucleov2)", KD_PORT);
    return true;
}

// ---------------------------------------------------------------- server task
static void drop_client(int *cli, const char *why)
{
    if (*cli < 0) return;
    close(*cli);
    *cli = -1;
    s_client_up = false;
    NV_LOGI(TAG, "client dropped (%s)", why);
    if (s_authed) ui_toast("KeyDeck disconnected");  // rejected PIN attempts stay silent
}

static void keydeck_task(void *)
{
    bool mdns_up = false;

    for (;;) {
        while (nv_wifi_get_state() != NV_WIFI_CONNECTED)
            vTaskDelay(pdMS_TO_TICKS(1000));
        if (!mdns_up) mdns_up = mdns_start();  // once; mDNS follows netif up/down by itself

        const int lis = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (lis < 0) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
        const int one = 1;
        setsockopt(lis, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in addr = {};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(KD_PORT);
        if (bind(lis, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(lis, 1) < 0) {
            NV_LOGE(TAG, "bind/listen on %d failed (errno %d)", KD_PORT, errno);
            close(lis);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        NV_LOGI(TAG, "listening on TCP %d", KD_PORT);

        int     cli = -1;
        char    line[KD_LINE_MAX];
        size_t  llen = 0;
        bool    overflow = false;
        int64_t last_rx = 0, last_stat = 0;

        while (nv_wifi_get_state() == NV_WIFI_CONNECTED) {
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(lis, &rf);
            if (cli >= 0) FD_SET(cli, &rf);
            const int maxfd = (cli > lis ? cli : lis) + 1;
            struct timeval tv = { 0, 250 * 1000 };
            select(maxfd, &rf, nullptr, nullptr, &tv);
            const int64_t now = esp_timer_get_time() / 1000;

            if (FD_ISSET(lis, &rf)) {
                const int nc = accept(lis, nullptr, nullptr);
                if (nc >= 0) {
                    if (cli >= 0) drop_client(&cli, "replaced");  // latest wins
                    cli = nc;
                    llen = 0; overflow = false;
                    last_rx = now; last_stat = 0;                 // STAT immediately
                    const struct timeval sto = { 0, 500 * 1000 }; // never wedge on a dead peer
                    setsockopt(cli, SOL_SOCKET, SO_SNDTIMEO, &sto, sizeof sto);
                    // PIN pairing (off unless "keydeck_pin" is set): re-read per connection so a
                    // change in Settings applies to the NEXT client without restarting the task.
                    nv_config_get_str("keydeck_pin", "", s_pin, sizeof s_pin);
                    s_authed = (s_pin[0] == '\0');
                    s_client_up = true;
                    NV_LOGI(TAG, "client connected%s", s_authed ? "" : " (awaiting PIN)");
                    if (s_authed) ui_toast("KeyDeck keyboard connected");
                }
            }

            if (cli >= 0 && FD_ISSET(cli, &rf)) {
                char tmp[64];
                const int n = recv(cli, tmp, sizeof tmp, 0);
                if (n <= 0) {
                    drop_client(&cli, "closed");
                } else {
                    last_rx = now;
                    for (int i = 0; i < n; i++) {
                        const char c = tmp[i];
                        if (c == '\n') {
                            line[llen] = '\0';
                            if (llen && line[llen - 1] == '\r') line[llen - 1] = '\0';
                            const bool keep = overflow || handle_line(cli, line);
                            llen = 0; overflow = false;
                            if (!keep) { drop_client(&cli, "auth failed"); break; }
                        } else if (llen < sizeof line - 1) {
                            line[llen++] = c;
                        } else {
                            overflow = true;     // oversized line: swallow to next LF
                        }
                    }
                }
            }

            if (cli >= 0 && now - last_rx > KD_IDLE_TO_MS)
                drop_client(&cli, "idle timeout");
            if (cli >= 0 && s_authed && now - last_stat >= KD_STAT_MS) {
                if (send_stat(cli)) last_stat = now;
                else drop_client(&cli, "send failed");
            }
        }

        drop_client(&cli, "wifi down");
        close(lis);
        NV_LOGI(TAG, "Wi-Fi down — server parked");
    }
}

// ---------------------------------------------------------------- public API
void nv_keydeck_init(void)
{
    if (s_task) return;
    xTaskCreateWithCaps(keydeck_task, "keydeck", 4096, nullptr, 4, &s_task,
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // stack in PSRAM
    NV_LOGI(TAG, "service task started (waiting for Wi-Fi)");
}

bool nv_keydeck_client_connected(void)
{
    return s_client_up;
}
