// nv_camera — OV02C10 2MP MIPI-CSI bring-up (this board's sensor; unsupported by Espressif's
// esp_cam_sensor, so we drive it directly). Chip-ID 0x5602 @ SCCB 0x36 on the shared internal
// I2C bus. Register init table ported from the Linux kernel drivers/media/i2c/ov02c10.c
// (1928x1092 30fps + 2-lane addendum). Pipeline: sensor RAW10 -> P4 ISP -> RGB565.
//   - reuse nv_hal_i2c_bus() for SCCB (no new master bus)
//   - do NOT acquire the MIPI PHY LDO (the display panel already holds channel 3)
#include <string.h>
#include <stdio.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "driver/ppa.h"
#include "driver/jpeg_encode.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_h264_enc_single_hw.h"   // P4 hardware H.264 encoder (MP4 recording)

#include "nv_camera.h"
#include "nv_mp4.h"   // minimal MP4 muxer
#include "nv_hal.h"   // nv_hal_i2c_bus()
#include <strings.h>  // strcasecmp (recording-format select by extension)
#include "nv_log.h"

static const char *TAG = "nv_cam";

#define OV02C10_ADDR      0x36
#define OV02C10_CHIP_ID   0x5602
#define CAM_W             1920        // output 1920x1080 (0x3808/0x380a) — P4 ISP max width is 1920,
#define CAM_H             1080        // so 1928 (Linux default) never processed → 0 frames
#define CAM_SCCB_FREQ     100000
#define CAM_LANE_MBPS     408         // IDI 81.6667MHz x5 = 408.3 Mbps/lane (NOT 800 — that
                                      // folklore never HS-syncs; from esp-video-components PR#46)
#define CAM_FB_LEN        ((size_t)CAM_W * CAM_H * 2)

// ---- OV02C10 register init (reg16, val8) ----
// EXACT copy of esp-video-components' ov02c10_input_24M_MIPI_2lane_raw10_1920x1080_30fps[]
// (Espressif's authoritative ESP32-P4 CSI+ISP set). The earlier Linux-derived table with a
// hand-patched "2-lane addendum" had the WRONG PLL (0x0305=0xe0, 0x0303=0x05) and WRONG frame
// timing (HTS/VTS 0x0474/0x0918 — those are the source's *commented-out* alternates), so the
// sensor's real MIPI rate never matched the 408 Mbps the CSI was told → 0 completed frames.
// Leading 0x0103 (reset) and trailing 0x0100 (stream-on) are issued in code, not here.
typedef struct { uint16_t reg; uint8_t val; } ov_reg_t;

static const ov_reg_t s_ov02c10_init[] = {
    {0x0301,0x08}, {0x0303,0x06}, {0x0304,0x01}, {0x0305,0x90}, {0x0313,0x40}, {0x031C,0x4F},
    {0x3016,0x32}, {0x301B,0xF0}, {0x3020,0x97}, {0x3021,0x23}, {0x3022,0x01}, {0x3026,0xB4},
    {0x3027,0xF1}, {0x303B,0x00}, {0x303C,0x4F}, {0x303D,0xE6}, {0x303E,0x00}, {0x303F,0x03},
    {0x3501,0x10}, {0x3502,0x6C}, {0x3504,0x0C}, {0x3507,0x00}, {0x3508,0x40}, {0x3509,0x00},
    {0x350A,0x01}, {0x350B,0x00}, {0x350C,0x41}, {0x3600,0x84}, {0x3603,0x08}, {0x3610,0x57},
    {0x3611,0x1B}, {0x3613,0x78}, {0x3623,0x00}, {0x3632,0xA0}, {0x3642,0xE8}, {0x364C,0x70},
    {0x365D,0x00}, {0x365F,0x0F}, {0x3708,0x30}, {0x3714,0x24}, {0x3725,0x02}, {0x3737,0x08},
    {0x3739,0x28}, {0x3749,0x32}, {0x374A,0x32}, {0x374B,0x32}, {0x374C,0x32}, {0x374D,0x81},
    {0x374E,0x81}, {0x374F,0x81}, {0x3752,0x36}, {0x3753,0x36}, {0x3754,0x36}, {0x3761,0x00},
    {0x376C,0x81}, {0x3774,0x18}, {0x3776,0x08}, {0x377C,0x81}, {0x377D,0x81}, {0x377E,0x81},
    {0x37A0,0x44}, {0x37A6,0x44}, {0x37AA,0x0D}, {0x37AE,0x00}, {0x37CB,0x03}, {0x37CC,0x01},
    {0x37D8,0x02}, {0x37D9,0x10}, {0x37E1,0x10}, {0x37E2,0x18}, {0x37E3,0x08}, {0x37E4,0x08},
    {0x37E5,0x02}, {0x37E6,0x08}, {0x3800,0x00}, {0x3801,0x00}, {0x3802,0x00}, {0x3803,0x04},
    {0x3804,0x07}, {0x3805,0x8F}, {0x3806,0x04}, {0x3807,0x43}, {0x3808,0x07}, {0x3809,0x80},
    {0x380A,0x04}, {0x380B,0x38}, {0x380C,0x08}, {0x380D,0xE8}, {0x380E,0x04}, {0x380F,0x8C},
    {0x3810,0x00}, {0x3811,0x07}, {0x3812,0x00}, {0x3813,0x04}, {0x3814,0x01}, {0x3815,0x01},
    {0x3816,0x01}, {0x3817,0x01}, {0x3820,0xA0}, {0x3821,0x00}, {0x3822,0x80}, {0x3823,0x08},
    {0x3824,0x00}, {0x3825,0x20}, {0x3826,0x00}, {0x3827,0x08}, {0x382A,0x00}, {0x382B,0x08},
    {0x382D,0x00}, {0x382E,0x00}, {0x382F,0x23}, {0x3834,0x00}, {0x3839,0x00}, {0x383A,0xD1},
    {0x383E,0x03}, {0x393D,0x29}, {0x393F,0x6E}, {0x394B,0x06}, {0x394C,0x06}, {0x394D,0x08},
    {0x394E,0x0A}, {0x394F,0x01}, {0x3950,0x01}, {0x3951,0x01}, {0x3952,0x01}, {0x3953,0x01},
    {0x3954,0x01}, {0x3955,0x01}, {0x3956,0x01}, {0x3957,0x0E}, {0x3958,0x08}, {0x3959,0x08},
    {0x395A,0x08}, {0x395B,0x13}, {0x395C,0x09}, {0x395D,0x05}, {0x395E,0x02}, {0x395F,0x00},
    {0x3960,0x00}, {0x3961,0x00}, {0x3962,0x00}, {0x3963,0x00}, {0x3964,0x00}, {0x3965,0x00},
    {0x3966,0x00}, {0x3967,0x00}, {0x3968,0x01}, {0x3969,0x01}, {0x396A,0x01}, {0x396B,0x01},
    {0x396C,0x10}, {0x396D,0xF0}, {0x396E,0x11}, {0x396F,0x00}, {0x3970,0x37}, {0x3971,0x37},
    {0x3972,0x37}, {0x3973,0x37}, {0x3974,0x00}, {0x3975,0x3C}, {0x3976,0x3C}, {0x3977,0x3C},
    {0x3978,0x3C}, {0x3C00,0x0F}, {0x3C20,0x01}, {0x3C21,0x08}, {0x3F00,0x8B}, {0x3F02,0x0F},
    {0x4000,0xC3}, {0x4001,0xE0}, {0x4002,0x00}, {0x4003,0x40}, {0x4008,0x04}, {0x4009,0x23},
    {0x400A,0x04}, {0x400B,0x01}, {0x4041,0x20}, {0x4077,0x06}, {0x4078,0x00}, {0x4079,0x1A},
    {0x407A,0x7F}, {0x407B,0x01}, {0x4080,0x03}, {0x4081,0x84}, {0x4308,0x03}, {0x4309,0xFF},
    {0x430D,0x00}, {0x4500,0x07}, {0x4501,0x00}, {0x4503,0x00}, {0x450A,0x04}, {0x450E,0x00},
    {0x450F,0x00}, {0x4800,0x64}, {0x4806,0x00}, {0x4813,0x00}, {0x4815,0x40}, {0x4816,0x12},
    {0x481F,0x30}, {0x4837,0x14}, {0x4857,0x05}, {0x4884,0x04}, {0x4900,0x00}, {0x4901,0x00},
    {0x4902,0x01}, {0x4D00,0x03}, {0x4D01,0xD8}, {0x4D02,0xBA}, {0x4D03,0xA0}, {0x4D04,0xB7},
    {0x4D05,0x34}, {0x4D0D,0x00}, {0x5000,0xFD}, {0x5001,0x50}, {0x5006,0x00}, {0x5080,0x40},
    {0x5181,0x2B}, {0x5202,0xA3}, {0x5206,0x01}, {0x5207,0x00}, {0x520A,0x01}, {0x520B,0x00},
    {0x4F00,0x01},
};

// Each pool buffer is a FULL 1920x1080 RGB565 frame = ~4 MB PSRAM. 6 buffers (24 MB) starved the
// PSRAM heap (dropped to ~400 KB) so LVGL had no memory to composite the screen -> BLACK preview,
// and video-recording buffers couldn't allocate. 4 buffers (16 MB) is the proven-working depth and
// leaves ~10 MB headroom for LVGL + the video encoder. Do NOT raise this without checking PSRAM.
#define CAM_NBUF 4
static esp_cam_ctlr_handle_t   s_cam    = NULL;
static isp_proc_handle_t        s_isp    = NULL;
static i2c_master_dev_handle_t  s_dev    = NULL;   // SCCB device @ 0x36
static uint8_t                 *s_fb[CAM_NBUF] = {0};
static volatile int             s_widx   = 0;      // next pool buffer to hand the driver
static uint8_t *volatile        s_latest = NULL;   // last completed frame (what render reads)
static volatile bool            s_have   = false;
static bool                     s_running = false;
static TaskHandle_t             s_task   = NULL;   // consumer: recycles finished frames back into the queue
static QueueHandle_t            s_free_q = NULL;   // ISR -> task: pointers of buffers that just finished DMA

// --- SCCB helpers (16-bit register address, 8-bit data) ---
static esp_err_t sccb_w8(uint16_t reg, uint8_t val) {
    uint8_t b[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    return i2c_master_transmit(s_dev, b, 3, 100);
}
static esp_err_t sccb_r8(uint16_t reg, uint8_t *val) {
    uint8_t ra[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_dev, ra, 2, val, 1, 100);
}

// These run in ISR context — MUST be in IRAM (the esp-idf CSI test marks them IRAM_ATTR).
// A non-IRAM ISR callback faults when the interrupt fires with the flash cache disabled →
// panic in the camera task + capture stalls after the first queued frames (our bug).
static volatile uint32_t s_frames = 0;   // frames DMA'd by the driver (ISR-counted)

// Producer/consumer streaming (the correct esp_cam_ctlr model, learned the hard way):
//   * receive() is a PRODUCER — it only xQueueSend()s a buffer to the driver's fill-queue
//     (esp_cam_ctlr_csi.c:516); it does NOT block for a frame. A receive() *loop* therefore just
//     counted submissions and stalled — never a real fps.
//   * on_trans_finished (ISR) is the CONSUMER notification: the driver hands back the buffer it
//     just filled. We publish it as the newest frame AND push its pointer to s_free_q so the task
//     can recycle it back into the fill-queue. Without recycling, the driver fills each submitted
//     buffer once and starves after CAM_NBUF frames.
// on_get_new_trans is NOT used (handing buffers that way yielded exactly 1 frame on this silicon).
// This runs in ISR context → IRAM_ATTR. Return value is need_yield.
static bool IRAM_ATTR cam_on_finished(esp_cam_ctlr_handle_t h, esp_cam_ctlr_trans_t *trans, void *ud) {
    uint8_t *buf = (uint8_t *)trans->buffer;
    s_latest = buf;
    s_have = true;
    s_frames++;
    BaseType_t hpw = pdFALSE;
    if (s_free_q) xQueueSendFromISR(s_free_q, &buf, &hpw);
    return hpw == pdTRUE;
}

bool nv_camera_running(void) { return s_running; }
uint32_t nv_camera_frames(void) { return s_frames; }

// Consumer: seed the driver's fill-queue with the whole pool, then keep recycling. As each buffer
// finishes (ISR pushes its pointer to s_free_q) we re-submit it via receive() so the queue never
// drains. We HOLD ONE finished buffer back (the current s_latest) so render never reads a buffer
// that's simultaneously being DMA'd into — re-submit the *previous* finished buffer instead.
static void capture_task(void *arg) {
    for (int k = 0; k < CAM_NBUF; k++) {
        esp_cam_ctlr_trans_t t = { .buffer = s_fb[k], .buflen = CAM_FB_LEN };
        esp_cam_ctlr_receive(s_cam, &t, 0);
    }
    uint8_t *hold = NULL;
    while (s_running) {
        uint8_t *done = NULL;
        if (xQueueReceive(s_free_q, &done, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (hold) {   // the frame before the newest one is now safe to reuse
                esp_cam_ctlr_trans_t t = { .buffer = hold, .buflen = CAM_FB_LEN };
                esp_cam_ctlr_receive(s_cam, &t, 0);
            }
            hold = done;  // keep the newest (== s_latest) out of the pool until the next frame lands
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}
void nv_camera_dims(int *w, int *h) { if (w) *w = CAM_W; if (h) *h = CAM_H; }

bool nv_camera_start(void) {
    if (s_running) return true;

    i2c_master_bus_handle_t bus = nv_hal_i2c_bus();
    if (!bus) { NV_LOGW(TAG, "shared I2C bus not up"); return false; }

    // ---- open the sensor on the shared bus + verify chip id ----
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = OV02C10_ADDR,
        .scl_speed_hz    = CAM_SCCB_FREQ,
    };
    if (i2c_master_bus_add_device(bus, &dc, &s_dev) != ESP_OK) {
        NV_LOGW(TAG, "SCCB add device failed");
        return false;
    }
    uint8_t idh = 0, idl = 0;
    sccb_r8(0x300A, &idh);
    sccb_r8(0x300B, &idl);
    uint16_t chip = ((uint16_t)idh << 8) | idl;
    if (chip != OV02C10_CHIP_ID) {
        NV_LOGW(TAG, "unsupported sensor (chip id 0x%04X); expected OV02C10 0x5602", chip);
        goto fail;
    }
    NV_LOGI(TAG, "OV02C10 detected (0x%04X)", chip);

    // ---- MIPI reset preamble (esp_video ov02c10_mipi_reset_regs) ----
    sccb_w8(0x0100, 0x00);              // stream off / sleep
    sccb_w8(0x0103, 0x01);              // software reset
    vTaskDelay(pdMS_TO_TICKS(10));
    sccb_w8(0x4800, 0x01);              // clock-lane disable -> hold D-PHY clock in LP-11
    // ---- program the 1920x1080 2-lane mode ----
    for (size_t i = 0; i < sizeof(s_ov02c10_init) / sizeof(s_ov02c10_init[0]); i++) {
        if (sccb_w8(s_ov02c10_init[i].reg, s_ov02c10_init[i].val) != ESP_OK) {
            NV_LOGE(TAG, "sensor init write failed @ reg 0x%04X", s_ov02c10_init[i].reg);
            goto fail;
        }
    }

    // ---- capture buffer POOL ----
    for (int i = 0; i < CAM_NBUF; i++) {
        s_fb[i] = (uint8_t *)heap_caps_aligned_calloc(64, 1, CAM_FB_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_fb[i]) { NV_LOGE(TAG, "frame buffer %d alloc failed (%u B)", i, (unsigned)CAM_FB_LEN); goto fail; }
    }
    s_widx = 0; s_latest = NULL; s_frames = 0;

    // ---- CSI controller (RAW10 in, RGB565 out via ISP) ----
    esp_cam_ctlr_csi_config_t csi = {
        .ctlr_id                = 0,
        .h_res                  = CAM_W,
        .v_res                  = CAM_H,
        .lane_bit_rate_mbps     = CAM_LANE_MBPS,
        .input_data_color_type  = CAM_CTLR_COLOR_RAW10,
        .output_data_color_type = CAM_CTLR_COLOR_RGB565,   // ISP demosaics RAW10 -> RGB565
        .data_lane_num          = 2,
        .byte_swap_en           = false,
        .queue_items            = CAM_NBUF,   // pool depth; buffers are handed via on_get_new_trans
    };
    if (esp_cam_new_csi_ctlr(&csi, &s_cam) != ESP_OK) { NV_LOGE(TAG, "CSI ctlr init failed"); goto fail; }

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans  = NULL,               // buffers are fed by the task via receive()
        .on_trans_finished = cam_on_finished,    // ISR publishes + recycles the completed frame
    };
    if (esp_cam_ctlr_register_event_callbacks(s_cam, &cbs, NULL) != ESP_OK) {
        NV_LOGE(TAG, "callback register failed"); goto fail;
    }
    if (esp_cam_ctlr_enable(s_cam) != ESP_OK) { NV_LOGE(TAG, "CSI enable failed"); goto fail; }

    // ---- ISP (RAW10 -> RGB565) ----
    esp_isp_processor_cfg_t isp = {
        .clk_src                = ISP_CLK_SRC_PLL240,     // esp_video uses PLL240
        .clk_hz                 = 240 * 1000 * 1000,      // must exceed the sensor pclk (~82 MHz);
                                                          // 80 MHz was BELOW it → ISP never completed
        .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type  = ISP_COLOR_RAW10,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet  = true,    // OV02C10 emits line-sync (reg 0x4800=0x64); match it
        .has_line_end_packet    = true,
        .h_res                  = CAM_W,
        .v_res                  = CAM_H,
        .bayer_order            = COLOR_RAW_ELEMENT_ORDER_GBRG,   // OV02C10 Bayer = GBRG
    };
    if (esp_isp_new_processor(&isp, &s_isp) != ESP_OK) { NV_LOGE(TAG, "ISP init failed"); goto fail; }
    if (esp_isp_enable(s_isp) != ESP_OK) { NV_LOGE(TAG, "ISP enable failed"); goto fail; }

    // Demosaic: the RAW->RGB converter. Must be explicitly configured + enabled — without it the
    // RAW10 pipeline produces NO completed frames (esp_video enables it in its ISP pipeline).
    esp_isp_demosaic_config_t demo = {
        .grad_ratio    = { .integer = 1, .decimal = 0 },
        .padding_mode  = ISP_DEMOSAIC_EDGE_PADDING_MODE_SRND_DATA,
        .padding_data  = 0,
        .padding_line_tail_valid_start_pixel = 0,
        .padding_line_tail_valid_end_pixel   = 0,
    };
    if (esp_isp_demosaic_configure(s_isp, &demo) != ESP_OK) NV_LOGW(TAG, "demosaic configure failed");
    if (esp_isp_demosaic_enable(s_isp) != ESP_OK) NV_LOGW(TAG, "demosaic enable failed");

    if (esp_cam_ctlr_start(s_cam) != ESP_OK) { NV_LOGE(TAG, "CSI start failed"); goto fail; }

    // Sensor stream-on LAST (CSI+ISP are ready to receive). Ref: esp-video-components PR#46.
    sccb_w8(0x0100, 0x01);

    s_have = false;
    s_running = true;
    if (!s_free_q) s_free_q = xQueueCreate(CAM_NBUF + 2, sizeof(uint8_t *));
    if (!s_free_q) { s_running = false; NV_LOGE(TAG, "free queue create failed"); goto fail; }
    if (xTaskCreate(capture_task, "nv_cam", 4096, NULL, 6, &s_task) != pdPASS) {
        s_running = false; NV_LOGE(TAG, "capture task create failed"); goto fail;
    }
    NV_LOGI(TAG, "OV02C10 streaming (%dx%d RGB565, %d-buf pool)", CAM_W, CAM_H, CAM_NBUF);
    return true;

fail:
    nv_camera_stop();
    return false;
}

static void video_release(void);   // defined with the video recorder, below

void nv_camera_stop(void) {
    nv_camera_video_stop();   // flush + close any active recording before tearing the pipeline down
    video_release();
    s_running = false;
    s_have = false;
    // Stop the sensor + controller so no more callbacks fire, THEN free the pool.
    if (s_cam) esp_cam_ctlr_stop(s_cam);       // unblocks the task's receive()
    for (int i = 0; i < 200 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(5));   // join before freeing
    if (s_dev) sccb_w8(0x0100, 0x00);   // stream off (best effort)
    s_latest = NULL;

    if (s_cam) {
        esp_cam_ctlr_disable(s_cam);
        esp_cam_ctlr_del(s_cam);
        s_cam = NULL;
    }
    if (s_isp) {
        esp_isp_disable(s_isp);
        esp_isp_del_processor(s_isp);
        s_isp = NULL;
    }
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    for (int i = 0; i < CAM_NBUF; i++) {
        if (s_fb[i]) { heap_caps_free(s_fb[i]); s_fb[i] = NULL; }
    }
    if (s_free_q) { vQueueDelete(s_free_q); s_free_q = NULL; }
}

bool nv_camera_render(uint8_t *dst, int dst_w, int dst_h) {
    uint8_t *src = s_latest;
    if (!s_have || !src || !dst || dst_w <= 0 || dst_h <= 0) return false;
    static int rc = 0;
    if ((rc++ % 30) == 0) NV_LOGI(TAG, "render: %u frames captured so far", (unsigned)s_frames);
    esp_cache_msync(src, CAM_FB_LEN, ESP_CACHE_MSYNC_FLAG_DIR_M2C);   // buf+size are 64B-aligned

    ppa_client_handle_t cl = NULL;
    ppa_client_config_t ccfg = { .oper_type = PPA_OPERATION_SRM };
    if (ppa_register_client(&ccfg, &cl) != ESP_OK) return false;

    ppa_srm_oper_config_t op = {0};
    op.in.buffer       = src;
    op.in.pic_w        = CAM_W;
    op.in.pic_h        = CAM_H;
    op.in.block_w      = CAM_W;
    op.in.block_h      = CAM_H;
    op.in.srm_cm       = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer      = dst;
    op.out.buffer_size = (uint32_t)dst_w * dst_h * 2;
    op.out.pic_w       = dst_w;
    op.out.pic_h       = dst_h;
    op.out.srm_cm      = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle  = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x         = (float)dst_w / CAM_W;
    op.scale_y         = (float)dst_h / CAM_H;
    op.mode            = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t e = ppa_do_scale_rotate_mirror(cl, &op);
    ppa_unregister_client(cl);
    return e == ESP_OK;
}

bool nv_camera_save_jpeg(const char *path) {
    uint8_t *src = s_latest;
    if (!s_have || !src || !path) return false;
    esp_cache_msync(src, CAM_FB_LEN, ESP_CACHE_MSYNC_FLAG_DIR_M2C);   // buf+size are 64B-aligned

    static jpeg_encoder_handle_t enc = NULL;
    if (!enc) {
        jpeg_encode_engine_cfg_t eng = { .timeout_ms = 400 };
        if (jpeg_new_encoder_engine(&eng, &enc) != ESP_OK) { enc = NULL; return false; }
    }

    jpeg_encode_memory_alloc_cfg_t in_cfg  = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    jpeg_encode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t in_got = 0, out_got = 0;
    uint8_t *in_buf  = (uint8_t *)jpeg_alloc_encoder_mem(CAM_FB_LEN, &in_cfg, &in_got);
    uint8_t *out_buf = (uint8_t *)jpeg_alloc_encoder_mem(CAM_FB_LEN, &out_cfg, &out_got);
    if (!in_buf || !out_buf) {
        if (in_buf) free(in_buf);
        if (out_buf) free(out_buf);
        return false;
    }
    memcpy(in_buf, src, CAM_FB_LEN);

    jpeg_encode_cfg_t cfg = {
        .height        = CAM_H,
        .width         = CAM_W,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = 88,
    };
    uint32_t out_size = 0;
    esp_err_t r = jpeg_encoder_process(enc, &cfg, in_buf, in_got, out_buf, out_got, &out_size);
    bool ok = false;
    if (r == ESP_OK && out_size > 0) {
        FILE *f = fopen(path, "wb");
        if (f) { ok = fwrite(out_buf, 1, out_size, f) == out_size; fclose(f); }
    }
    free(in_buf);
    free(out_buf);
    if (ok) NV_LOGI(TAG, "photo -> %s (%u KB)", path, (unsigned)(out_size / 1024));
    return ok;
}

// ============================ video recorder (MJPEG/AVI + H.264/MP4) ============================
// A dedicated task samples the live frame at a fixed cadence and PPA-downscales it to 1280x720
// RGB565, so the file has a constant frame rate independent of the (variable) capture rate. Two
// output formats, chosen by the file extension in nv_camera_video_start():
//   .avi  -> Motion-JPEG: HW-JPEG-encode each frame, append as a "00dc" chunk, write idx1 on stop.
//            Playable ON-device (HW JPEG decoder) and everywhere (VLC/WMP/browsers). Big files.
//   .mp4  -> H.264: the P4 has a HW H.264 *encoder* (RGB565 input supported directly), muxed into
//            MP4 via nv_mp4. ~10x smaller, plays on PC/phone. NOT playable on-device (the P4 has
//            no HW H.264 *decoder*).
#define VID_W       1280
#define VID_H       720
#define VID_FPS     12
#define VID_FB_LEN  ((size_t)VID_W * VID_H * 2)

// Back-patch offsets into the fixed 224-byte AVI header.
#define AVI_OFF_RIFFSZ    4
#define AVI_OFF_USPF      32
#define AVI_OFF_TOTFR     48
#define AVI_OFF_RATE      132
#define AVI_OFF_LEN       140
#define AVI_OFF_MOVISZ    216
#define AVI_MOVI_DATA     4       // idx offset of the first chunk, relative to the 'movi' fourcc

static FILE                  *s_vid_f     = NULL;
static jpeg_encoder_handle_t  s_vid_enc   = NULL;
static uint8_t               *s_vid_in    = NULL;   // PPA target + JPEG input (RGB565 VID_WxVID_H)
static uint8_t               *s_vid_out   = NULL;   // JPEG output
static size_t                 s_vid_in_cap = 0, s_vid_out_cap = 0;
static uint32_t              *s_vid_idx   = NULL;   // [offset,size] pair per frame, for idx1
static uint32_t               s_vid_idx_cap = 0;
static volatile uint32_t      s_vid_frames = 0;
static uint32_t               s_vid_movi  = 0;      // bytes written after the 'movi' fourcc
static volatile bool          s_vid_run   = false;
static TaskHandle_t           s_vid_task  = NULL;
static uint32_t               s_vid_start = 0;      // tick at start (for elapsed / fps)
static ppa_client_handle_t    s_vid_ppa   = NULL;

// H.264 / MP4 path (selected when the recording path ends in .mp4).
static bool                   s_vid_h264  = false;
static esp_h264_enc_handle_t  s_h264      = NULL;
static uint8_t               *s_h264_out  = NULL;   // encoder output (Annex-B) scratch
static nv_mp4_t              *s_mp4       = NULL;

static void wr32(FILE *f, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) }; fwrite(b, 1, 4, f); }
static void wr16(FILE *f, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v>>8) }; fwrite(b, 1, 2, f); }
static void wtag(FILE *f, const char *t) { fwrite(t, 1, 4, f); }

static void avi_write_header(FILE *f) {
    wtag(f, "RIFF"); wr32(f, 0); wtag(f, "AVI ");
    wtag(f, "LIST"); wr32(f, 192); wtag(f, "hdrl");
      wtag(f, "avih"); wr32(f, 56);
        wr32(f, 1000000u / VID_FPS);   // dwMicroSecPerFrame (patched)
        wr32(f, 0); wr32(f, 0); wr32(f, 0x10 /*HASINDEX*/);
        wr32(f, 0);                    // dwTotalFrames (patched)
        wr32(f, 0); wr32(f, 1); wr32(f, 0);
        wr32(f, VID_W); wr32(f, VID_H);
        wr32(f, 0); wr32(f, 0); wr32(f, 0); wr32(f, 0);
      wtag(f, "LIST"); wr32(f, 116); wtag(f, "strl");
        wtag(f, "strh"); wr32(f, 56);
          wtag(f, "vids"); wtag(f, "MJPG");
          wr32(f, 0); wr16(f, 0); wr16(f, 0); wr32(f, 0);
          wr32(f, 1000);                // dwScale
          wr32(f, 1000u * VID_FPS);     // dwRate (patched)  -> fps = rate/scale
          wr32(f, 0);
          wr32(f, 0);                   // dwLength (patched)
          wr32(f, 0); wr32(f, 0xFFFFFFFF); wr32(f, 0);
          wr16(f, 0); wr16(f, 0); wr16(f, VID_W); wr16(f, VID_H);
        wtag(f, "strf"); wr32(f, 40);
          wr32(f, 40); wr32(f, VID_W); wr32(f, VID_H);
          wr16(f, 1); wr16(f, 24); wtag(f, "MJPG");
          wr32(f, VID_W * VID_H * 3);
          wr32(f, 0); wr32(f, 0); wr32(f, 0); wr32(f, 0);
    wtag(f, "LIST"); wr32(f, 0); wtag(f, "movi");   // movi size patched
}

static bool vid_ensure_idx(void) {
    if ((s_vid_frames * 2 + 2) <= s_vid_idx_cap) return true;
    uint32_t ncap = s_vid_idx_cap ? s_vid_idx_cap * 2 : 8192;
    uint32_t *n = (uint32_t *)heap_caps_realloc(s_vid_idx, ncap * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!n) return false;
    s_vid_idx = n; s_vid_idx_cap = ncap;
    return true;
}

static void avi_write_frame(const uint8_t *jpg, uint32_t len) {
    uint32_t off = AVI_MOVI_DATA + s_vid_movi;    // chunk offset relative to 'movi' fourcc
    wtag(s_vid_f, "00dc"); wr32(s_vid_f, len);
    fwrite(jpg, 1, len, s_vid_f);
    uint32_t pad = len & 1u;
    if (pad) { uint8_t z = 0; fwrite(&z, 1, 1, s_vid_f); }
    if (vid_ensure_idx()) { s_vid_idx[s_vid_frames * 2] = off; s_vid_idx[s_vid_frames * 2 + 1] = len; }
    s_vid_movi += 8 + len + pad;
    s_vid_frames++;
}

static void avi_finalize(FILE *f) {
    // idx1 chunk.
    wtag(f, "idx1"); wr32(f, s_vid_frames * 16);
    for (uint32_t i = 0; i < s_vid_frames; i++) {
        wtag(f, "00dc"); wr32(f, 0x10 /*AVIIF_KEYFRAME*/);
        wr32(f, s_vid_idx[i * 2]); wr32(f, s_vid_idx[i * 2 + 1]);
    }
    long filesize = ftell(f);
    uint32_t elapsed_ms = (xTaskGetTickCount() - s_vid_start) * portTICK_PERIOD_MS;
    if (elapsed_ms < 1) elapsed_ms = 1;
    uint32_t frames = s_vid_frames ? s_vid_frames : 1;
    // Back-patch the fields we only know at the end.
    fseek(f, AVI_OFF_RIFFSZ, SEEK_SET); wr32(f, (uint32_t)(filesize - 8));
    fseek(f, AVI_OFF_MOVISZ, SEEK_SET); wr32(f, AVI_MOVI_DATA + s_vid_movi);
    fseek(f, AVI_OFF_TOTFR,  SEEK_SET); wr32(f, s_vid_frames);
    fseek(f, AVI_OFF_LEN,    SEEK_SET); wr32(f, s_vid_frames);
    fseek(f, AVI_OFF_USPF,   SEEK_SET); wr32(f, (uint32_t)((uint64_t)elapsed_ms * 1000ull / frames));
    fseek(f, AVI_OFF_RATE,   SEEK_SET); wr32(f, (uint32_t)((uint64_t)frames * 1000000ull / elapsed_ms));
}

static void video_task(void *arg) {
    const TickType_t period = pdMS_TO_TICKS(1000 / VID_FPS);
    TickType_t next = xTaskGetTickCount();
    while (s_vid_run) {
        vTaskDelayUntil(&next, period);
        uint8_t *src = s_latest;
        if (!s_have || !src || !s_vid_f) continue;
        esp_cache_msync(src, CAM_FB_LEN, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        // Downscale the full frame to 1280x720 straight into the JPEG input buffer.
        ppa_srm_oper_config_t op = {0};
        op.in.buffer = src; op.in.pic_w = CAM_W; op.in.pic_h = CAM_H;
        op.in.block_w = CAM_W; op.in.block_h = CAM_H; op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.out.buffer = s_vid_in; op.out.buffer_size = (uint32_t)VID_FB_LEN;
        op.out.pic_w = VID_W; op.out.pic_h = VID_H; op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.scale_x = (float)VID_W / CAM_W; op.scale_y = (float)VID_H / CAM_H;
        op.mode = PPA_TRANS_MODE_BLOCKING;
        if (ppa_do_scale_rotate_mirror(s_vid_ppa, &op) != ESP_OK) continue;

        if (s_vid_h264) {
            // HW H.264 encode the RGB565 frame -> Annex-B -> MP4 sample.
            esp_h264_enc_in_frame_t  in  = { .raw_data = { .buffer = s_vid_in, .len = (uint32_t)VID_FB_LEN },
                                             .pts = s_vid_frames };
            esp_h264_enc_out_frame_t out = { .raw_data = { .buffer = s_h264_out, .len = (uint32_t)VID_FB_LEN } };
            if (esp_h264_enc_process(s_h264, &in, &out) == ESP_H264_ERR_OK && out.length > 0) {
                const bool key = (out.frame_type == ESP_H264_FRAME_TYPE_IDR);
                if (nv_mp4_write(s_mp4, out.raw_data.buffer, out.length, key)) s_vid_frames++;
            }
            continue;
        }

        jpeg_encode_cfg_t cfg = {
            .height = VID_H, .width = VID_W,
            .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
            .sub_sample = JPEG_DOWN_SAMPLING_YUV420, .image_quality = 80,
        };
        uint32_t out_size = 0;
        if (jpeg_encoder_process(s_vid_enc, &cfg, s_vid_in, s_vid_in_cap,
                                 s_vid_out, s_vid_out_cap, &out_size) == ESP_OK && out_size > 0)
            avi_write_frame(s_vid_out, out_size);
    }
    s_vid_task = NULL;
    vTaskDelete(NULL);
}

bool nv_camera_video_recording(void) { return s_vid_run; }
uint32_t nv_camera_video_secs(void) {
    if (!s_vid_run) return 0;
    return (uint32_t)(((xTaskGetTickCount() - s_vid_start) * portTICK_PERIOD_MS) / 1000);
}

bool nv_camera_video_start(const char *path) {
    if (!s_running || !path) return false;
    if (s_vid_run) return true;

    const char *ext = strrchr(path, '.');
    s_vid_h264 = (ext && strcasecmp(ext, ".mp4") == 0);

    // Shared front-end: the PPA downscale target (RGB565 1280x720) + a PPA client. s_vid_in is
    // DMA-capable/aligned (jpeg allocator) and feeds either encoder.
    if (!s_vid_in) {
        jpeg_encode_memory_alloc_cfg_t ic = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
        s_vid_in = (uint8_t *)jpeg_alloc_encoder_mem(VID_FB_LEN, &ic, &s_vid_in_cap);
    }
    if (!s_vid_ppa) {
        ppa_client_config_t pc = { .oper_type = PPA_OPERATION_SRM };
        if (ppa_register_client(&pc, &s_vid_ppa) != ESP_OK) s_vid_ppa = NULL;
    }
    if (!s_vid_in || !s_vid_ppa) { NV_LOGE(TAG, "video alloc failed"); return false; }

    s_vid_frames = 0; s_vid_movi = 0;

    if (s_vid_h264) {
        if (!s_h264) {
            esp_h264_enc_cfg_hw_t cfg = {
                .pic_type = ESP_H264_RAW_FMT_RGB565_LE,
                .gop = VID_FPS, .fps = VID_FPS,
                .res = { .width = VID_W, .height = VID_H },
                .rc  = { .bitrate = 2 * 1024 * 1024, .qp_min = 25, .qp_max = 45 },
            };
            if (esp_h264_enc_hw_new(&cfg, &s_h264) != ESP_H264_ERR_OK) { s_h264 = NULL; NV_LOGE(TAG, "h264 new failed"); return false; }
            if (esp_h264_enc_open(s_h264) != ESP_H264_ERR_OK) { esp_h264_enc_del(s_h264); s_h264 = NULL; NV_LOGE(TAG, "h264 open failed"); return false; }
        }
        if (!s_h264_out) s_h264_out = (uint8_t *)heap_caps_malloc(VID_FB_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_h264_out) { NV_LOGE(TAG, "h264 out alloc failed"); return false; }
        s_mp4 = nv_mp4_open(path, VID_W, VID_H, VID_FPS);
        if (!s_mp4) { NV_LOGE(TAG, "mp4 open failed: %s", path); return false; }
    } else {
        if (!s_vid_enc) {
            jpeg_encode_engine_cfg_t eng = { .timeout_ms = 1000 };
            if (jpeg_new_encoder_engine(&eng, &s_vid_enc) != ESP_OK) { s_vid_enc = NULL; return false; }
        }
        if (!s_vid_out) {
            jpeg_encode_memory_alloc_cfg_t oc = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
            s_vid_out = (uint8_t *)jpeg_alloc_encoder_mem(VID_FB_LEN, &oc, &s_vid_out_cap);
        }
        if (!s_vid_out) { NV_LOGE(TAG, "video alloc failed"); return false; }
        s_vid_f = fopen(path, "wb");
        if (!s_vid_f) { NV_LOGE(TAG, "video fopen failed: %s", path); return false; }
        avi_write_header(s_vid_f);
    }

    s_vid_start = xTaskGetTickCount();
    s_vid_run = true;
    if (xTaskCreate(video_task, "nv_vid", 6144, NULL, 5, &s_vid_task) != pdPASS) {
        s_vid_run = false;
        if (s_vid_f) { fclose(s_vid_f); s_vid_f = NULL; }
        if (s_mp4)   { nv_mp4_close(s_mp4); s_mp4 = NULL; }
        NV_LOGE(TAG, "video task create failed"); return false;
    }
    NV_LOGI(TAG, "video REC start (%s) -> %s", s_vid_h264 ? "H.264/MP4" : "MJPEG/AVI", path);
    return true;
}

void nv_camera_video_stop(void) {
    if (!s_vid_run) return;
    s_vid_run = false;
    for (int i = 0; i < 300 && s_vid_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
    if (s_vid_h264) {
        if (s_mp4) {
            long sz = nv_mp4_close(s_mp4); s_mp4 = NULL;
            NV_LOGI(TAG, "video REC stop (MP4): %u frames, %ld KB", (unsigned)s_vid_frames, sz / 1024);
        }
    } else if (s_vid_f) {
        avi_finalize(s_vid_f);
        long sz = ftell(s_vid_f);
        fclose(s_vid_f); s_vid_f = NULL;
        NV_LOGI(TAG, "video REC stop (AVI): %u frames, %ld KB", (unsigned)s_vid_frames, sz / 1024);
    }
}

// Release the video encoder/buffers (called from nv_camera_stop after recording is stopped).
static void video_release(void) {
    if (s_vid_enc) { jpeg_del_encoder_engine(s_vid_enc); s_vid_enc = NULL; }
    if (s_vid_in)  { free(s_vid_in);  s_vid_in = NULL;  s_vid_in_cap = 0; }
    if (s_vid_out) { free(s_vid_out); s_vid_out = NULL; s_vid_out_cap = 0; }
    if (s_vid_ppa) { ppa_unregister_client(s_vid_ppa); s_vid_ppa = NULL; }
    if (s_vid_idx) { heap_caps_free(s_vid_idx); s_vid_idx = NULL; s_vid_idx_cap = 0; }
    if (s_mp4)     { nv_mp4_close(s_mp4); s_mp4 = NULL; }
    if (s_h264)    { esp_h264_enc_close(s_h264); esp_h264_enc_del(s_h264); s_h264 = NULL; }
    if (s_h264_out){ heap_caps_free(s_h264_out); s_h264_out = NULL; }
}
