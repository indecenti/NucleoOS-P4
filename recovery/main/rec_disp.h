// rec_disp — the recovery app's screen: JD9165 panel bring-up and a tiny software renderer drawing
// straight into the single PSRAM framebuffer (no LVGL).
#pragma once
#include <stdint.h>
#include "rec_assets.h"

#define REC_W 1024
#define REC_H 600

// RGB565 from 8-bit channels.
#define REC_RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

bool rec_disp_init(void);                 // panel + backlight; false if the panel is missing
void rec_disp_backlight(int percent);
void rec_fill(int x, int y, int w, int h, uint16_t c);
void rec_round_rect(int x, int y, int w, int h, int r, uint16_t c);
// Draw UTF-8 text (Latin-1 range) with its top-left at (x, y). Returns the pen x after the text.
int  rec_text(const rec_font_t *f, int x, int y, const char *utf8, uint16_t c);
int  rec_text_width(const rec_font_t *f, const char *utf8);
void rec_text_center(const rec_font_t *f, int cx, int y, const char *utf8, uint16_t c);
void rec_image(const rec_image_t *img, int x, int y, uint16_t bg);
void rec_flush(void);                     // write the framebuffer out of the CPU cache to the panel
