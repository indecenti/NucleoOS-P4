// vp_mp4 — MP4 moov parsing (see vp_mp4.h). No ESP-IDF APIs except the PSRAM allocator, which maps
// to the C library on the PC (tests/host).
//
// Every size and count read here is untrusted, and the device is 32-bit: `p + size` can wrap the
// address space, so bounds are checked as LENGTHS (size <= end - p), never by forming a pointer past
// the buffer and comparing it (the old `p + sz > end` let a box size of 0xFFFFFFF8 wrap, skip the
// clamp and walk find_child backwards off the moov buffer).
#include "vp_mp4.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define vp_malloc(n)   heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define vp_free(p)     heap_caps_free(p)
#else
#include <stdlib.h>
#define vp_malloc(n)   malloc(n)
#define vp_free(p)     free(p)
#endif

static uint16_t be16(const uint8_t *p){ return (uint16_t)((p[0]<<8)|p[1]); }
static const uint8_t *find4(const uint8_t *h, size_t n, const char *t){
    if (n < 4) return NULL;
    for (size_t i=0;i+4<=n;i++) if (h[i]==(uint8_t)t[0]&&h[i+1]==(uint8_t)t[1]&&h[i+2]==(uint8_t)t[2]&&h[i+3]==(uint8_t)t[3]) return h+i;
    return NULL;
}

// Generic ISO-BMFF box reader. `tag` points at the 4-byte fourcc (box start is tag-4); `size` is the
// box's total length (incl. its own 8-byte header); `payload` is tag+4 (right after the fourcc).
typedef struct { const uint8_t *tag; uint32_t size; const uint8_t *payload; } vp_box_t;
typedef struct { const uint8_t *lo, *hi; } vp_bounds_t;   // the whole loaded moov buffer's span

static bool in_bounds(const vp_bounds_t *b, const uint8_t *p, long n){
    return p >= b->lo && p <= b->hi && n >= 0 && (unsigned long)n <= (unsigned long)(b->hi - p);
}

static bool box_at(const uint8_t *p, const uint8_t *end, vp_box_t *out){
    if (p >= end || end - p < 8) return false;
    uint32_t sz = vp_be32(p);
    if (sz == 1 || sz < 8) return false;          // 64-bit extended size not expected in our moov; bail safely
    if (sz > (size_t)(end - p)) sz = (uint32_t)(end - p);   // clamp a truncated/oversized box to what we have
    out->tag = p + 4; out->size = sz; out->payload = p + 8;
    return true;
}
static bool find_child(const uint8_t *start, const uint8_t *end, const char *want, vp_box_t *out){
    const uint8_t *p = start;
    while (p < end) {
        vp_box_t b;
        if (!box_at(p, end, &b)) break;
        if (memcmp(b.tag, want, 4) == 0) { *out = b; return true; }
        p += b.size;                               // b.size <= end - p: never past `end`
    }
    return false;
}

// Combine stsc (samples-per-chunk run-length) + stco/co64 (chunk byte offsets) + stsz (sample sizes)
// into one flat {offset,size} table — replacing the old "single contiguous chunk" assumption that
// only ever matched our own muxer's output, not a normal interleaved MP4 (like an ffmpeg remux).
// Every declared count is clamped to what actually fits in `B` BEFORE it drives an indexed read —
// an attacker-supplied entry_count can't push any access past the moov buffer.
static bool build_sample_table(const vp_bounds_t *B,
                                const uint8_t *stsc_p, uint32_t stsc_n,
                                const uint8_t *stco_p, bool use64, uint32_t stco_n,
                                const uint8_t *stsz_p, vp_sample_t **out_tbl, uint32_t *out_n){
    if (!in_bounds(B, stsz_p, 12)) return false;
    uint32_t fixed_size = vp_be32(stsz_p + 4);
    uint32_t nsamp = vp_be32(stsz_p + 8);
    const uint8_t *sizes = stsz_p + 12;
    if (!fixed_size) {
        uint32_t max_sizes = (uint32_t)((B->hi - sizes) / 4);   // sizes <= hi: checked just above
        if (nsamp > max_sizes) nsamp = max_sizes;
    }
    // 200k samples = 1.6 MB of sample tables (~1.9 h at 30 fps). The old 2M cap let two 4-byte
    // header fields request 16 MB of PSRAM per track and starve LVGL/the reclaim broker.
    if (nsamp == 0 || nsamp > 200000) return false;

    if (!in_bounds(B, stsc_p, 8)) return false;
    { uint32_t max_stsc = (uint32_t)((B->hi - (stsc_p+8)) / 12);
      if (stsc_n > max_stsc) stsc_n = max_stsc; }
    if (stsc_n == 0) return false;

    const long entry_sz = use64 ? 8 : 4;
    if (!in_bounds(B, stco_p, 8)) return false;
    { uint32_t max_stco = (uint32_t)((B->hi - (stco_p+8)) / entry_sz);
      if (stco_n > max_stco) stco_n = max_stco; }
    if (stco_n == 0) return false;

    vp_sample_t *tbl = (vp_sample_t *)vp_malloc((size_t)nsamp * sizeof(vp_sample_t));
    if (!tbl) return false;

    uint32_t si = 0, entry_ix = 0;
    for (uint32_t c = 0; c < stco_n && si < nsamp; c++) {
        while (entry_ix + 1 < stsc_n && vp_be32(stsc_p + 8 + (entry_ix+1)*12) <= c + 1) entry_ix++;
        uint32_t spc = vp_be32(stsc_p + 8 + entry_ix*12 + 4);
        uint64_t off = use64 ? (((uint64_t)vp_be32(stco_p+8+c*8) << 32) | vp_be32(stco_p+8+c*8+4))
                             : vp_be32(stco_p+8+c*4);
        for (uint32_t s = 0; s < spc && si < nsamp; s++) {
            uint32_t szv = fixed_size ? fixed_size : vp_be32(sizes + si*4);
            tbl[si].offset = (uint32_t)off; tbl[si].size = szv;
            off += szv; si++;
        }
    }
    if (si == 0) { vp_free(tbl); return false; }
    *out_tbl = tbl; *out_n = si;
    return true;
}

// Fill V (handler 'vide') or A (handler 'soun', mp4a/AAC only) from one <trak> box's contents.
static void parse_trak(const vp_bounds_t *B, const uint8_t *p, const uint8_t *end, vp_track_t *V, vp_track_t *A){
    vp_box_t mdia;
    if (!find_child(p, end, "mdia", &mdia)) return;
    const uint8_t *mp = mdia.payload, *me = mdia.tag - 4 + mdia.size;   // box end (size clamped by box_at)

    vp_box_t hdlr, mdhd, minf;
    if (!find_child(mp, me, "hdlr", &hdlr)) return;
    if (!find_child(mp, me, "mdhd", &mdhd)) return;
    if (!find_child(mp, me, "minf", &minf)) return;

    if (!in_bounds(B, hdlr.payload, 12)) return;
    const bool is_vide = memcmp(hdlr.payload + 8, "vide", 4) == 0;
    const bool is_soun = memcmp(hdlr.payload + 8, "soun", 4) == 0;
    if (!is_vide && !is_soun) return;
    vp_track_t *T = is_vide ? V : A;
    if (T->present) return;                     // first track of each kind wins (a second one leaked its tables)

    if (!in_bounds(B, mdhd.payload, 1)) return;
    const long ts_off = (mdhd.payload[0] == 1) ? 20 : 12;   // version 1 -> 64-bit create/modify times
    if (!in_bounds(B, mdhd.payload, ts_off + 4)) return;
    uint32_t timescale = vp_be32(mdhd.payload + ts_off);

    vp_box_t stbl;
    const uint8_t *ip = minf.payload, *ie = minf.tag - 4 + minf.size;
    if (!find_child(ip, ie, "stbl", &stbl)) return;
    const uint8_t *sp = stbl.payload, *se = stbl.tag - 4 + stbl.size;

    vp_box_t stsd, stts, stsz, stsc, stco, co64b;
    bool has_stco = find_child(sp, se, "stco", &stco);
    bool has_co64 = !has_stco && find_child(sp, se, "co64", &co64b);
    if (!find_child(sp, se, "stsd", &stsd) || !find_child(sp, se, "stts", &stts) ||
        !find_child(sp, se, "stsz", &stsz) || !find_child(sp, se, "stsc", &stsc) ||
        (!has_stco && !has_co64)) return;

    if (!in_bounds(B, stts.payload, 16)) return;   // bytes 12-15 (the first run's delta) are read below
    uint32_t stts_n = vp_be32(stts.payload + 4);
    uint32_t delta = stts_n ? vp_be32(stts.payload + 8 + 4) : 0;   // first run's delta (CFR assumption)
    uint32_t period_ms = (timescale && delta) ? (uint32_t)((uint64_t)delta * 1000 / timescale) : 66;
    if (period_ms == 0) period_ms = 66;

    if (!in_bounds(B, stsc.payload, 8)) return;
    uint32_t stsc_n = vp_be32(stsc.payload + 4);
    const uint8_t *stco_p = has_stco ? stco.payload : co64b.payload;
    if (!in_bounds(B, stco_p, 8)) return;
    uint32_t stco_n = vp_be32(stco_p + 4);

    vp_sample_t *tbl = NULL; uint32_t nsamp = 0;
    if (!build_sample_table(B, stsc.payload, stsc_n, stco_p, has_co64, stco_n, stsz.payload, &tbl, &nsamp)) return;

    T->present = true; T->nsamp = nsamp; T->tbl = tbl; T->period_ms = period_ms;

    if (!in_bounds(B, stsd.payload, 8)) return;   // no codec info -> track stays "present" but unusable
    const uint8_t *entry = stsd.payload + 8;      // first (only) sample entry
    const uint8_t *stsd_end = stsd.tag - 4 + stsd.size;

    if (is_vide) {
        if (entry <= stsd_end) {
            const uint8_t *av = find4(entry, (size_t)(stsd_end - entry), "avcC");
            if (av && in_bounds(B, av, 12)) {
                uint16_t sps_len = be16(av+10);
                const uint8_t *sps = av + 12;
                if (in_bounds(B, sps, (long)sps_len + 3)) {
                    const uint8_t *pp = sps + sps_len;
                    uint16_t pps_len = be16(pp+1);
                    const uint8_t *pps = pp + 3;
                    if (in_bounds(B, pps, pps_len)) {
                        T->sps = sps; T->sps_len = sps_len;
                        T->pps = pps; T->pps_len = pps_len;
                        // avcC payload[4] low 2 bits = lengthSizeMinusOne (av+4 is payload start,
                        // since av points at the 4-byte "avcC" tag). Most encoders use 4, but some
                        // remux/export tools use 1 or 2 -- assuming 4 unconditionally silently
                        // truncates every NAL and leaves the decoder starved (audio still plays,
                        // since it's an independent track/task -- exactly the "sound but no video"
                        // symptom this fixes).
                        T->nal_len_size = (uint8_t)((av[8] & 0x03) + 1);
                    }
                }
            }
        }
        vp_box_t stssb;
        if (find_child(sp, se, "stss", &stssb) && in_bounds(B, stssb.payload, 8)) {
            uint32_t n = vp_be32(stssb.payload + 4);
            uint32_t max_n = (uint32_t)((B->hi - (stssb.payload+8)) / 4);
            if (n > max_n) n = max_n;
            if (n && n < 200000) {
                uint32_t *sync = (uint32_t *)vp_malloc((size_t)n * 4);
                if (sync) {
                    for (uint32_t k=0;k<n;k++) sync[k] = vp_be32(stssb.payload + 8 + k*4) - 1;   // 1-based -> 0-based
                    T->sync = sync; T->sync_count = n;
                }
            }
        }
    } else if (in_bounds(B, entry, 36) && memcmp(entry + 4, "mp4a", 4) == 0) {   // audio: AAC (mp4a) only
        T->channels = be16(entry + 24);
        T->bits     = be16(entry + 26);
        T->rate     = (int)(vp_be32(entry + 32) >> 16);
    }
}

void vp_mp4_parse_moov(const uint8_t *moov, uint32_t moov_sz, vp_track_t *V, vp_track_t *A){
    if (!moov || moov_sz < 8) return;
    const vp_bounds_t B = { moov, moov + moov_sz };
    const uint8_t *p = moov + 8, *end = moov + moov_sz;   // skip moov's own 8-byte header
    while (p < end) {
        vp_box_t b;
        if (!box_at(p, end, &b)) break;
        if (memcmp(b.tag, "trak", 4) == 0) parse_trak(&B, b.payload, p + b.size, V, A);
        p += b.size;
    }
}

uint32_t vp_mp4_seek_video_index(const vp_track_t *V, int want_ms){
    if (!V->nsamp) return 0;
    uint32_t target = (V->period_ms && want_ms > 0) ? (uint32_t)want_ms / V->period_ms : 0;
    if (target >= V->nsamp) target = V->nsamp - 1;
    if (!V->sync || V->sync_count == 0) return target;   // no stss -> every sample is a keyframe
    // The stss list comes from the file: don't trust it to be sorted or in range. The old loop
    // started from sync[0] and returned it even when it lay past the table (an stss entry of 0 or
    // 0xFFFFFFFF) -> play_mp4 indexed V.tbl out of bounds.
    uint32_t best = 0;
    for (uint32_t k = 0; k < V->sync_count; k++)
        if (V->sync[k] <= target && V->sync[k] > best) best = V->sync[k];
    return best;
}

void vp_mp4_track_free(vp_track_t *T){
    if (T->tbl) vp_free(T->tbl);
    if (T->sync) vp_free(T->sync);
    T->tbl = NULL; T->sync = NULL; T->nsamp = T->sync_count = 0; T->present = false;
}
