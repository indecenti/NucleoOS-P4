// nv_appstore — remote WASM app catalog + installer. See nv_appstore.h.
#include "nv_appstore.h"
#include "nv_log.h"
#include "nv_seclog.h"
#include "nv_config.h"
#include "nv_telemetry.h" // installs count only with the owner's opt-in (one consent)
#include "nv_sd.h"
#include "nv_wasm.h"      // nv_wasm_load_manifest — derive installed/update against the local card
#include "nv_i18n.h"      // active locale -> ?lang= so the store returns localized copy
#include "nv_mem_attr.h"  // NV_PSRAM_BSS: the icon cache table

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"   // https:// stores validate against the bundled root CAs
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <ctype.h>
#include "miniz.h"            // ROM tinfl: store icons are raw-deflate compressed
#include "mbedtls/pk.h"       // package.sig: ECDSA P-256 (store key)
#include "mbedtls/sha256.h"
#include "nv_store_pkg.h"
#include "nv_store_tree.h"  // system content packs: tree index, ustar, swap recovery

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <ctime>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <new>                // placement new: the extractor state lives in PSRAM

static const char *TAG = "store";

namespace {

// The GitHub Pages distribution repo (tools/dist.py publishes it), same place as the default OTA
// manifest. A local appstore_server.py is a URL typed in Settings (persists to NVS).
constexpr char     kDefaultUrl[]  = "https://indecenti.github.io/nucleoos-p4-store";
constexpr char     kStatsUrl[]    = "https://nucleoos.indexhub.it/stats";   // install counter
constexpr char     kAppsDir[]     = "/sdcard/apps";
constexpr long     kMaxWasm       = 6 * 1024 * 1024;   // 6 MB module cap (SD write + PSRAM run)
constexpr long     kMaxAot        = 20 * 1024 * 1024;  // precompiled image: native code is bigger
constexpr long     kMaxIcon       = 80 * 80 * 4;       // exactly one 80x80 ARGB8888 tile
constexpr int      kMaxIconZ      = 32 * 1024;         // compressed icon ceiling (a real one is ~1-2 KB)
constexpr int      kCatalogCap    = 512 * 1024;        // store.json ceiling (NV_STORE_MAX apps, PSRAM)
constexpr int      kFilesCap      = 16 * 1024;         // files.json ceiling
constexpr int      kMaxFiles      = 256;               // assets per package (chess ships 142)
constexpr long     kMaxAsset      = 4 * 1024 * 1024;   // one texture / sound / model
constexpr long     kMaxAssets     = 24 * 1024 * 1024;  // all of a package's assets
constexpr int      kMaxPlan       = 8;                 // packages one install may pull in
constexpr int      kMaxDepDepth   = 3;                 // requires of requires of requires
constexpr uint32_t kWasmMagic     = 0x6d736100;        // "\0asm" little-endian
constexpr uint32_t kAotMagic      = 0x746f6100;        // "\0aot"
constexpr uint32_t kLpkMagic      = 0x314b504c;        // "LPK1" little-endian (Lua app bundle)
constexpr long     kMaxLpk        = 1024 * 1024;       // the luaapp engine's bundle cap (MAX_BUNDLE)

SemaphoreHandle_t   s_lock = nullptr;
nv_store_state_t    s_state = NV_STORE_IDLE;
int                 s_progress = 0;
char                s_msg[96]  = "";
char                s_installing[32] = "";
char                s_job_variant[9] = "";       // edition to write after the install ("" = none)
bool                s_pinging = false;           // the worker is sending the install-counter ping

nv_store_entry_t   *s_cat   = nullptr;    // PSRAM catalog snapshot
NV_PSRAM_BSS nv_store_category_t s_cats[NV_STORE_CATS_MAX];   // its categories (under the lock)
int                 s_cats_n = 0;
int                 s_cat_n = 0;
uint32_t            s_cat_gen = 0;        // completed catalog fetches (nv_appstore_catalog_gen)

// store2 platforms (nv_store_platform_t) and which part of whose carts follows the native rows in
// s_cat. The name index of each platform ("names": one per line) is a PSRAM copy, for search.
struct PlatState {
    nv_store_platform_t p[NV_STORE_PLATS_MAX];
    char *names[NV_STORE_PLATS_MAX];
    int   n;
    int   main_n;          // native rows at the head of s_cat (== s_cat_n when no part is loaded)
    char  loaded[16];      // platform whose part follows them ("" = none)
    int   part;
    int   job_part;        // JOB_PLATFORM: the part to fetch (platform id in s_job_id)
};
NV_PSRAM_BSS PlatState s_pl;

// pending job, filled by the caller before the worker task starts
enum JobKind { JOB_FETCH, JOB_INSTALL, JOB_PLATFORM, JOB_CONTENT, JOB_CONTENT_INDEX, JOB_CONTENT_VERIFY };
JobKind s_job_kind = JOB_FETCH;
char    s_job_id[32]  = "";
char    s_job_base[192] = "";   // store base URL, captured on the caller thread

void lock()   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
void unlock() { if (s_lock) xSemaphoreGive(s_lock); }

void set_state(nv_store_state_t st, const char *msg) {
    lock();
    s_state = st;
    if (msg) snprintf(s_msg, sizeof s_msg, "%s", msg);
    unlock();
}
void set_progress(int p) { lock(); s_progress = p < 0 ? 0 : (p > 100 ? 100 : p); unlock(); }

// Active UI language as a 2-letter store code (matches the server's LANGS).
const char *lang_code(void) {
    switch (nv_i18n_get_lang()) {
        case NV_LANG_IT: return "it";
        case NV_LANG_ES: return "es";
        case NV_LANG_FR: return "fr";
        case NV_LANG_DE: return "de";
        default:         return "en";
    }
}

bool version_is_newer(const char *cand, const char *cur) {
    // Up to four fields: ported programs keep the upstream version and add a build number
    // (sqlite3 "3.53.4.1" must update "3.53.4").
    int a[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0};
    sscanf(cand, "%d.%d.%d.%d", &a[0], &a[1], &a[2], &a[3]);
    sscanf(cur,  "%d.%d.%d.%d", &b[0], &b[1], &b[2], &b[3]);
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

// ---- HTTP helpers -------------------------------------------------------------------------------

struct RespBuf { char *buf; int len; int cap; bool overflow; };
esp_err_t collect_evt(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data) {
        RespBuf *r = (RespBuf *)e->user_data;
        int n = e->data_len;
        // Truncate + flag, never silently DROP a chunk (a dropped middle chunk = "Bad catalog").
        const int room = r->cap - 1 - r->len;
        if (n > room) { n = room; r->overflow = true; }
        if (n > 0) { memcpy(r->buf + r->len, e->data, n); r->len += n; }
    }
    return ESP_OK;
}

// GET url into a caller buffer (NUL-terminated). Returns bytes on HTTP 200 + non-empty, else -1.
// `status` (optional) gets the HTTP status, 0 when the request never got an answer.
int http_get_buf(const char *url, char *out, int cap, int *status = nullptr) {
    RespBuf rb = { out, 0, cap, false };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = collect_evt;
    cfg.user_data = &rb;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    // 30 s: a TLS handshake over a lossy link (range extender, 30 % loss) alone takes ~15 s.
    cfg.timeout_ms = 30000;
    if (status) *status = 0;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_err_t err = esp_http_client_perform(c);
    const int st = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (status && err == ESP_OK) *status = st;
    if (err != ESP_OK || st != 200 || rb.len == 0) return -1;
    if (rb.overflow) { NV_LOGE(TAG, "response larger than %d bytes — refused (%s)", cap, url); return -1; }
    out[rb.len] = '\0';
    return rb.len;
}

// Open a GET and read the response headers, following up to 5 redirects: esp_http_client_perform()
// follows them by itself, open() doesn't (http:// -> https://, a GitHub release asset -> its CDN).
// Returns false when a connection fails; the final status is then esp_http_client_get_status_code().
bool open_following_redirects(esp_http_client_handle_t c, int *total) {
    for (int hop = 0;; ++hop) {
        if (esp_http_client_open(c, 0) != ESP_OK) return false;
        *total = (int)esp_http_client_fetch_headers(c);
        const int st = esp_http_client_get_status_code(c);
        const bool redirect = st == 301 || st == 302 || st == 303 || st == 307 || st == 308;
        if (!redirect || hop >= 5 || esp_http_client_set_redirection(c) != ESP_OK) return true;
        esp_http_client_close(c);
    }
}

// ---- signed packages ----------------------------------------------------------------------------
// apps/<id>/package.sig (nv_store_pkg.h): the store key signs the list of every file of the package
// with its sha256 and size. The installer downloads each file to <path>.tmp, hashes it on the way
// and only when EVERY file of the package matched renames them into place (manifest last). A
// package without package.sig is refused unless nv_config "store_unsigned" (developer switch).
extern const char store_pub_start[] asm("_binary_store_signing_pub_pem_start");
extern const char store_pub_end[]   asm("_binary_store_signing_pub_pem_end");
// The offline backup key (tools/store_sign.py keygen-backup): a signature from either key is accepted,
// so losing the main key never strands the devices.
extern const char store_backup_pub_start[] asm("_binary_store_signing_backup_pub_pem_start");
extern const char store_backup_pub_end[]   asm("_binary_store_signing_backup_pub_pem_end");
constexpr int kPkgCap    = (int)nv_store_pkg::kTextMax;
constexpr int kCommitMax = nv_store_pkg::kMaxFiles + 8;

// Files downloaded (as <path>.tmp) and waiting for the all-or-nothing commit. Worker task only.
struct Staged { int n; char path[kCommitMax][224]; };
Staged *s_staged = nullptr;

bool stage_add(const char *path) {
    if (!s_staged || s_staged->n >= kCommitMax) return false;
    snprintf(s_staged->path[s_staged->n++], sizeof s_staged->path[0], "%s", path);
    return true;
}
void stage_abort(void) {
    if (!s_staged) return;
    char tmp[240];
    for (int i = 0; i < s_staged->n; i++) {
        snprintf(tmp, sizeof tmp, "%s.tmp", s_staged->path[i]);
        unlink(tmp);
    }
    s_staged->n = 0;
}
// Rename every staged file over its target; `last` (the manifest) goes after all the others so the
// scanner never sees a new manifest next to old files.
bool stage_commit(const char *last) {
    if (!s_staged) return false;
    char tmp[240];
    bool ok = true;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < s_staged->n; i++) {
            const bool is_last = last && !strcmp(s_staged->path[i], last);
            if (is_last != (pass == 1)) continue;
            snprintf(tmp, sizeof tmp, "%s.tmp", s_staged->path[i]);
            if (rename(tmp, s_staged->path[i]) != 0) {
                unlink(s_staged->path[i]);                 // FAT rename won't overwrite
                if (rename(tmp, s_staged->path[i]) != 0) {
                    NV_LOGE(TAG, "commit: rename -> %s errno=%d", s_staged->path[i], errno);
                    unlink(tmp);
                    ok = false;
                }
            }
        }
    s_staged->n = 0;
    return ok;
}

bool sig_verify_with(const char *pem, size_t pem_len, const uint8_t h[32], const uint8_t *sig, int sig_len) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = mbedtls_pk_parse_public_key(&pk, reinterpret_cast<const unsigned char *>(pem), pem_len);
    if (rc == 0) rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, h, 32, sig, (size_t)sig_len);
    mbedtls_pk_free(&pk);
    return rc == 0;
}

bool sig_verify(const uint8_t *msg, size_t len, const uint8_t *sig, int sig_len) {
    uint8_t h[32];
    if (mbedtls_sha256(msg, len, h, 0) != 0) return false;
    return sig_verify_with(store_pub_start, (size_t)(store_pub_end - store_pub_start), h, sig, sig_len) ||
           sig_verify_with(store_backup_pub_start, (size_t)(store_backup_pub_end - store_backup_pub_start), h, sig, sig_len);
}

enum PkgResult { PKG_OK, PKG_MISSING, PKG_BAD };

// Fetch + parse + verify apps/<id>/package.sig into `out`. PKG_MISSING only for a clean 404.
PkgResult fetch_package(const char *base, const nv_store_entry_t *e, nv_store_pkg::Package *out) {
    char url[320];
    snprintf(url, sizeof url, "%s/apps/%s/package.sig", base, e->id);
    char *body = (char *)heap_caps_malloc(kPkgCap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) return PKG_BAD;
    int status = 0;
    const int got = http_get_buf(url, body, kPkgCap + 1, &status);
    PkgResult r = PKG_BAD;
    if (got < 0) {
        r = status == 404 ? PKG_MISSING : PKG_BAD;
        if (r == PKG_BAD) NV_LOGE(TAG, "install: package.sig for '%s' unreachable (HTTP %d)", e->id, status);
    } else if (!nv_store_pkg::parse(body, (size_t)got, out)) {
        NV_LOGE(TAG, "install: malformed package.sig for '%s'", e->id);
        nv_seclog_add(NV_SEC_APP_REFUSED, e->id);
    } else if (strcmp(out->id, e->id) != 0 || strcmp(out->version, e->version) != 0) {
        NV_LOGE(TAG, "install: package.sig is %s v%s, catalog says %s v%s", out->id, out->version,
                e->id, e->version);
        nv_seclog_add(NV_SEC_APP_REFUSED, e->id);
    } else if (!sig_verify((const uint8_t *)body, out->signed_len, out->sig, out->sig_len)) {
        NV_LOGE(TAG, "install: package.sig signature of '%s' does not verify", e->id);
        nv_seclog_add(NV_SEC_APP_REFUSED, e->id);
    } else {
        r = PKG_OK;
    }
    free(body);
    return r;
}

// The signed entry a download must match (nullptr = unsigned install, developer mode). Worker only.
const nv_store_pkg::Package *s_pkg = nullptr;

// Stream url to <path>.tmp. Without a package (developer mode, unsigned) it is renamed over `path`
// at once, as before. With a package the file must be listed under `rel` and hash to its
// sha256/size; it then stays staged until stage_commit(). When `magic`!=0 the first 4 bytes must
// match it (rejects an HTML error page served as app.wasm). `track` drives the progress bar.
bool http_get_file_raw(const char *url, const char *path, long max_bytes, uint32_t magic, bool track,
                       const nv_store_pkg::File *expect);

bool http_get_file(const char *url, const char *path, long max_bytes, uint32_t magic, bool track,
                   const char *rel = nullptr) {
    if (!s_pkg) return http_get_file_raw(url, path, max_bytes, magic, track, nullptr);
    const nv_store_pkg::File *f = rel ? nv_store_pkg::find(*s_pkg, rel) : nullptr;
    if (!f) { NV_LOGE(TAG, "dl: %s is not in the signed package", rel ? rel : url); return false; }
    if (!stage_add(path)) { NV_LOGE(TAG, "dl: too many files"); return false; }
    if (http_get_file_raw(url, path, max_bytes, magic, track, f)) return true;
    s_staged->n--;                                         // nothing staged under that name
    return false;
}

bool http_get_file_raw(const char *url, const char *path, long max_bytes, uint32_t magic, bool track,
                       const nv_store_pkg::File *expect) {
    char tmp[240];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (expect) {
        if ((long)expect->size > max_bytes) { NV_LOGE(TAG, "dl: signed size over cap"); return false; }
        max_bytes = (long)expect->size;                    // not one byte more than was signed
    }

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { NV_LOGE(TAG, "dl: init failed"); return false; }
    int total = 0;                                         // <=0 when chunked / unknown
    if (!open_following_redirects(c, &total)) {
        NV_LOGE(TAG, "dl: open %s failed", url); esp_http_client_cleanup(c); return false;
    }
    if (total > 0 && (long)total > max_bytes) {
        NV_LOGE(TAG, "dl: %d bytes over cap %ld", total, max_bytes);
        esp_http_client_close(c); esp_http_client_cleanup(c); return false;
    }

    FILE *f = nv_sd_fopen(tmp, "wb");
    if (!f) {
        NV_LOGE(TAG, "dl: fopen('%s') errno=%d", tmp, errno);
        esp_http_client_close(c); esp_http_client_cleanup(c); return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (expect) mbedtls_sha256_starts(&sha, 0);
    char buf[2048]; int r; long done = 0; bool ok = true, first = true;
    while ((r = esp_http_client_read(c, buf, sizeof buf)) > 0) {
        if (first && magic) {
            uint32_t head = 0;
            if (r >= 4) memcpy(&head, buf, 4);
            if (r < 4 || head != magic) {
                NV_LOGE(TAG, "dl: bad magic (%s)", path); ok = false; break;
            }
            first = false;
        }
        if (done + r > max_bytes) { NV_LOGE(TAG, "dl: exceeded cap mid-stream"); ok = false; break; }
        if (expect) mbedtls_sha256_update(&sha, reinterpret_cast<const unsigned char *>(buf), (size_t)r);
        if ((int)fwrite(buf, 1, (size_t)r, f) != r) {
            NV_LOGE(TAG, "dl: fwrite at %ld errno=%d (SD full?)", done, errno); ok = false; break;
        }
        done += r;
        if (track && total > 0) set_progress((int)((int64_t)done * 100 / total));
    }
    if (r < 0) { NV_LOGE(TAG, "dl: read error at %ld", done); ok = false; }
    const int status = esp_http_client_get_status_code(c);
    nv_sd_fclose(f);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (expect && ok) {
        uint8_t h[32];
        mbedtls_sha256_finish(&sha, h);
        if (done != (long)expect->size || memcmp(h, expect->sha256, sizeof h) != 0) {
            NV_LOGE(TAG, "dl: %s does not match the signed package (size %ld/%lu)", expect->path, done,
                    (unsigned long)expect->size);
            nv_seclog_add(NV_SEC_APP_REFUSED, expect->path);
            ok = false;
        }
    }
    mbedtls_sha256_free(&sha);

    if (!ok || status != 200 || done <= 0) {
        unlink(tmp);
        NV_LOGE(TAG, "dl: failed url=%s status=%d done=%ld", url, status, done);
        return false;
    }
    if (expect) return true;                               // staged: stage_commit() renames it
    if (rename(tmp, path) != 0) {
        // FAT rename won't overwrite an existing target — replace explicitly.
        unlink(path);
        if (rename(tmp, path) != 0) { NV_LOGE(TAG, "dl: rename -> %s errno=%d", path, errno); unlink(tmp); return false; }
    }
    return true;
}

// ---- data packs ("kind":"data") -------------------------------------------------------------------
// Big files (tens of MB to GB) from any https host, listed with their sha256 in a pack.sig signed by the
// store key. Each file downloads into <dest>/<name>.part, part by part with HTTP Range so a dropped
// connection (or a reboot) resumes where it stopped; the bytes already on the card are re-hashed first,
// so a resumed file is checked exactly like a fresh one. Only a fully verified file is renamed into place.
constexpr char kDataDir[]  = "/sdcard/data";
constexpr char kPacksDir[] = "/sdcard/data/packs";          // <id>.pack: version, dest, then one name per line
void (*s_data_hook)(const char *dest) = nullptr;

// SD I/O buffers for the big files (data / content packs). The SDMMC DMA moves a buffer directly only
// when its address and length are cache-line aligned and the file offset is sector aligned; otherwise
// every sector is bounced (memory sd-dma-alignment) - an 83 MB tree took 7.5 min to extract that way.
// So: 64-byte aligned PSRAM buffers of kSdBuf, and a FILE stream buffer of the same size for writes.
constexpr size_t kSdBuf = 32 * 1024;
void *sd_buf(void) {
    void *b = heap_caps_aligned_alloc(64, kSdBuf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return b ? b : heap_caps_malloc(kSdBuf, MALLOC_CAP_8BIT);
}

// Copy at most n-1 bytes and terminate: a record line longer than its field is cut, never overflowed.
void copy_bounded(char *d, size_t n, const char *s) {
    if (!n) return;
    const size_t l = strnlen(s, n - 1);
    memcpy(d, s, l);
    d[l] = 0;
}

// The record of an installed pack. version/dest may be nullptr. names: up to `max` file names.
int data_record_read(const char *id, char *version, size_t vn, char *dest, size_t dn,
                     char (*names)[nv_store_pkg::kDataNameMax], int max) {
    char p[96];
    snprintf(p, sizeof p, "%s/%s.pack", kPacksDir, id);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    char line[96];
    int n = 0, row = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (row == 0 && version) copy_bounded(version, vn, line);
        else if (row == 1 && dest) copy_bounded(dest, dn, line);
        else if (row >= 2 && names && n < max && line[0]) copy_bounded(names[n++], nv_store_pkg::kDataNameMax, line);
        row++;
    }
    fclose(f);
    return row >= 2 ? n : -1;
}

void mkdirs(const char *path) {                              // "a/b/c": every level, errors ignored
    char p[128];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0777); *s = '/'; }
    mkdir(p, 0777);
}

// GET `url` from byte `from` (Range) and append to `f` (already positioned), hashing as it goes, at most
// `want` bytes. Returns bytes written (== want on success), -1 on a network/SD error. *restart = the
// server ignored Range (200 to a ranged request): the caller rewinds and starts the part again.
long http_get_range(const char *url, uint64_t from, uint64_t want, FILE *f, mbedtls_sha256_context *sha,
                    uint64_t *done_all, uint64_t total_all, bool *restart) {
    *restart = false;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 30000;
    cfg.buffer_size = 4096;
    // A GitHub release asset redirects to objects.githubusercontent.com with a ~930-character signed
    // URL: the request line of that hop overflowed the 512-byte default and every pack download
    // failed at byte 0 ("interrupted").
    cfg.buffer_size_tx = 2048;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    char range[48];
    if (from) {
        snprintf(range, sizeof range, "bytes=%llu-", (unsigned long long)from);
        esp_http_client_set_header(c, "Range", range);
    }
    int total = 0;
    if (!open_following_redirects(c, &total)) { esp_http_client_cleanup(c); return -1; }
    const int st = esp_http_client_get_status_code(c);
    if (from && st == 200) { *restart = true; esp_http_client_close(c); esp_http_client_cleanup(c); return -1; }
    if (st != (from ? 206 : 200)) {
        NV_LOGE(TAG, "data: HTTP %d for %s", st, url);
        esp_http_client_close(c); esp_http_client_cleanup(c); return -1;
    }
    char *buf = (char *)sd_buf();
    long got = 0;
    int r = 0;
    while (buf && (uint64_t)got < want && (r = esp_http_client_read(c, buf, (int)kSdBuf)) > 0) {
        if ((uint64_t)(got + r) > want) r = (int)(want - (uint64_t)got);   // never more than signed
        mbedtls_sha256_update(sha, reinterpret_cast<const unsigned char *>(buf), (size_t)r);
        if ((int)fwrite(buf, 1, (size_t)r, f) != r) { NV_LOGE(TAG, "data: SD write failed (full?)"); got = -1; break; }
        // FatFs writes a file's size into its directory entry only on sync/close: without this a reboot
        // or a power cut leaves an empty .part and the download starts over. At most 1 MB is lost now.
        if ((got >> 20) != ((got + r) >> 20)) { fflush(f); fsync(fileno(f)); }
        got += r;
        *done_all += (uint64_t)r;
        if (total_all) set_progress((int)(*done_all * 100 / total_all));
    }
    if (r < 0) got = -1;
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return buf ? got : -1;
}

// Re-hash bytes [off, off+n) of an open file (a part already on the card before a resume).
bool rehash(FILE *f, uint64_t off, uint64_t n, mbedtls_sha256_context *sha) {
    char *buf = (char *)sd_buf();
    if (!buf || fseek(f, (long)off, SEEK_SET) != 0) { free(buf); return false; }
    while (n) {
        const size_t k = n > kSdBuf ? kSdBuf : (size_t)n;
        if (fread(buf, 1, k, f) != k) { free(buf); return false; }
        mbedtls_sha256_update(sha, reinterpret_cast<const unsigned char *>(buf), k);
        n -= k;
    }
    free(buf);
    return true;
}

// One file = parts [first, last] of the pack, written to `path`. Resumes from what <path>.part already
// holds. A part whose bytes don't match the signed hash is never retried in this call, and its .part is
// deleted: the next attempt downloads it fresh instead of re-hashing the same wrong bytes forever.
bool data_fetch_to(const nv_store_pkg::DataPack &pk, int first, int last, const char *path,
                   uint64_t *done_all, uint64_t total_all) {
    char part[208];
    snprintf(part, sizeof part, "%s.part", path);
    uint64_t want = 0;
    for (int i = first; i <= last; i++) want += pk.parts[i].size;
    struct stat stt;
    uint64_t have = stat(part, &stt) == 0 ? (uint64_t)stt.st_size : 0;
    if (have > want) { unlink(part); have = 0; }                   // not ours: start over
    FILE *f = nv_sd_fopen(part, have ? "r+b" : "wb");
    if (!f) { NV_LOGE(TAG, "data: open %s errno=%d", part, errno); return false; }
    char *wbuf = (char *)sd_buf();                                 // whole aligned 32 KB writes
    if (wbuf) setvbuf(f, wbuf, _IOFBF, kSdBuf);
    bool ok = true, corrupt = false;
    uint64_t off = 0;
    for (int i = first; i <= last && ok; i++) {
        const nv_store_pkg::DataPart &q = pk.parts[i];
        uint64_t present = have > off ? (have - off > q.size ? q.size : have - off) : 0;
        for (int attempt = 0; attempt < 6 && ok; attempt++) {
            mbedtls_sha256_context sha;
            mbedtls_sha256_init(&sha);
            mbedtls_sha256_starts(&sha, 0);
            if (present && !rehash(f, off, present, &sha)) present = 0, mbedtls_sha256_starts(&sha, 0);
            if (present && attempt == 0 && present < q.size)
                NV_LOGI(TAG, "data: %s resumes at %llu/%llu bytes", q.name, (unsigned long long)present,
                        (unsigned long long)q.size);
            *done_all += present;
            bool restart = false;
            long got = 0;
            if (present < q.size) {
                fseek(f, (long)(off + present), SEEK_SET);
                // every other attempt goes to the mirror, when the pack lists one (v2)
                const char *u = (q.url2[0] && (attempt & 1)) ? q.url2 : q.url;
                got = http_get_range(u, present, q.size - present, f, &sha, done_all, total_all, &restart);
            }
            uint8_t h[32];
            mbedtls_sha256_finish(&sha, h);
            mbedtls_sha256_free(&sha);
            if (got >= 0 && present + (uint64_t)got == q.size) {
                if (memcmp(h, q.sha256, 32) == 0) break;           // this part is verified
                NV_LOGE(TAG, "data: %s part %d does not match the signed hash", q.name, i - first + 1);
                nv_seclog_add(NV_SEC_APP_REFUSED, q.name);
                ok = false; corrupt = true; break;                 // a different file: never retry it here
            }
            *done_all -= present + (uint64_t)(got > 0 ? got : 0);
            if (restart) { present = 0; continue; }                // the host ignored Range: whole part again
            fflush(f);
            struct stat s2;
            present = (fstat(fileno(f), &s2) == 0 && (uint64_t)s2.st_size > off) ? (uint64_t)s2.st_size - off : 0;
            if (present > q.size) present = q.size;
            NV_LOGW(TAG, "data: %s interrupted at %llu/%llu, retry %d", q.name, (unsigned long long)present,
                    (unsigned long long)q.size, attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(2000 * (attempt + 1)));
            if (attempt == 5) ok = false;
        }
        off += q.size;
    }
    nv_sd_fclose(f);
    free(wbuf);
    if (corrupt) unlink(part);                                     // wrong bytes: the next try starts over
    if (!ok) return false;                                         // the .part stays: the next try resumes
    unlink(path);                                                  // FAT rename won't overwrite
    if (rename(part, path) != 0) { NV_LOGE(TAG, "data: rename %s errno=%d", part, errno); return false; }
    return true;
}

bool install_data(const char *base, const nv_store_entry_t *e) {
    char url[320];
    snprintf(url, sizeof url, "%s/data/%s/pack.sig", base, e->id);
    char *body = (char *)heap_caps_malloc(kPkgCap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto *pk = (nv_store_pkg::DataPack *)heap_caps_malloc(sizeof(nv_store_pkg::DataPack), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = false;
    if (!body || !pk) { set_state(NV_STORE_ERROR, "out of memory"); goto out; }
    {
        int status = 0;
        const int got = http_get_buf(url, body, kPkgCap + 1, &status);
        if (got < 0) { set_state(NV_STORE_ERROR, "Data pack unreachable"); goto out; }
        if (!nv_store_pkg::parse_data(body, (size_t)got, pk) || pk->format != 1 || strcmp(pk->id, e->id) ||
            strcmp(pk->version, e->version) ||
            !sig_verify((const uint8_t *)body, pk->signed_len, pk->sig, pk->sig_len)) {
            nv_seclog_add(NV_SEC_APP_REFUSED, e->id);
            set_state(NV_STORE_ERROR, "Package signature invalid - not installed");
            goto out;
        }
    }
    {
        char dir[96];
        snprintf(dir, sizeof dir, "%s/%s", kDataDir, pk->dest);
        mkdirs(dir);
        mkdirs(kPacksDir);
        const uint64_t total = nv_store_pkg::data_total(*pk);
        uint64_t have = 0;                                         // what earlier attempts left
        for (int i = 0; i < pk->n; i++) {
            if (i && !strcmp(pk->parts[i].name, pk->parts[i - 1].name)) continue;
            char part[168]; struct stat st;
            snprintf(part, sizeof part, "%s/%s.part", dir, pk->parts[i].name);
            if (stat(part, &st) == 0) have += (uint64_t)st.st_size;
        }
        uint64_t sd_total = 0, sd_free = 0;
        // the OTA reserve: a firmware update is staged on this card and must always fit
        if (nv_sd_info(&sd_total, &sd_free) && sd_free + have < total + NV_APPSTORE_OTA_RESERVE) {
            set_state(NV_STORE_ERROR, "Not enough space on the SD card");
            NV_LOGE(TAG, "data: %s needs %llu MB, %llu MB free", e->id, (unsigned long long)(total >> 20),
                    (unsigned long long)(sd_free >> 20));
            goto out;
        }
        uint64_t done = 0;
        set_progress(0);
        for (int i = 0; i < pk->n; ) {
            int j = i;
            while (j + 1 < pk->n && !strcmp(pk->parts[j + 1].name, pk->parts[i].name)) j++;
            char path[160];
            snprintf(path, sizeof path, "%s/%s", dir, pk->parts[i].name);
            if (!data_fetch_to(*pk, i, j, path, &done, total)) {
                set_state(NV_STORE_ERROR, "Download interrupted - tap Install to resume");
                goto out;
            }
            i = j + 1;
        }
        // The record: what is installed and where. Written last: without it the pack is "not installed".
        char rec[96], tmp[100];
        snprintf(rec, sizeof rec, "%s/%s.pack", kPacksDir, e->id);
        snprintf(tmp, sizeof tmp, "%s.tmp", rec);
        FILE *f = fopen(tmp, "w");
        if (!f) { set_state(NV_STORE_ERROR, "Install failed (SD write)"); goto out; }
        fprintf(f, "%s\n%s\n", pk->version, pk->dest);
        for (int i = 0; i < pk->n; i++)
            if (!i || strcmp(pk->parts[i].name, pk->parts[i - 1].name)) fprintf(f, "%s\n", pk->parts[i].name);
        const bool wrote = fclose(f) == 0;
        unlink(rec);
        if (!wrote || rename(tmp, rec) != 0) { set_state(NV_STORE_ERROR, "Install failed (SD write)"); goto out; }
        lock();
        for (int i = 0; i < s_cat_n; i++) if (!strcmp(s_cat[i].id, e->id)) { s_cat[i].installed = true; s_cat[i].update = false; }
        unlock();
        if (s_data_hook) s_data_hook(pk->dest);
        NV_LOGI(TAG, "installed data pack '%s' v%s (%llu MB) in %s", e->id, pk->version,
                (unsigned long long)(total >> 20), dir);
        ok = true;
    }
out:
    free(pk);
    free(body);
    return ok;
}

// ---- system content packs (content/<id>/pack.sig, "nucleoos-data-v2") ------------------------------
// See nv_appstore.h. Layout on the card:
//   /sdcard/nucleos/content/<id>.pack        the record: what is installed (written last)
//   /sdcard/nucleos/content/<id>.<k>.idx     the signed index of tree line k (verify/adopt later)
//   /sdcard/nucleos/content/stage/<id>/      downloads in progress (<k>.idx, <k>.tar, as .part)
//   /sdcard/nucleos/content/swap.journal     "extract <D>" or "swap <D>": the one tree operation in flight
// A tree that REPLACES folder D is extracted into D.new, every file checked against the index, then
// D -> D.old, D.new -> D, D.old deleted; nv_store_tree::swap_recover() finishes or undoes that after a
// power cut. A SEED tree only adds the files that don't exist yet (things the owner may edit).
constexpr char kContentDir[]   = "/sdcard/nucleos/content";
constexpr char kContentStage[] = "/sdcard/nucleos/content/stage";
constexpr char kSwapJournal[]  = "/sdcard/nucleos/content/swap.journal";
constexpr char kContentRec[]   = "nucleoos-content-v1";
constexpr char kSdRoot[]       = "/sdcard";
void (*s_content_hook)(const char *dest, const char *name, bool before) = nullptr;

bool exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

// Delete a file or a whole folder. Depth-first, bounded by kDepthMax + the path buffer.
bool rm_tree(const char *path, int depth = 0) {
    struct stat st;
    if (stat(path, &st) != 0) return true;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;
    if (depth > nv_store_tree::kDepthMax + 2) return false;
    DIR *d = opendir(path);
    if (!d) return false;
    char child[300];
    bool ok = true;
    while (struct dirent *e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(child, sizeof child, "%s/%s", path, e->d_name) >= (int)sizeof child) { ok = false; continue; }
        ok &= rm_tree(child, depth + 1);
    }
    closedir(d);
    return rmdir(path) == 0 && ok;
}

// mkdirs() of the folder that will hold `file`.
void mkdirs_parent(const char *file) {
    char p[300];
    snprintf(p, sizeof p, "%s", file);
    char *slash = strrchr(p, '/');
    if (slash && slash != p) { *slash = 0; mkdirs(p); }
}

bool write_small(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    const bool ok = fputs(text, f) >= 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    return fclose(f) == 0 && ok;
}

// A tree's root folder: /sdcard/<dest> or /sdcard/<dest>/<name>.
void tree_root(char *out, size_t n, const char *dest, const char *name) {
    if (!strcmp(name, ".")) snprintf(out, n, "%s/%s", kSdRoot, dest);
    else                    snprintf(out, n, "%s/%s/%s", kSdRoot, dest, name);
}

// A journal may only ever name a folder under one of the content destinations.
bool journal_target_ok(const char *d) {
    for (int i = 0; nv_store_pkg::kContentDests[i]; i++) {
        char root[64];
        snprintf(root, sizeof root, "%s/%s", kSdRoot, nv_store_pkg::kContentDests[i]);
        const size_t rl = strlen(root);
        if (!strncmp(d, root, rl) && (d[rl] == 0 || d[rl] == '/') && !strstr(d, "/..") && !strstr(d, "/./"))
            return true;
    }
    return false;
}

// Run swap_recover()'s verdict for folder D.
void swap_apply(const char *D, unsigned a) {
    char n[300], o[300];
    snprintf(n, sizeof n, "%s.new", D);
    snprintf(o, sizeof o, "%s.old", D);
    using namespace nv_store_tree;
    if (a & SWAP_RM_STALE)   rm_tree(o);
    if (a & SWAP_DIR_TO_OLD) { if (rename(D, o) != 0) NV_LOGE(TAG, "content: rename %s -> .old errno=%d", D, errno); }
    if (a & SWAP_NEW_TO_DIR) { if (rename(n, D) != 0) NV_LOGE(TAG, "content: rename %s.new errno=%d", D, errno); }
    if (a & SWAP_OLD_TO_DIR) { if (rename(o, D) != 0) NV_LOGE(TAG, "content: roll back %s errno=%d", D, errno); }
    if (a & SWAP_RM_NEW)     rm_tree(n);
    if (a & SWAP_RM_OLD)     rm_tree(o);
}

// Extracts a tree archive, checking every file against the signed index.
struct Extract : nv_store_tree::Sink {
    const char *root = nullptr;                 // write under here
    bool seed = false;                          // only files that don't exist yet
    const nv_store_tree::Index *ix = nullptr;
    uint8_t *seen = nullptr;                    // one flag per index entry
    FILE *f = nullptr;
    char *wbuf = nullptr;                       // aligned stream buffer, reused file after file
    int cur = -1;
    char path[300], tmp[308];
    nv_store_tree::Entry e;
    mbedtls_sha256_context sha;
    bool sha_on = false;

    bool dir(const char *p) override {
        char d[300];
        if (snprintf(d, sizeof d, "%s/%s", root, p) >= (int)sizeof d) return false;
        mkdirs(d);
        return true;
    }
    bool begin(const char *p, uint64_t size) override {
        cur = nv_store_tree::index_find(*ix, p);
        if (cur < 0 || seen[cur] || !nv_store_tree::index_entry(*ix, cur, &e) || e.size != size) {
            NV_LOGE(TAG, "content: '%s' is not in the signed index", p);
            return false;
        }
        if (snprintf(path, sizeof path, "%s/%s", root, p) >= (int)sizeof path) return false;
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
        sha_on = true;
        f = nullptr;
        if (seed && exists(path)) return true;  // the owner's copy wins: hash only
        mkdirs_parent(path);
        f = nv_sd_fopen(tmp, "wb");
        if (!f) NV_LOGE(TAG, "content: create %s errno=%d", tmp, errno);
        else if (wbuf) setvbuf(f, wbuf, _IOFBF, kSdBuf);
        return f != nullptr;
    }
    bool data(const uint8_t *p, size_t n) override {
        mbedtls_sha256_update(&sha, p, n);
        if (f && fwrite(p, 1, n, f) != n) { NV_LOGE(TAG, "content: SD write failed (full?)"); return false; }
        return true;
    }
    bool end() override {
        uint8_t h[32];
        mbedtls_sha256_finish(&sha, h);
        mbedtls_sha256_free(&sha);
        sha_on = false;
        const bool wrote = f != nullptr;
        if (f && nv_sd_fclose(f) != 0) { f = nullptr; unlink(tmp); return false; }
        f = nullptr;
        if (memcmp(h, e.sha256, 32) != 0) {
            NV_LOGE(TAG, "content: %s does not match the signed index", path);
            if (wrote) unlink(tmp);
            return false;
        }
        if (wrote) {
            unlink(path);                       // FAT rename won't overwrite (replace: a fresh D.new anyway)
            if (rename(tmp, path) != 0) { NV_LOGE(TAG, "content: rename %s errno=%d", tmp, errno); return false; }
        }
        seen[cur] = 1;
        return true;
    }
    void abort_file() {
        if (f) { nv_sd_fclose(f); f = nullptr; unlink(tmp); }
        if (sha_on) { mbedtls_sha256_free(&sha); sha_on = false; }
    }
};

// The file at `path` already holds exactly parts [first, last]: their sizes and signed hashes.
bool file_matches(const nv_store_pkg::DataPack &pk, int first, int last, const char *path) {
    uint64_t want = 0;
    for (int i = first; i <= last; i++) want += pk.parts[i].size;
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode) || (uint64_t)st.st_size != want) return false;
    FILE *f = nv_sd_fopen(path, "rb");
    if (!f) return false;
    bool ok = true;
    uint64_t off = 0;
    for (int i = first; i <= last && ok; i++) {
        mbedtls_sha256_context sha;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
        uint8_t h[32];
        ok = rehash(f, off, pk.parts[i].size, &sha);
        mbedtls_sha256_finish(&sha, h);
        mbedtls_sha256_free(&sha);
        ok = ok && !memcmp(h, pk.parts[i].sha256, 32);
        off += pk.parts[i].size;
    }
    nv_sd_fclose(f);
    return ok;
}

// Read a whole small file into a fresh PSRAM buffer (NUL-terminated). *len = its size.
char *read_small(const char *path, size_t max, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    char *b = (char *)heap_caps_malloc(max + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t n = b ? fread(b, 1, max + 1, f) : 0;
    fclose(f);
    if (!b || n > max) { free(b); return nullptr; }
    b[n] = 0;
    *len = n;
    return b;
}

// Extract archive `tar` into root (seed: in place; replace: root.new, then the journaled swap).
bool tree_install(const char *dest, const char *name, bool seed, const char *idx_path, const char *tar) {
    size_t il = 0;
    char *itext = read_small(idx_path, nv_store_pkg::kTreeIndexMax, &il);
    auto *ix = (nv_store_tree::Index *)heap_caps_malloc(sizeof(nv_store_tree::Index), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto *tr = (nv_store_tree::TarReader *)heap_caps_malloc(sizeof(nv_store_tree::TarReader), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto *x = (Extract *)heap_caps_calloc(1, sizeof(Extract), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // zeroed: wbuf is null on an early exit
    uint8_t *buf = (uint8_t *)sd_buf();
    uint8_t *seen = nullptr;
    FILE *in = nullptr;
    bool ok = false;
    char D[300], target[308], j[320];
    tree_root(D, sizeof D, dest, name);
    if (!itext || !ix || !tr || !x || !buf) { NV_LOGE(TAG, "content: out of memory"); goto out; }
    new (tr) nv_store_tree::TarReader();
    new (x) Extract();
    if (!nv_store_tree::parse_index(itext, il, ix)) { NV_LOGE(TAG, "content: bad tree index for %s", D); goto out; }
    seen = (uint8_t *)heap_caps_calloc(ix->n, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!seen) goto out;
    if (seed) {
        snprintf(target, sizeof target, "%s", D);
    } else {
        snprintf(target, sizeof target, "%s.new", D);
        snprintf(j, sizeof j, "extract %s\n", D);
        if (!write_small(kSwapJournal, j)) { NV_LOGE(TAG, "content: cannot write the journal"); goto out; }
        rm_tree(target);                                   // what an earlier, interrupted try left
    }
    mkdirs(target);
    x->root = target;
    x->wbuf = (char *)sd_buf();
    x->seed = seed;
    x->ix = ix;
    x->seen = seen;
    in = nv_sd_fopen(tar, "rb");
    if (!in) goto out;
    {
        size_t r;
        bool fed = true;
        while (fed && (r = fread(buf, 1, kSdBuf, in)) > 0) fed = tr->feed(buf, r, *x);
        x->abort_file();
        if (!fed || !tr->finished()) { NV_LOGE(TAG, "content: archive for %s is malformed or incomplete", D); goto out; }
    }
    for (int i = 0; i < ix->n; i++)
        if (!seen[i]) { NV_LOGE(TAG, "content: archive for %s misses indexed files", D); goto out; }
    if (seed) {
        ok = true;
    } else {
        // The swap. D.new is complete and verified: from the moment the journal says "swap", a reboot
        // finishes it (swap_recover); if a rename fails right now, the old tree goes back in place.
        char O[310];
        snprintf(O, sizeof O, "%s.old", D);
        if (s_content_hook) s_content_hook(dest, name, true);
        snprintf(j, sizeof j, "swap %s\n", D);
        if (write_small(kSwapJournal, j)) {
            swap_apply(D, nv_store_tree::swap_recover(true, exists(D), true, exists(O)));
            if (!exists(D) && exists(O) && rename(O, D) != 0) NV_LOGE(TAG, "content: roll back %s errno=%d", D, errno);
            ok = exists(D) && !exists(target);
            unlink(kSwapJournal);
            if (ok) rm_tree(O);
        } else {
            NV_LOGE(TAG, "content: cannot write the journal");
        }
        if (s_content_hook) s_content_hook(dest, name, false);   // whichever tree is live: reload it
    }
out:
    if (in) nv_sd_fclose(in);
    if (!ok && !seed) {
        rm_tree(target);                                   // a half or refused D.new never stays
        if (exists(kSwapJournal)) unlink(kSwapJournal);
    }
    free(seen);
    free(buf);
    if (x) free(x->wbuf);
    free(x);
    free(tr);
    free(ix);
    free(itext);
    return ok;
}

// Every file listed in index `idx_path` is under `root` with its size and signed sha256. *bad (may be
// nullptr) counts the files that are missing or differ. False also when the index can't be read.
bool tree_matches(const char *root, const char *idx_path, int *bad) {
    if (bad) *bad = 0;
    size_t il = 0;
    char *itext = read_small(idx_path, nv_store_pkg::kTreeIndexMax, &il);
    auto *ix = (nv_store_tree::Index *)heap_caps_malloc(sizeof(nv_store_tree::Index), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *buf = (uint8_t *)sd_buf();
    bool ok = itext && ix && buf && nv_store_tree::parse_index(itext, il, ix);
    int nbad = ok ? 0 : 1;
    for (int i = 0; ok && i < ix->n; i++) {
        nv_store_tree::Entry e;
        char path[320];
        struct stat st;
        if (!nv_store_tree::index_entry(*ix, i, &e) ||
            snprintf(path, sizeof path, "%s/%s", root, e.path) >= (int)sizeof path ||
            stat(path, &st) != 0 || (uint64_t)st.st_size != e.size) { nbad++; continue; }
        FILE *f = nv_sd_fopen(path, "rb");
        if (!f) { nbad++; continue; }
        mbedtls_sha256_context sha;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
        size_t r;
        while ((r = fread(buf, 1, kSdBuf, f)) > 0) mbedtls_sha256_update(&sha, buf, r);
        nv_sd_fclose(f);
        uint8_t h[32];
        mbedtls_sha256_finish(&sha, h);
        mbedtls_sha256_free(&sha);
        if (memcmp(h, e.sha256, 32)) nbad++;
    }
    free(buf);
    free(ix);
    free(itext);
    if (bad) *bad = nbad;
    return nbad == 0;
}

// The record of an installed content pack: its version (and dest), or false when there is none.
bool content_record_read(const char *id, char *version, size_t vn, char *dest, size_t dn) {
    char p[96], line[96];
    snprintf(p, sizeof p, "%s/%s.pack", kContentDir, id);
    FILE *f = fopen(p, "r");
    if (!f) return false;
    int row = 0;
    bool ok = false;
    while (fgets(line, sizeof line, f) && row < 3) {
        line[strcspn(line, "\r\n")] = 0;
        if (row == 0) ok = !strcmp(line, kContentRec);
        else if (row == 1 && version) copy_bounded(version, vn, line);
        else if (row == 2 && dest) copy_bounded(dest, dn, line);
        row++;
    }
    fclose(f);
    return ok && row == 3;
}

bool install_content(const char *base, const char *id) {
    if (!nv_sd_is_mounted()) { set_state(NV_STORE_ERROR, "No SD card"); return false; }
    char url[320];
    snprintf(url, sizeof url, "%s/content/%s/pack.sig", base, id);
    char *body = (char *)heap_caps_malloc(kPkgCap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto *pk = (nv_store_pkg::DataPack *)heap_caps_malloc(sizeof(nv_store_pkg::DataPack), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = false;
    char stage[128];
    snprintf(stage, sizeof stage, "%s/%s", kContentStage, id);
    set_state(NV_STORE_INSTALLING, "Downloading...");
    set_progress(0);
    if (!body || !pk) { set_state(NV_STORE_ERROR, "out of memory"); goto out; }
    {
        int status = 0;
        const int got = http_get_buf(url, body, kPkgCap + 1, &status);
        if (got < 0) { set_state(NV_STORE_ERROR, "Content unreachable"); goto out; }
        body[got] = 0;
        if (!nv_store_pkg::parse_data(body, (size_t)got, pk) || pk->format != 2 || strcmp(pk->id, id) ||
            !sig_verify((const uint8_t *)body, pk->signed_len, pk->sig, pk->sig_len)) {
            nv_seclog_add(NV_SEC_APP_REFUSED, id);
            set_state(NV_STORE_ERROR, "Content signature invalid - not installed");
            goto out;
        }
    }
    {
        char dir[96];
        snprintf(dir, sizeof dir, "%s/%s", kSdRoot, pk->dest);
        mkdirs(dir);
        mkdirs(stage);
        // Space: every download, plus each replaced tree once more while D.new and D coexist, plus
        // the OTA reserve. Bytes already in .part files from an earlier try count as present.
        const uint64_t total = nv_store_pkg::data_total(*pk);
        uint64_t need = total + NV_APPSTORE_OTA_RESERVE, have = 0;
        for (int i = 0; i < pk->n; i++) {
            const nv_store_pkg::DataPart &q = pk->parts[i];
            if (q.kind == 't' || q.kind == 'u') need += q.size;
            char part[240];
            struct stat st;
            if (q.kind == 'f') snprintf(part, sizeof part, "%s/%s.part", dir, q.name);
            else               snprintf(part, sizeof part, "%s/%d.%s.part", stage, i, q.kind == 'i' ? "idx" : "tar");
            if ((!i || strcmp(pk->parts[i - 1].name, q.name) || q.kind != 'f') && stat(part, &st) == 0)
                have += (uint64_t)st.st_size;
        }
        uint64_t sd_total = 0, sd_free = 0;
        if (nv_sd_info(&sd_total, &sd_free) && sd_free + have < need) {
            set_state(NV_STORE_ERROR, "Not enough space on the SD card");
            NV_LOGE(TAG, "content: %s needs %llu MB (incl. the OTA reserve), %llu MB free", id,
                    (unsigned long long)(need >> 20), (unsigned long long)(sd_free >> 20));
            goto out;
        }
        uint64_t done = 0;
        for (int i = 0; i < pk->n; ) {
            const nv_store_pkg::DataPart &q = pk->parts[i];
            int j = i;
            if (q.kind == 'f')
                while (j + 1 < pk->n && pk->parts[j + 1].kind == 'f' && !strcmp(pk->parts[j + 1].name, q.name)) j++;
            char path[240];
            if (q.kind == 'f') snprintf(path, sizeof path, "%s/%s", dir, q.name);
            else               snprintf(path, sizeof path, "%s/%d.%s", stage, i, q.kind == 'i' ? "idx" : "tar");
            if (q.kind == 'f') mkdirs_parent(path);
            bool adopted = false;
            if (q.kind == 't' || q.kind == 'u') {          // the tree already on the card matches its index
                char idx[240], root[300];
                snprintf(idx, sizeof idx, "%s/%d.idx", stage, i - 1);
                tree_root(root, sizeof root, pk->dest, q.name);
                adopted = tree_matches(root, idx, nullptr);
            }
            if (adopted || file_matches(*pk, i, j, path)) {   // already there (an earlier try, a card made on a PC)
                for (int k = i; k <= j; k++) done += pk->parts[k].size;
                set_progress((int)(done * 100 / total));
                if (adopted) NV_LOGI(TAG, "content: %s/%s already matches its index, kept", pk->dest, q.name);
            } else if (!data_fetch_to(*pk, i, j, path, &done, total)) {
                set_state(NV_STORE_ERROR, "Download interrupted - it resumes on the next try");
                goto out;
            }
            if (q.kind == 't' || q.kind == 'u') {
                char idx[240];
                snprintf(idx, sizeof idx, "%s/%d.idx", stage, i - 1);
                set_state(NV_STORE_INSTALLING, "Installing...");
                if (!adopted && !tree_install(pk->dest, q.name, q.kind == 'u', idx, path)) {
                    unlink(path);                          // verified bytes, but extraction failed: refetch
                    set_state(NV_STORE_ERROR, "Install failed - try again");
                    goto out;
                }
                unlink(path);                              // the archive is spent; the index is kept
                char keep[240];
                snprintf(keep, sizeof keep, "%s/%s.%d.idx", kContentDir, id, i - 1);
                unlink(keep);
                if (rename(idx, keep) != 0) NV_LOGW(TAG, "content: keep index %s errno=%d", keep, errno);
                set_state(NV_STORE_INSTALLING, "Downloading...");
            }
            i = j + 1;
        }
        // The signed list stays on the card: verify/repair re-check the files against it later.
        char sigp[96], sigt[100];
        snprintf(sigp, sizeof sigp, "%s/%s.sig", kContentDir, id);
        snprintf(sigt, sizeof sigt, "%s.tmp", sigp);
        if (FILE *sf = fopen(sigt, "wb")) {
            const bool w = fwrite(body, 1, strlen(body), sf) == strlen(body);
            if (fclose(sf) == 0 && w) { unlink(sigp); rename(sigt, sigp); }
        }
        // The record, written last: without it the pack is "not installed".
        char rec[96], tmp[100];
        snprintf(rec, sizeof rec, "%s/%s.pack", kContentDir, id);
        snprintf(tmp, sizeof tmp, "%s.tmp", rec);
        FILE *f = fopen(tmp, "w");
        if (!f) { set_state(NV_STORE_ERROR, "Install failed (SD write)"); goto out; }
        fprintf(f, "%s\n%s\n%s\n", kContentRec, pk->version, pk->dest);
        for (int i = 0; i < pk->n; i++)
            if (pk->parts[i].kind != 'i' && (!i || strcmp(pk->parts[i].name, pk->parts[i - 1].name) || pk->parts[i].kind != 'f'))
                fprintf(f, "%c %s\n", pk->parts[i].kind, pk->parts[i].name);
        const bool wrote = fclose(f) == 0;
        unlink(rec);
        if (!wrote || rename(tmp, rec) != 0) { set_state(NV_STORE_ERROR, "Install failed (SD write)"); goto out; }
        rm_tree(stage);
        if (s_content_hook) s_content_hook(pk->dest, nullptr, false);
        NV_LOGI(TAG, "installed content '%s' v%s (%llu MB) in %s", id, pk->version,
                (unsigned long long)(total >> 20), dir);
        char m[64];
        snprintf(m, sizeof m, "Installed %s", id);
        set_state(NV_STORE_READY, m);
        set_progress(100);
        ok = true;
    }
out:
    free(pk);
    free(body);
    return ok;
}

// ---- content index + verify ----
bool id_ok(const char *id);                                // catalog parse, below
const char *jstr(const cJSON *o, const char *k, const char *def);
constexpr int kIndexCap = 32 * 1024;                       // content/index-v1.json
nv_content_entry_t *s_cidx = nullptr;                      // PSRAM, NV_CONTENT_MAX rows
int      s_cidx_n = 0;
uint32_t s_cidx_gen = 0;
NV_PSRAM_BSS char s_verify_id[32];
int      s_verify_res = -1, s_verify_bad = 0;

// The string for the UI language in a {"it": "...", "en": "..."} object (English, then "" otherwise).
const char *pick_lang(const cJSON *o) {
    const cJSON *v = cJSON_GetObjectItem(o, lang_code());
    if (!cJSON_IsString(v)) v = cJSON_GetObjectItem(o, "en");
    return cJSON_IsString(v) ? v->valuestring : "";
}

// Rows of index body `buf` with names and descriptions in the CURRENT UI language. False when the body
// is not a format-1 index.
bool parse_content_index(const char *buf, size_t len, nv_content_entry_t *rows, int *count) {
    int n = 0;
    cJSON *root = cJSON_ParseWithLength(buf, len);
    const cJSON *packs = root ? cJSON_GetObjectItem(root, "packs") : nullptr;
    const cJSON *fmt = root ? cJSON_GetObjectItem(root, "format") : nullptr;
    const bool ok = cJSON_IsArray(packs) && cJSON_IsNumber(fmt) && fmt->valueint == 1;
    const cJSON *it = nullptr;
    if (ok) {
        cJSON_ArrayForEach(it, packs) {
            if (n >= NV_CONTENT_MAX) break;
            nv_content_entry_t &e = rows[n];
            memset(&e, 0, sizeof e);
            const char *id = jstr(it, "id", "");
            if (!id_ok(id) || strlen(id) >= sizeof e.id) continue;
            snprintf(e.id, sizeof e.id, "%s", id);
            snprintf(e.version, sizeof e.version, "%s", jstr(it, "version", ""));
            snprintf(e.dest, sizeof e.dest, "%s", jstr(it, "dest", ""));
            const cJSON *sz = cJSON_GetObjectItem(it, "size");
            e.size = cJSON_IsNumber(sz) && sz->valuedouble > 0 ? (uint64_t)sz->valuedouble : 0;
            const char *nm = pick_lang(cJSON_GetObjectItem(it, "names"));
            snprintf(e.name, sizeof e.name, "%s", nm[0] ? nm : e.id);
            snprintf(e.desc, sizeof e.desc, "%s", pick_lang(cJSON_GetObjectItem(it, "desc")));
            const cJSON *ls = cJSON_GetObjectItem(it, "langs");
            const cJSON *l = nullptr;
            size_t ln = 0;
            if (cJSON_IsArray(ls))
                cJSON_ArrayForEach(l, ls)
                    if (cJSON_IsString(l) && ln + strlen(l->valuestring) + 2 < sizeof e.langs)
                        ln += (size_t)snprintf(e.langs + ln, sizeof e.langs - ln, "%s%s", ln ? "," : "", l->valuestring);
            n++;
        }
    }
    if (root) cJSON_Delete(root);
    *count = n;
    return ok;
}

// The index body stays in PSRAM: a language change re-reads the names from it (no download).
char  *s_cidx_body = nullptr;
size_t s_cidx_len = 0;
char   s_cidx_lang[4] = "";

// Caller holds the lock. Re-reads the rows when the UI language changed since the last parse.
void cidx_relang_locked(void) {
    if (!s_cidx || !s_cidx_body || !strcmp(s_cidx_lang, lang_code())) return;
    int n = 0;
    if (parse_content_index(s_cidx_body, s_cidx_len, s_cidx, &n)) s_cidx_n = n;
    snprintf(s_cidx_lang, sizeof s_cidx_lang, "%s", lang_code());
}

void fetch_content_index(const char *base) {
    set_state(NV_STORE_FETCHING, "Loading content list...");
    char url[300];
    snprintf(url, sizeof url, "%s/content/index-v1.json", base);
    char *buf = (char *)heap_caps_malloc(kIndexCap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto *rows = (nv_content_entry_t *)heap_caps_calloc(NV_CONTENT_MAX, sizeof(nv_content_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int n = 0, status = 0;
    const int got = buf && rows ? http_get_buf(url, buf, kIndexCap + 1, &status) : -1;
    const bool ok = got > 0 && parse_content_index(buf, (size_t)got, rows, &n);
    if (!ok) {
        free(rows);
        free(buf);
        set_state(NV_STORE_ERROR, got < 0 ? "Content list unreachable" : "Bad content list");
        return;
    }
    lock();
    nv_content_entry_t *old = s_cidx;
    char *old_body = s_cidx_body;
    s_cidx = rows;
    s_cidx_n = n;
    s_cidx_body = buf;
    s_cidx_len = (size_t)got;
    snprintf(s_cidx_lang, sizeof s_cidx_lang, "%s", lang_code());
    s_cidx_gen++;
    unlock();
    free(old);
    free(old_body);
    set_state(NV_STORE_READY, "Content list loaded");
}

void verify_content(const char *id) {
    set_state(NV_STORE_INSTALLING, "Checking...");
    set_progress(0);
    int bad = 0, res = -1;
    char sigp[96];
    snprintf(sigp, sizeof sigp, "%s/%s.sig", kContentDir, id);
    size_t len = 0;
    char *body = read_small(sigp, (size_t)kPkgCap, &len);
    auto *pk = (nv_store_pkg::DataPack *)heap_caps_malloc(sizeof(nv_store_pkg::DataPack), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body && pk && nv_store_pkg::parse_data(body, len, pk) && pk->format == 2 && !strcmp(pk->id, id) &&
        sig_verify((const uint8_t *)body, pk->signed_len, pk->sig, pk->sig_len)) {
        res = 1;
        for (int i = 0; i < pk->n; ) {
            const nv_store_pkg::DataPart &q = pk->parts[i];
            int j = i;
            if (q.kind == 'f')
                while (j + 1 < pk->n && pk->parts[j + 1].kind == 'f' && !strcmp(pk->parts[j + 1].name, q.name)) j++;
            if (q.kind == 'f') {
                char path[240];
                snprintf(path, sizeof path, "%s/%s/%s", kSdRoot, pk->dest, q.name);
                if (!file_matches(*pk, i, j, path)) bad++;
            } else if (q.kind == 't') {                   // a seed tree ('u') belongs to the owner: not checked
                char idx[160], root[300];
                snprintf(idx, sizeof idx, "%s/%s.%d.idx", kContentDir, id, i - 1);
                tree_root(root, sizeof root, pk->dest, q.name);
                int b = 0;
                if (!tree_matches(root, idx, &b)) bad += b ? b : 1;
            }
            set_progress((j + 1) * 100 / pk->n);
            i = j + 1;
        }
        if (bad) res = 0;
    }
    free(pk);
    free(body);
    lock();
    snprintf(s_verify_id, sizeof s_verify_id, "%s", id);
    s_verify_res = res;
    s_verify_bad = bad;
    unlock();
    char m[96];
    if (res == 1)      snprintf(m, sizeof m, "%s: intact", id);
    else if (res == 0) snprintf(m, sizeof m, "%s: %d file(s) damaged or missing", id, bad);
    else               snprintf(m, sizeof m, "%s: cannot be checked (not installed?)", id);
    set_state(res == 1 ? NV_STORE_READY : NV_STORE_ERROR, m);
    NV_LOGI(TAG, "content verify: %s", m);
}

// Delete <dir>/*.part older than max_age_s (one level, plus `depth` levels below).
void sweep_parts(const char *dir, time_t now, uint32_t max_age_s, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    char p[300];
    while (struct dirent *e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth > 0) sweep_parts(p, now, max_age_s, depth - 1); continue; }
        const size_t nl = strlen(e->d_name);
        if (nl > 5 && !strcmp(e->d_name + nl - 5, ".part") && now > st.st_mtime && (uint32_t)(now - st.st_mtime) > max_age_s) {
            NV_LOGW(TAG, "content: dropping a stale download %s", p);
            unlink(p);
        }
    }
    closedir(d);
}

// Boot: finish or undo the one tree operation a reboot interrupted (see swap.journal above).
void content_recover() {
    size_t n = 0;
    char *j = read_small(kSwapJournal, 400, &n);
    if (!j) return;
    j[strcspn(j, "\r\n")] = 0;
    const bool swap = !strncmp(j, "swap ", 5), extract = !strncmp(j, "extract ", 8);
    const char *D = swap ? j + 5 : (extract ? j + 8 : "");
    if ((swap || extract) && journal_target_ok(D)) {
        char N[310], O[310];
        snprintf(N, sizeof N, "%s.new", D);
        snprintf(O, sizeof O, "%s.old", D);
        const unsigned a = nv_store_tree::swap_recover(swap, exists(D), exists(N), exists(O));
        NV_LOGW(TAG, "content: finishing an interrupted %s of %s (0x%x)", swap ? "swap" : "extraction", D, a);
        swap_apply(D, a);
    } else {
        NV_LOGE(TAG, "content: ignoring a malformed swap journal");
    }
    unlink(kSwapJournal);
    free(j);
}

// ---- catalog parse ------------------------------------------------------------------------------

// True when an id is safe as a directory name (mirrors nv_wasm's id_valid — never trust the server).
bool id_ok(const char *id) {
    if (!id || !id[0]) return false;
    for (const char *p = id; *p; ++p) {
        const char c = *p;
        const bool good = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!good || (p - id) >= 31) return false;
    }
    return true;
}

// A dependency version: digits and dots only, short ("1", "1.0", "2.3.1").
bool version_ok(const char *v) {
    if (!v || !v[0] || strlen(v) > 11) return false;
    for (const char *p = v; *p; ++p) if (!((*p >= '0' && *p <= '9') || *p == '.')) return false;
    return true;
}

const char *jstr(const cJSON *o, const char *k, const char *def) {
    const cJSON *j = cJSON_GetObjectItem(o, k);
    return (cJSON_IsString(j) && j->valuestring) ? j->valuestring : def;
}
uint32_t ju32(const cJSON *o, const char *k, uint32_t def) {
    const cJSON *j = cJSON_GetObjectItem(o, k);
    return (cJSON_IsNumber(j) && j->valuedouble >= 0) ? (uint32_t)j->valuedouble : def;
}
bool jbool(const cJSON *o, const char *k) {
    const cJSON *j = cJSON_GetObjectItem(o, k);
    return cJSON_IsTrue(j);
}

// A "variants" id: ^[a-z0-9_-]{1,8}$ (it becomes the content of a file the app reads).
bool variant_ok(const char *v) {
    if (!v || !*v || strlen(v) > 8) return false;
    for (const char *p = v; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    return true;
}
bool write_variant(const char *id, const char *variant) {
    char p[96];
    snprintf(p, sizeof p, "%s/%s/data", kAppsDir, id);
    mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/%s/data/variant", kAppsDir, id);
    FILE *f = fopen(p, "w");
    if (!f) return false;
    const bool ok = fputs(variant, f) >= 0;      // no newline: the app compares the whole file
    fclose(f);
    return ok;
}

// "YYYY-MM-DD" -> YYYYMMDD (0 when absent or malformed): sortable, and cheap to print.
uint32_t jdate(const cJSON *o, const char *k) {
    const char *v = jstr(o, k, "");
    unsigned y, m, d;
    if (sscanf(v, "%4u-%2u-%2u", &y, &m, &d) != 3 || y < 2000 || y > 2999 || !m || m > 12 || !d || d > 31)
        return 0;
    return y * 10000 + m * 100 + d;
}

// Parse a store.json body into `out` (NV_STORE_MAX rows), deriving installed/update from the local
// card. Returns the row count (0 is valid: an empty store), or -1 on a malformed document.
int parse_catalog(const char *body, nv_store_entry_t *out, int cap = NV_STORE_MAX) {
    cJSON *root = cJSON_Parse(body);   // PSRAM: cJSON hooks are installed once in app_main
    if (!root) return -1;
    cJSON *apps = cJSON_GetObjectItem(root, "apps");
    if (!cJSON_IsArray(apps)) { cJSON_Delete(root); return -1; }

    int n = 0;
    const cJSON *it = nullptr;
    cJSON_ArrayForEach(it, apps) {
        if (n >= cap) break;
        if (!cJSON_IsObject(it)) continue;
        const char *id = jstr(it, "id", "");
        if (!id_ok(id)) { NV_LOGW(TAG, "catalog: bad id '%s' skipped", id); continue; }

        nv_store_entry_t *e = &out[n];
        memset(e, 0, sizeof *e);
        snprintf(e->id,            sizeof e->id,            "%s", id);
        snprintf(e->name,          sizeof e->name,          "%s", jstr(it, "name", id));
        snprintf(e->version,       sizeof e->version,       "%s", jstr(it, "version", "?"));
        snprintf(e->author,        sizeof e->author,        "%s", jstr(it, "author", ""));
        snprintf(e->desc,          sizeof e->desc,          "%s", jstr(it, "description", ""));
        snprintf(e->license,       sizeof e->license,       "%s", jstr(it, "license", ""));
        snprintf(e->source,        sizeof e->source,        "%s", jstr(it, "source", ""));
        snprintf(e->category,      sizeof e->category,      "%s", jstr(it, "category", "other"));
        snprintf(e->category_name, sizeof e->category_name, "%s", jstr(it, "category_name", "Other"));
        snprintf(e->subcategory,      sizeof e->subcategory,      "%s", jstr(it, "subcategory", ""));
        snprintf(e->subcategory_name, sizeof e->subcategory_name, "%s", jstr(it, "subcategory_name", ""));
        snprintf(e->platform,         sizeof e->platform,         "%s", jstr(it, "platform", ""));
        e->abi      = ju32(it, "abi", 1);
        e->size     = ju32(it, "size", 0);
        e->aot_size = ju32(it, "aot", 0);
        e->icon_z   = ju32(it, "icon_z", 0);
        e->is_game  = jbool(it, "game");
        e->has_icon = jbool(it, "icon");
        e->featured = jbool(it, "featured");
        e->library  = !strcmp(jstr(it, "kind", ""), "library");
        e->engine   = jstr(it, "engine", "")[0] != '\0';
        e->has_doc  = jbool(it, "doc");
        e->console  = jbool(it, "console");
        e->data     = !strcmp(jstr(it, "kind", ""), "data");
        const uint32_t nf = ju32(it, "files", 0);
        e->files    = (uint16_t)(nf > (uint32_t)kMaxFiles ? kMaxFiles : nf);
        e->perms    = 0;
        const cJSON *pa = cJSON_GetObjectItem(it, "perms");
        const cJSON *pv = nullptr;
        if (cJSON_IsArray(pa))
            cJSON_ArrayForEach(pv, pa)
                if (cJSON_IsString(pv)) e->perms |= nv_wasm_perm_bit(pv->valuestring) & NV_WPERM_SENSITIVE;
        const cJSON *rq = cJSON_GetObjectItem(it, "requires"), *d = nullptr;
        if (cJSON_IsObject(rq))
            cJSON_ArrayForEach(d, rq) {
                if (e->n_deps >= NV_STORE_DEPS_MAX) break;
                if (!d->string || !id_ok(d->string) || !version_ok(d->valuestring)) continue;
                snprintf(e->deps[e->n_deps].id, sizeof e->deps[0].id, "%s", d->string);
                snprintf(e->deps[e->n_deps].version, sizeof e->deps[0].version, "%s", d->valuestring);
                e->n_deps++;
            }
        const cJSON *jr = cJSON_GetObjectItem(it, "rating");
        e->rating10 = (cJSON_IsNumber(jr) && jr->valuedouble > 0)
                      ? (uint16_t)(jr->valuedouble * 10 + 0.5) : 0;
        e->downloads = ju32(it, "downloads", 0);
        e->added     = jdate(it, "added");
        e->updated   = jdate(it, "updated");
        snprintf(e->notes, sizeof e->notes, "%s", jstr(it, "notes", ""));
        const cJSON *va = cJSON_GetObjectItem(it, "variants"), *vv = nullptr;
        if (cJSON_IsArray(va))
            cJSON_ArrayForEach(vv, va) {
                if (e->n_var >= NV_STORE_VARIANTS_MAX) break;
                const char *vid = jstr(vv, "id", "");
                if (!variant_ok(vid)) continue;
                auto &v = e->var[e->n_var++];
                snprintf(v.id, sizeof v.id, "%s", vid);
                snprintf(v.name, sizeof v.name, "%s", jstr(vv, "name", vid));
                snprintf(v.lang, sizeof v.lang, "%s", jstr(vv, "lang", ""));
                v.size = ju32(vv, "size", 0);
            }
        const uint32_t ns = ju32(it, "shots", 0);
        e->shots     = (uint8_t)(ns > NV_STORE_SHOTS_MAX ? NV_STORE_SHOTS_MAX : ns);

        nv_wasm_app_t local;
        char dver[16];
        if (e->data) {                                          // a data pack: its record, not a manifest
            if (data_record_read(id, dver, sizeof dver, nullptr, 0, nullptr, 0) >= 0) {
                e->installed = true;
                e->update    = version_is_newer(e->version, dver);
            }
        } else if (nv_wasm_load_manifest(id, &local)) {
            e->installed = true;
            e->update    = version_is_newer(e->version, local.version);
        }
        n++;
    }
    cJSON_Delete(root);
    return n;
}

// The catalog's "categories" (id, name, desc, colour "#RRGGBB", count, top[3]) into `out`.
int parse_categories(const char *body, nv_store_category_t *out) {
    cJSON *root = cJSON_Parse(body);   // PSRAM: cJSON hooks are installed once in app_main
    if (!root) return 0;
    int n = 0;
    const cJSON *arr = cJSON_GetObjectItem(root, "categories"), *it = nullptr;
    if (cJSON_IsArray(arr))
        cJSON_ArrayForEach(it, arr) {
            if (n >= NV_STORE_CATS_MAX) break;
            const char *id = jstr(it, "id", "");
            if (!id[0] || strlen(id) >= sizeof out[0].id) continue;
            nv_store_category_t *c = &out[n];
            memset(c, 0, sizeof *c);
            snprintf(c->id, sizeof c->id, "%s", id);
            snprintf(c->name, sizeof c->name, "%s", jstr(it, "name", id));
            snprintf(c->desc, sizeof c->desc, "%s", jstr(it, "desc", ""));
            const char *col = jstr(it, "color", "");
            if (col[0] == '#' && strlen(col) == 7) c->color = (uint32_t)strtoul(col + 1, nullptr, 16);
            c->count = (uint16_t)ju32(it, "count", 0);
            const cJSON *top = cJSON_GetObjectItem(it, "top"), *t = nullptr;
            int k = 0;
            if (cJSON_IsArray(top))
                cJSON_ArrayForEach(t, top) {
                    if (k >= 3) break;
                    if (cJSON_IsString(t)) snprintf(c->top[k++], sizeof c->top[0], "%s", t->valuestring);
                }
            n++;
        }
    cJSON_Delete(root);
    return n;
}

// store2 "platforms" into `out`, each one's name index into a PSRAM string in `names` (the caller
// frees them). Returns how many (0 for a legacy catalog).
int parse_platforms(const char *body, nv_store_platform_t *out, char **names) {
    cJSON *root = cJSON_Parse(body);   // PSRAM: cJSON hooks are installed once in app_main
    if (!root) return 0;
    int n = 0;
    const cJSON *arr = cJSON_GetObjectItem(root, "platforms"), *it = nullptr;
    if (cJSON_IsArray(arr))
        cJSON_ArrayForEach(it, arr) {
            if (n >= NV_STORE_PLATS_MAX) break;
            const char *id = jstr(it, "id", "");
            if (!id_ok(id) || strlen(id) >= sizeof out[0].id) continue;
            nv_store_platform_t *p = &out[n];
            memset(p, 0, sizeof *p);
            snprintf(p->id, sizeof p->id, "%s", id);
            snprintf(p->name, sizeof p->name, "%s", jstr(it, "name", id));
            snprintf(p->desc, sizeof p->desc, "%s", jstr(it, "desc", ""));
            const char *host = jstr(it, "host", "");
            if (id_ok(host)) snprintf(p->host, sizeof p->host, "%s", host);
            const char *col = jstr(it, "color", "");
            if (col[0] == '#' && strlen(col) == 7) p->color = (uint32_t)strtoul(col + 1, nullptr, 16);
            const uint32_t cnt = ju32(it, "count", 0), parts = ju32(it, "parts", 1), chunk = ju32(it, "chunk", 0);
            p->count = (uint16_t)(cnt > 60000 ? 60000 : cnt);
            p->parts = (uint16_t)(parts < 1 ? 1 : (parts > 999 ? 999 : parts));
            p->chunk = (uint16_t)(chunk > 0 && chunk <= NV_STORE_MAX ? chunk : (p->count ? p->count : 1));
            const char *nm = jstr(it, "names", "");
            const size_t len = strlen(nm);
            names[n] = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (names[n]) memcpy(names[n], nm, len + 1);
            n++;
        }
    cJSON_Delete(root);
    return n;
}

// ---- workers ------------------------------------------------------------------------------------

// <app dir>/category: the store category of an installed app (manifests do not carry it).
void category_write(const char *dir, const char *cat) {
    if (!cat || !cat[0]) return;
    char p[192], have[24] = "";
    snprintf(p, sizeof p, "%s/category", dir);
    if (FILE *f = fopen(p, "r")) { if (!fgets(have, sizeof have, f)) have[0] = '\0'; fclose(f); }
    if (!strcmp(have, cat)) return;
    if (FILE *f = fopen(p, "w")) { fputs(cat, f); fclose(f); }
}

void do_fetch(const char *base) {
    set_state(NV_STORE_FETCHING, "Contacting store...");
    char *body = (char *)heap_caps_malloc(kCatalogCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) body = (char *)malloc(kCatalogCap);
    if (!body) { set_state(NV_STORE_ERROR, "out of memory"); return; }

    // A static store (GitHub Pages) can't read a query string, so it pre-renders one catalog per
    // language: store-<lang>.json, every region. Only a 404 falls back to asking a live server
    // (?lang= localizes names/descriptions/categories, ?region= geolocates the catalog, omitted
    // when "*"; api=3: 256-byte descriptions, author / license / source and icon.z). A store that
    // doesn't answer at all isn't asked twice.
    char url[320];
    int status = 0;
    // Firmware 1.1.142+: store2-<lang>.json first (native apps + platform summaries, the carts in
    // per-platform parts); a store without it (older export, live server) gets the legacy catalog.
    snprintf(url, sizeof url, "%s/store2-%s.json", base, lang_code());
    int got = http_get_buf(url, body, kCatalogCap, &status);
    const bool v2 = got > 0;
    if (!v2) {
        snprintf(url, sizeof url, "%s/store-%s.json", base, lang_code());
        got = http_get_buf(url, body, kCatalogCap, &status);
    }
    if (!v2 && got < 0 && status == 404) {
        char region[16];
        nv_appstore_get_region(region, sizeof region);
        if (region[0] && strcmp(region, "*") != 0)
            snprintf(url, sizeof url, "%s/store.json?lang=%s&region=%s&api=3", base, lang_code(), region);
        else
            snprintf(url, sizeof url, "%s/store.json?lang=%s&api=3", base, lang_code());
        got = http_get_buf(url, body, kCatalogCap);
    }
    if (got < 0 && status != 404) {   // a transient failure (stalled read, lost handshake): once more
        NV_LOGW(TAG, "catalog fetch failed, retrying");
        got = http_get_buf(url, body, kCatalogCap, &status);
    }
    if (got < 0) {
        free(body);
        set_state(NV_STORE_ERROR, "Cannot reach store server");
        return;
    }

    // Parse into a scratch table and swap it in: parsing checks every row against the SD card,
    // and the UI reads the snapshot under the same lock, so it must not wait for that.
    auto *next = (nv_store_entry_t *)heap_caps_calloc(NV_STORE_MAX, sizeof(nv_store_entry_t),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) { free(body); set_state(NV_STORE_ERROR, "out of memory"); return; }
    const int n = parse_catalog(body, next, v2 ? NV_STORE_MAIN_MAX : NV_STORE_MAX);
    auto *cats = (nv_store_category_t *)heap_caps_calloc(NV_STORE_CATS_MAX, sizeof(nv_store_category_t),
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const int nc = (n >= 0 && cats) ? parse_categories(body, cats) : 0;
    auto *plats = (nv_store_platform_t *)heap_caps_calloc(NV_STORE_PLATS_MAX, sizeof(nv_store_platform_t),
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *names[NV_STORE_PLATS_MAX] = {};
    const int np = (n >= 0 && v2 && plats) ? parse_platforms(body, plats, names) : 0;
    free(body);
    if (n >= 0) {
        lock();
        if (cats) memcpy(s_cats, cats, (size_t)nc * sizeof(nv_store_category_t));
        s_cats_n = nc;
        unlock();
    }
    heap_caps_free(cats);
    char *old[NV_STORE_PLATS_MAX] = {};
    if (n >= 0) {
        lock();
        memcpy(s_cat, next, (size_t)n * sizeof(nv_store_entry_t));
        s_cat_n = n;
        memcpy(old, s_pl.names, sizeof old);
        if (np) memcpy(s_pl.p, plats, (size_t)np * sizeof(nv_store_platform_t));
        memcpy(s_pl.names, names, sizeof names);
        s_pl.n = np;
        s_pl.main_n = n;
        s_pl.loaded[0] = 0;
        s_pl.part = 0;
        unlock();
    } else {
        memcpy(old, names, sizeof old);   // not taken
    }
    for (char *o : old) heap_caps_free(o);
    // Apps installed before the store wrote categories: fill theirs in from this catalog.
    for (int i = 0; i < n; i++) {
        char dir[160];
        snprintf(dir, sizeof dir, "%s/%s", kAppsDir, next[i].id);
        struct stat st;
        if (stat(dir, &st) == 0) category_write(dir, next[i].category);
    }
    heap_caps_free(plats);
    free(next);

    if (n < 0) { set_state(NV_STORE_ERROR, "Bad catalog (store.json)"); return; }
    lock(); s_cat_gen++; unlock();
    char m[64];
    snprintf(m, sizeof m, n ? "%d app%s available" : "Store is empty", n, n == 1 ? "" : "s");
    set_state(NV_STORE_READY, m);
    NV_LOGI(TAG, "catalog: %d app(s), %d platform(s) from %s (%s)", n, np, base, v2 ? "store2" : "legacy");
}

// One part of one platform's carts into the table, after the native rows.
void do_platform(const char *base, const char *id, int part) {
    set_state(NV_STORE_FETCHING, "Contacting store...");
    char *body = (char *)heap_caps_malloc(kCatalogCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) { set_state(NV_STORE_ERROR, "out of memory"); return; }
    char url[320];
    snprintf(url, sizeof url, "%s/store2-%s-%s-%d.json", base, lang_code(), id, part);
    int status = 0;
    int got = http_get_buf(url, body, kCatalogCap, &status);
    if (got < 0 && status != 404) got = http_get_buf(url, body, kCatalogCap, &status);   // once more
    if (got < 0) { free(body); set_state(NV_STORE_ERROR, "Cannot reach store server"); return; }
    lock();
    const int room = NV_STORE_MAX - s_pl.main_n;
    unlock();
    auto *next = (nv_store_entry_t *)heap_caps_calloc((size_t)(room > 0 ? room : 1), sizeof(nv_store_entry_t),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) { free(body); set_state(NV_STORE_ERROR, "out of memory"); return; }
    const int n = parse_catalog(body, next, room);
    free(body);
    if (n < 0) { free(next); set_state(NV_STORE_ERROR, "Bad catalog (store.json)"); return; }
    int kept = 0;
    lock();
    for (int i = 0; i < n; i++)   // only carts of this platform, never an id the native rows list
        if (!strcmp(next[i].platform, id)) {
            bool dup = false;
            for (int j = 0; j < s_pl.main_n && !dup; j++) dup = !strcmp(s_cat[j].id, next[i].id);
            if (!dup) s_cat[s_pl.main_n + kept++] = next[i];
        }
    s_cat_n = s_pl.main_n + kept;
    snprintf(s_pl.loaded, sizeof s_pl.loaded, "%.15s", id);
    s_pl.part = part;
    unlock();
    free(next);
    char m[64];
    snprintf(m, sizeof m, "%d app%s available", kept, kept == 1 ? "" : "s");
    set_state(NV_STORE_READY, m);
    NV_LOGI(TAG, "platform %s part %d: %d cart(s)", id, part, kept);
}

// One asset path from files.json: "<img|snd|models>/<name>.<565|wav|vxm>", the name as strict as an
// app id: exactly what nv_wasm's asset lookup opens, and nothing that walks out of the package
// folder. Fills the subdirectory for the mkdir.
bool asset_ok(const char *p, char *sub, size_t sub_n) {
    static const struct { const char *dir, *ext; } kKinds[] = {
        { "img", ".565" }, { "snd", ".wav" }, { "models", ".vxm" } };
    if (!p) return false;
    const size_t n = strlen(p);
    for (const auto &k : kKinds) {
        const size_t dl = strlen(k.dir), el = strlen(k.ext);
        if (n <= dl + 1 + el || strncmp(p, k.dir, dl) != 0 || p[dl] != '/') continue;
        if (strcmp(p + n - el, k.ext) != 0) continue;
        char leaf[32];
        const size_t ln = n - dl - 1 - el;
        if (ln == 0 || ln >= sizeof leaf) return false;
        memcpy(leaf, p + dl + 1, ln);
        leaf[ln] = '\0';
        if (!id_ok(leaf)) return false;
        snprintf(sub, sub_n, "%s", k.dir);
        return true;
    }
    return false;
}

// The package's assets (textures, sounds, models) listed in files.json, each into its subfolder.
bool fetch_assets(const char *base, const char *id, const char *dir) {
    char url[320], path[224];
    snprintf(url, sizeof url, "%s/apps/%s/files.json", base, id);
    char *body = (char *)heap_caps_malloc(kFilesCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) return false;
    const int got = http_get_buf(url, body, kFilesCap);
    cJSON *root = got > 0 ? cJSON_Parse(body) : nullptr;   // PSRAM: hooks installed in app_main
    free(body);
    const cJSON *files = root ? cJSON_GetObjectItem(root, "files") : nullptr;
    if (!cJSON_IsArray(files) || cJSON_GetArraySize(files) > kMaxFiles) {
        cJSON_Delete(root);
        NV_LOGE(TAG, "install: bad files.json for '%s'", id);
        return false;
    }
    const int n = cJSON_GetArraySize(files);
    long total = 0;
    int k = 0;
    bool ok = true;
    const cJSON *f = nullptr;
    cJSON_ArrayForEach(f, files) {
        const char *p = jstr(f, "p", "");
        char sub[8];
        if (!asset_ok(p, sub, sizeof sub)) { NV_LOGE(TAG, "install: asset '%s' refused", p); ok = false; break; }
        total += (long)ju32(f, "n", 0);
        if (total > kMaxAssets) { NV_LOGE(TAG, "install: assets over %ld bytes", kMaxAssets); ok = false; break; }
        snprintf(path, sizeof path, "%s/%s", dir, sub);
        mkdir(path, 0777);
        snprintf(url, sizeof url, "%s/apps/%s/%s", base, id, p);
        snprintf(path, sizeof path, "%s/%s", dir, p);
        if (!http_get_file(url, path, kMaxAsset, 0, false, p)) { ok = false; break; }
        set_progress(++k * 100 / (n + 1));
    }
    cJSON_Delete(root);
    return ok;
}

// Download one package's files into dir (staged when signed). False with the state set on error.
bool install_files(const char *base, const nv_store_entry_t *e, const char *dir) {
    const char *id = e->id;
    char url[320], path[224];

    // Assets, then the module, the manifest last: the scanner only accepts a package once its
    // manifest exists (and an app once its module does), so no half-installed package surfaces.
    if (e->files && !fetch_assets(base, id, dir)) {
        set_state(NV_STORE_ERROR, "Download failed (assets)"); return false;
    }
    // An engine package runs its engine's module. A library ships one only when it is itself an
    // engine for other packages (ScummVM's per-engine modules): the catalog size says so.
    const bool has_module = !e->engine && (!e->library || e->size > 0);
    if (has_module) {
        snprintf(url,  sizeof url,  "%s/apps/%s/app.wasm", base, id);
        snprintf(path, sizeof path, "%s/app.wasm", dir);
        if (!http_get_file(url, path, kMaxWasm, kWasmMagic, true, "app.wasm")) {
            set_state(NV_STORE_ERROR, "Download failed (app.wasm)"); return false;
        }
    }
    // An engine package's own code (a Lua app's app.lpk, "wasi" 1.3): part of the signed package,
    // so it is installed and verified like every other file and the engine reads it read-only
    // from "/package" - no first-start download, no "net" permission. Only from a signed package
    // (its sha256/size bind it; the engine checks it again against the manifest "args"); a
    // leftover from an older version goes, the engine then falls back to its own copy.
    snprintf(path, sizeof path, "%s/app.lpk", dir);
    if (e->engine && s_pkg && nv_store_pkg::find(*s_pkg, "app.lpk")) {
        snprintf(url, sizeof url, "%s/apps/%s/app.lpk", base, id);
        if (!http_get_file(url, path, kMaxLpk, kLpkMagic, true, "app.lpk")) {
            set_state(NV_STORE_ERROR, "Download failed (app.lpk)"); return false;
        }
    } else {
        unlink(path);
    }
    set_progress(100);

    snprintf(url,  sizeof url,  "%s/apps/%s/manifest.json", base, id);
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    if (!http_get_file(url, path, 8192, 0, false, "manifest.json")) {
        set_state(NV_STORE_ERROR, "Download failed (manifest)"); return false;
    }

    // Precompiled image: nv_wasm runs app.aot instead of app.wasm when it exists (and falls back to
    // the .wasm when this firmware's runtime rejects it). A leftover from an older version would
    // shadow the new module, so without a fresh one the old one goes. Never fatal.
    snprintf(path, sizeof path, "%s/app.aot", dir);
    if (e->aot_size > 0 && has_module) {
        snprintf(url, sizeof url, "%s/apps/%s/app.aot", base, id);
        if (!http_get_file(url, path, kMaxAot, kAotMagic, false, "app.aot")) {
            NV_LOGW(TAG, "install: app.aot fetch failed, the app runs interpreted");
            unlink(path);
        }
    } else {
        unlink(path);
    }

    // Compressed icon for the launcher tile (icon.z) — never fatal; a stale one goes.
    snprintf(path, sizeof path, "%s/icon.z", dir);
    if (e->icon_z > 0) {
        snprintf(url, sizeof url, "%s/apps/%s/icon.z", base, id);
        if (!http_get_file(url, path, kMaxIconZ, 0, false, "icon.z")) { NV_LOGW(TAG, "install: icon fetch failed (ignored)"); unlink(path); }
    } else {
        unlink(path);
    }

    // Optional uncompressed icon (older stores) — never fatal.
    if (e->has_icon && !e->icon_z) {
        snprintf(url,  sizeof url,  "%s/apps/%s/icon.argb", base, id);
        snprintf(path, sizeof path, "%s/icon.argb", dir);
        if (!http_get_file(url, path, kMaxIcon, 0, false, "icon.argb")) NV_LOGW(TAG, "install: icon fetch failed (ignored)");
    }

    return true;
}

// Download one package (dependencies are the caller's) into /sdcard/apps/<id>/.
bool install_package(const char *base, const nv_store_entry_t *e) {
    if (e->data) return install_data(base, e);
    const char *id = e->id;
    set_progress(0);
    NV_LOGI(TAG, "install '%s'%s from %s", id, e->library ? " (library)" : "", base);

    char dir[160], mpath[224];
    mkdir(kAppsDir, 0777);
    snprintf(dir, sizeof dir, "%s/%s", kAppsDir, id);
    mkdir(dir, 0777);
    snprintf(mpath, sizeof mpath, "%s/manifest.json", dir);
    category_write(dir, e->category);   // Start > Games and friends read it with the manifest

    if (!s_staged) s_staged = (Staged *)heap_caps_calloc(1, sizeof(Staged), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto *pkg = (nv_store_pkg::Package *)heap_caps_malloc(sizeof(nv_store_pkg::Package),
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_staged || !pkg) { free(pkg); set_state(NV_STORE_ERROR, "out of memory"); return false; }
    s_staged->n = 0;
    const PkgResult pr = fetch_package(base, e, pkg);
    if (pr == PKG_BAD) {
        free(pkg);
        set_state(NV_STORE_ERROR, "Package signature invalid - not installed");
        return false;
    }
    if (pr == PKG_MISSING && !nv_config_get_bool("store_unsigned", false)) {
        nv_seclog_add(NV_SEC_APP_REFUSED, id);
        free(pkg);
        set_state(NV_STORE_ERROR, "Unsigned app - not installed");
        return false;
    }
    if (pr == PKG_MISSING) NV_LOGW(TAG, "install '%s': UNSIGNED (developer mode)", id);
    s_pkg = pr == PKG_OK ? pkg : nullptr;
    bool ok = install_files(base, e, dir);
    if (ok && s_pkg && !stage_commit(mpath)) {
        set_state(NV_STORE_ERROR, "Install failed (SD write)");
        ok = false;
    }
    if (!ok) stage_abort();
    s_pkg = nullptr;
    free(pkg);
    if (!ok) return false;

    // Validate what landed + refresh this row's installed/update flags in the snapshot.
    nv_wasm_app_t chk;
    if (!nv_wasm_load_manifest(id, &chk)) { set_state(NV_STORE_ERROR, "Installed files are invalid"); return false; }
    lock();
    for (int i = 0; i < s_cat_n; i++) if (!strcmp(s_cat[i].id, id)) {
        s_cat[i].installed = true;
        s_cat[i].update    = version_is_newer(s_cat[i].version, chk.version);
        break;
    }
    unlock();
    NV_LOGI(TAG, "installed '%s' v%s", id, chk.version);
    return true;
}

bool catalog_row(const char *id, nv_store_entry_t *out) {
    bool found = false;
    lock();
    for (int i = 0; i < s_cat_n && !found; i++)
        if (!strcmp(s_cat[i].id, id)) { *out = s_cat[i]; found = true; }
    unlock();
    return found;
}

// Scratch records for plan_install, off the worker's small internal stack (it recurses).
struct PlanScratch { nv_store_entry_t row, dep; nv_wasm_app_t local; };

// Install order for `id`: every package it requires that is missing or older than required, depth
// first, then `id` itself. Reads only the catalog and the card (no network). False, with the state
// set to ERROR, when a requirement can't be met: a system component too old, a package the store
// lacks or only has too old, a chain too deep (or circular), or too many packages.
bool plan_install(const char *id, int depth, char (*plan)[32], int *n) {
    for (int i = 0; i < *n; i++) if (!strcmp(plan[i], id)) return true;    // already planned
    if (depth > kMaxDepDepth) { set_state(NV_STORE_ERROR, "Dependency chain too deep"); return false; }
    auto *sc = (PlanScratch *)heap_caps_malloc(sizeof(PlanScratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sc) { set_state(NV_STORE_ERROR, "out of memory"); return false; }
    char m[96];
    bool ok = catalog_row(id, &sc->row);
    if (!ok) {
        snprintf(m, sizeof m, nv_tr(NV_STR_DEP_PACKAGE_FMT), id, "");
        set_state(NV_STORE_ERROR, m);
    }
    const int nd = ok ? sc->row.n_deps : 0;
    for (int k = 0; k < nd && ok; k++) {
        char dep[32], want[12];
        snprintf(dep, sizeof dep, "%s", sc->row.deps[k].id);
        snprintf(want, sizeof want, "%s", sc->row.deps[k].version);
        if (const char *sys = nv_wasm_sys_component(dep)) {
            if (!nv_wasm_version_ge(sys, want)) {
                snprintf(m, sizeof m, nv_tr(NV_STR_DEP_SYSTEM_FMT), nv_wasm_dep_name(dep), want);
                set_state(NV_STORE_ERROR, m);
                ok = false;
            }
            continue;
        }
        if (nv_wasm_load_manifest(dep, &sc->local) && nv_wasm_version_ge(sc->local.version, want)) continue;
        if (!catalog_row(dep, &sc->dep) || !nv_wasm_version_ge(sc->dep.version, want)) {
            snprintf(m, sizeof m, nv_tr(NV_STR_DEP_PACKAGE_FMT), dep, want);
            set_state(NV_STORE_ERROR, m);
            ok = false;
            continue;
        }
        ok = plan_install(dep, depth + 1, plan, n);
    }
    heap_caps_free(sc);
    if (!ok) return false;
    if (*n >= kMaxPlan) { set_state(NV_STORE_ERROR, "Too many dependencies"); return false; }
    snprintf(plan[(*n)++], 32, "%s", id);
    return true;
}

// Installs `id` and what it requires. True when `id` itself landed; *was_update says whether an
// older version of it was on the card (the install counter tells installs and updates apart).
bool do_install(const char *base, const char *id, bool *was_update) {
    if (!nv_sd_is_mounted()) { set_state(NV_STORE_ERROR, "No SD card"); return false; }
    set_progress(0);
    set_state(NV_STORE_INSTALLING, "Downloading...");
    char plan[kMaxPlan][32];
    int n = 0;
    if (!plan_install(id, 0, plan, &n)) return false;
    // Uninstall refuses while the app runs; install/update must too — replacing app.wasm, the
    // manifest and the assets under a running module is at best inconsistent. That holds for every
    // package of the plan. A finished run parked in DONE (its screen already closed) is collected
    // first instead of blocking.
    nv_wasm_exec_collect(nullptr, nullptr, nullptr, 0);
    if (nv_wasm_exec_state() != NV_WRUN_IDLE)
        for (int i = 0; i < n; i++)
            if (!strcmp(nv_wasm_exec_app_id(), plan[i])) {
                set_state(NV_STORE_ERROR, "App is running — close it first"); return false;
            }

    auto *e = (nv_store_entry_t *)heap_caps_malloc(sizeof(nv_store_entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!e) { set_state(NV_STORE_ERROR, "out of memory"); return false; }
    bool done = false;
    for (int i = 0; i < n; i++) {
        if (!catalog_row(plan[i], e)) { set_state(NV_STORE_ERROR, "Catalog changed"); break; }
        char m[96];
        if (i + 1 < n) {                                   // a dependency: say which one
            snprintf(m, sizeof m, nv_tr(NV_STR_STORE_DEP_INST_FMT), e->name);
            set_state(NV_STORE_INSTALLING, m);
        } else {
            set_state(NV_STORE_INSTALLING, "Downloading...");
        }
        const bool had = e->installed;
        if (!install_package(base, e)) break;
        // "origin": this package came from the store, so its (public) id may be counted by the
        // opt-in statistics; a side-loaded app has no such file.
        char mark[80];
        snprintf(mark, sizeof mark, "%s/%s/origin", kAppsDir, e->id);
        if (FILE *f = fopen(mark, "w")) { fputs("store\n", f); fclose(f); }
        if (i + 1 == n) {
            snprintf(m, sizeof m, "Installed %s v%s", e->name, e->version);
            set_state(NV_STORE_READY, m);
            *was_update = had;
            done = true;
        }
    }
    heap_caps_free(e);
    return done;
}

// Count one install/update of `id` (see nv_appstore_stats_enabled). Best effort: a short timeout,
// the answer is ignored. Only the public store's installs count: a developer's local store
// (another store_url) would otherwise inflate the public numbers.
void stats_ping(const char *base, const char *id, bool update) {
    if (!nv_appstore_stats_enabled() || strcmp(base, kDefaultUrl) != 0 || !id_ok(id)) return;
    char url[96];
    snprintf(url, sizeof url, "%s/%c/%s", kStatsUrl, update ? 'u' : 'i', id);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 4000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return;
    const esp_err_t err = esp_http_client_perform(c);
    NV_LOGI(TAG, "stats %s %s: %s %d", update ? "update" : "install", id, esp_err_to_name(err),
            err == ESP_OK ? esp_http_client_get_status_code(c) : 0);
    esp_http_client_cleanup(c);
}

void worker(void *) {
    // snapshot the job under the lock (caller filled it before creating us)
    JobKind kind; char id[32], base[192];
    lock();
    kind = s_job_kind;
    snprintf(id,   sizeof id,   "%s", s_job_id);
    snprintf(base, sizeof base, "%s", s_job_base);
    unlock();

    if (kind == JOB_FETCH) {
        do_fetch(base);
    } else if (kind == JOB_PLATFORM) {
        lock(); const int part = s_pl.job_part; unlock();
        do_platform(base, id, part);
    } else if (kind == JOB_CONTENT) {
        install_content(base, id);
    } else if (kind == JOB_CONTENT_INDEX) {
        fetch_content_index(base);
    } else if (kind == JOB_CONTENT_VERIFY) {
        verify_content(id);
    } else {
        bool update = false;
        const bool ok = do_install(base, id, &update);
        char variant[9];
        lock(); snprintf(variant, sizeof variant, "%s", s_job_variant); unlock();
        if (ok && variant[0] && !write_variant(id, variant))
            NV_LOGW(TAG, "install %s: could not write data/variant", id);
        // Done for the UI; the counter ping runs after, while busy() still holds off another job
        // (two workers must never share the job state).
        lock(); s_installing[0] = '\0'; s_pinging = ok; unlock();
        if (ok) {
            nv_telemetry_store(update ? NV_TL_STORE_UPDATE : NV_TL_STORE_INSTALL);
            stats_ping(base, id, update);
        }
    }

    lock(); s_installing[0] = '\0'; s_pinging = false; unlock();
    vTaskDeleteWithCaps(nullptr);   // PSRAM stack (see spawn_worker)
}

// ---- store icons ---------------------------------------------------------------------------------
// A compressed cache (icon.z bodies, ~1-2 KB each) filled by a background task in the order the UI
// asks. Slots are reused least-recently-wanted first; a body is freed only when its slot is.
enum IconSt : uint8_t { IC_FREE, IC_QUEUED, IC_FETCHING, IC_READY, IC_FAILED };
struct IconSlot { char id[32]; uint8_t *z; uint16_t len; IconSt st; uint32_t wanted; };
NV_PSRAM_BSS IconSlot s_icons[NV_STORE_MAX];
uint32_t s_icon_tick = 0;                 // want() counter, for least-recently-wanted reuse
bool     s_icon_running = false;          // the fetch task is alive
char     s_icon_base[192] = "";
tinfl_decompressor *s_tinfl = nullptr;    // ~11 KB, PSRAM, used under the lock

// ---- store screenshots ---------------------------------------------------------------------------
// One app's screenshots at a time (the app page on screen), as fetched JPEG bodies in PSRAM. A
// want() for another app bumps the generation: the running fetch drops what it gets and starts over.
constexpr int kShotCap = 96 * 1024;              // one screenshot (a real 512x300 one is ~40 KB)
enum ShotSt : int8_t { SH_NONE = -1, SH_WAIT = 0, SH_READY = 1 };
struct Shots {
    char     id[32];
    int      n;                                  // offered by the catalog
    uint8_t *jpg[NV_STORE_SHOTS_MAX];
    size_t   len[NV_STORE_SHOTS_MAX];
    ShotSt   st[NV_STORE_SHOTS_MAX];
    uint32_t gen;
    bool     running;
    char     base[192];
};
Shots s_shots = {};

void shots_free_locked() {
    for (int k = 0; k < NV_STORE_SHOTS_MAX; k++) {
        if (s_shots.jpg[k]) heap_caps_free(s_shots.jpg[k]);
        s_shots.jpg[k] = nullptr;
        s_shots.len[k] = 0;
        s_shots.st[k] = SH_NONE;
    }
}

void shots_worker(void *) {
    char *buf = (char *)heap_caps_malloc(kShotCap + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (;;) {
        char id[32], base[192];
        int k = -1;
        uint32_t gen;
        lock();
        for (int i = 0; i < s_shots.n && k < 0; i++) if (s_shots.st[i] == SH_WAIT && !s_shots.jpg[i]) k = i;
        if (k < 0 || !buf) { s_shots.running = false; unlock(); break; }
        snprintf(id, sizeof id, "%s", s_shots.id);
        snprintf(base, sizeof base, "%s", s_shots.base);
        gen = s_shots.gen;
        unlock();

        char url[288];
        snprintf(url, sizeof url, "%s/shots/%s/%d.jpg", base, id, k + 1);
        const int got = http_get_buf(url, buf, kShotCap + 1);
        uint8_t *copy = got > 0 ? (uint8_t *)heap_caps_malloc((size_t)got, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
        if (copy) memcpy(copy, buf, (size_t)got);
        lock();
        if (gen != s_shots.gen) { unlock(); heap_caps_free(copy); continue; }   // another app since
        if (copy) { s_shots.jpg[k] = copy; s_shots.len[k] = (size_t)got; s_shots.st[k] = SH_READY; }
        else      { s_shots.st[k] = SH_NONE; NV_LOGW(TAG, "screenshot %s/%d: download failed", id, k + 1); }
        unlock();
    }
    heap_caps_free(buf);
    vTaskDeleteWithCaps(nullptr);   // PSRAM stack (see spawn_worker)
}

// Raw deflate -> NV_STORE_ICON_BYTES of ARGB8888 (caller holds the lock: one shared decompressor).
bool inflate_icon(const uint8_t *z, size_t len, uint8_t *argb) {
    if (!z || !len || !argb) return false;
    if (!s_tinfl) s_tinfl = (tinfl_decompressor *)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
    if (!s_tinfl) return false;
    size_t in_len = len, out_len = NV_STORE_ICON_BYTES;
    tinfl_init(s_tinfl);
    const tinfl_status st = tinfl_decompress(s_tinfl, z, &in_len, argb, argb, &out_len,
                                             TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return st == TINFL_STATUS_DONE && out_len == NV_STORE_ICON_BYTES;
}

IconSlot *icon_slot(const char *id) {
    for (IconSlot &s : s_icons) if (s.st != IC_FREE && !strcmp(s.id, id)) return &s;
    return nullptr;
}

void icon_worker(void *) {
    char *buf = (char *)heap_caps_malloc(kMaxIconZ + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (;;) {
        char id[32] = "", base[192];
        lock();
        IconSlot *next = nullptr;
        for (IconSlot &s : s_icons)   // most recently wanted first: the page on screen now
            if (s.st == IC_QUEUED && (!next || s.wanted > next->wanted)) next = &s;
        if (next && buf) {
            next->st = IC_FETCHING;
            snprintf(id, sizeof id, "%s", next->id);
        } else {
            s_icon_running = false;
        }
        snprintf(base, sizeof base, "%s", s_icon_base);
        unlock();
        if (!id[0]) break;

        char url[320];
        snprintf(url, sizeof url, "%s/apps/%s/icon.z", base, id);
        const int n = http_get_buf(url, buf, kMaxIconZ + 1);
        uint8_t *z = n > 0 ? (uint8_t *)heap_caps_malloc((size_t)n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
        if (z) memcpy(z, buf, (size_t)n);
        lock();
        IconSlot *s = icon_slot(id);
        if (s && s->st == IC_FETCHING) {
            if (s->z) heap_caps_free(s->z);
            s->z = z;
            s->len = z ? (uint16_t)n : 0;
            s->st = z ? IC_READY : IC_FAILED;
            z = nullptr;
        }
        unlock();
        if (z) heap_caps_free(z);   // the slot was reused meanwhile
    }
    heap_caps_free(buf);
    vTaskDeleteWithCaps(nullptr);   // PSRAM stack (see spawn_worker)
}

// Ensure the one-time state (lock + catalog buffer) exists. Returns false on OOM.
bool ensure_init() {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_cat) {
        s_cat = (nv_store_entry_t *)heap_caps_calloc(NV_STORE_MAX, sizeof(nv_store_entry_t),
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_cat) s_cat = (nv_store_entry_t *)calloc(NV_STORE_MAX, sizeof(nv_store_entry_t));
    }
    return s_lock && s_cat;
}

bool busy() {
    lock();
    const bool b = (s_state == NV_STORE_FETCHING || s_state == NV_STORE_INSTALLING || s_pinging);
    unlock();
    return b;
}

// Capture the store base URL into the job (called on the caller thread, before the worker starts).
void capture_base() {
    char url[192];
    nv_appstore_get_url(url, sizeof url);
    // strip one trailing slash so "{base}/store.json" never doubles up
    size_t n = strlen(url);
    if (n && url[n - 1] == '/') url[n - 1] = '\0';
    lock(); snprintf(s_job_base, sizeof s_job_base, "%s", url); unlock();
}

bool spawn_worker() {
    // 12 KB PSRAM stack: an https:// store means a TLS handshake + cert-bundle verify (~8-10 KB)
    // on this stack. No store task touches flash (settings go through nv_config's proxy, packages
    // to the SD card), so all four take PSRAM stacks and end with vTaskDeleteWithCaps: with the
    // icon and screenshot fetchers running beside a job they used to pin up to 42 KB of SRAM.
    return xTaskCreateWithCaps(worker, "store", 12288, nullptr, 4, nullptr, MALLOC_CAP_SPIRAM) == pdPASS;
}

// ---- system apps --------------------------------------------------------------------------------
// nv_wasm_is_system_app: packages the OS relies on. A background task installs the missing ones
// from the (signed) store through the public API, one job at a time, never competing with the UI:
// it only starts a job when the store is idle. Once per boot it also updates the installed ones the
// catalog has a newer version of: a system tool (ANIMA's math, dates, PDF...) must not go stale.
bool s_sys_running = false;

bool in_catalog(const char *id) {
    bool found = false;
    lock();
    for (int i = 0; i < s_cat_n && !found; i++) found = !strcmp(s_cat[i].id, id);
    unlock();
    return found;
}

// The catalog offers a newer version than the installed one (flag set when the catalog loads).
bool catalog_has_update(const char *id) {
    bool upd = false;
    lock();
    for (int i = 0; i < s_cat_n; i++) if (!strcmp(s_cat[i].id, id)) { upd = s_cat[i].update; break; }
    unlock();
    return upd;
}

bool wait_idle(int ms);

// Updates the installed system apps that have a newer version in the store (once per boot).
void update_system_apps(const char *const *ids, int n) {
    if (!wait_idle(10 * 60 * 1000)) return;
    nv_appstore_refresh();                                       // fresh catalog: versions to compare
    vTaskDelay(pdMS_TO_TICKS(200));
    if (!wait_idle(2 * 60 * 1000)) return;
    for (int i = 0; i < n; i++) {
        if (!nv_sd_is_mounted() || !catalog_has_update(ids[i])) continue;
        if (!wait_idle(10 * 60 * 1000)) return;
        NV_LOGI(TAG, "system app '%s': newer version in the store, updating it", ids[i]);
        if (!nv_appstore_install(ids[i])) continue;
        vTaskDelay(pdMS_TO_TICKS(200));
        wait_idle(15 * 60 * 1000);
    }
}

// Wait until no job runs (at most `ms`). True when idle.
bool wait_idle(int ms) {
    for (; ms > 0 && busy(); ms -= 500) vTaskDelay(pdMS_TO_TICKS(500));
    return !busy();
}

void system_task(void *) {
    // After boot's Wi-Fi join and the auto-OTA check (an update reboots anyway).
    vTaskDelay(pdMS_TO_TICKS(90 * 1000));
    const char *const *ids = nullptr;
    const int n = nv_wasm_system_apps(&ids);
    auto *local = (nv_wasm_app_t *)heap_caps_malloc(sizeof(nv_wasm_app_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (int round = 0; local && round < 48; round++) {          // 5-min retries, ~4 h
        int missing = 0;
        bool fetched = false;
        for (int i = 0; i < n; i++) {
            if (!nv_sd_is_mounted()) { missing = -1; break; }
            if (nv_wasm_load_manifest(ids[i], local)) continue;
            missing++;
            if (!wait_idle(10 * 60 * 1000)) continue;
            if (!in_catalog(ids[i]) && !fetched) {                // one catalog fetch per round
                fetched = true;
                nv_appstore_refresh();
                vTaskDelay(pdMS_TO_TICKS(200));
                wait_idle(2 * 60 * 1000);
            }
            if (!in_catalog(ids[i]) || !wait_idle(10 * 60 * 1000)) continue;
            NV_LOGI(TAG, "system app '%s' missing: installing it", ids[i]);
            if (!nv_appstore_install(ids[i])) continue;
            vTaskDelay(pdMS_TO_TICKS(200));
            wait_idle(15 * 60 * 1000);
            if (nv_wasm_load_manifest(ids[i], local)) missing--;
            else NV_LOGW(TAG, "system app '%s': install failed (%s)", ids[i], nv_appstore_message());
        }
        if (missing == 0) break;
        vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
    }
    heap_caps_free(local);
    update_system_apps(ids, n);
    lock(); s_sys_running = false; unlock();
    vTaskDeleteWithCaps(nullptr);   // PSRAM stack (see spawn_worker)
}

}  // namespace

// ---- public API ---------------------------------------------------------------------------------

void nv_appstore_get_url(char *out, size_t n) {
    nv_config_get_str("store_url", kDefaultUrl, out, n);
    if (n && !out[0]) snprintf(out, n, "%s", kDefaultUrl);   // empty NVS value -> default
}
void nv_appstore_set_url(const char *url) { nv_config_set_str("store_url", url ? url : ""); }

void nv_appstore_get_region(char *out, size_t n) {
    nv_config_get_str("store_region", "", out, n);   // "" = worldwide (server infers from lang)
}
void nv_appstore_set_region(const char *region) {
    nv_config_set_str("store_region", region ? region : "");
}
// One consent for every statistic (nv_telemetry): the install counter included.
bool nv_appstore_stats_enabled(void) { return nv_telemetry_consent() == NV_TELEMETRY_YES; }
void nv_appstore_set_stats_enabled(bool on) { nv_telemetry_set_consent(on); }

nv_store_state_t nv_appstore_state(void) { lock(); auto s = s_state; unlock(); return s; }

const char *nv_appstore_message(void) {
    static char m[96];
    lock(); snprintf(m, sizeof m, "%s", s_msg); unlock();
    return m;
}
int nv_appstore_progress(void) { lock(); int p = s_progress; unlock(); return p; }

const char *nv_appstore_installing_id(void) {
    static char id[32];
    lock(); snprintf(id, sizeof id, "%s", s_installing); unlock();
    return id;
}

void nv_appstore_refresh(void) {
    if (!ensure_init() || busy()) return;
    capture_base();
    lock(); s_job_kind = JOB_FETCH; s_job_id[0] = '\0'; unlock();
    if (!spawn_worker()) set_state(NV_STORE_ERROR, "Could not start fetch");
}

int nv_appstore_count(void) { lock(); int n = s_cat_n; unlock(); return n; }

uint32_t nv_appstore_catalog_gen(void) {
    if (!ensure_init()) return 0;
    lock(); const uint32_t g = s_cat_gen; unlock();
    return g;
}

void nv_appstore_forget_installed(const char *id) {
    if (!id || !ensure_init()) return;
    lock();
    for (int i = 0; i < s_cat_n; i++)
        if (!strcmp(s_cat[i].id, id)) { s_cat[i].installed = false; s_cat[i].update = false; }
    unlock();
}

int nv_appstore_updates(char *names, size_t n, uint32_t *sig) {
    if (names && n) names[0] = '\0';
    uint32_t h = 2166136261u;   // FNV-1a over "id@version;" of each row with an update
    int count = 0;
    if (ensure_init()) {
        lock();
        size_t len = 0;
        for (int i = 0; i < s_cat_n; i++) {
            const nv_store_entry_t &e = s_cat[i];
            // a module for a newer OS is not offered; a data pack has no module ("abi" 99)
            if (!e.installed || !e.update || (!e.data && e.abi > (uint32_t)NV_WASM_ABI)) continue;
            count++;
            for (const char *p = e.id; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
            h = (h ^ '@') * 16777619u;
            for (const char *p = e.version; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
            h = (h ^ ';') * 16777619u;
            if (names && n && len + 1 < n) {
                const int w = snprintf(names + len, n - len, "%s%s", count > 1 ? ", " : "", e.name);
                len = w < 0 ? len : (len + (size_t)w < n ? len + (size_t)w : n - 1);
            }
        }
        unlock();
    }
    if (sig) *sig = count ? h : 0;
    return count;
}

bool nv_appstore_get(int i, nv_store_entry_t *out) {
    if (!out) return false;
    bool ok = false;
    lock();
    if (i >= 0 && i < s_cat_n) { *out = s_cat[i]; ok = true; }
    unlock();
    return ok;
}

void nv_appstore_icons_want(const char *const *ids, int n) {
    if (!ids || n <= 0 || !ensure_init()) return;
    bool start = false;
    lock();
    const uint32_t tick = ++s_icon_tick;
    for (int k = 0; k < n; k++) {
        const char *id = ids[k];
        if (!id || !id_ok(id)) continue;
        bool offered = false;
        for (int i = 0; i < s_cat_n; i++)
            if (!strcmp(s_cat[i].id, id)) { offered = s_cat[i].icon_z > 0; break; }
        if (!offered) continue;
        IconSlot *s = icon_slot(id);
        if (!s) {   // a free slot, else the least recently wanted one that isn't in flight
            for (IconSlot &c : s_icons)
                if (c.st == IC_FREE) { s = &c; break; }
            if (!s)
                for (IconSlot &c : s_icons)
                    if (c.st != IC_FETCHING && c.wanted != tick && (!s || c.wanted < s->wanted)) s = &c;
            if (!s) continue;
            if (s->z) heap_caps_free(s->z);
            memset(s, 0, sizeof *s);
            snprintf(s->id, sizeof s->id, "%s", id);
            s->st = IC_QUEUED;
        }
        s->wanted = tick;
        if (s->st == IC_QUEUED) start = true;
    }
    if (start && !s_icon_running) {
        char url[192];
        unlock();
        nv_appstore_get_url(url, sizeof url);   // NVS read: not under our lock
        lock();
        size_t len = strlen(url);
        if (len && url[len - 1] == '/') url[len - 1] = '\0';
        snprintf(s_icon_base, sizeof s_icon_base, "%s", url);
        // PSRAM stack like the install worker (http + a TLS handshake for https:// stores).
        s_icon_running = xTaskCreateWithCaps(icon_worker, "store_ic", 12288, nullptr, 3, nullptr,
                                             MALLOC_CAP_SPIRAM) == pdPASS;
    }
    unlock();
}

void nv_appstore_shots_want(const char *id) {
    if (!id || !id_ok(id) || !ensure_init()) return;
    int n = 0;
    lock();
    for (int i = 0; i < s_cat_n; i++) if (!strcmp(s_cat[i].id, id)) { n = s_cat[i].shots; break; }
    const bool same = !strcmp(s_shots.id, id);
    unlock();
    if (same || !n) return;
    char url[192];
    nv_appstore_get_url(url, sizeof url);        // NVS read: not under our lock
    size_t ul = strlen(url);
    if (ul && url[ul - 1] == '/') url[ul - 1] = '\0';
    lock();
    shots_free_locked();
    snprintf(s_shots.id, sizeof s_shots.id, "%s", id);
    snprintf(s_shots.base, sizeof s_shots.base, "%s", url);
    s_shots.n = n;
    s_shots.gen++;
    for (int k = 0; k < n; k++) s_shots.st[k] = SH_WAIT;
    if (!s_shots.running)   // PSRAM stack like the icon fetcher (http + a TLS handshake)
        s_shots.running = xTaskCreateWithCaps(shots_worker, "store_sh", 12288, nullptr, 3, nullptr,
                                              MALLOC_CAP_SPIRAM) == pdPASS;
    if (!s_shots.running) for (int k = 0; k < n; k++) s_shots.st[k] = SH_NONE;
    unlock();
}

int nv_appstore_shot_get(const char *id, int k, const uint8_t **jpg, size_t *len) {
    if (!id || k < 1 || k > NV_STORE_SHOTS_MAX || !ensure_init()) return -1;
    int r = -1;
    lock();
    if (!strcmp(s_shots.id, id) && k <= s_shots.n) {
        r = s_shots.st[k - 1];
        if (r == SH_READY) { *jpg = s_shots.jpg[k - 1]; *len = s_shots.len[k - 1]; }
    }
    unlock();
    return r;
}

bool nv_appstore_icon_inflate(const uint8_t *z, size_t len, uint8_t *argb) {
    if (!ensure_init()) return false;
    lock();
    const bool ok = inflate_icon(z, len, argb);
    unlock();
    return ok;
}

bool nv_appstore_icon_get(const char *id, uint8_t *argb) {
    if (!id || !argb || !ensure_init()) return false;
    lock();
    const IconSlot *s = icon_slot(id);
    const bool ok = s && s->st == IC_READY && inflate_icon(s->z, s->len, argb);
    unlock();
    return ok;
}

void nv_appstore_system_start(void) {
    if (!ensure_init()) return;
    lock();
    const bool start = !s_sys_running;
    s_sys_running = true;
    unlock();
    if (!start) return;
    // PSRAM stack: short-lived, self-deleting, reads the SD card (manifests).
    if (xTaskCreateWithCaps(system_task, "store_sys", 6144, nullptr, 3, nullptr, MALLOC_CAP_SPIRAM) != pdPASS) {
        lock(); s_sys_running = false; unlock();
    }
}

bool nv_appstore_install(const char *id) { return nv_appstore_install_variant(id, nullptr); }

bool nv_appstore_install_variant(const char *id, const char *variant) {
    if (!id || !ensure_init() || busy()) return false;
    if (variant && variant[0] && !variant_ok(variant)) return false;
    // id must be one we actually advertise (defends the SD path against arbitrary input)
    bool known = false;
    lock();
    for (int i = 0; i < s_cat_n; i++) if (!strcmp(s_cat[i].id, id)) { known = true; break; }
    unlock();
    if (!known || !id_ok(id)) return false;

    capture_base();
    lock();
    s_job_kind = JOB_INSTALL;
    snprintf(s_job_id,     sizeof s_job_id,     "%s", id);
    snprintf(s_installing, sizeof s_installing, "%s", id);
    snprintf(s_job_variant, sizeof s_job_variant, "%s", variant ? variant : "");
    unlock();
    if (!spawn_worker()) { set_state(NV_STORE_ERROR, "Could not start install"); lock(); s_installing[0]=0; unlock(); return false; }
    return true;
}

void nv_appstore_variant_get(const char *id, char *out, size_t n) {
    if (!out || !n) return;
    out[0] = 0;
    if (!id || !id_ok(id)) return;
    char p[96];
    snprintf(p, sizeof p, "%s/%s/data/variant", kAppsDir, id);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char v[16] = "";
    const size_t r = fread(v, 1, sizeof v - 1, f);
    fclose(f);
    v[r] = 0;
    if (variant_ok(v)) snprintf(out, n, "%s", v);
}

bool nv_appstore_variant_set(const char *id, const char *variant) {
    if (!id || !id_ok(id) || !variant_ok(variant)) return false;
    return write_variant(id, variant);
}

int nv_appstore_category_count(void) { lock(); const int n = s_cats_n; unlock(); return n; }

int nv_appstore_platform_count(void) { if (!ensure_init()) return 0; lock(); const int n = s_pl.n; unlock(); return n; }

bool nv_appstore_platform_get(int i, nv_store_platform_t *out) {
    if (!out || !ensure_init()) return false;
    bool ok = false;
    lock();
    if (i >= 0 && i < s_pl.n) { *out = s_pl.p[i]; ok = true; }
    unlock();
    return ok;
}

bool nv_appstore_platform_open(const char *id, int part) {
    if (!id || !id_ok(id) || !ensure_init() || busy()) return false;
    bool known = false;
    lock();
    for (int i = 0; i < s_pl.n && !known; i++)
        known = !strcmp(s_pl.p[i].id, id) && part >= 1 && part <= s_pl.p[i].parts;
    unlock();
    if (!known) return false;
    capture_base();
    lock();
    s_job_kind = JOB_PLATFORM;
    snprintf(s_job_id, sizeof s_job_id, "%s", id);
    s_pl.job_part = part;
    s_state = NV_STORE_FETCHING;   // busy() from now on: no second job before the worker starts
    unlock();
    if (!spawn_worker()) { set_state(NV_STORE_ERROR, "Could not start fetch"); return false; }
    return true;
}

int nv_appstore_platform_loaded(char *id, size_t n) {
    if (!ensure_init()) { if (id && n) id[0] = 0; return 0; }
    lock();
    if (id && n) snprintf(id, n, "%s", s_pl.loaded);
    const int part = s_pl.loaded[0] ? s_pl.part : 0;
    unlock();
    return part;
}

int nv_appstore_platform_search(int i, const char *query, int *first) {
    if (first) *first = -1;
    if (!query || !query[0] || !ensure_init()) return 0;
    const size_t ql = strlen(query);
    int hits = 0;
    lock();
    const char *s = (i >= 0 && i < s_pl.n) ? s_pl.names[i] : nullptr;
    for (int k = 0; s && *s; k++) {
        const char *eol = strchr(s, '\n');
        const size_t len = eol ? (size_t)(eol - s) : strlen(s);
        for (size_t a = 0; a + ql <= len; a++) {
            size_t b = 0;
            while (b < ql && tolower((unsigned char)s[a + b]) == tolower((unsigned char)query[b])) b++;
            if (b == ql) { if (!hits++ && first) *first = k; break; }
        }
        s = eol ? eol + 1 : s + len;
    }
    unlock();
    return hits;
}

bool nv_appstore_category_get(int i, nv_store_category_t *out) {
    if (!out) return false;
    bool ok = false;
    lock();
    if (i >= 0 && i < s_cats_n) { *out = s_cats[i]; ok = true; }
    unlock();
    return ok;
}

void nv_appstore_set_data_hook(void (*hook)(const char *dest)) { s_data_hook = hook; }

bool nv_appstore_data_installed(const char *id) {
    return id_ok(id) && data_record_read(id, nullptr, 0, nullptr, 0, nullptr, 0) >= 0;
}

bool nv_appstore_content_install(const char *id) {
    if (!id || !id_ok(id) || strlen(id) > 31 || !ensure_init() || busy() || !nv_sd_is_mounted()) return false;
    capture_base();
    lock();
    s_job_kind = JOB_CONTENT;
    snprintf(s_job_id,     sizeof s_job_id,     "%s", id);
    snprintf(s_installing, sizeof s_installing, "%s", id);
    s_state = NV_STORE_INSTALLING;   // busy() from now on: no second job before the worker starts
    unlock();
    if (!spawn_worker()) {
        set_state(NV_STORE_ERROR, "Could not start install");
        lock(); s_installing[0] = 0; unlock();
        return false;
    }
    return true;
}

bool nv_appstore_content_installed(const char *id, char *version, size_t n) {
    if (version && n) version[0] = 0;
    return id && id_ok(id) && content_record_read(id, version, n, nullptr, 0);
}

void nv_appstore_content_recover(void) {
    if (nv_sd_is_mounted()) content_recover();
}

void nv_appstore_set_content_hook(void (*hook)(const char *dest, const char *name, bool before)) { s_content_hook = hook; }

namespace {
bool start_content_job(JobKind k, const char *id) {
    if (!ensure_init() || busy()) return false;
    capture_base();
    lock();
    s_job_kind = k;
    snprintf(s_job_id, sizeof s_job_id, "%s", id ? id : "");
    s_state = k == JOB_CONTENT_INDEX ? NV_STORE_FETCHING : NV_STORE_INSTALLING;   // busy() from now on
    unlock();
    if (!spawn_worker()) { set_state(NV_STORE_ERROR, "Could not start"); return false; }
    return true;
}
}  // namespace

bool nv_appstore_content_refresh(void) { return start_content_job(JOB_CONTENT_INDEX, nullptr); }

int nv_appstore_content_count(void) { if (!ensure_init()) return 0; lock(); cidx_relang_locked(); const int n = s_cidx_n; unlock(); return n; }

bool nv_appstore_content_get(int i, nv_content_entry_t *out) {
    if (!out || !ensure_init()) return false;
    bool ok = false;
    lock();
    cidx_relang_locked();
    if (s_cidx && i >= 0 && i < s_cidx_n) { *out = s_cidx[i]; ok = true; }
    unlock();
    return ok;
}

uint32_t nv_appstore_content_gen(void) { if (!ensure_init()) return 0; lock(); const uint32_t g = s_cidx_gen; unlock(); return g; }

bool nv_appstore_content_verify(const char *id) {
    if (!id || !id_ok(id) || strlen(id) > 31 || !nv_sd_is_mounted()) return false;
    return start_content_job(JOB_CONTENT_VERIFY, id);
}

int nv_appstore_content_verify_result(const char **id, int *bad) {
    if (!ensure_init()) return -1;
    lock();
    const int r = s_verify_res;
    if (id) *id = s_verify_id;
    if (bad) *bad = s_verify_bad;
    unlock();
    return r;
}

void nv_appstore_content_sweep(uint32_t max_age_s, const char *const *keep) {
    if (!nv_sd_is_mounted() || busy()) return;
    const time_t now = time(nullptr);
    if (now < 1672531200) return;                           // the clock isn't set: ages mean nothing
    for (int i = 0; nv_store_pkg::kContentDests[i]; i++) {
        char d[64];
        snprintf(d, sizeof d, "%s/%s", kSdRoot, nv_store_pkg::kContentDests[i]);
        if (strcmp(nv_store_pkg::kContentDests[i], "web")) sweep_parts(d, now, max_age_s, 1);
    }
    DIR *st = opendir(kContentStage);
    if (!st) return;
    char p[200];
    while (struct dirent *e = readdir(st)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        bool kept = false;
        for (int k = 0; keep && keep[k]; k++) kept |= !strcmp(keep[k], e->d_name);
        if (snprintf(p, sizeof p, "%s/%s", kContentStage, e->d_name) >= (int)sizeof p) continue;
        if (!kept) { NV_LOGW(TAG, "content: dropping the downloads of '%s' (not queued)", e->d_name); rm_tree(p); }
        else sweep_parts(p, now, max_age_s, 0);
    }
    closedir(st);
}

bool nv_appstore_data_uninstall(const char *id) {
    if (!id_ok(id) || busy()) return false;
    char dest[24];
    char (*names)[nv_store_pkg::kDataNameMax] =
        (char (*)[nv_store_pkg::kDataNameMax])heap_caps_malloc(nv_store_pkg::kDataMax * nv_store_pkg::kDataNameMax,
                                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!names) return false;
    const int n = data_record_read(id, nullptr, 0, dest, sizeof dest, names, nv_store_pkg::kDataMax);
    if (n < 0) { free(names); return false; }
    bool dest_ok = false;                                        // the record is ours, but check anyway
    for (int i = 0; nv_store_pkg::kDataDests[i]; i++) dest_ok |= !strcmp(dest, nv_store_pkg::kDataDests[i]);
    char p[192];
    for (int i = 0; dest_ok && i < n; i++) {
        if (strchr(names[i], '/') || !strcmp(names[i], "..")) continue;
        snprintf(p, sizeof p, "%s/%s/%s", kDataDir, dest, names[i]);
        unlink(p);
        snprintf(p, sizeof p, "%s/%s/%s.part", kDataDir, dest, names[i]);
        unlink(p);
    }
    free(names);
    snprintf(p, sizeof p, "%s/%s.pack", kPacksDir, id);
    const bool ok = unlink(p) == 0;
    nv_appstore_forget_installed(id);
    if (ok && s_data_hook) s_data_hook(dest);
    NV_LOGI(TAG, "uninstalled data pack '%s'", id);
    return ok;
}

