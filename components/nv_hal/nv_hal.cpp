// nv_hal — JD9165 MIPI-DSI display + GT911 touch + LEDC backlight bring-up.
// Panel config + init sequence verified working on this exact board (Guition JC1060P470/P420).
#include "nv_hal.h"
#include "nv_pins.h"
#include "nv_log.h"

#include "driver/ledc.h"
#include "driver/i2c_master.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_jd9165.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include <cstdio>
#include <cstring>

static const char *TAG = "hal";

static lv_display_t *s_disp = nullptr;
static esp_ldo_channel_handle_t s_ldo = nullptr;
static esp_lcd_dsi_bus_handle_t s_dsi_bus = nullptr;
static i2c_master_bus_handle_t s_i2c_bus = nullptr;  // shared internal I2C (touch + RTC + codecs)
static esp_lcd_panel_handle_t s_panel = nullptr;     // raw JD9165 panel (direct-draw paths)
static esp_lcd_touch_handle_t s_touch = nullptr;     // raw GT911 (direct-read paths)
static lv_indev_t *s_touch_indev = nullptr;          // pointer indev fed by the poll task

// Decoupled touch: a dedicated high-prio task does the blocking I2C read at 60 Hz and
// caches the latest point here. LVGL's read_cb only copies this cache (no I2C inside the
// render lock), so a heavy software redraw can't starve touch sampling and the LVGL task
// holds its lock for a much shorter window each frame.
static portMUX_TYPE s_tp_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int16_t s_tp_x = 0;               // finger 0 — the LVGL primary pointer
static volatile int16_t s_tp_y = 0;
static volatile bool    s_tp_pressed = false;
// Full multi-touch snapshot (all active fingers, panel coords) for nv_hal_touch_points().
static volatile int16_t s_tp_mx[NV_TOUCH_MAX] = {};
static volatile int16_t s_tp_my[NV_TOUCH_MAX] = {};
static volatile uint8_t s_tp_cnt = 0;

// --- JD9165 init sequence (from ESP32-P4 Function EV Board; verified on this panel) ---
static const jd9165_lcd_init_cmd_t s_jd9165_init[] = {
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0xF7, (uint8_t[]){0x49, 0x61, 0x02, 0x00}, 4, 0},
    {0x30, (uint8_t[]){0x01}, 1, 0},
    {0x04, (uint8_t[]){0x0C}, 1, 0},
    {0x05, (uint8_t[]){0x00}, 1, 0},
    {0x06, (uint8_t[]){0x00}, 1, 0},
    {0x0B, (uint8_t[]){0x11}, 1, 0},
    {0x17, (uint8_t[]){0x00}, 1, 0},
    {0x20, (uint8_t[]){0x04}, 1, 0},
    {0x1F, (uint8_t[]){0x05}, 1, 0},
    {0x23, (uint8_t[]){0x00}, 1, 0},
    {0x25, (uint8_t[]){0x19}, 1, 0},
    {0x28, (uint8_t[]){0x18}, 1, 0},
    {0x29, (uint8_t[]){0x04}, 1, 0},
    {0x2A, (uint8_t[]){0x01}, 1, 0},
    {0x2B, (uint8_t[]){0x04}, 1, 0},
    {0x2C, (uint8_t[]){0x01}, 1, 0},
    {0x30, (uint8_t[]){0x02}, 1, 0},
    {0x01, (uint8_t[]){0x22}, 1, 0},
    {0x03, (uint8_t[]){0x12}, 1, 0},
    {0x04, (uint8_t[]){0x00}, 1, 0},
    {0x05, (uint8_t[]){0x64}, 1, 0},
    {0x0A, (uint8_t[]){0x08}, 1, 0},
    {0x0B, (uint8_t[]){0x0A, 0x1A, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x08, 0x1F, 0x1D}, 11, 0},
    {0x0C, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0D, (uint8_t[]){0x16, 0x1B, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x09, 0x1E, 0x1C}, 11, 0},
    {0x0E, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0F, (uint8_t[]){0x16, 0x1B, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1C, 0x1E, 0x09, 0x07}, 11, 0},
    {0x10, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x11, (uint8_t[]){0x0A, 0x1A, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1D, 0x1F, 0x08, 0x06}, 11, 0},
    {0x12, (uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x14, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0x18, (uint8_t[]){0x99}, 1, 0},
    {0x30, (uint8_t[]){0x06}, 1, 0},
    {0x12, (uint8_t[]){0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 14, 0},
    {0x13, (uint8_t[]){0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 14, 0},
    {0x30, (uint8_t[]){0x0A}, 1, 0},
    {0x02, (uint8_t[]){0x4F}, 1, 0},
    {0x0B, (uint8_t[]){0x40}, 1, 0},
    {0x12, (uint8_t[]){0x3E}, 1, 0},
    {0x13, (uint8_t[]){0x78}, 1, 0},
    {0x30, (uint8_t[]){0x0D}, 1, 0},
    {0x0D, (uint8_t[]){0x04}, 1, 0},
    {0x10, (uint8_t[]){0x0C}, 1, 0},
    {0x11, (uint8_t[]){0x0C}, 1, 0},
    {0x12, (uint8_t[]){0x0C}, 1, 0},
    {0x13, (uint8_t[]){0x0C}, 1, 0},
    {0x30, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 20},
};

// ---------------------------------------------------------------- backlight
void nv_hal_backlight_set(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const uint32_t duty = (uint32_t)((255 * percent) / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void backlight_init(void) {
    ledc_timer_config_t t = {};
    t.speed_mode = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_8_BIT;
    t.timer_num = LEDC_TIMER_0;
    t.freq_hz = 5000;
    t.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&t);

    ledc_channel_config_t c = {};
    c.gpio_num = NV_PIN_LCD_BACKLIGHT;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel = LEDC_CHANNEL_0;
    c.timer_sel = LEDC_TIMER_0;
    c.duty = 0;
    c.hpoint = 0;
    ledc_channel_config(&c);
    NV_LOGI(TAG, "backlight (LEDC GPIO%d) ready", NV_PIN_LCD_BACKLIGHT);
}

// ---------------------------------------------------------------- display
static esp_lcd_panel_handle_t display_init(esp_lcd_panel_io_handle_t *out_io) {
    // 1. MIPI DSI PHY power (LDO_VO3)
    esp_ldo_channel_config_t ldo = {};
    ldo.chan_id = NV_MIPI_LDO_CHAN;
    ldo.voltage_mv = NV_MIPI_LDO_MV;
    if (esp_ldo_acquire_channel(&ldo, &s_ldo) != ESP_OK) {
        NV_LOGE(TAG, "LDO acquire failed");
        return nullptr;
    }
    NV_LOGI(TAG, "MIPI DSI PHY powered");

    // 2. DSI bus
    esp_lcd_dsi_bus_config_t bus = {};
    bus.bus_id = 0;
    bus.num_data_lanes = NV_MIPI_DSI_LANES;
    bus.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus.lane_bit_rate_mbps = NV_MIPI_LANE_MBPS;
    if (esp_lcd_new_dsi_bus(&bus, &s_dsi_bus) != ESP_OK) {
        NV_LOGE(TAG, "DSI bus create failed");
        return nullptr;
    }

    // 3. DBI IO (commands)
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_dbi_io_config_t dbi = JD9165_PANEL_IO_DBI_CONFIG();
    if (esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi, &io) != ESP_OK) {
        NV_LOGE(TAG, "DBI IO create failed");
        return nullptr;
    }

    // 4. DPI video timings (1024x600)
    esp_lcd_dpi_panel_config_t dpi = {};
    dpi.virtual_channel = 0;
    dpi.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi.dpi_clock_freq_mhz = NV_DPI_CLK_MHZ;
    dpi.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
    dpi.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi.out_color_format = LCD_COLOR_FMT_RGB565;
    dpi.num_fbs = 1;
    dpi.video_timing.h_size = NV_LCD_H_RES;
    dpi.video_timing.v_size = NV_LCD_V_RES;
    dpi.video_timing.hsync_pulse_width = 20;
    dpi.video_timing.hsync_back_porch = 160;
    dpi.video_timing.hsync_front_porch = 160;
    dpi.video_timing.vsync_pulse_width = 2;
    dpi.video_timing.vsync_back_porch = 21;
    dpi.video_timing.vsync_front_porch = 12;
    dpi.flags.use_dma2d = 1;

    jd9165_vendor_config_t vendor = {};
    vendor.init_cmds = s_jd9165_init;
    vendor.init_cmds_size = sizeof(s_jd9165_init) / sizeof(s_jd9165_init[0]);
    vendor.mipi_config.dsi_bus = s_dsi_bus;
    vendor.mipi_config.dpi_config = &dpi;

    esp_lcd_panel_dev_config_t pcfg = {};
    pcfg.reset_gpio_num = NV_PIN_LCD_RESET;
    pcfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    pcfg.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    pcfg.bits_per_pixel = 16;
    pcfg.flags.reset_active_high = 0;
    pcfg.vendor_config = &vendor;

    esp_lcd_panel_handle_t panel = nullptr;
    if (esp_lcd_new_panel_jd9165(io, &pcfg, &panel) != ESP_OK) {
        NV_LOGE(TAG, "JD9165 panel create failed");
        return nullptr;
    }
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    NV_LOGI(TAG, "JD9165 panel up (%dx%d)", NV_LCD_H_RES, NV_LCD_V_RES);

    *out_io = io;
    return panel;
}

// ---------------------------------------------------------------- touch
static esp_lcd_touch_handle_t touch_init(void) {
    i2c_master_bus_config_t bcfg = {};
    bcfg.i2c_port = I2C_NUM_0;
    bcfg.sda_io_num = (gpio_num_t)NV_PIN_I2C_SDA;
    bcfg.scl_io_num = (gpio_num_t)NV_PIN_I2C_SCL;
    bcfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bcfg.glitch_ignore_cnt = 7;
    bcfg.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&bcfg, &bus) != ESP_OK) {
        NV_LOGE(TAG, "I2C bus create failed");
        return nullptr;
    }
    s_i2c_bus = bus;   // publish for other on-bus peripherals (RX8130 RTC, codecs)

    esp_lcd_panel_io_handle_t tio = nullptr;
    esp_lcd_panel_io_i2c_config_t iocfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    iocfg.dev_addr = NV_GT911_ADDR;  // force primary (avoid 0x14 = RTC)
    iocfg.scl_speed_hz = NV_I2C_HZ;
    if (esp_lcd_new_panel_io_i2c_v2(bus, &iocfg, &tio) != ESP_OK) {
        NV_LOGE(TAG, "GT911 IO create failed");
        return nullptr;
    }

    esp_lcd_touch_config_t tcfg = {};
    tcfg.x_max = NV_LCD_H_RES;
    tcfg.y_max = NV_LCD_V_RES;
    // On this board the GT911 RST/INT are NOT wired to the P4 (verified: Tactility uses NC).
    // Driving GPIO21/22 as reset/int makes the driver run a bogus reset -> wrong address ->
    // read fails. Leave them unconnected so the driver just talks to the GT911 at 0x5D.
    tcfg.rst_gpio_num = GPIO_NUM_NC;
    tcfg.int_gpio_num = GPIO_NUM_NC;
    tcfg.levels.reset = 0;
    tcfg.levels.interrupt = 0;
    tcfg.flags.swap_xy = 0;
    tcfg.flags.mirror_x = 0;
    tcfg.flags.mirror_y = 0;

    esp_lcd_touch_handle_t touch = nullptr;
    if (esp_lcd_touch_new_i2c_gt911(tio, &tcfg, &touch) != ESP_OK) {
        NV_LOGE(TAG, "GT911 touch create failed");
        return nullptr;
    }
    NV_LOGI(TAG, "GT911 touch up (addr 0x%02X)", NV_GT911_ADDR);
    return touch;
}

// LVGL read callback — runs inside the LVGL task. No I2C here: just copy the cache the
// poll task keeps warm. Keeps the per-frame LVGL lock hold as short as possible.
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    portENTER_CRITICAL(&s_tp_mux);
    bool pressed = s_tp_pressed;
    int16_t x = s_tp_x;
    int16_t y = s_tp_y;
    portEXIT_CRITICAL(&s_tp_mux);
    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Dedicated poll task — the only place the GT911 I2C bus is touched for input. Runs at a
// priority above the LVGL task so a busy renderer can't delay sampling. A transient I2C
// NAK is skipped, not fatal (never ESP_ERROR_CHECK -> abort/reboot on a glitchy read).
static void touch_task(void *arg) {
    esp_lcd_touch_handle_t touch = (esp_lcd_touch_handle_t)arg;
    const TickType_t period = pdMS_TO_TICKS(16);  // ~60 Hz
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        esp_lcd_touch_point_data_t pts[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {};
        uint8_t cnt = 0;
        if (esp_lcd_touch_read_data(touch) == ESP_OK &&
            esp_lcd_touch_get_data(touch, pts, &cnt, CONFIG_ESP_LCD_TOUCH_MAX_POINTS) == ESP_OK) {
            portENTER_CRITICAL(&s_tp_mux);
            uint8_t m = cnt < NV_TOUCH_MAX ? cnt : NV_TOUCH_MAX;
            for (uint8_t i = 0; i < m; i++) { s_tp_mx[i] = (int16_t)pts[i].x; s_tp_my[i] = (int16_t)pts[i].y; }
            s_tp_cnt = m;
            if (cnt > 0) {
                s_tp_x = (int16_t)pts[0].x;
                s_tp_y = (int16_t)pts[0].y;
                s_tp_pressed = true;
            } else {
                s_tp_pressed = false;
            }
            portEXIT_CRITICAL(&s_tp_mux);
        }
        vTaskDelayUntil(&last, period);
    }
}

// ---------------------------------------------------------------- init
bool nv_hal_init(void) {
    backlight_init();

    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = display_init(&panel_io);
    if (!panel) return false;

    // LVGL port. Bump the task stack well above TJPGD's 4 KB decode work-buffer (allocated on
    // the stack in lv_tjpgd's decoder_info/open) so image probes/decodes can't overflow it.
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 12288;
    // Pin the LVGL task to core 1 and raise its priority above the background daemons so a
    // busy WiFi/esp_hosted stack (core 0) can't CPU-starve rendering + input dispatch.
    port_cfg.task_priority = 6;
    port_cfg.task_affinity = 1;
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        NV_LOGE(TAG, "lvgl_port_init failed");
        return false;
    }

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = panel_io;
    disp_cfg.panel_handle = panel;
    disp_cfg.buffer_size = NV_LCD_H_RES * NV_LCD_V_RES / 4;   // larger partial buffer: fewer
                                                             // flushes + PPA rotations per frame
                                                             // (smoother scroll/anim); PSRAM has room
    disp_cfg.double_buffer = true;
    disp_cfg.hres = NV_LCD_H_RES;
    disp_cfg.vres = NV_LCD_V_RES;
    disp_cfg.monochrome = false;
    disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.buff_spiram = 1;  // big draw buffers in PSRAM
    // Runtime orientation: lets lv_display_set_rotation() work at runtime. With
    // CONFIG_LVGL_PORT_ENABLE_PPA the P4's PPA rotates flushed regions in hardware;
    // the port allocates one extra rotation buffer (same size/caps as a draw buffer).
    disp_cfg.flags.sw_rotate = 1;

    lvgl_port_display_dsi_cfg_t dsi_cfg = {};
    dsi_cfg.flags.avoid_tearing = 0;

    s_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!s_disp) {
        NV_LOGE(TAG, "lvgl_port_add_disp_dsi failed");
        return false;
    }

    s_panel = panel;

    esp_lcd_touch_handle_t touch = touch_init();
    s_touch = touch;
    if (touch) {
        // Own the indev instead of lvgl_port_add_touch: read_cb copies the cache the poll
        // task fills, and a 16 ms read timer samples LVGL at 60 Hz (default was 33 ms/30 Hz).
        lvgl_port_lock(0);
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_read_cb);
        lv_indev_set_disp(indev, s_disp);
        lv_timer_set_period(lv_indev_get_read_timer(indev), 16);
        s_touch_indev = indev;
        lvgl_port_unlock();

        // Poll task: prio 7 (above the LVGL task's 6) so touch sampling preempts a redraw
        // instead of waiting on it; pinned to core 1 alongside LVGL, leaving core 0 for WiFi.
        xTaskCreatePinnedToCore(touch_task, "nv_touch", 4096, touch,
                                7, nullptr, 1);
    } else {
        NV_LOGW(TAG, "continuing without touch");
    }

    nv_hal_backlight_set(90);
    NV_LOGI(TAG, "HAL ready: display + touch + backlight");
    return true;
}

lv_display_t *nv_hal_display(void) { return s_disp; }

void *nv_hal_panel(void) { return s_panel; }

void *nv_hal_touch(void) { return s_touch; }

// Multi-touch snapshot (see nv_hal.h). Copies the cache the 60 Hz poll task keeps warm.
int nv_hal_touch_points(int16_t *xs, int16_t *ys, int max) {
    if (max <= 0) return 0;
    portENTER_CRITICAL(&s_tp_mux);
    int n = s_tp_cnt;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) { if (xs) xs[i] = s_tp_mx[i]; if (ys) ys[i] = s_tp_my[i]; }
    portEXIT_CRITICAL(&s_tp_mux);
    return n;
}

// ---------------------------------------------------------------- screenshot (HW JPEG)
bool nv_hal_screenshot(const char *path) {
    if (!s_panel || !path) return false;

    void *fb = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb) != ESP_OK || !fb) {
        NV_LOGE(TAG, "screenshot: no framebuffer");
        return false;
    }

    const int    W       = NV_LCD_H_RES;
    const int    Vpad    = (NV_LCD_V_RES + 15) & ~15;      // JPEG YUV420 needs height %16 (600 -> 608)
    const size_t raw     = (size_t)W * NV_LCD_V_RES * 2;   // real framebuffer bytes (RGB565)
    const size_t enc_raw = (size_t)W * Vpad * 2;           // padded input the encoder actually reads
                                                           // (encoding 600 read 8 rows PAST the FB =
                                                           // the garbage/noise band the user saw)

    // The DPI framebuffer lives in PSRAM and is continuously scanned by DMA; invalidate the
    // CPU cache view so the copy below sees the latest composited pixels.
    esp_cache_msync(fb, raw, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    // JPEG encoder engine (lazy, kept for later screenshots).
    static jpeg_encoder_handle_t enc = nullptr;
    if (!enc) {
        jpeg_encode_engine_cfg_t eng = {};
        eng.timeout_ms = 300;
        if (jpeg_new_encoder_engine(&eng, &enc) != ESP_OK) {
            enc = nullptr;
            NV_LOGE(TAG, "screenshot: encoder unavailable");
            return false;
        }
    }

    // DMA-capable, aligned input + output scratch (freed before return).
    jpeg_encode_memory_alloc_cfg_t in_cfg = {};
    in_cfg.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
    size_t in_got = 0;
    uint8_t *in_buf = (uint8_t *)jpeg_alloc_encoder_mem(enc_raw, &in_cfg, &in_got);
    jpeg_encode_memory_alloc_cfg_t out_cfg = {};
    out_cfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
    size_t out_got = 0;
    uint8_t *out_buf = (uint8_t *)jpeg_alloc_encoder_mem(enc_raw, &out_cfg, &out_got);
    if (!in_buf || !out_buf) {
        if (in_buf) free(in_buf);
        if (out_buf) free(out_buf);
        NV_LOGE(TAG, "screenshot: scratch alloc failed");
        return false;
    }
    memset(in_buf, 0, in_got);   // zero the padding rows so they encode as clean black, not garbage
    memcpy(in_buf, fb, raw);     // the real 600 framebuffer rows

    jpeg_encode_cfg_t cfg = {};
    cfg.width = W;
    cfg.height = NV_LCD_V_RES;   // output the true 600 rows; the encoder reads the 16-aligned 608-row
                                 // padded input above, so its internal MCU alignment finds clean black
                                 // (not garbage past the FB) and the JPEG is a clean 1024x600.
    cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
    cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
    cfg.image_quality = 85;

    uint32_t out_size = 0;
    esp_err_t r = jpeg_encoder_process(enc, &cfg, in_buf, in_got, out_buf, out_got, &out_size);
    bool ok = false;
    if (r == ESP_OK && out_size > 0) {
        FILE *f = fopen(path, "wb");
        if (f) {
            ok = fwrite(out_buf, 1, out_size, f) == out_size;
            fclose(f);
        }
        if (ok) NV_LOGI(TAG, "screenshot -> %s (%u KB)", path, (unsigned)(out_size / 1024));
        else    NV_LOGE(TAG, "screenshot: write failed (%s)", path);
    } else {
        NV_LOGE(TAG, "screenshot: encode failed (0x%x)", r);
    }

    free(in_buf);
    free(out_buf);
    return ok;
}

// ---------------------------------------------------------------- thumbnail (PPA downscale)
// PPA-scale the panel framebuffer down to dw x dh and write it as raw RGB565 (no header) to
// `path`. Cheap for the Recents cards: the reader displays it directly as an LVGL RGB565 image
// (no JPEG decode). Best-effort; returns false on any failure (caller falls back to the icon).
bool nv_hal_thumbnail(const char *path, int dw, int dh) {
    if (!s_panel || !path || dw <= 0 || dh <= 0) return false;

    void *fb = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb) != ESP_OK || !fb) return false;
    const size_t raw = (size_t)NV_LCD_H_RES * NV_LCD_V_RES * 2;
    esp_cache_msync(fb, raw, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    const size_t dst_len = (size_t)dw * dh * 2;
    uint8_t *dst = (uint8_t *)heap_caps_aligned_calloc(64, 1, dst_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dst) return false;

    ppa_client_handle_t cl = nullptr;
    ppa_client_config_t ccfg = {};
    ccfg.oper_type = PPA_OPERATION_SRM;
    if (ppa_register_client(&ccfg, &cl) != ESP_OK) {
        heap_caps_free(dst);
        return false;
    }
    ppa_srm_oper_config_t op = {};
    op.in.buffer       = fb;
    op.in.pic_w        = NV_LCD_H_RES;
    op.in.pic_h        = NV_LCD_V_RES;
    op.in.block_w      = NV_LCD_H_RES;
    op.in.block_h      = NV_LCD_V_RES;
    op.in.srm_cm       = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer      = dst;
    op.out.buffer_size = dst_len;
    op.out.pic_w       = dw;
    op.out.pic_h       = dh;
    op.out.srm_cm      = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle  = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x         = (float)dw / NV_LCD_H_RES;
    op.scale_y         = (float)dh / NV_LCD_V_RES;
    op.mode            = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t e = ppa_do_scale_rotate_mirror(cl, &op);
    ppa_unregister_client(cl);

    bool ok = false;
    if (e == ESP_OK) {
        FILE *f = fopen(path, "wb");
        if (f) {
            ok = fwrite(dst, 1, dst_len, f) == dst_len;
            fclose(f);
        }
    }
    heap_caps_free(dst);
    return ok;
}

// ---------------------------------------------------------------- direct-to-panel video blit
static ppa_client_handle_t s_vblit_ppa = nullptr;   // cached SRM client (registered once)
bool nv_hal_video_blit(const void *src, int sw, int sh, int dx, int dy, int dw, int dh, bool clear_bars) {
    if (!s_panel || !src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return false;
    void *fb = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(s_panel, 1, &fb) != ESP_OK || !fb) return false;

    // clamp the destination rect to the panel, force even extents (YUV/2-px PPA rule friendliness)
    if (dx < 0) dx = 0; if (dy < 0) dy = 0;
    if (dx + dw > NV_LCD_H_RES) dw = NV_LCD_H_RES - dx;
    if (dy + dh > NV_LCD_V_RES) dh = NV_LCD_V_RES - dy;
    dx &= ~1; dy &= ~1; dw &= ~1; dh &= ~1;
    if (dw < 2 || dh < 2) return false;

    // letterbox: preserve aspect, centre inside the rect
    float sc = (float)dw / sw; { float sy = (float)dh / sh; if (sy < sc) sc = sy; }
    int tw = ((int)(sw * sc)) & ~1; if (tw < 2) tw = 2; if (tw > dw) tw = dw;
    int th = ((int)(sh * sc)) & ~1; if (th < 2) th = 2; if (th > dh) th = dh;
    int ox = (dx + (dw - tw) / 2) & ~1;
    int oy = (dy + (dh - th) / 2) & ~1;

    const size_t fbsz = (size_t)NV_LCD_H_RES * NV_LCD_V_RES * 2;
    if (clear_bars) {   // black ONLY the letterbox margins, never the picture area — so this is safe to
        uint16_t *p = (uint16_t *)fb;   // run every frame (no black flash on the video) while still
        for (int y = dy; y < dy + dh; y++) {   // wiping any ghost that bled into the bars.
            uint16_t *row = &p[(size_t)y * NV_LCD_H_RES];
            if (y < oy || y >= oy + th) {                       // full-width bar above/below the video
                memset(&row[dx], 0, (size_t)dw * 2);
            } else {                                            // side pillars beside the video
                if (ox > dx)             memset(&row[dx], 0, (size_t)(ox - dx) * 2);
                if (ox + tw < dx + dw)   memset(&row[ox + tw], 0, (size_t)(dx + dw - (ox + tw)) * 2);
            }
        }
        esp_cache_msync(fb, fbsz, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    if (!s_vblit_ppa) {
        ppa_client_config_t c = {};
        c.oper_type = PPA_OPERATION_SRM;
        if (ppa_register_client(&c, &s_vblit_ppa) != ESP_OK) { s_vblit_ppa = nullptr; return false; }
    }
    esp_cache_msync((void *)src, (size_t)sw * sh * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

    ppa_srm_oper_config_t op = {};
    op.in.buffer  = (void *)src; op.in.pic_w = sw; op.in.pic_h = sh; op.in.block_w = sw; op.in.block_h = sh;
    op.in.srm_cm  = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = fb; op.out.buffer_size = fbsz;
    op.out.pic_w  = NV_LCD_H_RES; op.out.pic_h = NV_LCD_V_RES; op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.block_offset_x = (uint32_t)ox; op.out.block_offset_y = (uint32_t)oy;
    op.scale_x = (float)tw / sw; op.scale_y = (float)th / sh;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(s_vblit_ppa, &op) == ESP_OK;
}

i2c_master_bus_handle_t nv_hal_i2c_bus(void) { return s_i2c_bus; }

// ---------------------------------------------------------------- on-die temperature (P4)
#include "driver/temperature_sensor.h"

bool nv_hal_temp_read(float *out_c) {
    if (!out_c) return false;
    static temperature_sensor_handle_t s_tsens = nullptr;
    static bool s_tried = false;
    if (!s_tsens && !s_tried) {   // lazy one-shot install; never retried on failure
        s_tried = true;
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK) {
            s_tsens = nullptr;
            NV_LOGW(TAG, "temperature sensor unavailable");
        } else if (temperature_sensor_enable(s_tsens) != ESP_OK) {
            temperature_sensor_uninstall(s_tsens);
            s_tsens = nullptr;
            NV_LOGW(TAG, "temperature sensor enable failed");
        }
    }
    if (!s_tsens) return false;
    return temperature_sensor_get_celsius(s_tsens, out_c) == ESP_OK;
}
