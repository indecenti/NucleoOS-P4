// vp_avi: AVI header + index parsing, on well-formed and hostile files.
#include "check.h"
#include "memfile.h"
#include "riff.h"
#include "vp_avi.h"

#include <cstdlib>

static bool probe(MemFile &m, const Bytes &file, vp_avi_t *A) {
    m.set(file.data(), file.size());
    return vp_avi_probe(m.f, A);
}

int main() {
    MemFile m;
    CHECK(m.f != nullptr);
    vp_avi_t A;

    // --- well-formed MJPEG + PCM with idx1
    AviLayout L;
    Bytes good = make_avi(5, 300, true, &L);
    CHECK(probe(m, good, &A));
    CHECK(A.v_stream == 0 && A.a_stream == 1);
    CHECK(A.vw == 64 && A.vh == 48 && A.v_fcc == VP_FCC('M','J','P','G'));
    CHECK(A.v_rate == 10 && A.v_scale == 1 && A.a_rate == 8000 && A.a_ch == 1 && A.a_bits == 16);
    CHECK(A.movi_pos == (long)L.movi_fourcc + 4);
    vp_ent_t *V = nullptr, *Au = nullptr; uint32_t vn = 0, an = 0;
    CHECK(vp_avi_index_idx1(m.f, &A, &V, &vn, &Au, &an));
    CHECK(vn == 5 && an == 5);
    for (uint32_t i = 0; V && i < vn; i++) CHECK(V[i].off == L.vpay[i] && V[i].size == 300);
    for (uint32_t i = 0; Au && i < an; i++) CHECK(Au[i].off == L.apay[i] && Au[i].size == 160);
    vp_avi_free(V); vp_avi_free(Au);

    // --- no idx1 (a recording cut off before its stop): the movi scan finds the frames
    Bytes cut = make_avi(4, 200, false, &L);
    CHECK(probe(m, cut, &A));
    V = nullptr; vn = 0;
    CHECK(!vp_avi_index_idx1(m.f, &A, &V, &vn, &Au, &an));
    CHECK(vp_avi_index_scan(m.f, &A, &V, &vn, nullptr));
    CHECK(vn == 4);
    for (uint32_t i = 0; V && i < vn; i++) CHECK(V[i].off == L.vpay[i] && V[i].size == 200);
    vp_avi_free(V);

    // --- truncated mid-chunk: scan stops at the torn tail instead of trusting its size
    Bytes torn(cut.begin(), cut.end() - 150);
    CHECK(probe(m, torn, &A));
    V = nullptr; vn = 0;
    if (vp_avi_index_scan(m.f, &A, &V, &vn, nullptr)) {
        CHECK(vn <= 4);
        for (uint32_t i = 0; i < vn; i++) CHECK((long)V[i].off + (long)V[i].size <= (long)torn.size() + 1);
        vp_avi_free(V);
    }

    // --- hostile sizes: a chunk claiming 4 GB, a LIST of size 0, an idx1 whose entries point past EOF
    Bytes huge; putid(huge, "RIFF"); put32(huge, 100); putid(huge, "AVI ");
    cat(huge, chunk("JUNK", Bytes(8, 0), 0xFFFFFFF0u));
    CHECK(!probe(m, huge, &A));
    Bytes zero; putid(zero, "RIFF"); put32(zero, 100); putid(zero, "AVI "); cat(zero, list("hdrl", Bytes(), 0));
    CHECK(!probe(m, zero, &A));
    Bytes lie = make_avi(3, 100, true, &L);
    // idx1 entries sit at the tail: poison each entry's offset (entry+8) and size (entry+12)
    for (size_t e = lie.size() - 3 * 32; e + 16 <= lie.size(); e += 16) {
        lie[e + 8] = 0xF0; lie[e + 9] = 0xFF; lie[e + 10] = 0xFF; lie[e + 11] = 0x7F;
        lie[e + 12] = 0xFF; lie[e + 13] = 0xFF; lie[e + 14] = 0xFF; lie[e + 15] = 0xFF;
    }
    CHECK(probe(m, lie, &A));
    V = Au = nullptr; vn = an = 0;
    if (vp_avi_index_idx1(m.f, &A, &V, &vn, &Au, &an)) {
        for (uint32_t i = 0; i < vn; i++) CHECK((uint64_t)V[i].off + V[i].size <= lie.size());
        vp_avi_free(V); vp_avi_free(Au);
    }

    // --- vp_read_span: the aligned span must be sized in 64-bit (regression: on the 32-bit device a
    // length near 4 GB wrapped the span to one sector, passed the capacity check and returned a
    // pointer the caller then read `len` bytes from)
    Bytes blob(4096, 0x5A);
    m.set(blob.data(), blob.size());
    uint8_t *buf = (uint8_t *)aligned_alloc(VP_ALIGN, 4096 + 2 * VP_SECT);
    CHECK(vp_read_span(m.f, buf, 4096 + 2 * VP_SECT, 100, 1000) == buf + 100);
    CHECK(vp_read_span(m.f, buf, 4096 + 2 * VP_SECT, 0x100, 0xFFFFFFFFu) == nullptr);
    CHECK(vp_read_span(m.f, buf, 4096 + 2 * VP_SECT, 0xFFFFFF00u, 0x200) == nullptr);
    CHECK(vp_read_span(m.f, buf, 4096 + 2 * VP_SECT, 4000, 200) == nullptr);   // past EOF
    CHECK(vp_read_span(m.f, buf, 4096 + 2 * VP_SECT, 0, 0) == nullptr);
    free(buf);

    return TEST_DONE("avi");
}
