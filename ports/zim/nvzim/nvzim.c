// nvzim.c — reader for ZIM files; see nvzim.h for the design and docs/ZIM_READER_PLAN.md for the
// measurements behind it. Every offset, count and length read from the file is checked against the
// file size before use: ZIM files come from the Internet (fuzzed in tests/host/fuzz/fuzz_nvzim.cpp).
#define _FILE_OFFSET_BITS 64   // 64-bit off_t on 32-bit hosts (the WASI libc has it anyway)
#include "nvzim.h"
#include "nvzim_md5.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ZSTD_STATIC_LINKING_ONLY   // ZSTD_d_stableOutBuffer
#include "zstd.h"
#include "xz.h"

#ifndef O_BINARY
#define O_BINARY 0
#endif

#define HDR_SIZE      80u
#define PAGE          4096u
#define SPAN_MAX      64u                 // entries left when a path search reads its range at once
#define SPAN_BYTES    (64u * 1024)        // ...if their dirents fit in this many bytes
#define DIRENT_MAX    (16 + NVZIM_PATH_MAX + NVZIM_TITLE_MAX + 255)
#define REDIRECT_MAX  16
#define MIME_BYTES    65536u
#define IN_CHUNK      (64u * 1024)        // compressed input per refill
#define FEED_STEP     (16u * 1024)        // ...given to zstd this much at a time
#define XZ_IN_MAX     (32u << 20)
#define LISTING_MEM_MAX (16u << 20)       // a title listing inside a compressed cluster is copied
#define VERIFY_CHUNK  (256u * 1024)
#define NONE64        UINT64_MAX

typedef struct {
    int fd;
    uint64_t start, size, pos;            // pos: where the fd is now (NONE64 = unknown)
} part_t;

typedef struct {
    char ns;
    char *key;                            // path or title; NULL = not seen yet
} node_t;

typedef struct {
    int64_t cluster;                      // -1 = free
    int comp, ext;
    uint8_t *out;
    size_t cap, pos;
    int done;                             // frame decoded to its end
    uint64_t in_off, in_end;              // next compressed byte, read limit
    ZSTD_DCtx *dctx;
    uint32_t used;
} slot_t;

struct nvzim {
    nvzim_config_t cfg;
    nvzim_info_t info;
    nvzim_stats_t st;
    part_t *parts;
    unsigned nparts;
    uint64_t path_ptr, title_idx, cluster_ptr, mime_pos, checksum_pos;
    uint32_t main_page;
    char *mime_blob;
    const char **mimes;
    unsigned nmimes;
    // page cache
    uint8_t *pages;
    uint64_t *page_off;
    uint32_t *page_used;
    uint32_t clock;
    // memoised binary searches (heap order, node 1 = root)
    node_t *ptree, *ttree;
    unsigned tree_n;
    // title listing
    int lst_ready, lst_kind;              // kind: 0 none, 1 u32 array in the file, 2 in memory
    int lst_full;                         // v0 index of every namespace: compare the namespace too
    uint64_t lst_off;
    uint32_t *lst_mem;
    uint32_t lst_lo, lst_n;
    // clusters
    slot_t *slots;
    uint32_t sclock;
    uint8_t *in;                          // IN_CHUNK
    uint8_t *span;                        // SPAN_BYTES + DIRENT_MAX
};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static int in_file(const nvzim_t *z, uint64_t off, uint64_t n) {
    return off <= z->info.size && n <= z->info.size - off;
}

const char *nvzim_strerror(int err) {
    switch (err) {
    case NVZIM_OK: return "ok";
    case NVZIM_ENOTFOUND: return "not found";
    case NVZIM_EIO: return "read error";
    case NVZIM_EFORMAT: return "not a ZIM file, or a damaged one";
    case NVZIM_ENOMEM: return "out of memory";
    case NVZIM_EUNSUPPORTED: return "unsupported content";
    case NVZIM_ELOOP: return "redirect loop";
    case NVZIM_ECANCELED: return "canceled";
    case NVZIM_EMISMATCH: return "checksum mismatch";
    default: return "error";
    }
}

// ------------------------------------------------------------------------------------------ I/O

static int io_read(nvzim_t *z, uint64_t off, void *buf, size_t n) {
    if (!in_file(z, off, n)) return NVZIM_EFORMAT;
    z->st.reads++;
    z->st.read_bytes += n;
    if (z->cfg.read) {
        const long r = z->cfg.read(z->cfg.ctx, off, buf, n);
        return r >= 0 && (size_t)r == n ? NVZIM_OK : NVZIM_EIO;
    }
    uint8_t *d = (uint8_t *)buf;
    unsigned i = 0;
    while (n) {
        while (i < z->nparts && off >= z->parts[i].start + z->parts[i].size) i++;
        if (i >= z->nparts) return NVZIM_EIO;
        part_t *p = &z->parts[i];
        const uint64_t rel = off - p->start;
        const size_t take = n < p->size - rel ? n : (size_t)(p->size - rel);
        // lseek + read, never pread: see nvzim.h. A read that continues where the last one ended
        // does not even seek.
        if (p->pos != rel) {
            if (lseek(p->fd, (off_t)rel, SEEK_SET) == (off_t)-1) { p->pos = NONE64; return NVZIM_EIO; }
            p->pos = rel;
        }
        size_t got = 0;
        while (got < take) {
            const ssize_t r = read(p->fd, d + got, take - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { p->pos = NONE64; return NVZIM_EIO; }
            got += (size_t)r;
        }
        p->pos += take;
        d += take;
        off += take;
        n -= take;
    }
    return NVZIM_OK;
}

// Small structural reads (pointers, dirents, listings) through the LRU of 4 KiB-aligned pages.
static int cread(nvzim_t *z, uint64_t off, void *buf, size_t n) {
    if (!z->cfg.cache_pages || n > 2 * PAGE) return io_read(z, off, buf, n);
    if (!in_file(z, off, n)) return NVZIM_EFORMAT;
    uint8_t *d = (uint8_t *)buf;
    while (n) {
        const uint64_t p = off & ~(uint64_t)(PAGE - 1);
        unsigned k = UINT_MAX, lru = 0;
        for (unsigned i = 0; i < z->cfg.cache_pages; i++) {
            if (z->page_used[i] && z->page_off[i] == p) { k = i; break; }
            if (z->page_used[i] < z->page_used[lru]) lru = i;
        }
        if (k == UINT_MAX) {
            k = lru;
            const size_t len = z->info.size - p < PAGE ? (size_t)(z->info.size - p) : PAGE;
            z->page_used[k] = 0;
            const int r = io_read(z, p, z->pages + (size_t)k * PAGE, len);
            if (r) return r;
            z->page_off[k] = p;
            z->st.page_misses++;
        } else {
            z->st.page_hits++;
        }
        z->page_used[k] = ++z->clock;
        const size_t o = (size_t)(off - p);
        const size_t c = PAGE - o < n ? PAGE - o : n;
        memcpy(d, z->pages + (size_t)k * PAGE + o, c);
        d += c;
        off += c;
        n -= c;
    }
    return NVZIM_OK;
}

// --------------------------------------------------------------------------------------- dirents

// Parses the dirent in b[0..n). Returns its length, 0 when n is too short to tell, -1 if invalid.
static long parse_dirent(const uint8_t *b, size_t n, nvzim_entry_t *e) {
    if (n < 8) return 0;
    const uint16_t mime = le16(b);
    const uint8_t plen = b[2];
    size_t p;
    e->cluster = e->blob = e->target = 0;
    if (mime == NVZIM_MIME_REDIRECT) {
        if (n < 12) return 0;
        e->target = le32(b + 8);
        p = 12;
    } else if (mime == NVZIM_MIME_LINKTARGET || mime == NVZIM_MIME_DELETED) {
        p = 8;
    } else {
        if (n < 16) return 0;
        e->cluster = le32(b + 8);
        e->blob = le32(b + 12);
        p = 16;
    }
    e->mime = mime;
    e->ns = (char)b[3];
    size_t avail = n - p < NVZIM_PATH_MAX ? n - p : NVZIM_PATH_MAX;
    const uint8_t *s = (const uint8_t *)memchr(b + p, 0, avail);
    if (!s) return n - p >= NVZIM_PATH_MAX ? -1 : 0;
    const size_t pl = (size_t)(s - (b + p));
    memcpy(e->path, b + p, pl);
    e->path[pl] = 0;
    p += pl + 1;
    avail = n - p < NVZIM_TITLE_MAX ? n - p : NVZIM_TITLE_MAX;
    const uint8_t *t = (const uint8_t *)memchr(b + p, 0, avail);
    if (!t) return n - p >= NVZIM_TITLE_MAX ? -1 : 0;
    const size_t tl = (size_t)(t - (b + p));
    if (tl) {
        memcpy(e->title, b + p, tl);
        e->title[tl] = 0;
    } else {
        memcpy(e->title, e->path, pl + 1);   // libzim: an empty title is the path
    }
    p += tl + 1;
    if (n - p < plen) return 0;
    return (long)(p + plen);
}

static int read_dirent(nvzim_t *z, uint64_t off, nvzim_entry_t *e) {
    uint8_t b[DIRENT_MAX];
    if (off >= z->info.size) return NVZIM_EFORMAT;
    const uint64_t left = z->info.size - off;
    size_t n = left < 512 ? (size_t)left : 512;
    int r = cread(z, off, b, n);
    if (r) return r;
    long len = parse_dirent(b, n, e);
    if (len == 0 && n < left) {
        n = left < DIRENT_MAX ? (size_t)left : DIRENT_MAX;
        if ((r = cread(z, off, b, n))) return r;
        len = parse_dirent(b, n, e);
    }
    return len > 0 ? NVZIM_OK : NVZIM_EFORMAT;
}

int nvzim_entry_at(nvzim_t *z, uint32_t index, nvzim_entry_t *e) {
    if (index >= z->info.entry_count) return NVZIM_ENOTFOUND;
    uint8_t p[8];
    const int r = cread(z, z->path_ptr + 8ull * index, p, 8);
    if (r) return r;
    const int r2 = read_dirent(z, le64(p), e);
    e->index = index;
    return r2;
}

static int key_cmp(char ans, const char *a, char bns, const char *b) {
    if (ans != bns) return (unsigned char)ans < (unsigned char)bns ? -1 : 1;
    return strcmp(a, b);   // compares as unsigned char: the byte order of the file
}

// Memoised probe: the key of node `node` of a search tree, read through `fetch` the first time.
static node_t *memo(nvzim_t *z, node_t *tree, unsigned node) {
    if (!tree || node >= z->tree_n || !tree[node].key) return NULL;
    z->st.tree_hits++;
    return &tree[node];
}

static void memo_put(nvzim_t *z, node_t *tree, unsigned node, char ns, const char *key) {
    if (!tree || node >= z->tree_n || tree[node].key) return;
    const size_t n = strlen(key) + 1;
    char *k = (char *)malloc(n);
    if (!k) return;                        // memoisation is an optimisation: carry on without
    memcpy(k, key, n);
    tree[node].key = k;
    tree[node].ns = ns;
}

static unsigned child(const nvzim_t *z, unsigned node, int right) {
    if (node >= z->tree_n) return z->tree_n;   // past the memoised levels (and no overflow)
    return 2 * node + (right ? 1 : 0);
}

// The last SPAN_MAX entries of a path search: their pointers in one read, their dirents (stored in
// path order, so contiguous) in a second one.
static int span_find(nvzim_t *z, uint32_t lo, uint32_t hi, char ns, const char *path, nvzim_entry_t *e) {
    if (lo >= hi) return NVZIM_ENOTFOUND;
    const uint32_t n = hi - lo;
    uint8_t pb[8 * SPAN_MAX];
    int r = cread(z, z->path_ptr + 8ull * lo, pb, 8 * (size_t)n);
    if (r) return r;
    const uint64_t first = le64(pb), last = le64(pb + 8 * (n - 1));
    int ordered = first < z->info.size && last >= first && last - first <= SPAN_BYTES;
    for (uint32_t k = 1; ordered && k < n; k++) ordered = le64(pb + 8 * k) > le64(pb + 8 * (k - 1));
    if (ordered) {
        const uint64_t left = z->info.size - first, want = last - first + DIRENT_MAX;
        const size_t len = (size_t)(want < left ? want : left);
        if ((r = io_read(z, first, z->span, len))) return r;
        uint32_t a = 0, b = n;
        while (a < b) {
            const uint32_t m = a + (b - a) / 2;
            const size_t rel = (size_t)(le64(pb + 8 * m) - first);
            if (parse_dirent(z->span + rel, len - rel, e) <= 0) return NVZIM_EFORMAT;
            const int c = key_cmp(e->ns, e->path, ns, path);
            if (!c) { e->index = lo + m; return NVZIM_OK; }
            if (c < 0) a = m + 1; else b = m;
        }
        return NVZIM_ENOTFOUND;
    }
    while (lo < hi) {   // an unusual file: probe one dirent at a time
        const uint32_t m = lo + (hi - lo) / 2;
        if ((r = nvzim_entry_at(z, m, e))) return r;
        const int c = key_cmp(e->ns, e->path, ns, path);
        if (!c) return NVZIM_OK;
        if (c < 0) lo = m + 1; else hi = m;
    }
    return NVZIM_ENOTFOUND;
}

int nvzim_find(nvzim_t *z, char ns, const char *key, nvzim_entry_t *e) {
    if (!key || strlen(key) >= NVZIM_PATH_MAX) return NVZIM_ENOTFOUND;
    char path[NVZIM_PATH_MAX];   // e is scratch below, and key may be e->path
    strcpy(path, key);
    uint32_t lo = 0, hi = z->info.entry_count;
    unsigned node = 1;
    while (hi - lo > SPAN_MAX) {
        const uint32_t mid = lo + (hi - lo) / 2;
        int c;
        const node_t *m = memo(z, z->ptree, node);
        if (m) {
            c = key_cmp(m->ns, m->key, ns, path);
            if (!c) return nvzim_entry_at(z, mid, e);
        } else {
            const int r = nvzim_entry_at(z, mid, e);
            if (r) return r;
            memo_put(z, z->ptree, node, e->ns, e->path);
            c = key_cmp(e->ns, e->path, ns, path);
            if (!c) return NVZIM_OK;
        }
        if (c < 0) lo = mid + 1; else hi = mid;
        node = child(z, node, c < 0);
    }
    return span_find(z, lo, hi, ns, path, e);
}

int nvzim_find_article(nvzim_t *z, const char *path, nvzim_entry_t *e) {
    if (!path) return NVZIM_ENOTFOUND;
    int r = nvzim_find(z, z->info.article_ns, path, e);
    if (r == NVZIM_ENOTFOUND && path[0] && path[1] == '/' && path[2])
        r = nvzim_find(z, path[0], path + 2, e);   // "C/Path", "A/Path", "I/x.png", "M/Title"...
    return r;
}

int nvzim_resolve(nvzim_t *z, nvzim_entry_t *e) {
    for (int hops = 0; e->mime == NVZIM_MIME_REDIRECT; hops++) {
        if (hops >= REDIRECT_MAX) return NVZIM_ELOOP;
        const int r = nvzim_entry_at(z, e->target, e);
        if (r) return r == NVZIM_ENOTFOUND ? NVZIM_EFORMAT : r;
    }
    return NVZIM_OK;
}

int nvzim_main_page(nvzim_t *z, nvzim_entry_t *e) {
    int r = NVZIM_ENOTFOUND;
    if (z->main_page != UINT32_MAX) r = nvzim_entry_at(z, z->main_page, e);
    if (r && z->info.new_scheme) r = nvzim_find(z, 'W', "mainPage", e);
    return r ? r : nvzim_resolve(z, e);
}

// -------------------------------------------------------------------------------------- clusters

static int cluster_info(nvzim_t *z, uint32_t c, uint64_t *start, uint64_t *end, int *comp, int *ext) {
    if (c >= z->info.cluster_count) return NVZIM_EFORMAT;
    uint8_t p[16];
    const int two = c + 1 < z->info.cluster_count;
    int r = cread(z, z->cluster_ptr + 8ull * c, p, two ? 16 : 8);
    if (r) return r;
    *start = le64(p);
    if (*start >= z->info.size) return NVZIM_EFORMAT;
    // Where the compressed data can end: the next cluster, else the next structure of the file.
    // It only bounds the reads (a frame ends by itself), so an odd layout costs reading time only.
    *end = z->info.size;
    const uint64_t cand[6] = {two ? le64(p + 8) : NONE64, z->path_ptr, z->title_idx, z->cluster_ptr,
                              z->info.has_checksum ? z->checksum_pos : NONE64, z->mime_pos};
    for (int i = 0; i < 6; i++)
        if (cand[i] > *start && cand[i] < *end) *end = cand[i];
    uint8_t info;
    if ((r = cread(z, *start, &info, 1))) return r;
    *comp = info & 0x0F;
    *ext = (info & 0x10) != 0;
    return NVZIM_OK;
}

// Blob `blob` of a cluster whose data (after the info byte) is `data[0..len)`: its byte range.
static int blob_range(const uint8_t *data, size_t len, int ext, uint32_t blob, uint64_t *b0, uint64_t *b1) {
    const size_t w = ext ? 8 : 4;
    if (len < w) return NVZIM_EFORMAT;
    const uint64_t first = ext ? le64(data) : le32(data);
    if (first < w || first % w) return NVZIM_EFORMAT;
    if ((uint64_t)blob >= first / w - 1) return NVZIM_EFORMAT;
    if ((blob + 2ull) * w > len) return NVZIM_EFORMAT;
    *b0 = ext ? le64(data + blob * w) : le32(data + (size_t)blob * w);
    *b1 = ext ? le64(data + (blob + 1) * w) : le32(data + ((size_t)blob + 1) * w);
    if (*b0 < first || *b1 < *b0) return NVZIM_EFORMAT;
    return NVZIM_OK;
}

// Where a blob of an uncompressed cluster lies in the file.
static int raw_blob_loc(nvzim_t *z, uint64_t start, int ext, uint32_t blob, uint64_t *off, uint64_t *len) {
    const size_t w = ext ? 8 : 4;
    uint8_t t[8], p[16];
    int r = cread(z, start + 1, t, w);
    if (r) return r;
    const uint64_t first = ext ? le64(t) : le32(t);
    if (first < w || first % w || (uint64_t)blob >= first / w - 1) return NVZIM_EFORMAT;
    if ((r = cread(z, start + 1 + (uint64_t)blob * w, p, 2 * w))) return r;
    const uint64_t b0 = ext ? le64(p) : le32(p), b1 = ext ? le64(p + 8) : le32(p + 4);
    if (b0 < first || b1 < b0 || !in_file(z, start + 1, b1)) return NVZIM_EFORMAT;
    *off = start + 1 + b0;
    *len = b1 - b0;
    return NVZIM_OK;
}

static void slot_reset(slot_t *s, int64_t cluster, int comp, int ext, uint64_t in_off, uint64_t in_end) {
    s->cluster = cluster;
    s->comp = comp;
    s->ext = ext;
    s->pos = 0;
    s->done = 0;
    s->in_off = in_off;
    s->in_end = in_end;
}

static slot_t *slot_get(nvzim_t *z, uint32_t c, int comp, int ext, uint64_t start, uint64_t end, int *fresh) {
    slot_t *lru = &z->slots[0];
    for (unsigned i = 0; i < z->cfg.clusters; i++) {
        slot_t *s = &z->slots[i];
        if (s->cluster == (int64_t)c) { s->used = ++z->sclock; *fresh = 0; return s; }
        if (s->used < lru->used) lru = s;
    }
    slot_reset(lru, c, comp, ext, start + 1, end);
    lru->used = ++z->sclock;
    *fresh = 1;
    return lru;
}

static int slot_grow(nvzim_t *z, slot_t *s, uint64_t need) {
    if (need > z->cfg.cluster_max) return NVZIM_EUNSUPPORTED;
    size_t cap = s->cap ? s->cap : (size_t)2 << 20;
    while (cap < need) cap = cap * 2 > z->cfg.cluster_max ? z->cfg.cluster_max : cap * 2;
    if (cap > z->cfg.cluster_max) cap = z->cfg.cluster_max;
    if (cap <= s->cap && s->out) return NVZIM_OK;
    // What the buffer holds is decoded again from the start after a grow, so it goes first: the
    // peak is the new size, not old + new (12 MiB for an 8 MiB cluster, too much for an app).
    free(s->out);
    s->out = NULL;
    s->cap = 0;
    s->pos = 0;
    s->done = 0;
    uint8_t *o = (uint8_t *)malloc(cap);
    if (!o) return NVZIM_ENOMEM;
    s->out = o;
    s->cap = cap;
    return NVZIM_OK;
}

// zstd: decode in place until s->pos >= want or the frame ends. Returns 1 when the output buffer
// filled up first (the caller grows it and starts over).
static int zstd_fill(nvzim_t *z, slot_t *s, size_t want) {
    if (!s->dctx && !(s->dctx = ZSTD_createDCtx())) return NVZIM_ENOMEM;
    if (s->pos == 0 && !s->done) {
        ZSTD_DCtx_reset(s->dctx, ZSTD_reset_session_only);
        if (ZSTD_isError(ZSTD_DCtx_setParameter(s->dctx, ZSTD_d_stableOutBuffer, 1))) return NVZIM_EFORMAT;
    }
    while (s->pos < want && !s->done) {
        if (s->in_off >= s->in_end) return NVZIM_EFORMAT;   // frame cut short
        const uint64_t avail = s->in_end - s->in_off;
        const size_t n = avail < IN_CHUNK ? (size_t)avail : IN_CHUNK;
        int r = io_read(z, s->in_off, z->in, n);
        if (r) return r;
        // The input goes in by FEED_STEP: given all of it, zstd decodes block after block in one
        // call, and stopping early (the point of the stable buffer) would not happen.
        ZSTD_inBuffer in = {z->in, 0, 0};
        ZSTD_outBuffer out = {s->out, s->cap, s->pos};
        int full = 0;
        while (in.pos < n && out.pos < want) {
            if (in.pos == in.size) in.size = in.size + FEED_STEP < n ? in.size + FEED_STEP : n;
            const size_t before_in = in.pos, before_out = out.pos;
            const size_t ret = ZSTD_decompressStream(s->dctx, &out, &in);
            if (ZSTD_isError(ret)) {
                // a stable output buffer too small for the next block is reported as an error
                if (ZSTD_getErrorCode(ret) == ZSTD_error_dstSize_tooSmall) { full = 1; break; }
                s->cluster = -1;
                return NVZIM_EFORMAT;
            }
            if (ret == 0) { s->done = 1; break; }
            if (out.pos == out.size) { full = 1; break; }   // full: the frame continues
            if (in.pos == before_in && out.pos == before_out && in.size == n) break;   // needs more input
        }
        if (full && out.pos < want) return 1;
        z->st.decoded_bytes += out.pos - s->pos;
        s->pos = out.pos;
        s->in_off += in.pos;
        if (!s->done && in.pos == 0 && s->pos < want) { s->cluster = -1; return NVZIM_EFORMAT; }
    }
    return s->pos >= want ? NVZIM_OK : NVZIM_EFORMAT;          // ended before the blob
}

// xz (ZIM 5): the whole cluster in one call; XZ_SINGLE uses the output buffer as the dictionary.
static int xz_fill(nvzim_t *z, slot_t *s) {
    if (s->done) return NVZIM_OK;
    const uint64_t avail = s->in_end - s->in_off;
    const size_t n = avail < XZ_IN_MAX ? (size_t)avail : XZ_IN_MAX;
    uint8_t *in = (uint8_t *)malloc(n ? n : 1);
    if (!in) return NVZIM_ENOMEM;
    int r = io_read(z, s->in_off, in, n);
    if (r) { free(in); return r; }
    static int crc_ready;   // read-only tables once filled
    if (!crc_ready) {
        xz_crc32_init();
        xz_crc64_init();   // built with XZ_USE_CRC64: libzim wrote CRC32, other writers CRC64
        crc_ready = 1;
    }
    for (;;) {
        struct xz_dec *d = xz_dec_init(XZ_SINGLE, 0);
        if (!d) { free(in); return NVZIM_ENOMEM; }
        struct xz_buf b = {in, 0, n, s->out, 0, s->cap};
        const enum xz_ret ret = xz_dec_run(d, &b);
        xz_dec_end(d);
        if (ret == XZ_STREAM_END) {
            z->st.decoded_bytes += b.out_pos;
            s->pos = b.out_pos;
            s->done = 1;
            break;
        }
        if (ret == XZ_BUF_ERROR && b.out_pos == s->cap && s->cap < z->cfg.cluster_max) {
            if ((r = slot_grow(z, s, (uint64_t)s->cap * 2))) { free(in); return r; }
            continue;
        }
        free(in);
        s->cluster = -1;
        return ret == XZ_MEM_ERROR ? NVZIM_ENOMEM : NVZIM_EFORMAT;
    }
    free(in);
    return NVZIM_OK;
}

// Decodes `slot` at least up to byte `want` (zstd), or completely (xz).
static int slot_fill(nvzim_t *z, slot_t *s, uint64_t want, uint64_t start) {
    if (want > z->cfg.cluster_max) return NVZIM_EUNSUPPORTED;
    for (;;) {
        int r;
        if (!s->out || s->cap < want) {
            if (s->pos || s->done) slot_reset(s, s->cluster, s->comp, s->ext, start + 1, s->in_end);
            if ((r = slot_grow(z, s, want))) return r;
        }
        if (s->comp == 4) return xz_fill(z, s);
        r = zstd_fill(z, s, (size_t)want);
        if (r != 1) return r;
        // the output filled before the blob was reached: a bigger buffer, from the start
        if ((r = slot_grow(z, s, (uint64_t)s->cap * 2))) return r;
        slot_reset(s, s->cluster, s->comp, s->ext, start + 1, s->in_end);
    }
}

static int compressed_blob(nvzim_t *z, uint32_t c, uint32_t blob, int comp, int ext, uint64_t start,
                           uint64_t end, nvzim_blob_t *b) {
    int fresh;
    slot_t *s = slot_get(z, c, comp, ext, start, end, &fresh);
    const size_t w = ext ? 8 : 4;
    if ((blob + 2ull) * w > z->cfg.cluster_max) return NVZIM_EFORMAT;   // no cluster has that many blobs
    int r = slot_fill(z, s, (blob + 2ull) * w, start);   // the offset table up to this blob
    if (r) { if (r != NVZIM_EUNSUPPORTED) s->cluster = -1; return r; }
    uint64_t b0, b1;
    if ((r = blob_range(s->out, s->pos, ext, blob, &b0, &b1))) return r;
    if (b1 - b0 > z->cfg.blob_max) return NVZIM_EUNSUPPORTED;
    if ((r = slot_fill(z, s, b1, start))) { if (r != NVZIM_EUNSUPPORTED) s->cluster = -1; return r; }
    if (b1 > s->pos) return NVZIM_EFORMAT;
    b->data = s->out + b0;
    b->size = b1 - b0;
    return NVZIM_OK;
}

int nvzim_read(nvzim_t *z, const nvzim_entry_t *e, nvzim_blob_t *b) {
    memset(b, 0, sizeof *b);
    if (e->mime >= NVZIM_MIME_DELETED) return NVZIM_ENOTFOUND;   // a redirect has no content
    uint64_t start, end;
    int comp, ext;
    int r = cluster_info(z, e->cluster, &start, &end, &comp, &ext);
    if (r) return r;
    if (comp == 0 || comp == 1) {
        uint64_t off, len;
        if ((r = raw_blob_loc(z, start, ext, e->blob, &off, &len))) return r;
        if (len > z->cfg.blob_max) return NVZIM_EUNSUPPORTED;
        uint8_t *d = (uint8_t *)malloc(len ? (size_t)len : 1);
        if (!d) return NVZIM_ENOMEM;
        if ((r = len <= 2 * PAGE ? cread(z, off, d, (size_t)len) : io_read(z, off, d, (size_t)len))) {
            free(d);
            return r;
        }
        b->data = d;
        b->size = len;
        b->owned_ = d;
        return NVZIM_OK;
    }
    if (comp == 4 || comp == 5) return compressed_blob(z, e->cluster, e->blob, comp, ext, start, end, b);
    return comp == 2 || comp == 3 ? NVZIM_EUNSUPPORTED : NVZIM_EFORMAT;   // zlib, bzip2: libzim too
}

void nvzim_blob_release(nvzim_t *z, nvzim_blob_t *b) {
    (void)z;
    if (b) {
        free(b->owned_);
        memset(b, 0, sizeof *b);
    }
}

int nvzim_metadata(nvzim_t *z, const char *name, char *buf, size_t cap) {
    nvzim_entry_t e;
    int r = nvzim_find(z, 'M', name, &e);
    if (!r) r = nvzim_resolve(z, &e);
    nvzim_blob_t b;
    if (!r) r = nvzim_read(z, &e, &b);
    if (r) {
        if (cap) buf[0] = 0;
        return r;
    }
    if (cap) {
        const size_t k = b.size < cap - 1 ? (size_t)b.size : cap - 1;
        memcpy(buf, b.data, k);
        buf[k] = 0;
    }
    const int n = b.size > INT_MAX ? INT_MAX : (int)b.size;
    nvzim_blob_release(z, &b);
    return n;
}

// ---------------------------------------------------------------------------------------- titles

static int listing_index(nvzim_t *z, uint32_t pos, uint32_t *idx) {
    if (z->lst_kind == 2) { *idx = z->lst_mem[pos]; return NVZIM_OK; }
    uint8_t p[4];
    const int r = cread(z, z->lst_off + 4ull * pos, p, 4);
    *idx = le32(p);
    return r;
}

static int listing_init(nvzim_t *z) {
    if (z->lst_ready) return NVZIM_OK;
    z->lst_ready = 1;
    nvzim_entry_t e;
    if (z->info.new_scheme && nvzim_find(z, 'X', "listing/titleOrdered/v1", &e) == NVZIM_OK &&
        e.mime < NVZIM_MIME_DELETED) {
        uint64_t start, end, off, len;
        int comp, ext;
        int r = cluster_info(z, e.cluster, &start, &end, &comp, &ext);
        if (r) return r;
        if (comp == 0 || comp == 1) {
            if ((r = raw_blob_loc(z, start, ext, e.blob, &off, &len))) return r;
            z->lst_kind = 1;
            z->lst_off = off;
        } else {
            nvzim_blob_t b;
            if ((r = nvzim_read(z, &e, &b))) return r;
            len = b.size;
            if (len > LISTING_MEM_MAX || !(z->lst_mem = (uint32_t *)malloc(len ? (size_t)len : 4))) {
                nvzim_blob_release(z, &b);
                return len > LISTING_MEM_MAX ? NVZIM_EUNSUPPORTED : NVZIM_ENOMEM;
            }
            for (uint64_t i = 0; i + 4 <= len; i += 4) z->lst_mem[i / 4] = le32(b.data + i);
            nvzim_blob_release(z, &b);
            z->lst_kind = 2;
        }
        z->lst_n = (uint32_t)(len / 4);
        return NVZIM_OK;
    }
    if (z->title_idx == NONE64) return NVZIM_OK;   // no title search in this file
    // the v0 index: every entry, ordered by namespace then title; keep the article namespace
    z->lst_kind = 1;
    z->lst_off = z->title_idx;
    z->lst_full = 1;
    uint32_t a = 0, b = z->info.entry_count, idx;
    for (int pass = 0; pass < 2; pass++) {   // first position with ns >= article_ns, then > article_ns
        uint32_t lo = 0, hi = z->info.entry_count;
        while (lo < hi) {
            const uint32_t m = lo + (hi - lo) / 2;
            int r = listing_index(z, m, &idx);
            if (!r) r = nvzim_entry_at(z, idx, &e);
            if (r) { z->lst_kind = 0; return r; }
            const unsigned char ns = (unsigned char)e.ns, want = (unsigned char)z->info.article_ns;
            if (pass ? ns <= want : ns < want) lo = m + 1; else hi = m;
        }
        if (pass) b = lo; else a = lo;
    }
    z->lst_lo = a;
    z->lst_n = b > a ? b - a : 0;
    return NVZIM_OK;
}

int nvzim_title_count(nvzim_t *z) {
    const int r = listing_init(z);
    if (r) return r;
    return z->lst_n > INT_MAX ? INT_MAX : (int)z->lst_n;
}

int nvzim_title_at(nvzim_t *z, uint32_t pos, nvzim_entry_t *e) {
    int r = listing_init(z);
    if (r) return r;
    if (pos >= z->lst_n) return NVZIM_ENOTFOUND;
    uint32_t idx;
    if ((r = listing_index(z, z->lst_lo + pos, &idx))) return r;
    return nvzim_entry_at(z, idx, e);
}

int nvzim_title_lower_bound(nvzim_t *z, const char *prefix, uint32_t *pos) {
    int r = listing_init(z);
    if (r) return r;
    nvzim_entry_t e;
    uint32_t lo = 0, hi = z->lst_n;
    unsigned node = 1;
    const char ns = z->lst_full ? z->info.article_ns : 0;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        int c;
        const node_t *m = memo(z, z->ttree, node);
        if (m) {
            c = key_cmp(m->ns, m->key, ns, prefix);
        } else {
            if ((r = nvzim_title_at(z, mid, &e))) return r;
            const char ens = z->lst_full ? e.ns : 0;
            memo_put(z, z->ttree, node, ens, e.title);
            c = key_cmp(ens, e.title, ns, prefix);
        }
        if (c < 0) lo = mid + 1; else hi = mid;
        node = child(z, node, c < 0);
    }
    *pos = lo;
    return NVZIM_OK;
}

// Case variants for suggestions: ASCII and the Latin-1 letters (UTF-8 C3 80..BE).
static void case_variant(const char *in, char *out, size_t cap, int mode) {   // 0 first up, 1 lower, 2 words up
    size_t o = 0;
    int word_start = 1;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 2 < cap; p++) {
        unsigned char ch = *p;
        const int up = mode == 0 ? (o == 0) : mode == 2 ? word_start : 0;
        if (ch == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBE) {
            unsigned char lo = p[1];
            if (up && lo >= 0xA0 && lo != 0xB7) lo -= 0x20;
            else if (mode == 1 && lo >= 0x80 && lo <= 0x9E && lo != 0x97) lo += 0x20;
            out[o++] = (char)ch;
            out[o++] = (char)lo;
            p++;
            word_start = 0;
            continue;
        }
        if (up && ch >= 'a' && ch <= 'z') ch -= 32;
        else if (mode == 1 && ch >= 'A' && ch <= 'Z') ch += 32;
        out[o++] = (char)ch;
        word_start = ch == ' ' || ch == '-' || ch == '(' || ch == '_';
    }
    out[o] = 0;
}

int nvzim_suggest(nvzim_t *z, const char *query, nvzim_entry_t *out, int max) {
    if (!query || !query[0] || max <= 0) return 0;
    const size_t ql = strlen(query);
    if (ql >= NVZIM_TITLE_MAX) return 0;
    int r = listing_init(z);
    if (r) return r;
    char var[4][NVZIM_TITLE_MAX];
    snprintf(var[0], sizeof var[0], "%s", query);
    case_variant(query, var[1], sizeof var[1], 0);
    case_variant(query, var[2], sizeof var[2], 1);
    case_variant(query, var[3], sizeof var[3], 2);
    uint32_t *seen = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)max);
    if (!seen) return NVZIM_ENOMEM;
    char lq[NVZIM_TITLE_MAX], lt[NVZIM_TITLE_MAX];
    case_variant(query, lq, sizeof lq, 1);
    int count = 0;
    nvzim_entry_t t;
    for (int v = 0; v < 4 && count < max; v++) {
        int dup = 0;
        for (int u = 0; u < v; u++) dup |= !strcmp(var[u], var[v]);
        if (dup) continue;
        uint32_t pos;
        if ((r = nvzim_title_lower_bound(z, var[v], &pos))) break;
        const size_t vl = strlen(var[v]);
        for (int steps = 0; count < max && steps < max * 4; steps++, pos++) {
            if (nvzim_title_at(z, pos, &out[count]) != NVZIM_OK) break;
            if (strncmp(out[count].title, var[v], vl)) break;
            t = out[count];
            if (nvzim_resolve(z, &t) != NVZIM_OK) continue;
            int again = 0;
            for (int k = 0; k < count && !again; k++) again = seen[k] == t.index;
            if (again) continue;
            seen[count] = t.index;
            // an exact match (ignoring case) goes first
            case_variant(out[count].title, lt, sizeof lt, 1);
            if (!strcmp(lt, lq) && count > 0) {
                const nvzim_entry_t x = out[count];
                const uint32_t sx = seen[count];
                memmove(&out[1], &out[0], sizeof out[0] * (size_t)count);
                memmove(&seen[1], &seen[0], sizeof seen[0] * (size_t)count);
                out[0] = x;
                seen[0] = sx;
            }
            count++;
        }
    }
    free(seen);
    return count;
}

int nvzim_random(nvzim_t *z, uint32_t seed, nvzim_entry_t *e) {
    int r = listing_init(z);
    if (r) return r;
    if (!z->lst_n) return NVZIM_ENOTFOUND;
    uint32_t x = seed * 2654435761u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    const uint32_t start = x % z->lst_n;
    for (uint32_t k = 0; k < 64 && k < z->lst_n; k++) {   // prefer an article over a redirect to it
        if ((r = nvzim_title_at(z, (start + k) % z->lst_n, e))) return r;
        if (e->mime != NVZIM_MIME_REDIRECT) return NVZIM_OK;
    }
    return nvzim_resolve(z, e);
}

// --------------------------------------------------------------------------------------- scan

long nvzim_scan(nvzim_t *z, char ns, int (*fn)(void *ctx, const nvzim_entry_t *e), void *ctx) {
    nvzim_entry_t e;
    // the first entry of the namespace (entries are in namespace order)
    uint32_t lo = 0, hi = z->info.entry_count;
    while (lo < hi) {
        const uint32_t m = lo + (hi - lo) / 2;
        const int r = nvzim_entry_at(z, m, &e);
        if (r) return r;
        if ((unsigned char)e.ns < (unsigned char)ns) lo = m + 1; else hi = m;
    }
    long seen = 0;
    uint8_t pb[8 * SPAN_MAX];
    for (uint32_t i = lo; i < z->info.entry_count;) {
        const uint32_t n = z->info.entry_count - i < SPAN_MAX ? z->info.entry_count - i : SPAN_MAX;
        int r = io_read(z, z->path_ptr + 8ull * i, pb, 8 * (size_t)n);
        if (r) return r;
        const uint64_t first = le64(pb), last = le64(pb + 8 * (n - 1));
        int ordered = first < z->info.size && last >= first && last - first <= SPAN_BYTES;
        for (uint32_t k = 1; ordered && k < n; k++) ordered = le64(pb + 8 * k) > le64(pb + 8 * (k - 1));
        size_t len = 0;
        if (ordered) {
            const uint64_t left = z->info.size - first, want = last - first + DIRENT_MAX;
            len = (size_t)(want < left ? want : left);
            if ((r = io_read(z, first, z->span, len))) return r;
        }
        for (uint32_t k = 0; k < n; k++) {
            if (ordered) {
                const size_t rel = (size_t)(le64(pb + 8 * k) - first);
                if (parse_dirent(z->span + rel, len - rel, &e) <= 0) return NVZIM_EFORMAT;
                e.index = i + k;
            } else if ((r = nvzim_entry_at(z, i + k, &e))) {
                return r;
            }
            if (e.ns != ns) return seen;   // past the namespace
            seen++;
            if (fn && fn(ctx, &e)) return seen;
        }
        i += n;
    }
    return seen;
}

// ------------------------------------------------------------------------------------ open/close

static int open_parts(nvzim_t *z, const char *path) {
    const size_t pl = strlen(path);
    int series = pl > 6 && !strcmp(path + pl - 6, ".zimaa");
    int fd = series ? -1 : open(path, O_RDONLY | O_BINARY);
    if (fd < 0 && !series && pl > 4 && !strcmp(path + pl - 4, ".zim")) series = 2;   // name.zim -> name.zimaa
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) || st.st_size < 0) { close(fd); return NVZIM_EIO; }
        if (!(z->parts = (part_t *)calloc(1, sizeof(part_t)))) { close(fd); return NVZIM_ENOMEM; }
        z->parts[0] = (part_t){fd, 0, (uint64_t)st.st_size, 0};
        z->nparts = 1;
        z->info.size = (uint64_t)st.st_size;
        return NVZIM_OK;
    }
    if (!series) return errno == ENOENT ? NVZIM_ENOTFOUND : NVZIM_EIO;
    const size_t base = series == 2 ? pl : pl - 2;   // up to and including ".zim"
    char *name = (char *)malloc(base + 3);
    if (!name) return NVZIM_ENOMEM;
    memcpy(name, path, base);
    for (unsigned i = 0; i < 26 * 26; i++) {
        name[base] = (char)('a' + i / 26);
        name[base + 1] = (char)('a' + i % 26);
        name[base + 2] = 0;
        fd = open(name, O_RDONLY | O_BINARY);
        if (fd < 0) break;
        struct stat st;
        part_t *np = (part_t *)realloc(z->parts, sizeof(part_t) * (z->nparts + 1));
        if (!np || fstat(fd, &st) || st.st_size <= 0) {
            close(fd);
            if (np) z->parts = np;
            free(name);
            return np ? NVZIM_EIO : NVZIM_ENOMEM;
        }
        z->parts = np;
        z->parts[z->nparts++] = (part_t){fd, z->info.size, (uint64_t)st.st_size, 0};
        z->info.size += (uint64_t)st.st_size;
    }
    free(name);
    return z->nparts ? NVZIM_OK : NVZIM_ENOTFOUND;
}

static int read_mimes(nvzim_t *z) {
    if (z->mime_pos >= z->info.size) return NVZIM_EFORMAT;
    const uint64_t left = z->info.size - z->mime_pos;
    const size_t n = left < MIME_BYTES ? (size_t)left : MIME_BYTES;
    if (!(z->mime_blob = (char *)malloc(n + 1))) return NVZIM_ENOMEM;
    int r = io_read(z, z->mime_pos, z->mime_blob, n);
    if (r) return r;
    z->mime_blob[n] = 0;
    unsigned cap = 0;
    for (size_t p = 0; p < n && z->mime_blob[p];) {
        const size_t l = strnlen(z->mime_blob + p, n - p);
        if (p + l >= n) return NVZIM_EFORMAT;   // the list must end with an empty string
        if (z->nmimes == cap) {
            cap = cap ? cap * 2 : 32;
            const char **m = (const char **)realloc(z->mimes, sizeof(char *) * cap);
            if (!m) return NVZIM_ENOMEM;
            z->mimes = m;
        }
        z->mimes[z->nmimes++] = z->mime_blob + p;
        if (z->nmimes >= NVZIM_MIME_DELETED) return NVZIM_EFORMAT;
        p += l + 1;
    }
    return NVZIM_OK;
}

static int parse_header(nvzim_t *z) {
    uint8_t h[HDR_SIZE];
    if (z->info.size < HDR_SIZE) return NVZIM_EFORMAT;
    int r = io_read(z, 0, h, HDR_SIZE);
    if (r) return r;
    if (le32(h) != 0x044D495Au) return NVZIM_EFORMAT;
    z->info.major = le16(h + 4);
    z->info.minor = le16(h + 6);
    if (z->info.major != 5 && z->info.major != 6) return NVZIM_EUNSUPPORTED;
    memcpy(z->info.uuid, h + 8, 16);
    z->info.entry_count = le32(h + 24);
    z->info.cluster_count = le32(h + 28);
    z->path_ptr = le64(h + 32);
    z->title_idx = le64(h + 40);
    z->cluster_ptr = le64(h + 48);
    z->mime_pos = le64(h + 56);
    z->main_page = le32(h + 64);
    z->checksum_pos = le64(h + 72);
    if (!in_file(z, z->path_ptr, 8ull * z->info.entry_count) ||
        !in_file(z, z->cluster_ptr, 8ull * z->info.cluster_count) || z->mime_pos < 72)
        return NVZIM_EFORMAT;
    if (z->title_idx != NONE64 && !in_file(z, z->title_idx, 4ull * z->info.entry_count)) z->title_idx = NONE64;
    if (z->main_page != UINT32_MAX && z->main_page >= z->info.entry_count) z->main_page = UINT32_MAX;
    z->info.has_checksum = z->checksum_pos != NONE64 && z->checksum_pos >= HDR_SIZE &&
                           in_file(z, z->checksum_pos, 16);
    z->info.new_scheme = z->info.major == 6 && z->info.minor >= 1;
    z->info.article_ns = z->info.new_scheme ? 'C' : 'A';
    return read_mimes(z);
}

int nvzim_open(nvzim_t **out, const char *path, const nvzim_config_t *cfg) {
    *out = NULL;
    nvzim_t *z = (nvzim_t *)calloc(1, sizeof *z);
    if (!z) return NVZIM_ENOMEM;
    if (cfg) z->cfg = *cfg;
    if (!z->cfg.cache_pages) z->cfg.cache_pages = 64;
    if (!z->cfg.clusters) z->cfg.clusters = 2;
    if (!z->cfg.cluster_max) z->cfg.cluster_max = (size_t)8 << 20;
    if (!z->cfg.blob_max) z->cfg.blob_max = (size_t)32 << 20;
    const unsigned depth = z->cfg.tree_depth ? z->cfg.tree_depth : 12;
    z->tree_n = 1u << (depth > 20 ? 20 : depth);
    int r;
    if (z->cfg.read) {
        z->info.size = z->cfg.size;
        z->info.parts = 1;
        r = NVZIM_OK;
    } else {
        r = path ? open_parts(z, path) : NVZIM_ENOTFOUND;
        z->info.parts = z->nparts;
    }
    if (!r) {
        z->pages = (uint8_t *)malloc((size_t)z->cfg.cache_pages * PAGE);
        z->page_off = (uint64_t *)calloc(z->cfg.cache_pages, sizeof(uint64_t));
        z->page_used = (uint32_t *)calloc(z->cfg.cache_pages, sizeof(uint32_t));
        z->ptree = (node_t *)calloc(z->tree_n, sizeof(node_t));
        z->ttree = (node_t *)calloc(z->tree_n, sizeof(node_t));
        z->slots = (slot_t *)calloc(z->cfg.clusters, sizeof(slot_t));
        z->in = (uint8_t *)malloc(IN_CHUNK);
        z->span = (uint8_t *)malloc(SPAN_BYTES + DIRENT_MAX);
        if (!z->pages || !z->page_off || !z->page_used || !z->ptree || !z->ttree || !z->slots || !z->in || !z->span)
            r = NVZIM_ENOMEM;
    }
    if (!r) {
        for (unsigned i = 0; i < z->cfg.clusters; i++) z->slots[i].cluster = -1;
        r = parse_header(z);
    }
    if (r) {
        nvzim_close(z);
        return r;
    }
    *out = z;
    return NVZIM_OK;
}

void nvzim_close(nvzim_t *z) {
    if (!z) return;
    for (unsigned i = 0; i < z->nparts; i++) close(z->parts[i].fd);
    free(z->parts);
    free(z->mime_blob);
    free(z->mimes);
    free(z->pages);
    free(z->page_off);
    free(z->page_used);
    for (unsigned i = 0; i < z->tree_n; i++) {
        if (z->ptree) free(z->ptree[i].key);
        if (z->ttree) free(z->ttree[i].key);
    }
    free(z->ptree);
    free(z->ttree);
    if (z->slots)
        for (unsigned i = 0; i < z->cfg.clusters; i++) {
            free(z->slots[i].out);
            ZSTD_freeDCtx(z->slots[i].dctx);
        }
    free(z->slots);
    free(z->in);
    free(z->span);
    free(z->lst_mem);
    free(z);
}

const nvzim_info_t *nvzim_get_info(const nvzim_t *z) { return &z->info; }
const nvzim_stats_t *nvzim_get_stats(const nvzim_t *z) { return &z->st; }

const char *nvzim_mime_name(const nvzim_t *z, uint16_t mime) {
    return mime < z->nmimes ? z->mimes[mime] : "";
}

int nvzim_verify(nvzim_t *z, int (*progress)(void *ctx, uint64_t done, uint64_t total), void *ctx) {
    if (!z->info.has_checksum) return NVZIM_ENOTFOUND;
    uint8_t *buf = (uint8_t *)malloc(VERIFY_CHUNK);
    if (!buf) return NVZIM_ENOMEM;
    nvzim_md5_t md5;
    nvzim_md5_init(&md5);
    int r = NVZIM_OK;
    for (uint64_t off = 0; off < z->checksum_pos && !r;) {
        const uint64_t left = z->checksum_pos - off;
        const size_t n = left < VERIFY_CHUNK ? (size_t)left : VERIFY_CHUNK;
        if ((r = io_read(z, off, buf, n))) break;
        nvzim_md5_update(&md5, buf, n);
        off += n;
        if (progress && progress(ctx, off, z->checksum_pos)) r = NVZIM_ECANCELED;
    }
    if (!r) {
        uint8_t want[16], got[16];
        nvzim_md5_final(&md5, got);
        r = io_read(z, z->checksum_pos, want, 16);
        if (!r && memcmp(want, got, 16)) r = NVZIM_EMISMATCH;
    }
    free(buf);
    return r;
}
