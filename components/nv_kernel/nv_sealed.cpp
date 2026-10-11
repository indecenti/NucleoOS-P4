// nv_sealed — secret files on the SD card, sealed to this chip. See nv_sealed.h.
#include "nv_sealed.h"
#include "nv_config.h"
#include "nv_log.h"

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "mbedtls/gcm.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

static const char *TAG = "sealed";

namespace {

constexpr unsigned char kMagic[4] = {'N', 'V', 'X', '1'};
constexpr size_t kIv = 12, kTag = 16, kHdr = sizeof(kMagic) + kIv + kTag;
constexpr char kKeyLabel[] = "nucleo-sealed-file-v1";

// Every secret file, as physical paths.
constexpr const char *kSecret[] = {
    "/sdcard/data/anima/teacher.json",
    "/sdcard/data/anima/telegram.json",   // the Telegram bot token (nucleo_anima_telegram.c)
    "/sdcard/system/config/authenticator.json",   // the Authenticator web app's TOTP seeds (sd/web/apps/authenticator)
};

// FatFs splits paths on '\' as well as '/'.
inline bool sep(char c) { return c == '/' || c == '\\'; }

// Next path component of `p` as FatFs sees it: [*b, *e) with trailing dots/spaces dropped. "."
// components are skipped (FatFs resolves "/a/./b" to "/a/b"); callers fold "..".
const char *component(const char *p, const char **b, const char **e) {
    for (;;) {
        while (sep(*p)) p++;
        *b = p;
        while (*p && !sep(*p)) p++;
        if (p - *b == 1 && **b == '.') continue;
        break;
    }
    const char *end = p;
    while (end > *b && (end[-1] == '.' || end[-1] == ' ')) end--;
    *e = end;
    return p;
}

bool same_name(const char *ab, const char *ae, const char *bb, const char *be) {
    if (ae - ab != be - bb) return false;
    for (const char *x = ab, *y = bb; x < ae; x++, y++)
        if (tolower((unsigned char)*x) != tolower((unsigned char)*y)) return false;
    return true;
}

// With long names on, FatFs also opens a file by its 8.3 alias: "TEACHE~1.JSO" for "teacher.json",
// or after many collisions a hashed tail, "TEAC~1F3.JSO". True when [ab, ae) has that shape for the
// long name [bb, be): the name's first letters, '~' and hex digits (8 at most), then the extension
// cut to 3. FatFs folds no other character into ASCII, so case and this are the only other spellings.
bool alias_of(const char *ab, const char *ae, const char *bb, const char *be) {
    const char *bdot = nullptr;
    int bdots = 0;
    for (const char *y = bb; y < be; y++) if (*y == '.') { bdot = y; bdots++; }
    const char *bbase_end = bdot ? bdot : be;
    if (bdots <= 1 && bbase_end - bb <= 8 && (bdot ? be - bdot - 1 : 0) <= 3) return false;   // an 8.3 name has no alias
    const char *adot = nullptr;
    for (const char *x = ab; x < ae; x++) if (*x == '.') { if (adot) return false; adot = x; }
    const char *abase_end = adot ? adot : ae;
    if (abase_end - ab > 8) return false;
    const char *t = abase_end;   // the numbered tail: '~' and hex digits
    while (t > ab && isxdigit((unsigned char)t[-1])) t--;
    if (t == abase_end || t == ab || t[-1] != '~') return false;
    // The letters before '~': the long name's, dots and spaces dropped.
    const char *y = bb;
    for (const char *x = ab; x < t - 1; x++) {
        while (y < bbase_end && (*y == '.' || *y == ' ')) y++;
        if (y >= bbase_end || toupper((unsigned char)*x) != toupper((unsigned char)*y)) return false;
        y++;
    }
    const long aext = adot ? ae - adot - 1 : 0, bext = bdot ? be - bdot - 1 : 0;
    if (aext != (bext < 3 ? bext : 3)) return false;
    for (long i = 0; i < aext; i++)
        if (toupper((unsigned char)adot[1 + i]) != toupper((unsigned char)bdot[1 + i])) return false;
    return true;
}

// Component-wise: true when `a` names the same file as `b` (prefix: when `a` is `b` or a folder on
// `b`'s way).
bool match_path(const char *a, const char *b, bool prefix) {
    for (;;) {
        const char *ab, *ae, *bb, *be;
        a = component(a, &ab, &ae);
        b = component(b, &bb, &be);
        if (ab == ae && !*a) return prefix || (bb == be && !*b);   // `a` ran out
        if (!same_name(ab, ae, bb, be) && !alias_of(ab, ae, bb, be)) return false;
        if (ab == ae) return !*a && !*b;   // both ran out together
    }
}

// The copy write_atomic leaves beside a secret while it replaces it ("<secret>.tmp"): in clear on a
// board without the settings key, so it is as secret as the file.
bool tmp_of(const char *path, const char *secret, bool prefix) {
    char t[64];
    return snprintf(t, sizeof t, "%s.tmp", secret) < (int)sizeof t && match_path(path, t, prefix);
}

bool same_path(const char *a, const char *b) { return match_path(a, b, false); }

bool gcm(bool enc, const unsigned char *iv, const unsigned char *in, size_t n, unsigned char *out,
         unsigned char *tag) {
    unsigned char key[32];
    if (!nv_config_device_key(kKeyLabel, key)) return false;
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256) == 0;
    if (ok && enc)
        ok = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, iv, kIv, kMagic, sizeof kMagic, in, out,
                                       kTag, tag) == 0;
    else if (ok)
        ok = mbedtls_gcm_auth_decrypt(&g, n, iv, kIv, kMagic, sizeof kMagic, tag, kTag, in, out) == 0;
    mbedtls_gcm_free(&g);
    memset(key, 0, sizeof key);
    return ok;
}

unsigned char *slurp(const char *path, size_t max, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    long sz = -1;
    if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
    if (sz <= 0 || (size_t)sz > max + kHdr) { fclose(f); return nullptr; }
    rewind(f);
    auto *buf = static_cast<unsigned char *>(heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) { fclose(f); return nullptr; }
    *n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (*n != (size_t)sz) { free(buf); return nullptr; }
    return buf;
}

bool write_atomic(const char *path, const void *data, size_t n) {
    char tmp[128];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return false;
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp); return false; }
    remove(path);                              // FATFS rename won't overwrite
    if (rename(tmp, path) != 0) {              // tmp KEPT: it is the only copy now
        NV_LOGE(TAG, "rename -> %s failed, content left in %s", path, tmp);
        return false;
    }
    return true;
}

// write_atomic is remove(path) + rename(tmp, path): a crash / card pull between the two leaves only
// tmp (complete — it was closed before path was removed). Put it back before reading. Only when path
// is missing: a tmp beside a live path is a failed write, never newer data.
void recover_tmp(const char *path) {
    char tmp[128];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return;
    struct stat st;
    if (stat(path, &st) == 0 || stat(tmp, &st) != 0) return;
    if (rename(tmp, path) == 0) NV_LOGW(TAG, "%s: recovered from interrupted write", path);
    else                        NV_LOGE(TAG, "%s: only %s exists, rename failed", path, tmp);
}

}  // namespace

bool nv_sealed_path(const char *path) {
    if (!path) return false;
    for (const char *s : kSecret)
        if (same_path(path, s) || tmp_of(path, s, false)) return true;
    return false;
}

bool nv_sealed_is(const char *path, const char *secret) {
    return path && secret && same_path(path, secret);
}

bool nv_sealed_within(const char *path) {
    if (!path) return false;
    for (const char *s : kSecret)
        if (match_path(path, s, true) || tmp_of(path, s, true)) return true;
    return false;
}

char *nv_sealed_read(const char *path, size_t max, size_t *len) {
    size_t n = 0;
    if (!path) return nullptr;
    recover_tmp(path);
    unsigned char *raw = slurp(path, max, &n);
    if (!raw) return nullptr;
    if (n >= kHdr && memcmp(raw, kMagic, sizeof kMagic) == 0) {
        const size_t pn = n - kHdr;
        auto *plain = static_cast<unsigned char *>(heap_caps_malloc(pn + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        const bool ok = plain && pn <= max &&
                        gcm(false, raw + sizeof kMagic, raw + kHdr, pn, plain, raw + sizeof kMagic + kIv);
        free(raw);
        if (!ok) {
            NV_LOGE(TAG, "%s: sealed by another device, or damaged", path);
            free(plain);
            return nullptr;
        }
        plain[pn] = '\0';
        if (len) *len = pn;
        return reinterpret_cast<char *>(plain);
    }
    if (n > max) { free(raw); return nullptr; }
    raw[n] = '\0';
    // A plaintext secret file (older firmware, or written by hand): seal it now.
    if (nv_config_encrypted() && nv_sealed_path(path)) {
        if (nv_sealed_write(path, raw, n)) NV_LOGI(TAG, "%s: sealed to this device", path);
    }
    if (len) *len = n;
    return reinterpret_cast<char *>(raw);
}

bool nv_sealed_write(const char *path, const void *data, size_t len) {
    if (!path) return false;
    for (const char *s : kSecret)
        if (tmp_of(path, s, false)) return false;   // write_atomic's own copy: never written as a file
    if (!nv_config_encrypted() || !nv_sealed_path(path)) return write_atomic(path, data, len);
    auto *out = static_cast<unsigned char *>(heap_caps_malloc(kHdr + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!out) return false;
    memcpy(out, kMagic, sizeof kMagic);
    esp_fill_random(out + sizeof kMagic, kIv);
    bool ok = gcm(true, out + sizeof kMagic, static_cast<const unsigned char *>(data), len, out + kHdr,
                  out + sizeof kMagic + kIv);
    if (ok) ok = write_atomic(path, out, kHdr + len);
    memset(out, 0, kHdr + len);
    free(out);
    return ok;
}
