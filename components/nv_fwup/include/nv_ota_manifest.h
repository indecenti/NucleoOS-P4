// nv_ota_manifest — the signed part of an OTA manifest, as pure code (no ESP-IDF): validate and
// decode the fields, and build the exact message the release key signed (tools/ota_sign.py).
// Unit-tested and fuzzed in tests/host; nv_ota.cpp does the ECDSA check and the image hash.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace nv_ota_manifest {

constexpr size_t kVersionMax = 32;   // incl. NUL
constexpr size_t kShaLen     = 32;
constexpr size_t kSigMax     = 80;   // DER ECDSA P-256 signatures are at most 72 bytes

struct Signed {
    char     version[kVersionMax];
    char     sha256_hex[kShaLen * 2 + 1];
    uint8_t  sha256[kShaLen];
    uint32_t size;
    uint8_t  sig[kSigMax];
    size_t   sig_len;
};

// Validate the manifest fields and decode them into `out`. `size` is the JSON number. Refused:
// a version outside [0-9A-Za-z.+_-] or too long, a sha256 that is not 64 lowercase hex digits, a
// size that is not an integer in 1..max_size, a signature that is not even-length hex of 8..kSigMax
// bytes.
bool parse(const char *version, const char *sha256_hex, double size, const char *sig_hex,
           uint32_t max_size, Signed *out);

// "nucleoos-ota-v1\n<version>\n<sha256 hex>\n<size>\n" (NUL-terminated). Returns its length, or 0
// when `cap` is too small.
size_t message(const Signed &s, char *out, size_t cap);

}  // namespace nv_ota_manifest
