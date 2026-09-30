// nv_config_secure — bring the default NVS partition up ENCRYPTED and migrate a plaintext one.
//
// NVS encryption (XTS-AES) with the HMAC key-protection scheme: on the first boot of a firmware
// built with it the chip generates a random 256-bit key and burns it into eFuse block
// KEY<CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID> with purpose HMAC_UP. That block is read-protected, so the
// key never leaves the chip: the NVS keys are derived from it by the HMAC peripheral at every boot.
// No flash encryption and no Secure Boot needed; flashing over USB keeps working.
//
// The trap this file exists for: nvs_flash_init() on an encryption build happily mounts a partition
// written in PLAINTEXT by older firmware — every entry then fails its CRC after "decryption" and is
// silently dropped (Wi-Fi networks, paired browsers, every setting). So the first boot snapshots the
// plaintext entries into PSRAM, burns the key, erases, mounts encrypted and writes them back. Later
// boots check the raw partition for plaintext entries too (a firmware without encryption flashed
// over USB since) and re-encrypt them the same way. The reverse cannot be helped: firmware without
// encryption on a chip with the key burnt sees an empty store. Power lost between the erase and the
// write-back loses the settings; nv_backup then restores them from the SD mirror like after a wipe.
#include "nv_config.h"
#include "nv_log.h"
#include "nv_seclog.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#if CONFIG_NVS_ENCRYPTION && CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC
#define NV_NVS_HMAC 1
#include "esp_efuse.h"
#include "esp_hmac.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#endif

#include <stdio.h>
#include <string.h>

static const char *TAG = "cfg";

namespace {

bool s_encrypted = false;

// Mount the default partition, wiping it when it is full or from a newer NVS format.
esp_err_t mount(bool secure, nvs_sec_cfg_t *cfg) {
    esp_err_t e = secure ? nvs_flash_secure_init(cfg) : nvs_flash_init_partition(NVS_DEFAULT_PART_NAME);
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(NVS_DEFAULT_PART_NAME);
        e = secure ? nvs_flash_secure_init(cfg) : nvs_flash_init_partition(NVS_DEFAULT_PART_NAME);
    }
    return e;
}

#if NV_NVS_HMAC
constexpr int kKeyId = CONFIG_NVS_SEC_HMAC_EFUSE_KEY_ID;

bool key_burnt(void) {
    const auto blk = (esp_efuse_block_t)(EFUSE_BLK_KEY0 + kKeyId);
    return esp_efuse_get_key_purpose(blk) == ESP_EFUSE_KEY_PURPOSE_HMAC_UP;
}

// Every entry of the plaintext partition, held in PSRAM across the erase.
struct Entry {
    char       ns[NVS_NS_NAME_MAX_SIZE];
    char       key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
    size_t     len;
    uint8_t   *val;
};

struct Snapshot {
    Entry *e = nullptr;
    int    n = 0, cap = 0;
    bool   oom = false;
    ~Snapshot() {
        for (int i = 0; i < n; i++) {
            if (e[i].val) memset(e[i].val, 0, e[i].len);   // secrets: don't leave them in PSRAM
            heap_caps_free(e[i].val);
        }
        heap_caps_free(e);
    }
    Entry *push(void) {
        if (n == cap) {
            const int c = cap ? cap * 2 : 64;
            void *q = heap_caps_realloc(e, c * sizeof(Entry), MALLOC_CAP_SPIRAM);
            if (!q) { oom = true; return nullptr; }
            e = static_cast<Entry *>(q);
            cap = c;
        }
        Entry *x = &e[n++];
        memset(x, 0, sizeof *x);
        return x;
    }
};

enum class Read { Ok, Skip, Oom };

Read read_entry(const nvs_entry_info_t &info, Entry *x) {
    snprintf(x->ns, sizeof x->ns, "%s", info.namespace_name);
    snprintf(x->key, sizeof x->key, "%s", info.key);
    x->type = info.type;
    nvs_handle_t h;
    if (nvs_open(info.namespace_name, NVS_READONLY, &h) != ESP_OK) return Read::Skip;
    esp_err_t e = ESP_FAIL;
    size_t len = 0;
    if (info.type == NVS_TYPE_STR) e = nvs_get_str(h, info.key, nullptr, &len);
    else if (info.type == NVS_TYPE_BLOB) e = nvs_get_blob(h, info.key, nullptr, &len);
    else if (info.type != NVS_TYPE_ANY) { len = (size_t)(info.type & 0x0F); e = ESP_OK; }   // 1/2/4/8
    if (e != ESP_OK || !len) { nvs_close(h); return Read::Skip; }   // unknown type / empty blob
    x->val = static_cast<uint8_t *>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
    if (!x->val) { nvs_close(h); return Read::Oom; }
    x->len = len;
    switch (info.type) {
        case NVS_TYPE_U8:   e = nvs_get_u8 (h, info.key, (uint8_t  *)x->val); break;
        case NVS_TYPE_I8:   e = nvs_get_i8 (h, info.key, (int8_t   *)x->val); break;
        case NVS_TYPE_U16:  e = nvs_get_u16(h, info.key, (uint16_t *)x->val); break;
        case NVS_TYPE_I16:  e = nvs_get_i16(h, info.key, (int16_t  *)x->val); break;
        case NVS_TYPE_U32:  e = nvs_get_u32(h, info.key, (uint32_t *)x->val); break;
        case NVS_TYPE_I32:  e = nvs_get_i32(h, info.key, (int32_t  *)x->val); break;
        case NVS_TYPE_U64:  e = nvs_get_u64(h, info.key, (uint64_t *)x->val); break;
        case NVS_TYPE_I64:  e = nvs_get_i64(h, info.key, (int64_t  *)x->val); break;
        case NVS_TYPE_STR:  e = nvs_get_str (h, info.key, (char *)x->val, &len); break;
        case NVS_TYPE_BLOB: e = nvs_get_blob(h, info.key, x->val, &len); break;
        default:            e = ESP_FAIL; break;
    }
    nvs_close(h);
    return e == ESP_OK ? Read::Ok : Read::Skip;
}

void take(Snapshot &s) {
    nvs_iterator_t it = nullptr;
    esp_err_t r = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
    while (r == ESP_OK && !s.oom) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        Entry *x = s.push();
        const Read rd = x ? read_entry(info, x) : Read::Oom;
        if (x && rd != Read::Ok) {
            heap_caps_free(x->val);
            s.n--;
        }
        if (rd == Read::Oom) s.oom = true;
        r = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);
}

int restore(const Snapshot &s) {
    int ok = 0;
    for (int i = 0; i < s.n; i++) {
        const Entry &x = s.e[i];
        nvs_handle_t h;
        if (nvs_open(x.ns, NVS_READWRITE, &h) != ESP_OK) continue;
        esp_err_t e = ESP_FAIL;
        switch (x.type) {
            case NVS_TYPE_U8:   e = nvs_set_u8 (h, x.key, *(const uint8_t  *)x.val); break;
            case NVS_TYPE_I8:   e = nvs_set_i8 (h, x.key, *(const int8_t   *)x.val); break;
            case NVS_TYPE_U16:  e = nvs_set_u16(h, x.key, *(const uint16_t *)x.val); break;
            case NVS_TYPE_I16:  e = nvs_set_i16(h, x.key, *(const int16_t  *)x.val); break;
            case NVS_TYPE_U32:  e = nvs_set_u32(h, x.key, *(const uint32_t *)x.val); break;
            case NVS_TYPE_I32:  e = nvs_set_i32(h, x.key, *(const int32_t  *)x.val); break;
            case NVS_TYPE_U64:  e = nvs_set_u64(h, x.key, *(const uint64_t *)x.val); break;
            case NVS_TYPE_I64:  e = nvs_set_i64(h, x.key, *(const int64_t  *)x.val); break;
            case NVS_TYPE_STR:  e = nvs_set_str (h, x.key, (const char *)x.val); break;
            case NVS_TYPE_BLOB: e = nvs_set_blob(h, x.key, x.val, x.len); break;
            default: break;
        }
        if (e == ESP_OK && nvs_commit(h) == ESP_OK) ok++;
        nvs_close(h);
    }
    return ok;
}

// True when the raw partition holds entries written in PLAINTEXT: a firmware without encryption ran
// on a chip whose key is already burnt (a downgrade over USB, a test build). Read-only: mounting it
// either way would drop the other kind's entries (NVS erases entries whose CRC does not match).
// Page and entry-state headers are never encrypted; an encrypted entry passes the plaintext CRC with
// odds of 2^-32, so one valid entry is proof.
bool raw_has_plaintext_entries(void) {
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,
                                                        NVS_DEFAULT_PART_NAME);
    if (!p) return false;
    constexpr size_t kPage = 4096, kEntries = 126, kEntry = 32, kTable = 32, kData = 64;
    auto *pg = static_cast<uint8_t *>(heap_caps_malloc(kPage, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!pg) return false;
    int plain = 0;
    for (size_t off = 0; off + kPage <= p->size && !plain; off += kPage) {
        if (esp_partition_read(p, off, pg, kPage) != ESP_OK) break;
        uint32_t state;
        memcpy(&state, pg, 4);
        if (state == 0xFFFFFFFF || state == 0) continue;        // uninitialized / invalid page
        for (size_t i = 0; i < kEntries; i++) {
            const uint32_t word = *reinterpret_cast<const uint32_t *>(pg + kTable + (i / 16) * 4);
            if (((word >> ((i % 16) * 2)) & 3) != 2) continue;  // not WRITTEN
            const uint8_t *it = pg + kData + i * kEntry;
            uint32_t crc = 0xFFFFFFFF, stored;
            crc = esp_rom_crc32_le(crc, it, 4);                 // nsIndex, datatype, span, chunkIndex
            crc = esp_rom_crc32_le(crc, it + 8, 16);            // key
            crc = esp_rom_crc32_le(crc, it + 24, 8);            // data
            memcpy(&stored, it + 4, 4);
            if (crc == stored) { plain++; break; }
        }
    }
    heap_caps_free(pg);
    return plain > 0;
}

// Snapshot the plaintext partition, then re-create it encrypted with `cfg` and write it all back.
esp_err_t encrypt_in_place(Snapshot &snap, nvs_sec_cfg_t *cfg);

esp_err_t bringup_encrypted(void) {
    nvs_sec_scheme_t *scheme = nvs_flash_get_default_security_scheme();
    nvs_sec_cfg_t cfg = {};
    esp_err_t e;
    if (key_burnt()) {                                          // every boot after the first
        e = nvs_flash_read_security_cfg_v2(scheme, &cfg);
        if (e == ESP_OK && raw_has_plaintext_entries()) {
            // Firmware without encryption wrote here since: bring its entries over, don't drop them.
            NV_LOGW(TAG, "NVS holds plaintext entries under a burnt key: re-encrypting them");
            Snapshot snap;
            if (nvs_flash_init_partition(NVS_DEFAULT_PART_NAME) == ESP_OK) {
                take(snap);
                nvs_flash_deinit_partition(NVS_DEFAULT_PART_NAME);
            }
            if (!snap.oom) {
                e = encrypt_in_place(snap, &cfg);
                memset(&cfg, 0, sizeof cfg);
                return e;
            }
            NV_LOGE(TAG, "re-encryption: out of memory, plaintext entries dropped");
        }
        if (e == ESP_OK) e = mount(true, &cfg);
        memset(&cfg, 0, sizeof cfg);
        s_encrypted = (e == ESP_OK);
        return e;
    }

    // No key yet: the partition is plaintext (older firmware) or blank. Snapshot, then encrypt.
    Snapshot snap;
    e = nvs_flash_init_partition(NVS_DEFAULT_PART_NAME);
    if (e == ESP_OK) {
        take(snap);
        nvs_flash_deinit_partition(NVS_DEFAULT_PART_NAME);
    } else {
        NV_LOGW(TAG, "plaintext NVS unreadable (0x%x): nothing to migrate", (int)e);
    }
    if (snap.oom) {                                             // never trade settings for a malloc
        NV_LOGE(TAG, "NVS migration: out of memory after %d entries, staying unencrypted", snap.n);
        nv_seclog_add(NV_SEC_NVS_PLAIN, "migration out of memory");
        return mount(false, nullptr);
    }
    e = nvs_flash_generate_keys_v2(scheme, &cfg);               // burns eFuse KEY<kKeyId> (once)
    if (e != ESP_OK || !key_burnt()) {
        NV_LOGE(TAG, "NVS encryption key not created (0x%x): settings stay unencrypted", (int)e);
        char why[24];
        snprintf(why, sizeof why, "eFuse key 0x%x", (unsigned)e);
        nv_seclog_add(NV_SEC_NVS_PLAIN, why);
        memset(&cfg, 0, sizeof cfg);
        return mount(false, nullptr);
    }
    NV_LOGW(TAG, "NVS encryption key generated in eFuse KEY%d; encrypting %d entries", kKeyId, snap.n);
    e = encrypt_in_place(snap, &cfg);
    memset(&cfg, 0, sizeof cfg);
    return e;
}

esp_err_t encrypt_in_place(Snapshot &snap, nvs_sec_cfg_t *cfg) {
    nvs_flash_erase_partition(NVS_DEFAULT_PART_NAME);
    const esp_err_t e = mount(true, cfg);
    if (e != ESP_OK) return e;
    const int ok = restore(snap);
    NV_LOGI(TAG, "NVS encrypted: %d/%d entries migrated", ok, snap.n);
    char what[32];
    snprintf(what, sizeof what, "%d/%d entries", ok, snap.n);
    nv_seclog_add(NV_SEC_NVS_ENCRYPTED, what);
    s_encrypted = true;
    return ESP_OK;
}
#endif  // NV_NVS_HMAC

}  // namespace

// Called once by nv_config_init() before anything touches NVS.
esp_err_t nv_config_nvs_bringup(void) {
#if NV_NVS_HMAC
    return bringup_encrypted();
#else
    return mount(false, nullptr);
#endif
}

bool nv_config_encrypted(void) { return s_encrypted; }

bool nv_config_device_key(const char *label, uint8_t out[32]) {
#if NV_NVS_HMAC
    if (!s_encrypted || !label || !out) return false;
    return esp_hmac_calculate((hmac_key_id_t)kKeyId, label, strlen(label), out) == ESP_OK;
#else
    (void)label; (void)out;
    return false;
#endif
}
