// nv_ss engine — see include/nv_ss.h for the model (sources, LIVE/PAUSED, lock order).
#include "nv_ss.h"
#include "ss_internal.h"

#include "nv_hal.h"
#include "nv_config.h"
#include "nv_log.h"

#include "esp_lvgl_port.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_timer.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"
#include "driver/ppa.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <cstring>
#include <cstdio>

namespace {

constexpr const char *TAG = "nv_ss";
constexpr int kW = NV_SS_PANEL_W;
constexpr int kH = NV_SS_PANEL_H;
constexpr size_t kFbBytes = (size_t)kW * kH * 2;
constexpr int kDecMaxH = (kH + 15) & ~15;                 // 608: 4:2:0 MCU padding of a full frame
constexpr size_t kDecBytes = (size_t)kW * kDecMaxH * 2;   // decode scratch (RGB565)
constexpr size_t kJpegInBytes = 512 * 1024;               // staging for JPEGs not in DMA memory
constexpr int kEdgeStripW = 28;    // left-edge strip, same as the system back gesture
constexpr int kEdgeTravel = 80;    // rightward travel that pauses the session

SemaphoreHandle_t s_mtx = nullptr;            // engine lock (session state + buffers + panel)
volatile bool s_open = false;
volatile nv_ss_mode_t s_mode = NV_SS_IDLE;
volatile nv_ss_src_t s_src = NV_SS_SRC_NONE;
nv_ss_source_ops_t s_ops = {};
char s_peer[48] = "";
nv_ss_src_t s_last_src = NV_SS_SRC_NONE;
char s_last_reason[48] = "";
volatile uint32_t s_gen = 0;
volatile bool s_touch_run = false;            // touch task should keep running
volatile bool s_touch_alive = false;          // touch task exists (set/cleared by the task)
volatile bool s_resume_busy = false;

jpeg_decoder_handle_t s_jpgd = nullptr;       // lazy, kept across sessions
ppa_client_handle_t s_ppa = nullptr;          // lazy, kept
uint8_t *s_dec = nullptr;  size_t s_dec_cap = 0;   // decode output (freed on close)
uint8_t *s_jin = nullptr;  size_t s_jin_cap = 0;   // JPEG input staging (freed on close)
uint16_t *s_snap = nullptr;                          // panel picture saved on pause

// stats
volatile uint32_t s_updates = 0;
volatile uint32_t s_bytes = 0;
volatile uint32_t s_dec_us = 0;
int64_t s_session_t0 = 0;
float s_fps = 0.f, s_kbps = 0.f;
uint32_t s_prev_updates = 0, s_prev_bytes = 0;
int64_t s_prev_t = 0;

struct Lock {
    Lock()  { xSemaphoreTake(s_mtx, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mtx); }
};

uint16_t *panel_fb(void) {
    void *fb = nullptr;
    auto panel = (esp_lcd_panel_handle_t)nv_hal_panel();
    if (!panel || esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb) != ESP_OK) return nullptr;
    return (uint16_t *)fb;
}

void bump_gen(void) { s_gen = s_gen + 1; }

inline uint32_t ema_us(uint32_t prev, int64_t sample) {
    return prev ? (uint32_t)((prev * 7 + (uint32_t)sample) / 8) : (uint32_t)sample;
}

// ---------------------------------------------------------------- panel ownership
// Stop LVGL and take the panel. Caller: NOT on the LVGL thread and NOT holding s_mtx (it takes
// the LVGL port lock so a render pass already inside lv_timer_handler finishes first).
void lvgl_stop_locked_out(void) {
    if (lvgl_port_lock(1000)) { lvgl_port_stop(); lvgl_port_unlock(); }
    else lvgl_port_stop();
    vTaskDelay(pdMS_TO_TICKS(40));   // let an in-flight DMA2D flush land before we draw
}

// Give the panel back to LVGL and repaint the app. Caller must NOT hold s_mtx. `have_lvgl_lock`:
// the caller is on the LVGL thread (page teardown) and already holds the port lock.
void lvgl_give_back(bool have_lvgl_lock) {
    const bool locked = have_lvgl_lock || lvgl_port_lock(1000);
    lvgl_port_resume();   // lv_timer_enable() is LVGL API: call it under the port lock
    if (locked) {
        // LVGL saw no input while we owned the panel: without this the accumulated inactivity
        // trips screen-sleep one tick after a long session ends.
        lv_display_trigger_activity(nullptr);
        lv_obj_invalidate(lv_screen_active());
        if (!have_lvgl_lock) lvgl_port_unlock();
    }
}

void fb_fill_black(uint16_t *fb) {
    memset(fb, 0, kFbBytes);
    esp_cache_msync(fb, kFbBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// ---------------------------------------------------------------- touch
// Finger tracking: nv_hal's cache gives positions without GT911 track ids, so ids are assigned
// by nearest-neighbour matching against the previous sample (HID contacts need stable ids).
struct Tracker {
    nv_ss_touch_pt_t prev[NV_SS_TOUCH_MAX];
    int prev_n = 0;
    uint8_t next_id = 1;

    int update(const int16_t *xs, const int16_t *ys, int n, nv_ss_touch_pt_t *out) {
        bool used[NV_SS_TOUCH_MAX] = {};
        for (int i = 0; i < n; i++) {
            int best = -1, bestd = 120 * 120;
            for (int j = 0; j < prev_n; j++) {
                if (used[j]) continue;
                const int dx = xs[i] - prev[j].x, dy = ys[i] - prev[j].y;
                const int d = dx * dx + dy * dy;
                if (d < bestd) { bestd = d; best = j; }
            }
            uint8_t id;
            if (best >= 0) { used[best] = true; id = prev[best].id; }
            else {
                // fresh id not used by any surviving contact
                for (;;) {
                    id = next_id++;
                    if (next_id > 250) next_id = 1;
                    bool clash = false;
                    for (int j = 0; j < prev_n; j++) if (prev[j].id == id) clash = true;
                    if (!clash) break;
                }
            }
            out[i] = {id, xs[i], ys[i]};
        }
        memcpy(prev, out, sizeof(nv_ss_touch_pt_t) * n);
        prev_n = n;
        return n;
    }
};

void touch_task(void *) {
    Tracker trk;
    bool sent_press = false;
    bool edge_candidate = false;
    int16_t edge_x0 = 0;
    bool was_down = false;
    int alive_div = 0;
    // The ops are fixed for the whole session (set by begin() before the panel is taken).
    nv_ss_source_ops_t ops;
    { Lock l; ops = s_ops; }

    while (s_touch_run && s_mode == NV_SS_LIVE) {

        // Link watchdog (source-defined; e.g. USB unmounted, socket closed).
        if (ops.alive && ++alive_div >= 2) {
            alive_div = 0;
            if (!ops.alive(ops.user)) {
                const nv_ss_src_t src = s_src;
                nv_ss_end(src, "link lost");
                break;
            }
        }

        int16_t xs[NV_SS_TOUCH_MAX], ys[NV_SS_TOUCH_MAX];
        const int cnt = nv_hal_touch_points(xs, ys, NV_SS_TOUCH_MAX);

        if (cnt > 0 && !was_down) {   // first contact of this gesture
            edge_candidate = (xs[0] < kEdgeStripW);
            edge_x0 = xs[0];
        }
        was_down = cnt > 0;

        if (edge_candidate && cnt > 0 && xs[0] - edge_x0 > kEdgeTravel) {
            // System back gesture: pause, don't leak the swipe to the remote side.
            if (sent_press && ops.touch) ops.touch(nullptr, 0, ops.user);
            sent_press = false;
            ss_engine_pause_from_touch();
            break;
        }

        if (!edge_candidate) {
            if (cnt > 0) {
                nv_ss_touch_pt_t pts[NV_SS_TOUCH_MAX];
                trk.update(xs, ys, cnt, pts);
                if (ops.touch) ops.touch(pts, cnt, ops.user);
                sent_press = true;
            } else if (sent_press) {
                trk.prev_n = 0;
                if (ops.touch) ops.touch(nullptr, 0, ops.user);
                sent_press = false;
            }
        }
        if (cnt == 0) edge_candidate = false;
        vTaskDelay(pdMS_TO_TICKS(16));   // nv_hal samples the GT911 at ~60 Hz
    }
    // Ended with a finger down: release it, or the remote side keeps a phantom contact.
    if (sent_press && ops.touch) ops.touch(nullptr, 0, ops.user);
    s_touch_run = false;
    s_touch_alive = false;
    vTaskDelete(nullptr);
}

TaskHandle_t s_touch_task = nullptr;

bool start_touch_task(void) {
    // A previous session's task may still be draining (it exits within one poll period).
    for (int i = 0; i < 30 && s_touch_alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
    s_touch_run = true;
    s_touch_alive = true;
    // Internal stack: short-lived, self-deleting (PSRAM stacks can't self-delete).
    // 6 KB: ops.touch may send through TLS (NucleoCast over https).
    if (xTaskCreate(touch_task, "ss_touch", 6144, nullptr, 5, &s_touch_task) != pdPASS) {
        s_touch_run = false;
        s_touch_alive = false;
        return false;
    }
    return true;
}

bool on_touch_task(void) { return s_touch_alive && xTaskGetCurrentTaskHandle() == s_touch_task; }

void stop_touch_task(void) {
    s_touch_run = false;
    if (on_touch_task()) return;   // the task itself ends the session: it exits on its own
    // Wait (bounded) so a stale task can't outlive the session and call the next source's ops.
    for (int i = 0; i < 30 && s_touch_alive; i++) vTaskDelay(pdMS_TO_TICKS(10));
}

// ---------------------------------------------------------------- takeover
// Take the panel for the current owner. Caller: source/worker task, NOT holding s_mtx, NOT the
// LVGL thread. `restore`: repaint the picture saved at pause time instead of clearing.
bool take_panel(bool restore) {
    lvgl_stop_locked_out();
    // Plugging a monitor turns it on: if the screen slept, the SystemUI wake path is frozen with
    // LVGL — restore the backlight directly.
    nv_hal_backlight_set(nv_config_get_int("brightness", 90));
    {
        Lock l;
        uint16_t *fb = panel_fb();
        if (!fb || !s_open || s_src == NV_SS_SRC_NONE) {
            // Session vanished while we waited: give the panel straight back.
            xSemaphoreGive(s_mtx);
            lvgl_give_back(false);
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            return false;
        }
        if (restore && s_snap) {
            memcpy(fb, s_snap, kFbBytes);
            esp_cache_msync(fb, kFbBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        } else {
            fb_fill_black(fb);
        }
        if (s_snap) { heap_caps_free(s_snap); s_snap = nullptr; }
        s_mode = NV_SS_LIVE;
        bump_gen();
    }
    if (!start_touch_task()) {
        // No touch task = no exit path: refuse the takeover rather than freeze the device.
        NV_LOGE(TAG, "touch task create failed — staying on the app UI");
        { Lock l; s_mode = NV_SS_PAUSED; bump_gen(); }
        lvgl_give_back(false);
        return false;
    }
    NV_LOGI(TAG, "LIVE: %s (%s)", nv_ss_src_name(s_src), s_peer);
    return true;
}

// Save the panel picture and hand it back to LVGL. Caller: NOT holding s_mtx.
void release_panel(nv_ss_mode_t next, bool have_lvgl_lock) {
    {
        Lock l;
        if (s_mode != NV_SS_LIVE) { s_mode = next; bump_gen(); return; }
        if (next == NV_SS_PAUSED) {
            uint16_t *fb = panel_fb();
            if (fb && !s_snap)
                s_snap = (uint16_t *)heap_caps_malloc(kFbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (fb && s_snap) {
                esp_cache_msync(fb, kFbBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                memcpy(s_snap, fb, kFbBytes);
            }
        }
        s_mode = next;   // present()/fb_begin() now refuse: nothing draws after we unlock
        bump_gen();
    }
    s_touch_run = false;
    lvgl_give_back(have_lvgl_lock);
}

void resume_task(void *) {
    bool ok = false;
    nv_ss_source_ops_t ops;
    {
        Lock l;
        ops = s_ops;
        ok = s_open && s_mode == NV_SS_PAUSED && s_src != NV_SS_SRC_NONE;
    }
    if (ok && take_panel(true) && ops.resume) ops.resume(ops.user);
    s_resume_busy = false;
    vTaskDelete(nullptr);
}

bool ensure_hw(void) {
    if (!s_jpgd) {
        jpeg_decode_engine_cfg_t eng = {};
        eng.intr_priority = 0;
        eng.timeout_ms = 60;
        if (jpeg_new_decoder_engine(&eng, &s_jpgd) != ESP_OK) { s_jpgd = nullptr; return false; }
    }
    if (!s_ppa) {
        ppa_client_config_t c = {};
        c.oper_type = PPA_OPERATION_SRM;
        if (ppa_register_client(&c, &s_ppa) != ESP_OK) { s_ppa = nullptr; return false; }
    }
    return true;
}

// Decode into s_dec. Caller holds s_mtx. Returns padded row stride in pixels.
bool decode_locked(const uint8_t *jpg, uint32_t len, bool dma_buf, int *w, int *h, int *stride) {
    if (!s_jpgd || !s_dec || !jpg || len < 4) return false;
    jpeg_decode_picture_info_t info = {};
    if (jpeg_decoder_get_info(jpg, len, &info) != ESP_OK || !info.width || !info.height) return false;
    const int align = (info.sample_method == JPEG_DOWN_SAMPLING_YUV444 ||
                       info.sample_method == JPEG_DOWN_SAMPLING_GRAY) ? 8 : 16;
    const int aw = ((int)info.width + align - 1) & ~(align - 1);
    const int ah = ((int)info.height + 15) & ~15;   // 4:2:0 pads rows to 16; harmless otherwise
    if ((size_t)aw * ah * 2 > s_dec_cap) return false;

    const uint8_t *src = jpg;
    if (!dma_buf) {
        if (!s_jin || len > s_jin_cap) return false;
        memcpy(s_jin, jpg, len);
        src = s_jin;
    }
    // BGR element order = little-endian RGB565 as this DSI pipeline scans it (same as the P4
    // EV-board reference and the USB path before the engine existed).
    jpeg_decode_cfg_t cfg = {};
    cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
    cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    uint32_t out = 0;
    const int64_t t0 = esp_timer_get_time();
    if (jpeg_decoder_process(s_jpgd, &cfg, src, len, s_dec, s_dec_cap, &out) != ESP_OK) return false;
    s_dec_us = ema_us(s_dec_us, esp_timer_get_time() - t0);
    *w = (int)info.width;
    *h = (int)info.height;
    *stride = aw;
    return true;
}

}  // namespace

// ================================================================ internal (ss_internal.h)
void ss_engine_pause_from_touch(void) {
    nv_ss_source_ops_t ops;
    { Lock l; ops = s_ops; }
    release_panel(NV_SS_PAUSED, false);
    if (ops.pause) ops.pause(ops.user);
    NV_LOGI(TAG, "paused (edge swipe)");
}

// ================================================================ public API
const char *nv_ss_src_name(nv_ss_src_t src) {
    switch (src) {
    case NV_SS_SRC_USB:  return "USB";
    case NV_SS_SRC_CAST: return "Cast";
    case NV_SS_SRC_VNC:  return "VNC";
    default:             return "-";
    }
}

bool nv_ss_open(void) {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return false;
    if (!ensure_hw()) { NV_LOGE(TAG, "JPEG/PPA hardware unavailable"); }
    {
        Lock l;
        if (s_jpgd && !s_dec) {
            jpeg_decode_memory_alloc_cfg_t o = {};
            o.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER;
            s_dec = (uint8_t *)jpeg_alloc_decoder_mem(kDecBytes, &o, &s_dec_cap);
            jpeg_decode_memory_alloc_cfg_t i = {};
            i.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER;
            s_jin = (uint8_t *)jpeg_alloc_decoder_mem(kJpegInBytes, &i, &s_jin_cap);
            if (!s_dec || !s_jin) {
                NV_LOGE(TAG, "decode buffers alloc failed");
                if (s_dec) free(s_dec);
                if (s_jin) free(s_jin);
                s_dec = s_jin = nullptr;
                s_dec_cap = s_jin_cap = 0;
            }
        }
        s_open = true;
        s_mode = NV_SS_IDLE;
        s_src = NV_SS_SRC_NONE;
        bump_gen();
    }
    ss_usb_attach();    // USB frames flow to the engine while the app is open
    ss_cast_on_open();  // a network sender waiting for the app may stream now
    return s_dec != nullptr;
}

void nv_ss_close(void) {
    if (!s_mtx) return;
    ss_usb_detach();
    nv_ss_source_ops_t ops;
    nv_ss_mode_t mode;
    {
        Lock l;
        s_open = false;          // begin() refuses from now on
        ops = s_ops;
        mode = s_mode;
    }
    // Page teardown runs on the LVGL thread with the port lock held (nv_ui open/close). A LIVE
    // session can still be here (e.g. /api/ui/home while streaming): hand the panel back inline.
    if (mode == NV_SS_LIVE) release_panel(NV_SS_IDLE, true);
    stop_touch_task();
    if (ops.stop) ops.stop(false, ops.user);
    {
        Lock l;
        if (s_src != NV_SS_SRC_NONE) {
            s_last_src = s_src;
            snprintf(s_last_reason, sizeof s_last_reason, "%s", "app closed");
        }
        s_src = NV_SS_SRC_NONE;
        s_ops = {};
        s_mode = NV_SS_IDLE;
        if (s_dec) { free(s_dec); s_dec = nullptr; s_dec_cap = 0; }
        if (s_jin) { free(s_jin); s_jin = nullptr; s_jin_cap = 0; }
        if (s_snap) { heap_caps_free(s_snap); s_snap = nullptr; }
        bump_gen();
    }
}

bool nv_ss_is_open(void) { return s_open; }

void nv_ss_resume(void) {
    if (!s_mtx || s_resume_busy) return;
    {
        Lock l;
        if (!s_open || s_mode != NV_SS_PAUSED || s_src == NV_SS_SRC_NONE) return;
    }
    s_resume_busy = true;
    // The takeover must not run on the LVGL thread (the current lv_timer_handler pass would flush
    // the app UI over the restored picture): a one-shot worker takes the port lock instead.
    if (xTaskCreate(resume_task, "ss_resume", 6144, nullptr, 5, nullptr) != pdPASS)
        s_resume_busy = false;
}

void nv_ss_disconnect(void) {
    if (!s_mtx) return;
    nv_ss_source_ops_t ops;
    nv_ss_src_t src;
    { Lock l; ops = s_ops; src = s_src; }
    if (src == NV_SS_SRC_NONE) return;
    if (ops.stop) ops.stop(true, ops.user);
    nv_ss_end(src, "disconnected");
}

bool nv_ss_begin(nv_ss_src_t src, const nv_ss_source_ops_t *ops, const char *peer) {
    if (!s_mtx || src == NV_SS_SRC_NONE) return false;
    {
        Lock l;
        if (!s_open || !s_dec) return false;
        if (s_src != NV_SS_SRC_NONE && s_src != src) return false;   // someone else owns it
        if (s_src == src && s_mode != NV_SS_IDLE) return true;       // already ours
        s_src = src;
        s_ops = ops ? *ops : nv_ss_source_ops_t{};
        snprintf(s_peer, sizeof s_peer, "%s", peer ? peer : nv_ss_src_name(src));
        s_updates = 0;
        s_session_t0 = esp_timer_get_time();
        s_mode = NV_SS_IDLE;
        bump_gen();
    }
    if (!take_panel(false)) {
        Lock l;
        if (s_src != src) return false;
        if (s_mode != NV_SS_PAUSED) { s_src = NV_SS_SRC_NONE; s_ops = {}; bump_gen(); return false; }
        return true;   // kept PAUSED (no touch task): the user can Resume from the app
    }
    return true;
}

void nv_ss_end(nv_ss_src_t src, const char *reason) {
    if (!s_mtx || src == NV_SS_SRC_NONE) return;
    nv_ss_mode_t mode;
    {
        Lock l;
        if (s_src != src) return;
        mode = s_mode;
    }
    const bool on_lvgl = xTaskGetCurrentTaskHandle() == xTaskGetHandle("taskLVGL");
    if (mode == NV_SS_LIVE) release_panel(NV_SS_IDLE, on_lvgl);
    stop_touch_task();
    {
        Lock l;
        if (s_src != src) return;
        s_last_src = src;
        snprintf(s_last_reason, sizeof s_last_reason, "%s", reason ? reason : "ended");
        s_src = NV_SS_SRC_NONE;
        s_ops = {};
        s_mode = NV_SS_IDLE;
        if (s_snap) { heap_caps_free(s_snap); s_snap = nullptr; }
        bump_gen();
    }
    NV_LOGI(TAG, "session ended: %s (%s)", nv_ss_src_name(src), reason ? reason : "-");
}

bool nv_ss_owner(nv_ss_src_t src) { return src != NV_SS_SRC_NONE && s_src == src; }
bool nv_ss_is_live(nv_ss_src_t src) { return nv_ss_owner(src) && s_mode == NV_SS_LIVE; }

bool nv_ss_present_jpeg(nv_ss_src_t src, const uint8_t *jpg, uint32_t len, bool dma_buf,
                        int dx, int dy, int dw, int dh) {
    if (!s_mtx) return false;
    Lock l;
    if (s_src != src || s_mode != NV_SS_LIVE || !s_ppa) return false;
    int w, h, stride;
    if (!decode_locked(jpg, len, dma_buf, &w, &h, &stride)) return false;
    uint16_t *fb = panel_fb();
    if (!fb) return false;
    if (dw <= 0 || dh <= 0) { dw = w; dh = h; }
    // clip to the panel (scaled placements are clipped by shrinking the source block)
    if (dx < 0 || dy < 0 || dx >= kW || dy >= kH) return false;
    int bw = w, bh = h;
    float sx = (float)dw / (float)w, sy = (float)dh / (float)h;
    if (dx + dw > kW) { dw = kW - dx; bw = (int)(dw / sx); }
    if (dy + dh > kH) { dh = kH - dy; bh = (int)(dh / sy); }
    if (bw <= 0 || bh <= 0) return false;

    ppa_srm_oper_config_t op = {};
    op.in.buffer = s_dec;
    op.in.pic_w = (uint32_t)stride;
    op.in.pic_h = (uint32_t)((h + 15) & ~15);
    op.in.block_w = (uint32_t)bw;
    op.in.block_h = (uint32_t)bh;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = fb;
    op.out.buffer_size = kFbBytes;
    op.out.pic_w = kW;
    op.out.pic_h = kH;
    op.out.block_offset_x = (uint32_t)dx;
    op.out.block_offset_y = (uint32_t)dy;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x = sx;
    op.scale_y = sy;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    if (ppa_do_scale_rotate_mirror(s_ppa, &op) != ESP_OK) return false;
    s_updates = s_updates + 1;
    s_bytes = s_bytes + len;
    return true;
}

bool nv_ss_decode_jpeg(nv_ss_src_t src, const uint8_t *jpg, uint32_t len,
                       const uint16_t **px, int *w, int *h, int *stride) {
    if (!s_mtx) return false;
    Lock l;
    if (s_src != src || s_mode != NV_SS_LIVE) return false;
    if (!decode_locked(jpg, len, false, w, h, stride)) return false;
    // The decoder wrote s_dec by DMA: drop any stale cached lines before the CPU reads it.
    esp_cache_msync(s_dec, (size_t)(*stride) * (((*h) + 15) & ~15) * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    *px = (const uint16_t *)s_dec;
    return true;
}

bool nv_ss_fb_begin(nv_ss_src_t src, uint16_t **fb) {
    if (!s_mtx) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    uint16_t *p = (s_src == src && s_mode == NV_SS_LIVE) ? panel_fb() : nullptr;
    if (!p) { xSemaphoreGive(s_mtx); return false; }
    *fb = p;
    return true;
}

void nv_ss_fb_end(int x, int y, int w, int h) {
    uint16_t *fb = panel_fb();
    if (fb && w > 0 && h > 0) {
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > kW) w = kW - x;
        if (y + h > kH) h = kH - y;
        if (w > 0 && h > 0) {
            // Rows are contiguous in the panel buffer: one write-back over the covering span.
            uint8_t *start = (uint8_t *)(fb + (size_t)y * kW + x);
            const size_t span = ((size_t)(h - 1) * kW + w) * 2;
            esp_cache_msync(start, span, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        }
    }
    s_updates = s_updates + 1;
    xSemaphoreGive(s_mtx);
}

void nv_ss_fb_clear(nv_ss_src_t src) {
    uint16_t *fb;
    if (!nv_ss_fb_begin(src, &fb)) return;
    fb_fill_black(fb);
    xSemaphoreGive(s_mtx);
}

void nv_ss_stat_update(uint32_t bytes) { s_bytes = s_bytes + bytes; }

void nv_ss_get_status(nv_ss_status_t *st) {
    if (!st) return;
    memset(st, 0, sizeof *st);
    if (!s_mtx) return;
    const int64_t now = esp_timer_get_time();
    Lock l;
    // Rolling rates over >= 500 ms windows (the UI polls ~2 Hz).
    if (!s_prev_t) { s_prev_t = now; s_prev_updates = s_updates; s_prev_bytes = s_bytes; }
    const int64_t dt = now - s_prev_t;
    if (dt >= 500000) {
        s_fps = (float)(s_updates - s_prev_updates) * 1e6f / (float)dt;
        s_kbps = (float)(s_bytes - s_prev_bytes) * 1e6f / (float)dt / 1024.f;
        s_prev_t = now; s_prev_updates = s_updates; s_prev_bytes = s_bytes;
    }
    st->mode = s_mode;
    st->src = s_src;
    snprintf(st->peer, sizeof st->peer, "%s", s_peer);
    st->fps = s_mode == NV_SS_LIVE ? s_fps : 0.f;
    st->kbps = s_mode == NV_SS_LIVE ? s_kbps : 0.f;
    st->dec_ms = s_dec_us / 1000.f;
    st->updates = s_updates;
    st->session_s = (s_src != NV_SS_SRC_NONE && s_session_t0) ? (uint32_t)((now - s_session_t0) / 1000000) : 0;
    st->last_src = s_last_src;
    snprintf(st->last_reason, sizeof st->last_reason, "%s", s_last_reason);
    st->generation = s_gen;
}
