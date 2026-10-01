// nv_fwup — see nv_fwup.h.
#include "nv_fwup.h"
#include "nv_ota_manifest.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

// The release key's public half (tools/ota_sign.py keygen), embedded NUL-terminated.
extern const char fwup_pub_start[] asm("_binary_ota_signing_pub_pem_start");
extern const char fwup_pub_end[]   asm("_binary_ota_signing_pub_pem_end");

static const char *TAG = "fwup";

namespace {

constexpr size_t kChunk = 32 * 1024;   // SD and flash both like big transfers

uint8_t *chunk_alloc(void) {
    // DMA-capable internal RAM is precious in NucleoOS; PSRAM is fine for FATFS + esp_partition I/O.
    void *p = heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(kChunk);
    return (uint8_t *)p;
}

void to_hex(const uint8_t *b, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
    out[2 * n] = '\0';
}

void report(nv_fwup_progress_cb cb, void *user, uint64_t done, uint64_t total, int lo, int hi) {
    if (!cb || !total) return;
    cb(lo + (int)((hi - lo) * done / total), user);
}

bool verify_signature(const nv_ota_manifest::Signed &s) {
    char msg[160];
    const size_t len = nv_ota_manifest::message(s, msg, sizeof msg);
    if (!len) return false;
    uint8_t h[32];
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = mbedtls_pk_parse_public_key(&pk, reinterpret_cast<const unsigned char *>(fwup_pub_start),
                                         (size_t)(fwup_pub_end - fwup_pub_start));
    if (rc == 0) rc = mbedtls_sha256(reinterpret_cast<const unsigned char *>(msg), len, h, 0);
    if (rc == 0) rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, h, sizeof h, s.sig, s.sig_len);
    mbedtls_pk_free(&pk);
    if (rc != 0) ESP_LOGE(TAG, "v%s: signature check failed (-0x%04x)", s.version, -rc);
    return rc == 0;
}

}  // namespace

const char *nv_fwup_err_str(nv_fwup_err_t e) {
    switch (e) {
        case NV_FWUP_OK:          return "ok";
        case NV_FWUP_E_FIELDS:    return "manifest malformed";
        case NV_FWUP_E_SIGNATURE: return "not signed by the release key";
        case NV_FWUP_E_IO:        return "read/write error";
        case NV_FWUP_E_SIZE:      return "wrong size";
        case NV_FWUP_E_HASH:      return "checksum mismatch";
        case NV_FWUP_E_IMAGE:     return "invalid firmware image";
        case NV_FWUP_E_VERSION:   return "version mismatch";
        case NV_FWUP_E_NOMEM:     return "out of memory";
    }
    return "?";
}

// ---------------------------------------------------------------- layout
const esp_partition_t *nv_fwup_recovery_part(void) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1,
                                     NV_FWUP_LABEL_RECOVERY);
}
const esp_partition_t *nv_fwup_system_part(void) {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                    NV_FWUP_LABEL_SYSTEM);
}
bool nv_fwup_layout_v2(void) { return nv_fwup_recovery_part() && nv_fwup_system_part(); }

char *nv_fwup_path(char *out, size_t n, const char *mount, const char *name) {
    snprintf(out, n, "%s/" NV_FWUP_DIR "/%s", mount, name);
    return out;
}

bool nv_fwup_ensure_dir(const char *mount) {
    char d[64];
    snprintf(d, sizeof d, "%s/" NV_FWUP_DIR, mount);
    struct stat st;
    if (stat(d, &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(d, 0775) == 0;
}

// ---------------------------------------------------------------- manifests
nv_fwup_err_t nv_fwup_manifest_parse(const char *json, uint32_t max_size, nv_fwup_manifest_t *out) {
    cJSON *root = json ? cJSON_Parse(json) : nullptr;
    if (!root) return NV_FWUP_E_FIELDS;
    const cJSON *jv = cJSON_GetObjectItem(root, "version");
    const cJSON *jh = cJSON_GetObjectItem(root, "sha256");
    const cJSON *js = cJSON_GetObjectItem(root, "size");
    const cJSON *jg = cJSON_GetObjectItem(root, "sig");
    nv_ota_manifest::Signed s;
    nv_fwup_err_t err = NV_FWUP_E_FIELDS;
    if (cJSON_IsString(jv) && cJSON_IsString(jh) && cJSON_IsNumber(js) && cJSON_IsString(jg) &&
        nv_ota_manifest::parse(jv->valuestring, jh->valuestring, js->valuedouble, jg->valuestring,
                               max_size, &s)) {
        err = verify_signature(s) ? NV_FWUP_OK : NV_FWUP_E_SIGNATURE;
        if (err == NV_FWUP_OK) {
            memset(out, 0, sizeof *out);
            snprintf(out->version, sizeof out->version, "%s", s.version);
            memcpy(out->sha256_hex, s.sha256_hex, sizeof out->sha256_hex);
            memcpy(out->sha256, s.sha256, sizeof out->sha256);
            out->size = s.size;
            to_hex(s.sig, s.sig_len, out->sig_hex);
        }
    }
    cJSON_Delete(root);
    return err;
}

nv_fwup_err_t nv_fwup_manifest_load(const char *path, uint32_t max_size, nv_fwup_manifest_t *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NV_FWUP_E_IO;
    char *buf = (char *)malloc(4096);
    const size_t n = buf ? fread(buf, 1, 4095, f) : 0;
    fclose(f);
    if (!buf) return NV_FWUP_E_NOMEM;
    buf[n] = '\0';
    const nv_fwup_err_t e = nv_fwup_manifest_parse(buf, max_size, out);
    free(buf);
    return e;
}

bool nv_fwup_manifest_save(const char *path, const nv_fwup_manifest_t *m) {
    char tmp[96];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    const int w = fprintf(f, "{\"version\":\"%s\",\"sha256\":\"%s\",\"size\":%lu,\"sig\":\"%s\"}\n",
                          m->version, m->sha256_hex, (unsigned long)m->size, m->sig_hex);
    const bool ok = w > 0 && fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (!ok) { remove(tmp); return false; }
    remove(path);   // FAT rename does not replace
    return rename(tmp, path) == 0;
}

// ---------------------------------------------------------------- hashing / flashing
nv_fwup_err_t nv_fwup_hash_file(const char *path, const nv_fwup_manifest_t *m, nv_fwup_progress_cb cb,
                                void *user) {
    FILE *f = fopen(path, "rb");
    if (!f) return NV_FWUP_E_IO;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || (uint32_t)st.st_size != m->size) {
        fclose(f);
        ESP_LOGE(TAG, "%s: %ld bytes, manifest says %lu", path, (long)st.st_size, (unsigned long)m->size);
        return NV_FWUP_E_SIZE;
    }
    uint8_t *buf = chunk_alloc();
    if (!buf) { fclose(f); return NV_FWUP_E_NOMEM; }
    mbedtls_sha256_context sc;
    mbedtls_sha256_init(&sc);
    mbedtls_sha256_starts(&sc, 0);
    uint64_t done = 0;
    size_t n;
    while ((n = fread(buf, 1, kChunk, f)) > 0) {
        mbedtls_sha256_update(&sc, buf, n);
        done += n;
        report(cb, user, done, m->size, 0, 100);
    }
    const bool rd_err = ferror(f);
    fclose(f);
    free(buf);
    uint8_t d[32];
    mbedtls_sha256_finish(&sc, d);
    mbedtls_sha256_free(&sc);
    if (rd_err || done != m->size) return NV_FWUP_E_IO;
    if (memcmp(d, m->sha256, 32) != 0) { ESP_LOGE(TAG, "%s: sha256 mismatch", path); return NV_FWUP_E_HASH; }
    return NV_FWUP_OK;
}

nv_fwup_err_t nv_fwup_hash_partition(const esp_partition_t *p, const nv_fwup_manifest_t *m,
                                     nv_fwup_progress_cb cb, void *user) {
    if (!p || m->size > p->size) return NV_FWUP_E_SIZE;
    uint8_t *buf = chunk_alloc();
    if (!buf) return NV_FWUP_E_NOMEM;
    mbedtls_sha256_context sc;
    mbedtls_sha256_init(&sc);
    mbedtls_sha256_starts(&sc, 0);
    nv_fwup_err_t err = NV_FWUP_OK;
    for (uint32_t off = 0; off < m->size;) {
        const uint32_t n = (m->size - off) < kChunk ? (m->size - off) : (uint32_t)kChunk;
        if (esp_partition_read(p, off, buf, n) != ESP_OK) { err = NV_FWUP_E_IO; break; }
        mbedtls_sha256_update(&sc, buf, n);
        off += n;
        report(cb, user, off, m->size, 0, 100);
    }
    uint8_t d[32];
    mbedtls_sha256_finish(&sc, d);
    mbedtls_sha256_free(&sc);
    free(buf);
    if (err == NV_FWUP_OK && memcmp(d, m->sha256, 32) != 0) err = NV_FWUP_E_HASH;
    return err;
}

namespace {
struct Sub { nv_fwup_progress_cb cb; void *user; int lo, hi; };
void sub_cb(int pct, void *u) {
    Sub *s = (Sub *)u;
    if (s->cb) s->cb(s->lo + (s->hi - s->lo) * pct / 100, s->user);
}
}  // namespace

nv_fwup_err_t nv_fwup_install(const char *path, const nv_fwup_manifest_t *m, const esp_partition_t *dst,
                              nv_fwup_progress_cb cb, void *user) {
    if (!dst || m->size > dst->size) return NV_FWUP_E_SIZE;
    // 1. The file must be exactly the signed image before a single byte of the slot is erased.
    Sub s1 = {cb, user, 0, 20};
    nv_fwup_err_t err = nv_fwup_hash_file(path, m, sub_cb, &s1);
    if (err != NV_FWUP_OK) return err;

    // 2. Write. esp_ota_end() validates the ESP image (segments, checksum, appended SHA-256).
    FILE *f = fopen(path, "rb");
    if (!f) return NV_FWUP_E_IO;
    uint8_t *buf = chunk_alloc();
    if (!buf) { fclose(f); return NV_FWUP_E_NOMEM; }
    esp_ota_handle_t h = 0;
    if (esp_ota_begin(dst, m->size, &h) != ESP_OK) { free(buf); fclose(f); return NV_FWUP_E_IO; }
    uint32_t done = 0;
    size_t n;
    while ((n = fread(buf, 1, kChunk, f)) > 0) {
        if (esp_ota_write(h, buf, n) != ESP_OK) { err = NV_FWUP_E_IO; break; }
        done += n;
        report(cb, user, done, m->size, 20, 85);
    }
    fclose(f);
    free(buf);
    if (err == NV_FWUP_OK && done != m->size) err = NV_FWUP_E_IO;
    if (err != NV_FWUP_OK) { esp_ota_abort(h); return err; }
    if (esp_ota_end(h) != ESP_OK) return NV_FWUP_E_IMAGE;

    // 3. Read back what the flash actually holds.
    Sub s3 = {cb, user, 85, 100};
    err = nv_fwup_hash_partition(dst, m, sub_cb, &s3);
    if (err != NV_FWUP_OK) { ESP_LOGE(TAG, "read-back: %s", nv_fwup_err_str(err)); return err; }

    // 4. A signed manifest must never relabel a different build.
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(dst, &d) != ESP_OK ||
        strncmp(d.version, m->version, sizeof d.version) != 0)
        return NV_FWUP_E_VERSION;
    ESP_LOGI(TAG, "v%s installed to '%s' (%lu bytes, verified)", m->version, dst->label, (unsigned long)m->size);
    return NV_FWUP_OK;
}

nv_fwup_err_t nv_fwup_backup(const esp_partition_t *src, const nv_fwup_manifest_t *m, const char *path,
                             nv_fwup_progress_cb cb, void *user) {
    if (!src || m->size > src->size) return NV_FWUP_E_SIZE;
    char tmp[96];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return NV_FWUP_E_IO;
    uint8_t *buf = chunk_alloc();
    if (!buf) { fclose(f); remove(tmp); return NV_FWUP_E_NOMEM; }
    mbedtls_sha256_context sc;
    mbedtls_sha256_init(&sc);
    mbedtls_sha256_starts(&sc, 0);
    nv_fwup_err_t err = NV_FWUP_OK;
    for (uint32_t off = 0; off < m->size;) {
        const uint32_t n = (m->size - off) < kChunk ? (m->size - off) : (uint32_t)kChunk;
        if (esp_partition_read(src, off, buf, n) != ESP_OK || fwrite(buf, 1, n, f) != n) { err = NV_FWUP_E_IO; break; }
        mbedtls_sha256_update(&sc, buf, n);
        off += n;
        report(cb, user, off, m->size, 0, 100);
    }
    uint8_t d[32];
    mbedtls_sha256_finish(&sc, d);
    mbedtls_sha256_free(&sc);
    free(buf);
    if (err == NV_FWUP_OK && (fflush(f) != 0 || fsync(fileno(f)) != 0)) err = NV_FWUP_E_IO;
    fclose(f);
    if (err == NV_FWUP_OK && memcmp(d, m->sha256, 32) != 0) err = NV_FWUP_E_HASH;   // slot != its manifest
    if (err != NV_FWUP_OK) { remove(tmp); return err; }
    remove(path);
    return rename(tmp, path) == 0 ? NV_FWUP_OK : NV_FWUP_E_IO;
}

// ---------------------------------------------------------------- state files
int nv_fwup_tries_get(const char *mount) {
    char p[64];
    FILE *f = fopen(nv_fwup_path(p, sizeof p, mount, NV_FWUP_TRIES), "rb");
    if (!f) return 0;
    int n = 0;
    if (fscanf(f, "%d", &n) != 1) n = 0;
    fclose(f);
    return n;
}

void nv_fwup_tries_set(const char *mount, int n) {
    char p[64];
    nv_fwup_path(p, sizeof p, mount, NV_FWUP_TRIES);
    if (n <= 0) { remove(p); return; }
    if (FILE *f = fopen(p, "wb")) { fprintf(f, "%d\n", n); fflush(f); fsync(fileno(f)); fclose(f); }
}

void nv_fwup_result_write(const char *mount, const char *op, bool ok, const char *from, const char *to,
                          const char *why) {
    char p[64];
    nv_fwup_path(p, sizeof p, mount, NV_FWUP_RESULT);
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    cJSON_AddStringToObject(o, "op", op ? op : "");
    cJSON_AddBoolToObject(o, "ok", ok);
    cJSON_AddStringToObject(o, "from", from ? from : "");
    cJSON_AddStringToObject(o, "to", to ? to : "");
    if (why) cJSON_AddStringToObject(o, "why", why);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s) return;
    if (FILE *f = fopen(p, "wb")) { fputs(s, f); fflush(f); fsync(fileno(f)); fclose(f); }
    cJSON_free(s);
}

bool nv_fwup_version_newer(const char *cand, const char *cur) {
    int a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
    sscanf(cand, "%d.%d.%d", &a[0], &a[1], &a[2]);
    sscanf(cur, "%d.%d.%d", &b[0], &b[1], &b[2]);
    for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}
