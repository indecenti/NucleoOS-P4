// nv_ota — Wi-Fi firmware updater. See nv_ota.h.
#include "nv_ota.h"
#include "nv_log.h"
#include "nv_mem_attr.h"   // NV_PSRAM_BSS
#include "nv_config.h"
#include "nv_sd.h"        // stage OTA payload on the SD card instead of internal flash

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_ota_ops.h"
#include "esp_timer.h"     // deferred mark-valid (60 s survival gate)
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "nv_ota_manifest.h"   // the signed manifest fields (pure, host-tested)

// The release key's public half (tools/ota_sign.py keygen), embedded NUL-terminated.
extern const char ota_pub_start[] asm("_binary_ota_signing_pub_pem_start");
extern const char ota_pub_end[]   asm("_binary_ota_signing_pub_pem_end");

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

// A remote update is installed only when its manifest is signed by the release key and the image
// then hashes to the signed sha256/size (checked on the bytes written to the slot, before the boot
// pointer moves). This is what the last verified manifest promised (guarded by s_lock).
struct Expect { uint8_t sha256[32]; uint32_t size; char version[32]; };
Expect s_expect = {};
bool   s_expect_set = false;

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

// ------------------------------------------------------------- manifest signature
// ECDSA P-256 over nv_ota_manifest::message(); see tools/ota_sign.py for the release side.
bool manifest_verify(cJSON *root, const char *version, Expect *out) {
    namespace m = nv_ota_manifest;
    const cJSON *jsha  = cJSON_GetObjectItem(root, "sha256");
    const cJSON *jsize = cJSON_GetObjectItem(root, "size");
    const cJSON *jsig  = cJSON_GetObjectItem(root, "sig");
    const esp_partition_t *np = esp_ota_get_next_update_partition(nullptr);
    m::Signed s;
    if (!cJSON_IsString(jsha) || !cJSON_IsNumber(jsize) || !cJSON_IsString(jsig) ||
        !m::parse(version, jsha->valuestring, jsize->valuedouble, jsig->valuestring,
                  np ? (uint32_t)np->size : 0, &s)) {
        NV_LOGE(TAG, "manifest v%s: missing or malformed signature fields, refused", version);
        return false;
    }
    char msg[160];
    const size_t len = m::message(s, msg, sizeof msg);
    uint8_t h[32];
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = len ? mbedtls_pk_parse_public_key(&pk, reinterpret_cast<const unsigned char *>(ota_pub_start),
                                               (size_t)(ota_pub_end - ota_pub_start))
                 : -1;
    if (rc == 0) rc = mbedtls_sha256(reinterpret_cast<const unsigned char *>(msg), len, h, 0);
    if (rc == 0) rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, h, sizeof h, s.sig, s.sig_len);
    mbedtls_pk_free(&pk);
    if (rc != 0) {
        NV_LOGE(TAG, "manifest v%s: signature check failed (-0x%04x), refused", version, -rc);
        return false;
    }
    memcpy(out->sha256, s.sha256, sizeof out->sha256);
    out->size = s.size;
    snprintf(out->version, sizeof out->version, "%s", s.version);
    return true;
}

// ------------------------------------------------------------- manifest fetch
struct RespBuf { char *buf; int len; int cap; bool overflow; };
esp_err_t http_evt(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data) {
        RespBuf *r = (RespBuf *)e->user_data;
        int n = e->data_len;
        // Truncate + flag instead of DROPPING a chunk that doesn't fit: a manifest with a long
        // `notes` field arriving in one segment used to yield len==0 ("Cannot reach update server")
        // or, split across chunks, a silently truncated JSON ("Bad manifest").
        const int room = r->cap - 1 - r->len;
        if (n > room) { n = room; r->overflow = true; }
        if (n > 0) { memcpy(r->buf + r->len, e->data, n); r->len += n; }
    }
    return ESP_OK;
}

// Fetch the manifest into `out` (NUL-terminated). Returns true on HTTP 200 + non-empty body.
bool fetch_manifest(const char *url, char *out, int out_cap) {
    RespBuf rb = { out, 0, out_cap, false };
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
    if (rb.overflow) { NV_LOGE(TAG, "manifest larger than %d bytes — refused", out_cap); return false; }
    out[rb.len] = '\0';
    return true;
}

// ------------------------------------------------------------- workers
void check_task(void *arg) {
    char *url = (char *)arg;
    set_state(NV_OTA_CHECKING, "Checking for updates...");

    NV_PSRAM_BSS static char body[4096];   // manifest with release notes; off the 12 KB stack, out of internal SRAM
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
            const bool newer = version_is_newer(jver->valuestring, running_version());
            Expect e = {};
            if (newer && !manifest_verify(root, jver->valuestring, &e)) {
                set_state(NV_OTA_FAILED, "Update refused: not signed by the release key");
            } else {
                lock();
                snprintf(s_avail_ver, sizeof(s_avail_ver), "%s", jver->valuestring);
                snprintf(s_bin_url, sizeof(s_bin_url), "%s", jurl->valuestring);
                s_expect = e;
                s_expect_set = newer;
                unlock();
                char m[128];
                if (cJSON_IsString(jnotes) && jnotes->valuestring[0])
                    snprintf(m, sizeof(m), "%s", jnotes->valuestring);
                else
                    snprintf(m, sizeof(m), newer ? "Version %s available" : "Up to date (%s)",
                             jver->valuestring);
                set_state(newer ? NV_OTA_AVAILABLE : NV_OTA_UPTODATE, m);
            }
        }
    }
    if (root) cJSON_Delete(root);
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

// sha256 + size of what was written against what the signed manifest promised.
bool matches(const Expect &e, const uint8_t digest[32], long size) {
    if ((long)e.size == size && memcmp(digest, e.sha256, 32) == 0) return true;
    NV_LOGE(TAG, "image %s differs from the signed manifest, refused",
            (long)e.size == size ? "sha256" : "size");
    return false;
}

// The version inside the written image must be the one the manifest was signed for: a signed
// manifest can then never relabel an older build as newer (a downgrade).
bool version_matches(const esp_partition_t *part, const Expect &e) {
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(part, &d) == ESP_OK &&
        strncmp(d.version, e.version, sizeof d.version) == 0)
        return true;
    NV_LOGE(TAG, "image version differs from the signed manifest (v%s), refused", e.version);
    return false;
}

// Write a local .bin (already on the SD card) into the inactive OTA slot and arm it for boot.
// esp_ota_end validates the image (magic + SHA-256) before we flip the boot pointer; with `e` (a
// remote update) the bytes written must also hash to the signed manifest, or the slot is dropped.
esp_err_t flash_from_file(const char *path, const Expect *e) {
    FILE *f = fopen(path, "rb");
    if (!f) { NV_LOGE(TAG, "flash: fopen('%s') failed errno=%d", path, errno); return ESP_ERR_NOT_FOUND; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { NV_LOGE(TAG, "flash: empty file"); fclose(f); return ESP_FAIL; }
    if (e && sz != (long)e->size) {
        NV_LOGE(TAG, "flash: %ld bytes, the signed manifest says %lu, refused", sz, (unsigned long)e->size);
        fclose(f); return ESP_ERR_INVALID_CRC;
    }

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
    mbedtls_sha256_context sc;
    mbedtls_sha256_init(&sc);
    mbedtls_sha256_starts(&sc, 0);
    long done = 0; size_t n;
    while ((n = fread(buf, 1, 4096, f)) > 0) {
        mbedtls_sha256_update(&sc, buf, n);
        if ((err = esp_ota_write(h, buf, n)) != ESP_OK) break;
        done += (long)n;
        set_progress((int)((int64_t)done * 100 / sz));
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&sc, digest);
    mbedtls_sha256_free(&sc);
    free(buf);
    fclose(f);
    if (err != ESP_OK) { NV_LOGE(TAG, "flash: write err=0x%x", (int)err); esp_ota_abort(h); return err; }
    if (e && !matches(*e, digest, done)) { esp_ota_abort(h); return ESP_ERR_INVALID_CRC; }
    if ((err = esp_ota_end(h)) != ESP_OK) {                   // image validation happens here
        NV_LOGE(TAG, "flash: esp_ota_end (validate) err=0x%x", (int)err); return err;
    }
    if (e && !version_matches(part, *e)) return ESP_ERR_INVALID_CRC;   // boot pointer untouched
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) NV_LOGE(TAG, "flash: set_boot err=0x%x", (int)err);
    return err;
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
        NV_LOGI(TAG, "dl: HTTP %d, following the redirect", st);
        esp_http_client_close(c);
    }
}

// Stream a URL straight to a file on the SD card (progress by content-length).
bool download_to_sd(const char *url, const char *path) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { NV_LOGE(TAG, "dl: client init failed"); return false; }
    int total = 0;                                            // content length (<=0 if chunked)
    if (!open_following_redirects(c, &total)) {
        NV_LOGE(TAG, "dl: open failed"); esp_http_client_cleanup(c); return false;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        NV_LOGE(TAG, "dl: fopen('%s') failed errno=%d", path, errno);
        esp_http_client_close(c); esp_http_client_cleanup(c); return false;
    }
    NV_LOGI(TAG, "dl: streaming %d bytes -> %s", total, path);
    // Nothing bigger than the OTA slot can ever be flashed: stop a wrong/huge URL mid-stream
    // instead of filling the card and only finding out in flash_from_file().
    const esp_partition_t *np = esp_ota_get_next_update_partition(nullptr);
    const long cap = np ? (long)np->size : (4608L * 1024);
    char buf[2048]; int r; long done = 0; bool ok = true;
    while ((r = esp_http_client_read(c, buf, sizeof(buf))) > 0) {
        if (done + r > cap) { NV_LOGE(TAG, "dl: image exceeds the OTA slot (%ld B) — aborted", cap); ok = false; break; }
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

// Stream the image straight into the inactive slot, hashing it on the way: the fallback when there
// is no SD card. The boot pointer moves only when the bytes match the signed manifest.
esp_err_t stream_to_slot(const char *url, const Expect &e) {
    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
    if (!part || e.size > part->size) { NV_LOGE(TAG, "direct: no slot for %lu bytes", (unsigned long)e.size); return ESP_FAIL; }
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;
    int total = 0;
    if (!open_following_redirects(c, &total) || esp_http_client_get_status_code(c) != 200) {
        NV_LOGE(TAG, "direct: open failed (HTTP %d)", esp_http_client_get_status_code(c));
        esp_http_client_cleanup(c); return ESP_FAIL;
    }
    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(part, e.size, &h);
    uint8_t *buf = err == ESP_OK ? (uint8_t *)malloc(4096) : nullptr;
    if (!buf) {
        if (err == ESP_OK) { esp_ota_abort(h); err = ESP_ERR_NO_MEM; }
        NV_LOGE(TAG, "direct: begin err=0x%x", (int)err);
        esp_http_client_close(c); esp_http_client_cleanup(c); return err;
    }
    mbedtls_sha256_context sc;
    mbedtls_sha256_init(&sc);
    mbedtls_sha256_starts(&sc, 0);
    long done = 0; int r;
    while ((r = esp_http_client_read(c, (char *)buf, 4096)) > 0) {
        if (done + r > (long)e.size) { NV_LOGE(TAG, "direct: more data than the signed size"); err = ESP_ERR_INVALID_CRC; break; }
        mbedtls_sha256_update(&sc, buf, (size_t)r);
        if ((err = esp_ota_write(h, buf, (size_t)r)) != ESP_OK) break;
        done += r;
        set_progress((int)((int64_t)done * 100 / e.size));
    }
    if (r < 0 && err == ESP_OK) { NV_LOGE(TAG, "direct: http read error at %ld", done); err = ESP_FAIL; }
    uint8_t digest[32];
    mbedtls_sha256_finish(&sc, digest);
    mbedtls_sha256_free(&sc);
    free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (err == ESP_OK && !matches(e, digest, done)) err = ESP_ERR_INVALID_CRC;
    if (err != ESP_OK) { esp_ota_abort(h); return err; }
    if ((err = esp_ota_end(h)) != ESP_OK) { NV_LOGE(TAG, "direct: validate err=0x%x", (int)err); return err; }
    if (!version_matches(part, e)) return ESP_ERR_INVALID_CRC;
    err = esp_ota_set_boot_partition(part);
    if (err == ESP_OK) NV_LOGI(TAG, "direct: OK");
    return err;
}

// Download `url` into the inactive slot and verify it against the signed manifest `e`. SD-staged
// when a card is present (transfer off internal flash), else streamed straight into the slot.
esp_err_t perform_update(const char *url, const Expect &e) {
    // A freshly booted image stays PENDING_VERIFY for the 60 s survival gate, and esp_ota_begin()
    // refuses to write the other slot in that state: publishing v2 while v1 boots used to download
    // the whole image, fail, download it AGAIN via the direct path and fail — every early update
    // needed an extra reboot. Wait the gate out (bounded) before touching the network.
    for (int i = 0; i < 75; i++) {
        esp_ota_img_states_t st;
        const esp_partition_t *run = esp_ota_get_running_partition();
        if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) break;
        if (i == 0) { set_state(NV_OTA_DOWNLOADING, "Waiting for boot validation..."); NV_LOGI(TAG, "update: waiting for the survival gate"); }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (nv_sd_is_mounted()) {
        char path[80];
        // 8.3 name: FATFS long-filename support is off (CONFIG_FATFS_LFN_NONE), so a >8-char base
        // like "nv_update" fails fopen with EINVAL. "nvupdate" (8 chars) is valid.
        snprintf(path, sizeof(path), "%s/nvupdate.bin", nv_sd_mount_point());
        set_progress(0); set_state(NV_OTA_DOWNLOADING, "Downloading to SD...");
        if (download_to_sd(url, path)) {
            set_progress(0); set_state(NV_OTA_DOWNLOADING, "Installing from SD...");
            esp_err_t err = flash_from_file(path, &e);
            remove(path);   // reclaim the SD staging file
            if (err == ESP_OK) return ESP_OK;
            if (err == ESP_ERR_INVALID_CRC) return err;   // the server's image is not the signed one
            NV_LOGW(TAG, "SD-staged flash failed (0x%x) -> falling back to direct", (int)err);
        } else {
            NV_LOGW(TAG, "SD staging failed -> falling back to direct-to-flash");
        }
        // SD path failed — don't strand the update; stream straight into the slot instead.
    }
    set_progress(0); set_state(NV_OTA_DOWNLOADING, "Downloading...");
    return stream_to_slot(url, e);
}

void update_task(void *) {
    char url[256];
    Expect e;
    bool have;
    lock(); snprintf(url, sizeof(url), "%s", s_bin_url); e = s_expect; have = s_expect_set; unlock();
    esp_err_t err = have ? perform_update(url, e) : ESP_ERR_INVALID_STATE;

    if (err == ESP_OK) {
        set_progress(100);
        set_state(NV_OTA_SUCCESS, "Update ready — restart to apply");
        NV_LOGI(TAG, "OTA image written OK");
    } else {
        set_state(NV_OTA_FAILED, err == ESP_ERR_INVALID_CRC   ? "Update refused: image does not match its signature"
                               : err == ESP_ERR_INVALID_STATE ? "No verified update to install"
                                                              : "Download/verify failed");
        NV_LOGE(TAG, "OTA failed (err=0x%x)", (int)err);
    }
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

// Flash a firmware image the user placed on the SD card directly (no server needed).
void install_sd_task(void *arg) {
    char *path = (char *)arg;
    set_progress(0); set_state(NV_OTA_DOWNLOADING, "Installing from SD...");
    // A file the owner put on the card is a local action, like a USB flash: no manifest to check.
    esp_err_t err = flash_from_file(path, nullptr);
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
        NV_PSRAM_BSS static char body[4096];   // see check_task
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
            Expect e = {};
            if (newer && !manifest_verify(root, jver->valuestring, &e)) {
                set_state(NV_OTA_FAILED, "Update refused: not signed by the release key");
            } else if (newer) {
                char bin[256];
                snprintf(bin, sizeof(bin), "%s", jurl->valuestring);
                lock(); snprintf(s_avail_ver, sizeof(s_avail_ver), "%s", jver->valuestring);
                        snprintf(s_bin_url, sizeof(s_bin_url), "%s", bin);
                        s_expect = e; s_expect_set = true; unlock();
                cJSON_Delete(root); root = nullptr;
                esp_err_t err = perform_update(bin, e);
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

// Background watch tick (see nv_ota_watch_start): one manifest read on a short-lived task.
void watch_task(void *) {
    char url[256];
    nv_ota_get_url(url, sizeof(url));
    NV_PSRAM_BSS static char body[4096];   // see check_task
    if (!fetch_manifest(url, body, sizeof(body))) {
        NV_LOGW(TAG, "watch: manifest unreachable (%s)", url);
    } else if (cJSON *root = cJSON_Parse(body)) {
        cJSON *jver = cJSON_GetObjectItem(root, "version");
        cJSON *jurl = cJSON_GetObjectItem(root, "url");
        if (cJSON_IsString(jver) && cJSON_IsString(jurl)) {
            const bool newer = version_is_newer(jver->valuestring, running_version());
            NV_LOGI(TAG, "watch: offered v%s vs running v%s -> %s", jver->valuestring,
                    running_version(), newer ? "available" : "up-to-date");
            Expect e = {};
            if (newer && manifest_verify(root, jver->valuestring, &e)) {   // unsigned: not announced
                lock();
                snprintf(s_avail_ver, sizeof(s_avail_ver), "%s", jver->valuestring);
                snprintf(s_bin_url, sizeof(s_bin_url), "%s", jurl->valuestring);
                s_expect = e;
                s_expect_set = true;
                unlock();
                char m[64];
                snprintf(m, sizeof(m), "Version %s available", jver->valuestring);
                set_state(NV_OTA_AVAILABLE, m);
            }
        }
        cJSON_Delete(root);
    }
    lock(); s_busy = false; unlock();
    vTaskDelete(nullptr);
}

esp_timer_handle_t s_watch_timer = nullptr;
bool s_watch_periodic = false;

void watch_timer_cb(void *) {
    if (!s_watch_periodic) {   // the first tick came 15 min after boot; from now on every 6 h
        s_watch_periodic = true;
        esp_timer_start_periodic(s_watch_timer, 6ULL * 3600 * 1000 * 1000);
    }
    lock();
    // A check or download in flight owns the state, and an image already written (SUCCESS) is
    // waiting for its reboot: neither needs to hear about the manifest again.
    const bool skip = s_busy || s_state == NV_OTA_SUCCESS;
    if (!skip) s_busy = true;
    unlock();
    if (skip) return;
    // 8 KB internal stack like the boot updater: TLS handshake + cert-bundle verify
    if (xTaskCreate(watch_task, "ota_watch", 8192, nullptr, 3, nullptr) != pdPASS) {
        lock(); s_busy = false; unlock();
    }
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
            // otadata erase+write on a dedicated internal-stack task, not the shared esp_timer
            // task (3.5 KB stack; the LVGL tick timer lives there too and would stall for the erase).
            auto mark = [](void *) {
                esp_ota_mark_app_valid_cancel_rollback();
                NV_LOGI(TAG, "image survived 60 s -> marked valid (rollback cancelled)");
                vTaskDelete(nullptr);
            };
            if (xTaskCreate(mark, "ota_valid", 4096, nullptr, 5, nullptr) != pdPASS)
                esp_ota_mark_app_valid_cancel_rollback();   // still confirm — never leave it pending
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
    if (n && !out[0]) snprintf(out, n, "%s", NV_OTA_DEFAULT_URL);   // empty NVS value -> default
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
