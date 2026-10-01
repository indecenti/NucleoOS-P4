// rec_disp — see rec_disp.h. Panel setup mirrors components/nv_hal/nv_hal.cpp display_init (same
// JD9165 init table and timings) with one framebuffer and no DMA2D.
#include "rec_disp.h"

#include "driver/ledc.h"
#include "esp_cache.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_jd9165.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

#include <cstring>

static const char *TAG = "rec_disp";

namespace {

constexpr int kPinReset = 5, kPinBacklight = 23;   // nv_pins.h
constexpr int kLdoChan = 3, kLdoMv = 2500, kLanes = 2, kLaneMbps = 750, kDpiMhz = 50;

uint16_t *s_fb = nullptr;

const jd9165_lcd_init_cmd_t kInit[] = {
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

inline uint16_t blend(uint16_t bg, uint16_t fg, uint8_t a) {
    if (a == 255) return fg;
    if (a == 0) return bg;
    const uint32_t rb = ((fg & 0xF81F) * a + (bg & 0xF81F) * (255 - a)) >> 8;
    const uint32_t g = ((fg & 0x07E0) * a + (bg & 0x07E0) * (255 - a)) >> 8;
    return (uint16_t)((rb & 0xF81F) | (g & 0x07E0));
}

inline void put(int x, int y, uint16_t c, uint8_t a) {
    if ((unsigned)x >= REC_W || (unsigned)y >= REC_H) return;
    uint16_t *p = &s_fb[y * REC_W + x];
    *p = blend(*p, c, a);
}

// Next code point from UTF-8 (Latin-1 is all we draw; anything else becomes '?').
uint32_t next_cp(const char **s) {
    const uint8_t *p = (const uint8_t *)*s;
    uint32_t cp = *p++;
    if (cp >= 0xC0 && cp < 0xE0 && (*p & 0xC0) == 0x80) cp = ((cp & 0x1F) << 6) | (*p++ & 0x3F);
    else if (cp >= 0x80) { while ((*p & 0xC0) == 0x80) p++; cp = '?'; }
    *s = (const char *)p;
    return cp;
}

const rec_glyph_t *glyph(const rec_font_t *f, uint32_t cp) {
    if (cp < f->first || cp > f->last) cp = '?';
    return &f->glyphs[cp - f->first];
}

}  // namespace

void rec_disp_backlight(int percent) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)(255 * percent / 100));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

bool rec_disp_init(void) {
    ledc_timer_config_t t = {};
    t.speed_mode = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_8_BIT;
    t.timer_num = LEDC_TIMER_0;
    t.freq_hz = 5000;
    t.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&t);
    ledc_channel_config_t c = {};
    c.gpio_num = kPinBacklight;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel = LEDC_CHANNEL_0;
    c.timer_sel = LEDC_TIMER_0;
    ledc_channel_config(&c);

    esp_ldo_channel_handle_t ldo_h = nullptr;
    esp_ldo_channel_config_t ldo = {};
    ldo.chan_id = kLdoChan;
    ldo.voltage_mv = kLdoMv;
    if (esp_ldo_acquire_channel(&ldo, &ldo_h) != ESP_OK) return false;

    esp_lcd_dsi_bus_handle_t bus_h = nullptr;
    esp_lcd_dsi_bus_config_t bus = {};
    bus.bus_id = 0;
    bus.num_data_lanes = kLanes;
    bus.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus.lane_bit_rate_mbps = kLaneMbps;
    if (esp_lcd_new_dsi_bus(&bus, &bus_h) != ESP_OK) return false;

    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_dbi_io_config_t dbi = JD9165_PANEL_IO_DBI_CONFIG();
    if (esp_lcd_new_panel_io_dbi(bus_h, &dbi, &io) != ESP_OK) return false;

    esp_lcd_dpi_panel_config_t dpi = {};
    dpi.virtual_channel = 0;
    dpi.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi.dpi_clock_freq_mhz = kDpiMhz;
    dpi.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
    dpi.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi.out_color_format = LCD_COLOR_FMT_RGB565;
    dpi.num_fbs = 1;
    dpi.video_timing.h_size = REC_W;
    dpi.video_timing.v_size = REC_H;
    dpi.video_timing.hsync_pulse_width = 20;
    dpi.video_timing.hsync_back_porch = 160;
    dpi.video_timing.hsync_front_porch = 160;
    dpi.video_timing.vsync_pulse_width = 2;
    dpi.video_timing.vsync_back_porch = 21;
    dpi.video_timing.vsync_front_porch = 12;

    jd9165_vendor_config_t vendor = {};
    vendor.init_cmds = kInit;
    vendor.init_cmds_size = sizeof(kInit) / sizeof(kInit[0]);
    vendor.mipi_config.dsi_bus = bus_h;
    vendor.mipi_config.dpi_config = &dpi;

    esp_lcd_panel_dev_config_t pcfg = {};
    pcfg.reset_gpio_num = kPinReset;
    pcfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    pcfg.data_endian = LCD_RGB_DATA_ENDIAN_LITTLE;
    pcfg.bits_per_pixel = 16;
    pcfg.vendor_config = &vendor;

    esp_lcd_panel_handle_t panel = nullptr;
    if (esp_lcd_new_panel_jd9165(io, &pcfg, &panel) != ESP_OK) return false;
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    void *fb = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb) != ESP_OK || !fb) return false;
    s_fb = (uint16_t *)fb;
    ESP_LOGI(TAG, "panel up");
    return true;
}

void rec_fill(int x, int y, int w, int h, uint16_t c) {
    if (!s_fb) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > REC_W) w = REC_W - x;
    if (y + h > REC_H) h = REC_H - y;
    for (int j = 0; j < h; j++) {
        uint16_t *row = &s_fb[(y + j) * REC_W + x];
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

void rec_round_rect(int x, int y, int w, int h, int r, uint16_t c) {
    if (!s_fb) return;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    rec_fill(x, y + r, w, h - 2 * r, c);
    // Anti-aliased corners: per-pixel coverage of the quarter circles, 4x4 supersampled.
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < w; i++) {
            // Circle centres sit on pixel corners: (r, r) on the left, (w - r, r) on the right.
            const int ccx = i < r ? r : (i >= w - r ? w - r : -1);
            int cov = 16;
            if (ccx >= 0) {
                cov = 0;
                for (int sy = 0; sy < 4; sy++)
                    for (int sx = 0; sx < 4; sx++) {
                        const float dx = i + (sx + 0.5f) / 4 - ccx;
                        const float dy = j + (sy + 0.5f) / 4 - r;
                        if (dx * dx + dy * dy <= (float)r * r) cov++;
                    }
            }
            const uint8_t a = (uint8_t)(cov * 255 / 16);
            put(x + i, y + j, c, a);
            put(x + i, y + h - 1 - j, c, a);
        }
    }
}

int rec_text(const rec_font_t *f, int x, int y, const char *s, uint16_t c) {
    while (*s) {
        const rec_glyph_t *g = glyph(f, next_cp(&s));
        const uint8_t *bits = f->bits + g->offset;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                const uint8_t a = bits[j * g->w + i];
                if (a) put(x + g->x + i, y + g->y + j, c, a);
            }
        x += g->adv;
    }
    return x;
}

int rec_text_width(const rec_font_t *f, const char *s) {
    int w = 0;
    while (*s) w += glyph(f, next_cp(&s))->adv;
    return w;
}

void rec_text_center(const rec_font_t *f, int cx, int y, const char *s, uint16_t c) {
    rec_text(f, cx - rec_text_width(f, s) / 2, y, s, c);
}

void rec_image(const rec_image_t *img, int x, int y, uint16_t bg) {
    (void)bg;
    for (int j = 0; j < img->h; j++)
        for (int i = 0; i < img->w; i++) {
            const uint8_t *p = &img->rgba[(j * img->w + i) * 4];
            if (p[3]) put(x + i, y + j, REC_RGB(p[0], p[1], p[2]), p[3]);
        }
}

void rec_flush(void) {
    if (s_fb) esp_cache_msync(s_fb, REC_W * REC_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}
