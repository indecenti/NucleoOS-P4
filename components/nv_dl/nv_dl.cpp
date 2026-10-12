// nv_dl — the system downloader. See include/nv_dl.h and docs/ZIM_READER_PLAN.md §4f.
//
// The worker (one task, PSRAM stack: it writes the SD card, never flash) takes the oldest queued job:
//  1. "meta4": fetches the Metalink, keeps size, hashes and mirrors in the record and <id>.sha1;
//  2. checks the room on the card (what is left to fetch + the 64 MB an OTA update needs);
//  3. "fetch": one GET from the last verified byte (Range), pieces hashed as they stream; a piece is
//     written in 256 KB blocks from a 64-byte aligned buffer at sector-aligned offsets (the SDMMC DMA
//     moves those directly), synced, and only then counted in the record;
//  4. "check" (plain URL only): the whole file against the caller's SHA-256;
//  5. renames <name>.part into place and calls the hook (the UI posts the notification).
// A dropped connection resumes from the record; two failures in a row move to the next mirror; a piece
// that is wrong three times fails the job (the user can retry it).
#include "nv_dl.h"
#include "nv_dl_core.h"
#include "nv_net_policy.h"

#include "nv_config.h"
#include "nv_eth.h"
#include "nv_log.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS: internal RAM is budgeted
#include "nv_ota.h"
#include "nv_sd.h"
#include "nv_sealed.h"
#include "nv_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "lwip/netdb.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"

#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "dl";

namespace {

constexpr char     kDir[]      = "/sdcard/data/dl";
constexpr size_t   kChunk      = 256 * 1024;           // one SD write
constexpr size_t   kMeta4Max   = 4 * 1024 * 1024;      // ~70k pieces: a 280 GB file
constexpr uint64_t kReserve    = 64ull << 20;          // NV_APPSTORE_OTA_RESERVE: room an update needs
constexpr int      kNetFails   = 15;                   // in a row, without a verified piece: failed
constexpr int      kPieceFails = 3;                    // the same piece wrong this often: failed
constexpr size_t   kRecMax     = 8192;

struct Job {
    bool       used;
    nvdl_job_t rec;           // under s_lock; the worker publishes its copy at every checkpoint
    uint64_t   done;
    uint32_t   bps;
    bool       running;
    bool       want_pause;
    bool       want_cancel;
    char       phase[12];
    int64_t    not_before_us;   // after a transient failure: not picked again before this
};

Job              *s_jobs = nullptr;                    // NV_DL_MAX, PSRAM
SemaphoreHandle_t s_lock = nullptr;
uint32_t          s_gen = 0;
uint32_t          s_next_id = 1;
const char       *s_waiting = "";
uint32_t          s_loaded_sd = UINT32_MAX;            // nv_sd_generation() the queue was read at
TaskHandle_t      s_task = nullptr;
void            (*s_hook)(const nv_dl_info_t *) = nullptr;
NV_PSRAM_BSS char s_ua[96];                           // User-Agent, filled by nv_dl_start

void lock()   { xSemaphoreTake(s_lock, portMAX_DELAY); }
void unlock() { xSemaphoreGive(s_lock); }
void bump()   { s_gen++; }
void kick()   { if (s_task) xTaskNotifyGive(s_task); }

bool init() {
    if (s_lock && s_jobs) return true;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_jobs) s_jobs = (Job *)heap_caps_calloc(NV_DL_MAX, sizeof(Job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_lock && s_jobs;
}

bool net_up() { return nv_wifi_get_state() == NV_WIFI_CONNECTED || nv_eth_get_state() == NV_ETH_UP; }

Job *find(uint32_t id) {
    for (int i = 0; i < NV_DL_MAX; i++) if (s_jobs[i].used && s_jobs[i].rec.id == id) return &s_jobs[i];
    return nullptr;
}

void info_of(const Job &j, nv_dl_info_t *o) {
    memset(o, 0, sizeof *o);
    o->id = j.rec.id;
    snprintf(o->owner, sizeof o->owner, "%s", j.rec.owner);
    snprintf(o->title, sizeof o->title, "%s", j.rec.title);
    snprintf(o->dest, sizeof o->dest, "%s", j.rec.dest);
    o->state = j.running ? NV_DL_RUNNING : (int)j.rec.state;
    o->size = j.rec.prepared ? j.rec.size : 0;
    o->verified = j.rec.verified;
    o->done = j.running && j.done > j.rec.verified ? j.done : j.rec.verified;
    o->bps = j.running ? j.bps : 0;
    snprintf(o->phase, sizeof o->phase, "%s", j.running ? j.phase : "");
    snprintf(o->error, sizeof o->error, "%s", j.rec.error);
}

void path_of(char *out, size_t n, uint32_t id, const char *ext) { snprintf(out, n, "%s/%" PRIu32 ".%s", kDir, id, ext); }

void mkdirs(const char *path) {
    char p[NVDL_PATH_MAX];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0777); *s = '/'; }
    mkdir(p, 0777);
}

// The record, atomically enough for FAT: <id>.job.tmp written and synced, then renamed over <id>.job
// (FAT rename won't overwrite: the old one goes first; load() takes the .tmp if that is all there is).
bool save(const nvdl_job_t &r) {
    char *buf = (char *)heap_caps_malloc(kRecMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return false;
    const int n = nvdl_job_format(&r, buf, kRecMax);
    char path[64], tmp[72];
    path_of(path, sizeof path, r.id, "job");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    bool ok = false;
    if (n > 0) {
        mkdirs(kDir);
        if (FILE *f = nv_sd_fopen(tmp, "wb")) {
            ok = fwrite(buf, 1, (size_t)n, f) == (size_t)n && fflush(f) == 0 && fsync(fileno(f)) == 0;
            nv_sd_fclose(f);
        }
        if (ok) { unlink(path); ok = rename(tmp, path) == 0; }
    }
    free(buf);
    if (!ok) NV_LOGE(TAG, "%" PRIu32 ": could not write the record (errno %d)", r.id, errno);
    return ok;
}

// What a job leaves on the card besides a finished file: the .part of every part and its records. A
// finished file is never deleted here: from then on it belongs to whoever reads it.
void delete_files(const nvdl_job_t &r) {
    char p[NVDL_PATH_MAX + 8], q[sizeof p + 8];
    const uint32_t np = r.prepared && nvdl_parts(r.size) ? nvdl_parts(r.size) : 1;
    for (uint32_t i = 0; i < np; i++) {
        if (!nvdl_part_name(r.dest, i, np, p, sizeof p)) continue;
        snprintf(q, sizeof q, "%s.part", p);
        unlink(q);
    }
    path_of(p, sizeof p, r.id, "job");  unlink(p);
    snprintf(q, sizeof q, "%s.tmp", p); unlink(q);
    path_of(p, sizeof p, r.id, "sha1"); unlink(p);
}

char *read_file(const char *path, size_t max, size_t *len) {
    FILE *f = nv_sd_fopen(path, "rb");
    if (!f) return nullptr;
    char *buf = (char *)heap_caps_malloc(max + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t n = buf ? fread(buf, 1, max + 1, f) : 0;
    nv_sd_fclose(f);
    if (!buf || n > max) { free(buf); return nullptr; }
    buf[n] = 0;
    *len = n;
    return buf;
}

// The queue as the card has it. Under s_lock.
void load_locked() {
    for (int i = 0; i < NV_DL_MAX; i++) if (!s_jobs[i].running) s_jobs[i].used = false;
    DIR *d = opendir(kDir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        const char *dot = strchr(e->d_name, '.');
        if (!dot) continue;
        const bool tmp = !strcmp(dot, ".job.tmp");
        if (strcmp(dot, ".job") && !tmp) continue;
        char path[64 + 256];
        snprintf(path, sizeof path, "%s/%s", kDir, e->d_name);
        if (tmp) {                                     // only when the rename never happened
            char real[sizeof path];
            snprintf(real, sizeof real, "%.*s", (int)(strlen(path) - 4), path);
            struct stat st;
            if (stat(real, &st) == 0) continue;
        }
        size_t n = 0;
        char *text = read_file(path, kRecMax, &n);
        if (!text) continue;
        nvdl_job_t *r = (nvdl_job_t *)heap_caps_malloc(sizeof(nvdl_job_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const bool ok = r && nvdl_job_parse(text, n, r);
        free(text);
        if (ok && !find(r->id)) {
            for (int i = 0; i < NV_DL_MAX; i++) {
                if (s_jobs[i].used) continue;
                memset(&s_jobs[i], 0, sizeof(Job));
                s_jobs[i].used = true;
                s_jobs[i].rec = *r;
                if (r->id >= s_next_id) s_next_id = r->id + 1;
                break;
            }
        } else if (!ok) {
            NV_LOGW(TAG, "%s: not a download record, ignored", e->d_name);
        }
        free(r);
    }
    closedir(d);
    bump();
}

void ensure_loaded() {
    if (!nv_sd_is_mounted()) return;
    const uint32_t g = nv_sd_generation();
    lock();
    if (g != s_loaded_sd) { s_loaded_sd = g; load_locked(); }
    unlock();
}

// ---- network --------------------------------------------------------------------------------------

// True when every address `host` resolves to is public (nv_net_policy): a mirror or a redirect must not
// reach the home network.
bool host_public(const char *host) {
    uint32_t ip;
    if (np_parse_ipv4(host, &ip)) return !np_ip_is_private(ip);
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res) return false;
    bool ok = true;
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        const uint32_t v = ntohl(((struct sockaddr_in *)a->ai_addr)->sin_addr.s_addr);
        if (np_ip_is_private(v)) ok = false;
    }
    freeaddrinfo(res);
    return ok;
}

bool url_allowed(const char *url, char *err, size_t en) {
    np_url_t u;
    if (!np_url_parse(url, &u) || u.scheme != NP_HTTPS) { snprintf(err, en, "not an https URL"); return false; }
    if (!host_public(u.host)) { snprintf(err, en, "%s: not a public address", u.host); return false; }
    return true;
}

struct Resp { char range[64]; };

esp_err_t on_evt(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value && e->user_data &&
        !strcasecmp(e->header_key, "Content-Range"))
        snprintf(((Resp *)e->user_data)->range, sizeof(((Resp *)e->user_data)->range), "%s", e->header_value);
    return ESP_OK;
}

// GET url (from byte `from` when > 0), following up to 4 redirects that stay https and public. Returns
// the open client (headers read) or nullptr with `err` set.
esp_http_client_handle_t http_open(const char *url, uint64_t from, Resp *resp, int *status, int64_t *len,
                                   char *err, size_t en) {
    *status = 0;
    if (!url_allowed(url, err, en)) return nullptr;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 30000;                  // a TLS handshake over a weak link alone can take 15 s
    cfg.buffer_size = 16 * 1024;
    cfg.buffer_size_tx = 2048;               // long mirror URLs in the request line
    cfg.disable_auto_redirect = true;
    cfg.user_agent = s_ua;
    cfg.event_handler = on_evt;
    cfg.user_data = resp;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { snprintf(err, en, "out of memory"); return nullptr; }
    char range[48];
    if (from) {
        snprintf(range, sizeof range, "bytes=%" PRIu64 "-", from);
        esp_http_client_set_header(c, "Range", range);
    }
    for (int hop = 0;; hop++) {
        resp->range[0] = 0;
        if (esp_http_client_open(c, 0) != ESP_OK) { snprintf(err, en, "no connection"); break; }
        *len = esp_http_client_fetch_headers(c);
        *status = esp_http_client_get_status_code(c);
        const bool redirect = *status == 301 || *status == 302 || *status == 303 || *status == 307 || *status == 308;
        if (!redirect) return c;
        if (hop >= 4 || esp_http_client_set_redirection(c) != ESP_OK) { snprintf(err, en, "too many redirects"); break; }
        char next[NVDL_URL_MAX];
        if (esp_http_client_get_url(c, next, sizeof next) != ESP_OK || !url_allowed(next, err, en)) break;
        esp_http_client_close(c);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return nullptr;
}

void http_done(esp_http_client_handle_t c) {
    if (!c) return;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
}

// ---- the files being written ----------------------------------------------------------------------

struct Out {
    const nvdl_job_t *r;
    uint32_t nparts;
    int      part = -1;
    int      fd = -1;
};

void out_close(Out &o) {
    if (o.fd >= 0) { fsync(o.fd); close(o.fd); nv_sd_session_end(); }
    o.fd = -1;
    o.part = -1;
}

bool out_open(Out &o, int part) {
    if (o.part == part && o.fd >= 0) return true;
    out_close(o);
    char name[NVDL_PATH_MAX + 8], p[NVDL_PATH_MAX];
    if (!nvdl_part_name(o.r->dest, (uint32_t)part, o.nparts, p, sizeof p)) return false;
    snprintf(name, sizeof name, "%s.part", p);
    if (!nv_sd_session_begin()) return false;
    o.fd = open(name, O_WRONLY | O_CREAT, 0666);
    if (o.fd < 0) { nv_sd_session_end(); return false; }
    o.part = part;
    return true;
}

// Bytes [abs, abs+n) of the download, split at the part boundaries.
bool out_write(Out &o, uint64_t abs, const uint8_t *buf, size_t n) {
    while (n) {
        const int part = (int)(abs / NVDL_PART_SIZE);
        const uint64_t in = abs - (uint64_t)part * NVDL_PART_SIZE;
        const size_t k = (size_t)(NVDL_PART_SIZE - in < n ? NVDL_PART_SIZE - in : n);
        if (!out_open(o, part) || lseek(o.fd, (off_t)in, SEEK_SET) != (off_t)in) return false;
        if (write(o.fd, buf, k) != (ssize_t)k) return false;
        abs += k; buf += k; n -= k;
    }
    return true;
}

bool out_sync(Out &o) { return o.fd < 0 || fsync(o.fd) == 0; }

// Size of the part that holds offset `abs` (0 when it doesn't exist yet).
uint64_t part_size_at(const nvdl_job_t &r, uint32_t nparts, uint64_t abs) {
    char name[NVDL_PATH_MAX + 8], p[NVDL_PATH_MAX];
    if (!nvdl_part_name(r.dest, (uint32_t)(abs / NVDL_PART_SIZE), nparts, p, sizeof p)) return 0;
    snprintf(name, sizeof name, "%s.part", p);
    struct stat st;
    return stat(name, &st) == 0 ? (uint64_t)st.st_size : 0;
}

// ---- the worker -----------------------------------------------------------------------------------

enum Result { R_DONE, R_WAIT, R_PAUSED, R_CANCELED, R_FAILED };

struct Run {
    uint32_t     id;
    nvdl_job_t  *r;            // the worker's copy (PSRAM)
    uint8_t     *pieces = nullptr;
    uint8_t     *buf = nullptr;
    char         err[NVDL_ERR_MAX] = "";
    int64_t      t_rate = 0;
    uint64_t     b_rate = 0;
};

void publish(Run &R, uint64_t done, const char *phase, bool save_rec) {
    lock();
    if (Job *j = find(R.id)) {
        j->rec = *R.r;
        j->done = done;
        if (phase) snprintf(j->phase, sizeof j->phase, "%s", phase);
        const int64_t now = esp_timer_get_time();
        if (!R.t_rate) { R.t_rate = now; R.b_rate = done; }
        else if (now - R.t_rate >= 2000000) {
            j->bps = (uint32_t)((done - R.b_rate) * 1000000ull / (uint64_t)(now - R.t_rate));
            R.t_rate = now;
            R.b_rate = done;
        }
        bump();
    }
    unlock();
    if (save_rec) save(*R.r);
}

// Pause / cancel asked for this job (checked between blocks).
int stop_asked(uint32_t id) {
    lock();
    Job *j = find(id);
    const int s = !j ? R_CANCELED : j->want_cancel ? R_CANCELED : j->want_pause ? R_PAUSED : -1;
    unlock();
    return s;
}

Result fail(Run &R, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
Result fail(Run &R, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(R.r->error, sizeof R.r->error, fmt, ap);
    va_end(ap);
    R.r->state = NVDL_FAILED;
    NV_LOGW(TAG, "%" PRIu32 ": failed: %s", R.id, R.r->error);
    return R_FAILED;
}

// Step 1: the Metalink. Leaves the record prepared, or fails.
Result prepare(Run &R) {
    nvdl_job_t &r = *R.r;
    publish(R, 0, "meta4", false);
    Resp resp;
    int status = 0;
    int64_t len = 0;
    esp_http_client_handle_t c = http_open(r.src, 0, &resp, &status, &len, R.err, sizeof R.err);
    if (!c) { snprintf(R.r->error, sizeof R.r->error, "metalink: %.80s", R.err); return R_WAIT; }   // tried again later
    if (status != 200) { http_done(c); snprintf(R.err, sizeof R.err, "HTTP %d", status); return fail(R, "metalink: %s", R.err); }
    char *xml = (char *)heap_caps_malloc(kMeta4Max + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t n = 0;
    int k = 0;
    while (xml && n < kMeta4Max && (k = esp_http_client_read(c, xml + n, (int)(kMeta4Max - n))) > 0) n += (size_t)k;
    const bool whole = k == 0 || esp_http_client_is_complete_data_received(c);
    http_done(c);
    if (!xml) return fail(R, "out of memory");
    if (!whole || k < 0) { free(xml); return R_WAIT; }   // cut short: try again later
    nvdl_meta4_t *m = (nvdl_meta4_t *)heap_caps_malloc(sizeof(nvdl_meta4_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int rc = m ? nvdl_meta4_parse(xml, n, m, nullptr, 0) : NVDL_EPARSE;
    uint8_t *ph = nullptr;
    if (rc == NVDL_OK && m->n_pieces) {
        ph = (uint8_t *)heap_caps_malloc((size_t)m->n_pieces * 20, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        rc = ph ? nvdl_meta4_parse(xml, n, m, ph, (size_t)m->n_pieces * 20) : NVDL_ETOOBIG;
    }
    free(xml);
    if (rc != NVDL_OK) {
        free(m); free(ph);
        return fail(R, "%s", rc == NVDL_ENOURL ? "metalink: no https mirror" : "metalink: not readable");   // a 404 page too
    }
    char path[64];
    path_of(path, sizeof path, r.id, "sha1");
    bool ok = true;
    if (ph) {
        FILE *f = nv_sd_fopen(path, "wb");
        ok = f && fwrite(ph, 20, m->n_pieces, f) == m->n_pieces && fflush(f) == 0 && fsync(fileno(f)) == 0;
        nv_sd_fclose(f);
    }
    if (ok) {
        r.size = m->size;
        r.has_sha256 = m->has_sha256;
        memcpy(r.sha256, m->sha256, 32);
        r.piece_len = m->piece_len;
        r.n_pieces = m->n_pieces;
        r.n_urls = m->n_urls;
        memcpy(r.urls, m->urls, sizeof r.urls);
        r.verified = 0;
        r.prepared = true;
        r.error[0] = 0;
    }
    NV_LOGI(TAG, "%" PRIu32 ": %s, %" PRIu64 " bytes, %" PRIu32 " pieces of %" PRIu32 ", %d mirror(s)", r.id,
            m->name, m->size, m->n_pieces, m->piece_len, m->n_urls);
    free(m);
    R.pieces = ph;
    if (!ok) return fail(R, "card: could not write the piece list");
    if (nvdl_parts(r.size) == 0) return fail(R, "file too big");
    publish(R, 0, "fetch", true);
    return R_DONE;
}

bool load_pieces(Run &R) {
    if (!R.r->piece_len || R.pieces) return true;
    char path[64];
    path_of(path, sizeof path, R.r->id, "sha1");
    size_t n = 0;
    char *b = read_file(path, (size_t)R.r->n_pieces * 20, &n);
    if (!b || n != (size_t)R.r->n_pieces * 20) { free(b); return false; }
    R.pieces = (uint8_t *)b;
    return true;
}

// Step 4 for a plain URL: the whole file against the caller's SHA-256.
bool check_whole(Run &R) {
    const nvdl_job_t &r = *R.r;
    const uint32_t np = nvdl_parts(r.size);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    bool ok = true;
    uint64_t seen = 0;
    for (uint32_t i = 0; i < np && ok; i++) {
        char name[NVDL_PATH_MAX + 8], p[NVDL_PATH_MAX];
        ok = nvdl_part_name(r.dest, i, np, p, sizeof p);
        snprintf(name, sizeof name, "%s.part", p);
        FILE *f = ok ? nv_sd_fopen(name, "rb") : nullptr;
        if (!f) { ok = false; break; }
        size_t k;
        while ((k = fread(R.buf, 1, kChunk, f)) > 0) {
            mbedtls_sha256_update(&sha, R.buf, k);
            seen += k;
            if ((seen & ((16u << 20) - 1)) < k) publish(R, seen, "check", false);
        }
        nv_sd_fclose(f);
    }
    uint8_t h[32];
    mbedtls_sha256_finish(&sha, h);
    mbedtls_sha256_free(&sha);
    return ok && seen == r.size && !memcmp(h, r.sha256, 32);
}

// Step 3: the bytes. R_DONE when every byte is on the card and checked.
Result fetch(Run &R) {
    nvdl_job_t &r = *R.r;
    const uint32_t np = nvdl_parts(r.size);
    const uint64_t unit = r.piece_len ? r.piece_len : NVDL_CHECKPOINT;
    Out o;
    o.r = &r;
    o.nparts = np;
    // Resume from the record, but never past what the card really holds (a part shorter than the record
    // says - an old card, a copy - costs the pieces it lacks, not a corrupt file).
    uint64_t off = nvdl_resume_offset(&r);
    const uint64_t in = off % NVDL_PART_SIZE, have = part_size_at(r, np, off);
    if (off && in && have < in) off = off - in + have / unit * unit;
    if (off != r.verified) { r.verified = off; publish(R, off, "fetch", true); }
    int mirror = 0, net_fails = 0, same_mirror = 0, piece_fails = 0;
    uint32_t bad_piece = UINT32_MAX;
    Result res = R_DONE;
    while (r.verified < r.size) {
        const int s = stop_asked(R.id);
        if (s >= 0) { res = (Result)s; break; }
        if (!net_up()) { res = R_WAIT; break; }
        off = r.verified;
        const char *url = r.urls[mirror % r.n_urls];
        Resp resp;
        int status = 0;
        int64_t len = 0;
        char why[NVDL_ERR_MAX] = "";
        esp_http_client_handle_t c = http_open(url, off, &resp, &status, &len, why, sizeof why);
        bool ok_start = c && status == (off ? 206 : 200);
        if (ok_start && len >= 0 && (uint64_t)len != r.size - off) ok_start = false;   // not the file we expect
        if (ok_start && off && resp.range[0]) {
            const char *slash = strrchr(resp.range, '/');
            if (!slash || strtoull(slash + 1, nullptr, 10) != r.size) ok_start = false;
        }
        if (c && !ok_start) snprintf(why, sizeof why, "HTTP %d", status);
        uint64_t pos = off;                    // next byte to come
        size_t fill = 0;                       // bytes in R.buf, starting at pos - fill
        uint32_t idx = (uint32_t)(off / unit);
        uint64_t pend = (uint64_t)(idx + 1) * unit < r.size ? (uint64_t)(idx + 1) * unit : r.size;
        mbedtls_sha1_context sha;
        mbedtls_sha1_init(&sha);
        mbedtls_sha1_starts(&sha);
        bool wrong_piece = false, card_error = false;
        int k = 0;
        uint64_t shown = pos;
        while (ok_start && pos < r.size) {
            const uint64_t room = pend - pos < kChunk - fill ? pend - pos : kChunk - fill;
            k = esp_http_client_read(c, (char *)R.buf + fill, (int)room);
            if (k <= 0) break;
            mbedtls_sha1_update(&sha, R.buf + fill, (size_t)k);
            fill += (size_t)k;
            pos += (uint64_t)k;
            if (fill == kChunk || pos == pend) {
                if (!out_write(o, pos - fill, R.buf, fill)) { card_error = true; break; }
                fill = 0;
            }
            if (pos == pend) {
                uint8_t h[20];
                mbedtls_sha1_finish(&sha, h);
                if (r.piece_len && memcmp(h, R.pieces + (size_t)idx * 20, 20)) { wrong_piece = true; break; }
                if (!out_sync(o)) { card_error = true; break; }
                r.verified = pend;
                r.error[0] = 0;
                net_fails = same_mirror = 0;
                publish(R, pos, "fetch", true);
                idx++;
                pend = (uint64_t)(idx + 1) * unit < r.size ? (uint64_t)(idx + 1) * unit : r.size;
                mbedtls_sha1_starts(&sha);
                if (stop_asked(R.id) >= 0) break;
            } else if (pos - shown >= 64 * 1024) {
                shown = pos;
                publish(R, pos, nullptr, false);
            }
        }
        mbedtls_sha1_free(&sha);
        http_done(c);
        if (card_error) { out_close(o); return fail(R, "card: write failed (full?)"); }
        if (r.verified >= r.size) break;
        if (stop_asked(R.id) >= 0) continue;                       // handled at the top of the loop
        if (wrong_piece) {
            piece_fails = bad_piece == idx ? piece_fails + 1 : 1;
            bad_piece = idx;
            NV_LOGW(TAG, "%" PRIu32 ": piece %" PRIu32 " from %s does not match its hash (%d)", R.id, idx, url, piece_fails);
            if (piece_fails >= kPieceFails) { out_close(o); return fail(R, "the mirrors send damaged data"); }
            mirror++;
            same_mirror = 0;
            continue;
        }
        // the connection failed or stopped: try again, the next mirror after two failures on this one
        if (!why[0]) snprintf(why, sizeof why, k < 0 ? "connection lost" : "connection closed");
        snprintf(r.error, sizeof r.error, "%.40s: %.50s", url + 8, why);
        NV_LOGW(TAG, "%" PRIu32 ": at %" PRIu64 "/%" PRIu64 ": %s", R.id, r.verified, r.size, r.error);
        publish(R, r.verified, nullptr, false);
        if (++net_fails >= kNetFails) { out_close(o); return fail(R, "network: %s", why); }
        if (++same_mirror >= 2) { mirror++; same_mirror = 0; }
        if (!net_up()) { res = R_WAIT; break; }
        const int wait_s = net_fails < 6 ? 2 << net_fails : 60;
        for (int t = 0; t < wait_s * 10 && stop_asked(R.id) < 0; t++) vTaskDelay(pdMS_TO_TICKS(100));
    }
    out_close(o);
    if (res != R_DONE || r.verified < r.size) return res == R_DONE ? R_WAIT : res;
    if (!r.piece_len && r.has_sha256) {
        publish(R, r.size, "check", false);
        if (!check_whole(R)) {
            delete_files(r);
            r.verified = 0;
            save(r);
            return fail(R, "the file does not match its SHA-256");
        }
    }
    return R_DONE;
}

// Step 5: <name>.part -> <name>, every part.
Result finish(Run &R) {
    nvdl_job_t &r = *R.r;
    const uint32_t np = nvdl_parts(r.size);
    for (uint32_t i = 0; i < np; i++) {
        char name[NVDL_PATH_MAX + 8], p[NVDL_PATH_MAX];
        if (!nvdl_part_name(r.dest, i, np, p, sizeof p)) return fail(R, "bad name");
        snprintf(name, sizeof name, "%s.part", p);
        struct stat st;
        if (stat(p, &st) == 0) return fail(R, "%s exists already", strrchr(p, '/') ? strrchr(p, '/') + 1 : p);
        if (rename(name, p) != 0) return fail(R, "card: rename failed");
    }
    r.state = NVDL_DONE;
    r.error[0] = 0;
    NV_LOGI(TAG, "%" PRIu32 ": done, %s (%" PRIu64 " bytes)", R.id, r.dest, r.size);
    return R_DONE;
}

void run(uint32_t id) {
    Run R;
    R.id = id;
    R.r = (nvdl_job_t *)heap_caps_malloc(sizeof(nvdl_job_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    R.buf = (uint8_t *)heap_caps_aligned_alloc(64, kChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lock();
    Job *j = find(id);
    if (j && R.r && R.buf) { *R.r = j->rec; j->running = true; j->done = j->rec.verified; j->bps = 0; bump(); }
    unlock();
    if (!j || !R.r || !R.buf) { free(R.r); free(R.buf); return; }

    Result res = R_DONE;
    if (!R.r->prepared) res = prepare(R);
    if (res == R_DONE && !load_pieces(R)) res = fail(R, "card: the piece list is damaged");
    if (res == R_DONE) {
        uint64_t total = 0, free_b = 0;
        const uint64_t need = R.r->size - nvdl_resume_offset(R.r) + kReserve;
        if (nv_sd_info(&total, &free_b) && free_b < need) {
            char mb[24];
            snprintf(mb, sizeof mb, "%" PRIu64, (need - free_b + (1u << 20) - 1) >> 20);
            res = fail(R, "not enough room on the card: %s MB more needed", mb);
        }
    }
    if (res == R_DONE) res = fetch(R);
    if (res == R_DONE) res = finish(R);

    nv_dl_info_t info;
    bool tell = false;
    lock();
    j = find(id);
    if (j) {
        if (res == R_PAUSED) { R.r->state = NVDL_PAUSED; j->want_pause = false; }
        // a transient failure (a mirror down, DNS) with the network up: not straight back to it
        if (res == R_WAIT && net_up()) j->not_before_us = esp_timer_get_time() + 30 * 1000000ll;
        j->rec = *R.r;
        j->running = false;
        j->bps = 0;
        j->phase[0] = 0;
        info_of(*j, &info);
        tell = res == R_DONE || res == R_FAILED;
        if (res == R_CANCELED) j->used = false;
        bump();
    }
    unlock();
    if (res == R_CANCELED) delete_files(*R.r);
    else save(*R.r);
    if (tell && s_hook) s_hook(&info);
    free(R.pieces);
    free(R.buf);
    free(R.r);
}

// The one thing the worker does per pass. Returns what it waits for ("" = nothing).
const char *step() {
    if (!nv_sd_is_mounted()) return "sd";
    if (nv_ota_safe_mode()) return "safe";
    if (!nv_ota_confirmed() || nv_ota_busy()) return "ota";
    ensure_loaded();
    lock();
    uint32_t pick = 0, best = UINT32_MAX;
    bool later = false;
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < NV_DL_MAX; i++) {
        const Job &j = s_jobs[i];
        if (!j.used || j.rec.state != NVDL_QUEUED || j.want_pause) continue;
        if (j.not_before_us > now) { later = true; continue; }
        if (j.rec.id < best) { best = j.rec.id; pick = j.rec.id; }
    }
    unlock();
    if (!pick) return later ? "retry" : "";
    if (!net_up()) return "net";
    run(pick);
    return "";
}

void task(void *) {
    vTaskDelay(pdMS_TO_TICKS(15 * 1000));          // after boot's Wi-Fi join and the auto-OTA check
    for (;;) {
        const char *w = step();
        lock();
        if (strcmp(w, s_waiting)) { s_waiting = w; bump(); }
        unlock();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(w[0] ? 5000 : 2000));
    }
}

bool owner_ok(const char *s) {
    if (!s || !*s || strlen(s) >= NVDL_OWNER_MAX) return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.')) return false;
    return true;
}

// Where a download may write: the card, outside the system's own folders and the secrets.
bool dest_ok(const char *d) {
    if (!d || strncmp(d, "/sdcard/", 8) || strlen(d) >= NVDL_PATH_MAX - 8) return false;
    if (strstr(d, "/../") || strstr(d, "/./") || strstr(d, "//") || strchr(d, '\\')) return false;
    const size_t dl = strlen(d);
    if (d[dl - 1] == '/' || d[dl - 1] == '.') return false;
    if (!strncasecmp(d, kDir, sizeof kDir - 1)) return false;
    if (nv_sealed_within(d)) return false;
    char p[NVDL_PATH_MAX + 8];
    snprintf(p, sizeof p, "%s.part", d);
    if (nv_sealed_path(p)) return false;
    snprintf(p, sizeof p, "%saa", d);
    return !nv_sealed_path(p);
}

}  // namespace

// ---- public API -----------------------------------------------------------------------------------

void nv_dl_start(void) {
    if (!init() || s_task) return;
    const esp_app_desc_t *a = esp_app_get_description();
    snprintf(s_ua, sizeof s_ua, "NucleoOS/%s (+https://indecenti.github.io/nucleoos-p4-store)", a ? a->version : "?");
    if (xTaskCreateWithCaps(task, "nv_dl", 12 * 1024, nullptr, 3, &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = nullptr;
        NV_LOGE(TAG, "could not start the worker");
    }
}

int nv_dl_add(const nv_dl_req_t *q, char *err, size_t en) {
    char dummy[4];
    if (!err) { err = dummy; en = sizeof dummy; }
    err[0] = 0;
    if (!init()) { snprintf(err, en, "out of memory"); return NV_DL_EARG; }
    if (!q || !owner_ok(q->owner)) { snprintf(err, en, "bad owner"); return NV_DL_EARG; }
    if (!nv_sd_is_mounted()) { snprintf(err, en, "no card"); return NV_DL_ESD; }
    if (!dest_ok(q->dest)) { snprintf(err, en, "this path is not allowed"); return NV_DL_EDENIED; }
    const char *src = q->meta4 ? q->meta4 : q->url;
    np_url_t u;
    if (!src || strlen(src) >= NVDL_URL_MAX || !np_url_parse(src, &u) || u.scheme != NP_HTTPS) {
        snprintf(err, en, "not an https URL");
        return NV_DL_EARG;
    }
    uint8_t sha[32];
    if (!q->meta4 && (!q->size || q->size > NVDL_SIZE_MAX || !nvdl_hex_decode(q->sha256, sha, 32))) {
        snprintf(err, en, "a plain URL needs its size and SHA-256");
        return NV_DL_EARG;
    }
    ensure_loaded();
    nvdl_job_t *r = (nvdl_job_t *)heap_caps_calloc(1, sizeof(nvdl_job_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r) { snprintf(err, en, "out of memory"); return NV_DL_EARG; }
    snprintf(r->owner, sizeof r->owner, "%s", q->owner);
    snprintf(r->title, sizeof r->title, "%s", q->title && q->title[0] ? q->title : strrchr(q->dest, '/') + 1);
    snprintf(r->dest, sizeof r->dest, "%s", q->dest);
    snprintf(r->src, sizeof r->src, "%s", src);
    r->src_meta4 = q->meta4 != nullptr;
    r->created = (uint32_t)time(nullptr);
    r->state = NVDL_QUEUED;
    if (!q->meta4) {
        r->prepared = true;
        r->size = q->size;
        r->has_sha256 = true;
        memcpy(r->sha256, sha, 32);
        r->n_urls = 1;
        snprintf(r->urls[0], NVDL_URL_MAX, "%s", q->url);
    }
    // the finished file(s) must not exist: a download never replaces what is on the card
    struct stat st;
    char p[NVDL_PATH_MAX + 8];
    const uint32_t np = r->prepared ? nvdl_parts(r->size) : 1;
    bool exists = stat(q->dest, &st) == 0;
    for (uint32_t i = 0; np > 1 && i < np && !exists; i++)
        exists = nvdl_part_name(q->dest, i, np, p, sizeof p) && stat(p, &st) == 0;
    snprintf(p, sizeof p, "%saa", q->dest);
    if (!exists) exists = stat(p, &st) == 0;
    int id = NV_DL_EEXISTS;
    lock();
    int slot = -1;
    for (int i = 0; i < NV_DL_MAX && !exists; i++) {
        if (!s_jobs[i].used) { if (slot < 0) slot = i; continue; }
        if (!strcasecmp(s_jobs[i].rec.dest, q->dest) && s_jobs[i].rec.state != NVDL_DONE) exists = true;
    }
    if (exists) snprintf(err, en, "the file exists, or is being downloaded");
    else if (slot < 0) { snprintf(err, en, "too many downloads listed"); id = NV_DL_EFULL; }
    else {
        r->id = s_next_id++;
        memset(&s_jobs[slot], 0, sizeof(Job));
        s_jobs[slot].used = true;
        s_jobs[slot].rec = *r;
        id = (int)r->id;
        bump();
    }
    unlock();
    if (id > 0) {
        if (!save(*r)) {
            lock();
            if (Job *j = find((uint32_t)id)) j->used = false;
            unlock();
            snprintf(err, en, "card: could not write");
            id = NV_DL_ESD;
        } else {
            NV_LOGI(TAG, "%d: queued for %s: %s -> %s", id, r->owner, r->src, r->dest);
            kick();
        }
    }
    free(r);
    return id;
}

bool nv_dl_get(uint32_t id, nv_dl_info_t *out) {
    if (!out || !init()) return false;
    ensure_loaded();
    lock();
    Job *j = find(id);
    if (j) info_of(*j, out);
    unlock();
    return j != nullptr;
}

int nv_dl_list(const char *owner, nv_dl_info_t *out, int max) {
    if (!out || max <= 0 || !init()) return 0;
    ensure_loaded();
    lock();
    int idx[NV_DL_MAX], n = 0;
    for (int i = 0; i < NV_DL_MAX; i++)
        if (s_jobs[i].used && (!owner || !strcmp(owner, s_jobs[i].rec.owner))) idx[n++] = i;
    for (int a = 1; a < n; a++)                         // oldest first
        for (int b = a; b > 0 && s_jobs[idx[b]].rec.id < s_jobs[idx[b - 1]].rec.id; b--) { const int t = idx[b]; idx[b] = idx[b - 1]; idx[b - 1] = t; }
    const int k = n < max ? n : max;
    for (int i = 0; i < k; i++) info_of(s_jobs[idx[i]], &out[i]);
    unlock();
    return k;
}

bool nv_dl_pause(uint32_t id, bool pause) {
    if (!init()) return false;
    nvdl_job_t *save_it = nullptr;
    lock();
    Job *j = find(id);
    bool ok = j != nullptr;
    if (j && pause) {
        if (j->running) j->want_pause = true;
        else if (j->rec.state == NVDL_QUEUED) { j->rec.state = NVDL_PAUSED; save_it = &j->rec; }
        else ok = false;
    } else if (j) {
        j->want_pause = false;
        j->not_before_us = 0;
        if (!j->running && (j->rec.state == NVDL_PAUSED || j->rec.state == NVDL_FAILED)) {
            j->rec.state = NVDL_QUEUED;
            j->rec.error[0] = 0;
            save_it = &j->rec;
        } else if (!j->running) {
            ok = j->rec.state == NVDL_QUEUED;
        }
    }
    nvdl_job_t copy;
    if (save_it) copy = *save_it;
    bump();
    unlock();
    if (save_it) save(copy);
    kick();
    return ok;
}

bool nv_dl_cancel(uint32_t id) {
    if (!init()) return false;
    nvdl_job_t copy;
    bool now = false;
    lock();
    Job *j = find(id);
    const bool ok = j != nullptr;
    if (j && j->running) j->want_cancel = true;
    else if (j) { copy = j->rec; j->used = false; now = true; }
    bump();
    unlock();
    if (now) delete_files(copy);
    kick();
    return ok;
}

int64_t nv_dl_room(void) {
    static int64_t at_us = 0;
    static uint64_t free_b = 0;
    static uint32_t sd_gen = 0;
    if (!init() || !nv_sd_is_mounted()) return -1;
    const int64_t now = esp_timer_get_time();
    uint64_t total = 0, f = 0;
    lock();
    const bool stale = !at_us || now - at_us > 30 * 1000000ll || sd_gen != nv_sd_generation();
    unlock();
    if (stale) {                                       // the card is asked outside the lock: it can be slow
        if (!nv_sd_info(&total, &f)) return -1;
        lock();
        free_b = f;
        at_us = now;
        sd_gen = nv_sd_generation();
        unlock();
    }
    ensure_loaded();
    uint64_t owed = 0;
    lock();
    for (int i = 0; i < NV_DL_MAX; i++) {
        const Job &j = s_jobs[i];
        if (!j.used || !j.rec.prepared || j.rec.state == NVDL_DONE || j.rec.state == NVDL_FAILED) continue;
        const uint64_t have = j.running && j.done > j.rec.verified ? j.done : j.rec.verified;
        owed += j.rec.size > have ? j.rec.size - have : 0;
    }
    const uint64_t fb = free_b;
    unlock();
    const uint64_t taken = owed + kReserve;
    return fb > taken ? (int64_t)(fb - taken) : 0;
}

uint32_t nv_dl_generation(void) { if (!init()) return 0; lock(); const uint32_t g = s_gen; unlock(); return g; }

const char *nv_dl_waiting(void) { if (!init()) return "sd"; lock(); const char *w = s_waiting; unlock(); return w; }

void nv_dl_set_hook(void (*hook)(const nv_dl_info_t *info)) { s_hook = hook; }
