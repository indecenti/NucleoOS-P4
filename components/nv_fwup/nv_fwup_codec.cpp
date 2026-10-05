// nv_fwup_codec — see nv_fwup_codec.h.
#include "nv_fwup_codec.h"

// miniz otherwise #defines deflate/inflate to its zlib-style names (the ROM header already opts out)
#ifndef MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#endif
#include "miniz.h"   // ESP-IDF: esp_rom's header for the ROM tdefl/tinfl; host: vendored miniz

#include <cstring>

namespace nv_fwup_codec {

namespace {
struct InflateWork {
    tinfl_decompressor r;
    uint8_t dict[kInflateDict];
};
}  // namespace

const char *err_str(Err e) {
    switch (e) {
        case Err::Ok:      return "ok";
        case Err::Param:   return "bad parameters";
        case Err::Read:    return "read error";
        case Err::Write:   return "write error";
        case Err::Corrupt: return "corrupt data";
        case Err::Size:    return "wrong size";
    }
    return "?";
}

size_t deflate_work_size(void) { return sizeof(tdefl_compressor); }
size_t inflate_work_size(void) { return sizeof(InflateWork); }

Err deflate(ReadFn rd, void *ru, uint32_t raw_size, WriteFn wr, void *wu, void *work,
            uint8_t *in_buf, size_t in_cap, uint8_t *out_buf, size_t out_cap, int probes,
            uint32_t *stored_out) {
    if (!rd || !wr || !work || !in_buf || !out_buf || in_cap < 1024 || out_cap < 1024 || !raw_size)
        return Err::Param;
    if (probes < 1) probes = 1;
    if (probes > 128) probes = 128;
    tdefl_compressor *d = static_cast<tdefl_compressor *>(work);
    // Raw deflate (no zlib header): the image is hashed separately, adler32 would add nothing.
    const int flags = probes | (probes <= 16 ? TDEFL_GREEDY_PARSING_FLAG : 0);
    if (tdefl_init(d, nullptr, nullptr, flags) != TDEFL_STATUS_OKAY) return Err::Param;
    uint32_t in_off = 0, stored = 0;
    size_t in_have = 0, in_pos = 0;
    for (;;) {
        if (in_pos == in_have && in_off < raw_size) {
            const uint32_t left = raw_size - in_off;
            in_have = left < in_cap ? left : in_cap;
            if (!rd(ru, in_off, in_buf, in_have)) return Err::Read;
            in_off += (uint32_t)in_have;
            in_pos = 0;
        }
        const bool last = in_off >= raw_size;
        size_t in_n = in_have - in_pos, out_n = out_cap;
        const tdefl_status st = tdefl_compress(d, in_buf + in_pos, &in_n, out_buf, &out_n,
                                               last ? TDEFL_FINISH : TDEFL_NO_FLUSH);
        in_pos += in_n;
        if (out_n) {
            if (!wr(wu, out_buf, out_n)) return Err::Write;
            stored += (uint32_t)out_n;
        }
        if (st == TDEFL_STATUS_DONE) break;
        if (st != TDEFL_STATUS_OKAY) return Err::Corrupt;
    }
    if (stored_out) *stored_out = stored;
    return Err::Ok;
}

Err inflate(ReadFn rd, void *ru, uint32_t stored_size, WriteFn wr, void *wu, uint32_t raw_size,
            void *work, uint8_t *in_buf, size_t in_cap) {
    if (!rd || !wr || !work || !in_buf || in_cap < 1024 || !stored_size || !raw_size) return Err::Param;
    InflateWork *w = static_cast<InflateWork *>(work);
    tinfl_init(&w->r);
    uint32_t in_off = 0, out_total = 0;
    size_t in_have = 0, in_pos = 0, dict_ofs = 0;
    for (;;) {
        if (in_pos == in_have && in_off < stored_size) {
            const uint32_t left = stored_size - in_off;
            in_have = left < in_cap ? left : in_cap;
            if (!rd(ru, in_off, in_buf, in_have)) return Err::Read;
            in_off += (uint32_t)in_have;
            in_pos = 0;
        }
        const bool more = in_off < stored_size;
        size_t in_n = in_have - in_pos, out_n = kInflateDict - dict_ofs;
        const tinfl_status st = tinfl_decompress(&w->r, in_buf + in_pos, &in_n, w->dict, w->dict + dict_ofs,
                                                 &out_n, more ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        in_pos += in_n;
        if (out_n) {
            if (out_n > raw_size - out_total) return Err::Size;   // never write past the image
            if (!wr(wu, w->dict + dict_ofs, out_n)) return Err::Write;
            out_total += (uint32_t)out_n;
            dict_ofs = (dict_ofs + out_n) & (kInflateDict - 1);
        }
        if (st == TINFL_STATUS_DONE) break;
        if (st < 0) return Err::Corrupt;
        // Needs input we don't have: the stream is truncated.
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && !more && in_pos == in_have) return Err::Corrupt;
        // No progress at all would loop forever on a malformed stream.
        if (!in_n && !out_n && in_pos == in_have && !more) return Err::Corrupt;
    }
    if (out_total != raw_size) return Err::Size;
    if (in_pos != in_have || in_off != stored_size) return Err::Corrupt;   // trailing garbage
    return Err::Ok;
}

}  // namespace nv_fwup_codec
