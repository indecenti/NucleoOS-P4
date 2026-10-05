// nv_fwup_safety — the `assets` partition side of nv_fwup: the journal both apps use to talk across a
// reboot without the SD card, and the last-known-good (LKG) copy of the system that makes a rollback
// possible with no card at all. Layout and record formats: nv_fwup_policy.h.
#include "nv_fwup.h"
#include "nv_fwup_codec.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "bootloader_common.h"
#include "esp_flash_partitions.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace nv_fwup_policy;
namespace codec = nv_fwup_codec;

static const char *TAG = "fwup";

namespace {

constexpr size_t kBuf = 32 * 1024;
constexpr int kProbes = 6;   // ~zlib level 2: 58 % of the image in a few seconds at 360 MHz

void *big_alloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

void report(nv_fwup_progress_cb cb, void *user, uint64_t done, uint64_t total, int lo, int hi) {
    if (!cb || !total) return;
    cb(lo + (int)((hi - lo) * (done > total ? total : done) / total), user);
}

uint32_t pack_version(const char *v) {
    int a = 0, b = 0, c = 0;
    sscanf(v, "%d.%d.%d", &a, &b, &c);
    return (uint32_t)(a * 10000 + b * 100 + c);
}

bool read_header(const esp_partition_t *a, LkgHeader *h) {
    return esp_partition_read(a, kLkgHeaderOffset, h, sizeof *h) == ESP_OK && lkg_valid(*h, (uint32_t)a->size);
}

// ---- hashing a partition range ----
bool sha_range(const esp_partition_t *p, uint32_t off, uint32_t len, uint8_t out[32], uint8_t *buf,
               nv_fwup_progress_cb cb, void *user, int lo, int hi) {
    mbedtls_sha256_context sc;
    mbedtls_sha256_init(&sc);
    mbedtls_sha256_starts(&sc, 0);
    bool ok = true;
    for (uint32_t done = 0; done < len;) {
        const uint32_t n = (len - done) < kBuf ? (len - done) : (uint32_t)kBuf;
        if (esp_partition_read(p, off + done, buf, n) != ESP_OK) { ok = false; break; }
        mbedtls_sha256_update(&sc, buf, n);
        done += n;
        report(cb, user, done, len, lo, hi);
    }
    mbedtls_sha256_finish(&sc, out);
    mbedtls_sha256_free(&sc);
    return ok;
}

// ---- codec plumbing ----
struct PartReader { const esp_partition_t *p; uint32_t base; };
bool part_read(void *u, uint32_t off, void *buf, size_t n) {
    const PartReader *r = static_cast<PartReader *>(u);
    return esp_partition_read(r->p, r->base + off, buf, n) == ESP_OK;
}

// Appends to the LKG data area, erasing 64 KB ahead of the write position (only what is used).
struct LkgWriter {
    const esp_partition_t *a;
    uint32_t pos, erased_to, limit;
    mbedtls_sha256_context sc;
    nv_fwup_progress_cb cb; void *user; uint32_t raw_total; const uint32_t *raw_done;
};
bool lkg_write(void *u, const void *buf, size_t n) {
    LkgWriter *w = static_cast<LkgWriter *>(u);
    if (w->pos + n > w->limit) return false;   // does not fit: caller reports NOSPACE
    while (w->erased_to < w->pos + n) {
        uint32_t step = 64 * 1024;
        if (w->erased_to % step) step = kSector;   // align to a block first
        if (w->erased_to + step > w->a->size) step = kSector;
        if (esp_partition_erase_range(w->a, w->erased_to, step) != ESP_OK) return false;
        w->erased_to += step;
    }
    if (esp_partition_write(w->a, w->pos, buf, n) != ESP_OK) return false;
    mbedtls_sha256_update(&w->sc, (const uint8_t *)buf, n);
    w->pos += (uint32_t)n;
    return true;
}

// Reads the system while compressing it: hashes the raw bytes and reports progress.
struct HashingReader {
    PartReader r;
    mbedtls_sha256_context sc;
    uint32_t done, total;
    nv_fwup_progress_cb cb; void *user; int lo, hi;
};
bool hashing_read(void *u, uint32_t off, void *buf, size_t n) {
    HashingReader *h = static_cast<HashingReader *>(u);
    if (!part_read(&h->r, off, buf, n)) return false;
    mbedtls_sha256_update(&h->sc, (const uint8_t *)buf, n);
    h->done += (uint32_t)n;
    report(h->cb, h->user, h->done, h->total, h->lo, h->hi);
    if ((h->done & 0x7FFFF) < n) vTaskDelay(1);   // every ~512 KB: let the idle task feed the WDT
    return true;
}

// Sink that only hashes (dry-run decompress) or writes into an OTA handle and hashes.
struct HashSink {
    mbedtls_sha256_context sc;
    esp_ota_handle_t ota;   // 0: verify only
    uint32_t done, total;
    nv_fwup_progress_cb cb; void *user; int lo, hi;
};
bool hash_sink(void *u, const void *buf, size_t n) {
    HashSink *s = static_cast<HashSink *>(u);
    if (s->ota && esp_ota_write(s->ota, buf, n) != ESP_OK) return false;
    mbedtls_sha256_update(&s->sc, (const uint8_t *)buf, n);
    s->done += (uint32_t)n;
    report(s->cb, s->user, s->done, s->total, s->lo, s->hi);
    if ((s->done & 0x7FFFF) < n) vTaskDelay(1);   // every ~512 KB: let the idle task feed the WDT
    return true;
}

// Decompress the stored LKG into `sink` and check the raw hash.
nv_fwup_err_t unpack(const esp_partition_t *a, const LkgHeader &h, HashSink *sink, uint8_t *in_buf) {
    void *work = big_alloc(codec::inflate_work_size());
    if (!work) return NV_FWUP_E_NOMEM;
    PartReader r = { a, kLkgDataOffset };
    mbedtls_sha256_init(&sink->sc);
    mbedtls_sha256_starts(&sink->sc, 0);
    codec::Err e = codec::Err::Ok;
    if (h.method == kLkgDeflate) {
        e = codec::inflate(part_read, &r, h.stored_size, hash_sink, sink, h.raw_size, work, in_buf, kBuf);
    } else {
        for (uint32_t off = 0; off < h.raw_size && e == codec::Err::Ok;) {
            const uint32_t n = (h.raw_size - off) < kBuf ? (h.raw_size - off) : (uint32_t)kBuf;
            if (!part_read(&r, off, in_buf, n)) e = codec::Err::Read;
            else if (!hash_sink(sink, in_buf, n)) e = codec::Err::Write;
            off += n;
        }
    }
    free(work);
    uint8_t d[32];
    mbedtls_sha256_finish(&sink->sc, d);
    mbedtls_sha256_free(&sink->sc);
    if (e != codec::Err::Ok) {
        ESP_LOGE(TAG, "LKG v%s: %s", h.version, codec::err_str(e));
        return e == codec::Err::Write ? NV_FWUP_E_IO : NV_FWUP_E_HASH;
    }
    if (memcmp(d, h.raw_sha256, 32) != 0) { ESP_LOGE(TAG, "LKG v%s: raw hash mismatch", h.version); return NV_FWUP_E_HASH; }
    return NV_FWUP_OK;
}

}  // namespace

// ================================================================== resets
Reset nv_fwup_reset_kind(int r) {
    switch ((esp_reset_reason_t)r) {
        case ESP_RST_POWERON: case ESP_RST_EXT: case ESP_RST_USB: case ESP_RST_JTAG: return Reset::PowerOn;
        case ESP_RST_SW:       return Reset::Software;
        case ESP_RST_BROWNOUT: case ESP_RST_PWR_GLITCH: return Reset::Brownout;
        case ESP_RST_PANIC: case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT:
        case ESP_RST_CPU_LOCKUP: return Reset::Crash;
        default:               return Reset::Other;
    }
}

// ================================================================== otadata
esp_err_t nv_fwup_slot_state(const esp_partition_t *p, esp_ota_img_states_t *state) {
    if (!p || !state || p->type != ESP_PARTITION_TYPE_APP || p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
        p->subtype > ESP_PARTITION_SUBTYPE_APP_OTA_MAX)
        return ESP_ERR_INVALID_ARG;
    const esp_partition_t *od = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    const int slots = esp_ota_get_app_partition_count();
    if (!od || slots <= 0) return ESP_ERR_NOT_FOUND;
    const int want = p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
    esp_ota_select_entry_t e[2];
    bool found = false;
    uint32_t best_seq = 0;
    for (int i = 0; i < 2; i++) {
        if (esp_partition_read(od, (size_t)i * od->erase_size, &e[i], sizeof e[i]) != ESP_OK) continue;
        if (e[i].ota_seq == UINT32_MAX || e[i].crc != bootloader_common_ota_select_crc(&e[i])) continue;
        if ((int)((e[i].ota_seq - 1) % (uint32_t)slots) != want) continue;
        if (!found || e[i].ota_seq > best_seq) {
            best_seq = e[i].ota_seq;
            *state = (esp_ota_img_states_t)e[i].ota_state;
            found = true;
        }
    }
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// ================================================================== image facts
bool nv_fwup_image_info(const esp_partition_t *p, uint32_t *len, char *version, size_t vn) {
    if (!p) return false;
    const esp_partition_pos_t pos = { (uint32_t)p->address, (uint32_t)p->size };
    esp_image_metadata_t md;
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &md) != ESP_OK) return false;
    if (len) *len = md.image_len;
    if (version && vn) {
        esp_app_desc_t d;
        if (esp_ota_get_partition_description(p, &d) != ESP_OK) return false;
        snprintf(version, vn, "%s", d.version);
    }
    return true;
}

// ================================================================== journal
const esp_partition_t *nv_fwup_assets_part(void) {
    const esp_partition_t *a = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "assets");
    return (a && a->size >= kLkgDataOffset + 64 * 1024) ? a : nullptr;
}

bool nv_fwup_journal_load(nv_fwup_journal_t *out) {
    memset(out, 0, sizeof *out);
    const esp_partition_t *a = nv_fwup_assets_part();
    if (!a) return false;
    Journal j0, j1;
    if (esp_partition_read(a, kJournalOffset0, &j0, sizeof j0) != ESP_OK) memset(&j0, 0, sizeof j0);
    if (esp_partition_read(a, kJournalOffset1, &j1, sizeof j1) != ESP_OK) memset(&j1, 0, sizeof j1);
    const Journal *j = journal_pick(&j0, &j1);
    if (!j) return false;
    *out = *j;
    return true;
}

bool nv_fwup_journal_store(nv_fwup_journal_t *j) {
    const esp_partition_t *a = nv_fwup_assets_part();
    if (!a) return false;
    Journal j0, j1;
    if (esp_partition_read(a, kJournalOffset0, &j0, sizeof j0) != ESP_OK) memset(&j0, 0, sizeof j0);
    if (esp_partition_read(a, kJournalOffset1, &j1, sizeof j1) != ESP_OK) memset(&j1, 0, sizeof j1);
    const Journal *cur = journal_pick(&j0, &j1);
    // Overwrite the sector that does NOT hold the current record.
    const uint32_t off = (cur == &j0) ? kJournalOffset1 : kJournalOffset0;
    j->seq = cur ? cur->seq + 1 : 1;
    journal_seal(j);
    if (esp_partition_erase_range(a, off, kSector) != ESP_OK) return false;
    if (esp_partition_write(a, off, j, sizeof *j) != ESP_OK) return false;
    Journal back;
    return esp_partition_read(a, off, &back, sizeof back) == ESP_OK && !memcmp(&back, j, sizeof back);
}

// ================================================================== LKG
bool nv_fwup_lkg_info(nv_fwup_lkg_info_t *out) {
    memset(out, 0, sizeof *out);
    const esp_partition_t *a = nv_fwup_assets_part();
    LkgHeader h;
    if (!a || !read_header(a, &h)) return false;
    out->valid = true;
    snprintf(out->version, sizeof out->version, "%s", h.version);
    out->raw_size = h.raw_size;
    out->stored_size = h.stored_size;
    memcpy(out->raw_sha256, h.raw_sha256, 32);
    return true;
}

bool nv_fwup_lkg_clear(void) {
    const esp_partition_t *a = nv_fwup_assets_part();
    return a && esp_partition_erase_range(a, kLkgHeaderOffset, kSector) == ESP_OK;
}

nv_fwup_err_t nv_fwup_lkg_save(const esp_partition_t *sys, uint32_t len, const char *version,
                               nv_fwup_progress_cb cb, void *user) {
    const esp_partition_t *a = nv_fwup_assets_part();
    if (!a) return NV_FWUP_E_NOSPACE;
    if (!sys || !len || len > sys->size || !version || !version[0]) return NV_FWUP_E_SIZE;
    uint8_t *in_buf = (uint8_t *)big_alloc(kBuf), *out_buf = (uint8_t *)big_alloc(kBuf);
    void *work = big_alloc(codec::deflate_work_size());
    nv_fwup_err_t err = (in_buf && out_buf && work) ? NV_FWUP_OK : NV_FWUP_E_NOMEM;

    // 1. Already there? (same version, same bytes): nothing to do.
    LkgHeader old;
    if (err == NV_FWUP_OK && read_header(a, &old) && old.raw_size == len && !strcmp(old.version, version)) {
        uint8_t d[32];
        if (sha_range(sys, 0, len, d, in_buf, cb, user, 0, 100) && !memcmp(d, old.raw_sha256, 32)) {
            free(in_buf); free(out_buf); free(work);
            ESP_LOGI(TAG, "LKG already holds v%s", version);
            return NV_FWUP_OK;
        }
    }

    // 2. Invalidate the old copy first: from here on a power cut means "no copy", never a wrong one.
    if (err == NV_FWUP_OK && esp_partition_erase_range(a, kLkgHeaderOffset, kSector) != ESP_OK) err = NV_FWUP_E_IO;

    // 3. Compress the system into the data area.
    LkgHeader h = {};
    uint32_t stored = 0;
    if (err == NV_FWUP_OK) {
        HashingReader hr = {};
        hr.r = { sys, 0 };
        hr.total = len; hr.cb = cb; hr.user = user; hr.lo = 0; hr.hi = 70;
        mbedtls_sha256_init(&hr.sc);
        mbedtls_sha256_starts(&hr.sc, 0);
        LkgWriter w = {};
        w.a = a; w.pos = kLkgDataOffset; w.erased_to = kLkgDataOffset; w.limit = (uint32_t)a->size;
        mbedtls_sha256_init(&w.sc);
        mbedtls_sha256_starts(&w.sc, 0);
        const codec::Err e = codec::deflate(hashing_read, &hr, len, lkg_write, &w, work, in_buf, kBuf, out_buf, kBuf,
                                            kProbes, &stored);
        mbedtls_sha256_finish(&hr.sc, h.raw_sha256);
        mbedtls_sha256_finish(&w.sc, h.stored_sha256);
        mbedtls_sha256_free(&hr.sc);
        mbedtls_sha256_free(&w.sc);
        if (e == codec::Err::Write && w.pos + kBuf > w.limit) err = NV_FWUP_E_NOSPACE;
        else if (e != codec::Err::Ok) err = NV_FWUP_E_IO;
        if (err != NV_FWUP_OK) ESP_LOGE(TAG, "LKG save v%s: %s", version, codec::err_str(e));
    }
    free(work);
    free(out_buf);

    // 4. Prove the copy: re-read, decompress, hash.
    if (err == NV_FWUP_OK) {
        set_field(h.version, sizeof h.version, version);
        h.raw_size = len;
        h.stored_size = stored;
        h.method = kLkgDeflate;
        h.saved_by = pack_version(esp_app_get_description()->version);
        uint8_t d[32];
        if (!sha_range(a, kLkgDataOffset, stored, d, in_buf, cb, user, 70, 80) || memcmp(d, h.stored_sha256, 32))
            err = NV_FWUP_E_HASH;
        HashSink s = {};
        s.total = len; s.cb = cb; s.user = user; s.lo = 80; s.hi = 99;
        if (err == NV_FWUP_OK) err = unpack(a, h, &s, in_buf);
    }
    free(in_buf);

    // 5. Header last.
    if (err == NV_FWUP_OK) {
        lkg_seal(&h);
        if (esp_partition_write(a, kLkgHeaderOffset, &h, sizeof h) != ESP_OK) err = NV_FWUP_E_IO;
        LkgHeader back;
        if (err == NV_FWUP_OK && !read_header(a, &back)) err = NV_FWUP_E_IO;
    }
    if (err == NV_FWUP_OK) {
        report(cb, user, 1, 1, 0, 100);
        ESP_LOGI(TAG, "LKG saved: v%s, %lu -> %lu bytes", version, (unsigned long)len, (unsigned long)stored);
    } else {
        ESP_LOGE(TAG, "LKG save v%s failed: %s", version, nv_fwup_err_str(err));
    }
    return err;
}

nv_fwup_err_t nv_fwup_lkg_restore(const esp_partition_t *sys, nv_fwup_progress_cb cb, void *user) {
    const esp_partition_t *a = nv_fwup_assets_part();
    LkgHeader h;
    if (!a || !read_header(a, &h)) return NV_FWUP_E_IO;
    if (!sys || h.raw_size > sys->size) return NV_FWUP_E_SIZE;
    uint8_t *buf = (uint8_t *)big_alloc(kBuf);
    if (!buf) return NV_FWUP_E_NOMEM;

    // 1. The stored bytes, then the whole decompression, are proven before the slot is erased.
    uint8_t d[32];
    nv_fwup_err_t err = NV_FWUP_OK;
    if (!sha_range(a, kLkgDataOffset, h.stored_size, d, buf, cb, user, 0, 10) || memcmp(d, h.stored_sha256, 32))
        err = NV_FWUP_E_HASH;
    HashSink dry = {};
    dry.total = h.raw_size; dry.cb = cb; dry.user = user; dry.lo = 10; dry.hi = 30;
    if (err == NV_FWUP_OK) err = unpack(a, h, &dry, buf);

    // 2. Write.
    esp_ota_handle_t ota = 0;
    if (err == NV_FWUP_OK && esp_ota_begin(sys, h.raw_size, &ota) != ESP_OK) err = NV_FWUP_E_IO;
    if (err == NV_FWUP_OK) {
        HashSink s = {};
        s.ota = ota; s.total = h.raw_size; s.cb = cb; s.user = user; s.lo = 30; s.hi = 85;
        err = unpack(a, h, &s, buf);
        if (err != NV_FWUP_OK) esp_ota_abort(ota);
        else if (esp_ota_end(ota) != ESP_OK) err = NV_FWUP_E_IMAGE;
    }
    // 3. Read back.
    if (err == NV_FWUP_OK && (!sha_range(sys, 0, h.raw_size, d, buf, cb, user, 85, 100) || memcmp(d, h.raw_sha256, 32)))
        err = NV_FWUP_E_HASH;
    free(buf);
    if (err == NV_FWUP_OK) {
        esp_app_desc_t ad;
        if (esp_ota_get_partition_description(sys, &ad) != ESP_OK || strncmp(ad.version, h.version, sizeof ad.version))
            err = NV_FWUP_E_VERSION;
    }
    if (err == NV_FWUP_OK) ESP_LOGI(TAG, "LKG v%s restored into '%s'", h.version, sys->label);
    else ESP_LOGE(TAG, "LKG restore failed: %s", nv_fwup_err_str(err));
    return err;
}
