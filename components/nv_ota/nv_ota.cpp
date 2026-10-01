// nv_ota — firmware updater, layout v2 (recovery + system). See nv_ota.h and docs/OTA.md.
#include "nv_ota.h"
#include "nv_fwup.h"
#include "nv_log.h"
#include "nv_seclog.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS
#include "nv_config.h"
#include "nv_sd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_ota_ops.h"
#include "esp_timer.h"     // deferred mark-valid (60 s survival gate)
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_vfs_fat.h"   // free space on the card
#include "cJSON.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <strings.h>   // strcasecmp

static const char *TAG = "ota";

namespace {

SemaphoreHandle_t s_lock = nullptr;
volatile nv_ota_state_t s_state = NV_OTA_IDLE;
volatile int  s_progress = 0;
volatile uint32_t s_gen  = 0;
char s_msg[160]      = "";
char s_avail_ver[32] = "";
NV_PSRAM_BSS char s_bin_url[256];
bool s_busy = false;   // a worker task is running

// What the last verified manifest promised (guarded by s_lock). Installed only if the staged image
// hashes to it; recovery checks it all again before touching the system slot.
NV_PSRAM_BSS nv_fwup_manifest_t s_expect;   // cold: read once per install
bool s_expect_set = false;

NV_PSRAM_BSS char s_boot_notice[128];   // what recovery did on the way to this boot, told once (UI)
bool s_boot_notice_taken = false;

void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

void set_state(nv_ota_state_t st, const char *msg) {
    lock();
    s_state = st;
    if (msg) snprintf(s_msg, sizeof(s_msg), "%s", msg);
    s_gen = s_gen + 1;   // not ++: volatile increment is deprecated in C++20
    unlock();
}
void set_progress(int p) {
    if (p < 0) p = 0; else if (p > 100) p = 100;
    if (p == s_progress) return;
    lock(); s_progress = p; s_gen = s_gen + 1; unlock();
}
void progress_cb(int pct, void *) { set_progress(pct); }

const char *running_version(void) {
    const esp_app_desc_t *d = esp_app_get_description();
    return d ? d->version : "?";
}

uint32_t slot_size(void) {
    const esp_partition_t *p = nv_fwup_system_part();
    return p ? (uint32_t)p->size : 0;
}

const char *mount(void) { return nv_sd_mount_point(); }

// ------------------------------------------------------------- manifest signature
bool manifest_verify(const char *json, const char *version, nv_fwup_manifest_t *out) {
    const nv_fwup_err_t e = nv_fwup_manifest_parse(json, slot_size(), out);
    if (e == NV_FWUP_OK && strcmp(out->version, version) == 0) return true;
    NV_LOGE(TAG, "manifest v%s refused: %s", version, nv_fwup_err_str(e));
    char d[NV_SECLOG_DETAIL_MAX];
    snprintf(d, sizeof d, "v%.20s %s", version, e == NV_FWUP_E_SIGNATURE ? "bad signature" : "unsigned manifest");
    nv_seclog_add(NV_SEC_FW_REFUSED, d);
    return false;
}

// ------------------------------------------------------------- manifest fetch
struct RespBuf { char *buf; int len; int cap; bool overflow; };
esp_err_t http_evt(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data) {
        RespBuf *r = (RespBuf *)e->user_data;
        int n = e->data_len;
        // Truncate + flag instead of dropping a chunk that doesn't fit (long `notes`).
        const int room = r->cap - 1 - r->len;
        if (n > room) { n = room; r->overflow = true; }
        if (n > 0) { memcpy(r->buf + r->len, e->data, n); r->len += n; }
    }
    return ESP_OK;
}

// Fetch a small JSON document into `out` (NUL-terminated). True on HTTP 200 + non-empty body.
bool fetch_json(const char *url, char *out, int out_cap) {
    RespBuf rb = { out, 0, out_cap, false };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = http_evt;
    cfg.user_data = &rb;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // https:// manifests
    cfg.timeout_ms = 10000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200 || rb.len == 0) return false;
    if (rb.overflow) { NV_LOGE(TAG, "%s: larger than %d bytes, refused", url, out_cap); return false; }
    out[rb.len] = '\0';
    return true;
}

// A version recovery had to roll back is not offered again by the hands-free paths (boot auto-update,
// background watch) - only an explicit "Install" from Settings retries it.
bool is_bad_version(const char *v) {
    char bad[32];
    nv_config_get_str("ota_bad", "", bad, sizeof bad);
    return bad[0] && strcmp(bad, v) == 0;
}

// Parse a manifest body: version/url/notes + signature. `newer_out` = strictly newer than running.
// Returns false (state FAILED set) when a newer version is offered but not properly signed.
struct Offer { char version[32]; char url[256]; char notes[128]; bool newer; nv_fwup_manifest_t m; };
bool parse_offer(const char *body, Offer *o, const char **err) {
    memset(o, 0, sizeof *o);
    cJSON *root = cJSON_Parse(body);
    if (!root) { *err = "Bad manifest"; return false; }
    const cJSON *jver = cJSON_GetObjectItem(root, "version");
    const cJSON *jurl = cJSON_GetObjectItem(root, "url");
    const cJSON *jnot = cJSON_GetObjectItem(root, "notes");
    bool ok = cJSON_IsString(jver) && cJSON_IsString(jurl);
    if (!ok) *err = "Manifest missing version/url";
    if (ok) {
        snprintf(o->version, sizeof o->version, "%s", jver->valuestring);
        snprintf(o->url, sizeof o->url, "%s", jurl->valuestring);
        if (cJSON_IsString(jnot)) snprintf(o->notes, sizeof o->notes, "%s", jnot->valuestring);
        o->newer = nv_fwup_version_newer(o->version, running_version());
        if (o->newer && !manifest_verify(body, o->version, &o->m)) {
            *err = "Update refused: not signed by the release key";
            ok = false;
        }
    }
    cJSON_Delete(root);
    return ok;
}

void remember_offer(const Offer &o) {
    lock();
    snprintf(s_avail_ver, sizeof s_avail_ver, "%s", o.version);
    snprintf(s_bin_url, sizeof s_bin_url, "%s", o.url);
    s_expect = o.m;
    s_expect_set = o.newer;
    unlock();
}

// ------------------------------------------------------------- download
// Open a GET and read the headers, following up to 5 redirects (http->https, GitHub asset -> CDN).
bool open_following_redirects(esp_http_client_handle_t c, int *total) {
    for (int hop = 0;; ++hop) {
        if (esp_http_client_open(c, 0) != ESP_OK) return false;
        *total = (int)esp_http_client_fetch_headers(c);
        const int st = esp_http_client_get_status_code(c);
        const bool redirect = st == 301 || st == 302 || st == 303 || st == 307 || st == 308;
        if (!redirect || hop >= 5 || esp_http_client_set_redirection(c) != ESP_OK) return true;
        NV_LOGI(TAG, "dl: HTTP %d, following the redirect", st);
        esp_http_client_close(c);
    }
}

// Stream `url` to `path` on the card; never more than `cap` bytes.
bool download_to_sd(const char *url, const char *path, uint32_t cap) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    cfg.buffer_size = 4096;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    int total = 0;
    if (!open_following_redirects(c, &total)) { esp_http_client_cleanup(c); return false; }
    FILE *f = fopen(path, "wb");
    if (!f) {
        NV_LOGE(TAG, "dl: fopen('%s') errno=%d", path, errno);
        esp_http_client_close(c); esp_http_client_cleanup(c); return false;
    }
    setvbuf(f, nullptr, _IOFBF, 16 * 1024);
    NV_LOGI(TAG, "dl: %d bytes -> %s", total, path);
    char *buf = (char *)malloc(8192);
    int r = 0; uint32_t done = 0; bool ok = buf != nullptr;
    while (ok && (r = esp_http_client_read(c, buf, 8192)) > 0) {
        if (done + (uint32_t)r > cap) { NV_LOGE(TAG, "dl: bigger than the signed size, aborted"); ok = false; break; }
        if ((int)fwrite(buf, 1, (size_t)r, f) != r) { NV_LOGE(TAG, "dl: write failed (card full?)"); ok = false; break; }
        done += (uint32_t)r;
        if (total > 0) set_progress((int)((int64_t)done * 100 / total));
    }
    if (r < 0) { NV_LOGE(TAG, "dl: read error at %lu", (unsigned long)done); ok = false; }
    free(buf);
    const int status = esp_http_client_get_status_code(c);
    if (fclose(f) != 0) ok = false;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok && status == 200 && done > 0;
}

// ------------------------------------------------------------- staging
uint64_t sd_free_bytes(void) {
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(mount(), &total, &free_b) != ESP_OK) return 0;
    return free_b;
}

// Save the running system (and its signed manifest) as nvupd/prev.* so recovery can roll back. Needs
// cur.jsn - the signed manifest of what is installed. Without it there is simply no rollback copy
// (a board fresh from USB that hasn't fetched its own manifest yet); the update still proceeds.
void backup_running(void) {
    char cur[64], prev_man[64], prev_bin[64];
    nv_fwup_path(cur, sizeof cur, mount(), NV_FWUP_CUR_MAN);
    nv_fwup_path(prev_man, sizeof prev_man, mount(), NV_FWUP_PREV_MAN);
    nv_fwup_path(prev_bin, sizeof prev_bin, mount(), NV_FWUP_PREV_BIN);
    nv_fwup_manifest_t m;
    if (nv_fwup_manifest_load(cur, slot_size(), &m) != NV_FWUP_OK || strcmp(m.version, running_version()) != 0) {
        NV_LOGW(TAG, "no signed manifest for the running v%s: no rollback copy this time", running_version());
        return;
    }
    nv_fwup_manifest_t have;
    if (nv_fwup_manifest_load(prev_man, slot_size(), &have) == NV_FWUP_OK && !strcmp(have.version, m.version) &&
        nv_fwup_hash_file(prev_bin, &have, nullptr, nullptr) == NV_FWUP_OK)
        return;   // already saved (an earlier attempt)
    set_progress(0);
    set_state(NV_OTA_DOWNLOADING, "Saving the current system for rollback...");
    remove(prev_man);   // a prev.bin without its manifest is never used
    const nv_fwup_err_t e = nv_fwup_backup(nv_fwup_system_part(), &m, prev_bin, progress_cb, nullptr);
    if (e == NV_FWUP_OK && nv_fwup_manifest_save(prev_man, &m))
        NV_LOGI(TAG, "rollback copy of v%s saved", m.version);
    else
        NV_LOGW(TAG, "rollback copy failed: %s", nv_fwup_err_str(e));
}

// Image at `bin` (already on the card, matching `m`) -> nvupd/next.*, then point the boot at recovery.
esp_err_t stage_and_arm(const char *bin, const nv_fwup_manifest_t &m) {
    char next_bin[64], next_man[64];
    nv_fwup_path(next_bin, sizeof next_bin, mount(), NV_FWUP_NEXT_BIN);
    nv_fwup_path(next_man, sizeof next_man, mount(), NV_FWUP_NEXT_MAN);
    if (strcmp(bin, next_bin) != 0) {
        remove(next_bin);
        if (rename(bin, next_bin) != 0) { NV_LOGE(TAG, "stage: rename errno=%d", errno); return ESP_FAIL; }
    }
    backup_running();
    // next.jsn last: its presence is what tells recovery to install.
    if (!nv_fwup_manifest_save(next_man, &m)) return ESP_FAIL;
    nv_fwup_tries_set(mount(), 0);
    const esp_err_t err = esp_ota_set_boot_partition(nv_fwup_recovery_part());
    if (err != ESP_OK) { NV_LOGE(TAG, "boot -> recovery failed: 0x%x", (int)err); remove(next_man); return err; }
    NV_LOGI(TAG, "v%s staged; recovery installs it on the next restart", m.version);
    return ESP_OK;
}

// Preconditions shared by every install path. Returns a user-facing reason, or nullptr when OK.
const char *install_blocker(uint32_t image_size) {
    if (!nv_fwup_layout_v2()) return "This device needs a one-time reinstall from the web flasher to receive updates";
    if (!nv_sd_is_mounted()) return "Insert a microSD card to update (the update is prepared on the card)";
    if (!nv_fwup_ensure_dir(mount())) return "Cannot write to the SD card";
    // new image + rollback copy of the running one + slack
    const uint64_t need = (uint64_t)image_size + slot_size() / 2 + 4 * 1024 * 1024;
    if (sd_free_bytes() < need) return "Not enough free space on the SD card (about 20 MB needed)";
    return nullptr;
}

// Download `url`, verify it against `m`, stage it.
esp_err_t perform_update(const char *url, const nv_fwup_manifest_t &m, const char **why) {
    if ((*why = install_blocker(m.size))) return ESP_ERR_INVALID_STATE;
    char tmp[64];
    nv_fwup_path(tmp, sizeof tmp, mount(), "dl.bin");
    set_progress(0);
    set_state(NV_OTA_DOWNLOADING, "Downloading to the SD card...");
    if (!download_to_sd(url, tmp, m.size)) { remove(tmp); *why = "Download failed"; return ESP_FAIL; }
    set_progress(0);
    set_state(NV_OTA_DOWNLOADING, "Verifying...");
    const nv_fwup_err_t e = nv_fwup_hash_file(tmp, &m, progress_cb, nullptr);
    if (e != NV_FWUP_OK) {
        remove(tmp);
        char d[NV_SECLOG_DETAIL_MAX];
        snprintf(d, sizeof d, "v%.20s image != manifest", m.version);
        nv_seclog_add(NV_SEC_FW_REFUSED, d);
        *why = "Update refused: the image does not match its signature";
        return ESP_ERR_INVALID_CRC;
    }
    const esp_err_t err = stage_and_arm(tmp, m);
    if (err != ESP_OK) { remove(tmp); *why = "Cannot prepare the update on the SD card"; }
    return err;
}

// ------------------------------------------------------------- workers
void check_task(void *arg) {
    char *url = (char *)arg;
    set_state(NV_OTA_CHECKING, "Checking for updates...");
    NV_PSRAM_BSS static char body[4096];
    const bool ok = fetch_json(url, body, sizeof(body));
    free(url);
    Offer o;
    const char *err = nullptr;
    if (!ok) {
        set_state(NV_OTA_FAILED, "Cannot reach the update server");
    } else if (!parse_offer(body, &o, &err)) {
        set_state(NV_OTA_FAILED, err);
    } else {
        remember_offer(o);
        char m[160];
        if (o.newer && o.notes[0]) snprintf(m, sizeof m, "Version %s: %.100s", o.version, o.notes);
        else snprintf(m, sizeof m, o.newer ? "Version %s available" : "Up to date (%s)", o.version);
        set_state(o.newer ? NV_OTA_AVAILABLE : NV_OTA_UPTODATE, m);
    }
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

void update_task(void *) {
    char url[256];
    nv_fwup_manifest_t m;
    bool have;
    lock(); snprintf(url, sizeof(url), "%s", s_bin_url); m = s_expect; have = s_expect_set; unlock();
    const char *why = "No verified update to install";
    const esp_err_t err = have ? perform_update(url, m, &why) : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        nv_config_set_str("ota_bad", "");   // the user chose this version explicitly
        set_progress(100);
        set_state(NV_OTA_SUCCESS, "Update ready - restart to install it");
    } else {
        set_state(NV_OTA_FAILED, why);
        NV_LOGE(TAG, "update failed: %s (0x%x)", why, (int)err);
    }
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

// "<name>.bin" on the card + its signed "<name>.json" beside it (release asset nucleos-anima.json).
void install_sd_task(void *arg) {
    char *path = (char *)arg;
    set_progress(0);
    set_state(NV_OTA_DOWNLOADING, "Checking the firmware on the SD card...");
    const char *why = nullptr;
    char man[100];
    const size_t n = strlen(path);
    nv_fwup_manifest_t m;
    FILE *probe = fopen(path, "rb");
    if (!probe) why = "No firmware file on the SD card";
    else fclose(probe);
    if (!why && (n < 4 || n + 2 > sizeof man || strcasecmp(path + n - 4, ".bin"))) why = "Invalid image";
    if (!why) {
        memcpy(man, path, n - 4);
        memcpy(man + n - 4, ".json", 6);
        const nv_fwup_err_t e = nv_fwup_manifest_load(man, slot_size(), &m);
        if (e == NV_FWUP_E_IO) {
            why = "Refused: signed manifest (.json) missing beside the image";
            nv_seclog_add(NV_SEC_FW_REFUSED, "SD image without manifest");
        } else if (e != NV_FWUP_OK) {
            why = "Refused: image not signed by the release key";
            nv_seclog_add(NV_SEC_FW_REFUSED, "SD manifest not signed");
        } else if (!nv_fwup_version_newer(m.version, running_version())) {
            why = "Refused: not newer than the installed firmware";
        }
    }
    if (!why) why = install_blocker(m.size);
    if (!why) {
        set_state(NV_OTA_DOWNLOADING, "Verifying...");
        if (nv_fwup_hash_file(path, &m, progress_cb, nullptr) != NV_FWUP_OK)
            why = "Refused: the image does not match its signature";
        else if (stage_and_arm(path, m) != ESP_OK)
            why = "Cannot prepare the update on the SD card";
        else
            remove(man);
    }
    if (!why) {
        set_progress(100);
        set_state(NV_OTA_SUCCESS, "Update ready - restart to install it");
    } else {
        set_state(NV_OTA_FAILED, why);
    }
    free(path);
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

// The signed manifest of the version we run, from the release channel ("<base>/<version>.json"), kept
// as nvupd/cur.jsn once the system slot is proven to hash to it. It is what makes the rollback copy
// possible on a board that was flashed over USB (recovery writes cur.jsn itself after an update).
void ensure_cur_manifest(const char *manifest_url) {
    if (!nv_fwup_layout_v2() || !nv_sd_is_mounted() || !nv_fwup_ensure_dir(mount())) return;
    char cur[64];
    nv_fwup_path(cur, sizeof cur, mount(), NV_FWUP_CUR_MAN);
    nv_fwup_manifest_t m;
    if (nv_fwup_manifest_load(cur, slot_size(), &m) == NV_FWUP_OK && !strcmp(m.version, running_version())) return;
    char url[288];
    const char *slash = strrchr(manifest_url, '/');
    if (!slash) return;
    snprintf(url, sizeof url, "%.*s/%s.json", (int)(slash - manifest_url), manifest_url, running_version());
    NV_PSRAM_BSS static char body[2048];
    if (!fetch_json(url, body, sizeof body)) { NV_LOGW(TAG, "own manifest not found: %s", url); return; }
    if (nv_fwup_manifest_parse(body, slot_size(), &m) != NV_FWUP_OK || strcmp(m.version, running_version())) return;
    if (nv_fwup_hash_partition(nv_fwup_system_part(), &m, nullptr, nullptr) != NV_FWUP_OK) {
        NV_LOGW(TAG, "running image is not the released v%s (local build?)", m.version);
        return;
    }
    if (nv_fwup_manifest_save(cur, &m)) NV_LOGI(TAG, "signed manifest of v%s saved (rollback ready)", m.version);
}

// Boot-time hands-free updater: wait for the network, fetch the manifest, install a newer signed
// version (unless recovery already had to roll that one back) and restart into recovery.
void boot_auto_task(void *arg) {
    char *url = (char *)arg;
    NV_LOGI(TAG, "auto-OTA: start, manifest=%s running=v%s", url, running_version());
    for (int attempt = 1; attempt <= 12; attempt++) {   // ~60 s while Wi-Fi/DHCP settle
        vTaskDelay(pdMS_TO_TICKS(5000));
        NV_PSRAM_BSS static char body[4096];
        if (!fetch_json(url, body, sizeof(body))) {
            NV_LOGW(TAG, "auto-OTA: manifest unreachable (attempt %d/12)", attempt);
            continue;
        }
        Offer o;
        const char *err = nullptr;
        if (!parse_offer(body, &o, &err)) {
            NV_LOGE(TAG, "auto-OTA: %s", err);
            set_state(NV_OTA_FAILED, err);
        } else if (!o.newer) {
            NV_LOGI(TAG, "auto-OTA: v%s is up to date", running_version());
            ensure_cur_manifest(url);
        } else if (is_bad_version(o.version)) {
            remember_offer(o);
            NV_LOGW(TAG, "auto-OTA: v%s was rolled back before, not reinstalled automatically", o.version);
            set_state(NV_OTA_AVAILABLE, "A previous install of this version failed; install it manually to retry");
        } else {
            remember_offer(o);
            ensure_cur_manifest(url);   // so this update leaves a rollback copy
            const char *why = nullptr;
            NV_LOGI(TAG, "auto-OTA: installing v%s", o.version);
            if (perform_update(o.url, o.m, &why) == ESP_OK) {
                set_progress(100);
                set_state(NV_OTA_SUCCESS, "Update prepared - restarting to install");
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_restart();
            }
            set_state(NV_OTA_FAILED, why);
            NV_LOGE(TAG, "auto-OTA: %s", why);
        }
        break;
    }
    free(url);
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

// Background watch tick (see nv_ota_watch_start): one manifest read on a short-lived task.
void watch_task(void *) {
    char url[256];
    nv_ota_get_url(url, sizeof(url));
    NV_PSRAM_BSS static char body[4096];
    Offer o;
    const char *err = nullptr;
    if (!fetch_json(url, body, sizeof(body))) {
        NV_LOGW(TAG, "watch: manifest unreachable (%s)", url);
    } else if (parse_offer(body, &o, &err)) {
        if (o.newer && !is_bad_version(o.version)) {
            remember_offer(o);
            char m[64];
            snprintf(m, sizeof(m), "Version %s available", o.version);
            set_state(NV_OTA_AVAILABLE, m);
        } else if (!o.newer) {
            ensure_cur_manifest(url);
        }
    }
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

esp_timer_handle_t s_watch_timer = nullptr;
bool s_watch_periodic = false;

void watch_timer_cb(void *) {
    if (!s_watch_periodic) {   // first tick 15 min after boot, then every 6 h
        s_watch_periodic = true;
        esp_timer_start_periodic(s_watch_timer, 6ULL * 3600 * 1000 * 1000);
    }
    lock();
    const bool skip = s_busy || s_state == NV_OTA_SUCCESS;
    if (!skip) s_busy = true;
    unlock();
    if (skip) return;
    // 8 KB internal stack: TLS handshake + cert-bundle verify; may hash the slot (flash reads).
    if (xTaskCreate(watch_task, "ota_watch", 8192, nullptr, 3, nullptr) != pdPASS) {
        lock(); s_busy = false; unlock();
    }
}

// What did recovery do before this boot? result.jsn -> a one-line notice (and the bad-version mark).
void read_boot_result(void) {
    if (!nv_sd_is_mounted()) return;
    char p[64];
    nv_fwup_path(p, sizeof p, mount(), NV_FWUP_RESULT);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    char buf[512];
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    remove(p);   // told once
    buf[n] = '\0';
    cJSON *o = cJSON_Parse(buf);
    if (!o) return;
    const cJSON *op = cJSON_GetObjectItem(o, "op"), *ok = cJSON_GetObjectItem(o, "ok");
    const cJSON *from = cJSON_GetObjectItem(o, "from"), *to = cJSON_GetObjectItem(o, "to");
    const char *sop = cJSON_IsString(op) ? op->valuestring : "";
    const char *sfrom = cJSON_IsString(from) ? from->valuestring : "";
    const char *sto = cJSON_IsString(to) ? to->valuestring : "";
    const bool good = cJSON_IsTrue(ok);
    if (!strcmp(sop, "install") && good) {
        snprintf(s_boot_notice, sizeof s_boot_notice, "NucleoOS updated to %s", sto);
        nv_config_set_str("ota_bad", "");
    } else if (!strcmp(sop, "install")) {
        snprintf(s_boot_notice, sizeof s_boot_notice, "Update to %s failed - NucleoOS %s kept", sto, sfrom);
    } else if (!strcmp(sop, "rollback")) {
        snprintf(s_boot_notice, sizeof s_boot_notice, "NucleoOS %s did not start correctly - %s restored", sfrom, sto);
        nv_config_set_str("ota_bad", sfrom);
    }
    if (s_boot_notice[0]) NV_LOGI(TAG, "recovery: %s", s_boot_notice);
    cJSON_Delete(o);
}

}  // namespace

// ============================================================= public API
void nv_ota_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    // Rollback: confirm this image only after it has SURVIVED 60 s (the 1.1.57 lesson: marking valid
    // at boot turned a boot-looping image into a brick). An image that dies sooner stays
    // PENDING_VERIFY; the bootloader then falls back to recovery, which restores the previous system.
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_timer_create_args_t a = {};
        a.callback = [](void *) {
            // otadata erase+write on an internal-stack task, not the shared esp_timer task.
            auto mark = [](void *) {
                esp_ota_mark_app_valid_cancel_rollback();
                NV_LOGI(TAG, "image survived 60 s -> marked valid (rollback cancelled)");
                vTaskDelete(nullptr);
            };
            if (xTaskCreate(mark, "ota_valid", 4096, nullptr, 5, nullptr) != pdPASS)
                esp_ota_mark_app_valid_cancel_rollback();
        };
        a.dispatch_method = ESP_TIMER_TASK;
        a.name = "ota_valid";
        esp_timer_handle_t t = nullptr;
        const bool created = esp_timer_create(&a, &t) == ESP_OK;
        if (created && esp_timer_start_once(t, 60 * 1000 * 1000ULL) == ESP_OK) {
            NV_LOGI(TAG, "image PENDING_VERIFY: validation deferred 60 s");
        } else {
            if (created) esp_timer_delete(t);
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }
    read_boot_result();
    NV_LOGI(TAG, "OTA service ready, running v%s (%s)", running_version(),
            nv_fwup_layout_v2() ? "layout v2" : "LEGACY layout - reinstall from the web flasher");
}

nv_ota_state_t nv_ota_state(void) { return s_state; }
int nv_ota_progress(void)         { return s_progress; }
uint32_t nv_ota_generation(void)  { return s_gen; }
const char *nv_ota_running_version(void)   { return running_version(); }
const char *nv_ota_available_version(void) { return s_avail_ver; }
const char *nv_ota_message(void)           { return s_msg; }
bool nv_ota_layout_ok(void)                { return nv_fwup_layout_v2(); }
bool nv_ota_busy(void)                     { lock(); const bool b = s_busy; unlock(); return b; }

bool nv_ota_take_boot_notice(char *out, size_t n) {
    if (s_boot_notice_taken || !s_boot_notice[0] || !out || !n) return false;
    s_boot_notice_taken = true;
    snprintf(out, n, "%s", s_boot_notice);
    return true;
}

void nv_ota_watch_start(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_watch_timer) return;
    esp_timer_create_args_t a = {};
    a.callback = watch_timer_cb;
    a.dispatch_method = ESP_TIMER_TASK;
    a.name = "ota_watch";
    if (esp_timer_create(&a, &s_watch_timer) != ESP_OK) { s_watch_timer = nullptr; return; }
    if (esp_timer_start_once(s_watch_timer, 15ULL * 60 * 1000 * 1000) != ESP_OK) {
        esp_timer_delete(s_watch_timer);
        s_watch_timer = nullptr;
        return;
    }
    NV_LOGI(TAG, "watch: first manifest check in 15 min, then every 6 h");
}

void nv_ota_get_url(char *out, size_t n) {
    nv_config_get_str("ota_url", NV_OTA_DEFAULT_URL, out, n);
    // Empty, or the layout-v1 channel saved by an older firmware: the v2 channel. A v1 image must
    // never be installed on this layout, and v1 boards must never see v2 images.
    if (n && (!out[0] || !strcmp(out, NV_OTA_LEGACY_URL))) snprintf(out, n, "%s", NV_OTA_DEFAULT_URL);
}

void nv_ota_check(const char *manifest_url) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    lock();
    if (s_busy) { unlock(); return; }
    s_busy = true;
    unlock();
    if (!manifest_url || !manifest_url[0]) {
        set_state(NV_OTA_FAILED, "No update URL set");
        lock(); s_busy = false; unlock();
        return;
    }
    char *arg = strdup(manifest_url);
    // 12 KB: an https:// manifest means a TLS handshake + cert-bundle verify (~8-10 KB of stack).
    if (!arg || xTaskCreate(check_task, "ota_chk", 12288, arg, 5, nullptr) != pdPASS) {
        free(arg);
        set_state(NV_OTA_FAILED, "Out of memory");
        lock(); s_busy = false; unlock();
    }
}

void nv_ota_update(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    lock();
    if (s_busy || s_bin_url[0] == '\0') { unlock(); return; }
    s_busy = true;
    unlock();
    // 8 KB internal stack: TLS + FATFS + flash reads (the rollback copy).
    if (xTaskCreate(update_task, "ota_dl", 8192, nullptr, 5, nullptr) != pdPASS) {
        set_state(NV_OTA_FAILED, "Out of memory");
        lock(); s_busy = false; unlock();
    }
}

void nv_ota_install_sd(const char *path) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    lock();
    if (s_busy) { unlock(); return; }
    s_busy = true;
    unlock();
    char full[96];
    if (path && path[0]) snprintf(full, sizeof(full), "%s", path);
    else snprintf(full, sizeof(full), "%s/nucleos-anima.bin", nv_sd_mount_point());
    char *arg = strdup(full);
    if (!arg || xTaskCreate(install_sd_task, "ota_sd", 6144, arg, 5, nullptr) != pdPASS) {
        free(arg);
        set_state(NV_OTA_FAILED, "Out of memory");
        lock(); s_busy = false; unlock();
    }
}

void nv_ota_reboot(void) {
    if (s_state == NV_OTA_SUCCESS) {
        NV_LOGI(TAG, "restarting into recovery to install the update");
        esp_restart();
    }
}

void nv_ota_boot_autoupdate(const char *manifest_url) {
    if (!manifest_url || !manifest_url[0]) return;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    lock();
    if (s_busy) { unlock(); return; }
    s_busy = true;
    unlock();
    char *arg = strdup(manifest_url);
    if (!arg || xTaskCreate(boot_auto_task, "ota_auto", 8192, arg, 4, nullptr) != pdPASS) {
        free(arg);
        lock(); s_busy = false; unlock();
    }
}
