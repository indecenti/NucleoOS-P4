// vp_avi — AVI header + index parsing (see vp_avi.h). No ESP-IDF APIs except the PSRAM allocator,
// which maps to the C library on the PC (tests/host).
#include "vp_avi.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define VP_CAPS                   (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define vp_malloc(n)              heap_caps_malloc((n), VP_CAPS)
#define vp_realloc(p, n)          heap_caps_realloc((p), (n), VP_CAPS)
#define vp_aligned_calloc(a, n)   heap_caps_aligned_calloc((a), 1, (n), VP_CAPS)
#define vp_free(p)                heap_caps_free(p)
#else
#include <stdlib.h>
#define vp_malloc(n)              malloc(n)
#define vp_realloc(p, n)          realloc((p), (n))
#define vp_free(p)                free(p)
static void *vp_aligned_calloc(size_t a, size_t n){
    void *p = NULL;
    if (posix_memalign(&p, a, n ? n : 1) != 0) return NULL;
    memset(p, 0, n);
    return p;
}
#endif

void vp_avi_free(void *p){ vp_free(p); }

// ---------------------------------------------------------------- aligned sector reads
size_t vp_rd_at(FILE *f, long off, void *dst, size_t n){
    const int fd = fileno(f);
    if (fd < 0 || lseek(fd, (off_t)off, SEEK_SET) != (off_t)off) return 0;
    size_t got = 0;
    while (got < n) {
        const ssize_t r = read(fd, (uint8_t *)dst + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    return got;
}
long vp_file_size(FILE *f){
    struct stat st;
    return (fstat(fileno(f), &st) == 0) ? (long)st.st_size : 0;
}

const uint8_t *vp_read_span(FILE *f, uint8_t *buf, size_t cap, uint32_t off, uint32_t len){
    const uint32_t a0 = off & ~(uint32_t)(VP_SECT - 1);
    const uint64_t a1 = ((uint64_t)off + len + VP_SECT - 1) & ~(uint64_t)(VP_SECT - 1);
    const uint64_t n = a1 - a0;
    if (!len || n > cap) return NULL;
    const size_t got = vp_rd_at(f, (long)a0, buf, (size_t)n);   // the last sector may run past EOF: short is fine
    if (got < (size_t)(off - a0) + len) return NULL;
    return buf + (off - a0);
}

// ---------------------------------------------------------------- AVI: header
// Every chunk stride is bounded by its parent and the file size, so a hostile size can neither
// wrap the `long` arithmetic nor park the scan on one spot (see vp_avi_index_scan).
static uint32_t le32(const uint8_t *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t le16(const uint8_t *p){ return (uint16_t)(p[0]|(p[1]<<8)); }

// One stream list ('strl'): strh + strf.
static void avi_parse_strl(FILE *f, long p, long end, vp_avi_t *A){
    const int sn = A->nstreams++;
    uint32_t type = 0, handler = 0, scale = 0, rate = 0;
    long indx = 0; uint32_t indx_sz = 0;
    while (p + 8 <= end) {
        uint8_t h[48] = {0};
        const size_t want = 8 + (sizeof h - 8);
        if (vp_rd_at(f, p, h, want) < 8) return;
        const uint32_t sz = le32(h + 4);
        const int64_t nx = (int64_t)p + 8 + sz + (sz & 1);
        if (nx <= p || nx > end) return;
        uint8_t b[40] = {0};
        memcpy(b, h + 8, sz < sizeof b ? sz : sizeof b);
        if (!memcmp(h, "strh", 4) && sz >= 28) {
            type = le32(b); handler = le32(b + 4); scale = le32(b + 20); rate = le32(b + 24);
        } else if (!memcmp(h, "strf", 4)) {
            if (type == VP_FCC('v','i','d','s') && A->v_stream < 0 && sz >= 20) {
                A->v_stream = sn; A->v_scale = scale; A->v_rate = rate;
                A->v_fcc = le32(b + 16) ? le32(b + 16) : handler;
                if (!A->vw) { A->vw = le32(b + 4); A->vh = le32(b + 8); }
            } else if (type == VP_FCC('a','u','d','s') && A->a_stream < 0 && sz >= 16) {
                A->a_stream = sn;
                A->a_tag = le16(b); A->a_ch = le16(b + 2); A->a_rate = le32(b + 4);
                A->a_bps = le32(b + 8); A->a_align = le16(b + 12); A->a_bits = le16(b + 14);
            }
        } else if (!memcmp(h, "indx", 4)) {
            indx = p + 8; indx_sz = sz;
        }
        p = (long)nx;
    }
    // 'indx' may come after 'strf': attach it once the stream's role is known
    if (indx && A->v_stream == sn) { A->v_indx = indx; A->v_indx_sz = indx_sz; }
    if (indx && A->a_stream == sn) { A->a_indx = indx; A->a_indx_sz = indx_sz; }
}

bool vp_avi_probe(FILE *f, vp_avi_t *A){
    memset(A, 0, sizeof *A);
    A->v_stream = A->a_stream = -1;
    uint8_t hdr[12];
    A->fsz = vp_file_size(f);
    if (vp_rd_at(f, 0, hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "AVI ", 4)) return false;
    long p = 12;
    while (p + 12 <= A->fsz) {
        uint8_t h[12];
        if (vp_rd_at(f, p, h, 12) != 12) break;
        const uint32_t sz = le32(h + 4);
        if (!memcmp(h, "LIST", 4) && !memcmp(h + 8, "movi", 4)) {
            A->movi_pos = p + 12;
            // A recording cut off before its stop (power loss, card pulled) still has the size 0 the
            // recorder writes up front: play to EOF.
            const int64_t e = (int64_t)p + 8 + sz;
            A->movi_end = (sz < 4 || e > A->fsz) ? A->fsz : (long)e;
            return A->v_stream >= 0;
        }
        const int64_t nx = (int64_t)p + 8 + sz + (sz & 1);
        if (nx <= p || nx > A->fsz) break;
        if (!memcmp(h, "LIST", 4) && !memcmp(h + 8, "hdrl", 4)) {
            long q = p + 12;
            while (q + 8 <= nx) {
                uint8_t c[12];
                if (vp_rd_at(f, q, c, 12) < 8) break;
                const uint32_t csz = le32(c + 4);
                const int64_t cn = (int64_t)q + 8 + csz + (csz & 1);
                if (cn <= q || cn > nx) break;
                if (!memcmp(c, "avih", 4) && csz >= 40) {
                    uint8_t a[40];
                    if (vp_rd_at(f, q + 8, a, 40) == 40) {
                        A->uspf = le32(a); A->total = le32(a + 16);
                        A->vw = le32(a + 32); A->vh = le32(a + 36);
                    }
                } else if (!memcmp(c, "LIST", 4) && !memcmp(c + 8, "strl", 4)) {
                    avi_parse_strl(f, q + 12, (long)cn, A);
                }
                q = (long)cn;
            }
        }
        p = (long)nx;
    }
    return false;
}

// ---------------------------------------------------------------- AVI: index
// "00dc"/"00db" (video) and "01wb" (audio) chunk ids for a stream number.
static bool is_ck(const uint8_t *id, int stream, char t0, char t1a, char t1b){
    if (stream < 0 || stream > 99) return false;
    return id[0] == '0' + stream / 10 && id[1] == '0' + stream % 10 && id[2] == t0 && (id[3] == t1a || id[3] == t1b);
}

// OpenDML (AVI 2.0, what ffmpeg writes past 1 GB): the stream's 'indx' super index lists 'ix##'
// standard indexes spread over the RIFF-AVI and RIFF-AVIX parts; each gives a 64-bit base plus
// 32-bit offsets to chunk PAYLOADS. idx1 only covers the first ~1 GB part, so this is preferred.
// All counts and offsets are bounded by the file size (a FAT32 file stays below 4 GB).
bool vp_avi_index_odml(FILE *f, const vp_avi_t *A, long indx, uint32_t indx_sz, vp_ent_t **out, uint32_t *out_n){
    uint8_t h[24];
    if (!indx || indx_sz < 24 || vp_rd_at(f, indx, h, 24) != 24) return false;
    if (le16(h) != 4 || h[3] != 0) return false;                 // wLongsPerEntry 4, AVI_INDEX_OF_INDEXES
    uint32_t nsup = le32(h + 4);
    if (nsup > (indx_sz - 24) / 16) nsup = (indx_sz - 24) / 16;
    if (nsup == 0 || nsup > 4096) return false;
    uint32_t cap = 0, n = 0;
    vp_ent_t *tbl = NULL;
    uint8_t *buf = NULL; size_t bcap = 0;
    bool ok = true;
    for (uint32_t s = 0; s < nsup && ok; s++) {
        uint8_t e[16];
        if (vp_rd_at(f, indx + 24 + (long)s * 16, e, 16) != 16) { ok = false; break; }
        const uint64_t ixo = (uint64_t)le32(e) | ((uint64_t)le32(e + 4) << 32);
        if (ixo + 32 > (uint64_t)A->fsz) { ok = false; break; }
        uint8_t ih[32];
        if (vp_rd_at(f, (long)ixo, ih, 32) != 32 || memcmp(ih, "ix", 2) != 0) { ok = false; break; }
        const uint32_t isz = le32(ih + 4);
        if (le16(ih + 8) != 2 || ih[11] != 1) { ok = false; break; }   // 2 longs/entry, AVI_INDEX_OF_CHUNKS
        uint32_t cnt = le32(ih + 12);
        if (isz < 24 || cnt > (isz - 24) / 8) cnt = isz >= 24 ? (isz - 24) / 8 : 0;
        const uint64_t base = (uint64_t)le32(ih + 20) | ((uint64_t)le32(ih + 24) << 32);
        if (!cnt) continue;
        if (n + cnt > 200000) { ok = false; break; }
        if (n + cnt > cap) {
            uint32_t nc = cap ? cap : 4096;
            while (nc < n + cnt) nc *= 2;
            vp_ent_t *g = (vp_ent_t *)vp_realloc(tbl, (size_t)nc * sizeof(vp_ent_t));
            if (!g) { ok = false; break; }
            tbl = g; cap = nc;
        }
        const size_t need = (size_t)cnt * 8 + 2 * VP_SECT;
        if (need > bcap) {
            if (buf) vp_free(buf);
            buf = (uint8_t *)vp_aligned_calloc(VP_ALIGN, need);
            bcap = buf ? need : 0;
            if (!buf) { ok = false; break; }
        }
        const uint8_t *raw = vp_read_span(f, buf, bcap, (uint32_t)(ixo + 32), cnt * 8);
        if (!raw) { ok = false; break; }
        for (uint32_t k = 0; k < cnt; k++) {
            const uint64_t off = base + le32(raw + k * 8);
            uint32_t size = le32(raw + k * 8 + 4) & 0x7FFFFFFFu;     // bit 31 = not a keyframe
            if (off >= (uint64_t)A->fsz) size = 0;
            else if (off + size > (uint64_t)A->fsz) size = (uint32_t)((uint64_t)A->fsz - off);
            tbl[n].off = (uint32_t)off; tbl[n].size = size; n++;
        }
    }
    if (buf) vp_free(buf);
    if (!ok || !n) { if (tbl) vp_free(tbl); return false; }
    *out = tbl; *out_n = n;
    return true;
}

// idx1 -> separate video / audio tables. The entry offset base is ambiguous across muxers (the
// 'movi' fourcc, the byte after it, or the file start): take the one whose first entry lands on a
// chunk carrying that entry's own id.
bool vp_avi_index_idx1(FILE *f, const vp_avi_t *A, vp_ent_t **V, uint32_t *vn, vp_ent_t **Au, uint32_t *an){
    long p = A->movi_end + (A->movi_end & 1);
    for (int guard = 0; guard < 8 && p + 8 <= A->fsz; guard++) {   // idx1 is normally right after movi
        uint8_t h[8];
        if (vp_rd_at(f, p, h, 8) != 8) return false;
        const uint32_t sz = le32(h + 4);
        if (memcmp(h, "idx1", 4) != 0) {
            const int64_t nx = (int64_t)p + 8 + sz + (sz & 1);
            if (nx <= p || nx > A->fsz) return false;
            p = (long)nx;
            continue;
        }
        uint32_t n = sz / 16;
        if ((int64_t)p + 8 + (int64_t)n * 16 > A->fsz) n = (uint32_t)((A->fsz - p - 8) / 16);
        if (n == 0 || n > 400000) return false;          // memory guard on an attacker-set size
        // sector-aligned bulk read (a 1 h clip's idx1 is ~2 MB: unaligned it went 512 B at a time)
        const size_t rcap = (size_t)n * 16 + 2 * VP_SECT;
        uint8_t *rbuf = (uint8_t *)vp_aligned_calloc(VP_ALIGN, rcap);
        if (!rbuf) return false;
        const uint8_t *raw = vp_read_span(f, rbuf, rcap, (uint32_t)(p + 8), n * 16);
        if (!raw) { vp_free(rbuf); return false; }
        uint32_t nv = 0, na = 0;
        for (uint32_t k = 0; k < n; k++) {
            if (is_ck(raw + k*16, A->v_stream, 'd', 'c', 'b')) nv++;
            else if (is_ck(raw + k*16, A->a_stream, 'w', 'b', 'b')) na++;
        }
        vp_ent_t *va = nv ? (vp_ent_t *)vp_malloc((size_t)nv * sizeof(vp_ent_t)) : NULL;
        vp_ent_t *aa = na ? (vp_ent_t *)vp_malloc((size_t)na * sizeof(vp_ent_t)) : NULL;
        if (!va || (na && !aa)) { vp_free(rbuf); if (va) vp_free(va); if (aa) vp_free(aa); return false; }
        // base probe on the first entry of either stream
        long base = -1;
        for (uint32_t k = 0; k < n && base < 0; k++) {
            const uint8_t *e = raw + k*16;
            if (!is_ck(e, A->v_stream, 'd', 'c', 'b') && !is_ck(e, A->a_stream, 'w', 'b', 'b')) continue;
            const long cand[3] = { A->movi_pos - 4, A->movi_pos, 0 };
            for (int c = 0; c < 3; c++) {
                uint8_t id[4];
                const int64_t at = (int64_t)cand[c] + le32(e + 8);
                if (at < 0 || at + 8 > A->fsz) continue;
                if (vp_rd_at(f, (long)at, id, 4) == 4 && !memcmp(id, e, 4)) { base = cand[c]; break; }
            }
            break;
        }
        if (base < 0) { vp_free(rbuf); vp_free(va); if (aa) vp_free(aa); return false; }
        uint32_t iv = 0, ia = 0;
        for (uint32_t k = 0; k < n; k++) {
            const uint8_t *e = raw + k*16;
            const int64_t off = (int64_t)base + le32(e + 8) + 8;      // payload, past the chunk header
            uint32_t size = le32(e + 12);
            if (off < 0 || off > A->fsz) size = 0;                   // hostile offset: unreadable entry
            else if (off + size > A->fsz) size = (uint32_t)(A->fsz - off);
            if (is_ck(e, A->v_stream, 'd', 'c', 'b'))      { va[iv].off = (uint32_t)off; va[iv].size = size; iv++; }
            else if (is_ck(e, A->a_stream, 'w', 'b', 'b')) { aa[ia].off = (uint32_t)off; aa[ia].size = size; ia++; }
        }
        vp_free(rbuf);
        *V = va; *vn = iv; *Au = aa; *an = ia;
        return iv > 0;
    }
    return false;
}

// No idx1 (a recording cut off before its stop): walk the movi chunk headers. One header read per
// chunk; `keep_going` is polled every 256 chunks so a long unindexed file can't wedge the player.
bool vp_avi_index_scan(FILE *f, const vp_avi_t *A, vp_ent_t **V, uint32_t *vn, bool (*keep_going)(void)){
    uint32_t cap = 4096, n = 0;
    vp_ent_t *va = (vp_ent_t *)vp_malloc(cap * sizeof(vp_ent_t));
    if (!va) return false;
    long p = A->movi_pos;
    while (p + 8 <= A->movi_end) {
        uint8_t h[12];
        if (vp_rd_at(f, p, h, 8) != 8) break;
        const uint32_t sz = le32(h + 4);
        if (!memcmp(h, "LIST", 4)) { p += 12; continue; }        // 'rec ' groups: step inside
        const int64_t nx = (int64_t)p + 8 + sz + (sz & 1);
        if (nx <= p || nx > A->movi_end + 1) break;               // torn tail of an unfinished file
        if (is_ck(h, A->v_stream, 'd', 'c', 'b')) {
            if (n == cap) {
                if (cap >= 200000) break;
                vp_ent_t *g = (vp_ent_t *)vp_realloc(va, (size_t)cap * 2 * sizeof(vp_ent_t));
                if (!g) break;
                va = g; cap *= 2;
            }
            va[n].off = (uint32_t)(p + 8); va[n].size = sz; n++;
        }
        p = (long)nx;
        if ((n & 255) == 0 && keep_going && !keep_going()) break;   // user moved on
    }
    if (!n) { vp_free(va); return false; }
    *V = va; *vn = n;
    return true;
}
