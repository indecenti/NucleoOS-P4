// nv_wasm_jpeg — ABI v19: the P4's hardware JPEG decoder for WASM apps. See nv_wasm.h for the import
// table.
//
// A comic page (baseline 1000x1383) took Libri ~1.8 s with TJpgDec in WASM. Here the hardware decodes
// it whole into a DMA buffer (RGB565, or 8-bit grey for a grey picture), then the CPU area-averages it to the exact size the app asked for (the
// PPA scales only in 1/16 steps: 1000 -> 496 is not one of them) straight into the app's memory. Every
// buffer is the call's own and is freed before it returns. Anything the decoder can't take is refused
// with a code (nv_jpeg_util.h): the app keeps its software decoder for those.
//
// Synchronous, on the app's own thread. Decoder and PPA share the 2D lock (nv_2d): a decode makes a
// scaled game canvas skip a frame at most; the decoder's own job timeout bounds the wait.
#include "nv_wasm_net.h"
#include "nv_wasm.h"
#include "nv_jpeg_util.h"
#include "nv_log.h"
#include "nv_2d.h"

#include <cstdlib>
#include <cstring>

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {

constexpr const char *TAG = "jpeg";
constexpr uint32_t kInMax = 8u * 1024 * 1024;   // compressed size ceiling
constexpr size_t   kLine = 128;                  // >= the PSRAM cache line (64 B on the P4)

jpeg_decoder_handle_t s_dec = nullptr;           // made on first use, kept (like the Gallery's)
SemaphoreHandle_t mtx(void) {                     // first use creates it (magic static: thread-safe)
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}
bool lock(void) { return mtx() && xSemaphoreTake(mtx(), pdMS_TO_TICKS(2000)) == pdTRUE; }
void unlock(void) { xSemaphoreGive(mtx()); }

bool gfx_ok(wasm_exec_env_t env) { return (nv_wasm_env_perms(env) & NV_WPERM_GFX) != 0; }

// Internal RAM for a few tens of KB, for ~100 ms, only with plenty left: the Wi-Fi driver's buffers die
// of SRAM famine (ENGINEERING_RULES §8). Otherwise PSRAM: slower, same result.
bool internal_ok(void) { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > 96 * 1024; }

// The decoded picture reaches the scaler in bands of rows copied into internal RAM by the AXI-GDMA
// (nv_2d_copy): the CPU reads PSRAM at ~16 MB/s here (a 1000x1383 page took 170 ms to scale straight
// from PSRAM), the DMA in long bursts. Bands start on a multiple of 8 rows: the MCU-padded picture has
// rows of a multiple of 8 pixels, so every band starts and ends on a cache line. No internal RAM, or the
// DMA refused: rows are read from PSRAM directly (slower, same result).
struct Band {
    const uint8_t *src = nullptr;   // the decoded picture (PSRAM)
    size_t stride = 0;              // bytes per row
    int total = 0;                  // rows, padded to the MCU
    uint8_t *buf = nullptr;         // internal RAM, `rows` rows
    int rows = 0, first = 0, n = 0;
};
const void *band_row(void *ctx, int y) {
    Band *b = (Band *)ctx;
    if (!b->buf) return b->src + (size_t)y * b->stride;
    if (y >= b->first && y < b->first + b->n) return b->buf + (size_t)(y - b->first) * b->stride;
    const int f = y & ~7, n = b->total - f < b->rows ? b->total - f : b->rows;
    if (nv_2d_copy(b->buf, b->src + (size_t)f * b->stride, (size_t)n * b->stride, 100) != ESP_OK) {
        heap_caps_free(b->buf);   // from here on straight from PSRAM
        b->buf = nullptr;
        return b->src + (size_t)y * b->stride;
    }
    b->first = f;
    b->n = n;
    return b->buf + (size_t)(y - f) * b->stride;
}

// nv.jpeg_info(jpg, len) -> (w << 16) | h when the hardware decodes it, else NV_JPEG_E_*.
int32_t nvi_jpeg_info(wasm_exec_env_t env, const void *jpg, uint32_t len) {
    if (!gfx_ok(env)) return NV_JPEG_E_PERM;
    nv_jpeg_hdr_t h;
    const int r = nv_jpeg_probe((const uint8_t *)jpg, len, &h);
    return r == NV_JPEG_OK ? (int32_t)((h.w << 16) | h.h) : r;
}

// nv.jpeg_decode(jpg, len, out, out_cap, w, h) -> 0, or NV_JPEG_E_*: the picture as RGB565 (the canvas
// byte order), exactly w x h, rows of w pixels.
int32_t nvi_jpeg_decode(wasm_exec_env_t env, const void *jpg, uint32_t len, void *out, uint32_t out_cap,
                        int32_t w, int32_t h) {
    if (!gfx_ok(env)) return NV_JPEG_E_PERM;
    if (!jpg || !out || len == 0 || len > kInMax || w < 1 || h < 1 || w > NV_JPEG_MAX_SIDE ||
        h > NV_JPEG_MAX_SIDE || (uint64_t)out_cap < (uint64_t)w * h * 2 || ((uintptr_t)out & 1))
        return NV_JPEG_E_ARG;
    nv_jpeg_hdr_t hd;
    int r = nv_jpeg_probe((const uint8_t *)jpg, len, &hd);
    if (r != NV_JPEG_OK) return r;
    if (!lock()) return NV_JPEG_E_DECODE;
    const int64_t t0 = esp_timer_get_time();
    if (!s_dec) {
        jpeg_decode_engine_cfg_t eng = {};
        eng.intr_priority = 0;
        eng.timeout_ms = 300;   // a 4096x4096 4:4:4 picture; a damaged stream ends here
        if (jpeg_new_decoder_engine(&eng, &s_dec) != ESP_OK) s_dec = nullptr;
    }
    const int pitch = (hd.w + hd.mcu_w - 1) / hd.mcu_w * hd.mcu_w;
    const int rows = (hd.h + hd.mcu_h - 1) / hd.mcu_h * hd.mcu_h;
    const int bpp = hd.nf == 1 ? 1 : 2;   // grey comes out as 8-bit grey
    uint8_t *in = nullptr, *px = nullptr;
    uint32_t *acc = nullptr;
    size_t in_cap = 0, px_cap = 0;
    int64_t t1 = t0;
    if (!s_dec) {
        r = NV_JPEG_E_DECODE;
        goto done;
    }
    {   // DMA buffers of our own. Not jpeg_alloc_decoder_mem: it zero-fills (calloc), ~2.8 MB of PSRAM
        // writes for a comic page that the decoder overwrites anyway. The output only needs whole cache
        // lines (the driver invalidates them before the DMA and after); the input, nothing.
        in_cap = len;
        in = (uint8_t *)heap_caps_malloc(in_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        px_cap = ((size_t)pitch * rows * bpp + kLine - 1) & ~(kLine - 1);
        px = (uint8_t *)heap_caps_aligned_alloc(kLine, px_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const size_t scr = nv_scale_scratch_words(w) * sizeof(uint32_t);   // <= 96 KB
        acc = internal_ok() ? (uint32_t *)heap_caps_malloc(scr, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : nullptr;
        if (!acc) acc = (uint32_t *)heap_caps_malloc(scr, MALLOC_CAP_SPIRAM);
        if (!in || !px || !acc) {
            r = NV_JPEG_E_NOMEM;
            goto done;
        }
    }
    memcpy(in, jpg, len);
    {
        jpeg_decode_cfg_t cfg = {};
        cfg.output_format = bpp == 1 ? JPEG_DECODE_OUT_FORMAT_GRAY : JPEG_DECODE_OUT_FORMAT_RGB565;
        cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;   // low byte first: the canvas/panel order
        cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
        uint32_t outsz = 0;
        if (nv_2d_jpeg_decode(s_dec, &cfg, in, len, px, (uint32_t)px_cap, &outsz) != ESP_OK) {
            r = NV_JPEG_E_DECODE;
            goto done;
        }
    }
    free(in);   // room for the scaler's reads
    in = nullptr;
    t1 = esp_timer_get_time();
    {   // the decoder invalidated px's cache lines (M2C) before handing it back
        Band b;
        b.src = px;
        b.stride = (size_t)pitch * bpp;
        b.total = rows;
        b.rows = 8;   // 16 KB for a 1000 px wide page
        if ((size_t)b.rows * b.stride <= 32 * 1024 && internal_ok())
            b.buf = (uint8_t *)heap_caps_aligned_alloc(64, (size_t)b.rows * b.stride,
                                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        const bool banded = b.buf != nullptr;
        nv_scale_area_rows(band_row, &b, bpp, hd.w, hd.h, (uint16_t *)out, w, h, acc);
        const char *how = !banded ? " (no internal RAM: from PSRAM)" : !b.buf ? " (DMA refused: from PSRAM)" : "";
        heap_caps_free(b.buf);
        r = NV_JPEG_OK;
        NV_LOGI(TAG, "%dx%d -> %dx%d: decode %lld ms, scale %lld ms%s", hd.w, hd.h, (int)w, (int)h,
                (long long)((t1 - t0) / 1000), (long long)((esp_timer_get_time() - t1) / 1000), how);
    }
done:
    free(in);
    free(px);
    free(acc);
    unlock();
    return r;
}

NativeSymbol s_natives[] = {
    {"jpeg_info",   (void *)nvi_jpeg_info,   "(*~)i",     nullptr},
    {"jpeg_decode", (void *)nvi_jpeg_decode, "(*~*~ii)i", nullptr},
};

}  // namespace

void nv_wasm_jpeg_register(void) {
    if (!wasm_runtime_register_natives("nv", s_natives, sizeof s_natives / sizeof s_natives[0]))
        NV_LOGE(TAG, "register ABI 19 jpeg natives failed");
}
