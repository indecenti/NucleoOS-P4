// nv_web — NucleoOS web-OS host: static shell/apps from SD + the REST API the browser shell
// (recovered from the Cardputer project) speaks. Serves the whole "NucleoOS web" desktop over
// Wi-Fi so a phone/PC browser is a companion OS for the board. Endpoint contract in the header.
#include "nv_web.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "mdns.h"

#include "nv_log.h"
#include "nv_config.h"
#include "nv_wifi.h"
#include "nv_time.h"
#include "nv_memory_broker.h"
#include "nv_wasm.h"
#include "nv_hal.h"
#include "nv_sysmon.h"
#include "nv_tts.h"
#include "nucleo_anima.h"
#include "nv_anima_system.h"   // shared ANIMA_ACT_SYSTEM {value} resolver (nv_apps)
#include "nv_media.h"
#include "nv_vplayer.h"
#include "nv_audio.h"
#include "nv_app.h"        // /api/anima/query LAUNCH -> open the app on the panel
#include "nv_ui.h"         // /api/ui/* remote automation (open/home/tap/state)
#include "nv_sd.h"         // removal-safe fopen/fclose for every docroot/FS read+write
#include "esp_lvgl_port.h"

static const char *TAG = "web";

// Static docroot (the recovered shell + apps live here) and the web-OS logical FS root. The shell
// speaks LOGICAL paths ("/system/config/...", "/data/...", "/DCIM", ...); every /api/fs/* call is
// mapped under FS_ROOT. FS_ROOT is the WHOLE card ("/sdcard") so the web OS's file manager, photo
// viewer and media players see the real device content (DCIM, Recordings, music, notes), while its
// own config still lands tidily under /sdcard/system + /sdcard/data. `..` is rejected, and the
// served OS tree (/sdcard/web) is write-protected so the file manager can't delete itself.
// Macros (not constexpr vars) so string-literal concatenation like WEB_ROOT "/apps.json" works.
#define WEB_ROOT "/sdcard/web"
#define FS_ROOT  "/sdcard"

namespace {

httpd_handle_t s_srv = nullptr;

// ---------------------------------------------------------------- small utils

// Percent-decode `in` into `out` (also '+' -> space). Safe for path use.
void url_decode(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], 0};
            out[o++] = (char)strtol(hex, nullptr, 16);
            p += 2;
        } else {
            out[o++] = (*p == '+') ? ' ' : *p;
        }
    }
    out[o] = '\0';
}

// URL-decoded query parameter `key` into out. Returns false (+ sends 400) when missing.
bool query_param(httpd_req_t *req, const char *key, char *out, size_t n) {
    char q[320];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        char raw[288];
        if (httpd_query_key_value(q, key, raw, sizeof raw) == ESP_OK) {
            url_decode(raw, out, n);
            return true;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing parameter");
    return false;
}

// Optional variant: fills `out` when present, leaves it untouched otherwise. Never errors the
// request (query_param above 400s on a miss — wrong for optional knobs like lang/mode).
bool query_param_opt(httpd_req_t *req, const char *key, char *out, size_t n) {
    char q[320];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return false;
    char raw[288];
    if (httpd_query_key_value(q, key, raw, sizeof raw) != ESP_OK) return false;
    url_decode(raw, out, n);
    return true;
}

bool client_accepts_gzip(httpd_req_t *req) {
    char h[64] = "";
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", h, sizeof h) == ESP_OK)
        return strstr(h, "gzip") != nullptr;
    return false;
}

const char *mime_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    struct { const char *ext, *mime; } M[] = {
        {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"}, {".mjs", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"}, {".json", "application/json; charset=utf-8"},
        {".webmanifest", "application/manifest+json"}, {".map", "application/json"},
        {".svg", "image/svg+xml"}, {".png", "image/png"}, {".jpg", "image/jpeg"},
        {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".webp", "image/webp"},
        {".ico", "image/x-icon"}, {".bmp", "image/bmp"}, {".wasm", "application/wasm"},
        {".woff2", "font/woff2"}, {".woff", "font/woff"}, {".ttf", "font/ttf"},
        {".txt", "text/plain; charset=utf-8"}, {".mp3", "audio/mpeg"}, {".wav", "audio/wav"},
        {".mp4", "video/mp4"}, {".webm", "video/webm"},
    };
    for (auto &m : M) if (!strcasecmp(dot, m.ext)) return m.mime;
    return "application/octet-stream";
}

// Map a shell LOGICAL path ("/system/..", "/data/..") to a physical path under FS_ROOT.
// Rejects "..". Returns false on a bad path.
bool map_fs(const char *logical, char *out, size_t n) {
    if (!logical || logical[0] != '/') return false;
    if (strstr(logical, "..")) return false;
    snprintf(out, n, "%s%s", FS_ROOT, logical);
    return true;
}

// Guard mutations: never let the file manager modify the served web-OS tree (/sdcard/web) — a stray
// delete there would take the OS offline until the next SD re-sync.
bool fs_writable(const char *phys) {
    const size_t wl = strlen(WEB_ROOT);
    return !(strncmp(phys, WEB_ROOT, wl) == 0 && (phys[wl] == '\0' || phys[wl] == '/'));
}

// mkdir -p for every parent directory of `file_path`. `base_skip` chars of the prefix are the
// mount root (e.g. "/sdcard/nucleo") and are never created/split.
void mkdirs_for(const char *file_path, size_t base_skip) {
    char tmp[384];
    snprintf(tmp, sizeof tmp, "%s", file_path);
    for (char *p = tmp + base_skip; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0777); *p = '/'; }
    }
}

// Read a bounded request body into a freshly malloc'd NUL-terminated buffer. Caller frees.
// Returns nullptr on error/oom/too-big (and sends the error).
char *recv_body(httpd_req_t *req, size_t cap, size_t *out_len) {
    if (req->content_len > cap) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too big"); return nullptr; }
    char *buf = (char *)malloc(req->content_len + 1);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return nullptr; }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) { free(buf); return nullptr; }
        got += (size_t)r;
    }
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

// Minimal JSON number extractor: finds "key": <int> in a small body. Returns default on miss.
long json_int(const char *body, const char *key, long dflt) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return dflt;
    p += strlen(pat);
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (!*p) return dflt;
    return strtol(p, nullptr, 10);
}

// Minimal JSON string extractor: finds "key":"<value>" in a small body. Copies value into out
// (no unescaping — fine for SSIDs/passwords). Returns false on miss.
bool json_str(const char *body, const char *key, char *out, size_t n) {
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (*p == ':') p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < n) { if (*p == '\\' && p[1]) p++; out[o++] = *p++; }
    out[o] = '\0';
    return true;
}

// Escape a string for embedding inside JSON double quotes (handles " \ and control chars).
void json_escape(char *out, size_t n, const char *src) {
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if (c < 0x20) { if (o + 6 < n) o += snprintf(out + o, n - o, "\\u%04x", c); }
        else out[o++] = c;
    }
    out[o] = '\0';
}

// Stream a physical file back (chunked). If `gz` set, advertise Content-Encoding: gzip.
// Returns ESP_OK, or ESP_FAIL if the file can't be opened (caller may then 404).
esp_err_t stream_file(httpd_req_t *req, const char *phys, const char *mime, bool gz) {
    FILE *f = nv_sd_fopen(phys, "rb");
    if (!f) return ESP_FAIL;
    httpd_resp_set_type(req, mime);
    if (gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    char *buf = (char *)malloc(4096);
    if (!buf) { nv_sd_fclose(f); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
    size_t k;
    while ((k = fread(buf, 1, 4096, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, k) != ESP_OK) { free(buf); nv_sd_fclose(f); return ESP_OK; }
    }
    free(buf);
    nv_sd_fclose(f);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

// ---------------------------------------------------------------- PSRAM asset cache
//
// The P4 has ~30 MB free PSRAM; the whole web tree is ~5 MB. So at boot we slurp every file under
// WEB_ROOT into PSRAM (preferring the precompressed .gz twin) and serve requests straight from RAM
// with a single httpd_resp_send — no per-request SD read, which was the real bottleneck under the
// browser's ~20 parallel boot fetches (single-task httpd + SD latency = the "pending" stalls).
// The cache is built once, read-only afterwards, so it needs no lock. Files too big to cache and
// anything not found fall back to streaming from SD (stream_file).

struct CachedFile {
    char        url[512];   // URL path key, e.g. "/shell.js" or "/apps/calculator/index.html"
    uint8_t    *data;       // PSRAM buffer
    size_t      len;
    const char *mime;       // static string
    bool        gz;         // data is gzip-encoded (serve with Content-Encoding: gzip)
};

CachedFile *s_cache = nullptr;
int         s_cache_n = 0;
int         s_cache_cap = 0;
size_t      s_cache_bytes = 0;

constexpr size_t kMaxCacheFile  = 2u * 1024 * 1024;    // files bigger than this stream from SD
constexpr size_t kMaxCacheTotal = 20u * 1024 * 1024;   // safety ceiling for the whole cache

CachedFile *cache_find(const char *url) {
    for (int i = 0; i < s_cache_n; i++)
        if (!strcmp(s_cache[i].url, url)) return &s_cache[i];
    return nullptr;
}

// Insert or update a cache entry. `data` is PSRAM-owned on success; freed here on skip. A gz variant
// always wins (and updates content); a non-gz only replaces a non-gz — so a live /api/web/put of the
// new gz twin refreshes what's served, without a non-gz push ever downgrading a cached gz.
void cache_put(const char *url, uint8_t *data, size_t len, bool gz) {
    CachedFile *ex = cache_find(url);
    if (ex) {
        if (gz || !ex->gz) { free(ex->data); ex->data = data; ex->len = len; ex->gz = gz; ex->mime = mime_for(url); }
        else free(data);         // don't downgrade a cached gz with a non-gz
        return;
    }
    if (s_cache_n == s_cache_cap) {
        int nc = s_cache_cap ? s_cache_cap * 2 : 128;
        CachedFile *np = (CachedFile *)heap_caps_realloc(s_cache, nc * sizeof(CachedFile), MALLOC_CAP_SPIRAM);
        if (!np) { free(data); return; }
        s_cache = np; s_cache_cap = nc;
    }
    CachedFile &c = s_cache[s_cache_n++];
    snprintf(c.url, sizeof c.url, "%s", url);
    c.data = data; c.len = len; c.gz = gz; c.mime = mime_for(url);
}

// Recursively slurp the web tree into the PSRAM cache. pbuf/ubuf are SHARED path buffers threaded
// through the recursion (current dir path, no trailing '/'): we append "/name", recurse, then
// truncate back. This keeps each recursion frame tiny — earlier per-frame char[768]+char[512]
// arrays blew the task stack (Stack protection fault) on the 3-4-deep apps/<id>/ tree.
void cache_walk(char *pbuf, size_t pcap, size_t plen, char *ubuf, size_t ucap, size_t ulen) {
    DIR *d = opendir(pbuf);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        int pn = snprintf(pbuf + plen, pcap - plen, "/%s", e->d_name);
        int un = snprintf(ubuf + ulen, ucap - ulen, "/%s", e->d_name);
        if (pn > 0 && (size_t)pn < pcap - plen && un > 0 && (size_t)un < ucap - ulen) {
            struct stat st{};
            if (stat(pbuf, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    cache_walk(pbuf, pcap, plen + pn, ubuf, ucap, ulen + un);
                } else if ((size_t)st.st_size <= kMaxCacheFile &&
                           s_cache_bytes + (size_t)st.st_size <= kMaxCacheTotal) {
                    bool gz = false;
                    size_t ul = strlen(ubuf);
                    if (ul > 3 && !strcmp(ubuf + ul - 3, ".gz")) { gz = true; ubuf[ul - 3] = '\0'; }
                    FILE *f = nv_sd_fopen(pbuf, "rb");
                    if (f) {
                        uint8_t *buf = (uint8_t *)heap_caps_malloc(st.st_size ? st.st_size : 1, MALLOC_CAP_SPIRAM);
                        if (buf) {
                            size_t got = fread(buf, 1, st.st_size, f);
                            if (got == (size_t)st.st_size) { cache_put(ubuf, buf, got, gz); s_cache_bytes += got; }
                            else free(buf);
                        }
                        nv_sd_fclose(f);
                    }
                }
            }
        }
        pbuf[plen] = '\0';   // restore path for the next sibling
        ubuf[ulen] = '\0';
    }
    closedir(d);
}

void cache_build(void) {
    s_cache_bytes = 0;
    char *pbuf = (char *)malloc(1024), *ubuf = (char *)malloc(1024);
    if (pbuf && ubuf) {
        snprintf(pbuf, 1024, "%s", WEB_ROOT);
        ubuf[0] = '\0';
        cache_walk(pbuf, 1024, strlen(pbuf), ubuf, 1024, 0);
    }
    free(pbuf); free(ubuf);
    NV_LOGI(TAG, "asset cache: %d files, %u KB in PSRAM", s_cache_n, (unsigned)(s_cache_bytes / 1024));
}

// ---------------------------------------------------------------- static shell

// Catch-all GET: serve WEB_ROOT/<uri>. Hits the PSRAM cache first (single send from RAM), mapping a
// directory to its index.html and preferring the gz variant when the client accepts gzip. Anything
// not cached (big files) falls back to streaming from SD.
esp_err_t h_static(httpd_req_t *req) {
    char uri[600];
    char raw[600];
    snprintf(raw, sizeof raw, "%s", req->uri);
    char *q = strchr(raw, '?'); if (q) *q = '\0';
    url_decode(raw, uri, sizeof uri);
    if (strstr(uri, "..")) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");

    // Normalize to a cache key: "/" and any dir → its index.html.
    char key[608];
    if (!uri[0] || !strcmp(uri, "/")) snprintf(key, sizeof key, "/index.html");
    else snprintf(key, sizeof key, "%s", uri);
    const bool accept_gz = client_accepts_gzip(req);

    // Cache lookup — try the path, then the path as a directory (+ "/index.html").
    CachedFile *c = cache_find(key);
    if (!c) {
        char alt[620];
        size_t kl = strlen(key);
        snprintf(alt, sizeof alt, "%s%s", key, (kl && key[kl - 1] == '/') ? "index.html" : "/index.html");
        c = cache_find(alt);
    }
    if (c && !(c->gz && !accept_gz)) {   // serve from RAM (unless it's gz-only and client won't take gz)
        httpd_resp_set_type(req, c->mime);
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        if (c->gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)c->data, c->len);
    }

    // SD fallback (uncached/oversized files, or gz-only miss).
    char phys[640];
    snprintf(phys, sizeof phys, "%s%s", WEB_ROOT, uri[0] && strcmp(uri, "/") ? uri : "/index.html");
    struct stat st{};
    if (stat(phys, &st) == 0 && S_ISDIR(st.st_mode)) {
        size_t l = strlen(phys);
        snprintf(phys + l, sizeof phys - l, "%s", phys[l - 1] == '/' ? "index.html" : "/index.html");
    }
    const char *mime = mime_for(phys);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (accept_gz) {
        char gz[648];
        snprintf(gz, sizeof gz, "%s.gz", phys);
        struct stat gs{};
        if (stat(gz, &gs) == 0 && S_ISREG(gs.st_mode)) return stream_file(req, gz, mime, true);
    }
    if (stream_file(req, phys, mime, false) == ESP_FAIL)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    return ESP_OK;
}

// ---------------------------------------------------------------- device info / status

esp_err_t h_info(httpd_req_t *req) {
    const esp_app_desc_t *ad = esp_app_get_description();
    char ip[16] = "?";
    esp_netif_ip_info_t ipi;
    esp_netif_t *nif = esp_netif_get_default_netif();
    if (nif && esp_netif_get_ip_info(nif, &ipi) == ESP_OK) snprintf(ip, sizeof ip, IPSTR, IP2STR(&ipi.ip));
    char body[320];
    snprintf(body, sizeof body,
             "{\"name\":\"NucleoOS\",\"version\":\"%s\",\"ip\":\"%s\",\"uptime_s\":%lld,"
             "\"sram_free_kb\":%u,\"psram_free_kb\":%u,\"abi\":%d}",
             ad->version, ip, (long long)(esp_timer_get_time() / 1000000),
             (unsigned)(nv_mem_free_internal() / 1024), (unsigned)(nv_mem_free_psram() / 1024), NV_WASM_ABI);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// GET /api/status — the shell's periodic device snapshot (drives tray, clock push, system-monitor).
esp_err_t h_status(httpd_req_t *req) {
    const esp_app_desc_t *ad = esp_app_get_description();
    char ip[16] = "0.0.0.0";
    esp_netif_ip_info_t ipi;
    esp_netif_t *nif = esp_netif_get_default_netif();
    if (nif && esp_netif_get_ip_info(nif, &ipi) == ESP_OK) snprintf(ip, sizeof ip, IPSTR, IP2STR(&ipi.ip));
    const bool wifi_up = nv_wifi_get_state() == NV_WIFI_CONNECTED;
    const bool synced = nv_time_is_synced();
    char body[420];
    snprintf(body, sizeof body,
             "{\"os\":\"NucleoOS\",\"version\":\"%s\",\"uptime_s\":%lld,\"free_heap\":%u,"
             "\"storage\":{\"mounted\":true,\"fs\":\"fat\",\"total_kb\":0,\"free_kb\":0},"
             "\"network\":{\"mode\":\"sta\",\"ssid\":\"\",\"ip\":\"%s\",\"connected\":%s,"
             "\"time_synced\":%s,\"epoch\":%lld},"
             "\"ota\":{\"active\":\"%s\",\"boot\":\"%s\"}}",
             ad->version, (long long)(esp_timer_get_time() / 1000000),
             (unsigned)nv_mem_free_internal(), ip, wifi_up ? "true" : "false",
             synced ? "true" : "false", (long long)time(nullptr), ad->version, ad->version);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// GET /api/auth/status — pairing gate. This build is LAN-open (no pairing), so the shell boots
// straight through to the desktop.
esp_err_t h_auth_status(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"required\":false,\"paired\":true}", HTTPD_RESP_USE_STRLEN);
}

// GET /api/apps — installed-app list. Served straight from the pre-generated apps.json so we don't
// parse manifests in C; the shell falls back to a built-in mock set if this 404s.
esp_err_t h_apps(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (stream_file(req, WEB_ROOT "/apps.json", "application/json", false) == ESP_FAIL)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no apps.json");
    return ESP_OK;
}

// GET /api/associations — file-type -> app map (optional; shell try/catches a miss).
esp_err_t h_assoc(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (stream_file(req, WEB_ROOT "/associations.json", "application/json", false) == ESP_FAIL)
        return httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// The full ANIMA cascade (trigram cosine, Levenshtein, token-edit, tier fan-out) needs deep frames
// — running it inline would blow the 8 KB httpd worker stack (observed: httpd crash in coh_tricos).
// Mirror the native chat app: run the query on a persistent 24 KB PSRAM-stack worker, join here.
// I/O is SD-only (no internal flash/NVS), so a PSRAM stack is safe per the psram-task-stacks rule.
// esp_http_server dispatches requests serially on one task, so these request statics never overlap.
static char             s_aq_text[256];
static char             s_aq_lang[4] = "it";
static anima_result_t   s_aq_res;
static SemaphoreHandle_t s_aq_go, s_aq_done;
static TaskHandle_t     s_aq_task;

static void anima_query_worker(void *) {
    for (;;) {
        xSemaphoreTake(s_aq_go, portMAX_DELAY);
        s_aq_res = nucleo_anima_query(s_aq_text, s_aq_lang);
        xSemaphoreGive(s_aq_done);
    }
}

// Lazily bring up the worker; returns false only on OOM (then the caller falls back to inline).
static bool anima_worker_ensure(void) {
    if (s_aq_task) return true;
    if (!s_aq_go)   s_aq_go   = xSemaphoreCreateBinary();
    if (!s_aq_done) s_aq_done = xSemaphoreCreateBinary();
    if (!s_aq_go || !s_aq_done) return false;
    return xTaskCreateWithCaps(anima_query_worker, "web_anima", 24 * 1024, nullptr, 4, &s_aq_task,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

// Long-form tail (L1 card / code snippet) captured under the spine lock right after the query —
// nucleo_anima_long_reply() points into engine state a later query may rewrite.
static char s_aq_long[2048];

// Run one query through the engine on the PSRAM worker (spine-gated). Returns false when the
// native chat owns the cascade — the caller answers {"busy":true}.
static bool anima_run(const char *text, const char *lang, anima_result_t *out) {
    nucleo_anima_init(lang);
    if (!nucleo_anima_try_lock()) return false;
    if (anima_worker_ensure()) {                       // run the cascade off the tiny httpd stack
        strlcpy(s_aq_text, text, sizeof s_aq_text);
        strlcpy(s_aq_lang, lang, sizeof s_aq_lang);
        xSemaphoreGive(s_aq_go);
        xSemaphoreTake(s_aq_done, portMAX_DELAY);
        *out = s_aq_res;
    } else {
        *out = nucleo_anima_query(text, lang);         // OOM fallback (worker couldn't start)
    }
    const char *lr = nucleo_anima_long_reply();
    snprintf(s_aq_long, sizeof s_aq_long, "%s", lr ? lr : "");
    nucleo_anima_unlock();
    return true;
}

// A LAUNCH action really opens the app on the panel (LVGL-locked) — same contract as native
// chat. TOOL proposals (set_volume/set_brightness) execute through the shared OS glue.
static void anima_do_launch(const anima_result_t &r) {
    if (r.action == ANIMA_ACT_LAUNCH && r.arg[0] && lvgl_port_lock(2000)) {
        const NvApp *a = nv_ui_find_app(r.arg);
        if (a) nv_ui_open_app(a);
        lvgl_port_unlock();
    }
    if (r.action == ANIMA_ACT_TOOL) nv_anima_os_exec(r.intent, r.arg);
}

// Final human-facing text: prefer the long-form tail, then splice live SYSTEM values into the
// {value} template (clock/SD/registry live here in the OS, not in the engine).
static void anima_final_text(const anima_result_t &r, bool en, char *out, size_t cap) {
    const char *base = s_aq_long[0] ? s_aq_long : r.reply;
    if (r.action == ANIMA_ACT_SYSTEM) nv_anima_system_reply(r.arg, base, en, out, cap);
    else                              snprintf(out, cap, "%s", base);
    if (r.action == ANIMA_ACT_LAUNCH && r.arg[0])
        nv_anima_pretty_launch(out, cap, r.arg);   // "Apro calc." -> "Apro Calcolatrice."
}

// POST /api/anima/query?text=... — the native ANIMA engine answers over REST (the web companion
// asks the DEVICE brain instead of its browser WASM twin).
esp_err_t h_anima_query(httpd_req_t *req) {
    char text[256];
    if (!query_param(req, "text", text, sizeof text)) return ESP_OK;
    char lang[4] = "it";
    query_param_opt(req, "lang", lang, sizeof lang);
    anima_result_t r;
    if (!anima_run(text, lang, &r)) {                  // native chat may own the cascade
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"busy\":true}", HTTPD_RESP_USE_STRLEN);
    }
    anima_do_launch(r);
    // Statics, not stack: the resolved+escaped long-form answer would eat most of the 12 KB httpd
    // stack. esp_http_server dispatches serially on one task, so they never overlap.
    static char resolved[2200], reply[2800], b[3400];
    anima_final_text(r, strncmp(lang, "en", 2) == 0, resolved, sizeof resolved);
    json_escape(reply, sizeof reply, resolved);
    snprintf(b, sizeof b,
             "{\"tier\":%d,\"action\":%d,\"intent\":\"%s\",\"arg\":\"%s\",\"conf\":%d,\"reply\":\"%s\"}",
             (int)r.tier, (int)r.action, r.intent, r.arg, r.confidence, reply);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

// GET /api/anima?q=...&lang=it|en — the copilot-compatible face of the same engine. The web
// shell's system copilot (sd/web/copilot.js) was written against this route/shape on the
// Cardputer: STRING action/tier + trace + resolved reply. Without it every device-side copilot
// turn 404'd into "I can't reach the ANIMA engine".
esp_err_t h_anima_get(httpd_req_t *req) {
    char q[256];
    if (!query_param(req, "q", q, sizeof q)) return ESP_OK;
    char lang[4] = "it";
    query_param_opt(req, "lang", lang, sizeof lang);   // `mode` accepted but not applied (auto)
    anima_result_t r;
    if (!anima_run(q, lang, &r)) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"busy\":true,\"reply\":\"\",\"action\":\"none\"}",
                               HTTPD_RESP_USE_STRLEN);
    }
    anima_do_launch(r);
    const char *tier = r.tier == ANIMA_TIER_COMMAND ? "command" :
                       r.tier == ANIMA_TIER_FACT    ? "fact"    :
                       r.tier == ANIMA_TIER_REMOTE  ? "remote"  : "none";
    const char *action = r.action == ANIMA_ACT_LAUNCH ? "launch" :
                         r.action == ANIMA_ACT_SYSTEM ? "system" :
                         r.action == ANIMA_ACT_ANSWER ? "answer" :
                         r.action == ANIMA_ACT_TOOL   ? "tool"   : "none";
    static char resolved[2200], reply[2800], trace[256], b[3600];
    anima_final_text(r, strncmp(lang, "en", 2) == 0, resolved, sizeof resolved);
    json_escape(reply, sizeof reply, resolved);
    json_escape(trace, sizeof trace, r.trace);
    snprintf(b, sizeof b,
             "{\"tier\":\"%s\",\"action\":\"%s\",\"intent\":\"%s\",\"tool\":\"%s\",\"arg\":\"%s\","
             "\"conf\":%d,\"trace\":\"%s\",\"reply\":\"%s\"}",
             tier, action, r.intent, r.action == ANIMA_ACT_TOOL ? r.intent : "", r.arg,
             r.confidence, trace, reply);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

// GET /api/audio/selftest — end-to-end audio loop, remotely triggerable: record 2 s from the
// on-board mic to SD, then play it back WITH the level meter running (the exact Recorder-app
// condition). Reports every stage so the PC can verify the whole audio system unattended.
esp_err_t h_audio_selftest(httpd_req_t *req) {
    const char *wav = "/sdcard/Recordings/selftest.wav";
    mkdir("/sdcard/Recordings", 0777);
    const bool rec_ok = nv_audio_rec_start(wav);
    if (rec_ok) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        nv_audio_rec_stop();
        vTaskDelay(pdMS_TO_TICKS(400));            // mic task finalizes the WAV header
    }
    struct stat st = {};
    const long rec_bytes = (stat(wav, &st) == 0) ? (long)st.st_size : -1;

    nv_media_init();
    nv_audio_mic_meter_start();                    // Recorder keeps the meter on while playing
    const bool play_ok = nv_media_play(wav);
    int waited = 0, pos = 0, dur = 0;
    while (play_ok && waited < 6000) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
        pos = nv_media_pos_ms();
        dur = nv_media_dur_ms();
        if (nv_media_state() != NV_MEDIA_PLAYING) break;
    }
    const int end_state = (int)nv_media_state();
    nv_audio_mic_meter_stop();

    char b[192];
    snprintf(b, sizeof b,
             "{\"rec_ok\":%s,\"rec_bytes\":%ld,\"play_ok\":%s,\"end_state\":%d,\"pos\":%d,\"dur\":%d}",
             rec_ok ? "true" : "false", rec_bytes, play_ok ? "true" : "false", end_state, pos, dur);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

// /api/media — remote transport for the nv_media engine (web companion control + testability:
// the PC can drive playback and read health without touching the panel).
esp_err_t h_media_play(httpd_req_t *req) {
    char logical[256], phys[320];
    if (!query_param(req, "path", logical, sizeof logical)) return ESP_OK;
    if (!map_fs(logical, phys, sizeof phys)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    nv_media_init();
    const bool ok = nv_media_play(phys);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t h_media_stop(httpd_req_t *req) {
    nv_media_stop();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t h_media_state(httpd_req_t *req) {
    static const char *kSt[] = {"stopped", "playing", "paused", "error"};
    char b[128];
    snprintf(b, sizeof b, "{\"state\":\"%s\",\"pos\":%d,\"dur\":%d}",
             kSt[nv_media_state() & 3], nv_media_pos_ms(), nv_media_dur_ms());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

// /api/video — remote transport for the nv_vplayer engine (mirrors /api/media above). Lets the PC
// drive play/seek/stop and read back pos/dur/has_audio/fps headlessly — the board has no way to
// show this over a screenshot alone (audio, A/V sync, seek correctness need behavioral checks).
esp_err_t h_video_play(httpd_req_t *req) {
    char logical[256], phys[320];
    if (!query_param(req, "path", logical, sizeof logical)) return ESP_OK;
    if (!map_fs(logical, phys, sizeof phys)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    nv_vplayer_init();
    const bool ok = nv_vplayer_open(phys);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t h_video_stop(httpd_req_t *req) {
    // release() (non solo stop): libera HW JPEG engine + client PPA + ring, altrimenti restano
    // allocati per sempre e la prossima app che vuole quei singoli HW si rompe.
    nv_vplayer_release();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t h_video_state(httpd_req_t *req) {
    static const char *kSt[] = {"stopped", "playing", "paused", "error"};
    char b[160];
    snprintf(b, sizeof b, "{\"state\":\"%s\",\"pos\":%d,\"dur\":%d,\"has_audio\":%s,\"fps10\":%d}",
             kSt[nv_vplayer_state() & 3], nv_vplayer_pos_ms(), nv_vplayer_dur_ms(),
             nv_vplayer_has_audio() ? "true" : "false", nv_vplayer_fps10());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

esp_err_t h_video_seek(httpd_req_t *req) {
    char v[16];
    if (!query_param(req, "pos_ms", v, sizeof v)) return ESP_OK;
    const bool ok = nv_vplayer_seek((int)strtol(v, nullptr, 10));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
}

// GET /api/anima/caps — AI capabilities, live from the nv_anima engine. Note: with no cloud key
// configured, teacher_info may run one rate-limited (5-min window) 2 s mDNS probe for a LAN teacher.
esp_err_t h_anima_caps(httpd_req_t *req) {
    char prov[24] = "", model[40] = "";
    const bool key    = nucleo_anima_teacher_info(prov, sizeof prov, model, sizeof model);
    const bool online = nucleo_anima_online_available();
    const int  mode   = nucleo_anima_l1_get_mode();
    char b[256];
    snprintf(b, sizeof b,
             "{\"hasKey\":%s,\"online\":%s,\"enabled\":true,\"provider\":\"%s\",\"model\":\"%s\","
             "\"l1Mode\":\"%s\",\"l1Serving\":%s}",
             key ? "true" : "false", online ? "true" : "false", prov, model,
             mode == ANIMA_L1_ON ? "on" : mode == ANIMA_L1_OFF ? "off" : "auto",
             nucleo_anima_l1_serving() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

// GET /api/heap — per-region heap stats for the system-monitor RAM tab. The `internal` region is the
// scarce on-chip SRAM that actually matters; `psram` is reported for completeness. Backed by nv_sysmon
// (the shared telemetry core — same data the on-device System Monitor app renders).
esp_err_t h_heap(httpd_req_t *req) {
    nv_sys_mem_t m;
    nv_sysmon_mem(&m);
    auto region = [](char *out, size_t n, const nv_mem_pool_t *p) {
        snprintf(out, n,
                 "{\"total_bytes\":%u,\"free_bytes\":%u,\"allocated_bytes\":%u,"
                 "\"largest_free_block\":%u,\"min_free_bytes\":%u,\"frag_pct\":%d}",
                 (unsigned)p->total, (unsigned)p->free_bytes, (unsigned)p->used,
                 (unsigned)p->largest, (unsigned)p->min_free, (int)p->frag_pct);
    };
    char in[220], ps[220], body[480];
    region(in, sizeof in, &m.internal);
    region(ps, sizeof ps, &m.psram);
    snprintf(body, sizeof body, "{\"internal\":%s,\"psram\":%s}", in, ps);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// GET /api/cpu — per-core load + freq/tasks/uptime for the system-monitor CPU tab. Backed by nv_sysmon;
// loads are integer percents (delta since the previous poll — poll at a steady cadence).
esp_err_t h_cpu(httpd_req_t *req) {
    nv_sys_perf_t p;
    nv_sysmon_perf(&p);
    int l0 = p.core_load[0] < 0 ? 0 : (int)(p.core_load[0] + 0.5f);
    int l1 = p.core_load[1] < 0 ? 0 : (int)(p.core_load[1] + 0.5f);
    int la = p.load_avg   < 0 ? 0 : (int)(p.load_avg   + 0.5f);
    char body[256];
    snprintf(body, sizeof body,
             "{\"uptime_s\":%lld,\"cores\":2,\"freq_mhz\":%u,\"tasks\":%u,"
             "\"load\":[%d,%d],\"load_avg\":%d}",
             (long long)p.uptime_s, (unsigned)p.freq_mhz,
             (unsigned)p.task_count, l0, l1, la);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// POST /api/web/put?path=<rel> — write body into the served web tree (/sdcard/web/<rel>) AND update
// the PSRAM cache live, so a pushed file is served immediately, persists across reboots, and needs no
// card removal. This is the ONE sanctioned writer into /sdcard/web (the fs API guards it); it's how
// web-frontend edits deploy over Wi-Fi. LAN-open like the rest.
esp_err_t h_web_put(httpd_req_t *req) {
    char rel[256];
    if (!query_param(req, "path", rel, sizeof rel)) return ESP_OK;
    const char *r = rel[0] == '/' ? rel + 1 : rel;        // tolerate a leading slash
    if (strstr(r, "..") || !r[0]) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    size_t len = req->content_len;
    if (len > 8u * 1024 * 1024) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too big (8MB cap)");

    char phys[320];
    snprintf(phys, sizeof phys, "%s/%s", WEB_ROOT, r);
    mkdirs_for(phys, strlen(WEB_ROOT) + 1);

    uint8_t *buf = (uint8_t *)heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    size_t got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, (char *)buf + got, len - got);
        if (n <= 0) { free(buf); return ESP_FAIL; }
        got += (size_t)n;
    }
    FILE *f = nv_sd_fopen(phys, "wb");
    if (!f) { free(buf); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed"); }
    size_t wr = fwrite(buf, 1, len, f);
    nv_sd_fclose(f);
    if (wr != len) { free(buf); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed"); }

    // Live-update the PSRAM cache. Key = URL path minus a trailing ".gz"; cache_put takes ownership of
    // `buf` (or frees it if a cached gz shouldn't be downgraded), so we don't free it here.
    char url[260];
    snprintf(url, sizeof url, "/%s", r);
    bool gz = false;
    size_t ul = strlen(url);
    if (ul > 3 && !strcmp(url + ul - 3, ".gz")) { gz = true; url[ul - 3] = '\0'; }
    cache_put(url, buf, len, gz);

    NV_LOGI(TAG, "web put: %s (%u bytes, cache updated)", phys, (unsigned)len);
    return httpd_resp_sendstr(req, "ok");
}

// ---------------------------------------------------------------- Wi-Fi (nv_wifi)

// GET /api/wifi/scan — trigger a scan, wait briefly for results, return the AP list.
esp_err_t h_wifi_scan(httpd_req_t *req) {
    nv_wifi_start_scan();
    uint32_t g0 = nv_wifi_scan_generation();
    const int64_t t0 = esp_timer_get_time();
    while (nv_wifi_scan_generation() == g0 && esp_timer_get_time() - t0 < 5000000)
        vTaskDelay(pdMS_TO_TICKS(200));
    nv_wifi_ap_t aps[32];
    int n = nv_wifi_copy_aps(aps, 32);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"networks\":[");
    char esc[80], item[220];
    for (int i = 0; i < n; i++) {
        json_escape(esc, sizeof esc, aps[i].ssid);
        snprintf(item, sizeof item,
                 "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\",\"secured\":%s,\"saved\":%s}",
                 i ? "," : "", esc, aps[i].rssi, nv_wifi_auth_label(aps[i].auth),
                 aps[i].secured ? "true" : "false", aps[i].saved ? "true" : "false");
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, nullptr);
}

// GET /api/wifi/known — saved networks (from the in-range scan) + the current association.
esp_err_t h_wifi_known(httpd_req_t *req) {
    char conn[33] = "";
    nv_wifi_get_connected(conn, sizeof conn, nullptr, 0, nullptr);
    nv_wifi_ap_t aps[32];
    int n = nv_wifi_copy_aps(aps, 32);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"networks\":[");
    char esc[80], item[160];
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (!aps[i].saved) continue;
        json_escape(esc, sizeof esc, aps[i].ssid);
        snprintf(item, sizeof item, "%s{\"ssid\":\"%s\",\"priority\":0,\"current\":%s}",
                 first ? "" : ",", esc, !strcmp(aps[i].ssid, conn) ? "true" : "false");
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    httpd_resp_sendstr_chunk(req, "],\"mode\":\"sta\",\"ssid\":\"");
    json_escape(esc, sizeof esc, conn);
    httpd_resp_sendstr_chunk(req, esc);
    httpd_resp_sendstr_chunk(req, "\"}");
    return httpd_resp_sendstr_chunk(req, nullptr);
}

// POST /api/wifi/join  body {"ssid":"..","password":".."} — associate, wait, report {ok,ip}.
esp_err_t h_wifi_join(httpd_req_t *req) {
    size_t len = 0;
    char *body = recv_body(req, 256, &len);
    if (!body) return ESP_OK;
    char ssid[33] = "", pass[65] = "";
    bool have = json_str(body, "ssid", ssid, sizeof ssid);
    json_str(body, "password", pass, sizeof pass);
    free(body);
    if (!have || !ssid[0]) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");

    nv_wifi_connect(ssid, pass);
    const int64_t t0 = esp_timer_get_time();
    while (nv_wifi_get_state() != NV_WIFI_CONNECTED && nv_wifi_get_state() != NV_WIFI_FAILED &&
           esp_timer_get_time() - t0 < 12000000)
        vTaskDelay(pdMS_TO_TICKS(250));
    char ip[16] = "";
    bool ok = nv_wifi_get_connected(nullptr, 0, ip, sizeof ip, nullptr);
    char out[64];
    snprintf(out, sizeof out, "{\"ok\":%s,\"ip\":\"%s\"}", ok ? "true" : "false", ip);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

// GET /ws — the shell's live-event socket. Minimal: complete the handshake and drain client frames
// so the shell attaches (badge "live", no retry spam). Server-push of bus events is a future add;
// the shell already falls back to polling for status/fs changes.
esp_err_t h_ws(httpd_req_t *req) {
    if (req->method == HTTP_GET) return ESP_OK;   // esp_http_server completed the WS upgrade
    httpd_ws_frame_t f;
    memset(&f, 0, sizeof f);
    f.type = HTTPD_WS_TYPE_TEXT;
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) return ESP_OK;   // length probe
    if (f.len && f.len < 2048) {
        uint8_t *b = (uint8_t *)malloc(f.len + 1);
        if (b) { f.payload = b; httpd_ws_recv_frame(req, &f, f.len); free(b); }   // drain, ignore
    }
    return ESP_OK;
}

// ---------------------------------------------------------------- filesystem API (logical paths)

// GET /api/fs/list?path=<logical> -> {"entries":[{"name","type":"dir"|"file","size","isDir"}]}
esp_err_t h_fs_list(httpd_req_t *req) {
    char logical[256], phys[320];
    if (!query_param(req, "path", logical, sizeof logical)) return ESP_OK;
    if (!map_fs(logical, phys, sizeof phys)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");

    DIR *d = opendir(phys);
    if (!d) {
        // A missing dir lists empty rather than 404 — the shell treats first-boot dirs as empty.
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"entries\":[]}", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"entries\":[");
    struct dirent *e;
    bool first = true;
    char item[400];
    while ((e = readdir(d)) != nullptr) {
        char full[640];
        snprintf(full, sizeof full, "%s/%s", phys, e->d_name);
        struct stat st{};
        stat(full, &st);
        const bool is_dir = S_ISDIR(st.st_mode);
        snprintf(item, sizeof item,
                 "%s{\"name\":\"%s\",\"type\":\"%s\",\"size\":%ld,\"isDir\":%s}",
                 first ? "" : ",", e->d_name, is_dir ? "dir" : "file",
                 is_dir ? 0L : (long)st.st_size, is_dir ? "true" : "false");
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    closedir(d);
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, nullptr);
}

// GET /api/fs/read?path=<logical> -> raw file bytes.
esp_err_t h_fs_read(httpd_req_t *req) {
    char logical[256], phys[320];
    if (!query_param(req, "path", logical, sizeof logical)) return ESP_OK;
    if (!map_fs(logical, phys, sizeof phys)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    if (stream_file(req, phys, mime_for(phys), false) == ESP_FAIL)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
    return ESP_OK;
}

// POST /api/fs/write?path=<logical>  body = raw bytes -> file (parents auto-created).
esp_err_t h_fs_write(httpd_req_t *req) {
    char logical[256], phys[320];
    if (!query_param(req, "path", logical, sizeof logical)) return ESP_OK;
    if (!map_fs(logical, phys, sizeof phys)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    if (!fs_writable(phys)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "read-only (web OS files)");
    if (req->content_len > 64u * 1024 * 1024)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too big (64MB cap)");

    mkdirs_for(phys, strlen(FS_ROOT) + 1);
    FILE *f = nv_sd_fopen(phys, "wb");
    if (!f) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
    char buf[2048];
    size_t left = req->content_len;
    while (left > 0) {
        const int n = httpd_req_recv(req, buf, left < sizeof buf ? left : sizeof buf);
        if (n <= 0) { nv_sd_fclose(f); unlink(phys); return ESP_FAIL; }
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            nv_sd_fclose(f); unlink(phys);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        }
        left -= (size_t)n;
    }
    nv_sd_fclose(f);
    return httpd_resp_sendstr(req, "ok");
}

// POST /api/fs/mkdir?path=<logical> -> mkdir -p.
esp_err_t h_fs_mkdir(httpd_req_t *req) {
    char logical[256], phys[320];
    if (!query_param(req, "path", logical, sizeof logical)) return ESP_OK;
    if (!map_fs(logical, phys, sizeof phys)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    if (!fs_writable(phys)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "read-only (web OS files)");
    // Create the full chain including the leaf.
    char leaf[336];
    snprintf(leaf, sizeof leaf, "%s/", phys);
    mkdirs_for(leaf, strlen(FS_ROOT) + 1);
    return httpd_resp_sendstr(req, "ok");
}

// POST /api/fs/delete?path=<logical> -> unlink file / rmdir empty dir.
esp_err_t h_fs_delete(httpd_req_t *req) {
    char logical[256], phys[320];
    if (!query_param(req, "path", logical, sizeof logical)) return ESP_OK;
    if (!map_fs(logical, phys, sizeof phys)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    if (!fs_writable(phys)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "read-only (web OS files)");
    struct stat st{};
    if (stat(phys, &st) != 0) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    const int rc = S_ISDIR(st.st_mode) ? rmdir(phys) : unlink(phys);
    if (rc != 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed (dir not empty?)");
    return httpd_resp_sendstr(req, "ok");
}

// POST /api/fs/move?from=<logical>&to=<logical> -> rename (parents of dest auto-created).
esp_err_t h_fs_move(httpd_req_t *req) {
    char lf[256], lt[256], pf[320], pt[320];
    if (!query_param(req, "from", lf, sizeof lf)) return ESP_OK;
    if (!query_param(req, "to", lt, sizeof lt)) return ESP_OK;
    if (!map_fs(lf, pf, sizeof pf) || !map_fs(lt, pt, sizeof pt))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    if (!fs_writable(pf) || !fs_writable(pt))
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "read-only (web OS files)");
    mkdirs_for(pt, strlen(FS_ROOT) + 1);
    if (rename(pf, pt) != 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "move failed");
    return httpd_resp_sendstr(req, "ok");
}

// ---------------------------------------------------------------- time / power

// POST /api/time/set  body {"ts":<epoch_s>} -> set the wall clock.
esp_err_t h_time_set(httpd_req_t *req) {
    size_t len = 0;
    char *body = recv_body(req, 256, &len);
    if (!body) return ESP_OK;
    long ts = json_int(body, "ts", 0);
    free(body);
    if (ts > 1000000000L) {
        struct timeval tv;
        tv.tv_sec = (time_t)ts;
        tv.tv_usec = 0;
        settimeofday(&tv, nullptr);
        NV_LOGI(TAG, "clock set from web: %ld", ts);
    }
    return httpd_resp_sendstr(req, "ok");
}

void reboot_task(void *) {
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

// POST /api/reboot -> restart shortly after replying so the response flushes first.
esp_err_t h_reboot(httpd_req_t *req) {
    httpd_resp_sendstr(req, "ok");
    xTaskCreate(reboot_task, "reboot", 2048, nullptr, 5, nullptr);
    return ESP_OK;
}

// ---------------------------------------------------------------- WASM app runner (dev hot-reload)

esp_err_t h_app_run(httpd_req_t *req) {
    char id[32];
    if (!query_param(req, "id", id, sizeof id)) return ESP_OK;

    nv_wasm_app_t app;
    if (!nv_wasm_load_manifest(id, &app))
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such app (manifest/app.wasm missing)");

    char err[128] = "";
    if (!nv_wasm_exec_start(&app, err, sizeof err)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, err);
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    char chunk[513];
    size_t k;
    const int64_t t0 = esp_timer_get_time();
    uint32_t web_ms = app.timeout_ms < 30000 ? app.timeout_ms : 30000;
    const int64_t budget_us = ((int64_t)web_ms + 2000) * 1000;
    bool web_timeout = false;
    for (;;) {
        while ((k = nv_wasm_exec_read(chunk, sizeof chunk - 1)) > 0)
            httpd_resp_send_chunk(req, chunk, k);
        int tkind; char tmsg[64];
        while (nv_wasm_exec_take_toast(&tkind, tmsg, sizeof tmsg)) {
            char line[96];
            const int n = snprintf(line, sizeof line, "[toast:%d] %s\n", tkind, tmsg);
            httpd_resp_send_chunk(req, line, n);
        }
        if (nv_wasm_exec_state() == NV_WRUN_DONE) break;
        if (esp_timer_get_time() - t0 > budget_us) { nv_wasm_exec_abort(); web_timeout = true; break; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    while ((k = nv_wasm_exec_read(chunk, sizeof chunk - 1)) > 0)
        httpd_resp_send_chunk(req, chunk, k);

    char foot[160];
    int n;
    if (web_timeout) {
        n = snprintf(foot, sizeof foot, "\n-- ABORTED: exceeded %u s web limit --\n",
                     (unsigned)((web_ms + 2000) / 1000));
    } else {
        bool ok = false; uint32_t ms = 0;
        nv_wasm_exec_collect(&ok, &ms, err, sizeof err);
        n = ok ? snprintf(foot, sizeof foot, "\n-- OK (%u ms) --\n", (unsigned)ms)
               : snprintf(foot, sizeof foot, "\n-- FAILED: %s --\n", err);
    }
    httpd_resp_send_chunk(req, foot, n);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t h_logs(httpd_req_t *req) {
    const size_t cap = 64 * 1024;
    char *buf = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    const size_t n = nv_log_snapshot(buf, cap);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    const esp_err_t rc = httpd_resp_send(req, buf, n);
    free(buf);
    return rc;
}

// GET /api/screen -> capture the live framebuffer to a JPEG (HW encoder) and stream it back. Lets a
// remote tool SEE the screen (companion to /api/logs).
esp_err_t h_screen(httpd_req_t *req) {
    const char *path = "/sdcard/tmp/screen.jpg";
    mkdir("/sdcard/tmp", 0777);
    if (!nv_hal_screenshot(path))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
    FILE *f = nv_sd_fopen(path, "rb");
    if (!f) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
    httpd_resp_set_type(req, "image/jpeg");
    char buf[2048]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) { nv_sd_fclose(f); return ESP_FAIL; }
    nv_sd_fclose(f);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// ---------------------------------------------------------------- remote UI automation (/api/ui/*)
// Headless UI driving so tooling can screenshot any app/state. All touch LVGL objects, so each
// takes the esp_lvgl_port lock (the httpd task is not the LVGL task). See nv_ui.h.

// GET /api/ui/state -> {"app":"<id>"} ("" at home).
esp_err_t h_ui_state(httpd_req_t *req) {
    char id[32] = "";
    if (lvgl_port_lock(1000)) { snprintf(id, sizeof id, "%s", nv_ui_current_app_id()); lvgl_port_unlock(); }
    char b[80]; snprintf(b, sizeof b, "{\"app\":\"%s\"}", id);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, b);
}

// GET /api/ui/open?id=<appid> -> open a native app (solo-mode). {"ok":bool,"app":"<current>"}.
esp_err_t h_ui_open(httpd_req_t *req) {
    char id[40];
    if (!query_param(req, "id", id, sizeof id)) return ESP_OK;
    bool ok = false; char cur[32] = "";
    if (lvgl_port_lock(3000)) {
        ok = nv_ui_open_app_id(id);
        snprintf(cur, sizeof cur, "%s", nv_ui_current_app_id());
        lvgl_port_unlock();
    }
    char b[112]; snprintf(b, sizeof b, "{\"ok\":%s,\"app\":\"%s\"}", ok ? "true" : "false", cur);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, b);
}

// GET /api/ui/home -> return to the launcher.
esp_err_t h_ui_home(httpd_req_t *req) {
    if (lvgl_port_lock(3000)) { nv_ui_go_home(); lvgl_port_unlock(); }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// GET /api/ui/tap?x=<0..1023>&y=<0..599> -> inject a synthetic pointer tap (drives tabs/buttons).
esp_err_t h_ui_tap(httpd_req_t *req) {
    char q[128]; int x = -1, y = -1;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(q, "x", v, sizeof v) == ESP_OK) x = atoi(v);
        if (httpd_query_key_value(q, "y", v, sizeof v) == ESP_OK) y = atoi(v);
    }
    if (x < 0 || y < 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need x,y"); return ESP_OK; }
    if (lvgl_port_lock(1000)) { nv_ui_tap(x, y); lvgl_port_unlock(); }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// GET /api/say?text=...&lang=it -> speak via nv_tts (diagnostic / remote voice trigger).
esp_err_t h_say(httpd_req_t *req) {
    char text[160] = "", lang[8] = "";
    if (!query_param(req, "text", text, sizeof text)) return ESP_OK;
    char q[256];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        char raw[16];
        if (httpd_query_key_value(q, "lang", raw, sizeof raw) == ESP_OK) url_decode(raw, lang, sizeof lang);
    }
    bool ok = nv_tts_say(text, lang[0] ? lang : nullptr);
    char msg[240];
    snprintf(msg, sizeof msg, "say('%s',%s)=%d available=%d", text, lang[0] ? lang : "def", ok, nv_tts_available());
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, msg);
}

// ---------------------------------------------------------------- lifecycle

void ensure_fs_home(void) {
    mkdir(FS_ROOT, 0777);
    char p[320];
    const char *dirs[] = { "/system", "/system/config", "/data", "/data/desktop" };
    for (auto d : dirs) { snprintf(p, sizeof p, "%s%s", FS_ROOT, d); mkdir(p, 0777); }
}

bool server_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    // MUST exceed the total registered handlers: the routes[] table + /ws + the "/*" catch-all.
    // esp_http_server silently drops registrations past this cap, and since "/*" (h_static) is
    // registered LAST, an undersized cap makes it vanish — every web page 404s ("Nothing matches
    // the given URI") while /api/* still works. Keep comfortably above the array size below.
    cfg.max_uri_handlers = 40;
    cfg.max_open_sockets = 8;          // browser opens ~6 parallel conns on boot; give it room
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    if (httpd_start(&s_srv, &cfg) != ESP_OK) { NV_LOGE(TAG, "httpd_start failed"); return false; }

    // Specific API routes first; the catch-all static "/*" is registered LAST so it only handles
    // whatever the API didn't claim (esp_http_server matches in registration order).
    const httpd_uri_t routes[] = {
        {"/api/info",        HTTP_GET,  h_info,        nullptr},
        {"/api/status",      HTTP_GET,  h_status,      nullptr},
        {"/api/auth/status", HTTP_GET,  h_auth_status, nullptr},
        {"/api/apps",        HTTP_GET,  h_apps,        nullptr},
        {"/api/associations",HTTP_GET,  h_assoc,       nullptr},
        {"/api/anima/caps",  HTTP_GET,  h_anima_caps,  nullptr},
        {"/api/anima",       HTTP_GET,  h_anima_get,   nullptr},
        {"/api/media/play",  HTTP_POST, h_media_play,  nullptr},
        {"/api/media/stop",  HTTP_POST, h_media_stop,  nullptr},
        {"/api/media/state", HTTP_GET,  h_media_state, nullptr},
        {"/api/video/play",  HTTP_POST, h_video_play,  nullptr},
        {"/api/video/stop",  HTTP_POST, h_video_stop,  nullptr},
        {"/api/video/state", HTTP_GET,  h_video_state, nullptr},
        {"/api/video/seek",  HTTP_POST, h_video_seek,  nullptr},
        {"/api/audio/selftest", HTTP_GET, h_audio_selftest, nullptr},
        {"/api/anima/query", HTTP_POST, h_anima_query, nullptr},
        {"/api/heap",        HTTP_GET,  h_heap,        nullptr},
        {"/api/cpu",         HTTP_GET,  h_cpu,         nullptr},
        {"/api/fs/list",     HTTP_GET,  h_fs_list,     nullptr},
        {"/api/fs/read",     HTTP_GET,  h_fs_read,     nullptr},
        {"/api/logs",        HTTP_GET,  h_logs,        nullptr},
        {"/api/screen",      HTTP_GET,  h_screen,      nullptr},
        {"/api/ui/state",    HTTP_GET,  h_ui_state,    nullptr},
        {"/api/ui/open",     HTTP_GET,  h_ui_open,     nullptr},
        {"/api/ui/home",     HTTP_GET,  h_ui_home,     nullptr},
        {"/api/ui/tap",      HTTP_GET,  h_ui_tap,      nullptr},
        {"/api/say",         HTTP_GET,  h_say,         nullptr},
        {"/api/fs/write",    HTTP_POST, h_fs_write,    nullptr},
        {"/api/fs/mkdir",    HTTP_POST, h_fs_mkdir,    nullptr},
        {"/api/fs/delete",   HTTP_POST, h_fs_delete,   nullptr},
        {"/api/fs/move",     HTTP_POST, h_fs_move,     nullptr},
        {"/api/time/set",    HTTP_POST, h_time_set,    nullptr},
        {"/api/reboot",      HTTP_POST, h_reboot,      nullptr},
        {"/api/app/run",     HTTP_POST, h_app_run,     nullptr},
        {"/api/web/put",     HTTP_POST, h_web_put,     nullptr},
        {"/api/wifi/scan",   HTTP_GET,  h_wifi_scan,   nullptr},
        {"/api/wifi/known",  HTTP_GET,  h_wifi_known,  nullptr},
        {"/api/wifi/join",   HTTP_POST, h_wifi_join,   nullptr},
    };
    for (auto &r : routes) httpd_register_uri_handler(s_srv, &r);

    // /ws (WebSocket) then the catch-all static "/*" MUST register last, in this order: the wildcard
    // would otherwise swallow /ws (first-match wins).
    httpd_uri_t ws = {};
    ws.uri = "/ws"; ws.method = HTTP_GET; ws.handler = h_ws; ws.is_websocket = true;
    httpd_register_uri_handler(s_srv, &ws);
    httpd_uri_t star = {};
    star.uri = "/*"; star.method = HTTP_GET; star.handler = h_static;
    httpd_register_uri_handler(s_srv, &star);
    return true;
}

void mdns_announce(void) {
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("nucleov2");
        mdns_instance_name_set("NucleoOS");
    }
    mdns_service_add("NucleoOS Web", "_http", "_tcp", 80, nullptr, 0);
}

void web_task(void *) {
    ensure_fs_home();
    cache_build();   // slurp the web tree into PSRAM (SD is mounted early in app_main, before us)
    for (;;) {
        while (nv_wifi_get_state() != NV_WIFI_CONNECTED) vTaskDelay(pdMS_TO_TICKS(1000));
        if (server_start()) {
            mdns_announce();
            NV_LOGI(TAG, "web OS up: http://nucleov2.local/  (docroot %s, fs %s)", WEB_ROOT, FS_ROOT);
            vTaskDelete(nullptr);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

}  // namespace

void nv_web_init(void) {
    // ANIMA's PSRAM file mirrors (up to 12 MB) are the biggest rebuildable cache on the device;
    // hand them to the memory broker so a RAM-heavy launch (camera: 4×4 MB contiguous) evicts
    // them instead of failing. Registered here, not in the engine — nv_anima stays kernel-free.
    nv_mem_reclaimer_add("anima-l1-mirrors",
                         [](void *) { return nucleo_anima_l1_cache_flush_if_idle(); }, nullptr);
    // Stack stays INTERNAL: web_task self-deletes with vTaskDelete() once the server is up, which
    // is incompatible with a caps-allocated (PSRAM) stack — keep the plain internal creation.
    // 12 KB: cache_build() recurses the web tree; the FATFS calls in the walk want headroom.
    xTaskCreate(web_task, "nv_web", 12288, nullptr, 4, nullptr);
}
