// zip.c — the members of a ZIP file (EPUB, CBZ, .fb2.zip): the central directory read once, a
// member read whole (stored, or deflated: inflate.c). The file is untrusted: every size and offset
// is checked against the file and against what was asked for (fuzzed: test/fuzz_book.c).
// Reads use lseek + read: ESP-IDF's pread on FAT32 walks the cluster chain from the start every call.
#include "bk.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

int bk_inflate(const void *src, size_t n, void *out, size_t out_n, int zlib);
int bk_inflate_head(const void *src, size_t n, void *out, size_t out_n);

typedef struct {
    uint32_t name;           // offset in z->names
    uint16_t name_len, method, flags;
    uint32_t csize, usize;
    uint64_t local;          // offset of the local header
} zent_t;

struct zip_s {
    int fd;
    uint64_t size;
    zent_t *e;
    int n;
    char *names;
};

#define ZIP_MAX_ENTRIES 20000
#define ZIP_MAX_CD (8u << 20)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static int read_at(int fd, uint64_t off, void *buf, size_t n) {
    if (off > 0x7FFFFFFFu || lseek(fd, (off_t)off, SEEK_SET) != (off_t)off) return -1;
    size_t got = 0;
    while (got < n) {
        const ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

int zip_open(const char *path, zip_t **out) {
    *out = NULL;
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    zip_t *z = (zip_t *)calloc(1, sizeof *z);
    if (!z) { close(fd); return -1; }
    z->fd = fd;
    const off_t end = lseek(fd, 0, SEEK_END);
    if (end < 22) goto bad;
    z->size = (uint64_t)end;
    // the end of central directory record: in the last 64 KB + 22 bytes (a comment may follow it)
    const size_t tail = z->size < 65557 ? (size_t)z->size : 65557;
    uint8_t *t = (uint8_t *)malloc(tail);
    if (!t || read_at(fd, z->size - tail, t, tail) < 0) { free(t); goto bad; }
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (rd32(t + i) == 0x06054b50u) { eocd = i; break; }
    if (eocd < 0) { free(t); goto bad; }
    uint64_t count = rd16(t + eocd + 10), cd_size = rd32(t + eocd + 12), cd_off = rd32(t + eocd + 16);
    if ((count == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu) && eocd >= 20 &&
        rd32(t + eocd - 20) == 0x07064b50u) {   // ZIP64: the locator points at the ZIP64 record
        uint8_t r[56];
        const uint64_t at = rd64(t + eocd - 20 + 8);
        if (read_at(fd, at, r, sizeof r) < 0 || rd32(r) != 0x06064b50u) { free(t); goto bad; }
        count = rd64(r + 32);
        cd_size = rd64(r + 40);
        cd_off = rd64(r + 48);
    }
    free(t);
    if (!count || count > ZIP_MAX_ENTRIES || cd_size > ZIP_MAX_CD || cd_off + cd_size > z->size) goto bad;
    uint8_t *cd = (uint8_t *)malloc((size_t)cd_size);
    z->e = (zent_t *)calloc((size_t)count, sizeof(zent_t));
    z->names = (char *)malloc((size_t)cd_size + 1);   // the names fit in the directory that holds them
    if (!cd || !z->e || !z->names || read_at(fd, cd_off, cd, (size_t)cd_size) < 0) { free(cd); goto bad; }
    size_t p = 0, np = 0;
    for (uint64_t k = 0; k < count && p + 46 <= cd_size; k++) {
        const uint8_t *h = cd + p;
        if (rd32(h) != 0x02014b50u) break;
        const unsigned nl = rd16(h + 28), xl = rd16(h + 30), cl = rd16(h + 32);
        if (p + 46 + nl + xl + cl > cd_size) break;
        zent_t *e = &z->e[z->n];
        e->flags = rd16(h + 8);
        e->method = rd16(h + 10);
        e->csize = rd32(h + 20);
        e->usize = rd32(h + 24);
        e->local = rd32(h + 42);
        // ZIP64 extra field: the 64-bit values of the fields that say 0xFFFFFFFF, in this order
        const uint8_t *x = h + 46 + nl, *xe = x + xl;
        uint64_t us = e->usize, cs = e->csize, lo = e->local;
        while (x + 4 <= xe) {
            const unsigned id = rd16(x), sz = rd16(x + 2);
            if (x + 4 + sz > xe) break;
            if (id == 1) {
                const uint8_t *v = x + 4, *ve = x + 4 + sz;
                if (e->usize == 0xFFFFFFFFu && v + 8 <= ve) { us = rd64(v); v += 8; }
                if (e->csize == 0xFFFFFFFFu && v + 8 <= ve) { cs = rd64(v); v += 8; }
                if (e->local == 0xFFFFFFFFu && v + 8 <= ve) { lo = rd64(v); }
            }
            x += 4 + sz;
        }
        p += 46 + nl + xl + cl;
        if (!nl || us > 0x7FFFFFFFu || cs > 0x7FFFFFFFu || lo >= z->size) continue;   // too big for us: skipped
        e->usize = (uint32_t)us;
        e->csize = (uint32_t)cs;
        e->local = lo;
        memcpy(z->names + np, h + 46, nl);
        for (unsigned i = 0; i < nl; i++)
            if (!z->names[np + i]) z->names[np + i] = '_';   // a NUL inside a name would cut it short
        e->name = (uint32_t)np;
        e->name_len = (uint16_t)nl;
        np += nl;
        z->names[np++] = 0;
        z->n++;
    }
    free(cd);
    if (!z->n) goto bad;
    *out = z;
    return 0;
bad:
    zip_close(z);
    return -1;
}

void zip_close(zip_t *z) {
    if (!z) return;
    if (z->fd >= 0) close(z->fd);
    free(z->e);
    free(z->names);
    free(z);
}

int zip_count(const zip_t *z) { return z->n; }
const char *zip_name(const zip_t *z, int i) { return i >= 0 && i < z->n ? z->names + z->e[i].name : ""; }
uint32_t zip_size(const zip_t *z, int i) { return i >= 0 && i < z->n ? z->e[i].usize : 0; }

int zip_find(const zip_t *z, const char *name) {
    for (int i = 0; i < z->n; i++)
        if (!strcmp(z->names + z->e[i].name, name)) return i;
    for (int i = 0; i < z->n; i++)
        if (!strcasecmp(z->names + z->e[i].name, name)) return i;
    return -1;
}

int zip_read(zip_t *z, int i, uint8_t **data, size_t *n, size_t max) {
    *data = NULL;
    *n = 0;
    if (i < 0 || i >= z->n) return BKE_FORMAT;
    const zent_t *e = &z->e[i];
    if (e->flags & 1) return BKE_DRM;                 // encrypted
    if (e->usize > max) return BKE_BIG;
    if (e->method != 0 && e->method != 8) return BKE_FORMAT;
    uint8_t lh[30];
    if (read_at(z->fd, e->local, lh, sizeof lh) < 0 || rd32(lh) != 0x04034b50u) return BKE_FORMAT;
    const uint64_t at = e->local + 30 + rd16(lh + 26) + rd16(lh + 28);
    if (at + e->csize > z->size) return BKE_FORMAT;
    uint8_t *out = (uint8_t *)malloc((size_t)e->usize + 1);
    if (!out) return BKE_MEM;
    if (e->method == 0) {
        if (e->csize != e->usize || read_at(z->fd, at, out, e->usize) < 0) { free(out); return BKE_FORMAT; }
    } else {
        uint8_t *c = (uint8_t *)malloc(e->csize ? e->csize : 1);
        if (!c) { free(out); return BKE_MEM; }
        const int r = read_at(z->fd, at, c, e->csize) < 0 ? -1 : bk_inflate(c, e->csize, out, e->usize, 0);
        free(c);
        if (r != (int)e->usize) { free(out); return BKE_FORMAT; }
    }
    out[e->usize] = 0;
    *data = out;
    *n = e->usize;
    return BKE_OK;
}

int zip_read_head(zip_t *z, int i, uint8_t **data, size_t *n, size_t want) {
    *data = NULL;
    *n = 0;
    if (i < 0 || i >= z->n) return BKE_FORMAT;
    const zent_t *e = &z->e[i];
    if (e->flags & 1) return BKE_DRM;
    if (e->method != 0 && e->method != 8) return BKE_FORMAT;
    if (want > e->usize) want = e->usize;
    uint8_t lh[30];
    if (read_at(z->fd, e->local, lh, sizeof lh) < 0 || rd32(lh) != 0x04034b50u) return BKE_FORMAT;
    const uint64_t at = e->local + 30 + rd16(lh + 26) + rd16(lh + 28);
    if (at + e->csize > z->size) return BKE_FORMAT;
    uint8_t *out = (uint8_t *)malloc(want + 1);
    if (!out) return BKE_MEM;
    int got;
    if (e->method == 0) {
        got = read_at(z->fd, at, out, want) < 0 ? -1 : (int)want;
    } else {
        const size_t cn = e->csize < want + 4096 ? e->csize : want + 4096;   // a picture's header does not compress
        uint8_t *c = (uint8_t *)malloc(cn ? cn : 1);
        if (!c) { free(out); return BKE_MEM; }
        got = read_at(z->fd, at, c, cn) < 0 ? -1 : bk_inflate_head(c, cn, out, want);
        free(c);
    }
    if (got < 0) { free(out); return BKE_FORMAT; }
    out[got] = 0;
    *data = out;
    *n = (size_t)got;
    return BKE_OK;
}
