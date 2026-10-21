// libFuzzer target for nv_jpeg_util.h (ABI v19 nv.jpeg_*): the header probe never reads out of bounds and
// what it accepts is what the hardware decoder and the firmware's buffers are sized for; the scaler,
// driven by sizes from the input, writes exactly dw x dh pixels and reads only the visible source.
#include "nv_jpeg_util.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    // exact-size copy: ASan sees any read past the end
    std::vector<uint8_t> buf(d, d + n);
    nv_jpeg_hdr_t h;
    const int r = nv_jpeg_probe(buf.data(), buf.size(), &h);
    if (r == NV_JPEG_OK) {
        if (h.w < 1 || h.h < 1 || h.w > NV_JPEG_MAX_SIDE || h.h > NV_JPEG_MAX_SIDE) abort();
        if (h.nf != 1 && h.nf != 3) abort();
        if ((h.mcu_w != 8 && h.mcu_w != 16) || (h.mcu_h != 8 && h.mcu_h != 16)) abort();
    } else if (r != NV_JPEG_E_UNSUP && r != NV_JPEG_E_DECODE) {
        abort();
    }
    if (n >= 4) {   // scaler: sizes from the input, small enough to run fast
        const int sw = 1 + d[0] % 64, sh = 1 + d[1] % 64, dw = 1 + d[2] % 64, dh = 1 + d[3] % 64;
        const int pitch = sw + d[0] % 3, bpp = 1 + (d[1] & 1);
        // the last row ends at its last pixel: ASan sees a read beyond the visible source
        std::vector<uint8_t> src(((size_t)pitch * (sh - 1) + sw) * bpp);
        for (size_t i = 0; i < src.size(); i++) src[i] = (uint8_t)(i * 2654435761u >> 13);
        std::vector<uint16_t> dst((size_t)dw * dh);
        std::vector<uint32_t> scr(nv_scale_scratch_words(dw));
        nv_scale_area(src.data(), bpp, sw, sh, pitch, dst.data(), dw, dh, scr.data());
    }
    return 0;
}
