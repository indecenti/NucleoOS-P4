// secondscreen_app — the board as a USB extended display for the PC (Second Screen).
// Transport is nv_usb (vendor bulk JPEG frames from the Windows IDD driver + HID touch
// back). While streaming, LVGL is stopped (lvgl_port_stop) and frames go straight from
// the P4 hardware JPEG decoder to the JD9165 panel (1024x576 letterboxed on 1024x600).
// The GT911 is then read directly: touches are forwarded to the PC as HID reports, and
// a swipe that STARTS on the left edge strip (the system back gesture) exits streaming.
#include "apps_internal.h"

#include "nv_app.h"
#include "nv_icons.h"
#include "nv_i18n.h"
#include "nv_fonts.h"
#include "nv_theme.h"
#include "nv_hal.h"
#include "nv_usb.h"
#include "nv_log.h"

#include "lvgl.h"
#include "nv_config.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_timer.h"
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <cstring>
#include <cstdio>

namespace {

constexpr const char *TAG = "secondscreen";

constexpr int kPanelW = 1024;
constexpr int kPanelH = 600;
constexpr int kYOff = (kPanelH - NV_USB_SCREEN_H) / 2;  // 12 px letterbox bands
constexpr int kEdgeStripW = 28;    // left-edge strip (matches the system back gesture)
constexpr int kEdgeTravel = 80;    // rightward travel that triggers the exit

enum class Mode { WAIT, STREAM, PAUSED };

// UI (LVGL thread only)
lv_obj_t *s_status = nullptr;
lv_obj_t *s_hint = nullptr;
lv_obj_t *s_fps = nullptr;
lv_obj_t *s_resume = nullptr;
lv_timer_t *s_timer = nullptr;

// Streaming engine (shared between nv_usb transfer task, touch task, LVGL thread)
volatile bool s_open = false;       // app page alive (sink registered)
volatile bool s_takeover = false;   // LVGL stopped, panel is ours
volatile Mode s_mode = Mode::WAIT;
volatile int64_t s_last_frame_us = 0;
jpeg_decoder_handle_t s_jpgd = nullptr;        // lazy, kept across app sessions
uint8_t *s_fb[2] = {nullptr, nullptr};         // decoded RGB565 frames (PSRAM, DMA-able)
size_t s_fb_len = 0;
int s_fb_idx = 0;
SemaphoreHandle_t s_fb_lock = nullptr;  // frame_sink holds it across decode/draw; page_deleted blocks on it before free

// Perf counters (written by the sink, read by the status timer — display only).
volatile uint32_t s_dec_us = 0;     // EMA of HW JPEG decode time
volatile uint32_t s_draw_us = 0;    // EMA of panel blit time
volatile uint32_t s_bytes = 0;      // JPEG bytes since boot (timer computes MB/s deltas)

inline uint32_t ema_us(uint32_t prev, int64_t sample) {
    return prev ? (uint32_t)((prev * 7 + (uint32_t)sample) / 8) : (uint32_t)sample;
}

// ---------------------------------------------------------------- UI helpers
// Apply the current mode to the labels/button. Caller holds the LVGL lock.
void ui_apply_mode(void) {
    if (!s_status) return;
    const bool paused = (s_mode == Mode::PAUSED);
    lv_label_set_text(s_status, paused ? nv_tr(NV_STR_SS_PAUSED)
                                       : (nv_usb_mounted() ? nv_tr(NV_STR_SS_USB_OK)
                                                           : nv_tr(NV_STR_SS_USB_NO)));
    if (paused) {
        lv_obj_remove_flag(s_resume, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_resume, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

void status_timer_cb(lv_timer_t *) {
    ui_apply_mode();
    // Link stats: input fps + decode/blit cost + JPEG bandwidth (delta since last tick).
    static uint32_t prev_bytes = 0;
    static int64_t prev_t = 0;
    const int64_t now = esp_timer_get_time();
    const uint32_t bytes = s_bytes;
    float mbs = 0.f;
    if (prev_t && bytes >= prev_bytes)
        mbs = (float)(bytes - prev_bytes) / ((float)(now - prev_t) / 1e6f) / 1e6f;
    prev_bytes = bytes;
    prev_t = now;

    const float fps = nv_usb_input_fps();
    if (fps > 0.f && nv_usb_frames_total() > 0) {
        char b[64];
        snprintf(b, sizeof b, "%.0f fps  |  dec %.1f ms  |  %.1f MB/s",
                 (double)fps, (double)(s_dec_us / 1000.0f), (double)mbs);
        lv_label_set_text(s_fps, b);
    } else {
        lv_label_set_text(s_fps, nv_tr(NV_STR_SS_WAIT));
    }
}

// ---------------------------------------------------------------- streaming exit
// Runs on the touch task: give the panel back to LVGL and show the app UI again.
void exit_takeover(Mode next_mode) {
    s_takeover = false;
    s_mode = next_mode;
    vTaskDelay(pdMS_TO_TICKS(40));  // let an in-flight decode/draw finish
    lvgl_port_resume();
    if (lvgl_port_lock(1000)) {
        // LVGL saw zero input while we owned the panel: without this, the accumulated
        // inactivity trips the screen-sleep watcher one tick after a long session ends.
        lv_display_trigger_activity(nullptr);
        ui_apply_mode();
        lv_obj_invalidate(lv_screen_active());  // repaint everything over our frame
        lvgl_port_unlock();
    }
    NV_LOGI(TAG, "streaming stopped (%s)", next_mode == Mode::PAUSED ? "paused" : "wait");
}

// ---------------------------------------------------------------- touch task
// While streaming: read the GT911 directly (the LVGL indev timer is stopped), forward
// fingers to the PC over HID, detect the left-edge exit swipe, and watchdog the link.
void touch_task(void *) {
    auto *tp = (esp_lcd_touch_handle_t)nv_hal_touch();
    bool sent_press = false;
    bool edge_candidate = false;   // current contact started on the left edge strip
    uint16_t edge_x0 = 0;
    bool was_down = false;

    while (s_takeover) {
        // Only a real link drop ends streaming: the IDD driver sends frames ONLY when
        // the desktop changes, so a frame gap is normal (static screen keeps the last
        // frame, like a real monitor). No frame-timeout watchdog.
        if (!nv_usb_mounted()) {
            exit_takeover(Mode::WAIT);
            break;
        }

        if (tp) {
            esp_lcd_touch_read_data(tp);
            esp_lcd_touch_point_data_t pts[NV_USB_TOUCH_MAX] = {};
            uint8_t cnt = 0;
            esp_lcd_touch_get_data(tp, pts, &cnt, NV_USB_TOUCH_MAX);

            if (cnt > 0 && !was_down) {  // first contact of this gesture
                edge_candidate = (pts[0].x < kEdgeStripW);
                edge_x0 = pts[0].x;
            }
            was_down = cnt > 0;

            if (edge_candidate && cnt > 0 &&
                (int)pts[0].x - (int)edge_x0 > kEdgeTravel) {
                // System back gesture: leave streaming, don't leak the touch to the PC.
                exit_takeover(Mode::PAUSED);
                break;
            }

            if (!edge_candidate) {
                if (cnt > 0) {
                    nv_usb_touch_pt_t out[NV_USB_TOUCH_MAX];
                    for (uint8_t i = 0; i < cnt && i < NV_USB_TOUCH_MAX; i++) {
                        int y = (int)pts[i].y - kYOff;  // panel 1024x600 -> screen 1024x576
                        if (y < 0) y = 0;
                        if (y >= NV_USB_SCREEN_H) y = NV_USB_SCREEN_H - 1;
                        int x = pts[i].x < kPanelW ? pts[i].x : kPanelW - 1;
                        out[i] = {pts[i].track_id, (uint16_t)x, (uint16_t)y,
                                  (uint8_t)(pts[i].strength ? pts[i].strength : 30)};
                    }
                    nv_usb_touch_report(out, cnt > NV_USB_TOUCH_MAX ? NV_USB_TOUCH_MAX : cnt);
                    sent_press = true;
                } else if (sent_press) {
                    nv_usb_touch_report(nullptr, 0);  // release
                    sent_press = false;
                }
            }
            if (cnt == 0) edge_candidate = false;
        }
        // GT911 misreports when polled faster than ~20 ms.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // Streaming ended (timeout/unmount/back-swipe) with a finger still down: release it, or the
    // PC keeps the last contact held forever. No-op on the host side if we're already unmounted.
    if (sent_press) nv_usb_touch_report(nullptr, 0);
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------- takeover
// Runs on the nv_usb transfer task, on the first frame: stop LVGL, paint the letterbox
// bands black, start the direct touch reader.
void takeover(void) {
    lvgl_port_stop();
    vTaskDelay(pdMS_TO_TICKS(40));  // let an in-flight LVGL flush finish

    // Plugging a monitor turns it on: if the screen slept while waiting, the SystemUI
    // wake path is frozen with LVGL — restore the backlight directly.
    nv_hal_backlight_set(nv_config_get_int("brightness", 90));

    auto panel = (esp_lcd_panel_handle_t)nv_hal_panel();
    memset(s_fb[0], 0, (size_t)kPanelW * kYOff * 2);
    esp_lcd_panel_draw_bitmap(panel, 0, 0, kPanelW, kYOff, s_fb[0]);
    esp_lcd_panel_draw_bitmap(panel, 0, kPanelH - kYOff, kPanelW, kPanelH, s_fb[0]);

    s_takeover = true;
    s_last_frame_us = esp_timer_get_time();
    xTaskCreate(touch_task, "ss_touch", 4096, nullptr, 5, nullptr);
    NV_LOGI(TAG, "streaming started (LVGL stopped, direct panel draw)");
}

// ---------------------------------------------------------------- frame sink
// Runs on the nv_usb transfer task for every complete JPEG frame from the PC.
void frame_sink(const uint8_t *jpg, uint32_t len, uint16_t w, uint16_t h, void *) {
    // Hold s_fb_lock across the WHOLE guard+decode+draw. page_deleted sets s_open=false, unhooks the
    // sink, then blocks on this same lock before freeing s_fb: a sink already touching s_fb forces
    // teardown to wait for it (no UAF); a sink that starts after close either never reaches here
    // (unhooked) or bails at the guard below without touching freed memory. Unlike the old bounded
    // poll, this can neither over-wait into a free nor hang teardown — the jpeg engine self-times-out
    // (50 ms) and the blit is bounded, so the lock is always released in bounded time.
    if (!s_fb_lock) return;
    xSemaphoreTake(s_fb_lock, portMAX_DELAY);
    if (!s_open || s_mode == Mode::PAUSED || !s_fb[0] ||
        w != NV_USB_SCREEN_W || h != NV_USB_SCREEN_H) { xSemaphoreGive(s_fb_lock); return; }

    if (!s_takeover) {
        s_mode = Mode::STREAM;
        takeover();
    }

    // BGR element order: matches this esp_lcd DPI pipeline (same as the P4 EV-board
    // reference); flip to RGB here if colors ever come out swapped.
    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    const int64_t t0 = esp_timer_get_time();
    if (jpeg_decoder_process(s_jpgd, &cfg, jpg, len, s_fb[s_fb_idx], s_fb_len, &out_size) != ESP_OK) {
        xSemaphoreGive(s_fb_lock);
        return;
    }
    const int64_t t1 = esp_timer_get_time();
    if (!s_takeover) { xSemaphoreGive(s_fb_lock); return; }  // exit raced the decode: don't draw over LVGL
    esp_lcd_panel_draw_bitmap((esp_lcd_panel_handle_t)nv_hal_panel(),
                              0, kYOff, kPanelW, kYOff + NV_USB_SCREEN_H, s_fb[s_fb_idx]);
    const int64_t t2 = esp_timer_get_time();
    s_dec_us = ema_us(s_dec_us, t1 - t0);
    s_draw_us = ema_us(s_draw_us, t2 - t1);
    s_bytes += len;
    s_fb_idx ^= 1;
    s_last_frame_us = esp_timer_get_time();
    xSemaphoreGive(s_fb_lock);
}

// ---------------------------------------------------------------- lifecycle
void resume_cb(lv_event_t *) {
    s_mode = Mode::STREAM;  // next frame re-takes the panel
    ui_apply_mode();
}

void page_deleted(lv_event_t *) {
    s_open = false;                          // a fresh frame_sink now bails at its guard
    nv_usb_set_frame_sink(nullptr, nullptr); // no NEW sink calls after this returns
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    // Block until any frame_sink already past its guard (decoding into s_fb on the USB transfer task)
    // releases the lock, THEN free — a decode into freed memory would corrupt the heap. Taking the
    // same lock the sink holds is an exact handshake: no fixed drain bound to over-wait past (the old
    // 60*4ms cap could expire mid-decode -> UAF) and no busy-poll. The sink can't hold it forever
    // (jpeg engine times out at 50 ms, blit bounded) and never takes the LVGL lock, so blocking here
    // under that lock can't deadlock. Freeing here returns ~2.3 MB PSRAM to the memory broker.
    if (s_fb_lock) xSemaphoreTake(s_fb_lock, portMAX_DELAY);
    for (auto &fb : s_fb) { if (fb) { free(fb); fb = nullptr; } }
    s_fb_len = 0;
    s_fb_idx = 0;
    if (s_fb_lock) xSemaphoreGive(s_fb_lock);
    s_mode = Mode::WAIT;
    s_status = nullptr; s_hint = nullptr; s_fps = nullptr; s_resume = nullptr;
}

void ss_build(lv_obj_t *content) {
    const NvTheme *th = nv_theme_get();
    lv_obj_set_style_bg_color(content, th->bg, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);

    // The OTG-HS controller is in HOST mode (USB speaker) this boot — TinyUSB never started,
    // so the extend-screen transport can't work. Tell the user how to switch instead of hanging.
    if (nv_config_get_bool("usbhost", true)) {
        lv_obj_t *lb = lv_label_create(content);
        lv_obj_set_width(lb, lv_pct(80));
        lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(lb, &nv_font_14, 0);
        lv_obj_set_style_text_color(lb, th->text, 0);
        lv_obj_center(lb);
        lv_label_set_text(lb, "USB is in host-audio mode (USB speaker).\n\n"
                              "To use the second screen: Terminal -> 'usb device' -> reboot.");
        return;
    }

    // Decoder engine (lazy, kept) + two decoded-frame buffers (freed on close).
    if (!s_jpgd) {
        jpeg_decode_engine_cfg_t eng = { .intr_priority = 0, .timeout_ms = 50 };
        if (jpeg_new_decoder_engine(&eng, &s_jpgd) != ESP_OK) {
            s_jpgd = nullptr;
            NV_LOGE(TAG, "HW JPEG decoder unavailable");
        }
    }
    if (!s_fb_lock) s_fb_lock = xSemaphoreCreateMutex();  // lazy, kept; serializes s_fb sink vs. teardown
    if (s_jpgd && !s_fb[0]) {
        jpeg_decode_memory_alloc_cfg_t rx_mem = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
        size_t got = 0;
        s_fb[0] = (uint8_t *)jpeg_alloc_decoder_mem((size_t)kPanelW * NV_USB_SCREEN_H * 2, &rx_mem, &got);
        s_fb[1] = (uint8_t *)jpeg_alloc_decoder_mem((size_t)kPanelW * NV_USB_SCREEN_H * 2, &rx_mem, &got);
        s_fb_len = got;
        if (!s_fb[0] || !s_fb[1]) NV_LOGE(TAG, "frame buffer alloc failed");
    }

    lv_obj_t *col = lv_obj_create(content);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(col, 24, 0);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(col, page_deleted, LV_EVENT_DELETE, nullptr);

    lv_obj_t *icon = lv_image_create(col);
    lv_image_set_src(icon, &nv_icon_screen);

    lv_obj_t *title = lv_label_create(col);
    lv_label_set_text(title, nv_tr(NV_STR_APP_SCREEN));
    lv_obj_set_style_text_font(title, &nv_font_28, 0);
    lv_obj_set_style_text_color(title, th->text_strong, 0);

    s_status = lv_label_create(col);
    lv_obj_set_style_text_color(s_status, th->text_strong, 0);

    s_fps = lv_label_create(col);
    lv_obj_set_style_text_color(s_fps, th->text_dim, 0);

    s_hint = lv_label_create(col);
    lv_label_set_text(s_hint, nv_tr(NV_STR_SS_HINT));
    lv_obj_set_width(s_hint, lv_pct(70));
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_hint, th->text_dim, 0);

    s_resume = lv_button_create(col);
    lv_obj_set_style_bg_color(s_resume, th->primary, 0);
    lv_obj_add_event_cb(s_resume, resume_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *rl = lv_label_create(s_resume);
    lv_label_set_text(rl, nv_tr(NV_STR_SS_RESUME));
    lv_obj_center(rl);
    lv_obj_add_flag(s_resume, LV_OBJ_FLAG_HIDDEN);

    // Auto-open on connect (SystemUI reads "ss_auto" when the PC starts streaming).
    lv_obj_t *arow = lv_obj_create(col);
    lv_obj_remove_style_all(arow);
    lv_obj_set_size(arow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(arow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(arow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(arow, 10, 0);
    lv_obj_set_style_pad_top(arow, 8, 0);
    lv_obj_clear_flag(arow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *al = lv_label_create(arow);
    lv_label_set_text(al, nv_tr(NV_STR_SS_AUTO));
    lv_obj_set_style_text_color(al, th->text_dim, 0);
    lv_obj_t *sw = lv_switch_create(arow);
    if (nv_config_get_bool("ss_auto", true)) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, [](lv_event_t *e) {
        lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
        nv_config_set_bool("ss_auto", lv_obj_has_state(o, LV_STATE_CHECKED));
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    s_mode = Mode::WAIT;
    s_fb_idx = 0;
    s_timer = lv_timer_create(status_timer_cb, 500, nullptr);
    ui_apply_mode();

    s_open = true;
    if (s_fb_lock) nv_usb_set_frame_sink(frame_sink, nullptr);
    else NV_LOGE(TAG, "frame lock alloc failed; streaming disabled");
}

const NvApp kScreenApp = {"secondscreen", "Second Screen", &nv_icon_screen, 4u << 20,
                          ss_build, NV_STR_APP_SCREEN, nullptr};

}  // namespace

void secondscreen_app_register(void) { nv_app_register(&kScreenApp); }
