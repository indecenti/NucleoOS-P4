// libFuzzer: vp_avi on arbitrary files — header probe, then every index strategy the player tries
// (OpenDML super index for both streams, idx1, movi scan). Tables must stay inside the file.
#include "memfile.h"
#include "vp_avi.h"

#include <cstdlib>

static void check_tbl(const vp_ent_t *t, uint32_t n, long fsz) {
    for (uint32_t i = 0; i < n; i++)
        if (t[i].size && (uint64_t)t[i].off + t[i].size > (uint64_t)fsz + 1) abort();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static MemFile m;
    if (!m.set(data, size)) return 0;
    vp_avi_t A;
    if (!vp_avi_probe(m.f, &A)) return 0;
    if (A.movi_pos > A.fsz || A.movi_end > A.fsz) abort();

    vp_ent_t *V = nullptr, *Au = nullptr; uint32_t vn = 0, an = 0;
    if (vp_avi_index_odml(m.f, &A, A.v_indx, A.v_indx_sz, &V, &vn)) { check_tbl(V, vn, A.fsz); vp_avi_free(V); }
    if (vp_avi_index_odml(m.f, &A, A.a_indx, A.a_indx_sz, &Au, &an)) { check_tbl(Au, an, A.fsz); vp_avi_free(Au); }
    V = Au = nullptr; vn = an = 0;
    if (vp_avi_index_idx1(m.f, &A, &V, &vn, &Au, &an)) {
        check_tbl(V, vn, A.fsz); check_tbl(Au, an, A.fsz);
        vp_avi_free(V); vp_avi_free(Au);
    }
    V = nullptr; vn = 0;
    if (vp_avi_index_scan(m.f, &A, &V, &vn, nullptr)) { check_tbl(V, vn, A.fsz); vp_avi_free(V); }
    return 0;
}
