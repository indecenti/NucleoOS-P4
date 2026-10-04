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

// ---- data packs (store rows "kind":"data": ANIMA's knowledge packs, data/<id>/pack.sig) ----------------
//
//     nucleoos-data-v1
//     <id>
//     <version>
//     <dest>                                   where the files land, under /sdcard/data: one of kDataDests
//     <sha256 lowercase hex> <size> <name> <url>     one line per file, or per PART of a file
//     ...
//     sig <DER ECDSA P-256 signature, lowercase hex>
//
// Same store key and signed span as package.sig. The files are big (tens of MB to GB) and may live on
// another host (GitHub release assets): the URL is part of the signed text and every byte must hash to
// the signed sha256, so the host is never trusted. Consecutive lines with the same <name> are PARTS of
// one file, concatenated in order (a host's per-file limit, e.g. 2 GB); a name never reappears later.
constexpr int      kDataMax     = 64;                        // lines (files + parts)
constexpr int      kDataNameMax = 48;                        // incl. NUL: one path segment
constexpr int      kDataUrlMax  = 256;                       // incl. NUL: https://...
constexpr uint64_t kDataFileMax = 0xFFFFFFFFull;             // per file (all its parts): FAT32's limit
extern const char *const kDataDests[];                       // allowed <dest>, nullptr-terminated

struct DataPart {
    char     name[kDataNameMax];
    char     url[kDataUrlMax];
    uint64_t size;
    uint8_t  sha256[32];
};

struct DataPack {
    char     id[32];
    char     version[16];
    char     dest[24];
    int      n;
    DataPart parts[kDataMax];
    size_t   signed_len;
    uint8_t  sig[kSigMax];
    int      sig_len;
};

// Parse and validate a pack.sig. False on anything malformed: wrong domain line, bad id/version, a dest
// not in kDataDests, a name that is not one plain segment, a URL that is not https:// printable ASCII
// without spaces, a file (sum of its parts) over kDataFileMax, a name that reappears after another one.
// Does NOT check the signature.
bool parse_data(const char *text, size_t len, DataPack *out);

// Total bytes the pack downloads (every part).
uint64_t data_total(const DataPack &p);

}  // namespace nv_store_pkg
