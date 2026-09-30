// nv_store_pkg — the signed package index of a store app (apps/<id>/package.sig). Pure code, no
// ESP-IDF: host unit tests + fuzzer in tests/host (target "pkg").
//
// Text format (LF line ends, what tools/store_sign.py writes):
//
//     nucleoos-app-v1
//     <id>
//     <version>
//     <sha256 lowercase hex> <size> <path>      one line per file, paths strictly ascending
//     ...
//     sig <DER ECDSA P-256 signature, lowercase hex>
//
// The signature (store key, NOT the OTA key) covers every byte before the "sig " line. The device
// installs a file only if its path is listed and its bytes hash to the listed sha256/size, so the
// signature also covers the app's manifest (permissions) and its module.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace nv_store_pkg {

constexpr int      kMaxFiles = 264;                // 256 assets + module, aot, manifest, icons, files.json
constexpr int      kPathMax  = 64;                 // incl. NUL
constexpr uint32_t kFileMax  = 24u * 1024 * 1024;
constexpr int      kSigMax   = 72;                 // DER ECDSA P-256 is 70-72 bytes
constexpr size_t   kTextMax  = 40 * 1024;          // whole package.sig

struct File {
    char     path[kPathMax];
    uint32_t size;
    uint8_t  sha256[32];
};

struct Package {
    char    id[32];
    char    version[16];
    int     n_files;
    File    files[kMaxFiles];
    size_t  signed_len;                            // bytes of the text covered by the signature
    uint8_t sig[kSigMax];
    int     sig_len;
};

// Parse and validate `text` (need not be NUL-terminated). False on anything malformed: wrong
// domain line, bad id/version charset, a path that could leave the package folder, unsorted or
// duplicate paths, sizes over kFileMax, a missing/odd signature. Does NOT check the signature.
bool parse(const char *text, size_t len, Package *out);

// The listed entry for `path`, or nullptr.
const File *find(const Package &p, const char *path);

// Allowed file path: 1..63 chars of [A-Za-z0-9._-] segments joined by '/', no empty/"."/".."
// segment, no leading '/', at most 2 directory levels.
bool path_ok(const char *path);

}  // namespace nv_store_pkg
