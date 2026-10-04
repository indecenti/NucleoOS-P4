// gfx.h — Lua App engine software renderer (gfx.c)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct surface { int w, h; uint16_t *px; uint8_t *a; } surface_t;

extern surface_t *g_tgt, g_screen;
extern int g_alpha, g_dirty_y0, g_dirty_y1;
enum { GFX_BLEND_ALPHA, GFX_BLEND_ADD, GFX_BLEND_SUB, GFX_BLEND_MUL, GFX_BLEND_SCREEN, GFX_BLEND_REPLACE,
       GFX_BLEND_LIGHTEN, GFX_BLEND_DARKEN };
extern int g_blend;

void gfx_set_target(surface_t *s);
void gfx_reset_clip(void);
void gfx_clip(int x, int y, int w, int h);
void gfx_clear(uint32_t color);
void gfx_clear_transparent(void);
void gfx_rect(float x, float y, float w, float h, uint32_t color, float r);
void gfx_frame(float x, float y, float w, float h, uint32_t color, float t, float r);
void gfx_circle(float x, float y, float r, uint32_t color);
void gfx_ring(float x, float y, float r, uint32_t color, float t);
void gfx_arc(float x, float y, float r, float a0, float a1, uint32_t color, float thick);
void gfx_line(float x0, float y0, float x1, float y1, uint32_t color, float t);
void gfx_poly(const float *pts, int n, uint32_t color);
int  gfx_text(float x, float y, const char *s, float size, uint32_t color);
int  gfx_text_width(const char *s, float size);
int  gfx_font_line(float size);
int  gfx_font_ascent(float size);
void gfx_pixel(float x, float y, uint32_t color);
uint32_t gfx_get_pixel(int x, int y);
surface_t *surface_new(int w, int h, bool alpha);
surface_t *surface_from_limg(const uint8_t *d, size_t n);
void surface_free(surface_t *s);
void gfx_draw(surface_t *s, float x, float y, float dw, float dh);
void gfx_draw_ex(surface_t *s, float x, float y, float dw, float dh, int qx, int qy, int qw, int qh,
                 float rot, float ox, float oy);
extern uint32_t g_tint;
void gfx_rotate(float r);
void gfx_shear(float kx, float ky);
bool gfx_push(void);   // false: the 32-level transform stack is full
void gfx_pop(void);
void gfx_translate(float x, float y);
void gfx_scale(float sx, float sy);
void gfx_origin(void);
void gfx_identity(void);
void gfx_get_xf(float *t);
