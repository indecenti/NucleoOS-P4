// nv_wallpaper — see nv_wallpaper.h.
//
// Pipeline (nv_bgwork worker, never the LVGL thread):
//   probe   JPEG markers from the first 128 KB: SOF size + luma MCU width (the HW decoder pads
//           each row to it), baseline check, EXIF orientation (phone photos are stored sideways).
//   decode  HW JPEG -> RGB565 at native size (gallery_jpeg_hw, shared engine + lock).
//   reduce  2x2 box halving while the crop is still >= 2x the panel on both axes, so the final
//           bilinear step never skips source pixels (no aliasing on 4K photos).
//   sample  bilinear, 8-bit weights, in the *displayed* orientation: a centred crop to the panel
//           aspect ("cover", like every phone launcher), mapped back through the EXIF transform.
//   encode  HW JPEG (baseline 4:2:0, q92) of exactly 1024x600 — what nv_ui's loader requires.
//   commit  write a temp file, then unlink + rename (FATFS rename never overwrites); on a failed
//           rename the temp file stays as the only good copy.
// The result is handed back to the LVGL thread (lv_async_call) for the launcher reload + toast.
#include "nv_wallpaper.h"

#include "gallery_jpeg_hw.h"
#include "nv_open.h"
#include "nv_ui.h"
#include "nv_i18n.h"
#include "nv_notify.h"
#include "nv_sd.h"
#include "nv_pins.h"
#include "nv_bgwork.h"
#include "nv_log.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_encode.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const char *TAG = "wallpaper";
constexpr char kWallPath[] = "/sdcard/wallpaper.jpg";   // nv_ui's loader reads exactly this file
constexpr char kWallTmp[]  = "/sdcard/wallpaper.tmp";
constexpr int  kOutW = NV_LCD_H_RES;                     // panel-native landscape, 1024
constexpr int  kOutH = NV_LCD_V_RES;                     // 600
constexpr uint32_t kMaxPixels = 3840u * 2160u;           // 4K UHD: 16.6 MB of RGB565, the ceiling
constexpr size_t   kProbeBytes = 128 * 1024;             // markers + EXIF live well inside this
constexpr int      kQuality = 92;

std::atomic<bool> s_busy{false};

// ---------------------------------------------------------------- JPEG probe
struct JpegInfo {
    int  w = 0, h = 0;
    int  mcux = 8;        // luma MCU width: decoded rows are padded to a multiple of it
    int  orient = 1;      // EXIF orientation 1..8
    bool baseline = false;
};

// EXIF APP1 -> orientation tag (0x0112) of IFD0. Every read is bounds-checked against the segment.
void parse_exif(const uint8_t *seg, uint32_t sl, int *orient) {
    if (sl < 14 || memcmp(seg, "Exif\0\0", 6) != 0) return;
    const uint8_t *t = seg + 6;
    const uint32_t tl = sl - 6;
    const bool le = t[0] == 'I' && t[1] == 'I';
    if (!le && !(t[0] == 'M' && t[1] == 'M')) return;
    auto u16 = [&](uint32_t o) -> uint32_t {
        if ((uint64_t)o + 2 > tl) return 0;
        return le ? (uint32_t)(t[o] | t[o + 1] << 8) : (uint32_t)(t[o] << 8 | t[o + 1]);
    };
    auto u32 = [&](uint32_t o) -> uint32_t {
        if ((uint64_t)o + 4 > tl) return 0;
        return le ? ((uint32_t)t[o] | (uint32_t)t[o + 1] << 8 | (uint32_t)t[o + 2] << 16 | (uint32_t)t[o + 3] << 24)
                  : ((uint32_t)t[o] << 24 | (uint32_t)t[o + 1] << 16 | (uint32_t)t[o + 2] << 8 | (uint32_t)t[o + 3]);
    };
    if (u16(2) != 42) return;
    const uint32_t ifd = u32(4);
    const uint32_t cnt = u16(ifd);
    if (cnt == 0 || cnt > 512) return;
    for (uint32_t k = 0; k < cnt; k++) {
        const uint64_t e = (uint64_t)ifd + 2 + (uint64_t)k * 12;
        if (e + 12 > tl) break;
        if (u16((uint32_t)e) == 0x0112) {
            const uint32_t v = u16((uint32_t)e + 8);   // SHORT, count 1: left-justified in the value
            if (v >= 1 && v <= 8) *orient = (int)v;
            return;
        }
    }
}

bool jpeg_probe(const char *path, JpegInfo *ji) {
    FILE *f = nv_sd_fopen(path, "rb");
    if (!f) return false;
    uint8_t *b = (uint8_t *)heap_caps_malloc(kProbeBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) { nv_sd_fclose(f); return false; }
    const size_t n = fread(b, 1, kProbeBytes, f);
    nv_sd_fclose(f);

    bool sof = false;
    if (n >= 4 && b[0] == 0xFF && b[1] == 0xD8) {
        size_t i = 2;
        while (i + 4 <= n) {
            if (b[i] != 0xFF) { i++; continue; }          // tolerate stray bytes between segments
            const uint8_t m = b[i + 1];
            if (m == 0xFF) { i++; continue; }            // fill byte
            if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) { i += 2; continue; }   // no length
            if (m == 0xD9 || m == 0xDA) break;           // EOI / SOS before any SOF
            const uint32_t len = (uint32_t)b[i + 2] << 8 | b[i + 3];
            if (len < 2 || (uint64_t)i + 2 + len > n) break;
            const uint8_t *seg = b + i + 4;
            const uint32_t sl = len - 2;
            if (m == 0xE1) parse_exif(seg, sl, &ji->orient);
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                ji->baseline = (m == 0xC0 || m == 0xC1);   // HW decoder: sequential DCT only
                if (sl >= 9) {
                    ji->h = seg[1] << 8 | seg[2];
                    ji->w = seg[3] << 8 | seg[4];
                    const int hi = seg[7] >> 4;          // component 0 (luma) sampling factor
                    ji->mcux = (hi >= 1 && hi <= 4 ? hi : 1) * 8;
                    sof = true;
                }
                break;
            }
            i += 2 + len;
        }
    }
    heap_caps_free(b);
    return sof;
}

// ---------------------------------------------------------------- resampling
inline uint32_t px565(const uint16_t *row, int x) { return row[x]; }

// 2x2 box average of a (w x h, stride) RGB565 image into a tight (w/2 x h/2) PSRAM buffer.
uint16_t *halve(const uint16_t *src, int w, int h, int stride, int *ow, int *oh) {
    const int nw = w / 2, nh = h / 2;
    if (nw < 1 || nh < 1) return nullptr;
    uint16_t *dst = (uint16_t *)heap_caps_malloc((size_t)nw * nh * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dst) return nullptr;
    for (int y = 0; y < nh; y++) {
        const uint16_t *r0 = src + (size_t)(2 * y) * stride;
        const uint16_t *r1 = r0 + stride;
        uint16_t *d = dst + (size_t)y * nw;
        for (int x = 0; x < nw; x++) {
            const uint32_t a = r0[2 * x], b = r0[2 * x + 1], c = r1[2 * x], e = r1[2 * x + 1];
            const uint32_t r = ((a >> 11) + (b >> 11) + (c >> 11) + (e >> 11) + 2) >> 2;
            const uint32_t g = (((a >> 5) & 63) + ((b >> 5) & 63) + ((c >> 5) & 63) + ((e >> 5) & 63) + 2) >> 2;
            const uint32_t bl = ((a & 31) + (b & 31) + (c & 31) + (e & 31) + 2) >> 2;
            d[x] = (uint16_t)(r << 11 | g << 5 | bl);
        }
    }
    *ow = nw;
    *oh = nh;
    return dst;
}

// Displayed (u,v) -> stored (x,y) for EXIF orientation `o`, 16.16 fixed point; W,H = stored size.
inline void map_orient(int o, int32_t u, int32_t v, int W, int H, int32_t *x, int32_t *y) {
    const int32_t wm = (int32_t)(W - 1) << 16, hm = (int32_t)(H - 1) << 16;
    switch (o) {
        case 2:  *x = wm - u; *y = v;      break;
        case 3:  *x = wm - u; *y = hm - v; break;
        case 4:  *x = u;      *y = hm - v; break;
        case 5:  *x = v;      *y = u;      break;
        case 6:  *x = v;      *y = hm - u; break;
        case 7:  *x = wm - v; *y = hm - u; break;
        case 8:  *x = wm - v; *y = u;      break;
        default: *x = u;      *y = v;      break;
    }
    if (*x < 0) *x = 0;
    if (*x > wm) *x = wm;
    if (*y < 0) *y = 0;
    if (*y > hm) *y = hm;
}

// Bilinear sample of the displayed-space crop (cx,cy,cw,ch) into dst (kOutW x kOutH, tight).
void sample(const uint16_t *src, int W, int H, int stride, int orient,
            int cx, int cy, int cw, int ch, uint16_t *dst) {
    const int32_t su = (int32_t)(((int64_t)cw << 16) / kOutW);
    const int32_t sv = (int32_t)(((int64_t)ch << 16) / kOutH);
    for (int oy = 0; oy < kOutH; oy++) {
        const int32_t v = ((int32_t)cy << 16) + sv * oy + sv / 2 - 32768;   // pixel centres
        uint16_t *d = dst + (size_t)oy * kOutW;
        for (int ox = 0; ox < kOutW; ox++) {
            const int32_t u = ((int32_t)cx << 16) + su * ox + su / 2 - 32768;
            int32_t xf, yf;
            map_orient(orient, u, v, W, H, &xf, &yf);
            const int x0 = xf >> 16, y0 = yf >> 16;
            const int x1 = x0 + 1 < W ? x0 + 1 : x0;
            const int y1 = y0 + 1 < H ? y0 + 1 : y0;
            const uint32_t fx = (uint32_t)(xf >> 8) & 0xFF, fy = (uint32_t)(yf >> 8) & 0xFF;
            const uint16_t *r0 = src + (size_t)y0 * stride;
            const uint16_t *r1 = src + (size_t)y1 * stride;
            const uint32_t p00 = px565(r0, x0), p10 = px565(r0, x1), p01 = px565(r1, x0), p11 = px565(r1, x1);
            const uint32_t w00 = (256 - fx) * (256 - fy), w10 = fx * (256 - fy);
            const uint32_t w01 = (256 - fx) * fy,         w11 = fx * fy;
            const uint32_t r = ((p00 >> 11) * w00 + (p10 >> 11) * w10 + (p01 >> 11) * w01 + (p11 >> 11) * w11 + 32768) >> 16;
            const uint32_t g = (((p00 >> 5) & 63) * w00 + ((p10 >> 5) & 63) * w10 +
                                ((p01 >> 5) & 63) * w01 + ((p11 >> 5) & 63) * w11 + 32768) >> 16;
            const uint32_t b = ((p00 & 31) * w00 + (p10 & 31) * w10 + (p01 & 31) * w01 + (p11 & 31) * w11 + 32768) >> 16;
            d[ox] = (uint16_t)(r << 11 | g << 5 | b);
        }
    }
}

// ---------------------------------------------------------------- encode + commit
bool encode_and_write(const uint16_t *img) {
    jpeg_encoder_handle_t enc = nullptr;
    jpeg_encode_engine_cfg_t eng = {};
    eng.timeout_ms = 500;
    if (jpeg_new_encoder_engine(&eng, &enc) != ESP_OK) return false;

    // YUV420 works on 16-row MCUs: the encoder reads a 16-aligned input (600 -> 608 rows). Zero the
    // padding so it never reads past the picture (the same trick as nv_hal_screenshot).
    const int    vpad    = (kOutH + 15) & ~15;
    const size_t raw     = (size_t)kOutW * kOutH * 2;
    const size_t enc_raw = (size_t)kOutW * vpad * 2;
    jpeg_encode_memory_alloc_cfg_t in_cfg = {};
    in_cfg.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
    jpeg_encode_memory_alloc_cfg_t out_cfg = {};
    out_cfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
    size_t in_got = 0, out_got = 0;
    uint8_t *in_buf  = (uint8_t *)jpeg_alloc_encoder_mem(enc_raw, &in_cfg, &in_got);
    uint8_t *out_buf = (uint8_t *)jpeg_alloc_encoder_mem(enc_raw, &out_cfg, &out_got);
    bool ok = false;
    if (in_buf && out_buf) {
        memset(in_buf, 0, in_got);
        memcpy(in_buf, img, raw);
        jpeg_encode_cfg_t cfg = {};
        cfg.width = kOutW;
        cfg.height = kOutH;
        cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
        cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
        cfg.image_quality = kQuality;
        uint32_t out_size = 0;
        if (jpeg_encoder_process(enc, &cfg, in_buf, (uint32_t)in_got, out_buf, (uint32_t)out_got,
                                 &out_size) == ESP_OK && out_size > 0) {
            if (FILE *f = nv_sd_fopen(kWallTmp, "wb")) {
                const bool wrote = fwrite(out_buf, 1, out_size, f) == out_size;
                ok = (nv_sd_fclose(f) == 0) && wrote;
            }
        }
    }
    free(in_buf);
    free(out_buf);
    jpeg_del_encoder_engine(enc);
    if (!ok) { unlink(kWallTmp); return false; }

    unlink(kWallPath);   // FATFS rename refuses to overwrite
    if (rename(kWallTmp, kWallPath) != 0) {
        NV_LOGE(TAG, "rename failed: new wallpaper kept as %s", kWallTmp);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- worker job
void result_apply(void *arg) {
    const bool ok = arg != nullptr;
    s_busy = false;
    if (ok) {
        nv_ui_wallpaper_reload();
        nv_toast(NV_NOTE_OK, nv_tr(NV_STR_WALLPAPER_DONE));
    } else {
        nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_WALLPAPER_FAILED));
    }
}

void post_result(bool ok) {
    bool posted = false;
    if (lvgl_port_lock(3000)) {
        posted = lv_async_call(result_apply, ok ? (void *)1 : nullptr) == LV_RESULT_OK;
        lvgl_port_unlock();
    }
    if (!posted) s_busy = false;   // UI unreachable: at least allow a retry
}

bool convert(const char *path) {
    JpegInfo ji;
    if (!jpeg_probe(path, &ji) || !ji.baseline || ji.w <= 0 || ji.h <= 0) {
        NV_LOGW(TAG, "%s: not a baseline JPEG", path);
        return false;
    }
    if ((uint64_t)ji.w * (uint64_t)ji.h > kMaxPixels) {
        NV_LOGW(TAG, "%s: %dx%d exceeds the decode ceiling", path, ji.w, ji.h);
        return false;
    }
    gallery_raster_t rr;   // RGB565 low byte first, rows rr.pitch pixels apart
    if (!gallery_jpeg_hw_decode_file(path, &rr)) {
        NV_LOGW(TAG, "%s: HW decode failed", path);
        return false;
    }
    uint8_t *raw = rr.px;
    int W = rr.w, H = rr.h;
    int stride = rr.pitch;
    if ((uint64_t)stride * H * 2 > rr.len) { gallery_jpeg_hw_free(&rr); return false; }

    // Displayed geometry and the centred "cover" crop at the panel aspect.
    const bool swap = ji.orient >= 5;
    int lw = swap ? H : W, lh = swap ? W : H;
    int cw, ch;
    if ((int64_t)lw * kOutH > (int64_t)lh * kOutW) { ch = lh; cw = (int)((int64_t)lh * kOutW / kOutH); }
    else                                            { cw = lw; ch = (int)((int64_t)lw * kOutH / kOutW); }
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    int cx = (lw - cw) / 2, cy = (lh - ch) / 2;

    const uint16_t *src = (const uint16_t *)raw;
    uint16_t *owned = nullptr;   // a halved copy we allocated (the decode buffer is freed early)
    while (cw >= 2 * kOutW && ch >= 2 * kOutH) {
        int nw = 0, nh = 0;
        uint16_t *h2 = halve(src, W, H, stride, &nw, &nh);
        if (!h2) break;                                  // out of PSRAM: bilinear from what we have
        if (owned) heap_caps_free(owned);
        else { gallery_jpeg_hw_free(&rr); raw = nullptr; }
        owned = h2;
        src = h2;
        W = nw; H = nh; stride = nw;
        lw /= 2; lh /= 2; cw /= 2; ch /= 2; cx /= 2; cy /= 2;
    }

    uint16_t *out = (uint16_t *)heap_caps_malloc((size_t)kOutW * kOutH * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = false;
    if (out) {
        sample(src, W, H, stride, ji.orient, cx, cy, cw, ch, out);
        if (owned) { heap_caps_free(owned); owned = nullptr; }
        if (raw)   { gallery_jpeg_hw_free(&rr); raw = nullptr; }
        ok = encode_and_write(out);
        heap_caps_free(out);
    }
    if (owned) heap_caps_free(owned);
    if (raw) gallery_jpeg_hw_free(&rr);
    if (ok) NV_LOGI(TAG, "%s -> %s (%dx%d, EXIF %d)", path, kWallPath, ji.w, ji.h, ji.orient);
    return ok;
}

void job(void *arg) {
    char *path = (char *)arg;
    const bool ok = convert(path);
    heap_caps_free(path);
    post_result(ok);
}

bool action_run(const char *path, void *) { return nv_wallpaper_set_from_file(path); }

const NvOpenHandler kWallpaperAction = {
    "sys.wallpaper", nullptr, "image/jpeg", NV_STR_SET_WALLPAPER, nullptr, LV_SYMBOL_IMAGE,
    NV_OPEN_ACTION, 0, action_run, nullptr,
};

}  // namespace

bool nv_wallpaper_set_from_file(const char *path) {
    if (!path || !path[0]) return false;
    if (strcmp(nv_open_mime(path), "image/jpeg") != 0) {
        nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_WALLPAPER_FAILED));
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        nv_toast(NV_NOTE_WARN, nv_tr(NV_STR_FILE_NOT_FOUND));
        return false;
    }
    bool expected = false;
    if (!s_busy.compare_exchange_strong(expected, true)) {
        nv_toast(NV_NOTE_INFO, nv_tr(NV_STR_WALLPAPER_BUSY));
        return false;
    }
    const size_t n = strlen(path) + 1;
    char *copy = (char *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy) memcpy(copy, path, n);   // filled BEFORE submit: the worker may start immediately
    if (!copy || !nv_bgwork_submit(job, copy)) {
        if (copy) heap_caps_free(copy);
        s_busy = false;
        nv_toast(NV_NOTE_ERROR, nv_tr(NV_STR_WALLPAPER_FAILED));
        return false;
    }
    nv_toast(NV_NOTE_INFO, nv_tr(NV_STR_WALLPAPER_BUSY));
    return true;
}

bool nv_wallpaper_is_set(void) {
    struct stat st;
    return stat(kWallPath, &st) == 0;
}

void nv_wallpaper_clear(void) {
    unlink(kWallPath);
    nv_ui_wallpaper_reload();
}

void nv_wallpaper_register(void) { nv_open_register(&kWallpaperAction); }
