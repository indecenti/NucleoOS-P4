// nv_backup — mirror NVS <-> SD. See nv_backup.h.
#include "nv_backup.h"
#include "nv_sd.h"
#include "nv_log.h"
#include "nv_event_bus.h"
#include "nv_mem_attr.h"
#include "nv_config.h"   // nv_config_encrypted / nv_config_device_key

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <climits>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

static const char *TAG = "backup";

namespace {

constexpr char kDir[]  = "/sdcard/nucleos";
constexpr char kFile[] = "/sdcard/nucleos/settings.nvb";
// NVB1: the plaintext record stream (older firmware, or a chip without the NVS eFuse key).
// NVB2: "NVB2" | iv[12] | tag[16] | AES-256-GCM(records), keyed by nv_config_device_key(): the
// card is removable plain FAT, so with an encrypted NVS the mirror must not hand the Wi-Fi
// passwords, PINs and tokens to whoever reads it. Only this chip can open it.
constexpr uint8_t kMagic[4]   = { 'N', 'V', 'B', '1' };
constexpr uint8_t kMagicV2[4] = { 'N', 'V', 'B', '2' };
constexpr size_t  kIvLen = 12, kTagLen = 16;
constexpr size_t  kHdrV2 = sizeof(kMagicV2) + kIvLen + kTagLen;
constexpr size_t  kFileMax = 256 * 1024;   // sanity cap on what import reads into PSRAM
constexpr int kValMax = 1024;   // largest NVS blob we hold (Wi-Fi creds ~784B)
constexpr uint32_t kExportStack = 6144;   // internal: the export reads NVS (ENGINEERING_RULES §2)

// Auto-export pacing. Every NV_EV_SETTINGS_CHANGED used to re-export 3 s later, and launch
// counters, the launcher page, Wi-Fi re-joins... fire it every few seconds while the device is
// used. Each export is ~2 flash reads per NVS entry (~260 cache-off IPC handshakes that stall both
// cores) plus a tmp-write/remove/rename on the SD — for a mirror that is only needed after an NVS
// wipe. So: coalesce bursts (debounce), never export more than once per kMinGapUs, and skip the SD
// write entirely when the serialized NVS is byte-identical to what the card already holds.
constexpr uint64_t kDebounceUs = 3ULL * 1000 * 1000;
constexpr int64_t  kMinGapUs   = 60LL * 1000 * 1000;

esp_timer_handle_t s_debounce = nullptr;
// Serializes export/import: export is called from BOTH the LVGL task ("Back up now") and the
// export task (auto-export on settings change). Without this they interleave on the shared
// static val[] + same output file and corrupt the mirror.
SemaphoreHandle_t s_lock = nullptr;

int64_t  s_last_export_us = 0;    // end of the last auto-export (0 = none yet this boot)
bool     s_crc_valid = false;     // s_file_* describe what kFile holds (s_lock)
uint32_t s_file_crc = 0;
size_t   s_file_len = 0;
time_t   s_file_mtime = 0;        // with the size: detects a swapped card / an external edit
uint32_t s_min_stack_free = UINT32_MAX;   // export task stack high-water mark, lowest seen

// ---- one NVS entry <-> file record --------------------------------------------------------
// record: [u8 nsLen][ns][u8 keyLen][key][u8 type][u16 valLen][val]
// Export serializes into a PSRAM buffer first: the CRC comparison needs the whole image, and one
// fwrite beats ~500 fputc/fwrite calls through newlib + FATFS.
struct Image {
    uint8_t *p = nullptr;
    size_t   n = 0, cap = 0;
    bool     oom = false;
    void put(const void *d, size_t len) {
        if (oom) return;
        if (n + len > cap) {
            size_t c = cap ? cap * 2 : 8192;
            while (c < n + len) c *= 2;
            void *q = heap_caps_realloc(p, c, MALLOC_CAP_SPIRAM);
            if (!q) { oom = true; return; }
            p = static_cast<uint8_t *>(q);
            cap = c;
        }
        memcpy(p + n, d, len);
        n += len;
    }
    void put_u8(uint8_t v)   { put(&v, 1); }
    void put_u16(uint16_t v) { const uint8_t b[2] = { uint8_t(v & 0xFF), uint8_t(v >> 8) }; put(b, 2); }
    ~Image() { heap_caps_free(p); }
};
// Two keys from the chip's eFuse HMAC key: one encrypts, one picks the IV. The IV is an HMAC of
// the plaintext, so unchanged settings seal to a byte-identical file (the skip-unchanged check in
// export keeps working) and different settings never share an IV under the same key.
bool backup_keys(uint8_t enc[32], uint8_t ivk[32]) {
    return nv_config_device_key("nucleo-sd-backup-enc-v1", enc) &&
           nv_config_device_key("nucleo-sd-backup-iv-v1", ivk);
}

// records -> NVB2 file image.
bool seal(const uint8_t *rec, size_t n, Image &out) {
    uint8_t enc[32], ivk[32], mac[32];
    if (!backup_keys(enc, ivk)) return false;
    bool ok = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), ivk, sizeof ivk, rec, n, mac) == 0;
    const uint8_t zero[kTagLen] = {};
    out.put(kMagicV2, sizeof kMagicV2);
    out.put(mac, kIvLen);
    out.put(zero, kTagLen);            // tag, filled below
    const size_t off = out.n;
    for (size_t i = 0; ok && i < n; i += sizeof zero) out.put(zero, n - i < sizeof zero ? n - i : sizeof zero);
    ok = ok && !out.oom;
    if (ok) {
        mbedtls_gcm_context g;
        mbedtls_gcm_init(&g);
        ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, enc, 256) == 0 &&
             mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, out.p + sizeof kMagicV2, kIvLen,
                                       kMagicV2, sizeof kMagicV2, rec, out.p + off,
                                       kTagLen, out.p + sizeof kMagicV2 + kIvLen) == 0;
        mbedtls_gcm_free(&g);
    }
    memset(enc, 0, sizeof enc);
    memset(ivk, 0, sizeof ivk);
    return ok;
}

// NVB2 file image -> records (n - kHdrV2 bytes into `plain`). false: another chip, or tampered.
bool unseal(const uint8_t *file, size_t n, uint8_t *plain) {
    uint8_t enc[32], ivk[32];
    if (n < kHdrV2 || !backup_keys(enc, ivk)) return false;
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    const bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, enc, 256) == 0 &&
                    mbedtls_gcm_auth_decrypt(&g, n - kHdrV2, file + sizeof kMagicV2, kIvLen,
                                             kMagicV2, sizeof kMagicV2,
                                             file + sizeof kMagicV2 + kIvLen, kTagLen,
                                             file + kHdrV2, plain) == 0;
    mbedtls_gcm_free(&g);
    memset(enc, 0, sizeof enc);
    memset(ivk, 0, sizeof ivk);
    return ok;
}

// Read the value of one entry into buf; returns length, or -1 to skip (unsupported/failed).
int read_value(const char *ns, const char *key, nvs_type_t type, uint8_t *buf, int cap) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return -1;
    int len = -1;
    esp_err_t e = ESP_FAIL;
    switch (type) {
        case NVS_TYPE_I8:  { int8_t   v; e = nvs_get_i8 (h, key, &v); if (e == ESP_OK){ memcpy(buf,&v,1); len=1;} } break;
        case NVS_TYPE_U8:  { uint8_t  v; e = nvs_get_u8 (h, key, &v); if (e == ESP_OK){ memcpy(buf,&v,1); len=1;} } break;
        case NVS_TYPE_I16: { int16_t  v; e = nvs_get_i16(h, key, &v); if (e == ESP_OK){ memcpy(buf,&v,2); len=2;} } break;
        case NVS_TYPE_U16: { uint16_t v; e = nvs_get_u16(h, key, &v); if (e == ESP_OK){ memcpy(buf,&v,2); len=2;} } break;
        case NVS_TYPE_I32: { int32_t  v; e = nvs_get_i32(h, key, &v); if (e == ESP_OK){ memcpy(buf,&v,4); len=4;} } break;
        case NVS_TYPE_U32: { uint32_t v; e = nvs_get_u32(h, key, &v); if (e == ESP_OK){ memcpy(buf,&v,4); len=4;} } break;
        case NVS_TYPE_I64: { int64_t  v; e = nvs_get_i64(h, key, &v); if (e == ESP_OK){ memcpy(buf,&v,8); len=8;} } break;
        case NVS_TYPE_U64: { uint64_t v; e = nvs_get_u64(h, key, &v); if (e == ESP_OK){ memcpy(buf,&v,8); len=8;} } break;
        case NVS_TYPE_STR: { size_t n = cap; e = nvs_get_str (h, key, (char *)buf, &n); if (e == ESP_OK) len = (int)n; } break;
        case NVS_TYPE_BLOB:{ size_t n = cap; e = nvs_get_blob(h, key, buf, &n);         if (e == ESP_OK) len = (int)n; } break;
        default: break;
    }
    nvs_close(h);
    return len;
}

void write_value(const char *ns, const char *key, nvs_type_t type, const uint8_t *buf, int len) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
    switch (type) {
        case NVS_TYPE_I8:   nvs_set_i8 (h, key, *(const int8_t   *)buf); break;
        case NVS_TYPE_U8:   nvs_set_u8 (h, key, *(const uint8_t  *)buf); break;
        case NVS_TYPE_I16:  nvs_set_i16(h, key, *(const int16_t  *)buf); break;
        case NVS_TYPE_U16:  nvs_set_u16(h, key, *(const uint16_t *)buf); break;
        case NVS_TYPE_I32:  nvs_set_i32(h, key, *(const int32_t  *)buf); break;
        case NVS_TYPE_U32:  nvs_set_u32(h, key, *(const uint32_t *)buf); break;
        case NVS_TYPE_I64:  nvs_set_i64(h, key, *(const int64_t  *)buf); break;
        case NVS_TYPE_U64:  nvs_set_u64(h, key, *(const uint64_t *)buf); break;
        case NVS_TYPE_STR:  nvs_set_str (h, key, (const char *)buf);     break;
        case NVS_TYPE_BLOB: nvs_set_blob(h, key, buf, (size_t)len);      break;
        default: break;
    }
    nvs_commit(h);
    nvs_close(h);
}

// Apply a record stream; returns the number of entries written.
int apply_records(const uint8_t *p, size_t n) {
    NV_PSRAM_BSS static uint8_t val[kValMax];   // nvs_set_* bounce non-internal sources (s_lock)
    char ns[16], key[16];
    int count = 0;
    size_t i = 0;
    while (i < n) {
        const size_t nsl = p[i++];
        if (nsl > 15 || i + nsl > n) break;
        memcpy(ns, p + i, nsl); ns[nsl] = '\0'; i += nsl;
        if (i >= n) break;
        const size_t kl = p[i++];
        if (kl > 15 || i + kl + 3 > n) break;
        memcpy(key, p + i, kl); key[kl] = '\0'; i += kl;
        const int type = p[i++];
        const size_t len = (size_t)p[i] | ((size_t)p[i + 1] << 8);
        i += 2;
        if (len > (size_t)kValMax || i + len > n) break;
        memcpy(val, p + i, len);
        i += len;
        // A record must match its type: fixed-size values exactly, strings NUL-terminated inside the
        // record (nvs_set_str runs strlen on it). A damaged or crafted record is skipped, not written.
        size_t want = 0;
        switch ((nvs_type_t)type) {
            case NVS_TYPE_I8:  case NVS_TYPE_U8:  want = 1; break;
            case NVS_TYPE_I16: case NVS_TYPE_U16: want = 2; break;
            case NVS_TYPE_I32: case NVS_TYPE_U32: want = 4; break;
            case NVS_TYPE_I64: case NVS_TYPE_U64: want = 8; break;
            case NVS_TYPE_STR:  if (len < 1 || val[len - 1] != 0) continue; break;
            case NVS_TYPE_BLOB: break;
            default: continue;
        }
        if (want && len != want) continue;
        write_value(ns, key, (nvs_type_t)type, val, (int)len);
        count++;
    }
    memset(val, 0, sizeof val);
    return count;
}

bool nvcfg_empty(void) {   // "nvcfg" holds all user prefs; no keys => NVS was wiped/fresh
    nvs_iterator_t it = nullptr;
    esp_err_t r = nvs_entry_find(NVS_DEFAULT_PART_NAME, "nvcfg", NVS_TYPE_ANY, &it);
    if (it) nvs_release_iterator(it);
    return r != ESP_OK;
}

// The export itself must NOT run on the esp_timer task: it iterates NVS (cache-disabling flash
// reads) and writes the SD for tens to hundreds of ms, and the LVGL tick is an esp_timer on that
// very task — so every app launch (usage counter -> settings-changed -> export) froze touch and
// animations for the whole export. Run it on a short-lived task with an INTERNAL stack (flash
// access forbids a PSRAM stack; self-deleting tasks must not use xTaskCreateWithCaps anyway).
volatile bool s_export_running = false;

void export_task(void *) {
    nv_backup_export();
    s_last_export_us = esp_timer_get_time();
    // Stack headroom, measured on the device (ESP-IDF reports bytes). Logged only when a run goes
    // deeper than every previous one, so the first export of a boot always reports it.
    const uint32_t free_b = uxTaskGetStackHighWaterMark(nullptr);
    if (free_b < s_min_stack_free) {
        s_min_stack_free = free_b;
        NV_LOGI(TAG, "export task stack: %lu of %lu B never used",
                (unsigned long)free_b, (unsigned long)kExportStack);
    }
    s_export_running = false;
    vTaskDelete(nullptr);
}

void debounce_cb(void *) {
    if (s_export_running) {                      // previous export still writing: try again later
        if (s_debounce) esp_timer_start_once(s_debounce, kDebounceUs);
        return;
    }
    const int64_t since = esp_timer_get_time() - s_last_export_us;
    if (s_last_export_us && since < kMinGapUs) {  // exported recently: fold this change into the next slot
        if (s_debounce) esp_timer_start_once(s_debounce, (uint64_t)(kMinGapUs - since));
        return;
    }
    s_export_running = true;
    if (xTaskCreate(export_task, "nv_bkexp", kExportStack, nullptr, 3, nullptr) != pdPASS) {
        s_export_running = false;
        NV_LOGW(TAG, "export task create failed");
    }
}

void on_settings_changed(nv_event_t, const void *, void *) {
    if (!s_debounce) return;
    esp_timer_stop(s_debounce);
    esp_timer_start_once(s_debounce, kDebounceUs);   // coalesce a burst of set()s
}

// CRC of what kFile holds now, so the first export after boot can skip an identical rewrite.
// Called under s_lock; `scratch` is the shared val[] buffer.
bool file_crc(uint32_t *crc, size_t *len, time_t *mtime, uint8_t *scratch, size_t cap) {
    struct stat st;
    if (stat(kFile, &st) != 0) return false;
    FILE *f = nv_sd_fopen(kFile, "rb");
    if (!f) return false;
    uint32_t c = 0;
    size_t total = 0, n;
    while ((n = fread(scratch, 1, cap, f)) > 0) {
        c = esp_rom_crc32_le(c, scratch, n);
        total += n;
    }
    const bool ok = !ferror(f);
    nv_sd_fclose(f);
    if (ok) { *crc = c; *len = total; *mtime = st.st_mtime; }
    return ok;
}

}  // namespace

bool nv_backup_available(void) {
    struct stat st;
    return nv_sd_is_mounted() && stat(kFile, &st) == 0 && st.st_size > (off_t)sizeof(kMagic);
}

bool nv_backup_delete(void) {
    // Factory reset relies on this: with the SD mirror gone, the restore-if-empty logic at the
    // next boot has nothing to bring back, so the wiped NVS truly starts fresh.
    if (!nv_sd_is_mounted()) return true;   // no card -> no backup to defeat the reset
    s_crc_valid = false;
    return remove(kFile) == 0 || !nv_backup_available();
}

bool nv_backup_export(void) {
    if (!nv_sd_is_mounted()) return false;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);

    NV_PSRAM_BSS static uint8_t val[kValMax];   // off the stack (guarded by s_lock); nvs_get_* bounce into it
    const bool sealed = nv_config_encrypted();
    Image rec;                                   // the record stream (sealed into another image for NVB2)
    if (!sealed) rec.put(kMagic, sizeof(kMagic));
    int count = 0;
    nvs_iterator_t it = nullptr;
    esp_err_t r = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
    while (r == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        // Third-party service secrets stay on the device: the card is removable and plain FAT.
        // A restore just asks for them again (docs/HOME_AUTOMATION_PLAN.md §10.2).
        if (!strcmp(info.namespace_name, "nvcfg") &&
            (!strcmp(info.key, "mqtt_pass") || !strcmp(info.key, "ha_token"))) {
            r = nvs_entry_next(&it);
            continue;
        }
        int len = read_value(info.namespace_name, info.key, info.type, val, kValMax);
        if (len >= 0) {
            rec.put_u8((uint8_t)strlen(info.namespace_name));
            rec.put(info.namespace_name, strlen(info.namespace_name));
            rec.put_u8((uint8_t)strlen(info.key));
            rec.put(info.key, strlen(info.key));
            rec.put_u8((uint8_t)info.type);
            rec.put_u16((uint16_t)len);
            rec.put(val, (size_t)len);
            count++;
        }
        r = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
    memset(val, 0, sizeof val);

    Image sealed_img;
    if (sealed && !rec.oom && count > 0 && !seal(rec.p, rec.n, sealed_img)) sealed_img.oom = true;
    if (sealed && rec.p) memset(rec.p, 0, rec.n);   // plaintext secrets: don't leave them in PSRAM
    Image &img = sealed ? sealed_img : rec;
    if (rec.oom) img.oom = true;

    bool ok = false, written = false;
    if (img.oom) {
        NV_LOGW(TAG, "export: out of memory serializing %d entries", count);
    } else if (count > 0) {
        const uint32_t crc = esp_rom_crc32_le(0, img.p, img.n);
        if (!s_crc_valid) s_crc_valid = file_crc(&s_file_crc, &s_file_len, &s_file_mtime, val, kValMax);
        struct stat st;
        if (s_crc_valid && s_file_crc == crc && s_file_len == img.n && stat(kFile, &st) == 0 &&
            (size_t)st.st_size == img.n && st.st_mtime == s_file_mtime) {
            ok = true;                            // the card already holds exactly this: no SD write
        } else {
            mkdir(kDir, 0777);   // ok if it already exists
            // Write to a temp file and rename over the real one only once it's complete: never
            // truncate the good backup in place (a crash / card-pull mid-write, or a spurious empty
            // enumeration, must not leave a partial settings.nvb that later restores garbage).
            char tmp[80];
            snprintf(tmp, sizeof tmp, "%s.tmp", kFile);
            FILE *f = nv_sd_fopen(tmp, "wb");
            if (f) {
                const bool full = fwrite(img.p, 1, img.n, f) == img.n && fflush(f) == 0;
                const bool closed = nv_sd_fclose(f) == 0;
                s_crc_valid = false;              // kFile is about to change (or vanish)
                if (!full || !closed) {
                    remove(tmp);                  // short write: keep the existing good backup
                    NV_LOGW(TAG, "export: write to %s failed", tmp);
                } else {
                    remove(kFile);                // FATFS rename won't overwrite an existing dest
                    if (rename(tmp, kFile) != 0) {    // rename failed: keep tmp as a recoverable copy
                        NV_LOGW(TAG, "export: rename failed, backup left as %s", tmp);
                    } else {
                        ok = written = true;
                        if (stat(kFile, &st) == 0) {
                            s_file_crc = crc;
                            s_file_len = img.n;
                            s_file_mtime = st.st_mtime;
                            s_crc_valid = true;
                        }
                    }
                }
            } else {
                NV_LOGW(TAG, "export: cannot open %s", tmp);
            }
        }
    }

    if (s_lock) xSemaphoreGive(s_lock);
    if (written) NV_LOGI(TAG, "exported %d NVS entries (%u B) to SD", count, (unsigned)img.n);
    return ok;
}

bool nv_backup_import(void) {
    if (!nv_sd_is_mounted()) return false;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    struct stat st;
    FILE *f = (stat(kFile, &st) == 0 && st.st_size > (off_t)sizeof(kMagic) && st.st_size <= (off_t)kFileMax)
                  ? nv_sd_fopen(kFile, "rb") : nullptr;
    const size_t n = f ? (size_t)st.st_size : 0;
    uint8_t *buf = f ? static_cast<uint8_t *>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM)) : nullptr;
    const bool got = buf && fread(buf, 1, n, f) == n;
    if (f) nv_sd_fclose(f);

    int count = -1;
    if (got && memcmp(buf, kMagic, sizeof kMagic) == 0) {
        // Plaintext (older firmware). With encrypted NVS anyone holding the card could forge one and
        // plant settings (PINs, tokens) that a wipe would then restore: only sealed backups count.
        if (nv_config_encrypted()) NV_LOGW(TAG, "import: plaintext backup refused (NVS is encrypted)");
        else count = apply_records(buf + sizeof kMagic, n - sizeof kMagic);
    } else if (got && n >= kHdrV2 && memcmp(buf, kMagicV2, sizeof kMagicV2) == 0) {
        uint8_t *plain = static_cast<uint8_t *>(heap_caps_malloc(n - kHdrV2 + 1, MALLOC_CAP_SPIRAM));
        if (plain && unseal(buf, n, plain)) count = apply_records(plain, n - kHdrV2);
        else NV_LOGW(TAG, "import: backup sealed by another device (or damaged), ignored");
        if (plain) { memset(plain, 0, n - kHdrV2); heap_caps_free(plain); }
    }
    if (buf) { memset(buf, 0, n); heap_caps_free(buf); }
    if (s_lock) xSemaphoreGive(s_lock);
    if (count >= 0) NV_LOGI(TAG, "imported %d NVS entries from SD", count);
    return count > 0;
}

void nv_backup_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    // Restore before the UI reads any preference: only when NVS is empty (a wipe/fresh chip) and
    // a backup exists — never clobber good NVS with a stale card.
    if (nvcfg_empty() && nv_backup_available()) {
        NV_LOGW(TAG, "NVS empty; restoring settings from SD backup");
        nv_backup_import();
    }
    // Auto-backup: re-export (debounced) whenever a setting changes.
    const esp_timer_create_args_t a = { debounce_cb, nullptr, ESP_TIMER_TASK, "nvbackup", true };
    esp_timer_create(&a, &s_debounce);
    nv_event_subscribe(NV_EV_SETTINGS_CHANGED, on_settings_changed, nullptr);
    // NVS is encrypted but the card still holds a plaintext NVB1 (first boot after the migration):
    // re-export now instead of at the next settings change, so the secrets leave the card.
    uint8_t magic[sizeof kMagic] = {};
    FILE *f = (nv_config_encrypted() && nv_backup_available()) ? nv_sd_fopen(kFile, "rb") : nullptr;
    if (f) {
        const bool v1 = fread(magic, 1, sizeof magic, f) == sizeof magic && memcmp(magic, kMagic, sizeof kMagic) == 0;
        nv_sd_fclose(f);
        if (v1 && s_debounce) esp_timer_start_once(s_debounce, kDebounceUs);
    }
}
