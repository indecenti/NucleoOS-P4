/* ScummVM - Graphic Adventure Engine
 *
 * NucleoOS backend: the host imports (module "nv") this port uses, declared here instead of
 * including sdk/include/nucleo_sdk.h (that header targets freestanding apps and redeclares libc).
 * Semantics are documented in nucleo_sdk.h (host ABI v12).
 *
 * This program is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */

#ifndef BACKENDS_PLATFORM_NUCLEO_IMPORTS_H
#define BACKENDS_PLATFORM_NUCLEO_IMPORTS_H

#include <stdint.h>

#define NV_IMPORT(sym) __attribute__((import_module("nv"), import_name(sym)))

extern "C" {
NV_IMPORT("log")             void    nv_log(int32_t level, const char *msg);
NV_IMPORT("millis")          int32_t nv_millis(void);
NV_IMPORT("lang")            int32_t nv_lang(char *buf, uint32_t len);
NV_IMPORT("gfx_width")       int32_t nv_gfx_width(void);
NV_IMPORT("gfx_height")      int32_t nv_gfx_height(void);
NV_IMPORT("gfx_blit")        void    nv_gfx_blit_raw(const void *px, int32_t len, int32_t x, int32_t y, int32_t w, int32_t h);
NV_IMPORT("gfx_input")       int32_t nv_gfx_input_raw(void);
NV_IMPORT("gfx_back")        int32_t nv_gfx_back(void);
NV_IMPORT("gfx_present")     int32_t nv_gfx_present(void);
NV_IMPORT("gfx_touch_count") int32_t nv_gfx_touch_count(void);
NV_IMPORT("gfx_touch_point") int32_t nv_gfx_touch_point_raw(int32_t idx);
NV_IMPORT("gfx_pad")         int32_t nv_gfx_pad(void);
NV_IMPORT("audio_open")      int32_t nv_audio_open(int32_t rate, int32_t channels);
NV_IMPORT("audio_write")     int32_t nv_audio_write(const void *pcm, int32_t bytes);
NV_IMPORT("audio_backlog")   int32_t nv_audio_backlog(void);
NV_IMPORT("audio_close")     void    nv_audio_close(void);

// ABI v12 network (non-blocking handles, see nucleo_sdk.h).
NV_IMPORT("http_req")        int32_t nv_http_req(const char *spec_json, const void *body, uint32_t len);
NV_IMPORT("http_state")      int32_t nv_http_state(int32_t h);
NV_IMPORT("http_status")     int32_t nv_http_status(int32_t h);
NV_IMPORT("http_read")       int32_t nv_http_read(int32_t h, void *buf, uint32_t len);
NV_IMPORT("http_close")      void    nv_http_close(int32_t h);

// ABI v11 game controllers (see nucleo_sdk.h).
typedef struct {
	uint32_t buttons;
	int16_t  lx, ly, rx, ry;
	int16_t  lt, rt;
	uint8_t  source, battery, mapped, rumble;
	uint16_t vid, pid;
} nv_pad_state_t;
NV_IMPORT("pad_count")       int32_t nv_pad_count(void);
NV_IMPORT("pad_state")       int32_t nv_pad_state(int32_t index, nv_pad_state_t *st, int32_t len);

// ABI v14 raw keyboard + mouse (see nucleo_sdk.h).
typedef struct { int32_t dx, dy, wheel; uint32_t buttons; } nv_mouse_t;
NV_IMPORT("kbd_state")       int32_t nv_kbd_state(uint8_t *buf, int32_t len);
NV_IMPORT("mouse_read")      int32_t nv_mouse_read(nv_mouse_t *m, int32_t len);
}

enum { NV_MOUSE_LEFT = 1, NV_MOUSE_RIGHT = 2, NV_MOUSE_MIDDLE = 4 };
enum { NV_PADB_A = 1 << 0, NV_PADB_B = 1 << 1, NV_PADB_X = 1 << 2, NV_PADB_Y = 1 << 3,
       NV_PADB_BACK = 1 << 4, NV_PADB_GUIDE = 1 << 5, NV_PADB_START = 1 << 6,
       NV_PADB_LB = 1 << 9, NV_PADB_RB = 1 << 10, NV_PADB_UP = 1 << 11, NV_PADB_DOWN = 1 << 12,
       NV_PADB_LEFT = 1 << 13, NV_PADB_RIGHT = 1 << 14 };

enum { NV_LOG_ERROR = 0, NV_LOG_WARN = 1, NV_LOG_INFO = 2, NV_LOG_DEBUG = 3 };

enum { NV_PAD_UP = 1, NV_PAD_DOWN = 2, NV_PAD_LEFT = 4, NV_PAD_RIGHT = 8, NV_PAD_A = 16, NV_PAD_B = 32,
       NV_PAD_X = 64, NV_PAD_Y = 128, NV_PAD_L = 256, NV_PAD_R = 512, NV_PAD_START = 1024,
       NV_PAD_SELECT = 2048 };

#endif
