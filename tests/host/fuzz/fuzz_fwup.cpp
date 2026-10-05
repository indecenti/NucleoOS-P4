// libFuzzer target for the safety-copy decoder and the on-flash records: whatever sits in the `assets`
// partition (a torn write, bit rot, a hostile image), recovery must refuse it or decode it within
// bounds - never write past the expected image size, never hang, never crash.
#include "nv_fwup_codec.h"
#include "nv_fwup_policy.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace nv_fwup_policy;
namespace codec = nv_fwup_codec;

namespace {
struct Src { const uint8_t *d; size_t n; };
bool rd(void *u, uint32_t off, void *buf, size_t n) {
    const Src *s = static_cast<Src *>(u);
    if (off > s->n || n > s->n - off) return false;
    memcpy(buf, s->d + off, n);
    return true;
}
struct Sink { size_t total, cap; };
bool wr(void *u, const void *, size_t n) {
    Sink *s = static_cast<Sink *>(u);
    s->total += n;
    if (s->total > s->cap) abort();   // the decoder must stop at raw_size by itself
    return true;
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    // Records: any bytes as a journal / header must validate without reading out of bounds.
    if (n >= sizeof(Journal)) {
        Journal j;
        memcpy(&j, d, sizeof j);
        if (journal_valid(j) && (!memchr(j.res_op, 0, sizeof j.res_op) || !memchr(j.rescue_ver, 0, sizeof j.rescue_ver)))
            abort();
        Journal k = j;
        journal_pick(&j, &k);
    }
    if (n >= sizeof(LkgHeader)) {
        LkgHeader h;
        memcpy(&h, d, sizeof h);
        if (lkg_valid(h, 0x4A0000) && h.stored_size > 0x4A0000 - kLkgDataOffset) abort();
    }
    // Inflate: the first 3 bytes pick the expected raw size (up to 1 MB) and the staging buffer.
    if (n < 4) return 0;
    const uint32_t raw = 1 + ((uint32_t)d[0] << 12 | (uint32_t)d[1] << 4 | (d[2] & 15));
    const size_t in_cap = 1024u << (d[2] >> 6);
    static std::vector<uint8_t> work(codec::inflate_work_size());
    std::vector<uint8_t> in(in_cap);
    Src src{d + 3, n - 3};
    Sink sink{0, raw};
    const codec::Err e = codec::inflate(rd, &src, (uint32_t)(n - 3), wr, &sink, raw, work.data(), in.data(), in.size());
    if (e == codec::Err::Ok && sink.total != raw) abort();
    return 0;
}
