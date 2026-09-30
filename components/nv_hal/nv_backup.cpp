// nv_backup — mirror NVS <-> SD. See nv_backup.h.
#include "nv_backup.h"
#include "nv_sd.h"
#include "nv_log.h"
#include "nv_event_bus.h"
#include "nv_mem_attr.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
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
constexpr uint8_t kMagic[4] = { 'N', 'V', 'B', '1' };
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
int  get_u8(FILE *f)  { return fgetc(f); }
int  get_u16(FILE *f) { int lo = fgetc(f); int hi = fgetc(f); return (lo < 0 || hi < 0) ? -1 : (lo | (hi << 8)); }

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
    Image img;
    img.put(kMagic, sizeof(kMagic));
    int count = 0;
    nvs_iterator_t it = nullptr;
    esp_err_t r = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
    while (r == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        // Third-party service secrets stay on the device: the card is removable and plain FAT.
        // A restore just asks for them again (docs/HOME_AUTOMATION_PLAN.md §10.2).
        if (!strcmp(info.namespace_name, "nvcfg") && !strcmp(info.key, "mqtt_pass")) {
            r = nvs_entry_next(&it);
            continue;
        }
        int len = read_value(info.namespace_name, info.key, info.type, val, kValMax);
        if (len >= 0) {
            img.put_u8((uint8_t)strlen(info.namespace_name));
            img.put(info.namespace_name, strlen(info.namespace_name));
            img.put_u8((uint8_t)strlen(info.key));
            img.put(info.key, strlen(info.key));
            img.put_u8((uint8_t)info.type);
            img.put_u16((uint16_t)len);
            img.put(val, (size_t)len);
            count++;
        }
        r = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);

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
    FILE *f = nv_sd_fopen(kFile, "rb");
    if (!f) { if (s_lock) xSemaphoreGive(s_lock); return false; }
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, kMagic, 4) != 0) {
        nv_sd_fclose(f); if (s_lock) xSemaphoreGive(s_lock); return false;
    }

    NV_PSRAM_BSS static uint8_t val[kValMax];   // nvs_set_* bounce non-internal sources
    char ns[16], key[16];
    int count = 0;
    for (;;) {
        int nsl = get_u8(f);
        if (nsl < 0) break;                                 // clean EOF
        if (nsl > 15 || fread(ns, 1, nsl, f) != (size_t)nsl) break;
        ns[nsl] = '\0';
        int kl = get_u8(f);
        if (kl < 0 || kl > 15 || fread(key, 1, kl, f) != (size_t)kl) break;
        key[kl] = '\0';
        int type = get_u8(f);
        int len  = get_u16(f);
        if (type < 0 || len < 0 || len > kValMax) break;
        if (len > 0 && fread(val, 1, len, f) != (size_t)len) break;
        write_value(ns, key, (nvs_type_t)type, val, len);
        count++;
    }
    nv_sd_fclose(f);
    if (s_lock) xSemaphoreGive(s_lock);
    NV_LOGI(TAG, "imported %d NVS entries from SD", count);
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
}
