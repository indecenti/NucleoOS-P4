// nv_backup — mirror NVS <-> SD. See nv_backup.h.
#include "nv_backup.h"
#include "nv_sd.h"
#include "nv_log.h"
#include "nv_event_bus.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

static const char *TAG = "backup";

namespace {

constexpr char kDir[]  = "/sdcard/nucleos";
constexpr char kFile[] = "/sdcard/nucleos/settings.nvb";
constexpr uint8_t kMagic[4] = { 'N', 'V', 'B', '1' };
constexpr int kValMax = 1024;   // largest NVS blob we hold (Wi-Fi creds ~784B)

esp_timer_handle_t s_debounce = nullptr;
// Serializes export/import: export is called from BOTH the LVGL task ("Back up now") and the
// esp_timer debounce task (auto-export on settings change). Without this they interleave on the
// shared static val[] + same output file and corrupt the mirror.
SemaphoreHandle_t s_lock = nullptr;

// ---- one NVS entry <-> file record --------------------------------------------------------
// record: [u8 nsLen][ns][u8 keyLen][key][u8 type][u16 valLen][val]
void put_u8(FILE *f, uint8_t v)  { fputc(v, f); }
void put_u16(FILE *f, uint16_t v){ fputc(v & 0xFF, f); fputc(v >> 8, f); }
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

void debounce_cb(void *) { nv_backup_export(); }

void on_settings_changed(nv_event_t, const void *, void *) {
    if (!s_debounce) return;
    esp_timer_stop(s_debounce);
    esp_timer_start_once(s_debounce, 3 * 1000 * 1000);   // coalesce a burst of set()s
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
    return remove(kFile) == 0 || !nv_backup_available();
}

bool nv_backup_export(void) {
    if (!nv_sd_is_mounted()) return false;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);

    mkdir(kDir, 0777);   // ok if it already exists
    // Write to a temp file and rename over the real one only once it's complete: never truncate
    // the good backup in place (a crash / card-pull mid-write, or a spurious empty enumeration,
    // must not leave a partial settings.nvb that later restores garbage into NVS).
    char tmp[80];
    snprintf(tmp, sizeof tmp, "%s.tmp", kFile);
    int count = 0;
    FILE *f = nv_sd_fopen(tmp, "wb");
    if (f) {
        fwrite(kMagic, 1, sizeof(kMagic), f);
        static uint8_t val[kValMax];   // static: keep it off the caller stack (guarded by s_lock)
        nvs_iterator_t it = nullptr;
        esp_err_t r = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
        while (r == ESP_OK) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            int len = read_value(info.namespace_name, info.key, info.type, val, kValMax);
            if (len >= 0) {
                put_u8(f, (uint8_t)strlen(info.namespace_name));
                fwrite(info.namespace_name, 1, strlen(info.namespace_name), f);
                put_u8(f, (uint8_t)strlen(info.key));
                fwrite(info.key, 1, strlen(info.key), f);
                put_u8(f, (uint8_t)info.type);
                put_u16(f, (uint16_t)len);
                fwrite(val, 1, (size_t)len, f);
                count++;
            }
            r = nvs_entry_next(&it);
        }
        if (it) nvs_release_iterator(it);
        fflush(f);
        nv_sd_fclose(f);
        if (count > 0) {
            remove(kFile);                       // FATFS rename won't overwrite an existing dest
            if (rename(tmp, kFile) != 0) {        // rename failed: keep tmp as a recoverable copy
                NV_LOGW(TAG, "export: rename failed, backup left as %s", tmp);
                count = 0;
            }
        } else {
            remove(tmp);                          // nothing useful -> leave any existing good backup
        }
    } else {
        NV_LOGW(TAG, "export: cannot open %s", tmp);
    }

    if (s_lock) xSemaphoreGive(s_lock);
    if (count > 0) NV_LOGI(TAG, "exported %d NVS entries to SD", count);
    return count > 0;
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

    static uint8_t val[kValMax];
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
