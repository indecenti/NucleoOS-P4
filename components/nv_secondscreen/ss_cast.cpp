// NucleoCast — the board as a network display for a browser page (or the helper script).
//
// Port 7070 (http) and 7443 (https, self-signed: browsers need a secure context to capture the
// screen) serve the connection portal page and a WebSocket at /cast. The sender captures a
// screen/window, diffs it against the previous picture and sends only the changed rectangles as
// JPEG tiles; the board hardware-decodes each tile straight into the panel (nv_ss engine).
//
// Wire protocol v1 (all little-endian):
//   board -> sender  text  {"t":"hello","v":1,"w":1024,"h":600,"name":..,"fw":..,"max":bytes}
//                          {"t":"state","s":"pending|ready|waiting|live|paused|denied|busy"}
//                          {"t":"ack","seq":N,"shown":true,"ms":dec}   after a frame's last tile
//                          {"t":"full"}                                 repaint everything please
//                          {"t":"touch","p":[[id,x,y],..]}              panel coordinates
//                          {"t":"trust","token":".."}                   remember this sender
//   sender -> board  text  {"t":"hi","agent":"..","token":"..","touch":bool}
//                    binary 16-byte header + baseline JPEG:
//                          u8 'N' u8 'C' u8 type(1=jpeg) u8 flags(bit0 = last tile of frame)
//                          u16 x u16 y u16 w u16 h (destination; w/h 0 = native size) u32 seq
//
// Tasks: "ss_cast" (listener: accepts, serves pages inline, hands WebSockets over) and
// "ss_castws" (one streaming session at a time). Both have PSRAM stacks and never exit.
#include "ss_internal.h"
#include "ss_net.h"
#include "ss_tls.h"
#include "nv_ss_links.h"

#include "nv_config.h"
#include "nv_log.h"
#include "nv_wifi.h"
#include "nv_eth.h"
#include "nv_mem_attr.h"

#include "cJSON.h"
#include "mdns.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"
#include "lwip/sockets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <strings.h>

extern const char cast_html_start[] asm("_binary_cast_html_start");
extern const char cast_html_end[] asm("_binary_cast_html_end");
extern const char nucleocast_py_start[] asm("_binary_nucleocast_py_start");
extern const char nucleocast_py_end[] asm("_binary_nucleocast_py_end");

namespace {

constexpr const char *TAG = "ss_cast";
constexpr size_t kFrameCap = 512 * 1024;     // max JPEG tile accepted (header excluded)
constexpr int kHdr = 16;
constexpr int kMaxTrusted = 8;

struct Handoff {
    SsConn conn;
    char label[48];
};

QueueHandle_t s_q = nullptr;
SemaphoreHandle_t s_lk = nullptr;     // status fields
SemaphoreHandle_t s_tx = nullptr;     // sends on the active session socket

// status (s_lk)
bool s_listening = false;
bool s_client = false;
char s_client_label[48] = "";
volatile bool s_pending = false;
volatile int s_answer = -1;           // -1 none, 0 deny, 1 allow
char s_new_token[24] = "";            // set by nv_ss_cast_answer(remember) for the worker to send
volatile uint32_t s_frames = 0;

// active session (worker-owned; ops read it under s_tx)
SsConn *s_active = nullptr;
volatile bool s_want_touch = false;
volatile bool s_close_req = false;
volatile bool s_need_full = false;
volatile bool s_dead = false;
volatile bool s_open_kick = false;

struct Lk {
    SemaphoreHandle_t m;
    explicit Lk(SemaphoreHandle_t mm) : m(mm) { xSemaphoreTake(m, portMAX_DELAY); }
    ~Lk() { xSemaphoreGive(m); }
};

// ---------------------------------------------------------------- network info
bool net_ip(char *ip, size_t n, char *iface, size_t in) {
    ip[0] = 0;
    if (iface) iface[0] = 0;
    if (nv_eth_get_state() == NV_ETH_UP) {
        nv_eth_get_ip(ip, n);
        if (ip[0]) { if (iface) snprintf(iface, in, "eth"); return true; }
    }
    nv_wifi_link_t lk;
    if (nv_wifi_get_state() == NV_WIFI_CONNECTED && nv_wifi_get_link(&lk) && lk.ip[0]) {
        snprintf(ip, n, "%s", lk.ip);
        if (iface) snprintf(iface, in, "wifi");
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- trust store
// "ss_trust" = comma-separated 16-hex tokens handed to senders the user chose to remember.
bool trusted(const char *tok) {
    if (!tok || strlen(tok) != 16) return false;
    char list[kMaxTrusted * 17 + 1];
    nv_config_get_str("ss_trust", "", list, sizeof list);
    return strstr(list, tok) != nullptr;
}

void trust_add(const char *tok) {
    char list[kMaxTrusted * 17 + 1];
    nv_config_get_str("ss_trust", "", list, sizeof list);
    // keep the newest kMaxTrusted: drop the oldest (front) when full
    int count = list[0] ? 1 : 0;
    for (char *p = list; *p; p++) if (*p == ',') count++;
    char *start = list;
    while (count >= kMaxTrusted) {
        char *c = strchr(start, ',');
        if (!c) { start = list + strlen(list); break; }
        start = c + 1;
        count--;
    }
    char out[sizeof list + 20];
    snprintf(out, sizeof out, "%s%s%s", start, *start ? "," : "", tok);
    nv_config_set_str("ss_trust", out);
}

// ---------------------------------------------------------------- user-agent label
void ua_label(const char *ua, const char *ip, char *out, size_t n) {
    const char *br = strstr(ua, "Edg/") ? "Edge" : strstr(ua, "OPR/") ? "Opera"
                   : strstr(ua, "Firefox/") ? "Firefox" : strstr(ua, "Chrome/") ? "Chrome"
                   : strstr(ua, "Safari/") ? "Safari" : strstr(ua, "python") ? "NucleoCast" : "";
    const char *os = strstr(ua, "Windows") ? "Windows" : strstr(ua, "Android") ? "Android"
                   : strstr(ua, "CrOS") ? "ChromeOS" : (strstr(ua, "iPhone") || strstr(ua, "iPad")) ? "iOS"
                   : strstr(ua, "Mac OS X") ? "macOS" : strstr(ua, "Linux") ? "Linux" : "";
    snprintf(out, n, "%s%s%s%s%s", ip, br[0] ? " · " : "", br, os[0] ? " · " : "", os);
}

// ---------------------------------------------------------------- engine source ops
void send_text_locked(const char *s) {
    if (s_active && !s_dead && !ss_ws_send_text(*s_active, s)) s_dead = true;
}

void send_text(const char *s) {
    Lk t(s_tx);
    send_text_locked(s);
}

void op_touch(const nv_ss_touch_pt_t *p, int n, void *) {
    if (!s_want_touch) return;
    char b[160];
    int o = snprintf(b, sizeof b, "{\"t\":\"touch\",\"p\":[");
    for (int i = 0; i < n && o < (int)sizeof b - 24; i++)
        o += snprintf(b + o, sizeof b - o, "%s[%u,%d,%d]", i ? "," : "", p[i].id, p[i].x, p[i].y);
    snprintf(b + o, sizeof b - o, "]}");
    send_text(b);
}
void op_pause(void *) { send_text("{\"t\":\"state\",\"s\":\"paused\"}"); }
void op_resume(void *) {
    send_text("{\"t\":\"state\",\"s\":\"live\"}");
    send_text("{\"t\":\"full\"}");
}
void op_stop(bool by_user, void *) {
    if (by_user) s_close_req = true;
    else send_text("{\"t\":\"state\",\"s\":\"waiting\"}");   // app closed: keep the link, wait
}
bool op_alive(void *) { return !s_dead && !s_close_req; }

const nv_ss_source_ops_t kOps = { op_touch, op_pause, op_resume, op_stop, op_alive, nullptr };

// ---------------------------------------------------------------- session
struct Session {
    SsConn &c;
    char label[48];
    uint8_t *buf = nullptr;   // DMA-capable JPEG input (jpeg_alloc_decoder_mem)
    size_t cap = 0;
    bool approved = false;
    bool got_hi = false;
    bool asked_foreground = false;
    bool announced_waiting = false;
    bool announced_occupied = false;
    int64_t last_rx = 0;
    int64_t last_ping = 0;

    explicit Session(SsConn &cc) : c(cc) {}

    void hello(void) {
        const esp_app_desc_t *d = esp_app_get_description();
        char b[200];
        snprintf(b, sizeof b,
                 "{\"t\":\"hello\",\"v\":1,\"w\":%d,\"h\":%d,\"name\":\"NucleoOS\",\"fw\":\"%s\",\"max\":%u,\"ask\":%s}",
                 NV_SS_PANEL_W, NV_SS_PANEL_H, d ? d->version : "?", (unsigned)kFrameCap,
                 nv_ss_cast_ask_enabled() ? "true" : "false");
        send_text(b);
    }

    void on_hi(const char *json) {
        cJSON *j = cJSON_Parse(json);
        if (!j) return;
        const cJSON *tok = cJSON_GetObjectItem(j, "token");
        const cJSON *agent = cJSON_GetObjectItem(j, "agent");
        const cJSON *touch = cJSON_GetObjectItem(j, "touch");
        s_want_touch = cJSON_IsTrue(touch);
        if (cJSON_IsString(agent) && agent->valuestring[0]) {
            // "192.168.0.216 · Chrome · Windows" -> keep the IP, prefer the sender's own name
            char ip[16];
            snprintf(ip, sizeof ip, "%s", c.peer_ip);
            snprintf(label, sizeof label, "%s · %.30s", ip, agent->valuestring);
            Lk l(s_lk);
            snprintf(s_client_label, sizeof s_client_label, "%s", label);
        }
        got_hi = true;
        approved = !nv_ss_cast_ask_enabled() || (cJSON_IsString(tok) && trusted(tok->valuestring));
        cJSON_Delete(j);
        if (approved) {
            send_text("{\"t\":\"state\",\"s\":\"ready\"}");
        } else {
            s_answer = -1;
            s_pending = true;
            send_text("{\"t\":\"state\",\"s\":\"pending\"}");
            ss_request_foreground();   // bring the app up so the user can answer
        }
    }

    // Housekeeping between frames: approval answers, app-open kicks, keepalive.
    bool tick(void) {
        const int64_t now = esp_timer_get_time();
        if (s_close_req) {   // user tapped Disconnect: tell the page not to reconnect by itself
            send_text("{\"t\":\"bye\",\"why\":\"user\"}");
            return false;
        }
        if (s_pending) {
            const int a = s_answer;
            if (a == 0) {
                s_pending = false;
                send_text("{\"t\":\"state\",\"s\":\"denied\"}");
                return false;
            }
            if (a == 1) {
                s_pending = false;
                approved = true;
                char tok[24];
                { Lk l(s_lk); snprintf(tok, sizeof tok, "%s", s_new_token); s_new_token[0] = 0; }
                if (tok[0]) {
                    char b[64];
                    snprintf(b, sizeof b, "{\"t\":\"trust\",\"token\":\"%s\"}", tok);
                    send_text(b);
                }
                send_text("{\"t\":\"state\",\"s\":\"ready\"}");
            }
        }
        if (s_open_kick) {   // app page just opened: a waiting sender can stream now
            s_open_kick = false;
            if (approved) {
                announced_waiting = false;
                send_text("{\"t\":\"state\",\"s\":\"ready\"}");
                send_text("{\"t\":\"full\"}");
            }
        }
        if (s_need_full) { s_need_full = false; send_text("{\"t\":\"full\"}"); }
        if ((now - last_ping) > 10 * 1000000LL) {
            last_ping = now;
            Lk t(s_tx);
            if (!ss_ws_send(c, WS_PING, "nc", 2)) s_dead = true;
        }
        if ((now - last_rx) > 35 * 1000000LL) { NV_LOGW(TAG, "sender silent 35 s: closing"); return false; }
        if (!got_hi && (now - last_rx) > 8 * 1000000LL) return false;
        return !s_dead;
    }

    // One binary message = header + JPEG tile. Payload already split: hdr + JPEG in buf.
    void on_tile(const uint8_t *h, uint32_t jlen) {
        if (h[0] != 'N' || h[1] != 'C' || h[2] != 1) return;
        const bool last = h[3] & 1;
        const int x = h[4] | (h[5] << 8), y = h[6] | (h[7] << 8);
        const int w = h[8] | (h[9] << 8), hh = h[10] | (h[11] << 8);
        const uint32_t seq = h[12] | (h[13] << 8) | (h[14] << 16) | ((uint32_t)h[15] << 24);
        s_frames = s_frames + (last ? 1 : 0);

        bool shown = false;
        if (approved) {
            if (!nv_ss_is_open()) {
                if (!asked_foreground) { asked_foreground = true; ss_request_foreground(); }
                if (!announced_waiting) {
                    announced_waiting = true;
                    send_text("{\"t\":\"state\",\"s\":\"waiting\"}");
                }
            } else {
                const bool fresh = !nv_ss_owner(NV_SS_SRC_CAST);
                if (fresh && nv_ss_begin(NV_SS_SRC_CAST, &kOps, label)) {
                    announced_occupied = false;
                    send_text("{\"t\":\"state\",\"s\":\"live\"}");
                    // The panel starts black: a sender that only sent a dirty tile repaints all.
                    if (!(x == 0 && y == 0 && last)) s_need_full = true;
                } else if (fresh && !announced_occupied) {
                    // Another source (USB / VNC) has the display: the page backs off and retries.
                    announced_occupied = true;
                    send_text("{\"t\":\"state\",\"s\":\"occupied\"}");
                }
                shown = nv_ss_present_jpeg(NV_SS_SRC_CAST, buf, jlen, true, x, y, w, hh);
            }
        }
        if (last) {
            nv_ss_status_t st;
            nv_ss_get_status(&st);
            char b[96];
            snprintf(b, sizeof b, "{\"t\":\"ack\",\"seq\":%u,\"shown\":%s,\"ms\":%.1f}",
                     (unsigned)seq, shown ? "true" : "false", (double)st.dec_ms);
            send_text(b);
        }
    }

    void run(void) {
        jpeg_decode_memory_alloc_cfg_t ic = {};
        ic.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
        buf = (uint8_t *)jpeg_alloc_decoder_mem(kFrameCap, &ic, &cap);
        if (!buf) { send_text("{\"t\":\"state\",\"s\":\"busy\",\"why\":\"memory\"}"); return; }
        last_rx = last_ping = esp_timer_get_time();
        hello();

        uint8_t hdr[kHdr];
        char *text = (char *)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!text) { free(buf); return; }
        // current message assembly (binary may be fragmented across continuation frames)
        uint8_t msg_op = 0;
        size_t msg_len = 0;          // payload bytes of the current message so far
        bool msg_skip = false;

        for (;;) {
            if (!tick()) break;
            SsWsFrame f;
            const int r = ss_ws_read_header(c, f, 5000);
            if (r == -2) continue;       // idle second: loop to tick()
            if (r == 0) break;
            last_rx = esp_timer_get_time();

            if (f.op >= 8) {             // control frames: <=125 bytes, never fragmented
                uint8_t cb[126];
                const size_t n = f.len < sizeof cb ? (size_t)f.len : sizeof cb;
                if (!ss_ws_read_payload(c, f, 0, cb, n)) break;
                if (f.op == WS_CLOSE) { Lk t(s_tx); ss_ws_send(c, WS_CLOSE, cb, n >= 2 ? 2 : 0); break; }
                if (f.op == WS_PING) { Lk t(s_tx); ss_ws_send(c, WS_PONG, cb, n); }
                continue;
            }
            if (f.op != WS_CONT) { msg_op = f.op; msg_len = 0; msg_skip = false; }

            if (msg_op == WS_TEXT) {
                if (msg_len + f.len >= 2047) { msg_skip = true; }
                if (!ss_ws_read_payload(c, f, 0, msg_skip ? nullptr : (uint8_t *)text + msg_len, (size_t)f.len)) break;
                msg_len += (size_t)f.len;
                if (f.fin && !msg_skip) {
                    text[msg_len] = 0;
                    if (strstr(text, "\"hi\"")) on_hi(text);
                    else if (strstr(text, "\"bye\"")) break;
                }
                continue;
            }
            if (msg_op != WS_BIN) {      // unknown: discard
                if (!ss_ws_read_payload(c, f, 0, nullptr, (size_t)f.len)) break;
                continue;
            }
            // binary: first kHdr bytes -> hdr, the rest -> buf (JPEG must start the DMA buffer)
            uint64_t off = 0;
            if (msg_len < kHdr) {
                const size_t take = (size_t)((kHdr - msg_len) < f.len ? (kHdr - msg_len) : f.len);
                if (!ss_ws_read_payload(c, f, 0, hdr + msg_len, take)) break;
                msg_len += take;
                off = take;
            }
            const uint64_t rest = f.len - off;
            if (rest) {
                const size_t jl = msg_len - kHdr;
                if (msg_skip || jl + rest > cap) {
                    msg_skip = true;
                    if (!ss_ws_read_payload(c, f, off, nullptr, (size_t)rest)) break;
                } else if (!ss_ws_read_payload(c, f, off, buf + jl, (size_t)rest)) break;
                msg_len += (size_t)rest;
            }
            if (f.fin) {
                if (msg_len > kHdr && !msg_skip) on_tile(hdr, (uint32_t)(msg_len - kHdr));
                else if (msg_skip) NV_LOGW(TAG, "tile dropped (%u B > %u)", (unsigned)msg_len, (unsigned)cap);
                nv_ss_stat_update(0);
            }
        }
        heap_caps_free(text);
        free(buf);
        buf = nullptr;
    }
};

void worker_task(void *) {
    Handoff h;
    for (;;) {
        if (xQueueReceive(s_q, &h, portMAX_DELAY) != pdTRUE) continue;
        h.conn.set_timeout_ms(1000);
        {
            Lk l(s_lk);
            s_client = true;
            snprintf(s_client_label, sizeof s_client_label, "%s", h.label);
        }
        s_dead = false;
        s_close_req = false;
        s_need_full = false;
        s_open_kick = false;
        s_want_touch = false;
        s_frames = 0;
        { Lk t(s_tx); s_active = &h.conn; }
        NV_LOGI(TAG, "sender connected: %s%s", h.label, h.conn.tls ? " (https)" : "");

        Session s(h.conn);
        snprintf(s.label, sizeof s.label, "%s", h.label);
        s.run();

        s_dead = true;
        nv_ss_end(NV_SS_SRC_CAST, "sender left");
        { Lk t(s_tx); s_active = nullptr; }
        h.conn.close();
        s_pending = false;
        { Lk l(s_lk); s_client = false; s_client_label[0] = 0; }
        NV_LOGI(TAG, "sender disconnected");
    }
}

// ---------------------------------------------------------------- portal / routes
struct PageSub { const char *k; const char *v; };

// Stream the page template with its {{KEY}} placeholders filled; emit(nullptr-safe) receives the
// pieces in order. Returns the total length (emit may be null to just measure).
size_t render_page(const PageSub *subs, int nsubs, bool (*emit)(const char *, size_t, void *), void *ctx) {
    const char *src = cast_html_start;
    size_t src_len = (size_t)(cast_html_end - cast_html_start);
    if (src_len && src[src_len - 1] == 0) src_len--;   // EMBED_TXTFILES appends a NUL
    size_t out_len = 0;
    const char *p = src, *end = src + src_len;
    while (p < end) {
        const char *m = (const char *)memmem(p, (size_t)(end - p), "{{", 2);
        const char *chunk_end = m ? m : end;
        out_len += (size_t)(chunk_end - p);
        if (emit && chunk_end > p && !emit(p, (size_t)(chunk_end - p), ctx)) return 0;
        if (!m) break;
        bool matched = false;
        for (int i = 0; i < nsubs; i++) {
            const size_t kl = strlen(subs[i].k);
            if ((size_t)(end - m) >= kl && memcmp(m, subs[i].k, kl) == 0) {
                const size_t vl = strlen(subs[i].v);
                out_len += vl;
                if (emit && !emit(subs[i].v, vl, ctx)) return 0;
                p = m + kl;
                matched = true;
                break;
            }
        }
        if (!matched) {
            out_len += 2;
            if (emit && !emit("{{", 2, ctx)) return 0;
            p = m + 2;
        }
    }
    return out_len;
}

// Board address for a page that runs from a file: the IP when known, else the mDNS name.
void baked_hosts(char *host, size_t hn, char *tls, size_t tn, bool fixed) {
    char ip[16] = "", iface[8];
    const bool up = net_ip(ip, sizeof ip, iface, sizeof iface);
    snprintf(host, hn, "%s:%d", up ? ip : "nucleov2.local", NV_SS_CAST_PORT);
    snprintf(tls, tn, "%s:%d", up ? ip : "nucleov2.local", NV_SS_CAST_TLS_PORT);
    if (fixed) {   // pad to 21 (longest "255.255.255.255:7443") so the size is IP-independent
        for (size_t l = strlen(host); l < 21 && l + 1 < hn; l++) { host[l] = ' '; host[l + 1] = 0; }
        for (size_t l = strlen(tls); l < 21 && l + 1 < tn; l++) { tls[l] = ' '; tls[l + 1] = 0; }
    }
}

// Page with {{KEY}} placeholders filled per request (the downloaded launcher needs the address
// baked in: it runs from file://, where location.host is empty).
void send_page(SsConn &c, bool as_download, bool tls) {
    char host[32], tlshost[32];
    baked_hosts(host, sizeof host, tlshost, sizeof tlshost, false);
    const esp_app_desc_t *d = esp_app_get_description();
    const PageSub subs[] = {
        {"{{HOST}}", host}, {"{{TLSHOST}}", tlshost}, {"{{FW}}", d ? d->version : "?"},
        {"{{MODE}}", as_download ? "file" : (tls ? "https" : "http")},
    };
    const size_t len = render_page(subs, 4, nullptr, nullptr);
    const char *extra = as_download
        ? "Content-Disposition: attachment; filename=\"NucleoCast.html\"\r\n" : nullptr;
    if (!ss_http_send_head(c, 200, "text/html; charset=utf-8", len, extra)) return;
    render_page(subs, 4, [](const char *p, size_t n, void *cx) { return ((SsConn *)cx)->send_all(p, n); }, &c);
}

// Stream a file from the SD card as a download.
void send_file(SsConn &c, const char *path, const char *name, const char *ctype) {
    FILE *f = fopen(path, "rb");
    if (!f) { ss_http_send(c, 404, "text/plain", "not on the SD card", 18); return; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char extra[128];
    snprintf(extra, sizeof extra, "Content-Disposition: attachment; filename=\"%s\"\r\n", name);
    if (n > 0 && ss_http_send_head(c, 200, ctype, (size_t)n, extra)) {
        constexpr size_t kChunk = 16 * 1024;
        uint8_t *b = (uint8_t *)heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (b) {
            size_t r;
            while ((r = fread(b, 1, kChunk, f)) > 0)
                if (!c.send_all(b, r)) break;
            heap_caps_free(b);
        }
    }
    fclose(f);
}

// First *.exe in the Windows driver folder (the file name carries the driver build date).
bool find_driver(char *path, size_t n, char *name, size_t nn) {
    const char *dir = "/sdcard/nucleos/drivers/windows";
    DIR *d = opendir(dir);
    if (!d) return false;
    bool ok = false;
    while (struct dirent *e = readdir(d)) {
        const size_t l = strlen(e->d_name);
        if (l > 4 && strcasecmp(e->d_name + l - 4, ".exe") == 0) {
            snprintf(path, n, "%s/%s", dir, e->d_name);
            snprintf(name, nn, "%s", e->d_name);
            ok = true;
            break;
        }
    }
    closedir(d);
    return ok;
}

void send_state_json(SsConn &c) {
    nv_ss_status_t st;
    nv_ss_get_status(&st);
    nv_ss_cast_info_t ci;
    nv_ss_cast_info(&ci);
    char b[512];
    const int n = snprintf(b, sizeof b,
        "{\"mode\":%d,\"src\":\"%s\",\"peer\":\"%s\",\"fps\":%.1f,\"kBps\":%.0f,\"dec_ms\":%.1f,"
        "\"updates\":%u,\"open\":%s,\"client\":%s,\"client_label\":\"%s\",\"pending\":%s,"
        "\"frames\":%u,\"tls\":%s,\"last\":\"%s\"}",
        (int)st.mode, nv_ss_src_name(st.src), st.peer, (double)st.fps, (double)st.kbps, (double)st.dec_ms,
        (unsigned)st.updates, nv_ss_is_open() ? "true" : "false", ci.client ? "true" : "false",
        ci.client_label, ci.pending ? "true" : "false", (unsigned)ci.frames,
        ci.tls_ready ? "true" : "false", st.last_reason);
    ss_http_send(c, 200, "application/json", b, (size_t)n, "Access-Control-Allow-Origin: *\r\n");
}

// Handle one accepted connection on the listener task. Returns true when the socket was handed
// to the streaming worker (caller must not close it).
bool handle(SsConn &c, bool tls) {
    if (tls && !ss_tls_wrap(c, 4000)) return false;
    c.set_timeout_ms(3000);
    SsHttpReq r;
    if (!ss_http_read_head(c, r, 4000)) return false;

    if (r.upgrade_ws && strcmp(r.path, "/cast") == 0) {
        bool busy;
        { Lk l(s_lk); busy = s_client; }
        if (busy) {
            const char *m = "{\"error\":\"busy\"}";
            ss_http_send(c, 409, "application/json", m, strlen(m));
            return false;
        }
        if (!ss_ws_accept(c, r)) return false;
        Handoff h;
        h.conn = c;
        ua_label(r.ua, c.peer_ip, h.label, sizeof h.label);
        if (xQueueSend(s_q, &h, 0) != pdTRUE) return false;
        return true;
    }
    if (strcmp(r.method, "GET") != 0) { ss_http_send(c, 400, "text/plain", "GET only", 8); return false; }

    if (!strcmp(r.path, "/") || !strcmp(r.path, "/index.html") || !strcmp(r.path, "/cast")) {
        send_page(c, false, tls);
    } else if (!strcmp(r.path, "/NucleoCast.html")) {
        send_page(c, true, tls);
    } else if (!strcmp(r.path, "/api/state")) {
        send_state_json(c);
    } else if (!strcmp(r.path, "/driver/windows")) {
        char path[160], name[96];
        if (find_driver(path, sizeof path, name, sizeof name)) send_file(c, path, name, "application/octet-stream");
        else ss_http_send(c, 404, "text/plain", "driver missing on SD", 20);
    } else if (!strcmp(r.path, "/nucleocast.py")) {   // the desktop helper, embedded in the firmware
        size_t n = (size_t)(nucleocast_py_end - nucleocast_py_start);
        if (n && nucleocast_py_start[n - 1] == 0) n--;
        ss_http_send(c, 200, "text/x-python; charset=utf-8", nucleocast_py_start, n,
                     "Content-Disposition: attachment; filename=\"nucleocast.py\"\r\n");
    } else {
        ss_http_send(c, 302, "text/plain", "", 0, "Location: /\r\n");
    }
    return false;
}

void mdns_advertise(void) {
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set("nucleov2");
    mdns_txt_item_t txt[] = { {"v", "1"}, {"path", "/cast"} };
    mdns_service_add("NucleoOS Second Screen", "_nucleocast", "_tcp", NV_SS_CAST_PORT, txt, 2);
}

void listener_task(void *) {
    vTaskDelay(pdMS_TO_TICKS(4000));   // let boot finish (SD, Wi-Fi) before generating a cert
    int lfd = -1, tfd = -1;
    bool advertised = false;
    for (;;) {
        char ip[16];
        const bool up = net_ip(ip, sizeof ip, nullptr, 0);
        if (lfd < 0) {
            lfd = ss_listen_tcp(NV_SS_CAST_PORT, 4);
            if (lfd >= 0) { Lk l(s_lk); s_listening = true; }
        }
        if (tfd < 0 && lfd >= 0 && ss_tls_init()) tfd = ss_listen_tcp(NV_SS_CAST_TLS_PORT, 4);
        if (up && !advertised) { mdns_advertise(); advertised = true; }
        if (lfd < 0) { vTaskDelay(pdMS_TO_TICKS(3000)); continue; }

        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(lfd, &rs);
        int mx = lfd;
        if (tfd >= 0) { FD_SET(tfd, &rs); if (tfd > mx) mx = tfd; }
        struct timeval tv = {2, 0};
        const int n = select(mx + 1, &rs, nullptr, nullptr, &tv);
        if (n <= 0) continue;
        for (int which = 0; which < 2; which++) {
            const int fd = which == 0 ? lfd : tfd;
            if (fd < 0 || !FD_ISSET(fd, &rs)) continue;
            struct sockaddr_in a = {};
            socklen_t al = sizeof a;
            const int cfd = accept(fd, (struct sockaddr *)&a, &al);
            if (cfd < 0) continue;
            int one = 1;
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            SsConn c;
            c.fd = cfd;
            inet_ntoa_r(a.sin_addr, c.peer_ip, sizeof c.peer_ip);
            if (!handle(c, which == 1)) c.close();
        }
    }
}

}  // namespace

// ================================================================ internal
void ss_cast_init(void) {
    if (s_q) return;
    s_q = xQueueCreate(1, sizeof(Handoff));
    s_lk = xSemaphoreCreateMutex();
    s_tx = xSemaphoreCreateMutex();
    // Forever tasks, no flash writes of their own (nv_config proxies NVS off PSRAM stacks):
    // PSRAM stacks keep the scarce internal SRAM free. TLS handshakes need a deep stack.
    xTaskCreateWithCaps(listener_task, "ss_cast", 10240, nullptr, 4, nullptr, MALLOC_CAP_SPIRAM);
    xTaskCreateWithCaps(worker_task, "ss_castws", 8192, nullptr, 5, nullptr, MALLOC_CAP_SPIRAM);
}

void ss_cast_on_open(void) { s_open_kick = true; }

size_t ss_cast_render_page(char *out, size_t cap, bool fixed) {
    char host[32], tlshost[32];
    baked_hosts(host, sizeof host, tlshost, sizeof tlshost, fixed);
    // Fixed width: the firmware version is padded too (release builds differ in length).
    const esp_app_desc_t *d = esp_app_get_description();
    char fw[20];
    snprintf(fw, sizeof fw, fixed ? "%-12.12s" : "%s", d ? d->version : "?");
    const PageSub subs[] = {{"{{HOST}}", host}, {"{{TLSHOST}}", tlshost}, {"{{FW}}", fw}, {"{{MODE}}", "file"}};
    struct Sink { char *o; size_t cap, n; } sk = {out, cap, 0};
    return render_page(subs, 4, [](const char *p, size_t n, void *cx) {
        auto *s = (Sink *)cx;
        if (s->o && s->n < s->cap) memcpy(s->o + s->n, p, n < s->cap - s->n ? n : s->cap - s->n);
        s->n += n;
        return true;
    }, &sk);
}

// ================================================================ public
void nv_ss_cast_info(nv_ss_cast_info_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->net_up = net_ip(out->ip, sizeof out->ip, out->iface, sizeof out->iface);
    if (!s_lk) return;
    Lk l(s_lk);
    out->listening = s_listening;
    out->tls_ready = ss_tls_ready();
    out->client = s_client;
    snprintf(out->client_label, sizeof out->client_label, "%s", s_client_label);
    out->pending = s_pending;
    out->frames = s_frames;
}

void nv_ss_cast_answer(bool allow, bool remember) {
    if (!s_pending) return;
    if (allow && remember) {
        uint8_t r[8];
        esp_fill_random(r, sizeof r);
        char tok[24];
        snprintf(tok, sizeof tok, "%02x%02x%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
        trust_add(tok);   // NVS write: we're on the LVGL thread (internal stack)
        Lk l(s_lk);
        snprintf(s_new_token, sizeof s_new_token, "%s", tok);
    }
    s_answer = allow ? 1 : 0;
}

void nv_ss_cast_forget_all(void) { nv_config_set_str("ss_trust", ""); }
bool nv_ss_cast_ask_enabled(void) { return nv_config_get_bool("ss_ask", true); }
void nv_ss_cast_set_ask(bool on) { nv_config_set_bool("ss_ask", on); }
