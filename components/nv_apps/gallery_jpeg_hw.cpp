// gallery_jpeg_hw — see header. Lazy-init engine/client (mirrors nv_vplayer.c's ensure_engine()),
// kept for the process lifetime once first touched (gallery_jpeg_hw_release frees them).
#include "gallery_jpeg_hw.h"

#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "nv_sd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

jpeg_decoder_handle_t s_dec = nullptr;
ppa_client_handle_t   s_ppa = nullptr;

// The engine/client singletons are shared between the LVGL thread (viewer full decode) and the
// nv_bgwork worker (thumbnail builds), so every public op serializes here. Magic static:
// thread-safe first-use creation. A viewer decode waits at most one thumb op (~tens of ms).
SemaphoreHandle_t hw_mtx(void) {
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}
struct HwLock {
    HwLock()  { xSemaphoreTake(hw_mtx(), portMAX_DELAY); }
    ~HwLock() { xSemaphoreGive(hw_mtx()); }
};

bool ensure_hw(void) {
    if (!s_dec) {
        jpeg_decode_engine_cfg_t eng = {};
        eng.intr_priority = 0;
        eng.timeout_ms    = 100;
        if (jpeg_new_decoder_engine(&eng, &s_dec) != ESP_OK) { s_dec = nullptr; return false; }
    }
    if (!s_ppa) {
        ppa_client_config_t cc = {};
        cc.oper_type = PPA_OPERATION_SRM;
        if (ppa_register_client(&cc, &s_ppa) != ESP_OK) { s_ppa = nullptr; return false; }
    }
    return true;
}

constexpr int    kMaxDim = 2048;              // decode guard: 2048x2048x2 = 8 MB of PSRAM
constexpr size_t kInMax  = 6 * 1024 * 1024;   // compressed-size ceiling for one picture

// Decode `len` bytes of baseline JPEG held in a decoder INPUT buffer. Caller holds HwLock.
bool decode_locked(const uint8_t *jpg, size_t len, gallery_raster_t *out) {
    jpeg_decode_picture_info_t info = {};
    if (jpeg_decoder_get_info(jpg, (uint32_t)len, &info) != ESP_OK) return false;
    const int w = (int)info.width, h = (int)info.height;
    if (w <= 0 || h <= 0 || w > kMaxDim || h > kMaxDim) return false;
    // The decoder writes whole MCUs: 16 px wide for 4:2:0 / 4:2:2, 8 for 4:4:4 and grey, and the
    // same vertically for 4:2:0. It rejects an output buffer smaller than that (sizing to w*h*2
    // made every 1920x1080 camera photo, 1088 rows decoded, fall back to the slow SW decoder).
    const int mcu_w = (info.sample_method == JPEG_DOWN_SAMPLING_YUV444 ||
                       info.sample_method == JPEG_DOWN_SAMPLING_GRAY) ? 8 : 16;
    const int mcu_h = (info.sample_method == JPEG_DOWN_SAMPLING_YUV420) ? 16 : 8;
    const int pitch = (w + mcu_w - 1) / mcu_w * mcu_w;
    const int rows  = (h + mcu_h - 1) / mcu_h * mcu_h;

    jpeg_decode_memory_alloc_cfg_t om_cfg = {};
    om_cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    size_t out_cap = 0;
    uint8_t *px = (uint8_t *)jpeg_alloc_decoder_mem((size_t)pitch * rows * 2, &om_cfg, &out_cap);
    if (!px) return false;

    jpeg_decode_cfg_t cfg = {};
    cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    // BGR = RGB565 low byte first (jpeg_types.h: "small endian"), the layout LVGL and the panel
    // use. RGB is the high-byte-first order: every thumbnail and viewed photo was coloured static.
    cfg.rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    cfg.conv_std      = JPEG_YUV_RGB_CONV_STD_BT601;
    uint32_t outsz = 0;
    if (jpeg_decoder_process(s_dec, &cfg, jpg, (uint32_t)len, px, (uint32_t)out_cap, &outsz) != ESP_OK) {
        free(px);
        return false;
    }
    out->px = px;
    out->len = out_cap;
    out->w = w;
    out->h = h;
    out->pitch = pitch;
    return true;
}

uint8_t *alloc_in(size_t len) {
    jpeg_decode_memory_alloc_cfg_t cfg = {};
    cfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
    size_t cap = 0;
    return (uint8_t *)jpeg_alloc_decoder_mem(gallery_ppa_align_size(len), &cfg, &cap);
}

uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// Seek to the first video chunk ('##dc' / '##db') of a RIFF AVI and return its payload size, or 0.
// Walks RIFF 'AVI ' -> LIST 'movi' (descending into 'rec ' lists), skipping everything else.
uint32_t avi_seek_first_frame(FILE *f) {
    uint8_t h[12];
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "AVI ", 4)) return 0;
    for (int guard = 0; guard < 4096; guard++) {
        uint8_t c[8];
        if (fread(c, 1, 8, f) != 8) return 0;
        const uint32_t sz = le32(c + 4);
        if (!memcmp(c, "LIST", 4) || !memcmp(c, "RIFF", 4)) {
            uint8_t type[4];
            if (fread(type, 1, 4, f) != 4) return 0;
            if (!memcmp(type, "movi", 4) || !memcmp(type, "rec ", 4)) continue;   // descend
            if (fseek(f, (long)sz - 4 + (sz & 1), SEEK_CUR) != 0) return 0;      // skip the list
            continue;
        }
        if (c[2] == 'd' && (c[3] == 'c' || c[3] == 'b') && sz >= 128) return sz;   // video frame
        if (fseek(f, (long)(sz + (sz & 1)), SEEK_CUR) != 0) return 0;              // idx1, audio...
    }
    return 0;
}

// Shared SRM setup: the input is the decoded picture (w x h visible pixels in rows `pitch` apart);
// callers narrow the input block (cover crop) or offset the output block (letterbox) and set the
// scale.
void fill_common(ppa_srm_oper_config_t &op, const gallery_raster_t *src,
                 uint8_t *dst, int dst_w, int dst_h, size_t dst_cap) {
    op = {};
    op.in.buffer  = src->px;
    op.in.pic_w   = (uint32_t)src->pitch;
    op.in.pic_h   = (uint32_t)src->h;
    op.in.block_w = (uint32_t)src->w;
    op.in.block_h = (uint32_t)src->h;
    op.in.srm_cm  = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer      = dst;
    op.out.buffer_size = (uint32_t)dst_cap;
    op.out.pic_w  = (uint32_t)dst_w;
    op.out.pic_h  = (uint32_t)dst_h;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.mode = PPA_TRANS_MODE_BLOCKING;
}

bool run_srm(const ppa_srm_oper_config_t &op, const gallery_raster_t *src, uint8_t *dst,
             size_t dst_len) {
    esp_cache_msync(src->px, src->len, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    // Write back what the CPU put in dst (the viewer pre-fills its letterbox) BEFORE the DMA:
    // the M2C invalidate below otherwise discards those still-cached pixels and the borders show
    // whatever PSRAM held before (bits of the grid under the viewer's caption bar).
    esp_cache_msync(dst, dst_len, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    bool ok = ppa_do_scale_rotate_mirror(s_ppa, &op) == ESP_OK;
    if (ok) esp_cache_msync(dst, dst_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    return ok;
}

}  // namespace

size_t gallery_ppa_align_size(size_t raw_bytes) {
    return (raw_bytes + (GALLERY_PPA_ALIGN - 1)) & ~(size_t)(GALLERY_PPA_ALIGN - 1);
}

bool gallery_jpeg_hw_decode_file(const char *posix_path, gallery_raster_t *out) {
    *out = {};
    if (!posix_path) return false;
    HwLock lk;
    if (!ensure_hw()) return false;

    FILE *f = nv_sd_fopen(posix_path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    // Sized to the actual file: a fixed 512 KB slab rejected every real phone photo.
    if (fsz <= 0 || (size_t)fsz > kInMax) { nv_sd_fclose(f); return false; }
    uint8_t *jpg = alloc_in((size_t)fsz);
    if (!jpg) { nv_sd_fclose(f); return false; }
    const size_t rd = fread(jpg, 1, (size_t)fsz, f);
    nv_sd_fclose(f);
    const bool ok = rd == (size_t)fsz && decode_locked(jpg, rd, out);
    free(jpg);
    return ok;
}

bool gallery_jpeg_hw_decode_avi_poster(const char *posix_path, gallery_raster_t *out) {
    *out = {};
    if (!posix_path) return false;
    HwLock lk;
    if (!ensure_hw()) return false;

    FILE *f = nv_sd_fopen(posix_path, "rb");
    if (!f) return false;
    const uint32_t len = avi_seek_first_frame(f);
    uint8_t *jpg = (len && len <= kInMax) ? alloc_in(len) : nullptr;
    const bool rd_ok = jpg && fread(jpg, 1, len, f) == len;
    nv_sd_fclose(f);
    // Only Motion-JPEG frames start with SOI; anything else (MPEG-4, H.264 in AVI) is not ours.
    const bool ok = rd_ok && jpg[0] == 0xFF && jpg[1] == 0xD8 && decode_locked(jpg, len, out);
    free(jpg);
    return ok;
}

void gallery_jpeg_hw_free(gallery_raster_t *r) {
    if (r && r->px) free(r->px);
    if (r) *r = {};
}

bool gallery_ppa_scale_cover(const gallery_raster_t *src, uint8_t *dst, int dst_w, int dst_h,
                             size_t dst_cap) {
    if (!src || !src->px || !dst || src->w <= 0 || src->h <= 0 || dst_w <= 0 || dst_h <= 0) return false;
    if (dst_cap < gallery_ppa_align_size((size_t)dst_w * dst_h * 2)) return false;
    HwLock lk;
    if (!ensure_hw()) return false;

    // The PPA applies scales in 1/16 steps, rounded DOWN (its argument check uses the exact float,
    // the hardware doesn't): 160/1024 ran at 2/16, filled 128 of the 160 columns and left the rest
    // as uninitialised memory. Use one exact k/16 for both axes, the smallest that covers dst, and
    // centre-crop the input to exactly dst * 16/k so the whole of dst is written.
    const int sw = src->w, sh = src->h;
    int k = (dst_w * 16 + sw - 1) / sw;
    { const int ky = (dst_h * 16 + sh - 1) / sh; if (ky > k) k = ky; }
    if (k < 1) k = 1;
    int bw = (dst_w * 16 + k - 1) / k, bh = (dst_h * 16 + k - 1) / k;
    if (bw > sw) bw = sw;
    if (bh > sh) bh = sh;

    ppa_srm_oper_config_t op;
    fill_common(op, src, dst, dst_w, dst_h, dst_cap);
    op.in.block_offset_x = (uint32_t)((sw - bw) / 2);
    op.in.block_offset_y = (uint32_t)((sh - bh) / 2);
    op.in.block_w = (uint32_t)bw;
    op.in.block_h = (uint32_t)bh;
    op.scale_x = op.scale_y = (float)k / 16.0f;
    return run_srm(op, src, dst, dst_cap);
}

bool gallery_ppa_scale_fit(const gallery_raster_t *src, uint8_t *dst, int dst_w, int dst_h,
                           size_t dst_cap) {
    if (!src || !src->px || !dst || src->w <= 0 || src->h <= 0 || dst_w <= 0 || dst_h <= 0) return false;
    if (dst_cap < gallery_ppa_align_size((size_t)dst_w * dst_h * 2)) return false;
    HwLock lk;
    if (!ensure_hw()) return false;

    // Uniform letterbox scale, snapped DOWN to the PPA's 1/16 grid (it rounds any other value down
    // anyway): the largest k/16 whose output fits, so the centring below matches what is drawn.
    const int sw = src->w, sh = src->h;
    int k = (dst_w * 16) / sw;
    { const int ky = (dst_h * 16) / sh; if (ky < k) k = ky; }
    if (k < 1) k = 1;   // > 16x larger than the viewer: the PPA's floor; shows the centre
    int tw = sw * k / 16, th = sh * k / 16;

    ppa_srm_oper_config_t op;
    fill_common(op, src, dst, dst_w, dst_h, dst_cap);
    if (tw > dst_w || th > dst_h) {   // only with the k = 1 floor: crop the input to what fits
        const int bw = dst_w * 16 < sw ? dst_w * 16 : sw, bh = dst_h * 16 < sh ? dst_h * 16 : sh;
        op.in.block_offset_x = (uint32_t)((sw - bw) / 2);
        op.in.block_offset_y = (uint32_t)((sh - bh) / 2);
        op.in.block_w = (uint32_t)bw;
        op.in.block_h = (uint32_t)bh;
        tw = bw * k / 16;
        th = bh * k / 16;
    }
    op.out.block_offset_x = (uint32_t)(((dst_w - tw) / 2) & ~1);
    op.out.block_offset_y = (uint32_t)(((dst_h - th) / 2) & ~1);
    op.scale_x = op.scale_y = (float)k / 16.0f;
    return run_srm(op, src, dst, dst_cap);
}

void gallery_jpeg_hw_release(void) {
    HwLock lk;
    if (s_dec) { jpeg_del_decoder_engine(s_dec); s_dec = nullptr; }
    if (s_ppa) { ppa_unregister_client(s_ppa); s_ppa = nullptr; }
}
