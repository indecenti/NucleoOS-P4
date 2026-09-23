// nv_wasm_w4 — WASM-4 cart support: env imports, console memory, touch gamepad, render, audio.
// See nv_wasm_w4.h. The rasterizer and the APU are the upstream WASM-4 sources (w4/, ISC).
#include "nv_wasm_w4.h"
#include "nv_log.h"
#include "nv_audio.h"
#include "nv_hid_host.h"   // USB keyboard / mouse / gamepads as console controls
#include "nv_mem_attr.h"   // NV_PSRAM_BSS: task-context buffers stay out of internal SRAM

extern "C" {
#include "w4/w4_apu.h"
#include "w4/w4_framebuffer.h"
#include "w4/w4_util.h"
}

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static const char *TAG = "w4";

namespace {

// ---- console memory map (https://wasm4.org/docs/reference/memory) -------------------------------
constexpr uint32_t kMemSize      = 65536;
constexpr uint32_t kPalette      = 0x04;
constexpr uint32_t kDrawColors   = 0x14;
constexpr uint32_t kGamepads     = 0x16;
constexpr uint32_t kMouseX       = 0x1a;
constexpr uint32_t kMouseY       = 0x1c;
constexpr uint32_t kMouseButtons = 0x1e;
constexpr uint32_t kSystemFlags  = 0x1f;
constexpr uint32_t kFramebuffer  = 0xa0;
constexpr uint32_t kFbBytes      = 160 * 160 / 4;

constexpr uint8_t kSysPreserveFb = 1, kSysHideGamepad = 2;
constexpr uint8_t kBtn1 = 1, kBtn2 = 2, kLeft = 16, kRight = 32, kUp = 64, kDown = 128;
constexpr uint8_t kMouseLeft = 1;

// ---- touch gamepad geometry (canvas px) ---------------------------------------------------------
constexpr int kPadX = kW4X0 / 2, kPadY = 360;          // D-pad centre
constexpr int kPadArm = 105, kPadHalf = 35;             // plus shape: arm reach / half thickness
constexpr int kPadReach = 175, kPadDead = 16;           // touch radius / centre dead zone
constexpr int kBtnR = 56, kBtnReach = 92;               // drawn radius / touch radius
constexpr int kBtnXx = kW4CanvasW - kW4X0 / 2 + 50, kBtnXy = 320;   // BUTTON_1 ("X")
constexpr int kBtnZx = kW4CanvasW - kW4X0 / 2 - 60, kBtnZy = 440;   // BUTTON_2 ("Z")

// ---- bound cart ----------------------------------------------------------------------------------
wasm_module_inst_t s_inst;       // set by begin, cleared by end; imports ignore any other instance
uint8_t           *s_mem;        // its linear memory (exactly 64 KB, never grows: max 1 page)
char               s_id[32];
struct Disk { uint16_t size; uint8_t data[1024]; };
NV_PSRAM_BSS Disk  s_disk;
int16_t            s_mouse_x = 0x7fff, s_mouse_y = 0x7fff;
NV_PSRAM_BSS uint8_t s_last_fb[kFbBytes];
uint32_t           s_last_pal[4];
bool               s_have_last;
int64_t            s_trace_window_us;
int                s_trace_count;

// APU: tone() runs on the WASM worker, sample synthesis on the audio task.
SemaphoreHandle_t  s_apu_mu;
TaskHandle_t       s_audio_task;
volatile bool      s_audio_run;      // a cart is bound: stream
volatile bool      s_audio_busy;     // the task holds the PCM stream open

inline bool bound(wasm_exec_env_t env) { return s_mem && wasm_runtime_get_module_inst(env) == s_inst; }

inline int clampi(int64_t v, int64_t lo, int64_t hi) { return (int)(v < lo ? lo : (v > hi ? hi : v)); }

void trap(wasm_exec_env_t env, const char *msg) {
    wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), msg);
}

// Throttled cart trace -> OS log (a cart tracing every frame would flood the serial console).
bool trace_ok(void) {
    const int64_t now = esp_timer_get_time();
    if (now - s_trace_window_us > 1000000) { s_trace_window_us = now; s_trace_count = 0; }
    return ++s_trace_count <= 20;
}

// ---- env imports -------------------------------------------------------------------------------
// Everything a cart can pass is attacker-controlled: pointers are validated against its 64 KB,
// and coordinates are clipped before the rasterizer, whose loops would otherwise iterate over
// the full int32 range (a native loop can't be interrupted by the abort path).

void env_blit_sub(wasm_exec_env_t env, int32_t sprite, int32_t x, int32_t y, int32_t w, int32_t h,
                  int32_t src_x, int32_t src_y, int32_t stride, int32_t flags) {
    if (!bound(env)) return;
    if (w <= 0 || h <= 0) return;
    if (w > 0xffff || h > 0xffff || src_x < 0 || src_y < 0 || stride < 0) {
        trap(env, "blit: invalid sprite geometry");
        return;
    }
    const bool bpp2 = flags & 1;
    const uint64_t last = (uint64_t)(src_y + (int64_t)h - 1) * (uint64_t)stride + (uint64_t)src_x + w - 1;
    const uint64_t bytes = ((last * (bpp2 ? 2 : 1)) >> 3) + 1;
    if (bytes > kMemSize) { trap(env, "blit: sprite out of bounds"); return; }
    // Every byte the rasterizer may sample, not just w*h (upstream checked only that): srcX/srcY/
    // stride move the window. On failure WAMR has already raised the trap.
    if (!wasm_runtime_validate_app_addr(s_inst, (uint64_t)(uint32_t)sprite, bytes)) return;
    const uint8_t *px = (const uint8_t *)wasm_runtime_addr_app_to_native(s_inst, (uint64_t)(uint32_t)sprite);
    w4_framebufferBlit(px, clampi(x, -0x20000, 0x20000), clampi(y, -0x20000, 0x20000), w, h, src_x, src_y,
                       stride, bpp2, flags & 2, flags & 4, flags & 8);
}

void env_blit(wasm_exec_env_t env, int32_t sprite, int32_t x, int32_t y, int32_t w, int32_t h, int32_t flags) {
    env_blit_sub(env, sprite, x, y, w, h, 0, 0, w, flags);
}

// Clip the segment to a box one pixel beyond the screen (Liang-Barsky) so Bresenham walks at most
// a few hundred steps however far away the endpoints are.
void env_line(wasm_exec_env_t env, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    if (!bound(env)) return;
    const int64_t lim = 4096;
    if (x1 > -lim && x1 < lim && y1 > -lim && y1 < lim && x2 > -lim && x2 < lim && y2 > -lim && y2 < lim) {
        w4_framebufferLine(x1, y1, x2, y2);
        return;
    }
    double t0 = 0, t1 = 1;
    const double dx = (double)x2 - x1, dy = (double)y2 - y1;
    const double p[4] = { -dx, dx, -dy, dy };
    const double q[4] = { x1 - (-2.0), 162.0 - x1, y1 - (-2.0), 162.0 - y1 };
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) { if (q[i] < 0) return; continue; }
        const double t = q[i] / p[i];
        if (p[i] < 0) { if (t > t1) return; if (t > t0) t0 = t; }
        else          { if (t < t0) return; if (t < t1) t1 = t; }
    }
    w4_framebufferLine((int)lround(x1 + t0 * dx), (int)lround(y1 + t0 * dy),
                       (int)lround(x1 + t1 * dx), (int)lround(y1 + t1 * dy));
}

void env_hline(wasm_exec_env_t env, int32_t x, int32_t y, int32_t len) {
    if (!bound(env)) return;
    const int a = clampi(x, -2, 162), b = clampi((int64_t)x + (uint32_t)len, -2, 162);
    if (b > a) w4_framebufferHLine(a, y, b - a);
}

void env_vline(wasm_exec_env_t env, int32_t x, int32_t y, int32_t len) {
    if (!bound(env)) return;
    const int a = clampi(y, -2, 162), b = clampi((int64_t)y + (uint32_t)len, -2, 162);
    if (b > a) w4_framebufferVLine(x, a, b - a);
}

// Clamping both edges to just outside the screen keeps every "is this edge visible" test of the
// original intact while bounding the integer math.
void env_rect(wasm_exec_env_t env, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!bound(env)) return;
    const int x0 = clampi(x, -2, 162), x1 = clampi((int64_t)x + (uint32_t)w, -2, 162);
    const int y0 = clampi(y, -2, 162), y1 = clampi((int64_t)y + (uint32_t)h, -2, 162);
    w4_framebufferRect(x0, y0, x1 - x0, y1 - y0);
}

void env_oval(wasm_exec_env_t env, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!bound(env)) return;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return;           // loop runs ~w+h steps
    if (x < -20000 || x > 20000 || y < -20000 || y > 20000) return;    // cannot reach the screen
    w4_framebufferOval(x, y, w, h);
}

void env_text(wasm_exec_env_t env, const char *str, int32_t x, int32_t y) {
    if (!bound(env) || !str) return;
    w4_framebufferText((const uint8_t *)str, clampi(x, -0x20000, 0x20000), clampi(y, -0x20000, 0x20000));
}

void env_text_utf8(wasm_exec_env_t env, const char *str, uint32_t len, int32_t x, int32_t y) {
    if (!bound(env) || !str) return;
    w4_framebufferTextUtf8((const uint8_t *)str, (int)len, clampi(x, -0x20000, 0x20000),
                           clampi(y, -0x20000, 0x20000));
}

void env_text_utf16(wasm_exec_env_t env, const char *str, uint32_t len, int32_t x, int32_t y) {
    if (!bound(env) || !str) return;
    w4_framebufferTextUtf16((const uint16_t *)str, (int)(len & ~1u), clampi(x, -0x20000, 0x20000),
                            clampi(y, -0x20000, 0x20000));
}

void env_tone(wasm_exec_env_t env, int32_t freq, int32_t duration, int32_t volume, int32_t flags) {
    if (!bound(env)) return;
    xSemaphoreTake(s_apu_mu, portMAX_DELAY);
    w4_apuTone(freq, duration, volume, flags);
    xSemaphoreGive(s_apu_mu);
}

void disk_path(char *out, size_t n) { snprintf(out, n, "/sdcard/apps/%s/disk.w4", s_id); }

int32_t env_diskr(wasm_exec_env_t env, void *dest, uint32_t size) {
    if (!bound(env) || !dest) return 0;
    if (size > s_disk.size) size = s_disk.size;
    memcpy(dest, s_disk.data, size);
    return (int32_t)size;
}

int32_t env_diskw(wasm_exec_env_t env, const void *src, uint32_t size) {
    if (!bound(env) || !src) return 0;
    if (size > sizeof s_disk.data) size = sizeof s_disk.data;
    s_disk.size = (uint16_t)size;
    memcpy(s_disk.data, src, size);
    char path[80];
    disk_path(path, sizeof path);
    FILE *f = fopen(path, "wb");   // write-through: carts save on events, not every frame
    if (f) {
        fwrite(s_disk.data, 1, size, f);
        fclose(f);
    } else {
        NV_LOGW(TAG, "diskw: cannot write %s", path);
    }
    return (int32_t)size;
}

void env_trace(wasm_exec_env_t env, const char *str) {
    if (bound(env) && str && trace_ok()) NV_LOGI(TAG, "%s: %s", s_id, str);
}

void env_trace_utf8(wasm_exec_env_t env, const char *str, uint32_t len) {
    if (bound(env) && str && trace_ok()) NV_LOGI(TAG, "%s: %.*s", s_id, (int)len, str);
}

void env_trace_utf16(wasm_exec_env_t env, const char *str, uint32_t len) {
    if (!bound(env) || !str || !trace_ok()) return;
    char out[128];
    size_t o = 0;
    for (uint32_t i = 0; i + 1 < len && o + 1 < sizeof out; i += 2) {
        const uint16_t c = w4_read16LE(str + i);
        out[o++] = c < 128 ? (char)c : '?';
    }
    out[o] = '\0';
    NV_LOGI(TAG, "%s: %s", s_id, out);
}

// tracef(fmt, stack): printf over arguments the cart laid out in its memory (%c %d %x %s %f).
void env_tracef(wasm_exec_env_t env, const char *fmt, int32_t stack) {
    if (!bound(env) || !fmt || !trace_ok()) return;
    char out[192];
    size_t o = 0;
    uint64_t arg = (uint32_t)stack;
    auto put = [&](const char *s) { while (*s && o + 1 < sizeof out) out[o++] = *s++; };
    auto word = [&](uint32_t n, void *dst) -> bool {
        if (!wasm_runtime_validate_app_addr(s_inst, arg, n)) return false;
        memcpy(dst, wasm_runtime_addr_app_to_native(s_inst, arg), n);
        arg += n;
        return true;
    };
    for (const char *p = fmt; *p && o + 1 < sizeof out; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        char tmp[40];
        uint32_t v = 0;
        double d = 0;
        switch (*++p) {
        case '\0': p--; continue;
        case '%': out[o++] = '%'; continue;
        case 'c': if (!word(4, &v)) return; tmp[0] = (char)v; tmp[1] = 0; put(tmp); break;
        case 'd': if (!word(4, &v)) return; snprintf(tmp, sizeof tmp, "%ld", (long)(int32_t)v); put(tmp); break;
        case 'x': if (!word(4, &v)) return; snprintf(tmp, sizeof tmp, "%lx", (unsigned long)v); put(tmp); break;
        case 'f': if (!word(8, &d)) return; snprintf(tmp, sizeof tmp, "%g", d); put(tmp); break;
        case 's': {
            if (!word(4, &v)) return;
            // Bounded copy: validate byte by byte up to the output room, never past the memory.
            while (o + 1 < sizeof out && wasm_runtime_validate_app_addr(s_inst, v, 1)) {
                const char c = *(const char *)wasm_runtime_addr_app_to_native(s_inst, v++);
                if (!c) break;
                out[o++] = c;
            }
            break;
        }
        default: out[o++] = '%'; if (o + 1 < sizeof out) out[o++] = *p; break;
        }
    }
    out[o] = '\0';
    NV_LOGI(TAG, "%s: %s", s_id, out);
}

NativeSymbol s_env[] = {
    { "blit",       (void *)env_blit,        "(iiiiii)",    nullptr },
    { "blitSub",    (void *)env_blit_sub,    "(iiiiiiiii)", nullptr },
    { "line",       (void *)env_line,        "(iiii)",      nullptr },
    { "hline",      (void *)env_hline,       "(iii)",       nullptr },
    { "vline",      (void *)env_vline,       "(iii)",       nullptr },
    { "oval",       (void *)env_oval,        "(iiii)",      nullptr },
    { "rect",       (void *)env_rect,        "(iiii)",      nullptr },
    { "text",       (void *)env_text,        "($ii)",       nullptr },
    { "textUtf8",   (void *)env_text_utf8,   "(*~ii)",      nullptr },
    { "textUtf16",  (void *)env_text_utf16,  "(*~ii)",      nullptr },
    { "tone",       (void *)env_tone,        "(iiii)",      nullptr },
    { "diskr",      (void *)env_diskr,       "(*~)i",       nullptr },
    { "diskw",      (void *)env_diskw,       "(*~)i",       nullptr },
    { "trace",      (void *)env_trace,       "($)",         nullptr },
    { "traceUtf8",  (void *)env_trace_utf8,  "(*~)",        nullptr },
    { "traceUtf16", (void *)env_trace_utf16, "(*~)",        nullptr },
    { "tracef",     (void *)env_tracef,      "($i)",        nullptr },
};

// ---- audio ----------------------------------------------------------------------------------------
// A forever task (PSRAM stack: it never touches flash) that streams the APU while a cart is bound.
// The PCM ring holds ~3 s, far too much latency for game sound, so it only tops the queue up to
// ~50 ms ahead of the DAC.
constexpr int kRate = 44100, kBlock = 441;   // 10 ms per block
NV_PSRAM_BSS int16_t s_st[kBlock * 2];
NV_PSRAM_BSS int16_t s_mono[kBlock];

void audio_task(void *) {
    int16_t *st = s_st, *mono = s_mono;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_audio_run) continue;
        if (!nv_audio_pcm_begin_as(kRate, 1, 16, NV_PCM_SFX)) {
            NV_LOGW(TAG, "audio busy (music/voice playing): cart runs silent");
            continue;
        }
        s_audio_busy = true;
        while (s_audio_run) {
            if (nv_audio_pcm_backlog() > (size_t)kRate * 2 / 20) { vTaskDelay(pdMS_TO_TICKS(4)); continue; }
            xSemaphoreTake(s_apu_mu, portMAX_DELAY);
            w4_apuWriteSamples(st, kBlock);
            xSemaphoreGive(s_apu_mu);
            for (int i = 0; i < kBlock; i++) mono[i] = (int16_t)(((int32_t)st[2 * i] + st[2 * i + 1]) / 2);
            if (nv_audio_pcm_write(mono, sizeof s_mono) < 0) break;
        }
        nv_audio_pcm_flush();
        nv_audio_pcm_end();
        s_audio_busy = false;
    }
}

// ---- drawing helpers for the gamepad art -----------------------------------------------------------
void fill_rect(uint16_t *cv, int x, int y, int w, int h, uint16_t c) {
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < kW4CanvasH; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < kW4CanvasW; xx++) cv[yy * kW4CanvasW + xx] = c;
}

void fill_circle(uint16_t *cv, int cx, int cy, int r, uint16_t c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) {
                const int x = cx + dx, y = cy + dy;
                if (x >= 0 && x < kW4CanvasW && y >= 0 && y < kW4CanvasH) cv[y * kW4CanvasW + x] = c;
            }
}

// A glyph from the console's own font, magnified.
void glyph(uint16_t *cv, char ch, int cx, int cy, int scale, uint16_t c) {
    const uint8_t *g = w4_fontGlyph((unsigned char)ch);
    if (!g) return;
    const int x0 = cx - 4 * scale, y0 = cy - 4 * scale;
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (!(g[row] & (0x80 >> col))) fill_rect(cv, x0 + col * scale, y0 + row * scale, scale, scale, c);
}

// Bounded unsigned LEB128 read (the bytes come from an untrusted cart file).
bool leb_u32(const uint8_t *&p, const uint8_t *end, uint32_t &v) {
    v = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (p >= end) return false;
        const uint8_t b = *p++;
        v |= (uint32_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

}  // namespace

// ---- public ----------------------------------------------------------------------------------------

// A "limits" record: flags, min[, max]. For a memory (buf != NULL) a single-byte min of 0 pages
// becomes 1: a cart always gets the console's 64 KB (upstream hosts provide one page regardless).
static bool limits(uint8_t *buf, const uint8_t *&q, const uint8_t *end) {
    if (q >= end) return false;
    const uint8_t flags = *q++;
    if (buf && q < end && *q == 0x00) buf[q - buf] = 0x01;
    uint32_t v;
    if (!leb_u32(q, end, v)) return false;
    return !(flags & 1) || leb_u32(q, end, v);
}

void nv_w4_prepare_module(uint8_t *buf, uint32_t len) {
    if (!buf || len < 8 || memcmp(buf, "\0asm", 4) != 0) return;
    const uint8_t *p = buf + 8, *const end = buf + len;
    while (p < end) {
        const uint8_t id = *p++;
        uint32_t size, count, v;
        if (!leb_u32(p, end, size) || size > (uint32_t)(end - p)) return;
        const uint8_t *const sec_end = p + size;
        const uint8_t *q = p;
        if (id == 2) {   // imports: vec(module, name, kind, desc) — find the memory
            if (!leb_u32(q, sec_end, count)) return;
            for (uint32_t i = 0; i < count; i++) {
                for (int s = 0; s < 2; s++) {   // module name, field name
                    if (!leb_u32(q, sec_end, v) || v > (uint32_t)(sec_end - q)) return;
                    q += v;
                }
                if (q >= sec_end) return;
                const uint8_t kind = *q++;
                if (kind == 0 && !leb_u32(q, sec_end, v)) return;                               // func
                if (kind == 1 && (q++ >= sec_end || !limits(nullptr, q, sec_end))) return;      // table
                if (kind == 2 && !limits(buf, q, sec_end)) return;                              // memory
                if (kind == 3) q += 2;                                                          // global
                if (kind == 4 && (q++ >= sec_end || !leb_u32(q, sec_end, v))) return;          // tag
                if (kind > 4) return;
            }
        } else if (id == 5) {   // defined memory (rare for carts; toolchains may emit an empty one)
            if (!leb_u32(q, sec_end, count)) return;
            if (count > 0 && !limits(buf, q, sec_end)) return;
        } else if (id == 7) {   // exports: vec(name, kind, index)
            if (!leb_u32(q, sec_end, count)) return;
            for (uint32_t i = 0; i < count; i++) {
                if (!leb_u32(q, sec_end, v) || v > (uint32_t)(sec_end - q)) return;
                uint8_t *name = buf + (q - buf);
                // Same-length renames. _initialize/_start: see nv_wasm_w4.h. __heap_base/__data_end:
                // from those WAMR (SHRUNK_MEMORY) truncates a module that never calls memory.grow
                // to its heap base, but a cart owns all 64 KB and may use memory past it.
                if (v == 11 && !memcmp(name, "_initialize", 11))      memcpy(name, kW4Init, 11);
                else if (v == 6 && !memcmp(name, "_start", 6))        memcpy(name, kW4Start, 6);
                else if (v == 11 && !memcmp(name, "__heap_base", 11)) memcpy(name, "__w4_hbase_", 11);
                else if (v == 10 && !memcmp(name, "__data_end", 10))  memcpy(name, "__w4_dend_", 10);
                q += v;
                if (q >= sec_end) return;
                q++;   // export kind
                if (!leb_u32(q, sec_end, v)) return;
            }
            return;   // exports come after imports/memories: nothing left to patch
        }
        p = sec_end;
    }
}

bool nv_w4_init(void) {
    static bool done;
    if (done) return true;
    if (!s_apu_mu) s_apu_mu = xSemaphoreCreateMutex();
    if (!s_apu_mu) return false;
    if (!wasm_runtime_register_natives("env", s_env, sizeof s_env / sizeof s_env[0])) {
        NV_LOGE(TAG, "register_natives(env) failed");
        return false;
    }
    if (!s_audio_task &&
        xTaskCreateWithCaps(audio_task, "w4snd", 4096, nullptr, 4, &s_audio_task, MALLOC_CAP_SPIRAM) != pdPASS)
        NV_LOGW(TAG, "audio task not started: carts will be silent");
    done = true;
    return true;
}

namespace {

// Where the 160x160 screen sits on the canvas. With the touch controls shown it is 3x (480x480,
// integer, pixel-exact) between the D-pad and the buttons; when they're hidden (a USB keyboard or
// gamepad is connected, or the cart set SYSTEM_HIDE_GAMEPAD_OVERLAY) it fills the panel height
// instead: 600x600 at 3.75x, each console pixel 3 or 4 panel pixels wide.
struct Layout {
    int     x0, y0, side;
    int16_t at[161];   // offset (from x0 / y0) where console pixel i starts; at[160] = side
};
Layout s_lay;
bool    s_fit;
bool    s_redraw_screen, s_redraw_overlay;   // set by a layout change
bool    s_quit;                              // Esc on a keyboard, Select + Start on a gamepad
int     s_usb_x = -1, s_usb_y = -1, s_usb_b = -1;   // USB mouse state last applied

void set_layout(bool fit) {
    s_fit = fit;
    s_lay.side = fit ? kW4CanvasH : kW4Side;
    s_lay.x0 = (kW4CanvasW - s_lay.side) / 2;
    s_lay.y0 = (kW4CanvasH - s_lay.side) / 2;
    for (int i = 0; i <= 160; i++) s_lay.at[i] = (int16_t)(i * s_lay.side / 160);
    s_redraw_screen = s_redraw_overlay = true;
}

// USB keyboard -> GAMEPAD1, with the official WASM-4 runtime's keys: arrows (or WASD) for the
// D-pad, X / V / Space / Period for button 1, Z / C / N / Comma for button 2. Esc quits the cart.
uint8_t keyboard_pad(void) {
    uint8_t u[6];
    const int n = nv_hid_host_keys_down(u);
    uint8_t pad = 0;
    for (int i = 0; i < n; i++) {
        switch (u[i]) {
        case 0x52: case 0x1a: pad |= kUp; break;                        // Up, W
        case 0x51: case 0x16: pad |= kDown; break;                      // Down, S
        case 0x50: case 0x04: pad |= kLeft; break;                      // Left, A
        case 0x4f: case 0x07: pad |= kRight; break;                     // Right, D
        case 0x1b: case 0x19: case 0x2c: case 0x37: pad |= kBtn1; break;   // X, V, Space, Period
        case 0x1d: case 0x06: case 0x11: case 0x36: pad |= kBtn2; break;   // Z, C, N, Comma
        case 0x29: s_quit = true; break;                                // Esc
        default: break;
        }
    }
    return pad;
}

// USB gamepad -> one console gamepad. The D-pad comes from the stick, hat or D-pad; the face
// buttons alternate (HID 1, 3, 5 -> button 1; 2, 4, 6 -> button 2), since their order differs
// from model to model and this way every one of them does something. Select + Start (HID 9 and
// 10 on most pads) held together quit the cart, like Esc.
uint8_t gamepad_bits(int index) {
    uint8_t dirs;
    uint32_t b;
    if (!nv_hid_host_gamepad_state(index, &dirs, &b)) return 0;
    uint8_t v = 0;
    if (dirs & NV_HID_DIR_UP)    v |= kUp;
    if (dirs & NV_HID_DIR_DOWN)  v |= kDown;
    if (dirs & NV_HID_DIR_LEFT)  v |= kLeft;
    if (dirs & NV_HID_DIR_RIGHT) v |= kRight;
    if (b & 0x15) v |= kBtn1;
    if (b & 0x2a) v |= kBtn2;
    if ((b & 0x300) == 0x300) s_quit = true;
    return v;
}

// Canvas coordinate -> console coordinate, rounding down (off-screen values stay off-screen).
int16_t to_console(int v, int origin) {
    const int d = (v - origin) * 160;
    return (int16_t)(d >= 0 ? d / s_lay.side : -((-d + s_lay.side - 1) / s_lay.side));
}

}  // namespace

bool nv_w4_begin(wasm_module_inst_t inst, const char *app_id, char *err, size_t err_n) {
    // Exactly one 64 KB page that can't grow (nv_wasm instantiates carts with max 1 page): the
    // console state and the rasterizer's pointers live at fixed offsets into it.
    wasm_memory_inst_t mem = wasm_runtime_get_default_memory(inst);
    const uint64_t bytes = mem ? wasm_memory_get_bytes_per_page(mem) * wasm_memory_get_cur_page_count(mem) : 0;
    const uint64_t max_pages = mem ? wasm_memory_get_max_page_count(mem) : 0;
    if (bytes != kMemSize || max_pages != 1) {
        snprintf(err, err_n, "not a WASM-4 cart: memory is %lu bytes (max %lu pages), needs exactly 64 KB",
                 (unsigned long)bytes, (unsigned long)max_pages);
        return false;
    }
    s_inst = inst;
    s_mem  = (uint8_t *)wasm_runtime_addr_app_to_native(inst, 0);
    snprintf(s_id, sizeof s_id, "%s", app_id);

    // Power-on state (upstream runtime.c). The rest of memory is the cart's own data segments,
    // already placed by instantiation, so unlike the upstream host we don't zero all 64 KB.
    memset(s_mem, 0, kFramebuffer + kFbBytes);
    w4_write32LE(s_mem + kPalette + 0,  0xe0f8cf);
    w4_write32LE(s_mem + kPalette + 4,  0x86c06c);
    w4_write32LE(s_mem + kPalette + 8,  0x306850);
    w4_write32LE(s_mem + kPalette + 12, 0x071821);
    s_mem[kDrawColors]     = 0x03;
    s_mem[kDrawColors + 1] = 0x12;
    s_mouse_x = s_mouse_y = 0x7fff;
    w4_write16LE(s_mem + kMouseX, (uint16_t)s_mouse_x);
    w4_write16LE(s_mem + kMouseY, (uint16_t)s_mouse_y);
    w4_framebufferInit(s_mem + kDrawColors, s_mem + kFramebuffer);

    s_disk.size = 0;
    char path[80];
    disk_path(path, sizeof path);
    if (FILE *f = fopen(path, "rb")) {
        s_disk.size = (uint16_t)fread(s_disk.data, 1, sizeof s_disk.data, f);
        fclose(f);
    }

    s_have_last = false;
    s_quit = false;
    s_usb_x = s_usb_y = s_usb_b = -1;
    set_layout(false);
    s_trace_window_us = 0;
    s_trace_count = 0;

    xSemaphoreTake(s_apu_mu, portMAX_DELAY);
    w4_apuInit();
    xSemaphoreGive(s_apu_mu);
    s_audio_run = true;
    if (s_audio_task) xTaskNotifyGive(s_audio_task);
    return true;
}

void nv_w4_end(void) {
    s_audio_run = false;
    for (int i = 0; i < 100 && s_audio_busy; i++) vTaskDelay(pdMS_TO_TICKS(5));   // stream closed
    // Silence every channel for the next cart (the APU keeps absolute sample times).
    xSemaphoreTake(s_apu_mu, portMAX_DELAY);
    for (int ch = 0; ch < 4; ch++) w4_apuTone(0, 0, 0, ch);
    xSemaphoreGive(s_apu_mu);
    s_inst = nullptr;
    s_mem  = nullptr;
}

void nv_w4_input(const int *xs, const int *ys, int n) {
    if (!s_mem) return;
    const bool kb = nv_hid_host_keyboard_present();
    const int pads = nv_hid_host_gamepad_count();
    const bool fit = kb || pads || (s_mem[kSystemFlags] & kSysHideGamepad);
    if (fit != s_fit) set_layout(fit);
    // Player 1 is the touch pad, the keyboard and the first USB gamepad together; gamepads 2-4
    // are players 2-4 (local multiplayer carts read GAMEPAD2..4).
    uint8_t pad = kb ? keyboard_pad() : 0, others[3] = {0, 0, 0};
    if (pads > 0) pad |= gamepad_bits(0);
    for (int i = 1; i < pads && i < 4; i++) others[i - 1] = gamepad_bits(i);
    uint8_t buttons = 0;
    bool on_screen = false;
    for (int i = 0; i < n; i++) {
        const int x = xs[i], y = ys[i];
        if (x >= s_lay.x0 && x < s_lay.x0 + s_lay.side && y >= s_lay.y0 && y < s_lay.y0 + s_lay.side) {
            on_screen = true;                     // a finger on the screen is the mouse
            buttons |= kMouseLeft;
            s_mouse_x = to_console(x, s_lay.x0);
            s_mouse_y = to_console(y, s_lay.y0);
            continue;
        }
        if (s_fit) continue;                      // no controls drawn, nothing to press
        const int dx = x - kPadX, dy = y - kPadY, d2 = dx * dx + dy * dy;
        if (d2 <= kPadReach * kPadReach && d2 >= kPadDead * kPadDead) {
            const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            // 8-way: an axis counts when it is within ~67.5 deg of the touch direction (tan 22.5 = .414).
            if (ax * 1000 >= ay * 414) pad |= dx < 0 ? kLeft : kRight;
            if (ay * 1000 >= ax * 414) pad |= dy < 0 ? kUp : kDown;
        }
        const int bx = x - kBtnXx, by = y - kBtnXy, zx = x - kBtnZx, zy = y - kBtnZy;
        if (bx * bx + by * by <= kBtnReach * kBtnReach) pad |= kBtn1;
        if (zx * zx + zy * zy <= kBtnReach * kBtnReach) pad |= kBtn2;
    }
    // A USB mouse also moves the console mouse without clicking (hover) and has all three
    // buttons (HID and WASM-4 agree: 1 left, 2 right, 4 middle). It takes over the position only
    // when it moves, so a touch position still sticks after the finger lifts.
    int mx, my;
    uint8_t mb;
    if (nv_hid_host_mouse_state(&mx, &my, &mb)) {
        mb &= 0x07;
        if (!on_screen && (mx != s_usb_x || my != s_usb_y || mb != s_usb_b)) {
            s_mouse_x = to_console(mx, s_lay.x0);
            s_mouse_y = to_console(my, s_lay.y0);
        }
        s_usb_x = mx; s_usb_y = my; s_usb_b = mb;
        buttons |= mb;
    }
    s_mem[kGamepads] = pad;
    for (int i = 0; i < 3; i++) s_mem[kGamepads + 1 + i] = others[i];
    w4_write16LE(s_mem + kMouseX, (uint16_t)s_mouse_x);   // WASM-4 keeps the last position
    w4_write16LE(s_mem + kMouseY, (uint16_t)s_mouse_y);
    s_mem[kMouseButtons] = buttons;
}

bool nv_w4_quit_requested(void) {
    const bool q = s_quit;
    s_quit = false;
    return q;
}

void nv_w4_screen_rect(int *x0, int *y0, int *side) {
    *x0 = s_lay.x0;
    *y0 = s_lay.y0;
    *side = s_lay.side;
}

void nv_w4_frame_begin(bool first) {
    if (s_mem && !first && !(s_mem[kSystemFlags] & kSysPreserveFb)) w4_framebufferClear();
}

void nv_w4_frame_end(void) {
    xSemaphoreTake(s_apu_mu, portMAX_DELAY);
    w4_apuTick();
    xSemaphoreGive(s_apu_mu);
}

// Only the bounding box of what changed is upscaled and handed back for the re-blit. PSRAM
// bandwidth is the scarce resource here: the DSI panel streams its framebuffer from PSRAM all the
// time, and when our writes + LVGL's blit starve it the bridge underruns and the panel flashes
// blue (esp_lcd_panel_dpi.c). A typical cart moves a few sprites, so the box is small.
bool nv_w4_render(uint16_t *cv, bool force, int rect[4], bool defer_large) {
    if (!s_mem || !cv) return false;
    const uint8_t *fb = s_mem + kFramebuffer;
    uint32_t pal[4];
    for (int i = 0; i < 4; i++) pal[i] = w4_read32LE(s_mem + kPalette + 4 * i) & 0xffffff;
    force = force || s_redraw_screen;          // a layout change redraws at once
    int y0 = 0, y1 = 159, b0 = 0, b1 = 39;   // dirty rows, dirty byte columns (4 px each)
    if (!force && s_have_last && !memcmp(pal, s_last_pal, sizeof pal)) {
        y0 = 160; y1 = -1; b0 = 40; b1 = -1;
        for (int y = 0; y < 160; y++) {
            const uint8_t *a = fb + y * 40, *b = s_last_fb + y * 40;
            if (!memcmp(a, b, 40)) continue;
            int l = 0, r = 39;
            while (a[l] == b[l]) l++;
            while (a[r] == b[r]) r--;
            if (y < y0) y0 = y;
            y1 = y;
            if (l < b0) b0 = l;
            if (r > b1) b1 = r;
        }
        if (y1 < 0) return false;   // static screen: nothing to re-blit
    }
    // A big change (scrolling, full redraws, palette changes) is shown at most every other frame:
    // the snapshot isn't updated, so the next call still sees it.
    if (defer_large && !force && (y1 - y0 + 1) * (b1 - b0 + 1) * 10 > 160 * 40 * 4) return false;
    memcpy(s_last_pal, pal, sizeof pal);
    memcpy(s_last_fb, fb, kFbBytes);
    s_have_last = true;
    s_redraw_screen = false;

    uint16_t c565[4];
    for (int i = 0; i < 4; i++)
        c565[i] = (uint16_t)(((pal[i] >> 8) & 0xf800) | ((pal[i] >> 5) & 0x07e0) | ((pal[i] >> 3) & 0x001f));
    // One upscaled row segment (lowest 2 bits = leftmost px), copied to every canvas row that
    // console row covers: 3 per pixel, or 3-4 in the 600 px layout.
    const int px0 = b0 * 4, px1 = (b1 + 1) * 4;
    const int cx = s_lay.at[px0], cw = s_lay.at[px1] - cx;
    uint16_t row[kW4CanvasH];   // the widest layout: 600 px
    for (int y = y0; y <= y1; y++) {
        const uint8_t *src = fb + y * 40;
        uint16_t *o = row;
        for (int p = px0; p < px1; p++) {
            const uint16_t c = c565[(src[p >> 2] >> ((p & 3) * 2)) & 3];
            for (int k = s_lay.at[p + 1] - s_lay.at[p]; k > 0; k--) *o++ = c;
        }
        uint16_t *dst = cv + (s_lay.y0 + s_lay.at[y]) * kW4CanvasW + s_lay.x0 + cx;
        for (int r = s_lay.at[y]; r < s_lay.at[y + 1]; r++, dst += kW4CanvasW) memcpy(dst, row, (size_t)cw * 2);
    }
    rect[0] = s_lay.x0 + cx;
    rect[1] = s_lay.y0 + s_lay.at[y0];
    rect[2] = cw;
    rect[3] = s_lay.at[y1 + 1] - s_lay.at[y0];
    return true;
}

namespace {

constexpr uint16_t kOvBg = 0x0000, kOvFrame = 0x2945, kOvKey = 0x4a69, kOvLit = 0xa534, kOvInk = 0xce79;
constexpr uint8_t  kDirs = kLeft | kRight | kUp | kDown;

void draw_dpad(uint16_t *cv, uint8_t pad) {
    fill_rect(cv, kPadX - kPadArm, kPadY - kPadHalf, 2 * kPadArm, 2 * kPadHalf, kOvKey);
    fill_rect(cv, kPadX - kPadHalf, kPadY - kPadArm, 2 * kPadHalf, 2 * kPadArm, kOvKey);
    const int arm = kPadArm - kPadHalf;   // one arm, outside the centre square
    if (pad & kLeft)  fill_rect(cv, kPadX - kPadArm, kPadY - kPadHalf, arm, 2 * kPadHalf, kOvLit);
    if (pad & kRight) fill_rect(cv, kPadX + kPadHalf, kPadY - kPadHalf, arm, 2 * kPadHalf, kOvLit);
    if (pad & kUp)    fill_rect(cv, kPadX - kPadHalf, kPadY - kPadArm, 2 * kPadHalf, arm, kOvLit);
    if (pad & kDown)  fill_rect(cv, kPadX - kPadHalf, kPadY + kPadHalf, 2 * kPadHalf, arm, kOvLit);
    fill_circle(cv, kPadX, kPadY, 12, kOvFrame);
}

void draw_button(uint16_t *cv, int x, int y, char label, bool lit) {
    fill_circle(cv, x, y, kBtnR, lit ? kOvLit : kOvKey);
    glyph(cv, label, x, y, 5, lit ? kOvKey : kOvInk);
}

void rect_union(int r[4], int x, int y, int w, int h) {
    if (r[2] <= 0) { r[0] = x; r[1] = y; r[2] = w; r[3] = h; return; }
    const int x1 = r[0] + r[2] > x + w ? r[0] + r[2] : x + w, y1 = r[1] + r[3] > y + h ? r[1] + r[3] : y + h;
    if (x < r[0]) r[0] = x;
    if (y < r[1]) r[1] = y;
    r[2] = x1 - r[0];
    r[3] = y1 - r[1];
}

int s_pad_drawn = -1;   // gamepad bits the controls were last drawn with

}  // namespace

// Full art on the first frame and on every layout change (controls shown / hidden); otherwise
// only the controls whose pressed state changed (the rect stays one control, not the canvas).
bool nv_w4_render_overlay(uint16_t *cv, bool force, int rect[4]) {
    if (!s_mem || !cv) return false;
    const uint8_t pad = s_mem[kGamepads];
    if (force || s_redraw_overlay) {
        s_redraw_overlay = false;
        const int x0 = s_lay.x0, y0 = s_lay.y0, sd = s_lay.side;
        fill_rect(cv, 0, 0, x0, kW4CanvasH, kOvBg);                                       // margins
        fill_rect(cv, x0 + sd, 0, kW4CanvasW - x0 - sd, kW4CanvasH, kOvBg);
        fill_rect(cv, x0, 0, sd, y0, kOvBg);
        fill_rect(cv, x0, y0 + sd, sd, kW4CanvasH - y0 - sd, kOvBg);
        if (y0 >= 3) {                                                                    // bezel
            fill_rect(cv, x0 - 3, y0 - 3, sd + 6, 3, kOvFrame);
            fill_rect(cv, x0 - 3, y0 + sd, sd + 6, 3, kOvFrame);
        }
        fill_rect(cv, x0 - 3, y0, 3, sd, kOvFrame);
        fill_rect(cv, x0 + sd, y0, 3, sd, kOvFrame);
        if (!s_fit) {
            draw_dpad(cv, pad);
            draw_button(cv, kBtnXx, kBtnXy, 'X', pad & kBtn1);
            draw_button(cv, kBtnZx, kBtnZy, 'Z', pad & kBtn2);
        }
        s_pad_drawn = pad;
        rect[0] = 0; rect[1] = 0; rect[2] = kW4CanvasW; rect[3] = kW4CanvasH;
        return true;
    }
    if (s_fit || pad == s_pad_drawn) return false;
    const uint8_t diff = (uint8_t)(pad ^ s_pad_drawn);
    s_pad_drawn = pad;
    rect[2] = 0;
    if (diff & kDirs) {
        draw_dpad(cv, pad);
        rect_union(rect, kPadX - kPadArm, kPadY - kPadArm, 2 * kPadArm, 2 * kPadArm);
    }
    if (diff & kBtn1) {
        draw_button(cv, kBtnXx, kBtnXy, 'X', pad & kBtn1);
        rect_union(rect, kBtnXx - kBtnR, kBtnXy - kBtnR, 2 * kBtnR + 1, 2 * kBtnR + 1);
    }
    if (diff & kBtn2) {
        draw_button(cv, kBtnZx, kBtnZy, 'Z', pad & kBtn2);
        rect_union(rect, kBtnZx - kBtnR, kBtnZy - kBtnR, 2 * kBtnR + 1, 2 * kBtnR + 1);
    }
    return rect[2] > 0;
}
