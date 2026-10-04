// nv_hal — JD9165 MIPI-DSI display + GT911 touch + LEDC backlight bring-up.
// Panel config + init sequence verified working on this exact board (Guition JC1060P470C_I_W).
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
#include "esp_timer.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "hal/axi_icm_ll.h"   // AXI interconnect QoS: the display's framebuffer reads go first
#include "freertos/semphr.h"
#include <cstdio>
#include <cstring>
#include "nv_2d.h"         // every PPA / JPEG job goes through the shared 2D-engine lock
#include "nv_disp.h"       // double-buffered compositor: LVGL display + front-buffer access
#include "nv_sd.h"         // nv_sd_fopen/nv_sd_fclose: removal-safe screenshot writes

static const char *TAG = "hal";

static lv_display_t *s_disp = nullptr;
static esp_ldo_channel_handle_t s_ldo = nullptr;
static esp_lcd_dsi_bus_handle_t s_dsi_bus = nullptr;
static i2c_master_bus_handle_t s_i2c_bus = nullptr;  // shared internal I2C (touch + RTC + codecs)
static esp_lcd_panel_handle_t s_panel = nullptr;     // raw JD9165 panel (direct-draw paths)
static esp_lcd_touch_handle_t s_touch = nullptr;     // raw GT911 (direct-read paths)

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
    {0x30, (const uint8_t[]){0x00}, 1, 0},
    {0xF7, (const uint8_t[]){0x49, 0x61, 0x02, 0x00}, 4, 0},
    {0x30, (const uint8_t[]){0x01}, 1, 0},
    {0x04, (const uint8_t[]){0x0C}, 1, 0},
    {0x05, (const uint8_t[]){0x00}, 1, 0},
    {0x06, (const uint8_t[]){0x00}, 1, 0},
    {0x0B, (const uint8_t[]){0x11}, 1, 0},
    {0x17, (const uint8_t[]){0x00}, 1, 0},
    {0x20, (const uint8_t[]){0x04}, 1, 0},
    {0x1F, (const uint8_t[]){0x05}, 1, 0},
    {0x23, (const uint8_t[]){0x00}, 1, 0},
    {0x25, (const uint8_t[]){0x19}, 1, 0},
    {0x28, (const uint8_t[]){0x18}, 1, 0},
    {0x29, (const uint8_t[]){0x04}, 1, 0},
    {0x2A, (const uint8_t[]){0x01}, 1, 0},
    {0x2B, (const uint8_t[]){0x04}, 1, 0},
    {0x2C, (const uint8_t[]){0x01}, 1, 0},
    {0x30, (const uint8_t[]){0x02}, 1, 0},
    {0x01, (const uint8_t[]){0x22}, 1, 0},
    {0x03, (const uint8_t[]){0x12}, 1, 0},
    {0x04, (const uint8_t[]){0x00}, 1, 0},
    {0x05, (const uint8_t[]){0x64}, 1, 0},
    {0x0A, (const uint8_t[]){0x08}, 1, 0},
    {0x0B, (const uint8_t[]){0x0A, 0x1A, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x08, 0x1F, 0x1D}, 11, 0},
    {0x0C, (const uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0D, (const uint8_t[]){0x16, 0x1B, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x09, 0x1E, 0x1C}, 11, 0},
    {0x0E, (const uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x0F, (const uint8_t[]){0x16, 0x1B, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1C, 0x1E, 0x09, 0x07}, 11, 0},
    {0x10, (const uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x11, (const uint8_t[]){0x0A, 0x1A, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1D, 0x1F, 0x08, 0x06}, 11, 0},
    {0x12, (const uint8_t[]){0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D}, 11, 0},
    {0x14, (const uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0x18, (const uint8_t[]){0x99}, 1, 0},
    {0x30, (const uint8_t[]){0x06}, 1, 0},
    {0x12, (const uint8_t[]){0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 14, 0},
    {0x13, (const uint8_t[]){0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29}, 14, 0},
    {0x30, (const uint8_t[]){0x0A}, 1, 0},
    {0x02, (const uint8_t[]){0x4F}, 1, 0},
    {0x0B, (const uint8_t[]){0x40}, 1, 0},
    {0x12, (const uint8_t[]){0x3E}, 1, 0},
    {0x13, (const uint8_t[]){0x78}, 1, 0},
    {0x30, (const uint8_t[]){0x0D}, 1, 0},
    {0x0D, (const uint8_t[]){0x04}, 1, 0},
    {0x10, (const uint8_t[]){0x0C}, 1, 0},
    {0x11, (const uint8_t[]){0x0C}, 1, 0},
    {0x12, (const uint8_t[]){0x0C}, 1, 0},
    {0x13, (const uint8_t[]){0x0C}, 1, 0},
    {0x30, (const uint8_t[]){0x00}, 1, 0},
    {0x3A, (const uint8_t[]){0x55}, 1, 0},
    {0x11, (const uint8_t[]){0x00}, 1, 120},
    {0x29, (const uint8_t[]){0x00}, 1, 20},
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
    // Two frame buffers: nv_disp composes the next frame in one while the panel scans the other,
    // and switches at vsync (no tearing). See nv_disp.h.
    dpi.num_fbs = 2;
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
    // The DSI bridge streams the framebuffer out of PSRAM through DW-GDMA, one line at a time, with
    // no slack: if a line is late the bridge underruns and the panel shows blue until the next
    // frame. Every AXI master starts at the same priority, so a burst from the CPUs' caches, DMA2D
    // (LVGL's flushes, PPA, JPEG) or the SD card could delay it. Its reads now win arbitration; it
    // only needs ~74 MB/s of the bus, so the others barely notice.
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(0, 0, 15);
    axi_icm_ll_set_dw_gdma_qos_arbiter_prio(1, 0, 15);
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
// Adaptive input rate: 60 Hz while a finger is down and for 1.5 s after (drags, scrolls, double
// taps), 30 Hz when idle, 10 Hz with the screen asleep. At idle the LVGL task and the I2C poll
// were waking 60x/s each for nothing; the first touch after a pause now lands within ~33 ms.
// 10 ms while in use (the GT911 reports at ~100 Hz; LVGL's read is only a cache copy), 16 ms idle: the
// first touch after a pause used to wait up to 33 + 33 ms before LVGL even saw it.
constexpr uint32_t kTouchFastMs = 10, kTouchIdleMs = 16, kTouchSleepMs = 100, kTouchHoldMs = 1500;
static volatile bool s_tp_sleep = false;
static uint32_t touch_period_ms(bool pressed, uint32_t *last_active_ms) {
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (pressed) *last_active_ms = now;
    if (now - *last_active_ms < kTouchHoldMs) return kTouchFastMs;
    return s_tp_sleep ? kTouchSleepMs : kTouchIdleMs;
}
void nv_hal_touch_set_sleep(bool asleep) { s_tp_sleep = asleep; }

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    static uint32_t last_active, cur_period = kTouchFastMs;
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
    const uint32_t want = touch_period_ms(pressed, &last_active);
    if (want != cur_period) { cur_period = want; lv_timer_set_period(lv_indev_get_read_timer(indev), want); }
}

// Dedicated poll task — the only place the GT911 I2C bus is touched for input. Runs at a
// priority above the LVGL task so a busy renderer can't delay sampling. A transient I2C
// NAK is skipped, not fatal (never ESP_ERROR_CHECK -> abort/reboot on a glitchy read).
static void touch_task(void *arg) {
    esp_lcd_touch_handle_t touch = (esp_lcd_touch_handle_t)arg;
    TickType_t last = xTaskGetTickCount();
    uint32_t last_active = 0;                     // adaptive rate, see touch_period_ms
    bool pressed_now = false;
    int fails = 0;                                // consecutive failed I2C reads
    for (;;) {
        esp_lcd_touch_point_data_t pts[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {};
        uint8_t cnt = 0;
        if (esp_lcd_touch_read_data(touch) == ESP_OK &&
            esp_lcd_touch_get_data(touch, pts, &cnt, CONFIG_ESP_LCD_TOUCH_MAX_POINTS) == ESP_OK) {
            fails = 0;
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
            pressed_now = cnt > 0;
        } else if (fails < 5 && ++fails == 5) {
            // A skipped read leaves the cache holding the last good sample — if that was a press,
            // LVGL sees a finger glued to the screen for as long as the bus stays glitchy. After
            // 5 straight failures (~80 ms) force a release; the next good read repopulates it.
            portENTER_CRITICAL(&s_tp_mux);
            s_tp_pressed = false;
            s_tp_cnt = 0;
            portEXIT_CRITICAL(&s_tp_mux);
        }
        const TickType_t period = pdMS_TO_TICKS(touch_period_ms(pressed_now, &last_active));
        if (xTaskGetTickCount() - last >= period) last = xTaskGetTickCount();   // rate dropped: no catch-up burst
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
    // LVGL time comes straight from esp_timer (lv_tick_set_cb below), so the port's tick timer is
    // only a fallback: 1 s instead of 5 ms = 200 fewer esp_timer wakeups a second at idle.
    port_cfg.timer_period_ms = 1000;
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        NV_LOGE(TAG, "lvgl_port_init failed");
        return false;
    }
    lv_tick_set_cb([]() -> uint32_t { return (uint32_t)(esp_timer_get_time() / 1000); });
    // Second LVGL pool, from the PSRAM heap: the static 192 KB one ran dry with the classic desktop
    // + a widget-heavy app (System Monitor > Processes asserted in lv_array_push_back). TLSF takes
    // extra pools at run time; allocated once, never freed.
    if (void *pool = heap_caps_aligned_alloc(16, 256 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
        if (lvgl_port_lock(0)) {
            if (!lv_mem_add_pool(pool, 256 * 1024)) NV_LOGW(TAG, "LVGL extra pool refused");
            lvgl_port_unlock();
        }
    }

    // The LVGL display is nv_disp's: double buffering with the switch at vsync, direct rendering in
    // landscape, PPA rotation into the back buffer when rotated (lv_display_set_rotation at runtime).
    // esp_lvgl_port still provides the LVGL task, lock and tick.
    s_disp = nv_disp_create(panel, NV_LCD_H_RES, NV_LCD_V_RES);
    if (!s_disp) {
        NV_LOGE(TAG, "display create failed");
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
        lv_timer_set_period(lv_indev_get_read_timer(indev), kTouchFastMs);
        lvgl_port_unlock();

        // Poll task: prio 7 (above the LVGL task's 6) so touch sampling preempts a redraw
        // instead of waiting on it; pinned to core 1 alongside LVGL, leaving core 0 for WiFi.
        // Forever task doing only I2C reads + a spinlock (never flash) -> stack in PSRAM.
        if (xTaskCreatePinnedToCoreWithCaps(touch_task, "nv_touch", 4096, touch, 7, nullptr, 1,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
            xTaskCreatePinnedToCore(touch_task, "nv_touch", 4096, touch, 7, nullptr, 1);
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

// Lazy one-time init of driver handles below (PPA clients, temp sensor) is reachable from several
// tasks at once (LVGL, httpd, media workers): serialize it. Thread-safe local static creation, no
// heap (same pattern as nv_2d's lock).
static SemaphoreHandle_t lazy_init_lock(void) {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t h = xSemaphoreCreateMutexStatic(&buf);
    return h;
}

// ---------------------------------------------------------------- screenshot (HW JPEG)
// Whole screen to a JPEG (shade button, shell, /api/screen): the region path below with the full
// frame, so one encoder engine serves every capture.
bool nv_hal_screenshot(const char *path) {
    if (!s_panel || !path) return false;
    uint16_t *frame = nv_hal_screen_freeze();
    if (!frame) { NV_LOGE(TAG, "screenshot: no memory / display busy"); return false; }
    const bool ok = nv_hal_jpeg_save_rgb565(frame, NV_LCD_H_RES, NV_LCD_V_RES, 0, path, 90);
    heap_caps_free(frame);
    return ok;
}

// ---------------------------------------------------------------- region capture (screenshot tool)
// The capture tool (nv_capture) freezes the frame on screen, lets the user pick a region, then saves
// it: two steps, so the pixels the user selected are exactly the ones saved (the live screen keeps
// moving under the overlay).

uint16_t *nv_hal_screen_freeze(void) {
    if (!s_panel) return nullptr;
    const size_t raw = (size_t)NV_LCD_H_RES * NV_LCD_V_RES * 2;
    uint16_t *copy = (uint16_t *)heap_caps_aligned_alloc(64, raw, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) return nullptr;
    nv_disp_surface_t fs;
    if (!nv_disp_front_begin(&fs, 500)) { heap_caps_free(copy); return nullptr; }
    esp_cache_msync(fs.px, raw, ESP_CACHE_MSYNC_FLAG_DIR_M2C);   // DMA writers bypass the CPU cache
    memcpy(copy, fs.px, raw);
    nv_disp_front_end();
    return copy;
}

// One JPEG at a time across every saver (shares the screenshot's lazy engine through the same lock).
static SemaphoreHandle_t jpeg_save_lock(void) {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t h = xSemaphoreCreateMutexStatic(&buf);
    return h;
}

bool nv_hal_jpeg_save_rgb565(const uint16_t *px, int w, int h, int stride_px, const char *path, int quality) {
    if (!px || !path || w < 8 || h < 8) return false;
    if (stride_px <= 0) stride_px = w;
    w &= ~7;                                         // YUV444 encodes 8x8 MCUs: whole blocks per row
    const int hpad = (h + 7) & ~7;                   // rows past the image read clean black padding
    xSemaphoreTake(jpeg_save_lock(), portMAX_DELAY);
    static jpeg_encoder_handle_t enc = nullptr;
    if (!enc) {
        jpeg_encode_engine_cfg_t eng = {};
        eng.timeout_ms = 500;
        if (jpeg_new_encoder_engine(&eng, &enc) != ESP_OK) { enc = nullptr; xSemaphoreGive(jpeg_save_lock()); return false; }
    }
    const size_t in_len = (size_t)w * hpad * 2;
    jpeg_encode_memory_alloc_cfg_t in_cfg = {};
    in_cfg.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
    size_t in_got = 0, out_got = 0;
    uint8_t *in_buf = (uint8_t *)jpeg_alloc_encoder_mem(in_len, &in_cfg, &in_got);
    jpeg_encode_memory_alloc_cfg_t out_cfg = {};
    out_cfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
    uint8_t *out_buf = (uint8_t *)jpeg_alloc_encoder_mem(in_len + 4096, &out_cfg, &out_got);
    bool ok = false;
    if (in_buf && out_buf) {
        memset(in_buf, 0, in_got);
        for (int y = 0; y < h; y++) memcpy(in_buf + (size_t)y * w * 2, px + (size_t)y * stride_px, (size_t)w * 2);
        jpeg_encode_cfg_t cfg = {};
        cfg.width = w;
        cfg.height = h;
        cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
        cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV444;   // full chroma: text and UI edges stay crisp
        cfg.image_quality = quality > 0 && quality <= 100 ? quality : 92;
        uint32_t out_size = 0;
        if (nv_2d_jpeg_encode(enc, &cfg, in_buf, in_got, out_buf, out_got, &out_size) == ESP_OK && out_size) {
            FILE *f = nv_sd_fopen(path, "wb");
            if (f) {
                ok = fwrite(out_buf, 1, out_size, f) == out_size;
                if (nv_sd_fclose(f) != 0) ok = false;
            }
        }
        if (ok) NV_LOGI(TAG, "capture %dx%d -> %s (%u KB)", w, h, path, (unsigned)(out_size / 1024));
        else NV_LOGE(TAG, "capture: encode/write failed (%s)", path);
    }
    if (in_buf) free(in_buf);
    if (out_buf) free(out_buf);
    xSemaphoreGive(jpeg_save_lock());
    return ok;
}

// ---------------------------------------------------------------- thumbnail (PPA downscale)
// PPA-scale the panel framebuffer down to dw x dh raw RGB565 pixels into the caller's 64B-aligned
// buffer (see nv_hal.h for why the caller owns it). Cheap for the Recents cards: the grab is a ~ms
// hardware op, safe on the LVGL thread; the caller writes the SD copy on the background worker.
// Best-effort; false on any failure (caller falls back to the icon).
bool nv_hal_thumbnail_grab(uint8_t *dst, int dw, int dh) {
    if (!s_panel || !dst || dw <= 0 || dh <= 0) return false;

    const size_t dst_len = (size_t)dw * dh * 2;

    // Registered once and kept (same lifecycle as s_vblit_ppa / the camera render client).
    static ppa_client_handle_t cl = nullptr;
    if (!cl) {
        xSemaphoreTake(lazy_init_lock(), portMAX_DELAY);
        if (!cl) {
            ppa_client_handle_t c = nullptr;
            ppa_client_config_t ccfg = {};
            ccfg.oper_type = PPA_OPERATION_SRM;
            if (ppa_register_client(&ccfg, &c) == ESP_OK) cl = c;
        }
        xSemaphoreGive(lazy_init_lock());
        if (!cl) return false;
    }
    // The PPA scales in 1/16 steps rounded DOWN (its argument check uses the exact float, the
    // hardware doesn't): 176/1024 ran at 2/16, so only 128x75 of the 176x104 card was written and
    // the rest of every Recents preview was garbage. One exact k/16 for both axes, the smallest
    // that covers the card, with the panel centre-cropped to exactly card * 16/k.
    int k = (dw * 16 + NV_LCD_H_RES - 1) / NV_LCD_H_RES;
    { const int ky = (dh * 16 + NV_LCD_V_RES - 1) / NV_LCD_V_RES; if (ky > k) k = ky; }
    if (k < 1) k = 1;
    int bw = (dw * 16 + k - 1) / k, bh = (dh * 16 + k - 1) / k;
    if (bw > NV_LCD_H_RES) bw = NV_LCD_H_RES;
    if (bh > NV_LCD_V_RES) bh = NV_LCD_V_RES;

    ppa_srm_oper_config_t op = {};
    // The PPA reads the frame on screen by DMA (the driver writes back the CPU cache first); hold
    // it so the next swap cannot hand this buffer to LVGL mid-read.
    nv_disp_surface_t fs;
    if (!nv_disp_front_begin(&fs, 200)) return false;
    op.in.buffer       = fs.px;
    op.in.pic_w        = NV_LCD_H_RES;
    op.in.pic_h        = NV_LCD_V_RES;
    op.in.block_offset_x = (uint32_t)((NV_LCD_H_RES - bw) / 2);
    op.in.block_offset_y = (uint32_t)((NV_LCD_V_RES - bh) / 2);
    op.in.block_w      = (uint32_t)bw;
    op.in.block_h      = (uint32_t)bh;
    op.in.srm_cm       = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer      = dst;
    op.out.buffer_size = dst_len;
    op.out.pic_w       = dw;
    op.out.pic_h       = dh;
    op.out.srm_cm      = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle  = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x         = (float)k / 16.0f;
    op.scale_y         = (float)k / 16.0f;
    op.mode            = PPA_TRANS_MODE_BLOCKING;
    const bool ok = nv_2d_srm(cl, &op) == ESP_OK;
    nv_disp_front_end();
    return ok;
}

// ---------------------------------------------------------------- direct-to-panel video blit
static ppa_client_handle_t s_vblit_ppa = nullptr;   // cached SRM client (registered once)
static int s_vregion[4] = { -1, -1, -1, -1 };      // direct region registered with nv_disp

// 1:1 full-width frames skip the PPA. On this chip revision the SRM walks the source in 16x16
// macro-blocks (18 PSRAM row reads of 36 bytes each), ~12 us per block whatever the scale: a
// 1024x576 frame costs ~28 ms even unscaled. When the picture already has the panel's width, its
// rows are contiguous in both buffers, so one AXI-GDMA copy (nv_2d_copy) moves it in long bursts.
bool nv_hal_video_geom(int sw, int sh, int dx, int dy, int dw, int dh, int mode, nv_hal_blit_geom_t *g) {
    if (!g || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return false;
    // clamp the destination rect to the panel
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    if (dx + dw > NV_LCD_H_RES) dw = NV_LCD_H_RES - dx;
    if (dy + dh > NV_LCD_V_RES) dh = NV_LCD_V_RES - dy;
    if (dw < 2 || dh < 2) return false;

    // The SRM hardware scales by k/16 (rounded down) and writes floor(block * k / 16) pixels, so pick
    // k per mode and crop the source block to match — the output then covers exactly what we clear
    // around. Source block = (bx, by, bw, bh) inside the sw x sh picture.
    int bx = 0, by = 0, bw = sw, bh = sh, kx, ky;
    if (mode == NV_HAL_BLIT_STRETCH) {
        kx = dw * 16 / sw;  ky = dh * 16 / sh;
    } else if (mode == NV_HAL_BLIT_ZOOM) {
        // cover the rect: the smallest k that fills both axes, overflow cropped (centred)
        int k = (dw * 16 + sw - 1) / sw;
        { const int k2 = (dh * 16 + sh - 1) / sh; if (k2 > k) k = k2; }
        kx = ky = k;
    } else {
        // fit inside the rect; one 1/16 step larger costs a thin edge crop — worth it while the crop
        // stays under ~4% per axis (1200x672 camera clips: 975x546 -> 1022x588 on the 1024x600 panel).
        // FIT_EXACT (game canvases: HUD at the edges, touch mapped back to canvas pixels) never crops.
        int k = dw * 16 / sw;
        { const int k2 = dh * 16 / sh; if (k2 < k) k = k2; }
        const int kb = k + 1;
        const int cw = (dw * 16 / kb < sw) ? dw * 16 / kb : sw;
        const int ch = (dh * 16 / kb < sh) ? dh * 16 / kb : sh;
        if (mode != NV_HAL_BLIT_FIT_EXACT && k >= 1 && cw * 100 >= sw * 96 && ch * 100 >= sh * 96) k = kb;
        kx = ky = k;
    }
    if (kx < 1) kx = 1;
    if (ky < 1) ky = 1;
    if (kx > 255 * 16) kx = 255 * 16;   // SRM integer part < 256
    if (ky > 255 * 16) ky = 255 * 16;
    {   // largest block that fits
        const int mw = dw * 16 / kx, mh = dh * 16 / ky;
        if (bw > mw) bw = mw;
        if (bh > mh) bh = mh;
    }
    if (bw < 1 || bh < 1) return false;
    bx = (sw - bw) / 2;  by = (sh - bh) / 2;
    const int tw = bw * kx / 16, th = bh * ky / 16;
    if (tw < 1 || th < 1) return false;
    g->kx = kx; g->ky = ky; g->bx = bx; g->by = by; g->bw = bw; g->bh = bh; g->tw = tw; g->th = th;
    g->ox = dx + (dw - tw) / 2; g->oy = dy + (dh - th) / 2;
    g->dx = dx; g->dy = dy; g->dw = dw; g->dh = dh;
    return true;
}

// Scale/copy one RGB565 picture into `fb` (a full panel frame buffer) at rect (dx,dy,dw,dh), which is
// clamped to the panel in place. Shared by the immediate blit and the vsync-paced layer draw.
static bool video_draw_into(void *fb, const void *src, int sw, int sh, int src_pitch, int &dx, int &dy,
                            int &dw, int &dh, int mode, bool clear_bars) {
    if (!fb || !src) return false;
    if (src_pitch < sw) src_pitch = sw;
    nv_hal_blit_geom_t gm;
    if (!nv_hal_video_geom(sw, sh, dx, dy, dw, dh, mode, &gm)) return false;
    dx = gm.dx; dy = gm.dy; dw = gm.dw; dh = gm.dh;
    const int kx = gm.kx, ky = gm.ky, bx = gm.bx, by = gm.by, bw = gm.bw, bh = gm.bh;
    const int tw = gm.tw, th = gm.th, ox = gm.ox, oy = gm.oy;

    if (clear_bars) {   // black ONLY the margins around the picture, never the picture area
        uint16_t *p = (uint16_t *)fb;
        for (int y = dy; y < dy + dh; y++) {
            uint16_t *row = &p[(size_t)y * NV_LCD_H_RES];
            if (y < oy || y >= oy + th) {                       // full-width bar above/below the video
                memset(&row[dx], 0, (size_t)dw * 2);
            } else {                                            // side pillars beside the video
                if (ox > dx)             memset(&row[dx], 0, (size_t)(ox - dx) * 2);
                if (ox + tw < dx + dw)   memset(&row[ox + tw], 0, (size_t)(dx + dw - (ox + tw)) * 2);
            }
        }
        // write back only the rows we touched (was the whole 1.2 MB framebuffer every frame)
        esp_cache_msync(&p[(size_t)dy * NV_LCD_H_RES], (size_t)dh * NV_LCD_H_RES * 2,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }

    // 1:1 and panel-wide (the rows line up): DMA copy, no scaler
    if (kx == 16 && ky == 16 && bx == 0 && bw == sw && src_pitch == NV_LCD_H_RES && tw == NV_LCD_H_RES && ox == 0) {
        uint16_t *p = (uint16_t *)fb;
        const uint8_t *s = (const uint8_t *)src + (size_t)by * src_pitch * 2;
        const esp_err_t e = nv_2d_copy(&p[(size_t)oy * NV_LCD_H_RES], s, (size_t)th * NV_LCD_H_RES * 2, 100);
        if (e == ESP_OK) return true;
        if (e == ESP_ERR_TIMEOUT) return false;   // skip this frame rather than race the DMA
        // else refused up front (alignment) or retired after a timeout: use the PPA
    }

    if (!s_vblit_ppa) {
        xSemaphoreTake(lazy_init_lock(), portMAX_DELAY);
        if (!s_vblit_ppa) {
            ppa_client_handle_t h = nullptr;
            ppa_client_config_t c = {};
            c.oper_type = PPA_OPERATION_SRM;
            if (ppa_register_client(&c, &h) == ESP_OK) s_vblit_ppa = h;
        }
        xSemaphoreGive(lazy_init_lock());
        if (!s_vblit_ppa) return false;
    }

    ppa_srm_oper_config_t op = {};
    op.in.buffer  = (void *)src; op.in.pic_w = (uint32_t)src_pitch; op.in.pic_h = (uint32_t)sh;
    op.in.block_offset_x = (uint32_t)bx; op.in.block_offset_y = (uint32_t)by;
    op.in.block_w = (uint32_t)bw; op.in.block_h = (uint32_t)bh;
    op.in.srm_cm  = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = fb; op.out.buffer_size = (uint32_t)NV_LCD_H_RES * NV_LCD_V_RES * 2;
    op.out.pic_w  = NV_LCD_H_RES; op.out.pic_h = NV_LCD_V_RES; op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.block_offset_x = (uint32_t)ox; op.out.block_offset_y = (uint32_t)oy;
    op.scale_x = (float)kx / 16.0f; op.scale_y = (float)ky / 16.0f;   // exact: what the HW applies
    op.mode = PPA_TRANS_MODE_BLOCKING;
    return nv_2d_srm(s_vblit_ppa, &op) == ESP_OK;
}

bool nv_hal_video_draw(uint16_t *fb, int stride, const void *src, int sw, int sh, int src_pitch, int dx,
                       int dy, int dw, int dh, int mode, bool clear_bars) {
    if (stride != NV_LCD_H_RES) return false;
    return video_draw_into(fb, src, sw, sh, src_pitch, dx, dy, dw, dh, mode, clear_bars);
}

bool nv_hal_video_blit(const void *src, int sw, int sh, int src_pitch, int dx, int dy, int dw, int dh,
                       int mode, bool clear_bars) {
    if (!s_panel) return false;
    // Immediate: the picture goes into the frame on screen, held for the blit only (a swap holding it
    // skips this frame). The rectangle is registered as a direct region, so every LVGL swap carries
    // the latest picture into the next frame.
    nv_disp_surface_t fs;
    if (!nv_disp_front_begin(&fs, 40)) return false;
    const bool ok = video_draw_into(fs.px, src, sw, sh, src_pitch, dx, dy, dw, dh, mode, clear_bars);
    nv_disp_front_end();
    if (ok && (dx != s_vregion[0] || dy != s_vregion[1] || dw != s_vregion[2] || dh != s_vregion[3])) {
        nv_disp_set_direct_region(dx, dy, dw, dh);
        s_vregion[0] = dx; s_vregion[1] = dy; s_vregion[2] = dw; s_vregion[3] = dh;
    }
    return ok;
}

void nv_hal_video_blit_end(void) {
    nv_disp_set_direct_region(0, 0, 0, 0);
    s_vregion[0] = s_vregion[1] = s_vregion[2] = s_vregion[3] = -1;
}

i2c_master_bus_handle_t nv_hal_i2c_bus(void) { return s_i2c_bus; }

// ---------------------------------------------------------------- on-die temperature (P4)
#include "driver/temperature_sensor.h"

bool nv_hal_temp_read(float *out_c) {
    if (!out_c) return false;
    static temperature_sensor_handle_t s_tsens = nullptr;
    static bool s_tried = false;
    xSemaphoreTake(lazy_init_lock(), portMAX_DELAY);   // install + read: no two callers race the driver
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
    const bool ok = s_tsens && temperature_sensor_get_celsius(s_tsens, out_c) == ESP_OK;
    xSemaphoreGive(lazy_init_lock());
    return ok;
}
