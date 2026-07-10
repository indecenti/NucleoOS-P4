// nv_ota — Wi-Fi firmware updater. See nv_ota.h.
#include "nv_ota.h"
#include "nv_log.h"
#include "nv_config.h"
#include "nv_sd.h"        // stage OTA payload on the SD card instead of internal flash

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_ota_ops.h"
#include "esp_timer.h"     // deferred mark-valid (60 s survival gate)
#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>

static const char *TAG = "ota";

namespace {

SemaphoreHandle_t s_lock = nullptr;
volatile nv_ota_state_t s_state = NV_OTA_IDLE;
volatile int  s_progress = 0;
volatile uint32_t s_gen  = 0;
char s_msg[128]      = "";
char s_avail_ver[32] = "";
char s_bin_url[256]  = "";
bool s_busy = false;   // a worker task is running

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

const char *running_version(void) {
    const esp_app_desc_t *d = esp_app_get_description();
    return d ? d->version : "?";
}

// Compare dotted versions "A.B.C". True only when `cand` is STRICTLY newer than `cur` — so a
// manifest that lists an older/equal build can never trigger a pointless (or looping) downgrade.
bool version_is_newer(const char *cand, const char *cur) {
    int a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
    sscanf(cand, "%d.%d.%d", &a[0], &a[1], &a[2]);
    sscanf(cur,  "%d.%d.%d", &b[0], &b[1], &b[2]);
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

// ------------------------------------------------------------- manifest fetch
struct RespBuf { char *buf; int len; int cap; };
esp_err_t http_evt(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data) {
        RespBuf *r = (RespBuf *)e->user_data;
        int n = e->data_len;
        if (r->len + n < r->cap - 1) { memcpy(r->buf + r->len, e->data, n); r->len += n; }
    }
    return ESP_OK;
}

// Fetch the manifest into `out` (NUL-terminated). Returns true on HTTP 200 + non-empty body.
bool fetch_manifest(const char *url, char *out, int out_cap) {
    RespBuf rb = { out, 0, out_cap };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = http_evt;
    cfg.user_data = &rb;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // enables https:// manifests
    cfg.timeout_ms = 10000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_err_t err = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status != 200 || rb.len == 0) return false;
    out[rb.len] = '\0';
    return true;
}

// ------------------------------------------------------------- workers
void check_task(void *arg) {
    char *url = (char *)arg;
    set_state(NV_OTA_CHECKING, "Checking for updates...");

    char body[512];
    const bool ok = fetch_manifest(url, body, sizeof(body));
    free(url);

    cJSON *root = ok ? cJSON_Parse(body) : nullptr;
    if (!ok) {
        set_state(NV_OTA_FAILED, "Cannot reach update server");
    } else if (!root) {
        set_state(NV_OTA_FAILED, "Bad manifest");
    } else {
        cJSON *jver   = cJSON_GetObjectItem(root, "version");
        cJSON *jurl   = cJSON_GetObjectItem(root, "url");
        cJSON *jnotes = cJSON_GetObjectItem(root, "notes");
        if (!cJSON_IsString(jver) || !cJSON_IsString(jurl)) {
            set_state(NV_OTA_FAILED, "Manifest missing version/url");
        } else {
            lock();
            snprintf(s_avail_ver, sizeof(s_avail_ver), "%s", jver->valuestring);
            snprintf(s_bin_url, sizeof(s_bin_url), "%s", jurl->valuestring);
            unlock();
            const bool newer = version_is_newer(jver->valuestring, running_version());
            char m[128];
            if (cJSON_IsString(jnotes) && jnotes->valuestring[0])
                snprintf(m, sizeof(m), "%s", jnotes->valuestring);
            else
                snprintf(m, sizeof(m), newer ? "Version %s available" : "Up to date (%s)",
                         jver->valuestring);
            set_state(newer ? NV_OTA_AVAILABLE : NV_OTA_UPTODATE, m);
        }
    }
    if (root) cJSON_Delete(root);
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

// Write a local .bin (already on the SD card) into the inactive OTA slot and arm it for boot.
// esp_ota_end validates the image (magic + SHA-256) before we flip the boot pointer.
esp_err_t flash_from_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { NV_LOGE(TAG, "flash: fopen('%s') failed errno=%d", path, errno); return ESP_ERR_NOT_FOUND; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { NV_LOGE(TAG, "flash: empty file"); fclose(f); return ESP_FAIL; }

    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
    if (!part) { NV_LOGE(TAG, "flash: no next OTA partition"); fclose(f); return ESP_FAIL; }
    if (sz > (long)part->size) {
        NV_LOGE(TAG, "flash: image %ld > slot %u", sz, (unsigned)part->size);
        fclose(f); return ESP_ERR_INVALID_SIZE;
    }
    NV_LOGI(TAG, "flash: writing %ld bytes to '%s'", sz, part->label);

    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(part, (size_t)sz, &h);
    if (err != ESP_OK) { NV_LOGE(TAG, "flash: esp_ota_begin err=0x%x", (int)err); fclose(f); return err; }

    uint8_t *buf = (uint8_t *)malloc(4096);
    if (!buf) { esp_ota_abort(h); fclose(f); return ESP_ERR_NO_MEM; }
    long done = 0; size_t n;
    while ((n = fread(buf, 1, 4096, f)) > 0) {
        if ((err = esp_ota_write(h, buf, n)) != ESP_OK) break;
        done += (long)n;
        set_progress((int)((int64_t)done * 100 / sz));
    }
    free(buf);
    fclose(f);
    if (err != ESP_OK) { NV_LOGE(TAG, "flash: write err=0x%x", (int)err); esp_ota_abort(h); return err; }
    if ((err = esp_ota_end(h)) != ESP_OK) {                   // image validation happens here
        NV_LOGE(TAG, "flash: esp_ota_end (validate) err=0x%x", (int)err); return err;
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) NV_LOGE(TAG, "flash: set_boot err=0x%x", (int)err);
    return err;
}

// Stream a URL straight to a file on the SD card (progress by content-length).
bool download_to_sd(const char *url, const char *path) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { NV_LOGE(TAG, "dl: client init failed"); return false; }
    if (esp_http_client_open(c, 0) != ESP_OK) {
        NV_LOGE(TAG, "dl: open failed"); esp_http_client_cleanup(c); return false;
    }
    const int total = esp_http_client_fetch_headers(c);      // content length (<=0 if chunked)
    FILE *f = fopen(path, "wb");
    if (!f) {
        NV_LOGE(TAG, "dl: fopen('%s') failed errno=%d", path, errno);
        esp_http_client_close(c); esp_http_client_cleanup(c); return false;
    }
    NV_LOGI(TAG, "dl: streaming %d bytes -> %s", total, path);
    char buf[2048]; int r; long done = 0; bool ok = true;
    while ((r = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
        if ((int)fwrite(buf, 1, (size_t)r, f) != r) {
            NV_LOGE(TAG, "dl: fwrite failed at %ld errno=%d (SD full/again?)", done, errno);
            ok = false; break;
        }
        done += r;
        if (total > 0) set_progress((int)((int64_t)done * 100 / total));
    }
    if (r < 0) { NV_LOGE(TAG, "dl: http read error at %ld", done); ok = false; }
    const int status = esp_http_client_get_status_code(c);
    fclose(f);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    NV_LOGI(TAG, "dl: done=%ld status=%d ok=%d", done, status, (int)ok);
    return ok && status == 200 && done > 0;
}

// esp_https_ota straight to the inactive partition — fallback when there is no SD card.
esp_err_t ota_https_direct(const char *url) {
    esp_http_client_config_t http = {};
    http.url = url;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms = 20000;
    http.keep_alive_enable = true;
    esp_https_ota_config_t cfg = {};
    cfg.http_config = &http;

    esp_https_ota_handle_t h = nullptr;
    esp_err_t be = esp_https_ota_begin(&cfg, &h);
    if (be != ESP_OK || !h) { NV_LOGE(TAG, "direct: begin err=0x%x", (int)be); return ESP_FAIL; }
    const int total = esp_https_ota_get_image_size(h);
    NV_LOGI(TAG, "direct: image %d bytes", total);
    esp_err_t err;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        const int done = esp_https_ota_get_image_len_read(h);
        set_progress(total > 0 ? (int)((int64_t)done * 100 / total) : 0);
    }
    const bool good = (err == ESP_OK) && esp_https_ota_is_complete_data_received(h);
    if (good && esp_https_ota_finish(h) == ESP_OK) { NV_LOGI(TAG, "direct: OK"); return ESP_OK; }
    NV_LOGE(TAG, "direct: perform err=0x%x complete=%d", (int)err, (int)good);
    if (!good) esp_https_ota_abort(h);
    return ESP_FAIL;
}

// Download `url` into the inactive slot and validate it. SD-staged when a card is present
// (transfer off internal flash + verify-before-flash), else esp_https_ota straight to flash.
esp_err_t perform_update(const char *url) {
    if (nv_sd_is_mounted()) {
        char path[80];
        // 8.3 name: FATFS long-filename support is off (CONFIG_FATFS_LFN_NONE), so a >8-char base
        // like "nv_update" fails fopen with EINVAL. "nvupdate" (8 chars) is valid.
        snprintf(path, sizeof(path), "%s/nvupdate.bin", nv_sd_mount_point());
        set_progress(0); set_state(NV_OTA_DOWNLOADING, "Downloading to SD...");
        if (download_to_sd(url, path)) {
            set_progress(0); set_state(NV_OTA_DOWNLOADING, "Installing from SD...");
            esp_err_t err = flash_from_file(path);
            remove(path);   // reclaim the SD staging file
            if (err == ESP_OK) return ESP_OK;
            NV_LOGW(TAG, "SD-staged flash failed (0x%x) -> falling back to direct", (int)err);
        } else {
            NV_LOGW(TAG, "SD staging failed -> falling back to direct-to-flash");
        }
        // SD path failed — don't strand the update; stream straight into the slot instead.
    }
    set_progress(0); set_state(NV_OTA_DOWNLOADING, "Downloading...");
    return ota_https_direct(url);
}

void update_task(void *) {
    char url[256];
    lock(); snprintf(url, sizeof(url), "%s", s_bin_url); unlock();
    esp_err_t err = perform_update(url);

    if (err == ESP_OK) {
        set_progress(100);
        set_state(NV_OTA_SUCCESS, "Update ready — restart to apply");
        NV_LOGI(TAG, "OTA image written OK");
    } else {
        set_state(NV_OTA_FAILED, "Download/verify failed");
        NV_LOGE(TAG, "OTA failed (err=0x%x)", (int)err);
    }
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

// Flash a firmware image the user placed on the SD card directly (no server needed).
void install_sd_task(void *arg) {
    char *path = (char *)arg;
    set_progress(0); set_state(NV_OTA_DOWNLOADING, "Installing from SD...");
    esp_err_t err = flash_from_file(path);
    if (err == ESP_OK) {
        set_progress(100);
        set_state(NV_OTA_SUCCESS, "Update ready — restart to apply");
    } else {
        set_state(NV_OTA_FAILED,
                  err == ESP_ERR_NOT_FOUND ? "No firmware file on the SD card" : "Invalid image");
    }
    free(path);
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

// Boot-time hands-free updater: waits for the network, fetches the manifest, and — if it offers a
// different version — downloads, flashes and reboots into it. Verbose logging so the whole path is
// visible on the serial console (and diagnoses reachability when the board can't reach the server).
void boot_auto_task(void *arg) {
    char *url = (char *)arg;
    NV_LOGI(TAG, "auto-OTA: start, manifest=%s running=v%s", url, running_version());

    for (int attempt = 1; attempt <= 12; attempt++) {   // ~60s of retries while Wi-Fi/DHCP settle
        vTaskDelay(pdMS_TO_TICKS(5000));
        char body[512];
        if (!fetch_manifest(url, body, sizeof(body))) {
            NV_LOGW(TAG, "auto-OTA: manifest unreachable (attempt %d/12)", attempt);
            continue;
        }
        NV_LOGI(TAG, "auto-OTA: manifest fetched (%d bytes)", (int)strlen(body));
        cJSON *root = cJSON_Parse(body);
        if (!root) { NV_LOGE(TAG, "auto-OTA: bad manifest json"); break; }
        cJSON *jver = cJSON_GetObjectItem(root, "version");
        cJSON *jurl = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(jver) && cJSON_IsString(jurl)) {
            const bool newer = version_is_newer(jver->valuestring, running_version());
            NV_LOGI(TAG, "auto-OTA: offered v%s vs running v%s -> %s",
                    jver->valuestring, running_version(), newer ? "INSTALL" : "up-to-date");
            if (newer) {
                char bin[256];
                snprintf(bin, sizeof(bin), "%s", jurl->valuestring);
                lock(); snprintf(s_avail_ver, sizeof(s_avail_ver), "%s", jver->valuestring);
                        snprintf(s_bin_url, sizeof(s_bin_url), "%s", bin); unlock();
                cJSON_Delete(root); root = nullptr;
                esp_err_t err = perform_update(bin);
                if (err == ESP_OK) {
                    set_progress(100);
                    set_state(NV_OTA_SUCCESS, "Update installed — rebooting");
                    NV_LOGI(TAG, "auto-OTA: installed OK, rebooting into new slot");
                    vTaskDelay(pdMS_TO_TICKS(1200));
                    esp_restart();
                } else {
                    set_state(NV_OTA_FAILED, "Auto-update failed");
                    NV_LOGE(TAG, "auto-OTA: install failed err=0x%x", (int)err);
                }
            }
        } else {
            NV_LOGE(TAG, "auto-OTA: manifest missing version/url");
        }
        if (root) cJSON_Delete(root);
        break;   // got a manifest this attempt — done (up-to-date, installed, or failed)
    }
    free(url);
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

}  // namespace

// ============================================================= public API
void nv_ota_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    // Rollback: confirm this image is good so the bootloader keeps it — but only after it has
    // SURVIVED 60 s. The 1.1.57 incident: marking valid here (first thing in app_main) turned a
    // boot-looping image into a permanent brick — every crash cycle re-ran the already-valid slot
    // and the bootloader never reverted. Launcher + Wi-Fi + web are all up well inside 60 s, so a
    // healthy image always confirms; an image that dies sooner stays PENDING_VERIFY and the next
    // boot rolls back to the previous slot. (Trade-off: power-cycling a JUST-updated board twice
    // within 60 s reverts the update — the updater simply reinstalls it.)
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_timer_create_args_t a = {};
        a.callback = [](void *) {
            esp_ota_mark_app_valid_cancel_rollback();
            NV_LOGI(TAG, "image survived 60 s -> marked valid (rollback cancelled)");
        };
        a.dispatch_method = ESP_TIMER_TASK;
        a.name = "ota_valid";
        esp_timer_handle_t t = nullptr;
        bool created = esp_timer_create(&a, &t) == ESP_OK;
        if (created && esp_timer_start_once(t, 60 * 1000 * 1000ULL) == ESP_OK) {
            NV_LOGI(TAG, "image PENDING_VERIFY: validation deferred 60 s");
        } else {
            if (created) esp_timer_delete(t);
            esp_ota_mark_app_valid_cancel_rollback();   // can't defer: keep the old guarantee
            NV_LOGI(TAG, "running image marked valid (rollback cancelled)");
        }
    }
    NV_LOGI(TAG, "OTA service ready, running v%s", running_version());
}

nv_ota_state_t nv_ota_state(void) { return s_state; }
int nv_ota_progress(void)         { return s_progress; }
uint32_t nv_ota_generation(void)  { return s_gen; }
const char *nv_ota_running_version(void)   { return running_version(); }
const char *nv_ota_available_version(void) { return s_avail_ver; }
const char *nv_ota_message(void)           { return s_msg; }

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
    if (!arg || xTaskCreate(check_task, "ota_chk", 6144, arg, 5, nullptr) != pdPASS) {
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
        NV_LOGI(TAG, "rebooting into new firmware");
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
