// nv_ota_manifest — see nv_ota_manifest.h.
#include "nv_ota_manifest.h"

#include <cstdio>
#include <cstring>

namespace nv_ota_manifest {

namespace {

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool version_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '.' || c == '-' || c == '+' || c == '_';
}

}  // namespace

bool parse(const char *version, const char *sha256_hex, double size, const char *sig_hex,
           uint32_t max_size, Signed *out) {
    if (!version || !sha256_hex || !sig_hex || !out) return false;
    const size_t vl = strlen(version);
    if (!vl || vl >= kVersionMax) return false;
    for (size_t i = 0; i < vl; i++)
        if (!version_char(version[i])) return false;
    if (strlen(sha256_hex) != kShaLen * 2) return false;
    for (size_t i = 0; i < kShaLen; i++) {
        const int hi = hex_val(sha256_hex[2 * i]), lo = hex_val(sha256_hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->sha256[i] = (uint8_t)(hi << 4 | lo);
    }
    // !(x >= 1) also rejects NaN.
    if (!(size >= 1) || size > (double)max_size || size != (double)(uint32_t)size) return false;
    const size_t sl = strlen(sig_hex);
    if (sl % 2 || sl / 2 < 8 || sl / 2 > kSigMax) return false;
    for (size_t i = 0; i < sl / 2; i++) {
        const int hi = hex_val(sig_hex[2 * i]), lo = hex_val(sig_hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->sig[i] = (uint8_t)(hi << 4 | lo);
    }
    memcpy(out->version, version, vl + 1);
    memcpy(out->sha256_hex, sha256_hex, kShaLen * 2 + 1);
    out->size = (uint32_t)size;
    out->sig_len = sl / 2;
    return true;
}

size_t message(const Signed &s, char *out, size_t cap) {
    const int n = snprintf(out, cap, "nucleoos-ota-v1\n%s\n%s\n%lu\n", s.version, s.sha256_hex,
                           (unsigned long)s.size);
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

}  // namespace nv_ota_manifest
