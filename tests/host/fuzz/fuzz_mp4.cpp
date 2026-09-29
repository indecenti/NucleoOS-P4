// libFuzzer: vp_mp4 on arbitrary moov bytes (the input is the whole moov box). The sample table and
// the keyframe seek must stay inside what was parsed.
#include "vp_mp4.h"

#include <cstdlib>
#include <cstring>
#include <initializer_list>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 4u * 1024 * 1024) return 0;          // play_mp4 refuses a bigger moov
    uint8_t *m = (uint8_t *)malloc(size ? size : 1); // exact size: ASan sees any overread
    memcpy(m, data, size);
    vp_track_t V = {}, A = {};
    vp_mp4_parse_moov(m, (uint32_t)size, &V, &A);
    for (vp_track_t *T : {&V, &A}) {
        if (!T->present) continue;
        if (!T->tbl || !T->nsamp || T->nsamp > 200000) abort();
        for (int want : {0, 1, 1000, 65536, 1 << 30}) if (vp_mp4_seek_video_index(T, want) >= T->nsamp) abort();
        if (T->sps && (T->sps < m || T->sps + T->sps_len > m + size)) abort();
        if (T->pps && (T->pps < m || T->pps + T->pps_len > m + size)) abort();
        vp_mp4_track_free(T);
    }
    free(m);
    return 0;
}
