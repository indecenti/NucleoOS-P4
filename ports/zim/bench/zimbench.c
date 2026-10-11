// zimbench.c — F0 measurements for the ZIM reader (docs/ZIM_READER_PLAN.md), run in the NucleoOS
// Terminal as a WASI program. A throwaway measuring tool: the real reader is nvzim (F1).
//
//   zimbench seek FILE [N] [SIZE]     N random preads of SIZE bytes, latency per tenth of the file
//   zimbench write FILE MB            sequential write in 1 MiB blocks (makes a big test file)
//   zimbench read FILE [MB]           sequential read in 1 MiB blocks
//   zimbench zstd ZIM [N]             read + one-shot zstd decode of N clusters
//   zimbench lookup ZIM [N]           N dirent lookups by path (binary search), raw and page-cached
//   zimbench title ZIM PREFIX         title-prefix search on X/listing/titleOrdered/v1
//   zimbench get ZIM PATH...          article fetch end to end (lookup, redirects, cluster, blob)
//   zimbench http URL [CHUNKS] [KB]   HTTP Range throughput with nv_http_req
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ZSTD_STATIC_LINKING_ONLY   // ZSTD_d_stableOutBuffer: decode in place, no 8 MiB window
#include "zstd.h"

#ifdef __wasm__
#define NV_IMPORT(sym) __attribute__((import_module("nv"), import_name(sym)))
NV_IMPORT("http_req")    int32_t nv_http_req(const char *spec_json, const void *body, uint32_t len);
NV_IMPORT("http_state")  int32_t nv_http_state(int32_t h);
NV_IMPORT("http_status") int32_t nv_http_status(int32_t h);
NV_IMPORT("http_read")   int32_t nv_http_read(int32_t h, void *buf, uint32_t len);
NV_IMPORT("http_close")  void    nv_http_close(int32_t h);
#else   // native build on the PC, to check the ZIM logic against the fixtures: no network
static int32_t nv_http_req(const char *s, const void *b, uint32_t n) { (void)s; (void)b; (void)n; return -1; }
static int32_t nv_http_state(int32_t h) { (void)h; return -1; }
static int32_t nv_http_status(int32_t h) { (void)h; return 0; }
static int32_t nv_http_read(int32_t h, void *b, uint32_t n) { (void)h; (void)b; (void)n; return 0; }
static void nv_http_close(int32_t h) { (void)h; }
#endif

// ------------------------------------------------------------------------------------- helpers

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static uint32_t g_rng = 2463534242u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static uint64_t rnd64(void) { return ((uint64_t)rnd() << 32) | rnd(); }

static int cmp_d(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void stats(const char *what, double *v, int n) {
    if (n <= 0) { printf("%-24s no samples\n", what); return; }
    qsort(v, n, sizeof *v, cmp_d);
    double s = 0;
    for (int i = 0; i < n; i++) s += v[i];
    printf("%-24s n=%-4d mean %7.2f  median %7.2f  p90 %7.2f  max %7.2f ms\n", what, n, s / n, v[n / 2],
           v[(n * 9) / 10], v[n - 1]);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static uint64_t g_reads;   // pread calls that reached the file system
static int g_seekread;     // -l: lseek + read instead of pread (no seek back to the old position)

static int xpread(int fd, void *buf, size_t n, uint64_t off) {
    g_reads++;
    size_t got = 0;
    if (g_seekread) {
        if (lseek(fd, (off_t)off, SEEK_SET) < 0) return -1;
        while (got < n) {
            const ssize_t r = read(fd, (char *)buf + got, n - got);
            if (r < 0) { if (errno == EINTR) continue; return -1; }
            if (r == 0) break;
            got += (size_t)r;
        }
        return (int)got;
    }
    while (got < n) {
        const ssize_t r = pread(fd, (char *)buf + got, n - got, (off_t)(off + got));
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        got += (size_t)r;
    }
    return (int)got;
}

static uint64_t file_size(int fd) {
    struct stat st;
    return fstat(fd, &st) ? 0 : (uint64_t)st.st_size;
}

// A small LRU of 4 KiB-aligned pages in front of the SD: what nvzim will do for the index reads.
enum { PAGE = 4096, NPAGES = 64 };
static struct { uint64_t off; uint32_t used; int valid; } g_pg[NPAGES];
static uint8_t *g_pgbuf;
static uint32_t g_pgclock;
static int g_cache;
static uint64_t g_hits;

static void cache_reset(void) { memset(g_pg, 0, sizeof g_pg); g_pgclock = 0; }

static int cread(int fd, void *dst, size_t n, uint64_t off) {
    if (!g_cache) return xpread(fd, dst, n, off);
    size_t done = 0;
    while (done < n) {
        const uint64_t p = (off + done) & ~(uint64_t)(PAGE - 1);
        int k = -1, lru = 0;
        for (int i = 0; i < NPAGES; i++) {
            if (g_pg[i].valid && g_pg[i].off == p) { k = i; break; }
            if (g_pg[i].used < g_pg[lru].used) lru = i;
        }
        if (k < 0) {
            k = lru;
            const int r = xpread(fd, g_pgbuf + (size_t)k * PAGE, PAGE, p);
            if (r <= 0) return done ? (int)done : -1;
            if (r < PAGE) memset(g_pgbuf + (size_t)k * PAGE + r, 0, PAGE - (size_t)r);
            g_pg[k].off = p;
            g_pg[k].valid = 1;
        } else {
            g_hits++;
        }
        g_pg[k].used = ++g_pgclock;
        const size_t o = (size_t)(off + done - p);
        size_t c = PAGE - o;
        if (c > n - done) c = n - done;
        memcpy((char *)dst + done, g_pgbuf + (size_t)k * PAGE + o, c);
        done += c;
    }
    return (int)n;
}

// ----------------------------------------------------------------------------------------- ZIM

typedef struct {
    int fd;
    uint16_t major, minor;
    uint32_t entries, clusters, main_page;
    uint64_t path_ptr, title_idx, cluster_ptr, mime_pos, checksum_pos, size;
} zim_t;

typedef struct {
    uint16_t mime;
    char ns;
    int redirect;
    uint32_t a, b;   // cluster and blob, or a = redirect target
    char path[256], title[256];
} dirent_t;

static int zim_open(zim_t *z, const char *path) {
    memset(z, 0, sizeof *z);
    z->fd = open(path, O_RDONLY);
    if (z->fd < 0) { printf("open %s: %s\n", path, strerror(errno)); return -1; }
    uint8_t h[80];
    if (xpread(z->fd, h, sizeof h, 0) != (int)sizeof h || le32(h) != 0x044D495Au) {
        printf("%s: not a ZIM file\n", path);
        return -1;
    }
    z->major = le16(h + 4);
    z->minor = le16(h + 6);
    z->entries = le32(h + 24);
    z->clusters = le32(h + 28);
    z->path_ptr = le64(h + 32);
    z->title_idx = le64(h + 40);
    z->cluster_ptr = le64(h + 48);
    z->mime_pos = le64(h + 56);
    z->main_page = le32(h + 64);
    z->checksum_pos = le64(h + 72);
    z->size = file_size(z->fd);
    return 0;
}

static void cstr(char *dst, const uint8_t *src, int avail) {
    int i = 0;
    for (; i < avail && i < 255 && src[i]; i++) dst[i] = (char)src[i];
    dst[i] = 0;
}

static int zim_dirent(zim_t *z, uint32_t idx, dirent_t *d) {
    uint8_t p8[8], b[600];
    if (idx >= z->entries || cread(z->fd, p8, 8, z->path_ptr + 8ull * idx) != 8) return -1;
    const int n = cread(z->fd, b, sizeof b, le64(p8));
    if (n < 16) return -1;
    d->mime = le16(b);
    d->ns = (char)b[3];
    int p;
    if (d->mime == 0xFFFF) { d->redirect = 1; d->a = le32(b + 8); d->b = 0; p = 12; }
    else if (d->mime >= 0xFFFD) { d->redirect = 0; d->a = d->b = 0; p = 8; }   // link target / deleted
    else { d->redirect = 0; d->a = le32(b + 8); d->b = le32(b + 12); p = 16; }
    cstr(d->path, b + p, n - p);
    p += (int)strlen(d->path) + 1;
    cstr(d->title, b + p, p < n ? n - p : 0);
    if (!d->title[0]) strcpy(d->title, d->path);
    return 0;
}

static int nscmp(char a, char b) { return ((unsigned char)a > (unsigned char)b) - ((unsigned char)a < (unsigned char)b); }

static int64_t zim_find(zim_t *z, char ns, const char *path, int *probes) {
    uint32_t lo = 0, hi = z->entries;
    dirent_t d;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (zim_dirent(z, mid, &d)) return -2;
        (*probes)++;
        int c = nscmp(d.ns, ns);
        if (!c) c = strcmp(d.path, path);
        if (!c) return mid;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

// Cluster bounds and info byte.
static int cluster_span(zim_t *z, uint32_t c, uint64_t *start, uint64_t *end, int *type, int *ext) {
    uint8_t p[16];
    const int two = c + 1 < z->clusters;
    if (cread(z->fd, p, two ? 16 : 8, z->cluster_ptr + 8ull * c) != (two ? 16 : 8)) return -1;
    *start = le64(p);
    *end = two ? le64(p + 8) : z->checksum_pos;
    uint8_t info;
    if (xpread(z->fd, &info, 1, *start) != 1) return -1;
    *type = info & 0x0F;
    *ext = (info & 0x10) != 0;
    return 0;
}

static ZSTD_DCtx *g_dctx;
static uint8_t *g_in, *g_out;
static size_t g_in_cap, g_out_cap;
static int64_t g_have = -1;   // cluster now in g_out
static size_t g_have_len;

// Loads a zstd cluster into g_out (one-shot decode). Returns its length, -2 for an uncompressed
// cluster (read its blobs in place), -3 for another compression, -1 on error.
static long cluster_load(zim_t *z, uint32_t c, double *t_read, double *t_dec, size_t *clen) {
    *t_read = *t_dec = 0;
    if ((int64_t)c == g_have) return (long)g_have_len;
    uint64_t start, end;
    int type, ext;
    if (cluster_span(z, c, &start, &end, &type, &ext)) return -1;
    if (type <= 1) return -2;
    if (type != 5) return -3;
    const size_t len = (size_t)(end - start);
    *clen = len;
    if (len > g_in_cap) {
        free(g_in);
        g_in_cap = len + 65536;
        g_in = malloc(g_in_cap);
        if (!g_in) { printf("out of memory (%zu)\n", g_in_cap); g_in_cap = 0; return -1; }
    }
    double t0 = now_ms();
    if (xpread(z->fd, g_in, len, start) != (int)len) return -1;
    *t_read = now_ms() - t0;
    t0 = now_ms();
    for (;;) {
        const size_t r = ZSTD_decompressDCtx(g_dctx, g_out, g_out_cap, g_in + 1, len - 1);
        if (!ZSTD_isError(r)) { g_have_len = r; break; }
        if (ZSTD_getErrorCode(r) == ZSTD_error_dstSize_tooSmall && g_out_cap < (8u << 20)) {
            free(g_out);
            g_out_cap *= 2;
            g_out = malloc(g_out_cap);
            printf("  (decode buffer grown to %zu KiB)\n", g_out_cap >> 10);
            if (!g_out) return -1;
            continue;
        }
        printf("zstd: %s\n", ZSTD_getErrorName(r));
        return -1;
    }
    *t_dec = now_ms() - t0;
    g_have = c;
    return (long)g_have_len;
}

// Partial decode: stream into g_out (stable buffer, so zstd keeps no window of its own) and stop
// as soon as blob *blob is complete. Returns bytes decoded, or -1; *t = decode time.
static long cluster_partial(zim_t *z, uint32_t c, uint32_t *blob, double *t) {
    uint64_t start, end;
    int type, ext;
    if (cluster_span(z, c, &start, &end, &type, &ext) || type != 5) return -1;
    const size_t len = (size_t)(end - start);
    if (len > g_in_cap) return -1;   // cluster_load ran first and sized g_in
    if (xpread(z->fd, g_in, len, start) != (int)len) return -1;
    const double t0 = now_ms();
    ZSTD_DCtx_reset(g_dctx, ZSTD_reset_session_only);
    ZSTD_DCtx_setParameter(g_dctx, ZSTD_d_stableOutBuffer, 1);
    ZSTD_outBuffer out = { g_out, g_out_cap, 0 };
    ZSTD_inBuffer in = { g_in + 1, 0, 0 };
    const size_t w = ext ? 8 : 4;
    uint64_t need = 0;
    long ret = -1;
    for (;;) {
        in.size = in.size + 16384 < len - 1 ? in.size + 16384 : len - 1;
        const size_t r = ZSTD_decompressStream(g_dctx, &out, &in);
        if (ZSTD_isError(r)) { printf("zstd stream: %s\n", ZSTD_getErrorName(r)); break; }
        if (!need && out.pos >= w) {
            const uint64_t first = ext ? le64(g_out) : le32(g_out);
            const uint32_t nblobs = (uint32_t)(first / w - 1);
            if (nblobs) *blob %= nblobs;
            if (out.pos >= (*blob + 2) * w)
                need = ext ? le64(g_out + (*blob + 1) * w) : le32(g_out + (*blob + 1) * w);
        }
        if ((need && out.pos >= need) || r == 0) { ret = (long)out.pos; break; }
        if (in.size == len - 1 && in.pos == in.size) break;   // input exhausted, frame unfinished
    }
    *t = now_ms() - t0;
    ZSTD_DCtx_reset(g_dctx, ZSTD_reset_session_only);   // parameters change only between frames
    ZSTD_DCtx_setParameter(g_dctx, ZSTD_d_stableOutBuffer, 0);
    return ret;
}

static int blob_offsets(const uint8_t *data, size_t len, int ext, uint32_t blob, uint64_t *b0, uint64_t *b1) {
    const size_t w = ext ? 8 : 4;
    if ((blob + 2) * w > len) return -1;
    *b0 = ext ? le64(data + blob * w) : le32(data + blob * w);
    *b1 = ext ? le64(data + (blob + 1) * w) : le32(data + (blob + 1) * w);
    return *b0 <= *b1 && *b1 <= len ? 0 : -1;
}

// --------------------------------------------------------------------------------- commands

static int cmd_seek(int argc, char **argv) {
    if (argc < 3) return 2;
    const int n = argc > 3 ? atoi(argv[3]) : 200;
    const size_t sz = argc > 4 ? (size_t)atoi(argv[4]) : 4096;
    const int fd = open(argv[2], O_RDONLY);
    if (fd < 0) { printf("open: %s\n", strerror(errno)); return 1; }
    const uint64_t size = file_size(fd);
    if (size < sz * 2) { printf("file too small\n"); return 1; }
    printf("file %s: %llu bytes; %d random reads of %zu bytes\n", argv[2], (unsigned long long)size, n, sz);
    uint8_t *buf = malloc(sz);
    double *v = malloc(sizeof(double) * n), bsum[10] = {0};
    int bcnt[10] = {0};
    for (int i = 0; i < n; i++) {
        uint64_t off = rnd64() % (size - sz);
        if (sz >= 512) off &= ~(uint64_t)511;
        const double t0 = now_ms();
        if (xpread(fd, buf, sz, off) != (int)sz) { printf("read error at %llu\n", (unsigned long long)off); return 1; }
        v[i] = now_ms() - t0;
        const int b = (int)(off * 10 / size);
        bsum[b] += v[i];
        bcnt[b]++;
    }
    for (int b = 0; b < 10; b++)
        printf("  tenth %d (%4llu MB): %3d reads, mean %7.2f ms\n", b, (unsigned long long)(size / 10 * b >> 20),
               bcnt[b], bcnt[b] ? bsum[b] / bcnt[b] : 0.0);
    stats("random read", v, n);
    // forward-only: the same reads sorted by offset (what a reader gets when it avoids going back)
    uint64_t *offs = malloc(sizeof(uint64_t) * n);
    for (int i = 0; i < n; i++) offs[i] = (rnd64() % (size - sz)) & ~(uint64_t)511;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && offs[j - 1] > offs[j]; j--) { const uint64_t t = offs[j]; offs[j] = offs[j - 1]; offs[j - 1] = t; }
    for (int i = 0; i < n; i++) {
        const double t0 = now_ms();
        xpread(fd, buf, sz, offs[i]);
        v[i] = now_ms() - t0;
    }
    stats("forward-sorted read", v, n);
    close(fd);
    return 0;
}

static int cmd_write(int argc, char **argv) {
    if (argc < 4) return 2;
    const int mb = atoi(argv[3]);
    const int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { printf("open: %s\n", strerror(errno)); return 1; }
    uint8_t *buf = malloc(1 << 20);
    for (int i = 0; i < (1 << 20); i++) buf[i] = (uint8_t)rnd();
    const double t0 = now_ms();
    for (int i = 0; i < mb; i++) {
        if (write(fd, buf, 1 << 20) != (1 << 20)) { printf("write error at %d MB: %s\n", i, strerror(errno)); return 1; }
        if (i % 256 == 255) printf("  %d MB, %.1f MB/s\n", i + 1, (i + 1) / ((now_ms() - t0) / 1000));
    }
    fsync(fd);
    close(fd);
    const double s = (now_ms() - t0) / 1000;
    printf("wrote %d MB in %.1f s = %.2f MB/s\n", mb, s, mb / s);
    return 0;
}

static int cmd_read(int argc, char **argv) {
    if (argc < 3) return 2;
    const int fd = open(argv[2], O_RDONLY);
    if (fd < 0) { printf("open: %s\n", strerror(errno)); return 1; }
    uint64_t want = file_size(fd);
    if (argc > 3 && (uint64_t)atoi(argv[3]) << 20 < want) want = (uint64_t)atoi(argv[3]) << 20;
    uint8_t *buf = malloc(1 << 20);
    const double t0 = now_ms();
    uint64_t got = 0;
    while (got < want) {
        const int r = xpread(fd, buf, 1 << 20, got);
        if (r <= 0) break;
        got += (uint64_t)r;
    }
    const double s = (now_ms() - t0) / 1000;
    printf("read %.1f MB in %.2f s = %.2f MB/s\n", got / 1048576.0, s, got / 1048576.0 / s);
    close(fd);
    return 0;
}

static int cmd_zstd(int argc, char **argv) {
    if (argc < 3) return 2;
    zim_t z;
    if (zim_open(&z, argv[2])) return 1;
    const int n = argc > 3 ? atoi(argv[3]) : 20;
    printf("ZIM %u.%u, %u entries, %u clusters\n", z.major, z.minor, z.entries, z.clusters);
    double *tr = malloc(sizeof(double) * n), *td = malloc(sizeof(double) * n), *mbs = malloc(sizeof(double) * n);
    int k = 0;
    uint64_t in_sum = 0, out_sum = 0;
    for (uint32_t c = 0; c < z.clusters && k < n; c += z.clusters / n ? z.clusters / n : 1) {
        double a, b;
        size_t clen = 0;
        g_have = -1;
        const long r = cluster_load(&z, c, &a, &b, &clen);
        if (r == -2 || r == -3) continue;
        if (r < 0) return 1;
        tr[k] = a;
        td[k] = b;
        mbs[k] = r / 1048576.0 / (b / 1000);
        in_sum += clen;
        out_sum += (uint64_t)r;
        k++;
    }
    printf("clusters decoded %d: compressed %.0f KB, decoded %.0f KB on average\n", k, k ? in_sum / 1024.0 / k : 0,
           k ? out_sum / 1024.0 / k : 0);
    stats("read compressed", tr, k);
    stats("zstd decode", td, k);
    qsort(mbs, k, sizeof *mbs, cmp_d);
    if (k) printf("decode speed median %.1f MB/s\n", mbs[k / 2]);
    // the same clusters again, decoding only up to one random blob of each
    int m = 0;
    double frac = 0;
    for (uint32_t c = 0; c < z.clusters && m < n; c += z.clusters / n ? z.clusters / n : 1) {
        uint64_t s0, e0;
        int type, ext;
        if (cluster_span(&z, c, &s0, &e0, &type, &ext) || type != 5) continue;
        uint32_t blob = rnd();
        double t;
        const long got = cluster_partial(&z, c, &blob, &t);
        if (got < 0) return 1;
        td[m] = t;
        frac += (double)got / (2u << 20);
        m++;
    }
    if (m) printf("partial decode: %.0f%% of a 2 MiB cluster on average\n", 100 * frac / m);
    stats("zstd partial decode", td, m);
    printf("DCtx %zu KB\n", ZSTD_sizeof_DCtx(g_dctx) >> 10);
    return 0;
}

static int cmd_lookup(int argc, char **argv) {
    if (argc < 3) return 2;
    zim_t z;
    if (zim_open(&z, argv[2])) return 1;
    const int n = argc > 3 ? atoi(argv[3]) : 50;
    printf("ZIM %u.%u, %u entries: %d lookups by path\n", z.major, z.minor, z.entries, n);
    dirent_t *q = malloc(sizeof(dirent_t) * n);
    uint32_t *want = malloc(sizeof(uint32_t) * n);
    for (int i = 0; i < n; i++) {
        want[i] = rnd() % z.entries;
        if (zim_dirent(&z, want[i], &q[i])) return 1;
    }
    double *v = malloc(sizeof(double) * n);
    for (int pass = 0; pass < 2; pass++) {
        g_cache = pass;
        cache_reset();
        g_hits = 0;
        const uint64_t r0 = g_reads;
        int probes = 0, bad = 0;
        for (int i = 0; i < n; i++) {
            const double t0 = now_ms();
            const int64_t got = zim_find(&z, q[i].ns, q[i].path, &probes);
            v[i] = now_ms() - t0;
            if (got != want[i]) bad++;
        }
        printf("%s: %.1f probes and %.1f SD reads per lookup, %d wrong%s\n", pass ? "page cache" : "raw",
               probes / (double)n, (g_reads - r0) / (double)n, bad,
               pass ? "" : "");
        if (pass) printf("  cache hits %llu\n", (unsigned long long)g_hits);
        stats(pass ? "lookup (cached pages)" : "lookup (raw)", v, n);
    }
    g_cache = 0;
    return 0;
}

static int find_listing(zim_t *z, uint64_t *off, uint32_t *count) {
    int probes = 0;
    const int64_t i = zim_find(z, 'X', "listing/titleOrdered/v1", &probes);
    dirent_t d;
    if (i < 0 || zim_dirent(z, (uint32_t)i, &d) || d.redirect) return -1;
    uint64_t start, end, b0, b1;
    int type, ext;
    if (cluster_span(z, d.a, &start, &end, &type, &ext) || type > 1) return -1;
    uint8_t p[16];
    const int w = ext ? 8 : 4;
    if (cread(z->fd, p, 2 * w, start + 1 + (uint64_t)w * d.b) != 2 * w) return -1;
    b0 = ext ? le64(p) : le32(p);
    b1 = ext ? le64(p + 8) : le32(p + 4);
    *off = start + 1 + b0;
    *count = (uint32_t)((b1 - b0) / 4);
    return 0;
}

static void title_search(zim_t *z, uint64_t loff, uint32_t cnt, const char *prefix) {
    const double t0 = now_ms();
    const uint64_t r0 = g_reads;
    uint32_t lo = 0, hi = cnt;
    dirent_t d;
    uint8_t p4[4];
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (cread(z->fd, p4, 4, loff + 4ull * mid) != 4 || zim_dirent(z, le32(p4), &d)) return;
        if (strcmp(d.title, prefix) < 0) lo = mid + 1; else hi = mid;
    }
    const size_t pl = strlen(prefix);
    char line[600] = "";
    int shown = 0;
    for (uint32_t i = lo; i < cnt && shown < 8; i++) {
        if (cread(z->fd, p4, 4, loff + 4ull * i) != 4 || zim_dirent(z, le32(p4), &d)) break;
        if (strncmp(d.title, prefix, pl)) break;
        if (strlen(line) + strlen(d.title) + 4 < sizeof line) {
            strcat(line, shown ? " | " : "");
            strcat(line, d.title);
        }
        shown++;
    }
    printf("  '%s': %.1f ms, %llu SD reads -> %s\n", prefix, now_ms() - t0, (unsigned long long)(g_reads - r0),
           shown ? line : "(nothing)");
}

static int cmd_title(int argc, char **argv) {
    if (argc < 4) return 2;
    zim_t z;
    if (zim_open(&z, argv[2])) return 1;
    g_cache = 1;
    cache_reset();
    uint64_t loff;
    uint32_t cnt;
    double t0 = now_ms();
    if (find_listing(&z, &loff, &cnt)) { printf("no X/listing/titleOrdered/v1\n"); return 1; }
    printf("title listing: %u titles, found in %.1f ms\n", cnt, now_ms() - t0);
    for (int a = 3; a < argc; a++) {
        char v[256];
        snprintf(v, sizeof v, "%s", argv[a]);
        title_search(&z, loff, cnt, v);                                  // as typed
        if (v[0] >= 'a' && v[0] <= 'z') { v[0] -= 32; title_search(&z, loff, cnt, v); }   // Capital
    }
    printf("  (second round, pages cached)\n");
    for (int a = 3; a < argc; a++) title_search(&z, loff, cnt, argv[a]);
    g_cache = 0;
    return 0;
}

static int cmd_get(int argc, char **argv) {
    if (argc < 4) return 2;
    double t0 = now_ms();
    zim_t z;
    if (zim_open(&z, argv[2])) return 1;
    printf("open %.1f ms (ZIM %u.%u, %u entries)\n", now_ms() - t0, z.major, z.minor, z.entries);
    g_cache = 1;
    cache_reset();
    for (int a = 3; a < argc; a++) {
        t0 = now_ms();
        int probes = 0;
        int64_t i = zim_find(&z, 'C', argv[a], &probes);
        if (i < 0) i = zim_find(&z, 'A', argv[a], &probes);   // ZIM before 6.1: articles in A/
        if (i < 0) i = zim_find(&z, 'W', argv[a], &probes);   // W/mainPage
        dirent_t d;
        int hops = 0;
        while (i >= 0 && !zim_dirent(&z, (uint32_t)i, &d) && d.redirect && hops < 8) { i = d.a; hops++; }
        const double t_look = now_ms() - t0;
        if (i < 0) { printf("%s: not found (%.1f ms)\n", argv[a], t_look); continue; }
        double t_read = 0, t_dec = 0;
        size_t clen = 0, blen = 0;
        const long r = cluster_load(&z, d.a, &t_read, &t_dec, &clen);
        const uint8_t *blob = NULL;
        uint8_t *raw = NULL;
        uint64_t b0, b1;
        if (r >= 0) {
            uint64_t s, e;
            int type, ext = 0;
            cluster_span(&z, d.a, &s, &e, &type, &ext);
            if (!blob_offsets(g_out, (size_t)r, ext, d.b, &b0, &b1)) { blob = g_out + b0; blen = (size_t)(b1 - b0); }
        } else if (r == -2) {
            uint64_t s, e;
            int type, ext;
            cluster_span(&z, d.a, &s, &e, &type, &ext);
            const int w = ext ? 8 : 4;
            uint8_t p[16];
            const double t1 = now_ms();
            if (xpread(z.fd, p, 2 * w, s + 1 + (uint64_t)w * d.b) == 2 * w) {
                b0 = ext ? le64(p) : le32(p);
                b1 = ext ? le64(p + 8) : le32(p + 4);
                blen = (size_t)(b1 - b0);
                raw = malloc(blen ? blen : 1);
                if (raw && xpread(z.fd, raw, blen, s + 1 + b0) == (int)blen) blob = raw;
            }
            t_read = now_ms() - t1;
        }
        const double total = now_ms() - t0;
        char head[121] = "";
        for (size_t k = 0, o = 0; blob && k < blen && o < 120; k++) {
            const uint8_t ch = blob[k];
            head[o++] = (ch < 32 || ch == 127) ? ' ' : (char)ch;
            head[o] = 0;
        }
        printf("%s -> %s%s: %zu bytes, lookup %.1f ms (%d probes, %d redirects), cluster read %.1f ms (%zu KB), "
               "decode %.1f ms, total %.1f ms\n    %s\n",
               argv[a], d.path, r == -2 ? " [raw cluster]" : (t_read == 0 && t_dec == 0 ? " [cluster cached]" : ""),
               blen, t_look, probes, hops, t_read, clen >> 10, t_dec, total, head);
        free(raw);
    }
    return 0;
}

static int cmd_http(int argc, char **argv) {
    if (argc < 3) return 2;
    const int chunks = argc > 3 ? atoi(argv[3]) : 8;
    const uint32_t kb = argc > 4 ? (uint32_t)atoi(argv[4]) : 1024;
    const uint32_t cap = kb * 1024;
    uint8_t *buf = malloc(cap);
    char spec[700];
    double *v = malloc(sizeof(double) * chunks);
    uint64_t total = 0;
    const double t_all = now_ms();
    int k = 0;
    for (int c = 0; c < chunks; c++) {
        const uint64_t a = (uint64_t)c * cap, b = a + cap - 1;
        snprintf(spec, sizeof spec, "{\"url\":\"%s\",\"headers\":{\"Range\":\"bytes=%llu-%llu\"},\"timeout\":30000,\"max\":%u}",
                 argv[2], (unsigned long long)a, (unsigned long long)b, cap);
        const double t0 = now_ms();
        const int32_t h = nv_http_req(spec, NULL, 0);
        if (h < 0) { printf("chunk %d: http_req error %d\n", c, h); break; }
        int32_t st;
        while ((st = nv_http_state(h)) == 0) usleep(2000);
        const int32_t status = nv_http_status(h);
        uint32_t got = 0;
        int32_t r;
        while (st > 0 && got < cap && (r = nv_http_read(h, buf + got, cap - got)) > 0) got += (uint32_t)r;
        nv_http_close(h);
        v[k] = now_ms() - t0;
        printf("  chunk %d: state %d, HTTP %d, %u bytes, %.0f ms (%.2f MB/s)\n", c, st, status, got, v[k],
               got / 1048576.0 / (v[k] / 1000));
        if (st < 0 || (status != 206 && status != 200)) break;
        total += got;
        k++;
    }
    const double s = (now_ms() - t_all) / 1000;
    printf("total %.1f MB in %.1f s = %.2f MB/s\n", total / 1048576.0, s, total / 1048576.0 / s);
    stats("chunk", v, k);
    return 0;
}

int main(int argc, char **argv) {
    g_pgbuf = malloc((size_t)NPAGES * PAGE);
    g_out_cap = 2u << 20;
    g_out = malloc(g_out_cap);
    g_dctx = ZSTD_createDCtx();
    if (!g_pgbuf || !g_out || !g_dctx) { printf("out of memory\n"); return 1; }
    if (argc > 1 && !strcmp(argv[1], "-l")) { g_seekread = 1; argv++; argc--; printf("(lseek + read)\n"); }
    const char *c = argc > 1 ? argv[1] : "";
    int r = 2;
    if (!strcmp(c, "seek")) r = cmd_seek(argc, argv);
    else if (!strcmp(c, "write")) r = cmd_write(argc, argv);
    else if (!strcmp(c, "read")) r = cmd_read(argc, argv);
    else if (!strcmp(c, "zstd")) r = cmd_zstd(argc, argv);
    else if (!strcmp(c, "lookup")) r = cmd_lookup(argc, argv);
    else if (!strcmp(c, "title")) r = cmd_title(argc, argv);
    else if (!strcmp(c, "get")) r = cmd_get(argc, argv);
    else if (!strcmp(c, "http")) r = cmd_http(argc, argv);
    if (r == 2)
        printf("usage: zimbench seek FILE [N] [SIZE] | write FILE MB | read FILE [MB] | zstd ZIM [N] |\n"
               "       lookup ZIM [N] | title ZIM PREFIX... | get ZIM PATH... | http URL [CHUNKS] [KB]\n");
    return r;
}
