// gallery_jpeg_hw — see header. Lazy-init engine/client (mirrors nv_vplayer.c's ensure_engine()),
// left allocated for the process lifetime once first touched (no app-close hook exists in this
// OS — see gallery_jpeg_hw_release's doc comment).
#include "gallery_jpeg_hw.h"

#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "nv_sd.h"

#include <cstdio>
#include <cstdlib>

namespace {

jpeg_decoder_handle_t s_dec = nullptr;
ppa_client_handle_t   s_ppa = nullptr;

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

}  // namespace

size_t gallery_ppa_align_size(size_t raw_bytes) {
    return (raw_bytes + (GALLERY_PPA_ALIGN - 1)) & ~(size_t)(GALLERY_PPA_ALIGN - 1);
}

bool gallery_jpeg_hw_decode_file(const char *posix_path, int src_w, int src_h,
                                  uint8_t **out_buf, size_t *out_len) {
    *out_buf = nullptr;
    *out_len = 0;
    if (!posix_path || src_w <= 0 || src_h <= 0) return false;
    if (!ensure_hw()) return false;

    FILE *f = nv_sd_fopen(posix_path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    // Size the input buffer to the ACTUAL file, not a fixed 512KB slab. The old 512KB cap rejected
    // any real phone photo (2-5MB JPEG) -> HW decode failed -> the grid fell back to a full-res SW
    // TJPGD decode PER TILE (the gallery's main source of slowness). PSRAM is abundant; cap only at
    // a sane ceiling that still comfortably covers a high-quality JPEG within the 2048x2048 dim cap.
    const size_t kInMax = 6 * 1024 * 1024;
    if (fsz <= 0 || (size_t)fsz > kInMax) { nv_sd_fclose(f); return false; }

    jpeg_decode_memory_alloc_cfg_t im_cfg = {};
    im_cfg.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
    size_t in_cap = 0;
    uint8_t *jpg = (uint8_t *)jpeg_alloc_decoder_mem(gallery_ppa_align_size((size_t)fsz), &im_cfg, &in_cap);
    if (!jpg) { nv_sd_fclose(f); return false; }

    size_t rd = fread(jpg, 1, (size_t)fsz, f);
    nv_sd_fclose(f);
    if (rd != (size_t)fsz) { free(jpg); return false; }

    // The decoder validates its output buffer's address AND size against the platform cache-line
    // alignment (esp_dma_is_buffer_alignment_satisfied) and rejects anything not obtained via
    // jpeg_alloc_decoder_mem — a plain heap_caps_malloc buffer would fail that check.
    jpeg_decode_memory_alloc_cfg_t om_cfg = {};
    om_cfg.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
    const size_t need = (size_t)src_w * (size_t)src_h * 2;
    size_t out_cap = 0;
    uint8_t *decoded = (uint8_t *)jpeg_alloc_decoder_mem(need, &om_cfg, &out_cap);
    if (!decoded) { free(jpg); return false; }

    jpeg_decode_cfg_t cfg = {};
    cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    cfg.rgb_order     = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
    cfg.conv_std      = JPEG_YUV_RGB_CONV_STD_BT601;

    uint32_t outsz = 0;
    bool ok = jpeg_decoder_process(s_dec, &cfg, jpg, (uint32_t)rd, decoded,
                                    (uint32_t)out_cap, &outsz) == ESP_OK;
    free(jpg);
    if (!ok) { free(decoded); return false; }

    *out_buf = decoded;
    *out_len = outsz;
    return true;
}

void gallery_jpeg_hw_free(uint8_t *buf) {
    if (buf) free(buf);
}

namespace {

// Shared SRM setup for both scale variants: input is the full (src_w x src_h) picture, no
// input-side offset/letterbox (that's only ever applied on the output side). Caller fills in
// out.block_offset_x/y and scale_x/y afterward to select STRETCH vs FIT.
void fill_common(ppa_srm_oper_config_t &op, const uint8_t *src, int src_w, int src_h,
                  uint8_t *dst, int dst_w, int dst_h, size_t dst_cap) {
    op = {};
    op.in.buffer  = src;
    op.in.pic_w   = (uint32_t)src_w;
    op.in.pic_h   = (uint32_t)src_h;
    op.in.block_w = (uint32_t)src_w;
    op.in.block_h = (uint32_t)src_h;
    op.in.srm_cm  = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer      = dst;
    op.out.buffer_size = (uint32_t)dst_cap;
    op.out.pic_w  = (uint32_t)dst_w;
    op.out.pic_h  = (uint32_t)dst_h;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.mode = PPA_TRANS_MODE_BLOCKING;
}

// Defensive cache sync around every PPA op, matching nv_vplayer.c:1171/1216's exact idiom (the
// PPA/JPEG drivers already msync internally around their own DMA, but this project's proven code
// re-syncs at every call site regardless — cheap, and it's what's been hardware-verified).
bool run_srm(const ppa_srm_oper_config_t &op, const uint8_t *src, size_t src_len,
             uint8_t *dst, size_t dst_len) {
    esp_cache_msync((void *)src, src_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    bool ok = ppa_do_scale_rotate_mirror(s_ppa, &op) == ESP_OK;
    if (ok) esp_cache_msync(dst, dst_len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    return ok;
}

}  // namespace

bool gallery_ppa_scale_stretch(const uint8_t *src, int src_w, int src_h,
                                uint8_t *dst, int dst_w, int dst_h, size_t dst_cap) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return false;
    if (dst_cap < gallery_ppa_align_size((size_t)dst_w * dst_h * 2)) return false;
    if (!ensure_hw()) return false;

    ppa_srm_oper_config_t op;
    fill_common(op, src, src_w, src_h, dst, dst_w, dst_h, dst_cap);
    op.scale_x = (float)dst_w / (float)src_w;
    op.scale_y = (float)dst_h / (float)src_h;

    return run_srm(op, src, (size_t)src_w * src_h * 2, dst, dst_cap);
}

bool gallery_ppa_scale_fit(const uint8_t *src, int src_w, int src_h,
                            uint8_t *dst, int dst_w, int dst_h, size_t dst_cap) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return false;
    if (dst_cap < gallery_ppa_align_size((size_t)dst_w * dst_h * 2)) return false;
    if (!ensure_hw()) return false;

    // Letterbox math ported verbatim from nv_vplayer.c's NV_VP_FIT mode: uniform scale (the
    // smaller of the two axis ratios), centered inside the (larger, fixed) dst canvas.
    float sc = (float)dst_w / (float)src_w;
    { float sy = (float)dst_h / (float)src_h; if (sy < sc) sc = sy; }
    int tw = ((int)(src_w * sc)) & ~1; if (tw < 2) tw = 2;
    int th = ((int)(src_h * sc)) & ~1; if (th < 2) th = 2;

    ppa_srm_oper_config_t op;
    fill_common(op, src, src_w, src_h, dst, dst_w, dst_h, dst_cap);
    op.out.block_offset_x = (uint32_t)(((dst_w - tw) / 2) & ~1);
    op.out.block_offset_y = (uint32_t)(((dst_h - th) / 2) & ~1);
    op.scale_x = sc;
    op.scale_y = sc;

    return run_srm(op, src, (size_t)src_w * src_h * 2, dst, dst_cap);
}

void gallery_jpeg_hw_release(void) {
    if (s_dec) { jpeg_del_decoder_engine(s_dec); s_dec = nullptr; }
    if (s_ppa) { ppa_unregister_client(s_ppa); s_ppa = nullptr; }
}
