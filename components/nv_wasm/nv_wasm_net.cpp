// nv_wasm_net — ABI v12: HTTP (any method, headers, body), WebSocket, MQTT and a Home Assistant
// proxy for WASM apps. See nv_wasm.h for the import table; docs/HOME_AUTOMATION_PLAN.md §5.
//
// Design:
//  - Nothing blocks the guest: http_req / ws_open return a handle at once, the app polls it.
//  - One worker task ("wnet", PSRAM stack: it never writes flash, lives forever once started) runs
//    the requests one at a time and services open WebSockets between them.
//  - Destination policy: an app with "net" reaches public addresses, one with "lan" private ones
//    (home network, .local, RFC1918...). The host is resolved and classified before connecting;
//    HTTP redirects are never followed (a public URL can't bounce to a LAN device).
//  - Home Assistant: the URL and long-lived token live in the system (Settings > Home). nv.ha_*
//    adds the token on the host side: the app never sees it. Needs the "ha" permission only.
//  - MQTT goes through the system connection (nv_mqtt_app_*).
//  - Every handle belongs to the current run; nv_wasm_net_cleanup() at collect orphans them and
//    the worker frees them (closing sockets) as soon as it can.
#include "nv_wasm_net.h"
#include "nv_wasm.h"
#include "nv_net_policy.h"
#include "nv_log.h"
#include "nv_config.h"
#include "nv_mqtt.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS

#include <atomic>
#include <new>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "lwip/netdb.h"
#include "cJSON.h"

static const char *TAG = "wnet";

namespace {

// Error codes returned to the guest (mirrored in sdk/include/nucleo_sdk.h NV_NET_E_*).
enum {
    E_PERM = -1,      // the app lacks the permission for this call
    E_ARG = -2,       // malformed spec / URL / header / path
    E_BUSY = -3,      // no free handle (NV_NET_HANDLES per app) or queue full
    E_DEST = -4,      // destination class not allowed (public without "net", private without "lan")
    E_CONNECT = -5,   // DNS / TCP / TLS failure
    E_TOOBIG = -6,    // response over the requested cap
    E_TIMEOUT = -7,
    E_CLOSED = -8,    // WebSocket closed
    E_NOTCONF = -9,   // Home Assistant / MQTT not configured in Settings
};

constexpr int      kHandles     = 4;
constexpr uint32_t kDefMaxResp  = 64 * 1024;
constexpr uint32_t kHardMaxResp = 1024 * 1024;
constexpr uint32_t kMaxBody     = 64 * 1024;
constexpr int      kSpecMax     = 2048;
constexpr int      kHdrMax      = 1536;
constexpr size_t   kWsRx        = 128 * 1024;    // queued incoming messages (PSRAM)
constexpr size_t   kWsTx        = 32 * 1024;
constexpr int      kWsMsgMax    = 96 * 1024;     // one message (HA state dumps can be big)
constexpr int      kWorkerStack = 16 * 1024;

enum Type : uint8_t { T_FREE = 0, T_HTTP, T_WS };
enum State : int8_t { S_PENDING = 0, S_DONE = 1, S_OPEN = 2 };   // + negative = error

struct Handle {
    Type     type;
    bool     orphan;          // run ended: worker frees it
    bool     close_req;       // app closed it (ws: worker closes the socket)
    bool     ha;              // Home Assistant proxy (token added here, no destination check)
    std::atomic<bool> ready;  // guest finished filling it: the worker may start
    std::atomic<int> state;   // State or E_*
    int      status;          // HTTP status
    uint32_t perms;           // run permissions at creation
    char    *url;             // PSRAM, 512
    char    *hdrs;            // PSRAM, "Name: value\r\n"... (kHdrMax)
    // HTTP
    esp_http_client_method_t method;
    uint8_t *body;  uint32_t body_len;
    uint8_t *resp;  uint32_t resp_len, resp_cap, resp_max, read_off;
    int      timeout_ms;
    // WS
    esp_transport_handle_t ws, tcp;
    RingbufHandle_t rx, tx;
    uint8_t *frame;  int frame_len;                  // incoming message being assembled
    bool     ha_authed;
};

NV_PSRAM_BSS Handle s_h[kHandles];   // cold, any task
SemaphoreHandle_t s_mx = nullptr;
TaskHandle_t      s_worker = nullptr;

void lock(void)   { xSemaphoreTake(s_mx, portMAX_DELAY); }
void unlock(void) { xSemaphoreGive(s_mx); }
void kick(void)   { if (s_worker) xTaskNotifyGive(s_worker); }

void *ps_alloc(size_t n) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

// Worker only (or with the handle already unreachable): release everything.
void free_handle(Handle &h)
{
    if (h.ws) { esp_transport_close(h.ws); esp_transport_destroy(h.ws); }
    if (h.tcp) esp_transport_destroy(h.tcp);
    if (h.rx) vRingbufferDeleteWithCaps(h.rx);
    if (h.tx) vRingbufferDeleteWithCaps(h.tx);
    free(h.url); free(h.hdrs); free(h.body); free(h.resp); free(h.frame);
    h.~Handle();
    new (&h) Handle{};
}

// ---------------------------------------------------------------- destination policy
// Resolve `host` and classify it. 0 = allowed, E_DEST / E_CONNECT otherwise.
int check_dest(const char *host, uint32_t perms)
{
    uint32_t ip = 0;
    if (!np_parse_ipv4(host, &ip)) {
        struct addrinfo hints = {};
        hints.ai_family = AF_INET;
        struct addrinfo *res = nullptr;
        if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return E_CONNECT;
        ip = ntohl(reinterpret_cast<struct sockaddr_in *>(res->ai_addr)->sin_addr.s_addr);
        freeaddrinfo(res);
    }
    const bool priv = np_ip_is_private(ip);
    if (priv && !(perms & NV_WPERM_LAN)) return E_DEST;
    if (!priv && !(perms & NV_WPERM_NET)) return E_DEST;
    return 0;
}

// Home Assistant base URL + token from Settings > Home. False when not configured.
bool ha_config(char *base, size_t bn, char *token, size_t tn)
{
    nv_config_get_str("ha_url", "", base, bn);
    nv_config_get_str("ha_token", "", token, tn);
    size_t l = strlen(base);
    while (l && base[l - 1] == '/') base[--l] = '\0';
    return base[0] && token[0];
}

// ---------------------------------------------------------------- handle allocation (guest side)
int alloc_handle(Type t, uint32_t perms)
{
    lock();
    int id = -1;
    for (int i = 0; i < kHandles; i++)
        if (s_h[i].type == T_FREE) { id = i; break; }
    if (id >= 0) {
        Handle &h = s_h[id];
        h.type = t;
        h.perms = perms;
        h.state.store(S_PENDING);
        h.url = (char *)ps_alloc(512);
        h.hdrs = (char *)ps_alloc(kHdrMax);
        if (!h.url || !h.hdrs) { free_handle(h); id = -1; }
    }
    unlock();
    return id;
}

bool handle_ok(int h, Type t)
{
    return h >= 0 && h < kHandles && s_h[h].type == t && !s_h[h].orphan && !s_h[h].close_req;
}

// Headers from a JSON object into "Name: value\r\n" lines. False on a refused header.
bool add_headers(char *out, size_t cap, const cJSON *obj)
{
    if (!obj) return true;
    if (!cJSON_IsObject(obj)) return false;
    size_t o = strlen(out);
    const cJSON *it = nullptr;
    int n = 0;
    cJSON_ArrayForEach(it, obj) {
        if (++n > 16 || !cJSON_IsString(it) || !np_header_ok(it->string, it->valuestring)) return false;
        const int w = snprintf(out + o, cap - o, "%s: %s\r\n", it->string, it->valuestring);
        if (w < 0 || (size_t)w >= cap - o) return false;
        o += (size_t)w;
    }
    return true;
}

// ---------------------------------------------------------------- worker: HTTP
esp_err_t http_evt(esp_http_client_event_t *e)
{
    Handle *h = static_cast<Handle *>(e->user_data);
    if (e->event_id != HTTP_EVENT_ON_DATA || !h || h->state.load() < 0) return ESP_OK;
    const uint32_t n = (uint32_t)e->data_len;
    if (h->resp_len + n > h->resp_max) { h->state.store(E_TOOBIG); return ESP_OK; }
    if (h->resp_len + n > h->resp_cap) {
        uint32_t cap = h->resp_cap ? h->resp_cap : 4096;
        while (cap < h->resp_len + n) cap *= 2;
        if (cap > h->resp_max) cap = h->resp_max;
        void *p = heap_caps_realloc(h->resp, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) { h->state.store(E_TOOBIG); return ESP_OK; }
        h->resp = (uint8_t *)p;
        h->resp_cap = cap;
    }
    memcpy(h->resp + h->resp_len, e->data, n);
    h->resp_len += n;
    return ESP_OK;
}

void run_http(Handle &h)
{
    np_url_t u;
    if (!np_url_parse(h.url, &u) || u.scheme > NP_HTTPS) { h.state.store(E_ARG); return; }
    if (!h.ha) {
        const int d = check_dest(u.host, h.perms);
        if (d) { h.state.store(d); return; }
    }
    esp_http_client_config_t cfg = {};
    cfg.url = h.url;
    cfg.method = h.method;
    cfg.timeout_ms = h.timeout_ms;
    cfg.event_handler = http_evt;
    cfg.user_data = &h;
    cfg.disable_auto_redirect = true;                     // 3xx goes back to the app, never followed
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;
    if (u.scheme == NP_HTTPS) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { h.state.store(E_CONNECT); return; }
    // Our header block is "Name: value\r\n" lines, validated when it was built.
    for (char *line = h.hdrs; line && *line;) {
        char *eol = strstr(line, "\r\n");
        if (!eol) break;
        *eol = '\0';
        char *colon = strchr(line, ':');
        if (colon) {
            *colon = '\0';
            esp_http_client_set_header(c, line, colon + 2);
            *colon = ':';
        }
        *eol = '\r';
        line = eol + 2;
    }
    if (h.ha) {
        char base[160], token[320];
        if (!ha_config(base, sizeof base, token, sizeof token)) { esp_http_client_cleanup(c); h.state.store(E_NOTCONF); return; }
        char auth[340];
        snprintf(auth, sizeof auth, "Bearer %s", token);
        esp_http_client_set_header(c, "Authorization", auth);
        memset(token, 0, sizeof token);
        memset(auth, 0, sizeof auth);
    }
    if (h.body_len) esp_http_client_set_post_field(c, (const char *)h.body, (int)h.body_len);
    const esp_err_t err = esp_http_client_perform(c);
    h.status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (h.state.load() < 0) return;                       // E_TOOBIG from the data handler
    if (err == ESP_ERR_HTTP_EAGAIN || err == ESP_ERR_TIMEOUT) { h.state.store(E_TIMEOUT); return; }
    if (err != ESP_OK) { h.state.store(E_CONNECT); return; }
    h.state.store(S_DONE);
}

// ---------------------------------------------------------------- worker: WebSocket
void ws_fail(Handle &h, int code)
{
    if (h.ws) { esp_transport_close(h.ws); esp_transport_destroy(h.ws); h.ws = nullptr; }
    if (h.tcp) { esp_transport_destroy(h.tcp); h.tcp = nullptr; }
    h.state.store(code);
}

void ws_connect(Handle &h)
{
    np_url_t u;
    if (!np_url_parse(h.url, &u) || u.scheme < NP_WS) { h.state.store(E_ARG); return; }
    if (!h.ha) {
        const int d = check_dest(u.host, h.perms);
        if (d) { h.state.store(d); return; }
    }
    h.tcp = u.scheme == NP_WSS ? esp_transport_ssl_init() : esp_transport_tcp_init();
    if (!h.tcp) { h.state.store(E_CONNECT); return; }
    if (u.scheme == NP_WSS) esp_transport_ssl_crt_bundle_attach(h.tcp, esp_crt_bundle_attach);
    h.ws = esp_transport_ws_init(h.tcp);
    if (!h.ws) { ws_fail(h, E_CONNECT); return; }
    esp_transport_ws_config_t wc = {};
    wc.ws_path = u.path;
    wc.headers = h.hdrs[0] ? h.hdrs : nullptr;
    wc.user_agent = "NucleoOS";
    wc.propagate_control_frames = false;                  // ping/pong/close handled by the transport
    esp_transport_ws_set_config(h.ws, &wc);
    if (esp_transport_connect(h.ws, u.host, u.port, 8000) < 0) { ws_fail(h, E_CONNECT); return; }
    h.rx = xRingbufferCreateWithCaps(kWsRx, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    h.tx = xRingbufferCreateWithCaps(kWsTx, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    h.frame = (uint8_t *)ps_alloc(kWsMsgMax);
    if (!h.rx || !h.tx || !h.frame) { ws_fail(h, E_BUSY); return; }
    h.state.store(S_OPEN);
    NV_LOGI(TAG, "ws %d open (%s)", (int)(&h - s_h), h.ha ? "home assistant" : u.host);
}

void ws_send_now(Handle &h, uint8_t opcode, const void *p, int len)
{
    if (esp_transport_ws_send_raw(h.ws, (ws_transport_opcodes_t)(opcode | WS_TRANSPORT_OPCODES_FIN),
                                  (const char *)p, len, 5000) < 0)
        ws_fail(h, E_CLOSED);
}

// A complete incoming text/binary message. HA proxy: answer auth_required with the token and
// never pass that exchange's secret on; auth_ok / auth_invalid reach the app.
void ws_deliver(Handle &h, int opcode, const uint8_t *p, int len)
{
    if (h.ha && !h.ha_authed) {
        if (len < 512 && memmem(p, (size_t)len, "\"auth_required\"", 15)) {
            char base[160], token[320], msg[400];
            if (!ha_config(base, sizeof base, token, sizeof token)) { ws_fail(h, E_NOTCONF); return; }
            const int n = snprintf(msg, sizeof msg, "{\"type\":\"auth\",\"access_token\":\"%s\"}", token);
            memset(token, 0, sizeof token);
            if (n > 0 && n < (int)sizeof msg) ws_send_now(h, WS_TRANSPORT_OPCODES_TEXT, msg, n);
            memset(msg, 0, sizeof msg);
            return;                                        // the app never sees auth_required
        }
        if (len < 512 && memmem(p, (size_t)len, "\"auth_ok\"", 9)) h.ha_authed = true;
    }
    void *slot = nullptr;
    if (xRingbufferSendAcquire(h.rx, &slot, (size_t)len + 1, 0) != pdTRUE || !slot) {
        NV_LOGW(TAG, "ws %d: app not reading, message of %d B dropped", (int)(&h - s_h), len);
        return;
    }
    uint8_t *b = static_cast<uint8_t *>(slot);
    b[0] = opcode == WS_TRANSPORT_OPCODES_BINARY ? 1 : 0;
    if (len) memcpy(b + 1, p, (size_t)len);
    xRingbufferSendComplete(h.rx, slot);
}

// Read what is waiting (bounded per round so one socket can't starve the others).
void ws_service(Handle &h)
{
    // outgoing: [opcode][payload]
    size_t n = 0;
    uint8_t *b;
    while (h.state.load() == S_OPEN && (b = static_cast<uint8_t *>(xRingbufferReceive(h.tx, &n, 0))) != nullptr) {
        ws_send_now(h, b[0], b + 1, (int)n - 1);
        vRingbufferReturnItem(h.tx, b);
    }
    for (int rounds = 0; rounds < 16 && h.state.load() == S_OPEN; rounds++) {
        const int pr = esp_transport_poll_read(h.ws, 0);
        if (pr == 0) return;
        if (pr < 0) { ws_fail(h, E_CLOSED); return; }
        char chunk[1024];
        const int r = esp_transport_read(h.ws, chunk, sizeof chunk, 1000);
        if (r < 0) { ws_fail(h, E_CLOSED); return; }
        const int op = esp_transport_ws_get_read_opcode(h.ws) & 0x0f;
        if (op == WS_TRANSPORT_OPCODES_CLOSE) { ws_fail(h, E_CLOSED); return; }
        if (r == 0 && op != WS_TRANSPORT_OPCODES_TEXT && op != WS_TRANSPORT_OPCODES_BINARY) continue;   // control
        const int total = esp_transport_ws_get_read_payload_len(h.ws);
        if (h.frame_len + r <= kWsMsgMax) memcpy(h.frame + h.frame_len, chunk, (size_t)r);
        h.frame_len += r;
        if (h.frame_len >= total) {                       // frame complete
            if (h.frame_len <= kWsMsgMax) ws_deliver(h, op, h.frame, h.frame_len);
            else NV_LOGW(TAG, "ws: %d B message over the %d B cap, dropped", h.frame_len, kWsMsgMax);
            h.frame_len = 0;
        }
    }
}

// ---------------------------------------------------------------- worker loop
void worker(void *)
{
    for (;;) {
        bool any_ws = false;
        for (int i = 0; i < kHandles; i++) {
            Handle &h = s_h[i];
            lock();
            const Type t = h.type;
            const bool gone = h.orphan || h.close_req;
            const bool ready = h.ready.load();
            if (t != T_FREE && gone) {
                free_handle(h);
                unlock();
                continue;
            }
            unlock();
            if (!ready) continue;
            if (t == T_HTTP && h.state.load() == S_PENDING) {
                run_http(h);
                if (h.state.load() == S_PENDING) h.state.store(E_CONNECT);
            } else if (t == T_WS) {
                if (h.state.load() == S_PENDING) ws_connect(h);
                if (h.state.load() == S_OPEN) { ws_service(h); any_ws = true; }
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(any_ws ? 20 : 1000));
    }
}

bool ensure_worker(void)
{
    if (s_worker) return true;
    // PSRAM stack: this task never writes flash/NVS and never exits (ENGINEERING_RULES §2).
    if (xTaskCreatePinnedToCoreWithCaps(worker, "wnet", kWorkerStack, nullptr, 3, &s_worker, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_worker = nullptr;
        NV_LOGE(TAG, "worker task create failed");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- guest imports
// Common tail of http_req / ha_req: body copy, queue, kick. Returns the handle.
int32_t submit_http(Handle &h, int id, const void *body, uint32_t blen)
{
    if (blen) {
        if (blen > kMaxBody) { lock(); free_handle(h); unlock(); return E_ARG; }
        h.body = (uint8_t *)heap_caps_malloc(blen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!h.body) { lock(); free_handle(h); unlock(); return E_BUSY; }
        memcpy(h.body, body, blen);
        h.body_len = blen;
    }
    if (!ensure_worker()) { lock(); free_handle(h); unlock(); return E_BUSY; }
    h.ready.store(true);
    kick();
    return id;
}

bool parse_method(const char *m, esp_http_client_method_t *out)
{
    static const struct { const char *n; esp_http_client_method_t v; } k[] = {
        {"GET", HTTP_METHOD_GET}, {"POST", HTTP_METHOD_POST}, {"PUT", HTTP_METHOD_PUT},
        {"PATCH", HTTP_METHOD_PATCH}, {"DELETE", HTTP_METHOD_DELETE}, {"HEAD", HTTP_METHOD_HEAD}};
    for (const auto &e : k) if (!strcmp(m, e.n)) { *out = e.v; return true; }
    return false;
}

// nv.http_req(spec_json, body, body_len) -> handle | E_*
// spec: {"url":"https://...", "method":"POST", "headers":{"Content-Type":"application/json"},
//        "timeout":8000, "max":65536}
int32_t w_http_req(wasm_exec_env_t env, const char *spec, const void *body, uint32_t blen)
{
    const uint32_t perms = nv_wasm_env_perms(env);
    if (!(perms & (NV_WPERM_NET | NV_WPERM_LAN))) return E_PERM;
    if (!spec || strnlen(spec, kSpecMax + 1) > kSpecMax) return E_ARG;
    cJSON *root = cJSON_Parse(spec);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return E_ARG; }
    const cJSON *ju = cJSON_GetObjectItem(root, "url"), *jm = cJSON_GetObjectItem(root, "method");
    const cJSON *jt = cJSON_GetObjectItem(root, "timeout"), *jx = cJSON_GetObjectItem(root, "max");
    np_url_t u;
    esp_http_client_method_t method = HTTP_METHOD_GET;
    if (!cJSON_IsString(ju) || !np_url_parse(ju->valuestring, &u) || u.scheme > NP_HTTPS ||
        (jm && (!cJSON_IsString(jm) || !parse_method(jm->valuestring, &method)))) {
        cJSON_Delete(root);
        return E_ARG;
    }
    const int id = alloc_handle(T_HTTP, perms);
    if (id < 0) { cJSON_Delete(root); return E_BUSY; }
    Handle &h = s_h[id];
    snprintf(h.url, 512, "%s", ju->valuestring);
    h.method = method;
    h.timeout_ms = cJSON_IsNumber(jt) ? (int)jt->valuedouble : 10000;
    h.timeout_ms = h.timeout_ms < 1000 ? 1000 : h.timeout_ms > 30000 ? 30000 : h.timeout_ms;
    h.resp_max = cJSON_IsNumber(jx) && jx->valuedouble > 0 ? (uint32_t)jx->valuedouble : kDefMaxResp;
    if (h.resp_max > kHardMaxResp) h.resp_max = kHardMaxResp;
    const bool hdr_ok = add_headers(h.hdrs, kHdrMax, cJSON_GetObjectItem(root, "headers"));
    cJSON_Delete(root);
    if (!hdr_ok) { lock(); free_handle(h); unlock(); return E_ARG; }
    return submit_http(h, id, body, blen);
}

// nv.ha_req(method, path, body, body_len) -> http handle: {ha_url}{path} with the system token.
int32_t w_ha_req(wasm_exec_env_t env, const char *method, const char *path, const void *body, uint32_t blen)
{
    const uint32_t perms = nv_wasm_env_perms(env);
    if (!(perms & NV_WPERM_HA)) return E_PERM;
    esp_http_client_method_t m;
    if (!method || !parse_method(method, &m) || !np_ha_path_ok(path)) return E_ARG;
    char base[160], token[320];
    const bool conf = ha_config(base, sizeof base, token, sizeof token);
    memset(token, 0, sizeof token);
    if (!conf) return E_NOTCONF;
    const int id = alloc_handle(T_HTTP, perms);
    if (id < 0) return E_BUSY;
    Handle &h = s_h[id];
    h.ha = true;
    h.method = m;
    h.timeout_ms = 10000;
    h.resp_max = 256 * 1024;
    snprintf(h.url, 512, "%s%s", base, path);
    snprintf(h.hdrs, kHdrMax, "Content-Type: application/json\r\n");
    return submit_http(h, id, body, blen);
}

int32_t w_http_state(wasm_exec_env_t, int32_t id)
{
    return handle_ok(id, T_HTTP) ? s_h[id].state.load() : E_ARG;
}

int32_t w_http_status(wasm_exec_env_t, int32_t id)
{
    return handle_ok(id, T_HTTP) && s_h[id].state.load() == S_DONE ? s_h[id].status : 0;
}

// Copy the next part of the response; 0 at the end.
int32_t w_http_read(wasm_exec_env_t, int32_t id, void *buf, uint32_t cap)
{
    if (!handle_ok(id, T_HTTP)) return E_ARG;
    Handle &h = s_h[id];
    const int st = h.state.load();
    if (st != S_DONE) return st < 0 ? st : 0;
    const uint32_t left = h.resp_len - h.read_off;
    const uint32_t n = left < cap ? left : cap;
    if (n) memcpy(buf, h.resp + h.read_off, n);
    h.read_off += n;
    return (int32_t)n;
}

void w_close(wasm_exec_env_t, int32_t id)
{
    if (id < 0 || id >= kHandles) return;
    lock();
    if (s_h[id].type != T_FREE) s_h[id].close_req = true;
    unlock();
    kick();
}

int32_t ws_open_common(uint32_t perms, const char *url, const char *hdr_json, bool ha)
{
    const int id = alloc_handle(T_WS, perms);
    if (id < 0) return E_BUSY;
    Handle &h = s_h[id];
    h.ha = ha;
    snprintf(h.url, 512, "%s", url);
    if (hdr_json && hdr_json[0]) {
        cJSON *root = strnlen(hdr_json, kSpecMax + 1) <= kSpecMax ? cJSON_Parse(hdr_json) : nullptr;
        const bool ok = root && add_headers(h.hdrs, kHdrMax, root);
        cJSON_Delete(root);
        if (!ok) { lock(); free_handle(h); unlock(); return E_ARG; }
    }
    if (!ensure_worker()) { lock(); free_handle(h); unlock(); return E_BUSY; }
    h.ready.store(true);
    kick();
    return id;
}

// nv.ws_open(url, headers_json) -> handle
int32_t w_ws_open(wasm_exec_env_t env, const char *url, const char *hdr_json)
{
    const uint32_t perms = nv_wasm_env_perms(env);
    if (!(perms & NV_WPERM_WS) || !(perms & (NV_WPERM_NET | NV_WPERM_LAN))) return E_PERM;
    np_url_t u;
    if (!url || !np_url_parse(url, &u) || u.scheme < NP_WS) return E_ARG;
    return ws_open_common(perms, url, hdr_json, false);
}

// nv.ha_ws() -> handle on {ha_url}/api/websocket, already authenticated by the host.
int32_t w_ha_ws(wasm_exec_env_t env)
{
    const uint32_t perms = nv_wasm_env_perms(env);
    if (!(perms & NV_WPERM_HA)) return E_PERM;
    char base[160], token[320], url[200];
    const bool conf = ha_config(base, sizeof base, token, sizeof token);
    memset(token, 0, sizeof token);
    if (!conf) return E_NOTCONF;
    const char *rest = !strncmp(base, "https://", 8) ? base + 8 : !strncmp(base, "http://", 7) ? base + 7 : nullptr;
    if (!rest) return E_NOTCONF;
    snprintf(url, sizeof url, "%s://%s/api/websocket", !strncmp(base, "https://", 8) ? "wss" : "ws", rest);
    return ws_open_common(perms, url, nullptr, true);
}

int32_t w_ws_state(wasm_exec_env_t, int32_t id)
{
    if (!handle_ok(id, T_WS)) return E_ARG;
    const int st = s_h[id].state.load();
    return st == S_OPEN ? 1 : st;                          // 0 connecting, 1 open, <0 error/closed
}

int32_t w_ws_send(wasm_exec_env_t, int32_t id, const void *p, uint32_t len, int32_t binary)
{
    if (!handle_ok(id, T_WS)) return E_ARG;
    Handle &h = s_h[id];
    if (h.state.load() != S_OPEN) return h.state.load() < 0 ? h.state.load() : E_BUSY;
    if (len > kWsTx / 2) return E_ARG;
    void *slot = nullptr;
    if (xRingbufferSendAcquire(h.tx, &slot, len + 1, 0) != pdTRUE || !slot) return E_BUSY;
    uint8_t *b = static_cast<uint8_t *>(slot);
    b[0] = binary ? WS_TRANSPORT_OPCODES_BINARY : WS_TRANSPORT_OPCODES_TEXT;
    if (len) memcpy(b + 1, p, len);
    xRingbufferSendComplete(h.tx, slot);
    kick();
    return (int32_t)len;
}

// Next message: its length (copied up to cap, the rest dropped), 0 when none, <0 closed.
int32_t w_ws_recv(wasm_exec_env_t, int32_t id, void *buf, uint32_t cap)
{
    if (!handle_ok(id, T_WS)) return E_ARG;
    Handle &h = s_h[id];
    if (!h.rx) return h.state.load() < 0 ? h.state.load() : 0;
    size_t n = 0;
    uint8_t *b = static_cast<uint8_t *>(xRingbufferReceive(h.rx, &n, 0));
    if (!b) return h.state.load() < 0 ? h.state.load() : 0;
    const uint32_t len = (uint32_t)(n - 1);
    memcpy(buf, b + 1, len < cap ? len : cap);
    vRingbufferReturnItem(h.rx, b);
    return (int32_t)len;
}

// nv.mqtt_sub(filter) -> 0 | E_*
int32_t w_mqtt_sub(wasm_exec_env_t env, const char *filter)
{
    if (!(nv_wasm_env_perms(env) & NV_WPERM_MQTT)) return E_PERM;
    const int r = nv_mqtt_app_sub(filter);
    return r == 0 ? 0 : r == -1 ? E_ARG : r == -2 ? E_NOTCONF : E_BUSY;
}

// nv.mqtt_pub(topic, payload, len, retain) -> 0 | E_*
int32_t w_mqtt_pub(wasm_exec_env_t env, const char *topic, const void *p, uint32_t len, int32_t retain)
{
    if (!(nv_wasm_env_perms(env) & NV_WPERM_MQTT)) return E_PERM;
    const int r = nv_mqtt_app_pub(topic, p, (int)len, retain != 0);
    return r == 0 ? 0 : r == -1 ? E_ARG : r == -2 ? E_NOTCONF : E_BUSY;
}

// nv.mqtt_recv(topic_buf, topic_cap, payload_buf, payload_cap) -> payload length | -1 none
int32_t w_mqtt_recv(wasm_exec_env_t env, char *topic, uint32_t tcap, void *p, uint32_t pcap)
{
    if (!(nv_wasm_env_perms(env) & NV_WPERM_MQTT)) return E_PERM;
    return nv_mqtt_app_recv(topic, tcap, p, pcap);
}

// nv.ha_available() -> 1 when Settings > Home has a Home Assistant URL + token
int32_t w_ha_available(wasm_exec_env_t env)
{
    if (!(nv_wasm_env_perms(env) & NV_WPERM_HA)) return 0;
    char base[160], token[320];
    const bool conf = ha_config(base, sizeof base, token, sizeof token);
    memset(token, 0, sizeof token);
    return conf ? 1 : 0;
}

NativeSymbol s_natives[] = {
    {"http_req",     (void *)w_http_req,     "($*~)i",   nullptr},
    {"http_state",   (void *)w_http_state,   "(i)i",     nullptr},
    {"http_status",  (void *)w_http_status,  "(i)i",     nullptr},
    {"http_read",    (void *)w_http_read,    "(i*~)i",   nullptr},
    {"http_close",   (void *)w_close,        "(i)",      nullptr},
    {"ws_open",      (void *)w_ws_open,      "($$)i",    nullptr},
    {"ws_state",     (void *)w_ws_state,     "(i)i",     nullptr},
    {"ws_send",      (void *)w_ws_send,      "(i*~i)i",  nullptr},
    {"ws_recv",      (void *)w_ws_recv,      "(i*~)i",   nullptr},
    {"ws_close",     (void *)w_close,        "(i)",      nullptr},
    {"mqtt_sub",     (void *)w_mqtt_sub,     "($)i",     nullptr},
    {"mqtt_pub",     (void *)w_mqtt_pub,     "($*~i)i",  nullptr},
    {"mqtt_recv",    (void *)w_mqtt_recv,    "(*~*~)i",  nullptr},
    {"ha_available", (void *)w_ha_available, "()i",      nullptr},
    {"ha_req",       (void *)w_ha_req,       "($$*~)i",  nullptr},
    {"ha_ws",        (void *)w_ha_ws,        "()i",      nullptr},
};

}  // namespace

void nv_wasm_net_register(void)
{
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
    if (!wasm_runtime_register_natives("nv", s_natives, sizeof s_natives / sizeof s_natives[0]))
        NV_LOGE(TAG, "native registration failed");
}

void nv_wasm_net_cleanup(void)
{
    if (!s_mx) return;
    lock();
    bool any = false;
    for (auto &h : s_h)
        if (h.type != T_FREE) { h.orphan = true; any = true; }
    unlock();
    if (any) kick();
    nv_mqtt_app_reset();
}
