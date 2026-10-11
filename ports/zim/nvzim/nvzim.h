// nvzim.h — reader for ZIM files (openZIM / Kiwix), written for NucleoOS from the file format:
// no libzim, no Xapian (both GPL). C99, no globals; one handle per open file, used by one thread.
//
// Built for the ESP32-P4 SD card (docs/ZIM_READER_PLAN.md §1b):
//  - Every read is lseek + read, never pread: ESP-IDF's pread walks the FAT32 cluster chain from
//    the start of the file on each call. Small structural reads go through an LRU of 4 KiB pages.
//  - Path lookups memoise the top of the binary search (the first probes are the same for every
//    query) and, once the range is small, read its pointers and its dirents in one go each:
//    dirents are stored in path order, so the range is one contiguous span of the file.
//  - zstd clusters are decoded in place (ZSTD_d_stableOutBuffer: no 8 MiB window) and only as far
//    as the blob asked for; a later blob of the same cluster resumes the decoding.
//  - Uncompressed clusters (images, indexes) are never loaded whole: blobs are read in place.
//  - xz clusters (ZIM 5) go through XZ Embedded in single-call mode (no dictionary of its own).
//  - Split files (name.zimaa, name.zimab, ...) are one virtual file.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NVZIM_OK = 0,
    NVZIM_ENOTFOUND = -1,     // no such entry / metadata / title
    NVZIM_EIO = -2,           // the file could not be read
    NVZIM_EFORMAT = -3,       // not a ZIM file, or a damaged one
    NVZIM_ENOMEM = -4,
    NVZIM_EUNSUPPORTED = -5,  // zlib / bzip2 clusters, blobs larger than blob_max, ...
    NVZIM_ELOOP = -6,         // redirect chain too long or circular
    NVZIM_ECANCELED = -7,     // a progress callback asked to stop
    NVZIM_EMISMATCH = -8,     // nvzim_verify: the MD5 checksum does not match
};

#define NVZIM_PATH_MAX  512   // bytes with the NUL; longer paths or titles make a dirent invalid
#define NVZIM_TITLE_MAX 512

#define NVZIM_MIME_REDIRECT   0xFFFF
#define NVZIM_MIME_LINKTARGET 0xFFFE   // obsolete entry kinds, kept for old files
#define NVZIM_MIME_DELETED    0xFFFD

typedef struct nvzim nvzim_t;

typedef struct {
    // Optional backend: reads n bytes at off of the virtual file, returns the count read (n unless
    // past the end) or -1. With it, `path` in nvzim_open is only a label and `size` is required.
    long (*read)(void *ctx, uint64_t off, void *buf, size_t n);
    void *ctx;
    uint64_t size;
    unsigned cache_pages;   // 4 KiB pages for index reads (default 64 = 256 KiB)
    unsigned clusters;      // decoded clusters kept (default 2, each up to cluster_max)
    size_t cluster_max;     // largest decoded cluster accepted (default 8 MiB)
    size_t blob_max;        // largest blob nvzim_read returns (default 32 MiB)
    unsigned tree_depth;    // memoised levels of the path and title searches (default 12, max 20)
} nvzim_config_t;

typedef struct {
    uint16_t major, minor;
    uint8_t uuid[16];
    uint32_t entry_count;     // all entries, every namespace
    uint32_t cluster_count;
    uint64_t size;            // bytes, all parts together
    unsigned parts;           // files making the virtual file (1 unless split)
    int new_scheme;           // 1: namespaces C M W X (ZIM 6.1+); 0: A I - M ... (older files)
    char article_ns;          // 'C' or 'A'
    int has_checksum;
} nvzim_info_t;

typedef struct {
    uint32_t index;           // position in path order (what redirects and listings point to)
    char ns;
    uint16_t mime;            // index into the MIME list, or NVZIM_MIME_*
    uint32_t cluster, blob;   // content entries
    uint32_t target;          // redirects
    char path[NVZIM_PATH_MAX];
    char title[NVZIM_TITLE_MAX];   // an empty title in the file means "the path": filled in here
} nvzim_entry_t;

typedef struct {
    const uint8_t *data;
    uint64_t size;
    void *owned_;             // internal: set when nvzim_read allocated the data
} nvzim_blob_t;

typedef struct {
    uint64_t reads, read_bytes;       // reads that reached the file (or the backend)
    uint64_t page_hits, page_misses;
    uint64_t decoded_bytes;           // cluster bytes decompressed
    uint64_t tree_hits;               // binary-search probes answered from memory
} nvzim_stats_t;

// Opens a .zim file, or the first part of a split one (name.zimaa, or name.zim when only the parts
// exist). cfg may be NULL. *out is NULL on error.
int nvzim_open(nvzim_t **out, const char *path, const nvzim_config_t *cfg);
void nvzim_close(nvzim_t *z);

const nvzim_info_t *nvzim_get_info(const nvzim_t *z);
const nvzim_stats_t *nvzim_get_stats(const nvzim_t *z);
const char *nvzim_mime_name(const nvzim_t *z, uint16_t mime);   // "" when out of range
const char *nvzim_strerror(int err);

// Entries
int nvzim_entry_at(nvzim_t *z, uint32_t index, nvzim_entry_t *e);
int nvzim_find(nvzim_t *z, char ns, const char *path, nvzim_entry_t *e);
// An article by its path in the article namespace ('C', or 'A' in old files). A leading "C/" or
// "A/" is accepted too.
int nvzim_find_article(nvzim_t *z, const char *path, nvzim_entry_t *e);
int nvzim_resolve(nvzim_t *z, nvzim_entry_t *e);        // follows redirects in place
int nvzim_main_page(nvzim_t *z, nvzim_entry_t *e);      // resolved
static inline int nvzim_is_redirect(const nvzim_entry_t *e) { return e->mime == NVZIM_MIME_REDIRECT; }

// Content. data stays valid until the next call on z (it may point into a decoded cluster);
// release every blob with nvzim_blob_release.
int nvzim_read(nvzim_t *z, const nvzim_entry_t *e, nvzim_blob_t *b);
void nvzim_blob_release(nvzim_t *z, nvzim_blob_t *b);
// Metadata M/<name> as a NUL-terminated string (truncated to cap). Returns its full length.
int nvzim_metadata(nvzim_t *z, const char *name, char *buf, size_t cap);

// Titles: the listing of front articles (X/listing/titleOrdered/v1), or the full title index of
// older files restricted to the article namespace. Ordered bytewise (case-sensitive). The listing
// is located on the first title call, not at open.
int nvzim_title_count(nvzim_t *z);   // titles in the listing (0 = no title search), or < 0
int nvzim_title_at(nvzim_t *z, uint32_t pos, nvzim_entry_t *e);
int nvzim_title_lower_bound(nvzim_t *z, const char *prefix, uint32_t *pos);   // first title >= prefix
// Titles starting with the query, also tried with a capital first letter, all lowercase and with
// capitalised words. Duplicates (two titles leading to the same article) are dropped; an exact
// match comes first. Entries are returned as listed (redirects not resolved). Returns the count.
int nvzim_suggest(nvzim_t *z, const char *query, nvzim_entry_t *out, int max);
// A random article (resolved) from the title listing; seed picks it.
int nvzim_random(nvzim_t *z, uint32_t seed, nvzim_entry_t *e);

// Checks the MD5 at the end of the file. progress (may be NULL) returns non-zero to cancel.
int nvzim_verify(nvzim_t *z, int (*progress)(void *ctx, uint64_t done, uint64_t total), void *ctx);

#ifdef __cplusplus
}
#endif
