// libFuzzer: pl_mpeg (MPEG-PS demux + MPEG-1 video + MP2 audio) on arbitrary .mpg bytes, opened the way
// nv_vplayer opens a clip (plm_create_with_file: 4 KB streaming buffer, like the device) and driven
// through the calls it makes: geometry, a few frames and samples, duration, a seek.
#include "pl_mpeg.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4 || size > (1u << 20)) return 0;
    FILE *f = fmemopen((void *)data, size, "rb");
    if (!f) return 0;
    plm_t *plm = plm_create_with_file(f, 1);           // closes f
    if (!plm) { fclose(f); return 0; }
    plm_set_loop(plm, 0);
    (void)plm_get_width(plm);
    (void)plm_get_height(plm);
    (void)plm_get_framerate(plm);
    (void)plm_get_samplerate(plm);
    (void)plm_get_num_audio_streams(plm);
    for (int i = 0; i < 24 && !plm_has_ended(plm); i++) if (!plm_decode_video(plm)) break;
    for (int i = 0; i < 24 && !plm_has_ended(plm); i++) if (!plm_decode_audio(plm)) break;
    (void)plm_get_duration(plm);
    (void)plm_seek_frame(plm, 0.2, 0);
    for (int i = 0; i < 4; i++) if (!plm_decode_video(plm)) break;
    plm_destroy(plm);
    return 0;
}
