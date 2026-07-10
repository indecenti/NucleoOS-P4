// nucleo_sdk.h — NucleoOS Anima WASM app SDK (host ABI v1).
//
// Write apps in plain C (freestanding, no libc): include this header, mark the entry point with
// NV_EXPORT, call the nv_* imports below. Build with sdk/build_app.ps1 (clang --target=wasm32,
// MVP feature set so the on-device WAMR interpreter loads it).
//
// Strings passed to the host must be NUL-terminated and live in app memory; the OS validates
// every pointer before touching it (a bad pointer traps the app, never the OS). Imports are
// permission-gated by the manifest ("permissions": ["log", "ui", ...]) — calls without the
// permission are silently dropped and logged as warnings on the OS side.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Host ABI generation this SDK targets; put the same value in the manifest "abi" field.
// (A game that uses the nv_gfx_* surface below must set "abi": 2 + permission "gfx".)
#define NUCLEO_SDK_ABI 4

#define NV_IMPORT(mod, sym) __attribute__((import_module(mod), import_name(sym)))
#define NV_EXPORT(sym)      __attribute__((export_name(sym), visibility("default")))

// nv.log levels
enum {
    NV_LOG_ERROR = 0,
    NV_LOG_WARN  = 1,
    NV_LOG_INFO  = 2,
    NV_LOG_DEBUG = 3,
};

// nv.toast kinds
enum {
    NV_TOAST_INFO  = 0,
    NV_TOAST_OK    = 1,
    NV_TOAST_WARN  = 2,
    NV_TOAST_ERROR = 3,
};

// ---- host imports (module "nv") -----------------------------------------------------------------

// Append a line to the app's on-screen output panel. [permission: log]
NV_IMPORT("nv", "print")     void    nv_print(const char *msg);

// Write to the OS log (tag "app:<id>"). [permission: log]
NV_IMPORT("nv", "log")       void    nv_log(int32_t level, const char *msg);

// Show a system toast. [permission: ui]
NV_IMPORT("nv", "toast")     void    nv_toast(int32_t kind, const char *msg);

// Milliseconds since device boot.
NV_IMPORT("nv", "millis")    int32_t nv_millis(void);

// Wall clock, UTC epoch seconds.
NV_IMPORT("nv", "time_unix") int64_t nv_time_unix(void);

// Active UI locale code ("en", "it", "es", "fr", "de") into buf; returns chars written.
NV_IMPORT("nv", "lang")      int32_t nv_lang(char *buf, uint32_t len);

// Hardware random number.
NV_IMPORT("nv", "rand")      int32_t nv_rand(void);

// Sleep (clamped to 1000 ms per call; the manifest timeout_ms bounds the whole run).
NV_IMPORT("nv", "sleep_ms")  void    nv_sleep_ms(int32_t ms);

// Persist a small blob (<= 8KB) in the app's own SD folder, and read it back. `name` is a plain
// filename (letters/digits/_/., no path). save returns 1 on success; load returns bytes read (0 if
// missing). Use for high scores / progress. [permission: gfx]
NV_IMPORT("nv", "save")      int32_t nv_save(const char *name, const void *data, int32_t len);
NV_IMPORT("nv", "load")      int32_t nv_load(const char *name, void *data, int32_t len);

// Play a sound effect: a WAV (48kHz mono 16-bit) from the app's SD folder /apps/<id>/snd/<name>.wav.
// Polyphony/harmony is baked into the sample. Non-blocking; a new call replaces a queued one.
NV_IMPORT("nv", "sound")     void    nv_sound(const char *name);
// Speak text with the OS offline voice (numbers/words/short phrases). `lang` = "it"/"en"/… (only
// installed voice packs play). Non-blocking. e.g. nv_speak("MELA", "it"), nv_speak("5", "en").
NV_IMPORT("nv", "speak")     void    nv_speak(const char *text, const char *lang);

// ---- ABI v2 game surface (module "nv", permission "gfx") ----------------------------------------
// Colors are RGB565 (use NV_RGB). All drawing targets the OS-owned canvas and is executed
// natively. A game's shape is:
//     NV_EXPORT("run") void run(void){ setup(); while (nv_gfx_present()) { step(); } }
// nv_gfx_present() shows the frame you just drew, paces it, and returns 0 when the OS wants the
// app closed — make it your loop condition.
NV_IMPORT("nv", "gfx_width")   int32_t nv_gfx_width(void);
NV_IMPORT("nv", "gfx_height")  int32_t nv_gfx_height(void);
NV_IMPORT("nv", "gfx_clear")   void    nv_gfx_clear(int32_t color);
NV_IMPORT("nv", "gfx_rect")    void    nv_gfx_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t color);
NV_IMPORT("nv", "gfx_circle")  void    nv_gfx_circle(int32_t cx, int32_t cy, int32_t r, int32_t color);
NV_IMPORT("nv", "gfx_line")    void    nv_gfx_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t color);
// Filled triangle (native scanline fill — cheap; prefer over many gfx_rect rows for polygon art).
NV_IMPORT("nv", "gfx_tri")     void    nv_gfx_tri(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t color);
NV_IMPORT("nv", "gfx_blit")    void    nv_gfx_blit_raw(const void *px, int32_t len, int32_t x, int32_t y, int32_t w, int32_t h);
// Blit a named RGB565 asset from the app's own SD folder (/sdcard/apps/<id>/img/<name>.565),
// scaled to w×h. Magenta (0xF81F) pixels are transparent. Cheap real-image art (no guest memory).
NV_IMPORT("nv", "gfx_image")   void    nv_gfx_image(const char *name, int32_t x, int32_t y, int32_t w, int32_t h);
// Draw text with the OS 5x7 font (space, 0-9, A-Z, - . : % / < > ! + x; lowercase auto-uppercased),
// magnified by `scale`. Each char advances 6*scale px.
NV_IMPORT("nv", "gfx_text")    void    nv_gfx_text(int32_t x, int32_t y, const char *s, int32_t color, int32_t scale);
NV_IMPORT("nv", "gfx_terrain") void    nv_gfx_terrain_raw(const void *tops, int32_t len, int32_t x0, int32_t ybot, int32_t cgrass, int32_t cdirt);
NV_IMPORT("nv", "gfx_tone")    void    nv_gfx_tone(int32_t freq_hz, int32_t ms);
NV_IMPORT("nv", "gfx_input")   int32_t nv_gfx_input_raw(void);
// Pending OS back-gesture requests since last call (cleared on read). Handle your own back-stack;
// return from run() when you're at your root screen to close the app.
NV_IMPORT("nv", "gfx_back")     int32_t nv_gfx_back(void);
NV_IMPORT("nv", "gfx_present") int32_t nv_gfx_present(void);
// ABI v3 multi-touch: the GT911 reports up to 5 fingers. nv_gfx_touch_count() is how many are down
// now; nv_gfx_touch_point(idx) packs (valid<<24)|(y<<12)|x for finger idx (canvas px). Prefer the
// nv_touch_count()/nv_touch_at() wrappers below. (Manifest must set "abi": 3.)
NV_IMPORT("nv", "gfx_touch_count") int32_t nv_gfx_touch_count(void);
NV_IMPORT("nv", "gfx_touch_point") int32_t nv_gfx_touch_point_raw(int32_t idx);

// ---- ABI v4 additions (manifest "abi": 4) -------------------------------------------------------
// Pixel width nv_gfx_text advances for `s` at `scale` (font metric; for centering/right-align).
NV_IMPORT("nv", "gfx_text_width") int32_t nv_gfx_text_width(const char *s, int32_t scale);
// Set panel backlight 0..100%. The OS restores the user's saved brightness when the app exits, so a
// flashlight can crank it to 100 without leaving the device stuck bright. [permission: gfx]
NV_IMPORT("nv", "backlight")       void    nv_backlight(int32_t level);

// RGB565 from 8-bit channels.
static inline int32_t NV_RGB(int r, int g, int b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
// Blit a w×h RGB565 image at (x,y) — computes the byte length for you.
static inline void nv_gfx_blit(const void *px, int32_t w, int32_t h, int32_t x, int32_t y) {
    nv_gfx_blit_raw(px, w * h * 2, x, y, w, h);
}
// Fill a destructible height field: tops[i] is the surface-y of column x0+i, filled to ybot.
static inline void nv_gfx_terrain(const short *tops, int32_t n, int32_t x0, int32_t ybot,
                                  int32_t grass, int32_t dirt) {
    nv_gfx_terrain_raw(tops, n * 2, x0, ybot, grass, dirt);
}
// Latest touch. Writes x,y (canvas pixels) and returns 1 while pressed, 0 when released.
static inline int nv_touch(int *x, int *y) {
    int32_t v = nv_gfx_input_raw();
    if (x) *x = v & 0xFFF;
    if (y) *y = (v >> 12) & 0xFFF;
    return (v >> 24) & 0x3;
}
// Multi-touch (ABI v3). nv_touch_count() = fingers currently down (0..5). nv_touch_at(idx,&x,&y)
// writes finger idx's canvas coords and returns 1 if that finger is down, 0 if idx >= count. Loop
// idx 0..count-1 to read every finger (e.g. play several piano keys at once). On an ABI<3 host these
// return 0 — guard with the count.
static inline int nv_touch_count(void) { return nv_gfx_touch_count(); }
static inline int nv_touch_at(int idx, int *x, int *y) {
    int32_t v = nv_gfx_touch_point_raw(idx);
    if (x) *x = v & 0xFFF;
    if (y) *y = (v >> 12) & 0xFFF;
    return (v >> 24) & 1;
}
// [ABI 4] Draw `s` horizontally centered on the canvas at row `y`.
static inline void nv_gfx_text_center(int y, const char *s, int color, int scale) {
    nv_gfx_text((nv_gfx_width() - nv_gfx_text_width(s, scale)) / 2, y, s, color, scale);
}

// ---- SDK helpers (implemented in nucleo_sdk.c, linked into the app) -----------------------------

// printf-style nv_print. Supports %s %d %u %x %X %c %% (32-bit only; no float, no width
// modifiers). Output clipped to 255 chars.
void nv_printf(const char *fmt, ...);

// Freestanding essentials (the compiler may emit calls to these for struct copies etc.).
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
size_t strlen(const char *s);

#ifdef __cplusplus
}
#endif
