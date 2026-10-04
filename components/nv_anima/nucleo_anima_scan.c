// LAN model-server discovery — finds Ollama / LM Studio / llama.cpp servers on the board's own subnet
// with no address typed in. A background task sweeps the /24 the station sits in with non-blocking TCP
// connects on the well-known ports (Ollama 11434, LM Studio 1234, llama.cpp / LocalAI 8080), then asks
// every open port GET /v1/models: only a real OpenAI-compatible model server makes the list. The
// result (servers and their chat models) feeds the model picker, and it re-links the teacher by
// itself: when the configured LAN server stops answering (the PC got a new address from DHCP) but the
// same model is served elsewhere, teacher.json is pointed there — the "it worked yesterday" case.
//
// Cost: ~254 hosts x 3 ports in batches of kBatch sockets, ~kConnMs each -> ~25 s of background work,
// at boot (once Wi-Fi is up), every kPeriodUs, and on demand (picker opened, a LAN call failed).
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "cJSON.h"

#include "nucleo_anima.h"
#include "nucleo_setup.h"

static const char *TAG = "anima_scan";

#define SCAN_MAX_SRV   8
#define SCAN_MAX_MOD   32
#define kBatch         12                       // sockets in flight (LWIP_MAX_SOCKETS is 24, shared)
#define kConnMs        350                      // a LAN host answers a SYN in a few ms; dead ones never
#define kPeriodUs      (10LL * 60 * 1000000)    // routine sweep every 10 min
#define kForceGapUs    (45LL * 1000000)         // an on-demand sweep at most every 45 s

static const uint16_t kPorts[] = {11434, 1234, 8080};
static const char *const kKind[] = {"Ollama", "LM Studio", "llama.cpp"};

typedef struct { char base[64]; char host[16]; uint8_t kind; } scan_srv_t;
typedef struct { char model[64]; uint8_t srv; } scan_mod_t;

// Last scan result (~2.7 KB), task context only: PSRAM, not internal .bss.
EXT_RAM_BSS_ATTR static scan_srv_t s_srv[SCAN_MAX_SRV];
EXT_RAM_BSS_ATTR static scan_mod_t s_mod[SCAN_MAX_MOD];
static int s_nsrv, s_nmod;
static int64_t s_last_us;                       // last completed sweep (0 = never)
static volatile bool s_busy;
// Diagnostics of the last sweep (GET /api/anima/lan): what the board actually saw on the LAN.
static int s_sweeps, s_open, s_sockfail, s_ms;
static bool s_netfail, s_relinked;
static SemaphoreHandle_t s_mx;                  // guards the published lists

static bool lock(void) {
    if (!s_mx) s_mx = xSemaphoreCreateMutex();
    return s_mx && xSemaphoreTake(s_mx, pdMS_TO_TICKS(2000)) == pdTRUE;
}
static void unlock(void) { xSemaphoreGive(s_mx); }

// GET <base>/models on a LAN server (no TLS, short timeout). Malloc'd body or NULL.
static char *http_get_small(const char *url)
{
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 2500, .buffer_size = 1024 };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return NULL;
    char *out = NULL;
    if (esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h);
        if (esp_http_client_get_status_code(h) == 200) {
            const int cap = 16 * 1024;
            out = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            int n = 0, r;
            while (out && n < cap - 1 && (r = esp_http_client_read(h, out + n, cap - 1 - n)) > 0) n += r;
            if (out) out[n] = 0;
        }
    }
    esp_http_client_cleanup(h);
    return out;
}

// Same filter as the picker: embeddings, rerankers, speech and image models cannot chat.
static bool chat_model(const char *id)
{
    char lo[64]; int i = 0;
    for (; id[i] && i < (int)sizeof lo - 1; i++) lo[i] = (char)tolower((unsigned char)id[i]);
    lo[i] = 0;
    static const char *const no[] = {"embed", "rerank", "bge-", "whisper", "tts", "dall-e", "stable-diffusion", "moderation", NULL};
    for (int k = 0; no[k]; k++) if (strstr(lo, no[k])) return false;
    return true;
}

// Ask an open port what it serves; record it when it is a model server.
static void probe_server(uint32_t ip_be, int port_idx, scan_srv_t *srv, int *nsrv, scan_mod_t *mod, int *nmod)
{
    if (*nsrv >= SCAN_MAX_SRV) return;
    char host[16], url[96];
    const uint8_t *b = (const uint8_t *)&ip_be;
    snprintf(host, sizeof host, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    snprintf(url, sizeof url, "http://%s:%u/v1/models", host, kPorts[port_idx]);
    char *body = http_get_small(url);
    if (!body) return;
    cJSON *o = cJSON_Parse(body);
    heap_caps_free(body);
    cJSON *data = o ? cJSON_GetObjectItem(o, "data") : NULL;
    if (!cJSON_IsArray(data)) { cJSON_Delete(o); return; }   // something else listens there (a web app)
    const int si = (*nsrv)++;
    snprintf(srv[si].host, sizeof srv[si].host, "%s", host);
    snprintf(srv[si].base, sizeof srv[si].base, "http://%s:%u/v1", host, kPorts[port_idx]);
    srv[si].kind = (uint8_t)port_idx;
    cJSON *e;
    cJSON_ArrayForEach(e, data) {
        const cJSON *id = cJSON_GetObjectItem(e, "id");
        if (!cJSON_IsString(id) || !id->valuestring[0] || strpbrk(id->valuestring, "\"\\") || !chat_model(id->valuestring)) continue;
        if (*nmod >= SCAN_MAX_MOD) break;
        snprintf(mod[*nmod].model, sizeof mod[0].model, "%s", id->valuestring);
        mod[*nmod].srv = (uint8_t)si;
        (*nmod)++;
    }
    cJSON_Delete(o);
    ESP_LOGI(TAG, "found %s at %s", kKind[port_idx], srv[si].base);
}

// One batch of non-blocking connects; marks open[i] for the targets that accepted.
static void connect_batch(const uint32_t *ip, const int *port, int n, bool *open)
{
    int fd[kBatch];
    for (int i = 0; i < n; i++) {
        open[i] = false;
        fd[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd[i] < 0) { s_sockfail++; continue; }
        fcntl(fd[i], F_SETFL, fcntl(fd[i], F_GETFL, 0) | O_NONBLOCK);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(kPorts[port[i]]), .sin_addr.s_addr = ip[i] };
        if (connect(fd[i], (struct sockaddr *)&a, sizeof a) == 0) open[i] = true;
        else if (errno != EINPROGRESS) { close(fd[i]); fd[i] = -1; }
    }
    const int64_t until = esp_timer_get_time() + kConnMs * 1000LL;
    for (;;) {
        fd_set wr; FD_ZERO(&wr);
        int maxfd = -1, pending = 0;
        for (int i = 0; i < n; i++) if (fd[i] >= 0 && !open[i]) { FD_SET(fd[i], &wr); if (fd[i] > maxfd) maxfd = fd[i]; pending++; }
        const int64_t left = until - esp_timer_get_time();
        if (!pending || left <= 0) break;
        struct timeval tv = { .tv_sec = 0, .tv_usec = (long)left };
        if (select(maxfd + 1, NULL, &wr, NULL, &tv) <= 0) break;
        for (int i = 0; i < n; i++) {
            if (fd[i] < 0 || open[i] || !FD_ISSET(fd[i], &wr)) continue;
            int err = 0; socklen_t el = sizeof err;
            getsockopt(fd[i], SOL_SOCKET, SO_ERROR, &err, &el);
            if (err == 0) open[i] = true;
            else { close(fd[i]); fd[i] = -1; }
        }
    }
    for (int i = 0; i < n; i++) if (fd[i] >= 0) close(fd[i]);
}

bool nucleo_anima_teacher_relink(const char *const *bases, const char *const *models, int n);   // nucleo_anima_online.c

static void scan_task(void *arg)
{
    (void)arg;
    esp_netif_ip_info_t ipi = {0};
    esp_netif_t *nif = esp_netif_get_default_netif();
    if (!nif || esp_netif_get_ip_info(nif, &ipi) != ESP_OK || !ipi.ip.addr) {
        ESP_LOGW(TAG, "sweep skipped: no default netif / address");
        s_netfail = true; s_sweeps++;
        s_busy = false; vTaskDeleteWithCaps(NULL); return;
    }
    s_netfail = false;
    int nopen = 0;

    scan_srv_t *srv = (scan_srv_t *)heap_caps_calloc(SCAN_MAX_SRV, sizeof *srv, MALLOC_CAP_SPIRAM);
    scan_mod_t *mod = (scan_mod_t *)heap_caps_calloc(SCAN_MAX_MOD, sizeof *mod, MALLOC_CAP_SPIRAM);
    int nsrv = 0, nmod = 0;
    const int64_t t0 = esp_timer_get_time();
    if (srv && mod) {
        // The /24 around our address (a wider netmask still sweeps only these 254: bounded work).
        const uint32_t self = ipi.ip.addr, net = self & htonl(0xFFFFFF00);
        uint32_t ip[kBatch]; int port[kBatch]; bool open[kBatch];
        for (int pi = 0; pi < 3; pi++) {                     // Ollama's port first: the common case ends early
            int n = 0;
            for (int h = 1; h <= 254; h++) {
                const uint32_t a = net | htonl((uint32_t)h);
                if (a == self) continue;
                ip[n] = a; port[n] = pi; n++;
                if (n == kBatch || h == 254) {
                    connect_batch(ip, port, n, open);
                    for (int i = 0; i < n; i++) if (open[i]) { nopen++; probe_server(ip[i], port[i], srv, &nsrv, mod, &nmod); }
                    n = 0;
                }
            }
        }
        if (lock()) {
            memcpy(s_srv, srv, sizeof s_srv); memcpy(s_mod, mod, sizeof s_mod);
            s_nsrv = nsrv; s_nmod = nmod;
            s_last_us = esp_timer_get_time();
            s_open = nopen; s_ms = (int)((s_last_us - t0) / 1000); s_sweeps++;
            unlock();
        }
        // Re-link: hand the engine every (server, model) pair; it moves the teacher only when its own
        // LAN server is gone and the same model lives elsewhere.
        if (nmod) {
            const char **b = (const char **)malloc(sizeof(char *) * nmod), **m = (const char **)malloc(sizeof(char *) * nmod);
            if (b && m) {
                for (int i = 0; i < nmod; i++) { b[i] = srv[mod[i].srv].base; m[i] = mod[i].model; }
                if (nucleo_anima_teacher_relink(b, m, nmod)) s_relinked = true;
            }
            free(b); free(m);
        }
    }
    ESP_LOGI(TAG, "sweep done in %lld ms: %d server(s), %d model(s)", (esp_timer_get_time() - t0) / 1000, nsrv, nmod);
    heap_caps_free(srv); heap_caps_free(mod);
    s_busy = false;
    vTaskDeleteWithCaps(NULL);   // PSRAM stack (see nucleo_anima_scan_start)
}

void nucleo_anima_scan_start(bool force)
{
    if (s_busy || !nucleo_setup_ip()[0]) return;
    const int64_t now = esp_timer_get_time();
    if (s_last_us && now - s_last_us < (force ? kForceGapUs : kPeriodUs)) return;
    s_busy = true;
    // PSRAM stack: lwIP sockets + esp_http_client, no flash work (keep it that way: not even an
    // esp_partition_mmap, which stops the cache); a small task, gone when done.
    if (xTaskCreateWithCaps(scan_task, "anima_scan", 8192, NULL, 2, NULL, MALLOC_CAP_SPIRAM) != pdPASS) s_busy = false;
}

bool nucleo_anima_scan_busy(void) { return s_busy; }

// The discovered chat models as JSON [{"m":model,"b":base,"h":host,"k":kind}], servers in sweep order.
int nucleo_anima_scan_models(char *out, int cap)
{
    if (!out || cap < 3) return -1;
    int o = snprintf(out, cap, "["), n = 0;
    if (lock()) {
        for (int i = 0; i < s_nmod && o < cap - 2; i++) {
            const scan_srv_t *s = &s_srv[s_mod[i].srv];
            const int w = snprintf(out + o, cap - o, "%s{\"m\":\"%s\",\"b\":\"%s\",\"h\":\"%s\",\"k\":\"%s\"}",
                                   n ? "," : "", s_mod[i].model, s->base, s->host, kKind[s->kind]);
            if (w < 0 || o + w >= cap - 1) break;
            o += w; n++;
        }
        unlock();
    }
    snprintf(out + o, cap - o, "]");
    return n;
}

// The first discovered server and its first chat model (zero-config: no teacher.json at all).
bool nucleo_anima_scan_first(char *base, size_t bcap, char *model, size_t mcap)
{
    bool ok = false;
    if (lock()) {
        if (s_nmod) {
            snprintf(base, bcap, "%s", s_srv[s_mod[0].srv].base);
            snprintf(model, mcap, "%s", s_mod[0].model);
            ok = true;
        }
        unlock();
    }
    return ok;
}

// Diagnostics for GET /api/anima/lan: {"busy","sweeps","age_s","ms","open","sockfail","netfail","relinked","models":[...]}
int nucleo_anima_scan_status(char *out, int cap)
{
    if (!out || cap < 200) return -1;
    const int64_t now = esp_timer_get_time();
    int o = snprintf(out, cap, "{\"busy\":%s,\"sweeps\":%d,\"age_s\":%d,\"ms\":%d,\"open\":%d,\"sockfail\":%d,"
                     "\"netfail\":%s,\"relinked\":%s,\"models\":",
                     s_busy ? "true" : "false", s_sweeps, s_last_us ? (int)((now - s_last_us) / 1000000) : -1, s_ms,
                     s_open, s_sockfail, s_netfail ? "true" : "false", s_relinked ? "true" : "false");
    if (o < 0 || o >= cap - 4) return -1;
    if (nucleo_anima_scan_models(out + o, cap - o - 1) < 0) snprintf(out + o, cap - o, "[]");
    o = (int)strlen(out);
    if (o < cap - 1) { out[o++] = '}'; out[o] = 0; }
    return o;
}
