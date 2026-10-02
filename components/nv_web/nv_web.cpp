// nv_web — NucleoOS web-OS host: static shell/apps from SD + the REST API the browser shell
// (recovered from the Cardputer project) speaks. Serves the whole "NucleoOS web" desktop over
// Wi-Fi so a phone/PC browser is a companion OS for the board. Endpoint contract in the header.
#include "nv_hid_host.h"   // /api/ui/hid: synthetic physical-keyboard keys
#include "nv_web.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <strings.h>   // strcasecmp / strncasecmp (Origin check)
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"   // /api/intr: esp_intr_dump
#include "nvs.h"              // /api/bench/nvs
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "mdns.h"

#include "nv_log.h"
#include "nv_config.h"
#include "nv_seclog.h"   // /api/security/events
#include "nv_wake.h"     // /api/anima/wake (hands-free ANIMA)
#include "nv_sealed.h"   // /api/fs on secret files: plaintext over the paired link, sealed on the card
#include "nv_mqtt.h"             // /api/home: MQTT state + node id
#include "nv_wifi.h"
#include "nv_time.h"
#include "nv_memory_broker.h"
#include "nv_wasm.h"
#include "nv_hal.h"
#include "nv_sysmon.h"
#include "nv_tts.h"
#include "nucleo_anima.h"
#include "nucleo_anima_conv.h"   // conversations + user memory (assistant layer)
#include "cJSON.h"               // body parsing for the conv/memory/chat POST endpoints
#include "nv_anima_system.h"   // shared ANIMA_ACT_SYSTEM {value} resolver (nv_apps)
#include "nv_media.h"
#include "nv_vplayer.h"
#include "nv_audio.h"
#include "nv_app.h"        // /api/anima/query LAUNCH -> open the app on the panel
#include "nv_ui.h"         // /api/ui/* remote automation (open/home/tap/state)
#include "nv_ime.h"        // /api/ui/type, /api/ui/key: text/key injection into the focused field
#include "nv_open.h"       // /api/open: open a file on the device (file associations)
#include "nv_term.h"       // /api/term/*: drive the Terminal by text
#include "nv_usb_storage.h"   // /api/usb + /mnt/usbN in the fs API
#include "nv_pad.h"         // /api/pads: connected game controllers (all transports)
#include "nv_bt.h"          // /api/bt: Bluetooth LE pads (scan / pair / forget)
#include "nv_web_util.h"      // WEB_ROOT/FS_ROOT + the LAN-input path/JSON helpers (host-tested)
#include "nv_sd.h"         // removal-safe fopen/fclose for every docroot/FS read+write
#include "nv_crash.h"      // /api/info + /api/crash: stored core dump (summary + raw image)
#include "nv_irqwatch.h"   // /api/crash: interrupt-storm sentinel report
#include "nv_disp.h"       // /api/display: compositor stats
#include "nv_mem_attr.h"   // NV_PSRAM_BSS: the handler scratch statics (~50 KB) out of internal SRAM
#include "nv_auth.h"       // pairing + session tokens: every /api route except a public few
#include "esp_lvgl_port.h"
#include "lwip/sockets.h"  // getpeername: who is asking to pair

// /ws is authenticated before the WebSocket handshake; without this option esp_http_server would
// upgrade any client first. An sdkconfig older than the default keeps "# ... is not set".
#if !CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
#  error "CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT must be y (sdkconfig.defaults): set it in sdkconfig, or delete sdkconfig to regenerate it"
#endif

static const char *TAG = "web";

// WEB_ROOT / FS_ROOT and the pure path/JSON helpers (url_decode, map_fs, fs_writable, json_*,
// mime_for, open_path_ok) live in nv_web_util: they parse LAN input and are unit-tested and fuzzed
// on the PC (tests/host).
using namespace nv_web_util;

namespace {

httpd_handle_t s_srv = nullptr;

// ---------------------------------------------------------------- small utils

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
    // PSRAM: bodies under 16 KB used to land in internal SRAM (SPIRAM_MALLOC_ALWAYSINTERNAL).
    char *buf = (char *)heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (char *)malloc(req->content_len + 1);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return nullptr; }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            free(buf);
            // Always answer: a silent return left the keep-alive connection hanging until the client's own timeout.
            httpd_resp_send_err(req, r == HTTPD_SOCK_ERR_TIMEOUT ? HTTPD_408_REQ_TIMEOUT : HTTPD_400_BAD_REQUEST, "body read failed");
            return nullptr;
        }
        got += (size_t)r;
    }
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
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
    char        url[160];   // URL path key, e.g. "/shell.js" (longest in the tree today: 44 chars)
    uint8_t    *data;       // PSRAM buffer
    size_t      len;
    const char *mime;       // static string
    bool        gz;         // data is gzip-encoded (serve with Content-Encoding: gzip)
};

CachedFile *s_cache = nullptr;
int         s_cache_n = 0;
int         s_cache_cap = 0;
size_t      s_cache_bytes = 0;

// Only the small hot files (shell html/css/js, icons) live in PSRAM; big assets (wallpapers, 3D models,
// vendor libs, .wasm) stream from SD at 40 MHz - fast enough, and it saves MBs of PSRAM for apps.
constexpr size_t kMaxCacheFile  = 96u * 1024;           // files bigger than this stream from SD
constexpr size_t kMaxCacheTotal = 2u * 1024 * 1024;     // ceiling for the whole cache

CachedFile *cache_find(const char *url) {
    for (int i = 0; i < s_cache_n; i++)
        if (!strcmp(s_cache[i].url, url)) return &s_cache[i];
    return nullptr;
}

// Insert or update a cache entry. `data` is PSRAM-owned on success; freed here on skip. A gz variant
// always wins (and updates content); a non-gz only replaces a non-gz — so a live /api/web/put of the
// new gz twin refreshes what's served, without a non-gz push ever downgrading a cached gz.
void cache_put(const char *url, uint8_t *data, size_t len, bool gz) {
    if (strlen(url) >= sizeof(CachedFile::url)) { free(data); return; }   // too long to key: SD-served
    CachedFile *ex = cache_find(url);
    if (len > kMaxCacheFile || (!ex && s_cache_bytes + len > kMaxCacheTotal)) {   // SD-served from now on
        if (ex && (gz || !ex->gz)) {   // drop the stale copy so the new file on SD is what's served
            s_cache_bytes -= ex->len;
            free(ex->data);
            *ex = s_cache[--s_cache_n];
        }
        free(data);
        return;
    }
    if (ex) {
        if (gz || !ex->gz) { s_cache_bytes = s_cache_bytes - ex->len + len; free(ex->data); ex->data = data; ex->len = len; ex->gz = gz; ex->mime = mime_for(url); }
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
    s_cache_bytes += len;
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
                    // A plain file with a cacheable .gz twin never stays in RAM (cache_put lets the
                    // gz entry replace it; gzip-less clients stream from SD), so don't read it: that
                    // was ~4 MB of allocate-then-free at boot, fragmenting the PSRAM that the camera's
                    // 4 MB contiguous frame buffers need.
                    bool twin = false;
                    const size_t pl = plen + (size_t)pn;
                    if (!gz && pl + 4 <= pcap) {
                        memcpy(pbuf + pl, ".gz", 4);
                        struct stat gs{};
                        twin = stat(pbuf, &gs) == 0 && S_ISREG(gs.st_mode) && (size_t)gs.st_size <= kMaxCacheFile;
                        pbuf[pl] = '\0';
                    }
                    FILE *f = twin ? nullptr : nv_sd_fopen(pbuf, "rb");
                    if (f) {
                        uint8_t *buf = (uint8_t *)heap_caps_malloc(st.st_size ? st.st_size : 1, MALLOC_CAP_SPIRAM);
                        if (buf) {
                            size_t got = fread(buf, 1, st.st_size, f);
                            if (got == (size_t)st.st_size) cache_put(ubuf, buf, got, gz);
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
void sec_headers(httpd_req_t *req);   // below

esp_err_t h_static(httpd_req_t *req) {
    sec_headers(req);
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

// Why this boot happened, as a stable token. Lets automation (tools/hil/smoke.py) tell a crash or
// watchdog reset from a power cycle / OTA restart without a serial cable.
const char *reset_reason_str(void) {
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:    return "poweron";
    case ESP_RST_EXT:        return "ext";
    case ESP_RST_SW:         return "sw";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "int_wdt";
    case ESP_RST_TASK_WDT:   return "task_wdt";
    case ESP_RST_WDT:        return "wdt";
    case ESP_RST_DEEPSLEEP:  return "deepsleep";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_SDIO:       return "sdio";
    case ESP_RST_USB:        return "usb";
    case ESP_RST_JTAG:       return "jtag";
    case ESP_RST_EFUSE:      return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default:                 return "unknown";
    }
}

// GET /api/info -> identity + health. "reset_reason" is this boot's cause; "crash" is the core dump
// still stored in flash ({task,pc,reason,this_build}, or null) — it survives until Diagnostics
// clears it, so it can predate this boot (this_build=false: an older image). Details: /api/crash.
esp_err_t h_info(httpd_req_t *req) {
    const esp_app_desc_t *ad = esp_app_get_description();
    char ip[16] = "?";
    esp_netif_ip_info_t ipi;
    esp_netif_t *nif = esp_netif_get_default_netif();
    if (nif && esp_netif_get_ip_info(nif, &ipi) == ESP_OK) snprintf(ip, sizeof ip, IPSTR, IP2STR(&ipi.ip));
    char crash[160] = "null";
    nv_crash_info_t ci;
    if (nv_crash_get(&ci)) {
        char task[40];
        json_escape(task, sizeof task, ci.task);
        snprintf(crash, sizeof crash, "{\"task\":\"%s\",\"pc\":\"0x%08lx\",\"reason\":\"%s\",\"this_build\":%s}",
                 task, (unsigned long)ci.pc, ci.reason, ci.this_build ? "true" : "false");
    }
    char body[528];
    snprintf(body, sizeof body,
             "{\"name\":\"NucleoOS\",\"version\":\"%s\",\"ip\":\"%s\",\"uptime_s\":%lld,"
             "\"sram_free_kb\":%u,\"psram_free_kb\":%u,\"abi\":%d,\"reset_reason\":\"%s\",\"crash\":%s}",
             ad->version, ip, (long long)(esp_timer_get_time() / 1000000),
             (unsigned)(nv_mem_free_internal() / 1024), (unsigned)(nv_mem_free_psram() / 1024), NV_WASM_ABI,
             reset_reason_str(), crash);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// GET /api/crash -> the stored core dump's panic frame, or {"present":false}. "reason" decodes
// mcause (int_wdt_cpu0 = CPU0 stopped ticking for CONFIG_ESP_INT_WDT_TIMEOUT_MS; "task" is then
// only the task that CPU0 had interrupted). "elf_sha" names the image that crashed: addr2line /
// espcoredump are only valid against that exact ELF ("this_build" says whether it is the running one).
// "storm" is nv_irqwatch's report: which interrupt sources were asserted while a CPU had stopped
// ticking (from the boot that reset, or from this boot if a storm resolved on its own), or null.
// GET /api/security/events -> {"encrypted":bool,"events":[{"t":unix|0,"up":s,"id":"pair_wrong",
// "detail":"..."}, ...]} newest first (nv_seclog: RAM ring since this boot). Paired clients only.
esp_err_t h_sec_events(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    char chunk[192];
    snprintf(chunk, sizeof chunk, "{\"encrypted\":%s,\"events\":[", nv_config_encrypted() ? "true" : "false");
    httpd_resp_sendstr_chunk(req, chunk);
    const int n = nv_seclog_count();
    for (int i = 0; i < n; i++) {
        nv_sec_entry_t ev;
        if (!nv_seclog_get(i, &ev)) break;
        char det[NV_SECLOG_DETAIL_MAX * 2];
        json_escape(det, sizeof det, ev.detail);
        snprintf(chunk, sizeof chunk, "%s{\"t\":%lu,\"up\":%lu,\"id\":\"%s\",\"detail\":\"%s\"}",
                 i ? "," : "", (unsigned long)ev.unix_time, (unsigned long)ev.uptime_s,
                 nv_seclog_code_id((nv_sec_event_t)ev.code), det);
        httpd_resp_sendstr_chunk(req, chunk);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, nullptr);
}

esp_err_t h_crash(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    char storm[640] = "null";
    nv_irqwatch_report_t w;
    if (nv_irqwatch_get(&w)) {
        char now[160], late[160];
        nv_irqwatch_sources(w.src, now, sizeof now);
        nv_irqwatch_sources(w.src_late, late, sizeof late);
        snprintf(storm, sizeof storm,
                 "{\"from_last_boot\":%s,\"cpu\":%u,\"events\":%lu,\"asserted\":\"%s\",\"asserted_late\":\"%s\","
                 "\"i2c0\":[%lu,%lu,%lu],\"dw_gdma\":[%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu],"
                 "\"dma2d_out\":[%lu,%lu,%lu,%lu,%lu,%lu],\"dma2d_in\":[%lu,%lu,%lu,%lu]}",
                 w.from_last_boot ? "true" : "false", (unsigned)w.cpu, (unsigned long)w.events, now, late,
                 (unsigned long)w.i2c0[0], (unsigned long)w.i2c0[1], (unsigned long)w.i2c0[2],
                 (unsigned long)w.dwg[0], (unsigned long)w.dwg[1], (unsigned long)w.dwg[2],
                 (unsigned long)w.dwg[3], (unsigned long)w.dwg[4], (unsigned long)w.dwg[5],
                 (unsigned long)w.dwg[6], (unsigned long)w.dwg[7], (unsigned long)w.dwg[8],
                 (unsigned long)w.d2d_out[0], (unsigned long)w.d2d_out[1], (unsigned long)w.d2d_out[2],
                 (unsigned long)w.d2d_out[3], (unsigned long)w.d2d_out[4], (unsigned long)w.d2d_out[5],
                 (unsigned long)w.d2d_in[0], (unsigned long)w.d2d_in[1], (unsigned long)w.d2d_in[2],
                 (unsigned long)w.d2d_in[3]);
    }
    nv_crash_info_t ci;
    if (!nv_crash_get(&ci)) {
        char body[704];
        snprintf(body, sizeof body, "{\"present\":false,\"storm\":%s}", storm);
        return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    }
    char task[40];
    json_escape(task, sizeof task, ci.task);
    char body[1088];
    snprintf(body, sizeof body,
             "{\"present\":true,\"task\":\"%s\",\"reason\":\"%s\",\"mcause\":%lu,\"pc\":\"0x%08lx\","
             "\"ra\":\"0x%08lx\",\"sp\":\"0x%08lx\",\"mtval\":\"0x%08lx\",\"elf_sha\":\"%s\","
             "\"this_build\":%s,\"size\":%lu,\"dump\":\"/api/crash/dump\",\"storm\":%s}",
             task, ci.reason, (unsigned long)ci.mcause, (unsigned long)ci.pc, (unsigned long)ci.ra,
             (unsigned long)ci.sp, (unsigned long)ci.mtval, ci.elf_sha, ci.this_build ? "true" : "false",
             (unsigned long)ci.size, storm);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// GET /api/crash/dump -> the raw core dump image (flash format), for tools\decode-coredump.ps1 -Url:
// the full decode without a serial cable and without resetting the board. Flash reads: httpd runs on
// an internal-stack task (ENGINEERING_RULES §2).
esp_err_t h_crash_dump(httpd_req_t *req) {
    if (!nv_crash_get(nullptr)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no core dump");
    constexpr size_t kChunk = 4096;
    uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(kChunk, MALLOC_CAP_INTERNAL));
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");
    esp_err_t r = ESP_OK;
    for (uint32_t off = 0;;) {
        const int n = nv_crash_read(off, buf, kChunk);
        if (n <= 0) { if (n < 0) r = ESP_FAIL; break; }
        if ((r = httpd_resp_send_chunk(req, (const char *)buf, n)) != ESP_OK) break;
        off += (uint32_t)n;
    }
    heap_caps_free(buf);
    if (r != ESP_OK) return r;   // client gone / flash error: drop the connection, no terminator
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// GET /api/intr -> esp_intr_dump(): every CPU interrupt line and the sources sharing it. The only
// way to name the peripherals behind a shared line (e.g. the one an int_wdt core dump caught in
// shared_intr_isr) — the dump itself does not carry the handler list.
esp_err_t h_intr(httpd_req_t *req) {
    char *text = nullptr;
    size_t len = 0;
    FILE *f = open_memstream(&text, &len);
    if (!f) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    esp_intr_dump(f);
    fclose(f);
    httpd_resp_set_type(req, "text/plain");
    const esp_err_t r = httpd_resp_send(req, text, (ssize_t)len);
    free(text);
    return r;
}

// GET /api/display -> nv_disp's compositor state. "fps" = frames presented and "refresh_hz" = panel
// refreshes per second since the previous call (poll twice, >= 1 s apart, around what you measure).
// "violations" must stay 0 (LVGL never presents the buffer on screen); "vsync_timeouts" > 0 means
// the panel stopped refreshing at some point.
esp_err_t h_display(httpd_req_t *req) {
    static uint32_t s_prev_swaps = 0, s_prev_vsyncs = 0;
    static int64_t  s_prev_t = 0;
    nv_disp_stats_t st;
    nv_disp_get_stats(&st);
    const int64_t now = esp_timer_get_time();
    const double dt = s_prev_t ? (double)(now - s_prev_t) / 1e6 : 0.0;
    const double fps = dt > 0 ? (double)(st.swaps - s_prev_swaps) / dt : 0.0;
    const double hz = dt > 0 ? (double)(st.vsyncs - s_prev_vsyncs) / dt : 0.0;
    s_prev_swaps = st.swaps;
    s_prev_vsyncs = st.vsyncs;
    s_prev_t = now;
    char body[600];
    snprintf(body, sizeof body,
             "{\"mode\":\"%s\",\"rotation\":%d,\"swaps\":%lu,\"vsyncs\":%lu,\"fps\":%.1f,"
             "\"refresh_hz\":%.1f,\"window_s\":%.1f,\"vsync_timeouts\":%lu,\"violations\":%lu,"
             "\"wait_us_avg\":%lu,\"wait_us_max\":%lu,\"present_us_avg\":%lu,\"sync_us_avg\":%lu,"
             "\"render_us_avg\":%lu,\"frame_us_avg\":%lu,\"layer_draws\":%lu,\"layer_copies\":%lu,"
             "\"layer_draw_us_avg\":%lu,\"layer_copy_us_avg\":%lu}",
             st.rotated ? "rotated" : "direct", st.rotation * 90, (unsigned long)st.swaps,
             (unsigned long)st.vsyncs, fps, hz, dt, (unsigned long)st.vsync_timeouts,
             (unsigned long)st.violations, (unsigned long)st.wait_us_avg, (unsigned long)st.wait_us_max,
             (unsigned long)st.present_us_avg, (unsigned long)st.sync_us_avg,
             (unsigned long)st.render_us_avg, (unsigned long)st.frame_us_avg,
             (unsigned long)st.layer_draws, (unsigned long)st.layer_copies,
             (unsigned long)st.layer_draw_us_avg, (unsigned long)st.layer_copy_us_avg);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// GET /api/usb -> {"volumes":[...],"bus":[...]} — USB drives (/mnt/usbN in the fs API) + every
// device on the bus, hubs included (diagnoses "why doesn't X work behind my hub").
esp_err_t h_usb(httpd_req_t *req) {
    static const char *kState[] = {"empty", "mounted", "unformatted", "ejected", "error"};
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"volumes\":[");
    nv_usb_stor_info_t v[NV_USB_STOR_SLOTS];
    const int n = nv_usb_storage_list(v, NV_USB_STOR_SLOTS);
    char item[512], label[48], vendor[20], product[36], manu[48];
    for (int i = 0; i < n; i++) {
        json_escape(label, sizeof label, v[i].label);
        json_escape(vendor, sizeof vendor, v[i].vendor);
        json_escape(product, sizeof product, v[i].product);
        const bool known_free = v[i].free_bytes != UINT64_MAX;
        snprintf(item, sizeof item,
                 "%s{\"path\":\"/mnt%s\",\"state\":\"%s\",\"label\":\"%s\",\"fs\":\"%s\","
                 "\"total\":%llu,\"free\":%lld,\"read_only\":%s,\"removable\":%s,\"vendor\":\"%s\","
                 "\"product\":\"%s\",\"vid\":\"%04x\",\"pid\":\"%04x\",\"addr\":%u,\"lun\":%u,\"speed\":%u}",
                 i ? "," : "", v[i].path, kState[v[i].state < 5 ? v[i].state : 4], label, v[i].fs,
                 (unsigned long long)v[i].total_bytes, known_free ? (long long)v[i].free_bytes : -1LL,
                 v[i].read_only ? "true" : "false", v[i].removable ? "true" : "false", vendor, product,
                 v[i].vid, v[i].pid, v[i].addr, v[i].lun, v[i].speed);
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "],\"bus\":[");
    nv_usb_bus_dev_t b[16];
    const int nb = nv_usb_bus_list(b, 16);
    for (int i = 0; i < nb; i++) {
        json_escape(manu, sizeof manu, b[i].manufacturer);
        json_escape(product, sizeof product, b[i].product);
        char ifs[40] = "";
        size_t l = 0;
        for (int k = 0; k < 6 && b[i].if_classes[k] != 0xFF; k++)
            l += snprintf(ifs + l, sizeof ifs - l, "%s%u", k ? "," : "", b[i].if_classes[k]);
        snprintf(item, sizeof item,
                 "%s{\"addr\":%u,\"parent\":%u,\"port\":%u,\"speed\":%u,\"vid\":\"%04x\",\"pid\":\"%04x\","
                 "\"class\":%u,\"ifaces\":[%s],\"manufacturer\":\"%s\",\"product\":\"%s\"}",
                 i ? "," : "", b[i].addr, b[i].parent_addr, b[i].port, b[i].speed, b[i].vid, b[i].pid,
                 b[i].dev_class, ifs, manu, product);
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, nullptr);
}

// POST /api/usb/eject?path=/mnt/usbN -> {"ok":bool} (false = files still open)
esp_err_t h_usb_eject(httpd_req_t *req) {
    char path[32];
    if (!query_param(req, "path", path, sizeof path)) return ESP_OK;
    const int slot = nv_usb_storage_slot_of(!strncmp(path, "/mnt/", 5) ? path + 4 : path);
    if (slot < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    const bool ok = nv_usb_storage_eject(slot);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// ---------------------------------------------------------------- game controllers + Bluetooth

const char *pad_source_name(uint8_t s) {
    switch (s) {
        case NV_PAD_SRC_USB_HID: return "usb";
        case NV_PAD_SRC_XINPUT:  return "xinput";
        case NV_PAD_SRC_BLE:     return "ble";
        default:                 return "none";
    }
}

// GET /api/pads -> {"count":n,"pads":[{"name","source","vid","pid","mapped","battery","rumble",
// "buttons":<NV_PADB_* mask>,"axes":[lx,ly,rx,ry,lt,rt]}]} — live state, player order.
esp_err_t h_pads(httpd_req_t *req) {
    const int n = nv_pad_count();
    char item[320], name[96];
    snprintf(item, sizeof item, "{\"count\":%d,\"pads\":[", n);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, item);
    bool first = true;
    for (int i = 0; i < n; i++) {
        nv_pad_input_t in;
        nv_pad_info_t inf;
        if (!nv_pad_get(i, &in, &inf)) continue;
        inf.name[sizeof inf.name - 1] = '\0';
        json_escape(name, sizeof name, inf.name);
        snprintf(item, sizeof item,
                 "%s{\"name\":\"%s\",\"source\":\"%s\",\"vid\":\"%04x\",\"pid\":\"%04x\",\"mapped\":%s,"
                 "\"battery\":%d,\"rumble\":%s,\"buttons\":%lu,\"axes\":[%d,%d,%d,%d,%d,%d]}",
                 first ? "" : ",", name, pad_source_name(inf.source), inf.vid, inf.pid,
                 inf.mapped ? "true" : "false", inf.battery == 255 ? -1 : (int)inf.battery,
                 inf.rumble ? "true" : "false", (unsigned long)in.buttons,
                 in.axis[NV_PADA_LX], in.axis[NV_PADA_LY], in.axis[NV_PADA_RX], in.axis[NV_PADA_RY],
                 in.axis[NV_PADA_LT], in.axis[NV_PADA_RT]);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, nullptr);
}

const char *bt_state_name(nv_bt_state_t s) {
    switch (s) {
        case NV_BT_OFF:        return "off";
        case NV_BT_STARTING:   return "starting";
        case NV_BT_READY:      return "ready";
        case NV_BT_SCANNING:   return "scanning";
        case NV_BT_CONNECTING: return "connecting";
        case NV_BT_ERROR:      return "error";
        default:               return "?";
    }
}

// Scan + paired snapshots for one request (~3 KB PSRAM: static, httpd runs one handler at a time).
constexpr int kBtScanN = 48;
NV_PSRAM_BSS nv_bt_device_t s_bt_scan[kBtScanN];
NV_PSRAM_BSS nv_bt_device_t s_bt_paired[8];
bool s_bt_conn[8];

// GET /api/bt -> {"enabled","state","error","busy","connected","own_addr","paired":[{"addr","type",
// "name","connected","kind"}],"scan":[{"addr","type","name","rssi","appearance","hid","paired","kind",
// "company" (null = no manufacturer data),"connectable"}]}. kind: nv_bt_kind_id().
esp_err_t h_bt_get(httpd_req_t *req) {
    nv_bt_status_t st;
    nv_bt_status(&st);
    st.error[sizeof st.error - 1] = '\0';
    st.busy_name[sizeof st.busy_name - 1] = '\0';
    char item[360], e1[140], e2[72], addr[18], own[18] = "";
    json_escape(e1, sizeof e1, st.error);
    json_escape(e2, sizeof e2, st.busy_name);
    uint8_t oa[6];
    if (nv_bt_own_addr(oa)) nv_bt_addr_str(oa, own);
    snprintf(item, sizeof item,
             "{\"enabled\":%s,\"state\":\"%s\",\"error\":\"%s\",\"busy\":\"%s\",\"connected\":%u,"
             "\"own_addr\":\"%s\",\"paired\":[",
             nv_bt_is_enabled() ? "true" : "false", bt_state_name(st.state), e1, e2,
             (unsigned)st.n_connected, own);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, item);

    const int np = nv_bt_paired(s_bt_paired, s_bt_conn, 8);
    for (int i = 0; i < np; i++) {
        nv_bt_device_t &d = s_bt_paired[i];
        d.name[sizeof d.name - 1] = '\0';
        nv_bt_addr_str(d.addr, addr);
        json_escape(e2, sizeof e2, d.name);
        snprintf(item, sizeof item,
                 "%s{\"addr\":\"%s\",\"type\":%u,\"name\":\"%s\",\"connected\":%s,\"kind\":\"%s\"}",
                 i ? "," : "", addr, (unsigned)d.addr_type, e2, s_bt_conn[i] ? "true" : "false",
                 nv_bt_kind_id(nv_bt_kind(&d)));
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "],\"scan\":[");
    const int ns = nv_bt_scan_results(s_bt_scan, kBtScanN);
    for (int i = 0; i < ns; i++) {
        nv_bt_device_t &d = s_bt_scan[i];
        d.name[sizeof d.name - 1] = '\0';
        nv_bt_addr_str(d.addr, addr);
        json_escape(e2, sizeof e2, d.name);
        char co[8] = "null";
        if (d.company != 0xFFFF) snprintf(co, sizeof co, "%u", (unsigned)d.company);
        snprintf(item, sizeof item,
                 "%s{\"addr\":\"%s\",\"type\":%u,\"name\":\"%s\",\"rssi\":%d,\"appearance\":%u,"
                 "\"hid\":%s,\"paired\":%s,\"kind\":\"%s\",\"company\":%s,\"connectable\":%s}",
                 i ? "," : "", addr, (unsigned)d.addr_type, e2, (int)d.rssi, (unsigned)d.appearance,
                 d.hid ? "true" : "false", d.paired ? "true" : "false", nv_bt_kind_id(nv_bt_kind(&d)), co,
                 d.connectable ? "true" : "false");
        httpd_resp_sendstr_chunk(req, item);
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, nullptr);
}

// POST /api/bt  action=on|off|scan|stop|connect|forget [&addr=aa:bb:..&type=0|1] as query params
// or a JSON body {"action":..,"addr":..,"type":..}; a missing type is looked up in the scan
// results (connect) / bond list (forget). -> {"ok":bool}. Everything is async: poll GET /api/bt.
esp_err_t h_bt_post(httpd_req_t *req) {
    char action[12] = "", addr_s[24] = "", type_s[4] = "";
    query_param_opt(req, "action", action, sizeof action);
    query_param_opt(req, "addr", addr_s, sizeof addr_s);
    query_param_opt(req, "type", type_s, sizeof type_s);
    long type = type_s[0] ? atol(type_s) : -1;
    if (req->content_len > 0) {
        size_t len = 0;
        char *body = recv_body(req, 256, &len);
        if (!body) return ESP_OK;
        if (!action[0]) json_str(body, "action", action, sizeof action);
        if (!addr_s[0]) json_str(body, "addr", addr_s, sizeof addr_s);
        if (type < 0) type = json_int(body, "type", -1);
        free(body);
    }

    bool ok = false;
    if (!strcmp(action, "on"))        { nv_bt_set_enabled(true); ok = true; }
    else if (!strcmp(action, "off"))  { nv_bt_set_enabled(false); ok = true; }
    else if (!strcmp(action, "scan")) ok = nv_bt_scan_start(15);
    else if (!strcmp(action, "stop")) { nv_bt_scan_stop(); ok = true; }
    else if (!strcmp(action, "connect") || !strcmp(action, "forget")) {
        uint8_t a[6];
        if (!nv_bt_addr_parse(addr_s, a)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad addr");
        const bool conn = action[0] == 'c';
        if (type < 0) {   // resolve the address type from what the stack already knows
            const int n = conn ? nv_bt_scan_results(s_bt_scan, kBtScanN) : nv_bt_paired(s_bt_paired, s_bt_conn, 8);
            const nv_bt_device_t *list = conn ? s_bt_scan : s_bt_paired;
            for (int i = 0; i < n && type < 0; i++)
                if (!memcmp(list[i].addr, a, 6)) type = list[i].addr_type;
            if (type < 0) type = 0;
        }
        ok = conn ? nv_bt_connect(a, (uint8_t)type) : nv_bt_forget(a, (uint8_t)type);
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "action: on|off|scan|stop|connect|forget");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
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

// ---------------------------------------------------------------- authentication
// Every /api route except kPublicRoutes and the /ws upgrade require a paired session: the nv_s
// cookie (browsers, set by POST /api/pair) or "Authorization: Bearer <token>" (tools). Static web
// files stay public; they hold no user data.

// Header buffers live in this frame, which is gone before the real handler runs (8 KB httpd stack).
__attribute__((noinline)) bool req_authed(httpd_req_t *req) {
    char authz[96] = "", cookie[512] = "";
    if (httpd_req_get_hdr_value_str(req, "Authorization", authz, sizeof authz) != ESP_OK) authz[0] = '\0';
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof cookie) != ESP_OK) cookie[0] = '\0';
    return nv_auth_check_headers(authz, cookie);
}

esp_err_t send_unauthorized(httpd_req_t *req) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer realm=\"NucleoOS\"");
    return httpd_resp_send(req, "{\"error\":\"unauthorized\",\"pair\":\"/api/pair\"}", HTTPD_RESP_USE_STRLEN);
}

using Handler = esp_err_t (*)(httpd_req_t *);

// ---------------------------------------------------------------- request hardening
// Sent with every response. Literals only: httpd keeps the pointers until the response goes out.
void sec_headers(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");                      // no clickjacking frames
    httpd_resp_set_hdr(req, "Content-Security-Policy", "frame-ancestors 'none'");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
}

// A name the device can legitimately be reached by on a LAN: an IP literal, a bare host name, or a
// local-only suffix. Anything else is a DNS-rebinding page (a public domain its owner re-pointed at
// this IP, so the browser treats our API as same-origin with it), refused before the API runs.
bool host_is_local(const char *host) {
    if (!host[0] || host[0] == '[') return true;               // no Host (HTTP/1.0 tools) / IPv6 literal
    char name[96];
    size_t n = 0;
    for (; host[n] && host[n] != ':' && n < sizeof name - 1; n++) name[n] = (char)tolower((unsigned char)host[n]);
    name[n] = '\0';
    if (n && name[n - 1] == '.') name[--n] = '\0';            // "nucleov2.local." is the same name
    bool ipv4 = n > 0, dot = false;
    for (size_t i = 0; i < n; i++) {
        if (name[i] == '.') dot = true;
        else if (!isdigit((unsigned char)name[i])) ipv4 = false;
    }
    if (ipv4 || !dot) return true;
    static const char *const kLocal[] = {".local", ".lan", ".home", ".home.arpa", ".internal", ".localdomain"};
    for (const char *s : kLocal) {
        const size_t l = strlen(s);
        if (n > l && strcmp(name + n - l, s) == 0) return true;
    }
    return false;
}

// Cross-site guard: a browser sends Origin on every cross-origin POST and on WebSocket handshakes;
// it must name the host the request was sent to. No Origin = a tool or a same-origin navigation.
bool origin_matches(const char *origin, const char *host) {
    if (!origin[0]) return true;
    const char *o = origin;
    if (strncasecmp(o, "http://", 7) == 0) o += 7;
    else if (strncasecmp(o, "https://", 8) == 0) o += 8;
    else return false;                                         // "null" (sandboxed/file pages) and others
    return strcasecmp(o, host) == 0;
}

// Host/Origin checks for /api and /ws. Buffers in this frame, gone before the handler runs.
__attribute__((noinline)) bool req_origin_ok(httpd_req_t *req, bool check_origin) {
    char host[128] = "", origin[160] = "";
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) != ESP_OK) host[0] = '\0';
    if (!host_is_local(host)) {
        NV_LOGW(TAG, "refused %s: Host \"%.40s\" is not a local name", req->uri, host);
        return false;
    }
    if (!check_origin) return true;
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof origin) != ESP_OK) origin[0] = '\0';
    if (origin_matches(origin, host)) return true;
    NV_LOGW(TAG, "refused %s: cross-site Origin \"%.40s\"", req->uri, origin);
    return false;
}

esp_err_t send_forbidden(httpd_req_t *req) {
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"error\":\"forbidden\"}", HTTPD_RESP_USE_STRLEN);
}

// Every /api route runs behind one of these two; the real handler rides in user_ctx. State-changing
// methods must also come from our own origin.
esp_err_t h_open(httpd_req_t *req) {
    sec_headers(req);
    if (!req_origin_ok(req, req->method != HTTP_GET)) return send_forbidden(req);
    return reinterpret_cast<Handler>(req->user_ctx)(req);
}

// Registered in place of every non-public handler.
esp_err_t h_guarded(httpd_req_t *req) {
    sec_headers(req);
    if (!req_origin_ok(req, req->method != HTTP_GET)) return send_forbidden(req);
    if (!req_authed(req)) return send_unauthorized(req);
    return reinterpret_cast<Handler>(req->user_ctx)(req);
}

esp_err_t ws_pre_handshake(httpd_req_t *req) {
    sec_headers(req);
    if (!req_origin_ok(req, true)) { send_forbidden(req); return ESP_FAIL; }   // cross-site WebSocket
    if (req_authed(req)) return ESP_OK;
    send_unauthorized(req);
    return ESP_FAIL;   // esp_http_server closes the socket instead of upgrading it
}

// "192.168.1.23" for the pairing prompt (IPv4-mapped IPv6 peers shown as IPv4).
void peer_label(httpd_req_t *req, char *out, size_t n) {
    snprintf(out, n, "?");
    struct sockaddr_storage ss = {};
    socklen_t len = sizeof ss;
    if (getpeername(httpd_req_to_sockfd(req), reinterpret_cast<struct sockaddr *>(&ss), &len) != 0) return;
    if (ss.ss_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<struct sockaddr_in *>(&ss)->sin_addr, out, n);
    } else if (ss.ss_family == AF_INET6) {
        const auto *a6 = reinterpret_cast<struct sockaddr_in6 *>(&ss);
        const uint8_t *b = reinterpret_cast<const uint8_t *>(&a6->sin6_addr);
        static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(b, kMapped, sizeof kMapped) == 0) snprintf(out, n, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
        else inet_ntop(AF_INET6, &a6->sin6_addr, out, n);
    }
}

// GET /api/auth/status -> {"required":true,"paired":bool}. An unpaired client also puts a pairing
// code on the device screen (the web shell then shows its PIN overlay; tools/pair.py asks for it).
esp_err_t h_auth_status(httpd_req_t *req) {
    const bool paired = req_authed(req);
    if (!paired) {
        char who[NV_AUTH_WHO_MAX];
        peer_label(req, who, sizeof who);
        nv_auth_pair_request(who);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, paired ? "{\"required\":true,\"paired\":true}"
                                       : "{\"required\":true,\"paired\":false}", HTTPD_RESP_USE_STRLEN);
}

// POST /api/pair {"pin":"123456","name":"...","token":true}. Success sets the HttpOnly nv_s cookie;
// with "token":true (tools) the token is also in the body. 401 wrong code, 429 locked, 409 no code.
esp_err_t h_pair(httpd_req_t *req) {
    char body[256];
    if (req->content_len >= sizeof body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too big");
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return ESP_FAIL;
        got += (size_t)r;
    }
    body[got] = '\0';
    char pin[8] = "", name[NV_AUTH_NAME_MAX] = "";
    bool want_token = false;
    if (cJSON *j = cJSON_Parse(body)) {
        const cJSON *p = cJSON_GetObjectItem(j, "pin");
        const cJSON *nm = cJSON_GetObjectItem(j, "name");
        if (cJSON_IsString(p)) snprintf(pin, sizeof pin, "%s", p->valuestring);
        if (cJSON_IsString(nm)) snprintf(name, sizeof name, "%s", nm->valuestring);
        want_token = cJSON_IsTrue(cJSON_GetObjectItem(j, "token"));
        cJSON_Delete(j);
    }
    if (!name[0]) {   // browsers send no name: label the session by address
        char ip[40];
        peer_label(req, ip, sizeof ip);
        snprintf(name, sizeof name, "browser %.23s", ip);   // IPv6 labels are cut, IPv4 fits
    }
    char token[NV_AUTH_TOKEN_HEX + 1];
    const nv_auth_pair_result_t r = nv_auth_pair_finish(pin, name, token);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    switch (r) {
    case NV_AUTH_PAIR_OK: break;
    case NV_AUTH_PAIR_LOCKED:
        httpd_resp_set_status(req, "429 Too Many Requests");
        return httpd_resp_send(req, "{\"ok\":false,\"locked\":true}", HTTPD_RESP_USE_STRLEN);
    case NV_AUTH_PAIR_NO_CODE:
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, "{\"ok\":false,\"expired\":true}", HTTPD_RESP_USE_STRLEN);
    default:
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }
    char cookie[128];
    snprintf(cookie, sizeof cookie, "nv_s=%s; Path=/; Max-Age=31536000; HttpOnly; SameSite=Strict", token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    char out[96];
    if (want_token) snprintf(out, sizeof out, "{\"ok\":true,\"token\":\"%s\"}", token);
    else snprintf(out, sizeof out, "{\"ok\":true}");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
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
NV_PSRAM_BSS static char s_aq_text[3072];       // chat turns arrive via POST body; sized to the conv
                                                // store's per-message cap so device-exec turns aren't
                                                // clipped harder than browser-direct ones
static char             s_aq_lang[4] = "it";
NV_PSRAM_BSS static anima_result_t s_aq_res;
static SemaphoreHandle_t s_aq_go, s_aq_done;
static TaskHandle_t     s_aq_task;
// job kind: 0 = cascade query (nucleo_anima_query), 1 = conversation chat (nucleo_anima_conv_chat —
// memory + rolling summary + recent turns, appended to the SD conversation store).
static int              s_aq_kind = 0;
NV_PSRAM_BSS static char s_aq_conv[NV_CONV_ID_CAP];     // in: conv id ("" = new); out: resolved id
static int              s_aq_rc = 0;                    // conv-chat return (1 answered / 0 miss / <0 store error)
// job kind 2: an /api/llm relay (in: url, method, headers, body; out: response body + status)
struct LlmRelay { char url[512]; char method[8]; char hk[3][24]; char hv[3][320]; char *body; char *resp; int len; int status; };
NV_PSRAM_BSS static LlmRelay s_relay;

static void anima_query_worker(void *) {
    for (;;) {
        xSemaphoreTake(s_aq_go, portMAX_DELAY);
        if (s_aq_kind == 2) {
            const char *hdr[6] = { s_relay.hk[0], s_relay.hv[0], s_relay.hk[1], s_relay.hv[1], s_relay.hk[2], s_relay.hv[2] };
            s_relay.len = nucleo_anima_http_relay(s_relay.url, s_relay.method, hdr, s_relay.body, 64 * 1024,
                                                  &s_relay.resp, &s_relay.status);
        } else if (s_aq_kind == 1) {
            const bool en = strncmp(s_aq_lang, "en", 2) == 0;
            s_aq_rc = nucleo_anima_conv_chat(s_aq_conv[0] ? s_aq_conv : nullptr, s_aq_text, en,
                                             &s_aq_res, s_aq_conv, sizeof s_aq_conv);
        } else {
            s_aq_res = nucleo_anima_query(s_aq_text, s_aq_lang);
        }
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
NV_PSRAM_BSS static char s_aq_long[2048];
// A TOOL result is executed under the same lock (its payload is engine state); the outcome is
// reported in the reply so the web never shows "done" for an action that did not happen.
NV_PSRAM_BSS static char s_aq_tool_note[160];
NV_PSRAM_BSS static char s_aq_why[160];   // why the model call failed this turn ("" = it did not)
static bool s_aq_tool_ok = false;

static void anima_run_tool(const anima_result_t &r, const char *lang) {
    s_aq_tool_ok = false;
    s_aq_tool_note[0] = 0;
    if (nucleo_anima_has_tool_work(&r))   // a TOOL, or a compound request's steps
        s_aq_tool_ok = nv_anima_os_run(&r, strncmp(lang, "en", 2) == 0, s_aq_tool_note, sizeof s_aq_tool_note);
}

// Run one query through the engine on the PSRAM worker (spine-gated). Returns false when the
// native chat owns the cascade — the caller answers {"busy":true}.
static bool anima_run(const char *text, const char *lang, anima_result_t *out) {
    nucleo_anima_init(lang);
    if (!nucleo_anima_try_lock()) return false;
    if (!anima_worker_ensure()) {                      // OOM: NO inline fallback — the cascade needs
        nucleo_anima_unlock();                         // ~15-19 KB of stack, the httpd task has 8 KB
        return false;                                  // (caller answers busy; this is the crash the
    }                                                  // worker was introduced to stop)
    s_aq_kind = 0;
    strlcpy(s_aq_text, text, sizeof s_aq_text);
    strlcpy(s_aq_lang, lang, sizeof s_aq_lang);
    xSemaphoreGive(s_aq_go);
    xSemaphoreTake(s_aq_done, portMAX_DELAY);
    *out = s_aq_res;
    const char *lr = nucleo_anima_long_reply();
    snprintf(s_aq_long, sizeof s_aq_long, "%s", lr ? lr : "");
    anima_run_tool(*out, lang);
    snprintf(s_aq_why, sizeof s_aq_why, "%s", nucleo_anima_online_fail_note(strncmp(lang, "en", 2) == 0));
    nucleo_anima_unlock();
    return true;
}

// One CONVERSATION turn on the same spine-locked worker (TLS + deep frames must stay off the 8 KB
// httpd stack). conv_io: in = requested conversation ("" = create new), out = resolved id.
// Returns false when the native chat owns the cascade (caller answers busy); *rc is the conv-chat
// verdict (1 answered / 0 honest miss / <0 store error).
static bool anima_chat_run(const char *conv, const char *text, const char *lang,
                           anima_result_t *out, char *conv_out, int convcap, int *rc) {
    nucleo_anima_init(lang);
    if (!nucleo_anima_try_lock()) return false;
    if (!anima_worker_ensure()) {                       // OOM: no inline fallback (needs the big stack)
        nucleo_anima_unlock();
        memset(out, 0, sizeof *out);                    // callers format from *out — never leave it garbage
        conv_out[0] = 0; s_aq_long[0] = 0;              // and never leak a previous turn's long tail
        *rc = -3;
        return true;
    }
    s_aq_kind = 1;
    strlcpy(s_aq_conv, conv ? conv : "", sizeof s_aq_conv);
    strlcpy(s_aq_text, text, sizeof s_aq_text);
    strlcpy(s_aq_lang, lang, sizeof s_aq_lang);
    xSemaphoreGive(s_aq_go);
    xSemaphoreTake(s_aq_done, portMAX_DELAY);
    *out = s_aq_res;
    *rc = s_aq_rc;
    snprintf(conv_out, convcap, "%s", s_aq_conv);
    const char *lr = nucleo_anima_long_reply();
    snprintf(s_aq_long, sizeof s_aq_long, "%s", lr ? lr : "");
    anima_run_tool(*out, lang);
    snprintf(s_aq_why, sizeof s_aq_why, "%s", nucleo_anima_online_fail_note(strncmp(lang, "en", 2) == 0));
    nucleo_anima_unlock();
    return true;
}

// A LAUNCH action really opens the app on the panel (LVGL-locked) — same contract as native
// chat. TOOL proposals already ran under the engine lock (anima_run_tool).
static void anima_do_launch(const anima_result_t &r) {
    // Post the open to the UI thread (see h_ui_open) instead of opening under lvgl_port_lock on this
    // httpd task — a WASM app teardown+relaunch under a foreign-held lock can deadlock UI + web.
    if (r.action == ANIMA_ACT_LAUNCH && r.arg[0]) nv_ui_open_app_id_async(r.arg);
}

// Final human-facing text: prefer the long-form tail, then splice live SYSTEM values into the
// {value} template (clock/SD/registry live here in the OS, not in the engine).
static void anima_final_text(const anima_result_t &r, bool en, char *out, size_t cap) {
    const char *base = s_aq_long[0] ? s_aq_long : r.reply;
    if (r.action == ANIMA_ACT_SYSTEM) nv_anima_system_reply(r.arg, base, en, out, cap);
    else                              snprintf(out, cap, "%s", base);
    nv_anima_pretty_reply(out, cap, &r);   // "Apro calc." -> "Apro Calcolatrice." (every app a plan names)
}

// What a TOOL result really did, as JSON members appended to a reply object:
// ,"done":true|false,"note":"calendario: 2026-10-02 09:00" — "" for any other result.
static void anima_tool_json(const anima_result_t &r, char *out, size_t cap) {
    out[0] = 0;
    if (!nucleo_anima_has_tool_work(&r)) return;
    char en_note[sizeof s_aq_tool_note * 2];
    json_escape(en_note, sizeof en_note, s_aq_tool_note);
    size_t o = (size_t)snprintf(out, cap, ",\"done\":%s,\"note\":\"%s\"", s_aq_tool_ok ? "true" : "false", en_note);
    // A compound request: every step, in the order it ran ("steps":[{"intent":"set_volume","arg":"30"},...]).
    if (r.nsteps > 0 && o < cap) {
        o += (size_t)snprintf(out + o, cap - o, ",\"steps\":[");
        for (int i = 0; i < r.nsteps && i < ANIMA_PLAN_MAX && o < cap; i++) {
            char ea[140]; json_escape(ea, sizeof ea, r.steps[i].arg);
            o += (size_t)snprintf(out + o, cap - o, "%s{\"intent\":\"%s\",\"arg\":\"%s\"}", i ? "," : "", r.steps[i].intent, ea);
        }
        if (o < cap) snprintf(out + o, cap - o, "]");
    }
}

// POST /api/anima/query?text=... — the native ANIMA engine answers over REST (the web companion
// asks the DEVICE brain instead of its browser WASM twin).
esp_err_t h_anima_query(httpd_req_t *req) {
    char text[256];
    if (!query_param(req, "text", text, sizeof text)) return ESP_OK;
    char lang[4] = "en";
    query_param_opt(req, "lang", lang, sizeof lang);
    anima_result_t r;
    if (!anima_run(text, lang, &r)) {                  // native chat may own the cascade
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"busy\":true}", HTTPD_RESP_USE_STRLEN);
    }
    anima_do_launch(r);
    // Statics, not stack: the resolved+escaped long-form answer would eat most of the 12 KB httpd
    // stack. esp_http_server dispatches serially on one task, so they never overlap.
    NV_PSRAM_BSS static char resolved[2200], reply[2800], b[4800];   // off the 8 KB httpd stack, and out of internal SRAM
    char ei[80], ea[160];   // intent/arg can echo user text: escape them like the reply
    anima_final_text(r, strncmp(lang, "en", 2) == 0, resolved, sizeof resolved);
    json_escape(reply, sizeof reply, resolved);
    json_escape(ei, sizeof ei, r.intent);
    json_escape(ea, sizeof ea, r.arg);
    NV_PSRAM_BSS static char tj[1000]; anima_tool_json(r, tj, sizeof tj);   // static: not on the httpd stack
    snprintf(b, sizeof b,
             "{\"tier\":%d,\"action\":%d,\"intent\":\"%s\",\"arg\":\"%s\",\"conf\":%d,\"reply\":\"%s\",\"degraded\":%s%s}",
             (int)r.tier, (int)r.action, ei, ea, r.confidence, reply, r.degraded ? "true" : "false", tj);
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
    char lang[4] = "en";
    query_param_opt(req, "lang", lang, sizeof lang);   // the network mode is the device's (/api/anima/net)
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
    NV_PSRAM_BSS static char resolved[2200], reply[2800], trace[256], b[5200];   // off the 8 KB httpd stack + out of internal SRAM
    char ei[80], ea[160];   // intent/arg can echo user text: escape them like the reply
    anima_final_text(r, strncmp(lang, "en", 2) == 0, resolved, sizeof resolved);
    json_escape(reply, sizeof reply, resolved);
    json_escape(trace, sizeof trace, r.trace);
    json_escape(ei, sizeof ei, r.intent);
    json_escape(ea, sizeof ea, r.arg);
    NV_PSRAM_BSS static char tj[1000]; anima_tool_json(r, tj, sizeof tj);   // static: not on the httpd stack
    snprintf(b, sizeof b,
             "{\"tier\":\"%s\",\"action\":\"%s\",\"intent\":\"%s\",\"tool\":\"%s\",\"arg\":\"%s\","
             "\"conf\":%d,\"trace\":\"%s\",\"reply\":\"%s\",\"degraded\":%s%s}",
             tier, action, ei, r.action == ANIMA_ACT_TOOL ? ei : "", ea,
             r.confidence, trace, reply, r.degraded ? "true" : "false", tj);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

// ───────────────────────── assistant layer: conversations + memory ─────────────────────────

// POST /api/anima/chat — body {"q":"…","conv":"<id|empty>","lang":"it|en"}. One conversational turn
// with persistent context (user memory + rolling summary + recent tail); both sides are appended to
// the SD conversation store. Response mirrors GET /api/anima (string action + reply) plus the
// resolved {"conv":"<id>"} so the client pins the conversation. ok:false = honest miss (offline/no key).
esp_err_t h_anima_chat(httpd_req_t *req) {
    size_t len = 0;
    char *body = recv_body(req, 8192, &len);   // q up to 3 KB arrives JSON-escaped (worst ~2x)
    if (!body) return ESP_OK;
    cJSON *o = cJSON_Parse(body); free(body);
    // q buffer off the 12 KB httpd stack; serial dispatch means no overlap.
    NV_PSRAM_BSS static char q[3072];
    q[0] = 0;
    char conv[NV_CONV_ID_CAP] = "", lang[4] = "it";
    if (o) {
        cJSON *jq = cJSON_GetObjectItem(o, "q"), *jc = cJSON_GetObjectItem(o, "conv"), *jl = cJSON_GetObjectItem(o, "lang");
        if (cJSON_IsString(jq)) strlcpy(q, jq->valuestring, sizeof q);
        if (cJSON_IsString(jc)) strlcpy(conv, jc->valuestring, sizeof conv);
        if (cJSON_IsString(jl)) strlcpy(lang, jl->valuestring, sizeof lang);
        cJSON_Delete(o);
    }
    // strlcpy can cut a multi-byte UTF-8 sequence at the cap: trim dangling continuation bytes AND
    // the orphaned lead byte, so the stored transcript / provider request stay valid UTF-8 (invalid
    // bytes break the client's JSON.parse of our own response).
    {
        size_t n = strlen(q), s = n;
        while (s > 0 && ((unsigned char)q[s-1] & 0xC0) == 0x80) s--;          // back over continuation bytes
        unsigned char lead = s > 0 ? (unsigned char)q[s-1] : 0;
        size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (s < n && lead < 0xC0)                       q[s] = 0;             // orphan continuation run
        else if (lead >= 0xC0 && (n - (s - 1)) < want)  q[s-1] = 0;           // truncated multi-byte seq
    }
    if (!q[0]) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no q");
    anima_result_t r; int rc = 0; char conv_out[NV_CONV_ID_CAP] = "";
    if (!anima_chat_run(conv, q, lang, &r, conv_out, sizeof conv_out, &rc)) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"busy\":true,\"reply\":\"\",\"action\":\"none\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (rc == -3) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "anima worker oom");
    // Without a usable model the device answered (nucleo_anima_conv_chat's fallback): that can be a
    // command, so it is carried out and rendered like /api/anima's.
    anima_do_launch(r);
    // Statics, not stack (12 KB httpd stack); serial dispatch means no overlap.
    NV_PSRAM_BSS static char chat_resolved[2200], chat_reply[2800], chat_b[4800];
    anima_final_text(r, strncmp(lang, "en", 2) == 0, chat_resolved, sizeof chat_resolved);
    json_escape(chat_reply, sizeof chat_reply, chat_resolved);
    char why[340]; json_escape(why, sizeof why, rc > 0 && !r.degraded ? "" : s_aq_why);
    char ei[80]; json_escape(ei, sizeof ei, r.intent);
    NV_PSRAM_BSS static char tj[1000]; anima_tool_json(r, tj, sizeof tj);   // static: not on the httpd stack
    const char *tier = r.tier == ANIMA_TIER_COMMAND ? "command" : r.tier == ANIMA_TIER_FACT ? "fact" :
                       r.tier == ANIMA_TIER_REMOTE  ? "remote"  : r.tier == ANIMA_TIER_NONE ? "none" : "fact";
    const char *action = r.action == ANIMA_ACT_LAUNCH ? "launch" : r.action == ANIMA_ACT_SYSTEM ? "system" :
                         r.action == ANIMA_ACT_TOOL   ? "tool"   : "answer";
    snprintf(chat_b, sizeof chat_b,
             "{\"ok\":%s,\"conv\":\"%s\",\"tier\":\"%s\",\"action\":\"%s\",\"intent\":\"%s\",\"conf\":%d,"
             "\"reply\":\"%s\",\"why\":\"%s\",\"degraded\":%s%s}",
             rc > 0 ? "true" : "false", conv_out, tier, action, ei, r.confidence, chat_reply, why,
             r.degraded ? "true" : "false", tj);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, chat_b, HTTPD_RESP_USE_STRLEN);
}

// GET /api/anima/conv?op=list | ?op=msgs&id=…&tail=40 | ?op=ctx&id=…&lang=it — store reads.
esp_err_t h_anima_conv_get(httpd_req_t *req) {
    char op[8] = "list";
    query_param_opt(req, "op", op, sizeof op);
    httpd_resp_set_type(req, "application/json");
    if (!strcmp(op, "msgs") || !strcmp(op, "ctx")) {
        char id[NV_CONV_ID_CAP] = "";
        query_param_opt(req, "id", id, sizeof id);
        if (!id[0]) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no id");
        if (!strcmp(op, "ctx")) {                       // persistent-context block for browser-exec chat
            char lang[4] = "en"; query_param_opt(req, "lang", lang, sizeof lang);
            NV_PSRAM_BSS static char blk[2600], eblk[5300], bctx[5400];
            int n = nucleo_anima_conv_ctx_block(id, strncmp(lang, "en", 2) == 0, blk, sizeof blk);
            json_escape(eblk, sizeof eblk, n > 0 ? blk : "");
            snprintf(bctx, sizeof bctx, "{\"sys\":\"%s\"}", eblk);
            return httpd_resp_send(req, bctx, HTTPD_RESP_USE_STRLEN);
        }
        char tl[8] = "40"; query_param_opt(req, "tail", tl, sizeof tl);
        char *json = nullptr;
        int n = nucleo_anima_conv_msgs_json(id, atoi(tl), &json);
        if (n < 0 || !json) { free(json); return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no conv"); }
        esp_err_t e = httpd_resp_send(req, json, n);
        free(json);
        return e;
    }
    NV_PSRAM_BSS static char lst[4600];   // 20 convs × (esc'd 63-char title + fields) can pass 3 KB
    int n = nucleo_anima_conv_list_json(lst, sizeof lst);
    if (n < 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "list fail");
    return httpd_resp_send(req, lst, n);
}

// POST /api/anima/conv — {"op":"new","title"?} | {"op":"del","id"} | {"op":"title","id","title"} |
// {"op":"append","id","r":"u"|"a","t":"…"} (browser-exec surfaces mirror their turns here so every
// surface shares one durable history).
esp_err_t h_anima_conv_post(httpd_req_t *req) {
    size_t len = 0;
    char *body = recv_body(req, 8192, &len);
    if (!body) return ESP_OK;
    cJSON *o = cJSON_Parse(body); free(body);
    if (!o) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    const cJSON *jop = cJSON_GetObjectItem(o, "op");
    const char *op = cJSON_IsString(jop) ? jop->valuestring : "";
    char okb[64] = "{\"ok\":true}";
    int rc = -1;
    if (!strcmp(op, "new")) {
        cJSON *jt = cJSON_GetObjectItem(o, "title");
        char id[NV_CONV_ID_CAP];
        rc = nucleo_anima_conv_create(id, sizeof id, cJSON_IsString(jt) ? jt->valuestring : nullptr);
        if (rc == 0) snprintf(okb, sizeof okb, "{\"ok\":true,\"id\":\"%s\"}", id);
    } else if (!strcmp(op, "del")) {
        cJSON *ji = cJSON_GetObjectItem(o, "id");
        rc = cJSON_IsString(ji) ? nucleo_anima_conv_delete(ji->valuestring) : -1;
    } else if (!strcmp(op, "title")) {
        cJSON *ji = cJSON_GetObjectItem(o, "id"), *jt = cJSON_GetObjectItem(o, "title");
        rc = (cJSON_IsString(ji) && cJSON_IsString(jt)) ? nucleo_anima_conv_set_title(ji->valuestring, jt->valuestring) : -1;
    } else if (!strcmp(op, "append")) {
        cJSON *ji = cJSON_GetObjectItem(o, "id"), *jr = cJSON_GetObjectItem(o, "r"), *jt = cJSON_GetObjectItem(o, "t");
        rc = (cJSON_IsString(ji) && cJSON_IsString(jr) && jr->valuestring[0] && cJSON_IsString(jt))
             ? nucleo_anima_conv_append(ji->valuestring, jr->valuestring[0], jt->valuestring) : -1;
    }
    cJSON_Delete(o);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, rc == 0 ? okb : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
}

// GET /api/anima/memory — the user-memory list {"facts":[{"ts","t"},…]}. Worst case is bounded
// (48 facts × esc'd 240 chars ≈ 25 KB), so a PSRAM static beats a per-request malloc that could
// fail under memory pressure and 500 an otherwise infallible read (serial httpd: no overlap).
EXT_RAM_BSS_ATTR static char s_mem_json[26 * 1024];
esp_err_t h_anima_mem_get(httpd_req_t *req) {
    int n = nucleo_anima_mem_list_json(s_mem_json, sizeof s_mem_json);
    httpd_resp_set_type(req, "application/json");
    if (n < 0) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem fail");
    return httpd_resp_send(req, s_mem_json, n);
}

// POST /api/anima/memory — {"op":"add","t":"…"} | {"op":"del","ts":123} |
// {"op":"capture","q":"…","lang":"it"} → {"handled":bool,"reply":"…"}. `capture` runs the SAME
// firmware-side "ricordati che…" detector every other surface uses, so the browser keeps ZERO
// trigger-phrase lists (they would drift from the C ones across the two deploy channels).
EXT_RAM_BSS_ATTR static char s_cap_reply[1500], s_cap_esc[3100], s_cap_b[3200];
esp_err_t h_anima_mem_post(httpd_req_t *req) {
    size_t len = 0;
    char *body = recv_body(req, 4096, &len);
    if (!body) return ESP_OK;
    cJSON *o = cJSON_Parse(body); free(body);
    if (!o) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    const cJSON *jop = cJSON_GetObjectItem(o, "op");
    const char *op = cJSON_IsString(jop) ? jop->valuestring : "";
    if (!strcmp(op, "capture")) {
        cJSON *jq = cJSON_GetObjectItem(o, "q"), *jl = cJSON_GetObjectItem(o, "lang");
        const bool en = cJSON_IsString(jl) && strncmp(jl->valuestring, "en", 2) == 0;
        bool handled = false;
        s_cap_reply[0] = 0;
        if (cJSON_IsString(jq) && jq->valuestring[0])
            handled = nucleo_anima_mem_capture(jq->valuestring, en, s_cap_reply, sizeof s_cap_reply);
        cJSON_Delete(o);
        json_escape(s_cap_esc, sizeof s_cap_esc, s_cap_reply);
        snprintf(s_cap_b, sizeof s_cap_b, "{\"handled\":%s,\"reply\":\"%s\"}", handled ? "true" : "false", s_cap_esc);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, s_cap_b, HTTPD_RESP_USE_STRLEN);
    }
    int rc = -1;
    if (!strcmp(op, "add")) {
        cJSON *jt = cJSON_GetObjectItem(o, "t");
        rc = cJSON_IsString(jt) ? nucleo_anima_mem_add(jt->valuestring) : -1;
    } else if (!strcmp(op, "del")) {
        cJSON *jt = cJSON_GetObjectItem(o, "ts");
        rc = cJSON_IsNumber(jt) ? nucleo_anima_mem_del((long)jt->valuedouble) : -1;
    }
    cJSON_Delete(o);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
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
    char b[384];   // err reasons are static literals without quotes (JSON-safe)
    uint32_t shown = 0, dropped = 0;
    nv_vplayer_stats(&shown, &dropped);
    int w = 0, h = 0;
    nv_vplayer_frame(&w, &h, nullptr, nullptr);
    snprintf(b, sizeof b, "{\"state\":\"%s\",\"pos\":%d,\"dur\":%d,\"has_audio\":%s,\"fps10\":%d,"
             "\"shown\":%u,\"dropped\":%u,\"w\":%d,\"h\":%d,\"err\":\"%s\"}",
             kSt[nv_vplayer_state() & 3], nv_vplayer_pos_ms(), nv_vplayer_dur_ms(),
             nv_vplayer_has_audio() ? "true" : "false", nv_vplayer_fps10(),
             (unsigned)shown, (unsigned)dropped, w, h,
             nv_vplayer_state() == NV_VP_ERROR ? nv_vplayer_err_reason() : "");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

// POST /api/video/pause?on=1|0 — transport pause/resume (remote A/V + clock checks).
esp_err_t h_video_pause(httpd_req_t *req) {
    char v[8] = "1";
    query_param_opt(req, "on", v, sizeof v);
    nv_vplayer_pause(v[0] != '0');
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

esp_err_t h_video_seek(httpd_req_t *req) {
    char v[16];
    if (!query_param(req, "pos_ms", v, sizeof v)) return ESP_OK;
    const bool ok = nv_vplayer_seek((int)strtol(v, nullptr, 10));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":true}" : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
}

// ANIMA's network mode (ANIMA_NET_* order), shared with the native app's /mode.
static const char *const kAnimaNet[] = {"offline", "local", "hybrid", "llm"};

// GET /api/anima/net -> {"mode":"hybrid"} · POST /api/anima/net {"mode":"local"} sets and persists it.
// /api/anima/wake — hands-free ANIMA (nv_wake). GET -> the status; POST {"on":bool, "word":"<model>",
// "sens":0..2} changes any of them (the wake service applies it a moment later).
//   {"state":"listening","available":true,"on":true,"word":"wn9_hiesp","label":"Hi ESP","sens":1,
//    "words":[{"id":"wn9_hiesp","label":"Hi ESP"}],"triggers":3,"last":42,"reason":"",
//    "stt":{"route":"home"|"cloud"|"none","where":"192.168.1.20:8080"}}
esp_err_t h_anima_wake(httpd_req_t *req) {
    char lang[4] = "it";
    query_param_opt(req, "lang", lang, sizeof lang);
    const bool en = strncmp(lang, "en", 2) == 0;
    httpd_resp_set_type(req, "application/json");
    if (req->method == HTTP_POST) {
        size_t len = 0;
        char *body = recv_body(req, 256, &len);
        if (!body) return ESP_OK;
        cJSON *o = cJSON_Parse(body); free(body);
        if (!o) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        cJSON *on = cJSON_GetObjectItem(o, "on"), *w = cJSON_GetObjectItem(o, "word"), *sn = cJSON_GetObjectItem(o, "sens");
        bool bad = false;
        if (cJSON_IsString(w) && !nv_wake_set_word(w->valuestring)) bad = true;
        if (cJSON_IsNumber(sn)) nv_wake_set_sensitivity(sn->valueint);
        if (cJSON_IsBool(on)) nv_wake_set_enabled(cJSON_IsTrue(on));
        cJSON_Delete(o);
        if (bad) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "word: not an installed wake word");
    }
    NV_PSRAM_BSS static nv_wake_status_t st;
    nv_wake_status(&st, en);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "state", nv_wake_state_name(st.state));
    cJSON_AddBoolToObject(r, "available", st.available);
    cJSON_AddBoolToObject(r, "on", st.enabled);
    cJSON_AddStringToObject(r, "word", st.word);
    cJSON_AddStringToObject(r, "label", st.label);
    cJSON_AddNumberToObject(r, "sens", st.sensitivity);
    cJSON *ws = cJSON_AddArrayToObject(r, "words");
    for (int i = 0; i < st.nwords; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", st.words[i]);
        cJSON_AddStringToObject(e, "label", st.labels[i]);
        cJSON_AddItemToArray(ws, e);
    }
    cJSON_AddNumberToObject(r, "triggers", st.triggers);
    cJSON_AddNumberToObject(r, "last", st.last_ago_s);
    cJSON_AddStringToObject(r, "reason", st.reason);
    char where[64];
    const int route = nucleo_anima_stt_route(where, sizeof where);
    cJSON *stt = cJSON_AddObjectToObject(r, "stt");
    cJSON_AddStringToObject(stt, "route", route == 1 ? "home" : route == 2 ? "cloud" : "none");
    cJSON_AddStringToObject(stt, "where", route ? where : "");
    char *out = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (!out) return httpd_resp_send_500(req);
    const esp_err_t e = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    cJSON_free(out);
    return e;
}

// /api/anima/hb — the proactive heartbeat. GET -> {"every":30,"next":12} (next: minutes, -1 = off or no
// HEARTBEAT.md); POST {"every":0|15|30|60} sets the interval ("anima.hb").
esp_err_t h_anima_hb(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (req->method == HTTP_POST) {
        size_t len = 0;
        char *body = recv_body(req, 128, &len);
        if (!body) return ESP_OK;
        cJSON *o = cJSON_Parse(body); free(body);
        cJSON *e = o ? cJSON_GetObjectItem(o, "every") : nullptr;
        const int v = cJSON_IsNumber(e) ? e->valueint : -1;
        cJSON_Delete(o);
        if (v < 0 || v > 24 * 60) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "every: minutes 0..1440");
        nv_config_set_int("anima.hb", v);
    }
    char b[64];
    snprintf(b, sizeof b, "{\"every\":%d,\"next\":%d}", nv_config_get_int("anima.hb", 30), nv_anima_heartbeat_next_min());
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

// /api/anima/telegram — ANIMA's Telegram channel. GET -> {"configured","enabled","paired","checking",
// "bot","code","error"}; POST {"token":"..."} (checked by the channel task: poll GET until checking is
// false), {"enabled":bool}, {"unlink":true} (forget the paired chat), {"forget":true} (token too).
esp_err_t h_anima_tg(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (req->method == HTTP_POST) {
        size_t len = 0;
        char *body = recv_body(req, 512, &len);
        if (!body) return ESP_OK;
        cJSON *o = cJSON_Parse(body); free(body);
        if (!o) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        cJSON *t = cJSON_GetObjectItem(o, "token"), *e = cJSON_GetObjectItem(o, "enabled");
        if (cJSON_IsString(t)) nucleo_anima_tg_request_token(t->valuestring);
        if (cJSON_IsBool(e)) nucleo_anima_tg_set_enabled(cJSON_IsTrue(e));
        if (cJSON_IsTrue(cJSON_GetObjectItem(o, "unlink"))) nucleo_anima_tg_unlink();
        if (cJSON_IsTrue(cJSON_GetObjectItem(o, "forget"))) nucleo_anima_tg_forget();
        cJSON_Delete(o);
    }
    anima_tg_status_t st;
    nucleo_anima_tg_status(&st);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "configured", st.configured);
    cJSON_AddBoolToObject(r, "enabled", st.enabled);
    cJSON_AddBoolToObject(r, "paired", st.paired);
    cJSON_AddBoolToObject(r, "checking", st.checking);
    cJSON_AddStringToObject(r, "bot", st.bot);
    cJSON_AddStringToObject(r, "code", st.paired ? "" : st.code);
    cJSON_AddStringToObject(r, "error", st.error);
    char *out = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (!out) return httpd_resp_send_500(req);
    const esp_err_t err = httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
    cJSON_free(out);
    return err;
}

esp_err_t h_anima_net(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    if (req->method == HTTP_POST) {
        size_t len = 0;
        char *body = recv_body(req, 256, &len);
        if (!body) return ESP_OK;
        cJSON *o = cJSON_Parse(body); free(body);
        cJSON *m = o ? cJSON_GetObjectItem(o, "mode") : nullptr;
        int mode = -1;
        for (int i = 0; i < 4 && cJSON_IsString(m); i++) if (!strcmp(m->valuestring, kAnimaNet[i])) mode = i;
        cJSON_Delete(o);
        if (mode < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode: offline|local|hybrid|llm");
        nucleo_anima_set_net_mode(mode);
        nv_config_set_int("anima.net", mode);
    }
    // What the next turn will really use (the mode is a wish; see docs/ANIMA_MODES.md).
    static const char *const kRun[] = {"device", "web", "local_llm", "hybrid", "agent"};
    anima_route_t rt;
    nucleo_anima_route(&rt);
    char b[400];
    snprintf(b, sizeof b,
             "{\"mode\":\"%s\",\"run\":\"%s\",\"label\":\"%s\",\"label_en\":\"%s\",\"network\":%s,\"web\":%s,"
             "\"model\":%s,\"degraded\":%s}",
             kAnimaNet[nucleo_anima_get_net_mode() & 3], kRun[rt.run], nucleo_anima_route_label(&rt, false),
             nucleo_anima_route_label(&rt, true),
             rt.network ? "true" : "false", rt.web ? "true" : "false", rt.model ? "true" : "false",
             rt.degraded ? "true" : "false");
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

// GET /api/anima/models -> {"ok":true,"models":["llama3.2",...]} from the active teacher's server
// (Ollama / LM Studio / llama.cpp list what they have pulled), or {"ok":false,"why":"..."}. Runs the
// request on the ANIMA worker under the spine gate, like a query (TLS / long frames stay off httpd).
esp_err_t h_anima_models(httpd_req_t *req) {
    char lang[4] = "it";
    query_param_opt(req, "lang", lang, sizeof lang);
    NV_PSRAM_BSS static char list[2048], out[2400];
    if (!nucleo_anima_try_lock()) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"busy\":true}", HTTPD_RESP_USE_STRLEN);
    }
    nucleo_anima_online_turn_begin();          // the fail note below is this call's
    const int n = nucleo_anima_teacher_models(list, sizeof list);
    char why[200]; json_escape(why, sizeof why, n < 0 ? nucleo_anima_online_fail_note(strncmp(lang, "en", 2) == 0) : "");
    nucleo_anima_unlock();
    if (n < 0) snprintf(out, sizeof out, "{\"ok\":false,\"why\":\"%s\"}", why);
    else       snprintf(out, sizeof out, "{\"ok\":true,\"models\":%s}", list);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

// /api/llm?url=<https://… | http://<lan>…> — relay ONE model-API request for a browser surface that
// cannot make it itself: Gemini has no CORS for the OpenAI-compatible endpoint, a LAN server (Ollama,
// LM Studio, llama.cpp) is plain HTTP without CORS. Paired only (every /api route is), hosts limited to
// the AI providers and the LAN, the device's network mode applies (offline: refused; local: LAN only).
// Authorization / x-api-key / anthropic-version are forwarded; the provider's status and body come
// back as they are, so its error message reaches the user. Runs on the ANIMA worker (TLS stack).
static bool llm_host_allowed(const char *url) {
    if (nucleo_anima_url_is_local(url)) return !strncmp(url, "http://", 7) || !strncmp(url, "https://", 8);
    static const char *const kHosts[] = { "https://generativelanguage.googleapis.com/", "https://api.groq.com/",
        "https://api.openai.com/", "https://api.x.ai/", "https://api.anthropic.com/", "https://openrouter.ai/",
        "https://api.mistral.ai/", "https://api.deepseek.com/", "https://api.together.xyz/" };
    for (const char *h : kHosts) if (!strncmp(url, h, strlen(h))) return true;
    return false;
}

esp_err_t h_llm(httpd_req_t *req) {
    NV_PSRAM_BSS static char url[512];
    if (!query_param(req, "url", url, sizeof url)) return ESP_OK;
    if (!llm_host_allowed(url)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "host not allowed");
    size_t len = 0;
    char *body = nullptr;
    if (req->method == HTTP_POST) { body = recv_body(req, 48 * 1024, &len); if (!body) return ESP_OK; }
    if (!nucleo_anima_try_lock()) {
        free(body);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":{\"message\":\"ANIMA is busy, retry\"}}", HTTPD_RESP_USE_STRLEN);
    }
    if (!anima_worker_ensure()) { nucleo_anima_unlock(); free(body); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "worker oom"); }
    memset(&s_relay, 0, sizeof s_relay);
    strlcpy(s_relay.url, url, sizeof s_relay.url);
    strlcpy(s_relay.method, req->method == HTTP_POST ? "POST" : "GET", sizeof s_relay.method);
    static const char *const kFwd[3] = { "Authorization", "x-api-key", "anthropic-version" };
    for (int i = 0; i < 3; i++) {
        strlcpy(s_relay.hk[i], kFwd[i], sizeof s_relay.hk[i]);
        if (httpd_req_get_hdr_value_str(req, kFwd[i], s_relay.hv[i], sizeof s_relay.hv[i]) != ESP_OK) s_relay.hv[i][0] = 0;
    }
    s_relay.body = body;
    s_aq_kind = 2;
    xSemaphoreGive(s_aq_go);
    xSemaphoreTake(s_aq_done, portMAX_DELAY);
    char *resp = s_relay.resp; const int rlen = s_relay.len, status = s_relay.status;
    const char *why = rlen < 0 ? nucleo_anima_online_fail_note(false) : "";
    memset(s_relay.hv, 0, sizeof s_relay.hv);          // keys never linger in RAM
    nucleo_anima_unlock();
    free(body);
    httpd_resp_set_type(req, "application/json");
    if (rlen < 0) {
        free(resp);
        char e[300], ew[220];
        json_escape(ew, sizeof ew, why[0] ? why : "the model server did not answer");
        snprintf(e, sizeof e, "{\"error\":{\"message\":\"%s\"}}", ew);
        httpd_resp_set_status(req, "502 Bad Gateway");
        return httpd_resp_send(req, e, HTTPD_RESP_USE_STRLEN);
    }
    char st[40];
    snprintf(st, sizeof st, "%d %s", status, status == 200 ? "OK" : "Upstream");
    httpd_resp_set_status(req, st);
    esp_err_t r = httpd_resp_send(req, resp, rlen);
    free(resp);
    return r;
}

// GET /api/anima/caps — AI capabilities, live from the nv_anima engine. Note: with no cloud key
// configured, teacher_info may run one rate-limited (5-min window) 2 s mDNS probe for a LAN teacher.
esp_err_t h_anima_caps(httpd_req_t *req) {
    char prov[24] = "", model[40] = "";
    const bool key    = nucleo_anima_teacher_info(prov, sizeof prov, model, sizeof model);
    const bool online = nucleo_anima_online_available();
    const int  mode   = nucleo_anima_l1_get_mode();
    char b[320];
    snprintf(b, sizeof b,
             "{\"hasKey\":%s,\"online\":%s,\"enabled\":true,\"provider\":\"%s\",\"model\":\"%s\","
             "\"l1Mode\":\"%s\",\"l1Serving\":%s,\"net\":\"%s\"}",
             key ? "true" : "false", online ? "true" : "false", prov, model,
             mode == ANIMA_L1_ON ? "on" : mode == ANIMA_L1_OFF ? "off" : "auto",
             nucleo_anima_l1_serving() ? "true" : "false", kAnimaNet[nucleo_anima_get_net_mode() & 3]);
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

// GET /api/heap/map[?min_kb=64] -> the PSRAM heap's block layout, for fragmentation work:
//   {"free_kb":..,"largest_kb":..,"fit4m":3,"blocks":[["0x48a1c000",4096,"F"],["0x48e1c000",120,"u",37],..]}
// Blocks of at least min_kb are listed one by one (F free / U used); the runs of smaller blocks
// between them fold into one "u" entry with their count. largest_kb is exact (the /api/heap figure
// is a TLSF size class); fit4m = how many camera frames (4,147,200 B) the free blocks can hold.
struct HeapMapEnt { uint32_t addr, size, count; char kind; };
struct HeapMapCtx {
    HeapMapEnt *e; int n, cap;
    uint32_t min;
    HeapMapEnt run;              // pending fold of small blocks
    size_t free_total, largest;
    uint32_t fit4m;
    bool truncated;
};
void heap_map_push(HeapMapCtx *c, const HeapMapEnt &x) {
    if (c->n < c->cap) c->e[c->n++] = x; else c->truncated = true;
}
// Runs under the heap lock: no allocation, no logging.
bool heap_map_walk(walker_heap_into_t, walker_block_info_t b, void *user) {
    auto *c = static_cast<HeapMapCtx *>(user);
    if (!b.used) {
        c->free_total += b.size;
        if (b.size > c->largest) c->largest = b.size;
        c->fit4m += (uint32_t)(b.size / 4147200u);
    }
    if (b.size >= c->min) {
        if (c->run.count) { heap_map_push(c, c->run); c->run = {}; }
        heap_map_push(c, {(uint32_t)(uintptr_t)b.ptr, (uint32_t)b.size, 1, b.used ? 'U' : 'F'});
    } else {
        if (!c->run.count) c->run.addr = (uint32_t)(uintptr_t)b.ptr;
        c->run.size += (uint32_t)b.size;
        c->run.count++;
        c->run.kind = 'u';
    }
    return true;
}

esp_err_t h_heap_map(httpd_req_t *req) {
    char q[16];
    const uint32_t min_kb = query_param_opt(req, "min_kb", q, sizeof q) ? (uint32_t)atoi(q) : 64;
    HeapMapCtx c = {};
    c.cap = 1024;
    c.min = (min_kb ? min_kb : 1) * 1024;
    c.e = (HeapMapEnt *)heap_caps_malloc(sizeof(HeapMapEnt) * c.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c.e) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    heap_caps_walk(MALLOC_CAP_SPIRAM, heap_map_walk, &c);
    if (c.run.count) heap_map_push(&c, c.run);

    httpd_resp_set_type(req, "application/json");
    char b[160];
    int n = snprintf(b, sizeof b, "{\"free_kb\":%u,\"largest_kb\":%u,\"fit4m\":%u,\"truncated\":%s,\"blocks\":[",
                     (unsigned)(c.free_total / 1024), (unsigned)(c.largest / 1024), (unsigned)c.fit4m,
                     c.truncated ? "true" : "false");
    esp_err_t rc = httpd_resp_send_chunk(req, b, n);
    for (int i = 0; i < c.n && rc == ESP_OK; i++) {
        const HeapMapEnt &x = c.e[i];
        n = x.kind == 'u'
            ? snprintf(b, sizeof b, "%s[\"0x%08lx\",%u,\"u\",%u]", i ? "," : "", (unsigned long)x.addr,
                       (unsigned)(x.size / 1024), (unsigned)x.count)
            : snprintf(b, sizeof b, "%s[\"0x%08lx\",%u,\"%c\"]", i ? "," : "", (unsigned long)x.addr,
                       (unsigned)(x.size / 1024), x.kind);
        rc = httpd_resp_send_chunk(req, b, n);
    }
    free(c.e);
    if (rc == ESP_OK) rc = httpd_resp_send_chunk(req, "]}", 2);
    if (rc == ESP_OK) rc = httpd_resp_send_chunk(req, nullptr, 0);
    return rc;
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
    } else if (f.len >= 2048) {
        // Oversized frame: a payload left in the socket is re-parsed as bogus frame headers on
        // every wake-up (desync + a spinning httpd task). Close this session instead.
        httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
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

// httpd_send() may take a buffer in pieces; loop until it's all on the wire.
bool send_all(httpd_req_t *req, const char *p, size_t n) {
    while (n) {
        const int k = httpd_send(req, p, n);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}

// GET /api/fs/read?path=<logical> -> raw file bytes, with a Content-Length (no chunked framing) and
// single-range support ("Range: bytes=a-b" / "a-" / "-n" -> 206 + Content-Range). The web video
// player streams and seeks big recordings in ~1 MB pieces: one never-ending response would hold the
// single-task server (and every other request) for the length of the film. The file is read
// unbuffered, sector-aligned, into a cache-aligned buffer so the SD driver DMAs straight into it
// (unaligned reads fall back to one 512-byte sector per command).
esp_err_t h_fs_read(httpd_req_t *req) {
    char logical[256], phys[320];
    if (!query_param(req, "path", logical, sizeof logical)) return ESP_OK;
    if (!map_fs(logical, phys, sizeof phys)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    struct stat st{};
    if (stat(phys, &st) != 0 || S_ISDIR(st.st_mode)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
    if (nv_sealed_path(phys)) {                    // small secret file: whole, unsealed, never cached
        size_t n = 0;
        char *plain = nv_sealed_read(phys, 64 * 1024, &n);
        if (!plain) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot open sealed file");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        const esp_err_t r = httpd_resp_send(req, plain, (ssize_t)n);
        memset(plain, 0, n);
        free(plain);
        return r;
    }
    const uint64_t size = (uint64_t)(uint32_t)st.st_size;   // off_t is 32-bit here; FAT32 files reach 4 GB

    uint64_t first = 0, last = size ? size - 1 : 0;
    bool partial = false;
    char rh[64] = "";
    if (size && httpd_req_get_hdr_value_str(req, "Range", rh, sizeof rh) == ESP_OK && !strncmp(rh, "bytes=", 6)) {
        const char *r = rh + 6;
        char *e = nullptr;
        bool unsat = false;
        if (*r == '-') {                                       // suffix: the last n bytes
            const uint64_t n = strtoull(r + 1, &e, 10);
            if (n) { first = n >= size ? 0 : size - n; partial = true; }
            else unsat = true;                                 // "bytes=-0"
        } else if (isdigit((unsigned char)*r)) {
            first = strtoull(r, &e, 10);
            uint64_t l = size - 1;
            if (e && *e == '-' && isdigit((unsigned char)e[1])) l = strtoull(e + 1, nullptr, 10);
            if (first >= size) unsat = true;
            else if (l >= first) { last = l < size ? l : size - 1; partial = true; }
            else first = 0;                                    // "bytes=5-3": invalid -> whole file
        }
        if (unsat) {
            char cr[48];
            snprintf(cr, sizeof cr, "bytes */%llu", (unsigned long long)size);
            httpd_resp_set_status(req, "416 Range Not Satisfiable");
            httpd_resp_set_hdr(req, "Content-Range", cr);
            return httpd_resp_send(req, nullptr, 0);
        }
    }

    FILE *f = nv_sd_fopen(phys, "rb");
    if (!f) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
    setvbuf(f, nullptr, _IONBF, 0);
    constexpr size_t kBuf = 32 * 1024;
    char *buf = (char *)heap_caps_aligned_alloc(128, kBuf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { nv_sd_fclose(f); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); }

    const uint64_t len = size ? last - first + 1 : 0;
    char hdr[384];
    int hl;
    if (partial)
        hl = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\n"
                      "Content-Range: bytes %llu-%llu/%llu\r\nContent-Length: %llu\r\nCache-Control: no-cache\r\n\r\n",
                      mime_for(phys), (unsigned long long)first, (unsigned long long)last,
                      (unsigned long long)size, (unsigned long long)len);
    else
        hl = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\n"
                      "Content-Length: %llu\r\nCache-Control: no-cache\r\n\r\n",
                      mime_for(phys), (unsigned long long)len);
    bool ok = hl > 0 && (size_t)hl < sizeof hdr && send_all(req, hdr, (size_t)hl);
    uint64_t pos = first;
    const uint64_t end = first + len;
    while (ok && pos < end) {
        const uint64_t a0 = pos & ~(uint64_t)511;              // sector-aligned file position
        uint64_t want = end - a0;
        if (want > kBuf) want = kBuf;
        want = (want + 511) & ~(uint64_t)511;                  // whole sectors (EOF: short read)
        if (want > kBuf) want = kBuf;
        // read() on the descriptor: unbuffered stdio walks every open stream per call
        const int fd = fileno(f);
        if (lseek(fd, (off_t)a0, SEEK_SET) != (off_t)a0) break;
        size_t got = 0;
        while (got < (size_t)want) {
            const ssize_t r = read(fd, buf + got, (size_t)want - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        const size_t skip = (size_t)(pos - a0);
        if (got <= skip) break;
        size_t n = got - skip;
        if (n > end - pos) n = (size_t)(end - pos);
        ok = send_all(req, buf + skip, n);
        pos += n;
    }
    heap_caps_free(buf);
    nv_sd_fclose(f);
    // A short body after the headers promised more can't be repaired: drop the connection so the
    // client sees a truncated response instead of waiting for bytes that never come.
    if (pos < end) httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
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
    if (nv_sealed_path(phys)) {                    // secret file: take the whole body, seal it
        if (req->content_len > 64 * 1024) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too big");
        char *body = (char *)heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        size_t got = 0;
        while (got < req->content_len) {
            const int n = httpd_req_recv(req, body + got, req->content_len - got);
            if (n <= 0) { heap_caps_free(body); return ESP_FAIL; }
            got += (size_t)n;
        }
        const bool ok = nv_sealed_write(phys, body, got);
        memset(body, 0, got);
        heap_caps_free(body);
        return ok ? httpd_resp_sendstr(req, "ok")
                  : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }
    // Collect the body in a cache-aligned 64 KB buffer and write it in whole blocks through an
    // unbuffered FILE: the SD driver then DMAs straight from it. 2 KB stdio writes from a stack
    // buffer went one 512-byte sector per command through a bounce buffer (~0.3 MB/s).
    constexpr size_t kBuf = 64 * 1024;
    char *buf = (char *)heap_caps_aligned_alloc(128, kBuf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    FILE *f = nv_sd_fopen(phys, "wb");
    if (!f) { heap_caps_free(buf); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed"); }
    setvbuf(f, nullptr, _IONBF, 0);
    size_t left = req->content_len, fill = 0;
    bool ok = true;
    while (left > 0) {
        const size_t room = kBuf - fill;
        const int n = httpd_req_recv(req, buf + fill, left < room ? left : room);
        if (n <= 0) { heap_caps_free(buf); nv_sd_fclose(f); unlink(phys); return ESP_FAIL; }
        fill += (size_t)n; left -= (size_t)n;
        if (fill == kBuf || left == 0) {
            // write() on the descriptor: unbuffered fwrite goes out in 1 KB (BUFSIZ) pieces
            size_t done = 0;
            while (done < fill) {
                const ssize_t w = write(fileno(f), buf + done, fill - done);
                if (w <= 0) break;
                done += (size_t)w;
            }
            if (done != fill) { ok = false; break; }
            fill = 0;
        }
    }
    heap_caps_free(buf);
    nv_sd_fclose(f);
    if (!ok) {
        unlink(phys);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }
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

// ---------------------------------------------------------------- diagnostics: throughput benches

// GET /api/bench/nvs[?n=N] — N reads of one NVS key (default 2000, max 100000) -> {"n","ms","us_per_op"}.
// Every read is a flash operation: the cache goes off and the other CPU is parked through the IPC
// handshake in spi_flash/cache_utils.c. Besides the latency figure this is the HIL stress for bugs
// that only bite inside that handshake (an interrupt source routed to both CPUs deadlocked it:
// see nv_camera.c, "bring-up / teardown always on core 0"). httpd runs on an internal stack (§2).
esp_err_t h_bench_nvs(httpd_req_t *req) {
    char v[16];
    long n = 2000;
    if (query_param_opt(req, "n", v, sizeof v)) n = atol(v);
    if (n < 1) n = 1;
    if (n > 100000) n = 100000;
    nvs_handle_t h;
    if (nvs_open("nvcfg", NVS_READONLY, &h) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs open failed");
    const int64_t t0 = esp_timer_get_time();
    int32_t val = 0;
    long ok = 0;
    for (long i = 0; i < n; i++) ok += nvs_get_i32(h, "brightness", &val) == ESP_OK;
    const int64_t us = esp_timer_get_time() - t0;
    nvs_close(h);
    char body[128];
    snprintf(body, sizeof body, "{\"n\":%ld,\"found\":%ld,\"ms\":%lld,\"us_per_op\":%.1f}", n, ok,
             (long long)(us / 1000), (double)us / (double)n);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// GET /api/bench/sd?path=<logical>[&mb=N][&chunk=B] — sequential SD read speed, card -> RAM with no
// network in the loop. Reads N MB (default 16; the file is rewound at EOF) in `chunk`-byte read()s
// (default 64 KB, cache-aligned PSRAM buffer) -> {"bytes","ms","kBps","bus_khz","chunk"}.
esp_err_t h_bench_sd(httpd_req_t *req) {
    char logical[256], phys[320], v[16];
    if (!query_param(req, "path", logical, sizeof logical)) return ESP_OK;
    if (!map_fs(logical, phys, sizeof phys)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    size_t want = 16u << 20, chunk = 64u << 10;
    if (query_param_opt(req, "mb", v, sizeof v)) {
        const long m = atol(v);
        if (m >= 1 && m <= 256) want = (size_t)m << 20;
    }
    if (query_param_opt(req, "chunk", v, sizeof v)) {
        const long c = atol(v);
        if (c >= 512 && c <= (256 << 10)) chunk = (size_t)c & ~(size_t)511;
    }

    uint8_t *buf = (uint8_t *)heap_caps_aligned_alloc(64, chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    if (!nv_sd_session_begin()) {
        heap_caps_free(buf);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no card");
    }
    const int fd = open(phys, O_RDONLY);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) {
        if (fd >= 0) close(fd);
        nv_sd_session_end();
        heap_caps_free(buf);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file (or empty)");
    }
    size_t done = 0;
    bool ok = true;
    const int64_t t0 = esp_timer_get_time();
    while (done < want) {
        const size_t n = want - done < chunk ? want - done : chunk;
        const ssize_t r = read(fd, buf, n);
        if (r < 0) { ok = false; break; }
        if (r == 0) {   // EOF: rewind (st_size > 0, so the next read makes progress)
            if (lseek(fd, 0, SEEK_SET) != 0) { ok = false; break; }
            continue;
        }
        done += (size_t)r;
    }
    const int64_t us = esp_timer_get_time() - t0;
    close(fd);
    const uint32_t khz = nv_sd_bus_khz();
    nv_sd_session_end();
    heap_caps_free(buf);
    if (!ok) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");

    char body[160];
    snprintf(body, sizeof body,
             "{\"bytes\":%u,\"ms\":%u,\"kBps\":%u,\"bus_khz\":%u,\"chunk\":%u}",
             (unsigned)done, (unsigned)(us / 1000),
             (unsigned)(us > 0 ? (uint64_t)done * 1000000 / (uint64_t)us / 1024 : 0),
             (unsigned)khz, (unsigned)chunk);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// POST /api/bench/sink — network RX speed: drains the body (<= 64 MB) and discards it, no SD in the
// loop -> {"bytes","ms","kBps"}.
esp_err_t h_bench_sink(httpd_req_t *req) {
    if (req->content_len > 64u * 1024 * 1024)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too big (64MB cap)");
    const size_t cap = 16 * 1024;
    char *buf = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    size_t left = req->content_len;
    const int64_t t0 = esp_timer_get_time();
    while (left > 0) {
        const int n = httpd_req_recv(req, buf, left < cap ? left : cap);
        if (n <= 0) { heap_caps_free(buf); return ESP_FAIL; }
        left -= (size_t)n;
    }
    const int64_t us = esp_timer_get_time() - t0;
    heap_caps_free(buf);

    char body[120];
    snprintf(body, sizeof body, "{\"bytes\":%u,\"ms\":%u,\"kBps\":%u}",
             (unsigned)req->content_len, (unsigned)(us / 1000),
             (unsigned)(us > 0 ? (uint64_t)req->content_len * 1000000 / (uint64_t)us / 1024 : 0));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
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

// ---------------------------------------------------------------- Home (MQTT + Home Assistant)
// GET /api/home -> the Settings > Home fields. Secrets (MQTT password, HA token) are write-only:
// reported as *_set booleans, never echoed back.
esp_err_t h_home_get(httpd_req_t *req) {
    char host[64], user[64], url[160], sec[8], det[80];
    nv_config_get_str("mqtt_host", "", host, sizeof host);
    nv_config_get_str("mqtt_user", "", user, sizeof user);
    nv_config_get_str("ha_url", "", url, sizeof url);
    nv_config_get_str("mqtt_pass", "", sec, sizeof sec);
    const bool pass_set = sec[0] != '\0';
    nv_config_get_str("ha_token", "", sec, sizeof sec);
    const bool tok_set = sec[0] != '\0';
    memset(sec, 0, sizeof sec);
    const nv_mqtt_state_t st = nv_mqtt_status(det, sizeof det);
    char eh[132], eu[132], el[330], ed[170];
    json_escape(eh, sizeof eh, host);
    json_escape(eu, sizeof eu, user);
    json_escape(el, sizeof el, url);
    json_escape(ed, sizeof ed, det);
    char b[1024];
    snprintf(b, sizeof b,
             "{\"mqtt_en\":%s,\"mqtt_host\":\"%s\",\"mqtt_port\":%d,\"mqtt_user\":\"%s\",\"mqtt_pass_set\":%s,"
             "\"mqtt_state\":%d,\"mqtt_detail\":\"%s\",\"node\":\"%s\",\"ha_url\":\"%s\",\"ha_token_set\":%s}",
             nv_config_get_bool("mqtt_en", false) ? "true" : "false", eh, nv_config_get_int("mqtt_port", 1883),
             eu, pass_set ? "true" : "false", (int)st, ed, nv_mqtt_node_id(), el, tok_set ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, b);
}

// POST /api/home {"mqtt_en","mqtt_host","mqtt_port","mqtt_user","mqtt_pass","ha_url","ha_token"}:
// sets the fields present (an empty mqtt_pass / ha_token keeps the stored secret). Pasting a
// 180-character Home Assistant token here beats typing it on the panel.
esp_err_t h_home_post(httpd_req_t *req) {
    size_t len = 0;
    char *body = recv_body(req, 2048, &len);
    if (!body) return ESP_OK;
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json object expected");
    }
    static const struct { const char *key; size_t max; bool secret; } kStr[] = {
        {"mqtt_host", 63, false}, {"mqtt_user", 63, false}, {"mqtt_pass", 63, true},
        {"ha_url", 159, false}, {"ha_token", 319, true}};
    bool bad = false;
    for (const auto &k : kStr) {
        const cJSON *j = cJSON_GetObjectItem(root, k.key);
        if (!j) continue;
        if (cJSON_IsNull(j)) { nv_config_set_str(k.key, ""); continue; }   // null = forget it
        if (!cJSON_IsString(j) || strlen(j->valuestring) > k.max) { bad = true; continue; }
        if (k.secret && !j->valuestring[0]) continue;
        nv_config_set_str(k.key, j->valuestring);
    }
    const cJSON *jp = cJSON_GetObjectItem(root, "mqtt_port");
    if (cJSON_IsNumber(jp) && jp->valuedouble >= 1 && jp->valuedouble <= 65535)
        nv_config_set_int("mqtt_port", (int)jp->valuedouble);
    const cJSON *je = cJSON_GetObjectItem(root, "mqtt_en");
    if (cJSON_IsBool(je)) nv_config_set_bool("mqtt_en", cJSON_IsTrue(je));
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, bad ? "{\"ok\":false,\"error\":\"field too long\"}" : "{\"ok\":true}");
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
    nv_wasm_exec_set_launch_file(nullptr);   // dev hot-reload: never opened on a file (ABI v7)
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

// GET /api/ui/apps -> {"apps":[{"id":"calc","name":"Calculator","kind":"native"},...]}: every
// registered launcher entry in registry order ("wasm" = a WASM tile, which carries its record in
// NvApp.user). What tools/hil/smoke.py walks. The registry changes on the LVGL thread (store
// install/uninstall), so it is read under the port lock into a PSRAM buffer, sent after unlock.
esp_err_t h_ui_apps(httpd_req_t *req) {
    const size_t cap = 12 * 1024;
    char *b = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    if (!lvgl_port_lock(1000)) {
        free(b);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "ui busy");
    }
    size_t o = snprintf(b, cap, "{\"apps\":[");
    bool first = true;
    for (int i = 0; i < nv_app_count() && o + 200 < cap; i++) {
        const NvApp *a = nv_app_at(i);
        if (!a || !a->id) continue;
        char id[48], name[64];
        json_escape(id, sizeof id, a->id);
        json_escape(name, sizeof name, a->name ? a->name : "");
        o += snprintf(b + o, cap - o, "%s{\"id\":\"%s\",\"name\":\"%s\",\"kind\":\"%s\"}",
                      first ? "" : ",", id, name, a->user ? "wasm" : "native");
        first = false;
    }
    lvgl_port_unlock();
    o += snprintf(b + o, cap - o, "]}");
    httpd_resp_set_type(req, "application/json");
    const esp_err_t rc = httpd_resp_send(req, b, o);
    free(b);
    return rc;
}

// GET /api/ui/open?id=<appid> -> open a native app (solo-mode). {"ok":bool,"app":"<current>"}.
// The open is posted to the LVGL thread (nv_ui_open_app_id_async) instead of run here under
// lvgl_port_lock: a WASM app's teardown+relaunch must NOT execute on the httpd task while it holds
// the lock — the app's terminate/frame handshake needs the LVGL thread, which would be blocked on
// that same lock, deadlocking UI + web until the watchdog reboots. We just enqueue, then poll the
// foreground id (lock-free read of a stable literal) so the reply reflects the switch when it lands.
esp_err_t h_ui_open(httpd_req_t *req) {
    char id[40];
    if (!query_param(req, "id", id, sizeof id)) return ESP_OK;
    bool posted = nv_ui_open_app_id_async(id);
    char cur[32] = "";
    for (int i = 0; posted && i < 80; i++) {          // up to ~800 ms for the UI thread to apply
        vTaskDelay(pdMS_TO_TICKS(10));
        snprintf(cur, sizeof cur, "%s", nv_ui_current_app_id());
        if (!strcmp(cur, id)) break;
    }
    bool ok = (strcmp(cur, id) == 0);
    char b[112]; snprintf(b, sizeof b, "{\"ok\":%s,\"app\":\"%s\"}", ok ? "true" : "false", cur);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, b);
}

// GET /api/ui/home -> return to the launcher.
esp_err_t h_ui_home(httpd_req_t *req) {
    // Posted to the LVGL thread (like /api/ui/open): closing a WASM game runs its abort handshake,
    // a Recents thumbnail grab and SD writes — none of that may run on the httpd task under a
    // foreign-held port lock (the documented UI+httpd freeze pattern). ok = the request was queued.
    const bool posted = nv_ui_go_home_async();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, posted ? "{\"ok\":true}" : "{\"ok\":false}");
}

// GET /api/open?path=/sdcard/...[&with=<handler id>] -> open a file on the device exactly like a tap
// in Files: the default app, the "Open with" sheet when ambiguous, or `with` (an opener or an action,
// e.g. sys.wallpaper). Posted to the LVGL thread; ok = queued (the device toasts any failure).
esp_err_t h_open_file(httpd_req_t *req) {
    char path[NV_OPEN_PATH_MAX];
    if (!query_param(req, "path", path, sizeof path)) return ESP_OK;
    if (!open_path_ok(path)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    char with[NV_OPEN_ID_MAX] = "";
    query_param_opt(req, "with", with, sizeof with);
    const bool ok = nv_open_file_async(path, with[0] ? with : nullptr);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// GET /api/open/handlers?path=/sdcard/... -> the file's type and what can handle it:
// {"mime":"image/jpeg","kind":2,"preferred":"gallery.view","default":"",
//  "openers":[{"id":"gallery.view","label":"Gallery"},...],"actions":[...]}
// Read under the LVGL lock: labels and the default-app cache belong to the UI thread.
esp_err_t h_open_handlers(httpd_req_t *req) {
    char path[NV_OPEN_PATH_MAX];
    if (!query_param(req, "path", path, sizeof path)) return ESP_OK;
    if (!open_path_ok(path)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
    if (!lvgl_port_lock(1000)) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ui busy");
    cJSON *root = cJSON_CreateObject();
    const char *mime = nv_open_mime(path);
    cJSON_AddStringToObject(root, "mime", mime);
    cJSON_AddNumberToObject(root, "kind", nv_open_kind_of_mime(mime));
    const NvOpenHandler *pref = nv_open_preferred_mime(mime);
    cJSON_AddStringToObject(root, "preferred", pref ? pref->id : "");
    char def[NV_OPEN_ID_MAX];
    nv_open_get_default(mime, def, sizeof def);
    cJSON_AddStringToObject(root, "default", def);
    const NvOpenHandler *hs[16];
    for (int role = 0; role < 2; role++) {
        cJSON *arr = cJSON_AddArrayToObject(root, role == NV_OPEN_OPENER ? "openers" : "actions");
        const int n = nv_open_handlers_mime(mime, (nv_open_role_t)role, hs, 16);
        for (int i = 0; i < n; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", hs[i]->id);
            cJSON_AddStringToObject(o, "label", nv_open_handler_label(hs[i]));
            cJSON_AddItemToArray(arr, o);
        }
    }
    lvgl_port_unlock();
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t r = httpd_resp_sendstr(req, s ? s : "{}");
    cJSON_free(s);
    return r;
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

// GET /api/ui/input -> text dump of the UI input state (indev internals, gesture flags, overlays,
// touch cache, recent indev events). Diagnostics for "the touch stopped responding".
esp_err_t h_ui_input(httpd_req_t *req) {
    constexpr size_t kN = 8192;
    char *buf = (char *)heap_caps_malloc(kN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    size_t len = 0;
    if (lvgl_port_lock(1000)) { len = nv_ui_input_debug(buf, kN); lvgl_port_unlock(); }
    else len = (size_t)snprintf(buf, kN, "LVGL lock busy (UI thread stuck?)\n");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    const esp_err_t r = httpd_resp_send(req, buf, (ssize_t)len);
    heap_caps_free(buf);
    return r;
}

// GET /api/ui/swipe?x0=&y0=&x1=&y1=&ms= -> synthetic drag (pager/gesture remote tests).
esp_err_t h_ui_swipe(httpd_req_t *req) {
    char q[128]; int x0 = -1, y0 = -1, x1 = -1, y1 = -1, ms = 250;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(q, "x0", v, sizeof v) == ESP_OK) x0 = atoi(v);
        if (httpd_query_key_value(q, "y0", v, sizeof v) == ESP_OK) y0 = atoi(v);
        if (httpd_query_key_value(q, "x1", v, sizeof v) == ESP_OK) x1 = atoi(v);
        if (httpd_query_key_value(q, "y1", v, sizeof v) == ESP_OK) y1 = atoi(v);
        if (httpd_query_key_value(q, "ms", v, sizeof v) == ESP_OK) ms = atoi(v);
    }
    if (x0 < 0 || y0 < 0 || x1 < 0 || y1 < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need x0,y0,x1,y1");
        return ESP_OK;
    }
    if (lvgl_port_lock(1000)) { nv_ui_swipe(x0, y0, x1, y1, ms); lvgl_port_unlock(); }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// GET /api/ui/type?text=<url-encoded> -> insert literal text into the IME's focused field, exactly
// as if typed on the on-screen keyboard (nv_ime_inject_text). {"ok":bool} — false when no field is
// focused; tap one first (e.g. /api/ui/tap on the field), same as a human would. Lets a remote
// caller fill a whole line in one call instead of tapping each on-screen key.
esp_err_t h_ui_type(httpd_req_t *req) {
    char text[256];
    if (!query_param(req, "text", text, sizeof text)) return ESP_OK;
    bool ok = false;
    if (lvgl_port_lock(1000)) { ok = nv_ime_inject_text(text); lvgl_port_unlock(); }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// GET /api/ui/key?code=enter|esc|backspace|delete|tab|left|right|up|down -> a special key on the
// IME's focused field (nv_ime_inject_key), e.g. "enter" to submit a Terminal line typed via
// /api/ui/type. {"ok":bool}.
// /api/ui/hid?usage=0x28[&mods=0x04]: one key through the physical-keyboard path (queue, layout,
// shortcuts, lock screen, IME) — what a real USB / Bluetooth keyboard would do. Numbers are
// decimal or 0x-hex HID usages / boot modifier bits.
esp_err_t h_ui_hid(httpd_req_t *req) {
    char u[8], m[8] = "0";
    if (!query_param(req, "usage", u, sizeof u)) return ESP_OK;
    char *end = nullptr;
    const long usage = strtol(u, &end, 0);
    query_param_opt(req, "mods", m, sizeof m);
    const long mods = strtol(m, nullptr, 0);
    if (!end || *end || usage <= 0 || usage > 0xE7 || mods < 0 || mods > 0xFF) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad usage/mods");
        return ESP_OK;
    }
    const bool ok = nv_hid_host_inject_key((uint8_t)usage, (uint8_t)mods);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

esp_err_t h_ui_key(httpd_req_t *req) {
    char code[16];
    if (!query_param(req, "code", code, sizeof code)) return ESP_OK;
    nv_ime_remote_key_t k;
    if      (!strcmp(code, "enter"))     k = NV_IME_RK_ENTER;
    else if (!strcmp(code, "esc"))       k = NV_IME_RK_ESC;
    else if (!strcmp(code, "backspace")) k = NV_IME_RK_BACKSPACE;
    else if (!strcmp(code, "delete"))    k = NV_IME_RK_DELETE;
    else if (!strcmp(code, "tab"))       k = NV_IME_RK_TAB;
    else if (!strcmp(code, "left"))      k = NV_IME_RK_LEFT;
    else if (!strcmp(code, "right"))     k = NV_IME_RK_RIGHT;
    else if (!strcmp(code, "up"))        k = NV_IME_RK_UP;
    else if (!strcmp(code, "down"))      k = NV_IME_RK_DOWN;
    else { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad code"); return ESP_OK; }
    bool ok = false;
    if (lvgl_port_lock(1000)) { ok = nv_ime_inject_key(k); lvgl_port_unlock(); }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

// ---------------------------------------------------------------- Terminal by text (/api/term/*)
// Run shell command lines and talk to terminal programs without screenshots: what the Terminal
// prints is captured as plain text (nv_term.h) and handed back with a cursor ("seq"). Commands are
// echoed on the screen as if typed. The server has one task: a waiting call holds every other
// request back, so clients keep wait_ms short (a few seconds) and poll /api/term/out.

constexpr uint32_t kTermWaitMaxMs = 120000;
constexpr uint32_t kTermQuietMs   = 300;    // out/input: return once output pauses this long
constexpr size_t   kTermLineMax   = 1000;   // the shell's line buffer is 1024

uint32_t term_query_ms(httpd_req_t *req, const char *key, uint32_t def) {
    char v[16];
    if (!query_param_opt(req, key, v, sizeof v) || !v[0]) return def;
    const long ms = strtol(v, nullptr, 10);
    if (ms <= 0) return 0;
    return ms > (long)kTermWaitMaxMs ? kTermWaitMaxMs : (uint32_t)ms;
}

esp_err_t term_error(httpd_req_t *req, const char *status, const char *msg) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char b[240];
    snprintf(b, sizeof b, "{\"error\":\"%s\"}", msg);
    return httpd_resp_sendstr(req, b);
}

int64_t term_now_ms(void) { return esp_timer_get_time() / 1000; }

// Captured text from `since` as {"out":..,"seq":..,"done":..,"status":..,"reading":..,"trunc":..}.
// The text is "cooked" the way a terminal shows it: CR LF -> LF, a lone CR redraws its line
// (progress bars), backspace erases.
esp_err_t term_reply(httpd_req_t *req, uint64_t since, bool done, const char *extra) {
    char *raw = (char *)heap_caps_malloc(NV_TERM_CAPTURE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *js = (char *)heap_caps_malloc(NV_TERM_CAPTURE_BYTES * 2 + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw || !js) {
        heap_caps_free(raw);
        heap_caps_free(js);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    nv_term_state_t st;
    nv_term_state(&st);
    uint64_t next = since;
    bool lost = false;
    const size_t n = nv_term_read(since, raw, NV_TERM_CAPTURE_BYTES, &next, &lost);
    size_t c = 0;   // cooked length, in place (never longer than the raw text)
    for (size_t i = 0; i < n; i++) {
        const char ch = raw[i];
        if (ch == '\r') {
            if (i + 1 < n && raw[i + 1] == '\n') continue;
            while (c && raw[c - 1] != '\n') c--;           // the line is drawn again
        } else if (ch == '\b') {
            if (c && raw[c - 1] != '\n') {
                do { c--; } while (c && ((unsigned char)raw[c] & 0xC0) == 0x80);
            }
        } else {
            raw[c++] = ch;
        }
    }
    size_t o = 0;
    for (size_t i = 0; i < c; i++) {
        const unsigned char ch = (unsigned char)raw[i];
        if (ch == '"' || ch == '\\') { js[o++] = '\\'; js[o++] = (char)ch; }
        else if (ch == '\n') { js[o++] = '\\'; js[o++] = 'n'; }
        else if (ch == '\t') { js[o++] = '\\'; js[o++] = 't'; }
        else if (ch < 0x20) continue;
        else js[o++] = (char)ch;
    }
    char status[16];
    if (done) snprintf(status, sizeof status, "%d", st.status);
    else snprintf(status, sizeof status, "null");
    char tail[256];
    const int tn = snprintf(tail, sizeof tail,
                            "\",\"seq\":%llu,\"done\":%s,\"status\":%s,\"reading\":%s,\"trunc\":%s%s}",
                            (unsigned long long)next, done ? "true" : "false", status,
                            st.reading ? "true" : "false", lost ? "true" : "false", extra ? extra : "");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t rc = httpd_resp_send_chunk(req, "{\"out\":\"", 8);
    if (rc == ESP_OK && o) rc = httpd_resp_send_chunk(req, js, o);
    if (rc == ESP_OK) rc = httpd_resp_send_chunk(req, tail, (size_t)tn);
    if (rc == ESP_OK) rc = httpd_resp_send_chunk(req, nullptr, 0);
    heap_caps_free(raw);
    heap_caps_free(js);
    return rc;
}

// Wait for output after `since`: until the shell is idle, the output pauses for kTermQuietMs, or
// wait_ms runs out. Returns whether the shell is idle.
bool term_wait_output(uint64_t since, uint32_t wait_ms) {
    const int64_t t0 = term_now_ms();
    int64_t grew = -1;
    uint64_t last = since;
    for (;;) {
        nv_term_state_t st;
        nv_term_state(&st);
        if (st.idle || !st.open) return true;
        const int64_t now = term_now_ms();
        if (st.seq != last) { last = st.seq; grew = now; }
        if (grew >= 0 && now - grew >= kTermQuietMs) return false;
        if (now - t0 >= wait_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// POST /api/term/run?wait_ms=15000  body: a command line (plain text; several lines run in turn).
// Opens the Terminal if needed, types the line, waits until it finishes or wait_ms (max 120000)
// runs out -> {"out","seq","done","status","reading","trunc"}. 409 while a command runs.
esp_err_t h_term_run(httpd_req_t *req) {
    size_t len = 0;
    char *body = recv_body(req, 4096, &len);
    if (!body) return ESP_OK;
    // One shell line: lines become ';'-separated commands, blank lines and CRs drop out.
    size_t o = 0;
    bool sep = false;
    for (size_t i = 0; i < len; i++) {
        const char ch = body[i];
        if (ch == '\r') continue;
        if (ch == '\n') { if (o) sep = true; continue; }
        if (sep) {
            while (o && (body[o - 1] == ' ' || body[o - 1] == '\t')) o--;
            if (o && body[o - 1] != ';' && body[o - 1] != '&' && body[o - 1] != '|') body[o++] = ';';
            body[o++] = ' ';
            sep = false;
        }
        body[o++] = ch;
    }
    while (o && (body[o - 1] == ' ' || body[o - 1] == '\t' || body[o - 1] == ';')) o--;
    body[o] = '\0';
    const char *line = body;
    while (*line == ' ' || *line == '\t') line++;
    const uint32_t wait_ms = term_query_ms(req, "wait_ms", 15000);
    if (strlen(line) > kTermLineMax) {
        free(body);
        return term_error(req, "400 Bad Request", "command line too long (max 1000 bytes)");
    }
    if (!nv_term_open(5000)) {
        free(body);
        return term_error(req, "503 Service Unavailable", "the Terminal could not be opened");
    }
    nv_term_state_t st;
    nv_term_state(&st);
    if (!*line) {   // nothing to run
        free(body);
        return term_reply(req, st.seq, st.idle, nullptr);
    }
    uint64_t seq0 = 0;
    uint32_t jobs0 = 0;
    const nv_term_rc_t rc = nv_term_run(line, &seq0, &jobs0);
    free(body);
    if (rc == NV_TERM_BUSY)
        return term_error(req, "409 Conflict", "shell busy: a command is still running (read it with "
                          "/api/term/out, answer it with /api/term/input, stop it with /api/term/interrupt)");
    if (rc == NV_TERM_CLOSED) return term_error(req, "503 Service Unavailable", "the Terminal closed");
    if (rc != NV_TERM_OK) return term_error(req, "503 Service Unavailable", "ui busy, retry");
    const int64_t t0 = term_now_ms();
    int64_t grew = t0;
    uint64_t last = seq0;
    bool done = false;
    for (;;) {
        nv_term_state(&st);
        if (!st.open || (st.jobs != jobs0 && st.idle)) { done = true; break; }
        const int64_t now = term_now_ms();
        if (st.seq != last) { last = st.seq; grew = now; }
        // An interactive program that printed its prompt and now waits for input: answer now.
        if (st.reading && now - grew >= kTermQuietMs) break;
        if (now - t0 >= wait_ms) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return term_reply(req, seq0, done, nullptr);
}

// GET /api/term/out?since=<seq>&wait_ms=0 -> output after the cursor (all the ring holds without
// since; since=end: nothing yet, just the current cursor). With wait_ms, waits for output to
// arrive and pause, or the shell to go idle.
esp_err_t h_term_out(httpd_req_t *req) {
    char v[24] = "";
    uint64_t since = 0;
    if (query_param_opt(req, "since", v, sizeof v) && v[0]) {
        if (!strcmp(v, "end")) {
            nv_term_state_t st;
            nv_term_state(&st);
            since = st.seq;
        } else {
            since = strtoull(v, nullptr, 10);
        }
    }
    const uint32_t wait_ms = term_query_ms(req, "wait_ms", 0);
    const bool done = term_wait_output(since, wait_ms);
    return term_reply(req, since, done, nullptr);
}

// POST /api/term/input?eof=0&wait_ms=3000  body: text for the running program's stdin (a '\n' is
// added when missing; eof=1 then closes its input, ^D). Replies like /api/term/out with the output
// that followed, plus "written". 409 when no program reads the keyboard.
esp_err_t h_term_input(httpd_req_t *req) {
    size_t len = 0;
    char *body = recv_body(req, 4096, &len);
    if (!body) return ESP_OK;
    char v[8] = "";
    const bool eof = query_param_opt(req, "eof", v, sizeof v) && (v[0] == '1' || v[0] == 't');
    const uint32_t wait_ms = term_query_ms(req, "wait_ms", 3000);
    // A program started a moment ago may still be loading: give it a few seconds to take input.
    nv_term_state_t st;
    for (int i = 0; i < 250; i++) {
        nv_term_state(&st);
        if (st.reading || st.idle || !st.open) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    size_t written = 0;
    const nv_term_rc_t rc = st.reading ? nv_term_input(body, len, eof, &written) : NV_TERM_BUSY;
    free(body);
    if (rc == NV_TERM_BUSY)
        return term_error(req, "409 Conflict", st.idle ? "no program is running: use /api/term/run"
                                                       : "the running command does not read input");
    if (rc == NV_TERM_CLOSED) return term_error(req, "503 Service Unavailable", "the Terminal is not open");
    if (rc != NV_TERM_OK) return term_error(req, "503 Service Unavailable", "ui busy, retry");
    const bool done = term_wait_output(st.seq, wait_ms);
    char extra[40];
    snprintf(extra, sizeof extra, ",\"written\":%u", (unsigned)written);
    return term_reply(req, st.seq, done, extra);
}

// POST /api/term/interrupt?wait_ms=2000 -> ^C, then wait for the shell to go idle.
// {"ok":true,"was_running":bool,"done":bool,"status":<n|null>,"seq":<cursor>}
esp_err_t h_term_interrupt(httpd_req_t *req) {
    const uint32_t wait_ms = term_query_ms(req, "wait_ms", 2000);
    const bool was = nv_term_interrupt();
    nv_term_state_t st;
    const int64_t t0 = term_now_ms();
    for (;;) {
        nv_term_state(&st);
        if (st.idle || !st.open || term_now_ms() - t0 >= wait_ms) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    const bool done = st.idle || !st.open;
    char b[144], status[16];
    if (done) snprintf(status, sizeof status, "%d", st.status);
    else snprintf(status, sizeof status, "null");
    snprintf(b, sizeof b, "{\"ok\":true,\"was_running\":%s,\"done\":%s,\"status\":%s,\"seq\":%llu}",
             was ? "true" : "false", done ? "true" : "false", status, (unsigned long long)st.seq);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, b);
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
    cfg.max_resp_headers = 16;         // 4 security headers on every response + the handlers' own
    cfg.stack_size = 8192;
    // MUST exceed the total registered handlers: the routes[] table + /ws + the "/*" catch-all.
    // esp_http_server silently drops registrations past this cap, and since "/*" (h_static) is
    // registered LAST, an undersized cap makes it vanish — every web page 404s ("Nothing matches
    // the given URI") while /api/* still works. Keep comfortably above the array size below.
    cfg.max_uri_handlers = 96;         // ~75 API routes + /ws + /* today: keep headroom
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
        {"/api/crash",       HTTP_GET,  h_crash,       nullptr},
        {"/api/security/events", HTTP_GET, h_sec_events, nullptr},
        {"/api/crash/dump",  HTTP_GET,  h_crash_dump,  nullptr},
        {"/api/intr",        HTTP_GET,  h_intr,        nullptr},
        {"/api/display",     HTTP_GET,  h_display,     nullptr},
        {"/api/status",      HTTP_GET,  h_status,      nullptr},
        {"/api/auth/status", HTTP_GET,  h_auth_status, nullptr},
        {"/api/pair",        HTTP_POST, h_pair,        nullptr},
        {"/api/apps",        HTTP_GET,  h_apps,        nullptr},
        {"/api/associations",HTTP_GET,  h_assoc,       nullptr},
        {"/api/anima/caps",  HTTP_GET,  h_anima_caps,  nullptr},
        {"/api/anima/net",   HTTP_GET,  h_anima_net,   nullptr},
        {"/api/anima/net",   HTTP_POST, h_anima_net,   nullptr},
        {"/api/anima/wake",  HTTP_GET,  h_anima_wake,  nullptr},
        {"/api/anima/wake",  HTTP_POST, h_anima_wake,  nullptr},
        {"/api/anima/hb",    HTTP_GET,  h_anima_hb,    nullptr},
        {"/api/anima/hb",    HTTP_POST, h_anima_hb,    nullptr},
        {"/api/anima/telegram", HTTP_GET,  h_anima_tg, nullptr},
        {"/api/anima/telegram", HTTP_POST, h_anima_tg, nullptr},
        {"/api/anima/models",HTTP_GET,  h_anima_models,nullptr},
        {"/api/llm",         HTTP_GET,  h_llm,         nullptr},
        {"/api/llm",         HTTP_POST, h_llm,         nullptr},
        {"/api/anima/chat",  HTTP_POST, h_anima_chat,  nullptr},
        {"/api/anima/conv",  HTTP_GET,  h_anima_conv_get,  nullptr},
        {"/api/anima/conv",  HTTP_POST, h_anima_conv_post, nullptr},
        {"/api/anima/memory",HTTP_GET,  h_anima_mem_get,   nullptr},
        {"/api/anima/memory",HTTP_POST, h_anima_mem_post,  nullptr},
        {"/api/anima",       HTTP_GET,  h_anima_get,   nullptr},
        {"/api/media/play",  HTTP_POST, h_media_play,  nullptr},
        {"/api/media/stop",  HTTP_POST, h_media_stop,  nullptr},
        {"/api/media/state", HTTP_GET,  h_media_state, nullptr},
        {"/api/video/play",  HTTP_POST, h_video_play,  nullptr},
        {"/api/video/stop",  HTTP_POST, h_video_stop,  nullptr},
        {"/api/video/state", HTTP_GET,  h_video_state, nullptr},
        {"/api/video/seek",  HTTP_POST, h_video_seek,  nullptr},
        {"/api/video/pause", HTTP_POST, h_video_pause, nullptr},
        {"/api/audio/selftest", HTTP_GET, h_audio_selftest, nullptr},
        {"/api/anima/query", HTTP_POST, h_anima_query, nullptr},
        {"/api/heap",        HTTP_GET,  h_heap,        nullptr},
        {"/api/heap/map",    HTTP_GET,  h_heap_map,    nullptr},
        {"/api/cpu",         HTTP_GET,  h_cpu,         nullptr},
        {"/api/fs/list",     HTTP_GET,  h_fs_list,     nullptr},
        {"/api/fs/read",     HTTP_GET,  h_fs_read,     nullptr},
        {"/api/logs",        HTTP_GET,  h_logs,        nullptr},
        {"/api/screen",      HTTP_GET,  h_screen,      nullptr},
        {"/api/ui/state",    HTTP_GET,  h_ui_state,    nullptr},
        {"/api/ui/apps",     HTTP_GET,  h_ui_apps,     nullptr},
        {"/api/ui/open",     HTTP_GET,  h_ui_open,     nullptr},
        {"/api/ui/home",     HTTP_GET,  h_ui_home,     nullptr},
        {"/api/open",        HTTP_GET,  h_open_file,   nullptr},
        {"/api/open/handlers", HTTP_GET, h_open_handlers, nullptr},
        {"/api/ui/tap",      HTTP_GET,  h_ui_tap,      nullptr},
        {"/api/ui/swipe",    HTTP_GET,  h_ui_swipe,    nullptr},
        {"/api/ui/type",     HTTP_GET,  h_ui_type,     nullptr},
        {"/api/ui/key",      HTTP_GET,  h_ui_key,      nullptr},
        {"/api/ui/hid",      HTTP_GET,  h_ui_hid,      nullptr},
        {"/api/ui/input",    HTTP_GET,  h_ui_input,    nullptr},
        {"/api/say",         HTTP_GET,  h_say,         nullptr},
        {"/api/term/run",    HTTP_POST, h_term_run,    nullptr},
        {"/api/term/out",    HTTP_GET,  h_term_out,    nullptr},
        {"/api/term/input",  HTTP_POST, h_term_input,  nullptr},
        {"/api/term/interrupt", HTTP_POST, h_term_interrupt, nullptr},
        {"/api/fs/write",    HTTP_POST, h_fs_write,    nullptr},
        {"/api/fs/mkdir",    HTTP_POST, h_fs_mkdir,    nullptr},
        {"/api/fs/delete",   HTTP_POST, h_fs_delete,   nullptr},
        {"/api/fs/move",     HTTP_POST, h_fs_move,     nullptr},
        {"/api/bench/sd",    HTTP_GET,  h_bench_sd,    nullptr},
        {"/api/bench/nvs",   HTTP_GET,  h_bench_nvs,   nullptr},
        {"/api/bench/sink",  HTTP_POST, h_bench_sink,  nullptr},
        {"/api/time/set",    HTTP_POST, h_time_set,    nullptr},
        {"/api/home",        HTTP_GET,  h_home_get,    nullptr},
        {"/api/home",        HTTP_POST, h_home_post,   nullptr},
        {"/api/reboot",      HTTP_POST, h_reboot,      nullptr},
        {"/api/app/run",     HTTP_POST, h_app_run,     nullptr},
        {"/api/web/put",     HTTP_POST, h_web_put,     nullptr},
        {"/api/wifi/scan",   HTTP_GET,  h_wifi_scan,   nullptr},
        {"/api/wifi/known",  HTTP_GET,  h_wifi_known,  nullptr},
        {"/api/wifi/join",   HTTP_POST, h_wifi_join,   nullptr},
        {"/api/usb",         HTTP_GET,  h_usb,         nullptr},
        {"/api/usb/eject",   HTTP_POST, h_usb_eject,   nullptr},
        {"/api/pads",        HTTP_GET,  h_pads,        nullptr},
        {"/api/bt",          HTTP_GET,  h_bt_get,      nullptr},
        {"/api/bt",          HTTP_POST, h_bt_post,     nullptr},
    };
    // Everything but these needs a paired session (see req_authed): discovery/version, the pairing
    // status probe and pairing itself.
    static const char *const kPublicRoutes[] = {"/api/info", "/api/auth/status", "/api/pair"};
    for (httpd_uri_t r : routes) {
        bool pub = false;
        for (const char *p : kPublicRoutes) pub = pub || strcmp(r.uri, p) == 0;
        r.user_ctx = reinterpret_cast<void *>(r.handler);
        r.handler = pub ? h_open : h_guarded;   // both: security headers + Host/Origin checks
        httpd_register_uri_handler(s_srv, &r);
    }

    // /ws (WebSocket) then the catch-all static "/*" MUST register last, in this order: the wildcard
    // would otherwise swallow /ws (first-match wins).
    httpd_uri_t ws = {};
    ws.uri = "/ws"; ws.method = HTTP_GET; ws.handler = h_ws; ws.is_websocket = true;
    ws.ws_pre_handshake_cb = ws_pre_handshake;
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
