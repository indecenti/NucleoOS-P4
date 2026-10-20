// nv_dl_core — the pure parts of the system downloader (nv_dl.cpp): Metalink 4 parsing, the job record
// kept on the card, the files a big download is split into, and the mapping of an app's path to the
// card. No ESP-IDF: unit-tested and fuzzed in tests/host (target "dl"). docs/ZIM_READER_PLAN.md §4f.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVDL_URLS_MAX   8          // mirrors kept per download
#define NVDL_URL_MAX    320
#define NVDL_PATH_MAX   192
#define NVDL_TITLE_MAX  80
#define NVDL_OWNER_MAX  32
#define NVDL_ERR_MAX    96
#define NVDL_NAME_MAX   128

#define NVDL_PIECE_MIN  (16u * 1024)          // smallest / largest verified piece accepted
#define NVDL_PIECE_MAX  (64u * 1024 * 1024)
#define NVDL_SIZE_MAX   (1ull << 40)          // 1 TiB: more than any card, less than an overflow
// A download bigger than this is written in parts name.zimaa, name.zimab, ... (nvzim reads them as one
// file): 2 GiB minus 4 MiB, under the guests' 32-bit file offsets and a multiple of the 4 MiB pieces
// Kiwix uses, so a piece never straddles two files.
#define NVDL_PART_SIZE  (2147483648ull - 4194304ull)
#define NVDL_PARTS_MAX  (26u * 26u)

enum {
    NVDL_OK = 0,
    NVDL_EPARSE = -1,     // malformed document, no <file>, no or bad <size>
    NVDL_ETOOBIG = -2,    // the piece hashes don't fit the caller's buffer
    NVDL_EPIECES = -3,    // piece hashes inconsistent with the size (count, length, type)
    NVDL_ENOURL = -4,     // no usable https URL
};

typedef struct {
    char     name[NVDL_NAME_MAX];                 // <file name="..."> (informative, sanitised)
    uint64_t size;
    bool     has_sha256;
    uint8_t  sha256[32];
    uint32_t piece_len;                           // 0 = no SHA-1 pieces
    uint32_t n_pieces;
    int      n_urls;
    char     urls[NVDL_URLS_MAX][NVDL_URL_MAX];   // https only, best priority first
} nvdl_meta4_t;

// Parses the FIRST <file> of a Metalink 4 document (RFC 5854, what MirrorBrain serves). The SHA-1 piece
// hashes go to `pieces`, 20 bytes each in order; with pieces == NULL they are only counted (call once
// to size the buffer, then again to fill it). Returns NVDL_OK or a negative NVDL_E*.
int nvdl_meta4_parse(const char *xml, size_t n, nvdl_meta4_t *out, uint8_t *pieces, size_t pieces_cap);

// ---- the job record (<id>.job on the card) --------------------------------------------------------

typedef enum {
    NVDL_QUEUED = 0,      // waiting its turn (also: was running when the board stopped)
    NVDL_PAUSED,
    NVDL_DONE,
    NVDL_FAILED,
} nvdl_state_t;

typedef struct {
    uint32_t id;
    char     owner[NVDL_OWNER_MAX];        // app id that asked ("shell" from the Terminal)
    char     title[NVDL_TITLE_MAX];
    char     dest[NVDL_PATH_MAX];          // physical path of the finished file ("/sdcard/...")
    char     src[NVDL_URL_MAX];            // what was asked for: a .meta4 or the file itself
    bool     src_meta4;
    bool     prepared;                     // size, hashes and mirrors are known (meta4 read)
    uint64_t size;
    bool     has_sha256;
    uint8_t  sha256[32];
    uint32_t piece_len;                    // 0: no pieces (checkpoints every NVDL_CHECKPOINT bytes)
    uint32_t n_pieces;                     // their hashes: <id>.sha1, 20 bytes each
    uint64_t verified;                     // bytes on the card, synced and checked (from 0, no holes)
    nvdl_state_t state;
    uint32_t created;                      // unix time
    char     error[NVDL_ERR_MAX];
    int      n_urls;
    char     urls[NVDL_URLS_MAX][NVDL_URL_MAX];
} nvdl_job_t;

// Without piece hashes the downloader still syncs and records its progress this often.
#define NVDL_CHECKPOINT (4u * 1024 * 1024)

// Writes the record as text ("nvdl 1" then "key value" lines). Returns its length, or -1 if `cap` is
// too small. Free text (title, error) is written without line breaks.
int nvdl_job_format(const nvdl_job_t *j, char *buf, size_t cap);
// Reads a record back. False when it is not one, or when its numbers don't hold together (a damaged
// file must not resume into the wrong offsets).
bool nvdl_job_parse(const char *text, size_t n, nvdl_job_t *j);

// Next offset the download continues from: `verified` rounded down to a whole piece (or checkpoint).
uint64_t nvdl_resume_offset(const nvdl_job_t *j);

// ---- files ----------------------------------------------------------------------------------------

// How many files a download of `size` bytes takes (1 up to NVDL_PART_SIZE). 0 if over NVDL_PARTS_MAX.
uint32_t nvdl_parts(uint64_t size);
// The name of file i (0-based): `dest` itself when nparts is 1, else dest + "aa", "ab", ... False when
// it doesn't fit or i is out of range.
bool nvdl_part_name(const char *dest, uint32_t i, uint32_t nparts, char *out, size_t cap);

// The card path for `guest`, an absolute path in the app's own view ("/zim/x.zim"), under `root`
// ("/sdcard/home", or the app's data folder). Refuses: relative paths, empty / "." / ".." components,
// '\' and the characters FAT can't store (: * ? " < > |), control characters, a trailing '/' or '.',
// components over 120 bytes, results over cap.
bool nvdl_guest_path(const char *root, const char *guest, char *out, size_t cap);

// "0123..." -> bytes. False unless exactly 2*n hex digits.
bool nvdl_hex_decode(const char *hex, uint8_t *out, size_t n);

#ifdef __cplusplus
}
#endif
