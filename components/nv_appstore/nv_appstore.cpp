// nv_appstore — remote WASM app catalog + installer. See nv_appstore.h.
#include "nv_appstore.h"
#include "nv_log.h"
#include "nv_config.h"
#include "nv_sd.h"
#include "nv_wasm.h"      // nv_wasm_load_manifest — derive installed/update against the local card
#include "nv_i18n.h"      // active locale -> ?lang= so the store returns localized copy

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"   // https:// stores validate against the bundled root CAs
#include "esp_heap_caps.h"
#include "cJSON.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <sys/stat.h>

static const char *TAG = "store";

namespace {

// Same PC as the default OTA server, one port up. Change in Settings → the field persists to NVS.
constexpr char     kDefaultUrl[]  = "http://192.168.0.216:8090";
constexpr char     kAppsDir[]     = "/sdcard/apps";
constexpr long     kMaxWasm       = 2 * 1024 * 1024;   // 2 MB module cap (SD write + PSRAM run)
constexpr long     kMaxIcon       = 80 * 80 * 4;       // exactly one 80x80 ARGB8888 tile
constexpr int      kCatalogCap    = 32 * 1024;         // store.json ceiling (32 apps of metadata)
constexpr uint32_t kWasmMagic     = 0x6d736100;        // "\0asm" little-endian

SemaphoreHandle_t   s_lock = nullptr;
nv_store_state_t    s_state = NV_STORE_IDLE;
int                 s_progress = 0;
char                s_msg[96]  = "";
char                s_installing[32] = "";

nv_store_entry_t   *s_cat   = nullptr;    // PSRAM catalog snapshot
int                 s_cat_n = 0;

// pending job, filled by the caller before the worker task starts
enum JobKind { JOB_FETCH, JOB_INSTALL };
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
    int a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
    sscanf(cand, "%d.%d.%d", &a[0], &a[1], &a[2]);
    sscanf(cur,  "%d.%d.%d", &b[0], &b[1], &b[2]);
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

// ---- HTTP helpers -------------------------------------------------------------------------------

struct RespBuf { char *buf; int len; int cap; };
esp_err_t collect_evt(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data) {
        RespBuf *r = (RespBuf *)e->user_data;
        int n = e->data_len;
        if (r->len + n < r->cap - 1) { memcpy(r->buf + r->len, e->data, n); r->len += n; }
    }
    return ESP_OK;
}

// GET url into a caller buffer (NUL-terminated). Returns bytes on HTTP 200 + non-empty, else -1.
int http_get_buf(const char *url, char *out, int cap) {
    RespBuf rb = { out, 0, cap };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = collect_evt;
    cfg.user_data = &rb;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200 || rb.len == 0) return -1;
    out[rb.len] = '\0';
    return rb.len;
}

// Stream url to a temp file next to `path` then rename over it, so a failed download never leaves a
// half-written file for the scanner to trip on. When `magic`!=0 the first 4 bytes must match it
// (rejects an HTML error page served as app.wasm). `track` drives the install progress bar.
bool http_get_file(const char *url, const char *path, long max_bytes, uint32_t magic, bool track) {
    char tmp[224];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { NV_LOGE(TAG, "dl: init failed"); return false; }
    if (esp_http_client_open(c, 0) != ESP_OK) {
        NV_LOGE(TAG, "dl: open %s failed", url); esp_http_client_cleanup(c); return false;
    }
    const int total = esp_http_client_fetch_headers(c);   // <=0 when chunked / unknown
    if (total > 0 && (long)total > max_bytes) {
        NV_LOGE(TAG, "dl: %d bytes over cap %ld", total, max_bytes);
        esp_http_client_close(c); esp_http_client_cleanup(c); return false;
    }

    FILE *f = nv_sd_fopen(tmp, "wb");
    if (!f) {
        NV_LOGE(TAG, "dl: fopen('%s') errno=%d", tmp, errno);
        esp_http_client_close(c); esp_http_client_cleanup(c); return false;
    }

    char buf[2048]; int r; long done = 0; bool ok = true, first = true;
    while ((r = esp_http_client_read(c, buf, sizeof buf)) > 0) {
        if (first && magic) {
            uint32_t head = 0;
            if (r >= 4) memcpy(&head, buf, 4);
            if (r < 4 || head != magic) {
                NV_LOGE(TAG, "dl: bad magic (not a wasm module)"); ok = false; break;
            }
            first = false;
        }
        if (done + r > max_bytes) { NV_LOGE(TAG, "dl: exceeded cap mid-stream"); ok = false; break; }
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

    if (!ok || status != 200 || done <= 0) {
        unlink(tmp);
        NV_LOGE(TAG, "dl: failed url=%s status=%d done=%ld", url, status, done);
        return false;
    }
    if (rename(tmp, path) != 0) {
        // FAT rename won't overwrite an existing target — replace explicitly.
        unlink(path);
        if (rename(tmp, path) != 0) { NV_LOGE(TAG, "dl: rename -> %s errno=%d", path, errno); unlink(tmp); return false; }
    }
    return true;
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

// Parse a store.json body into the catalog snapshot, deriving installed/update from the local card.
// Returns the row count (0 is valid: an empty store), or -1 on a malformed document.
int parse_catalog(const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (!root) return -1;
    cJSON *apps = cJSON_GetObjectItem(root, "apps");
    if (!cJSON_IsArray(apps)) { cJSON_Delete(root); return -1; }

    int n = 0;
    const cJSON *it = nullptr;
    cJSON_ArrayForEach(it, apps) {
        if (n >= NV_STORE_MAX) break;
        if (!cJSON_IsObject(it)) continue;
        const char *id = jstr(it, "id", "");
        if (!id_ok(id)) { NV_LOGW(TAG, "catalog: bad id '%s' skipped", id); continue; }

        nv_store_entry_t *e = &s_cat[n];
        memset(e, 0, sizeof *e);
        snprintf(e->id,            sizeof e->id,            "%s", id);
        snprintf(e->name,          sizeof e->name,          "%s", jstr(it, "name", id));
        snprintf(e->version,       sizeof e->version,       "%s", jstr(it, "version", "?"));
        snprintf(e->author,        sizeof e->author,        "%s", jstr(it, "author", ""));
        snprintf(e->desc,          sizeof e->desc,          "%s", jstr(it, "description", ""));
        snprintf(e->category,      sizeof e->category,      "%s", jstr(it, "category", "other"));
        snprintf(e->category_name, sizeof e->category_name, "%s", jstr(it, "category_name", "Other"));
        e->abi      = ju32(it, "abi", 1);
        e->size     = ju32(it, "size", 0);
        e->is_game  = jbool(it, "game");
        e->has_icon = jbool(it, "icon");
        e->featured = jbool(it, "featured");
        const cJSON *jr = cJSON_GetObjectItem(it, "rating");
        e->rating10 = (cJSON_IsNumber(jr) && jr->valuedouble > 0)
                      ? (uint16_t)(jr->valuedouble * 10 + 0.5) : 0;

        nv_wasm_app_t local;
        if (nv_wasm_load_manifest(id, &local)) {
            e->installed = true;
            e->update    = version_is_newer(e->version, local.version);
        }
        n++;
    }
    cJSON_Delete(root);
    return n;
}

// ---- workers ------------------------------------------------------------------------------------

void do_fetch(const char *base) {
    set_state(NV_STORE_FETCHING, "Contacting store...");
    char region[16];
    nv_appstore_get_region(region, sizeof region);
    char url[320];
    // ?lang= localizes names/descriptions/categories; ?region= geolocates the catalog (omit when "*").
    if (region[0] && strcmp(region, "*") != 0)
        snprintf(url, sizeof url, "%s/store.json?lang=%s&region=%s", base, lang_code(), region);
    else
        snprintf(url, sizeof url, "%s/store.json?lang=%s", base, lang_code());

    char *body = (char *)heap_caps_malloc(kCatalogCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) body = (char *)malloc(kCatalogCap);
    if (!body) { set_state(NV_STORE_ERROR, "out of memory"); return; }

    const int got = http_get_buf(url, body, kCatalogCap);
    if (got < 0) {
        free(body);
        set_state(NV_STORE_ERROR, "Cannot reach store server");
        return;
    }

    lock();
    const int n = parse_catalog(body);
    if (n >= 0) s_cat_n = n;
    unlock();
    free(body);

    if (n < 0) { set_state(NV_STORE_ERROR, "Bad catalog (store.json)"); return; }
    char m[64];
    snprintf(m, sizeof m, n ? "%d app%s available" : "Store is empty", n, n == 1 ? "" : "s");
    set_state(NV_STORE_READY, m);
    NV_LOGI(TAG, "catalog: %d app(s) from %s", n, base);
}

void do_install(const char *base, const char *id) {
    if (!nv_sd_is_mounted()) { set_state(NV_STORE_ERROR, "No SD card"); return; }
    // find the advertised icon flag in the current snapshot (best-effort — install works without it)
    bool want_icon = false;
    lock();
    for (int i = 0; i < s_cat_n; i++) if (!strcmp(s_cat[i].id, id)) { want_icon = s_cat[i].has_icon; break; }
    unlock();

    set_progress(0);
    set_state(NV_STORE_INSTALLING, "Downloading...");
    NV_LOGI(TAG, "install '%s' from %s", id, base);

    char dir[160], url[320], path[224];
    mkdir(kAppsDir, 0777);
    snprintf(dir, sizeof dir, "%s/%s", kAppsDir, id);
    mkdir(dir, 0777);

    // app.wasm first (the big file, with the magic gate + progress) then manifest.json — the scanner
    // only accepts an app once both exist, so this order never surfaces a manifest without a module.
    snprintf(url,  sizeof url,  "%s/apps/%s/app.wasm", base, id);
    snprintf(path, sizeof path, "%s/app.wasm", dir);
    if (!http_get_file(url, path, kMaxWasm, kWasmMagic, true)) {
        set_state(NV_STORE_ERROR, "Download failed (app.wasm)"); return;
    }
    set_progress(100);

    snprintf(url,  sizeof url,  "%s/apps/%s/manifest.json", base, id);
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    if (!http_get_file(url, path, 8192, 0, false)) {
        set_state(NV_STORE_ERROR, "Download failed (manifest)"); return;
    }

    // Optional launcher icon — never fatal.
    if (want_icon) {
        snprintf(url,  sizeof url,  "%s/apps/%s/icon.argb", base, id);
        snprintf(path, sizeof path, "%s/icon.argb", dir);
        if (!http_get_file(url, path, kMaxIcon, 0, false)) NV_LOGW(TAG, "install: icon fetch failed (ignored)");
    }

    // Validate what landed + refresh this row's installed/update flags in the snapshot.
    nv_wasm_app_t chk;
    if (!nv_wasm_load_manifest(id, &chk)) { set_state(NV_STORE_ERROR, "Installed files are invalid"); return; }
    lock();
    for (int i = 0; i < s_cat_n; i++) if (!strcmp(s_cat[i].id, id)) {
        s_cat[i].installed = true;
        s_cat[i].update    = version_is_newer(s_cat[i].version, chk.version);
        break;
    }
    unlock();

    char m[96];
    snprintf(m, sizeof m, "Installed %s v%s", chk.name, chk.version);
    set_state(NV_STORE_READY, m);
    NV_LOGI(TAG, "installed '%s' v%s", id, chk.version);
}

void worker(void *) {
    // snapshot the job under the lock (caller filled it before creating us)
    JobKind kind; char id[32], base[192];
    lock();
    kind = s_job_kind;
    snprintf(id,   sizeof id,   "%s", s_job_id);
    snprintf(base, sizeof base, "%s", s_job_base);
    unlock();

    if (kind == JOB_FETCH) do_fetch(base);
    else                   do_install(base, id);

    lock(); s_installing[0] = '\0'; unlock();
    vTaskDelete(nullptr);
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
    const bool b = (s_state == NV_STORE_FETCHING || s_state == NV_STORE_INSTALLING);
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
    // 6 KB internal stack: short-lived, self-deleting, and writes the SD card (never a PSRAM stack).
    return xTaskCreate(worker, "store", 6144, nullptr, 4, nullptr) == pdPASS;
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

bool nv_appstore_get(int i, nv_store_entry_t *out) {
    if (!out) return false;
    bool ok = false;
    lock();
    if (i >= 0 && i < s_cat_n) { *out = s_cat[i]; ok = true; }
    unlock();
    return ok;
}

bool nv_appstore_install(const char *id) {
    if (!id || !ensure_init() || busy()) return false;
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
    unlock();
    if (!spawn_worker()) { set_state(NV_STORE_ERROR, "Could not start install"); lock(); s_installing[0]=0; unlock(); return false; }
    return true;
}
