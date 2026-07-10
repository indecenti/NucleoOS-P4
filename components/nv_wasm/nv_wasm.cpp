// nv_wasm — WAMR bring-up, host-import ABI v1, manifest v2, async runner. See nv_wasm.h.
#include "nv_wasm.h"
#include "nv_log.h"
#include "nv_sd.h"
#include "nv_i18n.h"
#include "nv_audio.h"
#include "nv_hal.h"       // nv_hal_backlight_set — ABI v4 nv.backlight
#include "nv_config.h"    // restore the user's brightness when a backlight app exits
#include "nv_tts.h"
#include "nv_memory_broker.h"

#include "wasm_export.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "wasm";

namespace {

bool s_ready = false;

constexpr size_t   kMaxModuleSize   = 2 * 1024 * 1024;   // app.wasm cap (PSRAM-backed)
constexpr uint32_t kMinHeap         = 64 * 1024;
constexpr uint32_t kMaxHeap         = 8 * 1024 * 1024;
constexpr uint32_t kMinStackKb      = 4,    kMaxStackKb   = 256,     kDefStackKb   = 16;
constexpr uint32_t kMinTimeoutMs    = 1000, kMaxTimeoutMs = 120000,  kDefTimeoutMs = 10000;
constexpr size_t   kWorkerNativeStk = 48 * 1024;          // pthread stack for the interp loop
constexpr size_t   kOutCap          = 4096;               // per-run nv.print buffer
constexpr int      kInstrBudget     = 200'000'000;        // hard per-run opcode ceiling (anti-wedge)

struct Exec;   // fwd

// One WASM invocation, carried to the worker pthread (WAMR pthread_self requires a pthread) and
// set as exec-env user-data so host imports can find their run's permissions + output sinks.
struct RunReq {
    const uint8_t *mod;
    uint32_t       mod_size;
    const char    *fn;
    uint32_t       argv[4];
    uint32_t       argc;
    uint32_t       perms;        // granted capabilities (OR of nv_wperm_t)
    uint32_t       heap_size;    // module instance heap
    uint32_t       stack_size;   // WASM operand stack
    bool           game;         // ABI v2 game run: no opcode cap (present() is the liveness point)
    char           tag[44];      // log tag ("wasmapp" or "app:<id>")
    Exec          *ex;           // async sink; NULL on the synchronous demo paths
    char          *out;          // sync fallback sink (demo paths)
    size_t         out_n;
    size_t         out_len;
    bool           ok;
    char           err[128];
};

// The single async run (solo-mode: one app at a time). UI side polls under `lock`; the worker
// and host imports write under the same lock. Nothing here touches LVGL.
struct Exec {
    pthread_mutex_t     lock = PTHREAD_MUTEX_INITIALIZER;
    nv_wrun_state_t     state = NV_WRUN_IDLE;
    pthread_t           thread{};
    bool                thread_valid = false;
    nv_wasm_app_t       app{};
    uint8_t            *bytes = nullptr;      // loaded app.wasm (freed on collect)
    wasm_module_inst_t  inst = nullptr;       // valid only while the worker runs (for terminate)
    bool                abort_req = false;
    char               *outbuf = nullptr;     // linear per-run buffer (PSRAM), reset each start
    size_t              out_w = 0, out_r = 0;
    bool                out_trunc = false, out_trunc_told = false;
    struct { int kind; char msg[64]; } toasts[4];
    int                 toast_n = 0;
    int64_t             t_start_us = 0;
    uint32_t            elapsed_ms = 0;
    RunReq              req{};
};
Exec s_exec;

// ---- ABI v2 game surface -----------------------------------------------------------------------
// A double-buffered RGB565 canvas shared between the guest (worker thread, via the nv.gfx_* draw
// imports) and the UI (LVGL thread, which shows finished frames). The guest draws into buf[draw_idx]
// and calls gfx_present(): the frame is published (ready_idx) and the worker blocks until the UI
// has consumed it (frame pacing). draw_idx then flips, so the UI can keep rendering the shown
// buffer while the guest fills the other one — two buffers, no tearing, no per-pixel locking.
// Shared flags (frame_ready / ready_idx / want_stop / input) are guarded by s_exec.lock; the draw
// commands touch only buf[draw_idx], which the UI never writes, so they run lock-free.
struct Gfx {
    bool      open      = false;
    int       w         = 0, h = 0;
    uint16_t *buf[2]    = {nullptr, nullptr};
    int       cap_px    = 0;
    int       draw_idx  = 0;      // worker draws here (worker-only)
    int       ready_idx = 0;      // UI shows here (guarded)
    bool      dirty     = true;   // worker-only: any draw since last present (skip republish when clean)
    bool      frame_ready = false;
    bool      want_stop   = false;
    int       in_x = 0, in_y = 0, in_state = 0;
    int       back_req    = 0;      // OS back-gesture requests, consumed by nv_gfx_back()
    bool      bl_touched  = false;  // ABI v4: app changed the backlight -> restore user brightness on exit
    volatile uint32_t present_seq = 0;   // liveness heartbeat: bumped on every present (see nv_wasm.h)
    // Multi-touch snapshot (canvas coords), pushed each frame by the UI via nv_wasm_gfx_set_multi.
    // Parallel to in_x/in_y (finger 0): games that want >1 finger read gfx_touch_count/_point.
    int       mt_x[5] = {}, mt_y[5] = {}, mt_cnt = 0;
};
Gfx s_gfx;

inline uint16_t *gfx_dst(void) { s_gfx.dirty = true; return s_gfx.buf[s_gfx.draw_idx]; }
inline void gfx_px(int x, int y, uint16_t c) {
    if ((unsigned)x < (unsigned)s_gfx.w && (unsigned)y < (unsigned)s_gfx.h)
        s_gfx.buf[s_gfx.draw_idx][y * s_gfx.w + x] = c;
}

// Allocate (or reuse) the two canvas buffers for a game of w×h. UI thread, before the worker runs.
bool gfx_open(int w, int h) {
    if (w < 16) w = 16;
    if (w > 1024) w = 1024;
    if (h < 16) h = 16;
    if (h > 600) h = 600;
    const int px = w * h;
    if (!s_gfx.buf[0] || !s_gfx.buf[1] || s_gfx.cap_px < px) {
        for (int i = 0; i < 2; i++) { if (s_gfx.buf[i]) heap_caps_free(s_gfx.buf[i]); s_gfx.buf[i] = nullptr; }
        for (int i = 0; i < 2; i++) {
            s_gfx.buf[i] = (uint16_t *)heap_caps_aligned_calloc(64, 1, (size_t)px * 2,
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_gfx.buf[i]) {
                for (int j = 0; j < 2; j++) { if (s_gfx.buf[j]) heap_caps_free(s_gfx.buf[j]); s_gfx.buf[j] = nullptr; }
                s_gfx.cap_px = 0;
                return false;
            }
        }
        s_gfx.cap_px = px;
    } else {
        memset(s_gfx.buf[0], 0, (size_t)px * 2);
        memset(s_gfx.buf[1], 0, (size_t)px * 2);
    }
    s_gfx.w = w; s_gfx.h = h;
    s_gfx.draw_idx = 0; s_gfx.ready_idx = 0;
    s_gfx.frame_ready = false; s_gfx.want_stop = false;
    s_gfx.dirty = true;
    s_gfx.in_x = s_gfx.in_y = s_gfx.in_state = 0;
    s_gfx.back_req = 0;
    s_gfx.open = true;
    return true;
}

// ---- host imports (the "syscalls" a WASM app may call) -----------------------------------------
// WAMR signature '$' delivers a validated, NUL-terminated native char* into app memory and '*~'
// a validated (ptr,len) buffer — an out-of-bounds pointer traps the module, never the OS. The
// worker is NOT the LVGL thread, so imports never touch UI: prints/toasts are queued under the
// exec lock and drained by the UI from an LVGL timer.

RunReq *req_of(wasm_exec_env_t env) {
    return static_cast<RunReq *>(wasm_runtime_get_user_data(env));
}

bool perm_ok(RunReq *r, uint32_t bit, const char *what) {
    if (r && (r->perms & bit)) return true;
    NV_LOGW(r ? r->tag : TAG, "%s denied (missing permission)", what);
    return false;
}

void emit_out(RunReq *r, const char *s) {
    if (!r || !s) return;
    const size_t len = strlen(s);
    if (r->ex) {
        Exec *ex = r->ex;
        pthread_mutex_lock(&ex->lock);
        if (ex->outbuf) {
            size_t room = kOutCap - ex->out_w;
            size_t k = len + 1 <= room ? len : (room ? room - 1 : 0);   // keep room for '\n'
            if (k < len) ex->out_trunc = true;
            memcpy(ex->outbuf + ex->out_w, s, k);
            ex->out_w += k;
            if (ex->out_w < kOutCap) ex->outbuf[ex->out_w++] = '\n';
        }
        pthread_mutex_unlock(&ex->lock);
    } else if (r->out && r->out_len < r->out_n) {
        int k = snprintf(r->out + r->out_len, r->out_n - r->out_len, "%s\n", s);
        if (k > 0) r->out_len += (size_t)k;
    }
}

// legacy slice-1 import (kept so already-installed apps keep running)
void host_log(wasm_exec_env_t env, int32_t v) {
    RunReq *r = req_of(env);
    if (!perm_ok(r, NV_WPERM_LOG, "env.host_log")) return;
    NV_LOGI(r->tag, "host_log(%d)", (int)v);
    char line[32];
    snprintf(line, sizeof line, "host_log(%d)", (int)v);
    emit_out(r, line);
}

void nvi_print(wasm_exec_env_t env, const char *msg) {
    RunReq *r = req_of(env);
    if (!perm_ok(r, NV_WPERM_LOG, "nv.print")) return;
    emit_out(r, msg ? msg : "");
}

void nvi_log(wasm_exec_env_t env, int32_t level, const char *msg) {
    RunReq *r = req_of(env);
    if (!perm_ok(r, NV_WPERM_LOG, "nv.log")) return;
    if (!msg) msg = "";
    switch (level) {
        case 0:  NV_LOGE(r->tag, "%s", msg); break;
        case 1:  NV_LOGW(r->tag, "%s", msg); break;
        case 3:  NV_LOGD(r->tag, "%s", msg); break;
        default: NV_LOGI(r->tag, "%s", msg); break;
    }
}

void nvi_toast(wasm_exec_env_t env, int32_t kind, const char *msg) {
    RunReq *r = req_of(env);
    if (!perm_ok(r, NV_WPERM_UI, "nv.toast")) return;
    if (!msg || !msg[0]) return;
    if (!r->ex) { NV_LOGI(r->tag, "toast(%d): %s", (int)kind, msg); return; }
    Exec *ex = r->ex;
    pthread_mutex_lock(&ex->lock);
    if (ex->toast_n < (int)(sizeof(ex->toasts) / sizeof(ex->toasts[0]))) {
        auto &t = ex->toasts[ex->toast_n++];
        t.kind = kind < 0 ? 0 : (kind > 3 ? 3 : kind);
        snprintf(t.msg, sizeof t.msg, "%s", msg);
    } else {
        NV_LOGW(r->tag, "toast queue full, dropped: %s", msg);
    }
    pthread_mutex_unlock(&ex->lock);
}

int32_t nvi_millis(wasm_exec_env_t) {
    return (int32_t)(esp_timer_get_time() / 1000);
}

int64_t nvi_time_unix(wasm_exec_env_t) {
    return (int64_t)time(nullptr);
}

int32_t nvi_lang(wasm_exec_env_t, char *buf, uint32_t len) {
    static const char *codes[NV_LANG_COUNT] = {"en", "it", "es", "fr", "de"};
    const nv_lang_t l = nv_i18n_get_lang();
    const char *code = (l >= 0 && l < NV_LANG_COUNT) ? codes[l] : "en";
    if (!buf || len == 0) return 0;
    const int k = snprintf(buf, len, "%s", code);
    return k < 0 ? 0 : (k >= (int)len ? (int32_t)len - 1 : k);
}

int32_t nvi_rand(wasm_exec_env_t) {
    return (int32_t)esp_random();
}

void nvi_sleep_ms(wasm_exec_env_t env, int32_t ms) {
    if (ms <= 0) return;
    // Honor abort/timeout. A guest that sleeps between opcodes (while(1){ nv_sleep_ms(); }) never
    // burns enough opcodes to hit the instruction cap, so it would ignore Stop / the manifest
    // watchdog forever. When abort_req is set, return immediately: the loop then spins freely and
    // trips the instruction limit within seconds, the worker reaches DONE, and the engine frees up.
    RunReq *r = req_of(env);
    if (r && r->ex) {
        pthread_mutex_lock(&r->ex->lock);
        const bool ab = r->ex->abort_req;
        pthread_mutex_unlock(&r->ex->lock);
        if (ab) return;
    }
    if (ms > 1000) ms = 1000;   // keep runs responsive; the manifest timeout is the real bound
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// 5x7 row-major font (bit4 = leftmost) for gfx_text — space, 0-9, A-Z, and HUD symbols.
static const uint8_t FONT5x7[][7] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // ' '
    { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e },  // '0'
    { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e },  // '1'
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f },  // '2'
    { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e },  // '3'
    { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 },  // '4'
    { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e },  // '5'
    { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e },  // '6'
    { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },  // '7'
    { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e },  // '8'
    { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c },  // '9'
    { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 },  // 'A'
    { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e },  // 'B'
    { 0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e },  // 'C'
    { 0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c },  // 'D'
    { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f },  // 'E'
    { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 },  // 'F'
    { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f },  // 'G'
    { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 },  // 'H'
    { 0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e },  // 'I'
    { 0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0c },  // 'J'
    { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 },  // 'K'
    { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f },  // 'L'
    { 0x11, 0x1b, 0x15, 0x11, 0x11, 0x11, 0x11 },  // 'M'
    { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 },  // 'N'
    { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e },  // 'O'
    { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 },  // 'P'
    { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d },  // 'Q'
    { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 },  // 'R'
    { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e },  // 'S'
    { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },  // 'T'
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e },  // 'U'
    { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 },  // 'V'
    { 0x11, 0x11, 0x11, 0x15, 0x15, 0x1b, 0x11 },  // 'W'
    { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 },  // 'X'
    { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 },  // 'Y'
    { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f },  // 'Z'
    { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 },  // '-'
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06 },  // '.'
    { 0x00, 0x06, 0x06, 0x00, 0x06, 0x06, 0x00 },  // ':'
    { 0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13 },  // '%'
    { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 },  // '/'
    { 0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10 },  // '>'
    { 0x01, 0x02, 0x04, 0x08, 0x04, 0x02, 0x01 },  // '<'
    { 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04 },  // '!'
    { 0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00 },  // '+'
    { 0x00, 0x00, 0x11, 0x0a, 0x04, 0x0a, 0x11 },  // 'x'
};
int font_glyph(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c == ' ') return 0;
    if (c >= '0' && c <= '9') return 1 + (c - '0');
    if (c >= 'A' && c <= 'Z') return 11 + (c - 'A');
    switch (c) {
        case '-': return 37; case '.': return 38; case ':': return 39; case '%': return 40;
        case '/': return 41; case '>': return 42; case '<': return 43; case '!': return 44;
        case '+': return 45; case 'x': return 46;
        default:  return 0;
    }
}

// ---- ABI v2 game-surface imports (permission "gfx"). Draw ops run on the worker and touch only
// buf[draw_idx]; coordinates are clamped so a wild guest value can never write out of the buffer.
bool gfx_perm(wasm_exec_env_t env) {
    RunReq *r = req_of(env);
    return r && (r->perms & NV_WPERM_GFX) && s_gfx.open;
}

int32_t nvi_gfx_width(wasm_exec_env_t env)  { return gfx_perm(env) ? s_gfx.w : 0; }
int32_t nvi_gfx_height(wasm_exec_env_t env) { return gfx_perm(env) ? s_gfx.h : 0; }

void nvi_gfx_clear(wasm_exec_env_t env, int32_t color) {
    if (!gfx_perm(env)) return;
    uint16_t c = (uint16_t)color;
    uint16_t *b = gfx_dst();
    const int n = s_gfx.w * s_gfx.h;
    for (int i = 0; i < n; i++) b[i] = c;
}
void nvi_gfx_rect(wasm_exec_env_t env, int32_t x, int32_t y, int32_t w, int32_t h, int32_t color) {
    if (!gfx_perm(env)) return;
    s_gfx.dirty = true;
    uint16_t c = (uint16_t)color;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > s_gfx.w) x1 = s_gfx.w;
    if (y1 > s_gfx.h) y1 = s_gfx.h;
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = &s_gfx.buf[s_gfx.draw_idx][yy * s_gfx.w];
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}
void nvi_gfx_circle(wasm_exec_env_t env, int32_t cx, int32_t cy, int32_t r, int32_t color) {
    if (!gfx_perm(env) || r <= 0) return;
    s_gfx.dirty = true;
    int rmax = s_gfx.w + s_gfx.h;
    if (r > rmax) r = rmax;   // cap hostile r: keeps r*r in int range, bounds the loop (off-screen anyway)
    uint16_t c = (uint16_t)color;
    for (int dy = -r; dy <= r; dy++) {
        int yy = cy + dy;
        if ((unsigned)yy >= (unsigned)s_gfx.h) continue;
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        int x0 = cx - dx, x1 = cx + dx;
        if (x0 < 0) x0 = 0;
        if (x1 >= s_gfx.w) x1 = s_gfx.w - 1;
        uint16_t *row = &s_gfx.buf[s_gfx.draw_idx][yy * s_gfx.w];
        for (int xx = x0; xx <= x1; xx++) row[xx] = c;
    }
}
void nvi_gfx_line(wasm_exec_env_t env, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t color) {
    if (!gfx_perm(env)) return;
    s_gfx.dirty = true;
    uint16_t c = (uint16_t)color;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_px(x0, y0, c); gfx_px(x0 + 1, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
// Filled triangle, scanline rasterized natively — one host call instead of the guest issuing dozens
// of gfx_rect rows, which crushed WASM->native call overhead for icon/starfield art.
static inline void tri_edge_native(int ax, int ay, int bx, int by, int y, int *lo, int *hi) {
    if (ay == by) return;
    if (ay > by) { int t = ax; ax = bx; bx = t; t = ay; ay = by; by = t; }
    if (y < ay || y > by) return;
    int x = ax + (bx - ax) * (y - ay) / (by - ay);
    if (x < *lo) *lo = x;
    if (x > *hi) *hi = x;
}
void nvi_gfx_tri(wasm_exec_env_t env, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                 int32_t x2, int32_t y2, int32_t color) {
    if (!gfx_perm(env)) return;
    s_gfx.dirty = true;
    uint16_t c = (uint16_t)color;
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    if (miny < 0) miny = 0;
    if (maxy >= s_gfx.h) maxy = s_gfx.h - 1;
    for (int y = miny; y <= maxy; y++) {
        int lo = 0x7fffffff, hi = -0x7fffffff;
        tri_edge_native(x0, y0, x1, y1, y, &lo, &hi);
        tri_edge_native(x1, y1, x2, y2, y, &lo, &hi);
        tri_edge_native(x2, y2, x0, y0, y, &lo, &hi);
        if (hi < lo) continue;
        if (lo < 0) lo = 0;
        if (hi >= s_gfx.w) hi = s_gfx.w - 1;
        uint16_t *row = &s_gfx.buf[s_gfx.draw_idx][y * s_gfx.w];
        for (int x = lo; x <= hi; x++) row[x] = c;
    }
}
// gfx_image: blit a named RGB565 asset from the running app's own SD folder, scaled to w×h with a
// magenta (0xF81F) color-key for transparency. Assets live at /sdcard/apps/<id>/img/<name>.565
// (u16 w, u16 h header + pixels; see tools/build_abc_assets.py). Loaded once into a small PSRAM
// cache reused across runs — the guest can't read the SD itself (no fs permission), so real image
// art goes through here instead of dozens of primitive draw calls.
struct ImgCache { char name[24]; uint16_t *px; int w, h; };
// 128, not 48: abc123 alone shows >100 distinct icons; a smaller cache round-robin-evicts and
// re-reads from SD mid-frame (blocking the render loop). ~128 KB PSRAM worst case — cheap.
#define IMG_CAP 128
static ImgCache s_img[IMG_CAP];
static int      s_img_n = 0;
static int      s_img_next = 0;   // round-robin eviction cursor

// Drop every cached asset. Called on collect (the cache is keyed by name only, so entries MUST NOT
// outlive the app that loaded them — two apps shipping the same asset name would bleed pixels into
// each other) and by the broker reclaim below. Caller holds s_exec.lock or the worker is quiesced.
static size_t img_cache_flush(void) {
    size_t freed = 0;
    for (int i = 0; i < s_img_n; i++) {
        if (!s_img[i].px) continue;
        freed += (size_t)s_img[i].w * s_img[i].h * 2;
        heap_caps_free(s_img[i].px);
        s_img[i].px = nullptr;
        s_img[i].name[0] = 0;
    }
    s_img_n = 0;
    s_img_next = 0;
    return freed;
}

static ImgCache *img_get(const char *name) {
    for (int i = 0; i < s_img_n; i++) if (!strcmp(s_img[i].name, name)) return &s_img[i];
    char path[128];
    snprintf(path, sizeof path, "/sdcard/apps/%s/img/%s.565", s_exec.app.id, name);
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    uint16_t hdr[2];
    if (fread(hdr, 2, 2, f) != 2) { fclose(f); return nullptr; }
    int w = hdr[0], h = hdr[1];
    if (w <= 0 || h <= 0 || w > 512 || h > 512) { fclose(f); return nullptr; }
    size_t n = (size_t)w * h;
    uint16_t *px = (uint16_t *)heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!px) { fclose(f); return nullptr; }
    if (fread(px, 2, n, f) != n) { heap_caps_free(px); fclose(f); return nullptr; }
    fclose(f);
    ImgCache *c;
    if (s_img_n < IMG_CAP) {
        c = &s_img[s_img_n++];
    } else {                                    // full: evict round-robin so any object count works
        c = &s_img[s_img_next];
        s_img_next = (s_img_next + 1) % IMG_CAP;
        if (c->px) heap_caps_free(c->px);
    }
    snprintf(c->name, sizeof c->name, "%s", name);
    c->px = px; c->w = w; c->h = h;
    return c;
}
void nvi_gfx_image(wasm_exec_env_t env, const char *name, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!gfx_perm(env) || !name || w <= 0 || h <= 0) return;
    ImgCache *c = img_get(name);
    if (!c) return;
    s_gfx.dirty = true;
    const uint16_t KEY = 0xF81F;
    if (w > 1024) w = 1024;
    // Precompute the source column for every destination column once (was a divide per pixel).
    static int sxmap[1024];
    for (int dx = 0; dx < w; dx++) sxmap[dx] = (dx * c->w) / w;
    for (int dy = 0; dy < h; dy++) {
        int yy = y + dy;
        if ((unsigned)yy >= (unsigned)s_gfx.h) continue;
        const uint16_t *srow = &c->px[((dy * c->h) / h) * c->w];
        uint16_t *drow = &s_gfx.buf[s_gfx.draw_idx][yy * s_gfx.w];
        for (int dx = 0; dx < w; dx++) {
            int xx = x + dx;
            if ((unsigned)xx >= (unsigned)s_gfx.w) continue;
            uint16_t p = srow[sxmap[dx]];
            if (p != KEY) drow[xx] = p;
        }
    }
}
// Copy an RGB565 image (guest memory, WAMR-validated ptr+len) onto the canvas at (x,y), clipped.
void nvi_gfx_blit(wasm_exec_env_t env, void *ptr, uint32_t len, int32_t x, int32_t y,
                  int32_t w, int32_t h) {
    if (!gfx_perm(env) || !ptr || w <= 0 || h <= 0) return;
    // Validate in 64-bit: `w*h*2` in signed int overflows (w=h=40000 wraps negative -> passes the
    // old check) and the blit then reads far past the WAMR-validated `len` bytes (OOB read / escape).
    if ((int64_t)w * h * 2 > (int64_t)len) return;   // guest lied about the size -> refuse
    s_gfx.dirty = true;
    const uint16_t *src = (const uint16_t *)ptr;
    // Clip to the canvas in 64-bit up front (x/y are hostile too — INT_MIN would break `-y`). Source
    // stride stays = w, so only the on-screen sub-rectangle is copied. Every index below is provably
    // in range: rows land in [0,s_gfx.h), cols in [0,s_gfx.w), src offset < w*h <= len/2.
    int64_t W = w, H = h, X = x, Y = y, GW = s_gfx.w, GH = s_gfx.h;
    int64_t r0 = Y < 0 ? -Y : 0, r1 = (Y + H > GH) ? GH - Y : H;
    int64_t c0 = X < 0 ? -X : 0, c1 = (X + W > GW) ? GW - X : W;
    for (int64_t r = r0; r < r1; r++) {
        uint16_t *drow = &s_gfx.buf[s_gfx.draw_idx][(Y + r) * GW];
        const uint16_t *srow = &src[r * W];
        for (int64_t cx = c0; cx < c1; cx++) drow[X + cx] = srow[cx];
    }
}
// Fill a destructible height field in ONE call: `tops` = int16 surface-y per column (guest mem,
// validated). Column x0+i is filled from tops[i] to ybot — a grass cap (cgrass) then dirt (cdirt).
// Saves a game from issuing ~1 draw call per terrain column each frame.
void nvi_gfx_terrain(wasm_exec_env_t env, void *ptr, uint32_t len, int32_t x0, int32_t ybot,
                     int32_t cgrass, int32_t cdirt) {
    if (!gfx_perm(env) || !ptr) return;
    s_gfx.dirty = true;
    const int16_t *tops = (const int16_t *)ptr;
    const int n = (int)(len / 2);
    uint16_t g = (uint16_t)cgrass, d = (uint16_t)cdirt;
    if (ybot > s_gfx.h) ybot = s_gfx.h;
    for (int i = 0; i < n; i++) {
        int x = x0 + i;
        if ((unsigned)x >= (unsigned)s_gfx.w) continue;
        int top = tops[i];
        if (top < 0) top = 0;
        uint16_t *col = &s_gfx.buf[s_gfx.draw_idx][x];
        for (int y = top; y < ybot; y++) col[y * s_gfx.w] = (y < top + 3) ? g : d;
    }
}
void nvi_gfx_tone(wasm_exec_env_t env, int32_t freq, int32_t ms) {
    if (!gfx_perm(env)) return;
    nv_audio_tone(freq, ms);
}
// Draw a string with the built-in 5x7 font, magnified by `scale`. Advance = 6*scale px/char.
void nvi_gfx_text(wasm_exec_env_t env, int32_t x, int32_t y, const char *s, int32_t color, int32_t scale) {
    if (!gfx_perm(env) || !s) return;
    s_gfx.dirty = true;
    if (scale < 1) scale = 1;
    uint16_t c = (uint16_t)color;
    int cx = x;
    for (const char *p = s; *p; p++) {
        const uint8_t *gl = FONT5x7[font_glyph(*p)];
        for (int row = 0; row < 7; row++) {
            uint8_t bits = gl[row];
            for (int col = 0; col < 5; col++) {
                if (!(bits & (1 << (4 - col)))) continue;
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        gfx_px(cx + col * scale + dx, y + row * scale + dy, c);
            }
        }
        cx += 6 * scale;
    }
}
// ABI v4: width in px that nvi_gfx_text would advance for `s` at `scale` (6*scale per char). Lets
// apps center/right-align text without hard-coding the font metric.
int32_t nvi_gfx_text_width(wasm_exec_env_t env, const char *s, int32_t scale) {
    if (!gfx_perm(env) || !s) return 0;
    if (scale < 1) scale = 1;
    return (int32_t)strlen(s) * 6 * scale;
}
// ABI v4: set the panel backlight (0..100%). Gated on gfx like the rest of the surface; the runner
// restores the user's saved brightness when the app exits (see collect), so a flashlight can crank it.
void nvi_backlight(wasm_exec_env_t env, int32_t level) {
    if (!gfx_perm(env)) return;
    if (level < 0) level = 0; else if (level > 100) level = 100;
    s_gfx.bl_touched = true;
    nv_hal_backlight_set(level);
}
int32_t nvi_gfx_input(wasm_exec_env_t env) {
    if (!gfx_perm(env)) return 0;
    pthread_mutex_lock(&s_exec.lock);
    int32_t v = ((s_gfx.in_state & 0x3) << 24) |
                ((s_gfx.in_y & 0xFFF) << 12) | (s_gfx.in_x & 0xFFF);
    pthread_mutex_unlock(&s_exec.lock);
    return v;
}
// Multi-touch (ABI v3): number of fingers currently down (0..5). The full set is pushed each frame
// by the UI from the raw GT911 cache (nv_wasm_gfx_set_multi). FALLBACK: when the hardware cache is
// empty, the single LVGL pointer (in_x/in_y/in_state) is reported as one finger — so a synthetic tap
// (/api/ui/tap remote test) or an HID mouse/trackpad still drive a multi-touch-only app as one point.
int32_t nvi_gfx_touch_count(wasm_exec_env_t env) {
    if (!gfx_perm(env)) return 0;
    pthread_mutex_lock(&s_exec.lock);
    int32_t n = s_gfx.mt_cnt;
    if (n == 0 && (s_gfx.in_state & 0x3)) n = 1;
    pthread_mutex_unlock(&s_exec.lock);
    return n;
}
// One finger by index: packs (valid<<24) | (y<<12) | x in canvas pixels. valid=0 (whole word 0)
// when idx is out of range — the guest stops iterating there. Same single-pointer fallback as above.
int32_t nvi_gfx_touch_point(wasm_exec_env_t env, int32_t idx) {
    if (!gfx_perm(env)) return 0;
    pthread_mutex_lock(&s_exec.lock);
    int32_t v = 0;
    if (s_gfx.mt_cnt > 0) {
        if (idx >= 0 && idx < s_gfx.mt_cnt)
            v = (1 << 24) | ((s_gfx.mt_y[idx] & 0xFFF) << 12) | (s_gfx.mt_x[idx] & 0xFFF);
    } else if (idx == 0 && (s_gfx.in_state & 0x3)) {
        v = (1 << 24) | ((s_gfx.in_y & 0xFFF) << 12) | (s_gfx.in_x & 0xFFF);
    }
    pthread_mutex_unlock(&s_exec.lock);
    return v;
}
// Pending OS back-gesture requests since the last call (the game handles its own back-stack,
// exiting only when it's at its own root). Cleared on read.
int32_t nvi_gfx_back(wasm_exec_env_t env) {
    if (!gfx_perm(env)) return 0;
    pthread_mutex_lock(&s_exec.lock);
    int v = s_gfx.back_req; s_gfx.back_req = 0;
    pthread_mutex_unlock(&s_exec.lock);
    return v;
}
// Publish the just-drawn frame and block until the UI has shown it (frame pacing). Returns 0 when
// the OS wants the app to exit — the guest's loop condition, its cooperative stop point.
int32_t nvi_gfx_present(wasm_exec_env_t env) {
    RunReq *r = req_of(env);
    if (!r || !r->ex || !(r->perms & NV_WPERM_GFX) || !s_gfx.open) return 0;
    Exec *ex = r->ex;
    s_gfx.present_seq++;   // heartbeat for the UI-side wedge watchdog (u32 store: no torn read)
    // Nothing drawn since the last present (games skip their draw pass on static screens): don't
    // republish — the UI would otherwise recomposite the full 1024×600 canvas at 60 fps forever,
    // starving the LVGL draw units. Keep the loop paced at ~60 Hz for input polling.
    if (!s_gfx.dirty) {
        pthread_mutex_lock(&ex->lock);
        bool stop_clean = s_gfx.want_stop || ex->abort_req;
        pthread_mutex_unlock(&ex->lock);
        if (stop_clean) return 0;
        vTaskDelay(pdMS_TO_TICKS(16));
        return 1;
    }
    s_gfx.dirty = false;
    pthread_mutex_lock(&ex->lock);
    s_gfx.ready_idx   = s_gfx.draw_idx;
    s_gfx.frame_ready = true;
    bool stop = s_gfx.want_stop || ex->abort_req;
    pthread_mutex_unlock(&ex->lock);
    while (!stop) {
        vTaskDelay(1);
        pthread_mutex_lock(&ex->lock);
        const bool rdy = s_gfx.frame_ready;
        stop = s_gfx.want_stop || ex->abort_req;
        pthread_mutex_unlock(&ex->lock);
        if (!rdy) break;   // UI consumed the frame
    }
    s_gfx.draw_idx ^= 1;   // next frame draws into the other buffer
    return stop ? 0 : 1;
}

// Persistence: a game saves/loads a small blob in its own SD folder (/sdcard/apps/<id>/<name>).
// Lets games keep high scores, collectibles and progress across runs (the guest can't touch the SD
// itself). Gated on the gfx permission (games); name is sanitized, size capped at 8 KB.
static bool save_name_ok(const char *n) {
    if (!n || !n[0] || strstr(n, "..")) return false;
    for (const char *p = n; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.'))
            return false;
    }
    return true;
}
int32_t nvi_save(wasm_exec_env_t env, const char *name, void *ptr, uint32_t len) {
    RunReq *r = req_of(env);
    if (!r || !(r->perms & NV_WPERM_GFX) || !save_name_ok(name) || !ptr || len == 0 || len > 8192) return 0;
    char path[160];
    snprintf(path, sizeof path, "/sdcard/apps/%s/%s", s_exec.app.id, name);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = fwrite(ptr, 1, len, f);
    fclose(f);
    return (n == len) ? 1 : 0;
}
// Sound effects: play a WAV (48kHz mono 16-bit; polyphony baked into the sample) from the app's own
// SD folder /sdcard/apps/<id>/snd/<name>.wav. A dedicated task streams it to the audio codec via the
// PCM path, so the guest never blocks. Richer than the mono gfx_tone beeps.
QueueHandle_t s_snd_q = nullptr;
void snd_task(void *) {
    char path[128];
    static int16_t buf[1024];
    for (;;) {
        if (xQueueReceive(s_snd_q, path, portMAX_DELAY) != pdTRUE) continue;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        // Parse the fmt chunk instead of assuming the canonical 44-byte 48 kHz/mono header:
        // editor exports insert LIST/fact chunks and ship other rates — the blind skip played
        // those as wrong-pitch noise. PCM 16-bit only; anything else is skipped in silence.
        uint8_t riff[12];
        uint32_t rate = 0; uint16_t ch = 0, bits = 0; bool pcm_ok = false;
        if (fread(riff, 1, 12, f) == 12 && !memcmp(riff, "RIFF", 4) && !memcmp(riff + 8, "WAVE", 4)) {
            for (;;) {
                uint8_t ck[8];
                if (fread(ck, 1, 8, f) != 8) break;
                const uint32_t sz = ck[4] | (ck[5] << 8) | (ck[6] << 16) | ((uint32_t)ck[7] << 24);
                if (!memcmp(ck, "fmt ", 4)) {
                    uint8_t fm[16];
                    if (sz < 16 || fread(fm, 1, 16, f) != 16) break;
                    const uint16_t afmt = fm[0] | (fm[1] << 8);
                    ch   = fm[2] | (fm[3] << 8);
                    rate = fm[4] | (fm[5] << 8) | (fm[6] << 16) | ((uint32_t)fm[7] << 24);
                    bits = fm[14] | (fm[15] << 8);
                    if (afmt != 1) break;                      // PCM only
                    if (sz > 16) fseek(f, (long)(sz - 16 + (sz & 1)), SEEK_CUR);
                } else if (!memcmp(ck, "data", 4)) {           // file now positioned at the samples
                    pcm_ok = ch >= 1 && ch <= 2 && bits == 16 && rate >= 8000 && rate <= 48000;
                    break;
                } else {
                    fseek(f, (long)(sz + (sz & 1)), SEEK_CUR); // chunks are word-padded
                }
            }
        }
        if (!pcm_ok) { fclose(f); continue; }
        if (!nv_audio_pcm_begin_as((int)rate, ch, bits, NV_PCM_SFX)) { fclose(f); continue; }
        size_t n;
        // Tagged NV_PCM_SFX: if the voice engine preempts (pcm_write returns <0), stop at once so
        // the spoken word never plays over the jingle.
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) { if (nv_audio_pcm_write(buf, n) < 0) break; }
        nv_audio_pcm_end();
        fclose(f);
    }
}
void nvi_sound(wasm_exec_env_t env, const char *name) {
    RunReq *r = req_of(env);
    if (!r || !(r->perms & NV_WPERM_GFX) || !save_name_ok(name) || !s_snd_q) return;
    char path[128];
    snprintf(path, sizeof path, "/sdcard/apps/%s/snd/%s.wav", s_exec.app.id, name);
    xQueueSend(s_snd_q, path, 0);                      // drop if a sound is already queued
}
// nv.speak: speak `text` via the OS offline voice (nv_tts) in `lang` ("it"/"en"/…; only installed
// packs play). Non-blocking. Any word/number the voice pack covers is spoken naturally.
void nvi_speak(wasm_exec_env_t env, const char *text, const char *lang) {
    RunReq *r = req_of(env);
    if (!r || !(r->perms & NV_WPERM_GFX) || !text) return;
    nv_tts_say(text, lang);
}
int32_t nvi_load(wasm_exec_env_t env, const char *name, void *ptr, uint32_t len) {
    RunReq *r = req_of(env);
    if (!r || !(r->perms & NV_WPERM_GFX) || !save_name_ok(name) || !ptr || len == 0 || len > 8192) return 0;
    char path[160];
    snprintf(path, sizeof path, "/sdcard/apps/%s/%s", s_exec.app.id, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(ptr, 1, len, f);
    fclose(f);
    return (int32_t)n;
}

NativeSymbol s_env_natives[] = {
    { "host_log", (void *)host_log, "(i)", nullptr },
};

NativeSymbol s_nv_natives[] = {
    { "print",     (void *)nvi_print,     "($)",   nullptr },
    { "log",       (void *)nvi_log,       "(i$)",  nullptr },
    { "toast",     (void *)nvi_toast,     "(i$)",  nullptr },
    { "millis",    (void *)nvi_millis,    "()i",   nullptr },
    { "time_unix", (void *)nvi_time_unix, "()I",   nullptr },
    { "lang",      (void *)nvi_lang,      "(*~)i", nullptr },
    { "rand",      (void *)nvi_rand,      "()i",   nullptr },
    { "sleep_ms",  (void *)nvi_sleep_ms,  "(i)",   nullptr },
    { "save",      (void *)nvi_save,      "($*~)i", nullptr },
    { "load",      (void *)nvi_load,      "($*~)i", nullptr },
    { "sound",     (void *)nvi_sound,     "($)",    nullptr },
    { "speak",     (void *)nvi_speak,     "($$)",   nullptr },
    // ABI v2 game surface
    { "gfx_width",   (void *)nvi_gfx_width,   "()i",      nullptr },
    { "gfx_height",  (void *)nvi_gfx_height,  "()i",      nullptr },
    { "gfx_clear",   (void *)nvi_gfx_clear,   "(i)",      nullptr },
    { "gfx_rect",    (void *)nvi_gfx_rect,    "(iiiii)",  nullptr },
    { "gfx_circle",  (void *)nvi_gfx_circle,  "(iiii)",   nullptr },
    { "gfx_line",    (void *)nvi_gfx_line,    "(iiiii)",  nullptr },
    { "gfx_tri",     (void *)nvi_gfx_tri,     "(iiiiiii)", nullptr },
    { "gfx_image",   (void *)nvi_gfx_image,   "($iiii)",  nullptr },
    { "gfx_blit",    (void *)nvi_gfx_blit,    "(*~iiii)", nullptr },
    { "gfx_text",    (void *)nvi_gfx_text,    "(ii$ii)",  nullptr },
    { "gfx_text_width", (void *)nvi_gfx_text_width, "($i)i", nullptr },  // ABI v4
    { "backlight",   (void *)nvi_backlight,   "(i)",      nullptr },     // ABI v4
    { "gfx_terrain", (void *)nvi_gfx_terrain, "(*~iiii)", nullptr },
    { "gfx_tone",    (void *)nvi_gfx_tone,    "(ii)",     nullptr },
    { "gfx_input",   (void *)nvi_gfx_input,   "()i",      nullptr },
    { "gfx_touch_count", (void *)nvi_gfx_touch_count, "()i",  nullptr },
    { "gfx_touch_point", (void *)nvi_gfx_touch_point, "(i)i", nullptr },
    { "gfx_back",    (void *)nvi_gfx_back,    "()i",      nullptr },
    { "gfx_present", (void *)nvi_gfx_present, "()i",      nullptr },
};

// ---- bundled demo modules (hand-assembled; no wasm toolchain needed) ----------------------------
// add(i32 a, i32 b) -> i32  { return a + b; }   — proves load/instantiate/call + return value.
const uint8_t kAddWasm[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x07, 0x01, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00,
    0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b,
};

// run() { host_log(6 * 7); }   — imports env.host_log; boot self-test of the import path.
const uint8_t kAppWasm[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,                          // header
    0x01, 0x08, 0x02, 0x60, 0x01, 0x7f, 0x00, 0x60, 0x00, 0x00,             // types: (i32)->(), ()->()
    0x02, 0x10, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x08, 0x68, 0x6f, 0x73, 0x74, // import "env"."host_log"
    0x5f, 0x6c, 0x6f, 0x67, 0x00, 0x00,                                      //   func type 0
    0x03, 0x02, 0x01, 0x01,                                                  // func: run, type 1
    0x07, 0x07, 0x01, 0x03, 0x72, 0x75, 0x6e, 0x00, 0x01,                    // export "run" -> func 1
    0x0a, 0x0b, 0x01, 0x09, 0x00, 0x41, 0x06, 0x41, 0x07, 0x6c, 0x10, 0x00, 0x0b,  // run{6 7 mul call0}
};

// Bundled demo app for /sdcard/apps/hello — exercises the ABI v1 imports:
//   run() { nv.print("Ciao da NucleoOS WASM!"); env.host_log(nv.millis()); nv.toast(1, "Demo ABI v1 OK"); }
const uint8_t kHelloWasm[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,                          // header
    // types: t0 (i32)->(), t1 (i32,i32)->(), t2 ()->i32, t3 ()->()
    0x01, 0x11, 0x04, 0x60, 0x01, 0x7f, 0x00, 0x60, 0x02, 0x7f, 0x7f, 0x00,
    0x60, 0x00, 0x01, 0x7f, 0x60, 0x00, 0x00,
    // imports: nv.print(t0) nv.toast(t1) nv.millis(t2) env.host_log(t0)
    0x02, 0x32, 0x04,
    0x02, 0x6e, 0x76, 0x05, 0x70, 0x72, 0x69, 0x6e, 0x74, 0x00, 0x00,
    0x02, 0x6e, 0x76, 0x05, 0x74, 0x6f, 0x61, 0x73, 0x74, 0x00, 0x01,
    0x02, 0x6e, 0x76, 0x06, 0x6d, 0x69, 0x6c, 0x6c, 0x69, 0x73, 0x00, 0x02,
    0x03, 0x65, 0x6e, 0x76, 0x08, 0x68, 0x6f, 0x73, 0x74, 0x5f, 0x6c, 0x6f, 0x67, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x03,                                                  // func: run, type t3
    0x05, 0x03, 0x01, 0x00, 0x01,                                            // memory: 1 page
    0x07, 0x07, 0x01, 0x03, 0x72, 0x75, 0x6e, 0x00, 0x04,                    // export "run" -> func 4
    // code: print(16); host_log(millis()); toast(1, 48)
    0x0a, 0x12, 0x01, 0x10, 0x00,
    0x41, 0x10, 0x10, 0x00,                                                  // i32.const 16; call print
    0x10, 0x02, 0x10, 0x03,                                                  // call millis; call host_log
    0x41, 0x01, 0x41, 0x30, 0x10, 0x01,                                      // 1, 48; call toast
    0x0b,
    // data: 16 = "Ciao da NucleoOS WASM!\0", 48 = "Demo ABI v1 OK\0"
    0x0b, 0x31, 0x02,
    0x00, 0x41, 0x10, 0x0b, 0x17,
    'C', 'i', 'a', 'o', ' ', 'd', 'a', ' ', 'N', 'u', 'c', 'l', 'e', 'o',
    'O', 'S', ' ', 'W', 'A', 'S', 'M', '!', 0x00,
    0x00, 0x41, 0x30, 0x0b, 0x0f,
    'D', 'e', 'm', 'o', ' ', 'A', 'B', 'I', ' ', 'v', '1', ' ', 'O', 'K', 0x00,
};
constexpr char kHelloVersion[] = "2.0";   // bump to reseed installed copies of the bundled demo

void set_err(char *err, size_t n, const char *msg) {
    NV_LOGE(TAG, "%s", msg);
    if (err && n) { strncpy(err, msg, n - 1); err[n - 1] = '\0'; }
}

void worker_stack_to_psram(void);   // defined below; used by both spawn paths

void *run_worker(void *p) {
    RunReq *r = static_cast<RunReq *>(p);
    r->ok = false; r->err[0] = '\0';

    // WAMR loads from a MUTABLE buffer (fast-interp rewrites opcodes in place) -> copy per run.
    uint8_t stackbuf[128];
    uint8_t *buf = r->mod_size <= sizeof(stackbuf) ? stackbuf : (uint8_t *)malloc(r->mod_size);
    if (!buf) { set_err(r->err, sizeof r->err, "oom"); goto done; }
    memcpy(buf, r->mod, r->mod_size);

    {
        char ebuf[128] = "";
        wasm_module_t module = wasm_runtime_load(buf, r->mod_size, ebuf, sizeof(ebuf));
        if (!module) {
            set_err(r->err, sizeof r->err, ebuf[0] ? ebuf : "load failed");
            goto free_buf;
        }

        wasm_module_inst_t inst =
            wasm_runtime_instantiate(module, r->stack_size, r->heap_size, ebuf, sizeof(ebuf));
        if (!inst) {
            set_err(r->err, sizeof r->err, ebuf[0] ? ebuf : "instantiate failed");
            wasm_runtime_unload(module);
            goto free_buf;
        }

        // Publish the live instance so nv_wasm_exec_abort can wasm_runtime_terminate it. If an
        // abort raced in before we got here, terminate right away.
        if (r->ex) {
            pthread_mutex_lock(&r->ex->lock);
            r->ex->inst = inst;
            const bool aborted = r->ex->abort_req;
            pthread_mutex_unlock(&r->ex->lock);
            if (aborted) wasm_runtime_terminate(inst);
        }

        wasm_function_inst_t func = wasm_runtime_lookup_function(inst, r->fn);
        if (!func) {
            set_err(r->err, sizeof r->err, "export not found");
        } else {
            wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, r->stack_size);
            if (!env) {
                set_err(r->err, sizeof r->err, "exec env failed");
            } else {
                wasm_runtime_set_user_data(env, r);   // host imports read perms + sinks
                // Cap total opcodes so a runaway console app traps promptly. Games run their own
                // frame loop indefinitely, so they get NO cap — gfx_present() is the cooperative
                // stop point, and since THREAD_MGR (wamr CMakeLists) nv_wasm_exec_abort() is the
                // forcible one: terminate() spreads the TERMINATE suspend flag, which the
                // interpreter checks at every br/call, so even a while(1){} dies in microseconds.
                wasm_runtime_set_instruction_count_limit(env, r->game ? -1 : kInstrBudget);
                if (wasm_runtime_call_wasm(env, func, r->argc, r->argv)) {
                    r->ok = true;
                } else {
                    const char *ex = wasm_runtime_get_exception(inst);
                    set_err(r->err, sizeof r->err, ex ? ex : "call trapped");
                }
                wasm_runtime_destroy_exec_env(env);
            }
        }

        if (r->ex) {   // retire the instance handle BEFORE tearing it down
            pthread_mutex_lock(&r->ex->lock);
            r->ex->inst = nullptr;
            pthread_mutex_unlock(&r->ex->lock);
        }
        wasm_runtime_deinstantiate(inst);
        wasm_runtime_unload(module);
    }

free_buf:
    if (buf != stackbuf) free(buf);
done:
    if (r->ex) {
        Exec *ex = r->ex;
        pthread_mutex_lock(&ex->lock);
        ex->elapsed_ms = (uint32_t)((esp_timer_get_time() - ex->t_start_us) / 1000);
        ex->state = NV_WRUN_DONE;
        pthread_mutex_unlock(&ex->lock);
    }
    return nullptr;
}

void worker_stack_to_psram(void);   // defined below; forward-declared for run_on_pthread's call

// Run a module export on a dedicated pthread (WAMR pthread_self requirement). Blocks until done.
// Synchronous demo path only — installed apps go through nv_wasm_exec_start.
bool run_on_pthread(RunReq *r, char *err, size_t err_n) {
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, kWorkerNativeStk);
    worker_stack_to_psram();
    int rc = pthread_create(&t, &at, run_worker, r);
    pthread_attr_destroy(&at);
    if (rc != 0) { set_err(err, err_n, "worker thread failed"); return false; }
    pthread_join(t, nullptr);
    if (!r->ok) set_err(err, err_n, r->err[0] ? r->err : "run failed");
    return r->ok;
}

// Place the WASM worker's (large) stack in PSRAM: internal SRAM runs as low as ~30 KB contiguous
// with Wi-Fi/LVGL up, too little for a 48 KB stack, so an internal-stack pthread_create fails
// ("worker thread failed"). PSRAM has tens of MB free and the interpreter never runs with flash
// cache disabled, so an external stack is safe here. Must be set on the calling thread right
// before pthread_create.
void worker_stack_to_psram(void) {
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size       = kWorkerNativeStk;
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    cfg.thread_name      = "wasm_worker";
    // Prio 2, BELOW the LVGL SW draw units (prio 3): the pthread default (5) let the interpreter
    // preempt the very draw workers the prio-6 LVGL task blocks on — full-UI freezes during heavy
    // game frames. The worker is pure CPU-bound background work; nothing waits on it synchronously.
    cfg.prio             = 2;
    esp_pthread_set_cfg(&cfg);
}

void req_defaults(RunReq *r) {
    memset(r, 0, sizeof(*r));
    r->heap_size  = 8 * 1024;
    r->stack_size = 8 * 1024;
    r->game       = false;
    snprintf(r->tag, sizeof r->tag, "%s", "wasmapp");
}

}  // namespace

// WAMR allocator -> PSRAM, 16-byte aligned. Two constraints together:
//  1) Plain malloc pulls from internal SRAM, whose largest free block is only ~31KB under normal
//     load, so per-run allocations (exec-env stack, instance structs) intermittently failed as
//     internal fragmented -> route to the 25MB+ of PSRAM.
//  2) WAMR's ems app-heap requires its control struct to be 8-byte aligned, but heap_caps_malloc on
//     PSRAM only guarantees 4-byte -> ~50% of runs died "init app heap failed / heap init struct buf
//     not 8-byte aligned". heap_caps_aligned_alloc(16, ...) fixes that.
// free() and heap_caps_free() handle aligned allocations on IDF 5.x; realloc is done by hand (there
// is no aligned realloc) using the block's queried size, so alignment is preserved across grows.
static void *wamr_malloc(unsigned int size) {
    void *p = heap_caps_aligned_alloc(16, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_aligned_alloc(16, size, MALLOC_CAP_8BIT);   // fall back to internal
    return p;
}
static void wamr_free(void *ptr) { if (ptr) heap_caps_free(ptr); }
static void *wamr_realloc(void *ptr, unsigned int size) {
    if (!ptr) return wamr_malloc(size);
    void *np = wamr_malloc(size);
    if (!np) return nullptr;
    size_t old = heap_caps_get_allocated_size(ptr);
    memcpy(np, ptr, old < size ? old : size);
    heap_caps_free(ptr);
    return np;
}

// Broker reclaim: between runs the runtime keeps the canvas double buffer (up to 2.4 MB) and the
// asset cache for a fast relaunch. A RAM-heavy app (camera: 4×4 MB contiguous PSRAM) matters more —
// drop them when no app is running. Fail-closed: mid-run, frees nothing.
static size_t wasm_reclaim(void *) {
    size_t freed = 0;
    pthread_mutex_lock(&s_exec.lock);
    if (s_exec.state == NV_WRUN_IDLE && !s_gfx.open) {
        for (int i = 0; i < 2; i++) {
            if (!s_gfx.buf[i]) continue;
            freed += (size_t)s_gfx.cap_px * 2;
            heap_caps_free(s_gfx.buf[i]);
            s_gfx.buf[i] = nullptr;
        }
        s_gfx.cap_px = 0;
        freed += img_cache_flush();
    }
    pthread_mutex_unlock(&s_exec.lock);
    return freed;
}

bool nv_wasm_init(void) {
    if (s_ready) return true;

    RuntimeInitArgs init;
    memset(&init, 0, sizeof(init));
    init.mem_alloc_type = Alloc_With_Allocator;
    init.mem_alloc_option.allocator.malloc_func  = (void *)wamr_malloc;
    init.mem_alloc_option.allocator.realloc_func = (void *)wamr_realloc;
    init.mem_alloc_option.allocator.free_func    = (void *)wamr_free;

    if (!wasm_runtime_full_init(&init)) {
        NV_LOGE(TAG, "wasm_runtime_full_init failed");
        return false;
    }
    // Publish the host import tables BEFORE any module instantiates.
    if (!wasm_runtime_register_natives("env", s_env_natives,
                                       sizeof(s_env_natives) / sizeof(s_env_natives[0])))
        NV_LOGW(TAG, "register_natives(env) failed");
    if (!wasm_runtime_register_natives("nv", s_nv_natives,
                                       sizeof(s_nv_natives) / sizeof(s_nv_natives[0])))
        NV_LOGW(TAG, "register_natives(nv) failed");

    // Sound-effect player: a task drains a small queue of WAV paths and streams them to the codec.
    // PSRAM stack (it only reads the SD and writes audio — no internal-flash access).
    if (!s_snd_q) {
        s_snd_q = xQueueCreate(2, 128);
        if (s_snd_q) xTaskCreateWithCaps(snd_task, "nvsnd", 4096, nullptr, 4, nullptr, MALLOC_CAP_SPIRAM);
    }

    nv_mem_reclaimer_add("wasm-caches", wasm_reclaim, nullptr);

    s_ready = true;
    NV_LOGI(TAG, "WAMR runtime up (interpreter), host ABI v%d", NV_WASM_ABI);
    return true;
}

bool nv_wasm_run_demo(int a, int b, int *out, char *err, size_t err_n) {
    if (!nv_wasm_init()) { set_err(err, err_n, "runtime init failed"); return false; }
    RunReq r;
    req_defaults(&r);
    r.mod = kAddWasm; r.mod_size = sizeof(kAddWasm); r.fn = "add";
    r.argv[0] = (uint32_t)a; r.argv[1] = (uint32_t)b; r.argc = 2;
    r.perms = NV_WPERM_LOG;
    bool ok = run_on_pthread(&r, err, err_n);
    if (ok) {
        if (out) *out = (int)r.argv[0];
        NV_LOGI(TAG, "demo add(%d,%d) = %d", a, b, (int)r.argv[0]);
    }
    return ok;
}

bool nv_wasm_run_app(char *err, size_t err_n) {
    if (!nv_wasm_init()) { set_err(err, err_n, "runtime init failed"); return false; }
    RunReq r;
    req_defaults(&r);
    r.mod = kAppWasm; r.mod_size = sizeof(kAppWasm); r.fn = "run";
    r.perms = NV_WPERM_LOG;
    bool ok = run_on_pthread(&r, err, err_n);
    if (ok) NV_LOGI(TAG, "app run() OK (called host_log)");
    return ok;
}

// ---- installed WASM apps (manifest v2 + SD) -----------------------------------------------------
namespace {

constexpr char kAppsDir[] = "/sdcard/apps";

uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool id_valid(const char *id) {
    if (!id || !id[0]) return false;
    for (const char *p = id; *p; ++p) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
        if (p - id >= 31) return false;
    }
    return true;
}

uint32_t parse_perms(const cJSON *arr) {
    uint32_t p = 0;
    if (!cJSON_IsArray(arr)) return 0;
    const cJSON *it = nullptr;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsString(it) || !it->valuestring) continue;
        if      (!strcmp(it->valuestring, "log")) p |= NV_WPERM_LOG;
        else if (!strcmp(it->valuestring, "ui"))  p |= NV_WPERM_UI;
        else if (!strcmp(it->valuestring, "net")) p |= NV_WPERM_NET;
        else if (!strcmp(it->valuestring, "fs"))  p |= NV_WPERM_FS;
        else if (!strcmp(it->valuestring, "gfx")) p |= NV_WPERM_GFX;
        else NV_LOGW(TAG, "manifest: unknown permission '%s' (ignored)", it->valuestring);
    }
    return p;
}

uint32_t json_u32(const cJSON *root, const char *key, uint32_t def) {
    const cJSON *j = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(j) && j->valuedouble >= 0 ? (uint32_t)j->valuedouble : def;
}

// Read a manifest.json + fill an app record. Returns false if it isn't a usable app.
bool read_manifest(const char *dir, const char *id, nv_wasm_app_t *out) {
    if (!id_valid(id)) return false;
    char path[192];
    snprintf(path, sizeof path, "%s/%s/manifest.json", dir, id);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char json[1024];
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';

    cJSON *root = cJSON_Parse(json);
    if (!root) return false;

    memset(out, 0, sizeof(*out));
    const cJSON *jn = cJSON_GetObjectItem(root, "name");
    const cJSON *jv = cJSON_GetObjectItem(root, "version");
    const cJSON *je = cJSON_GetObjectItem(root, "entry");
    snprintf(out->id, sizeof out->id, "%s", id);
    snprintf(out->name, sizeof out->name, "%s",
             (cJSON_IsString(jn) && jn->valuestring) ? jn->valuestring : id);
    snprintf(out->version, sizeof out->version, "%s",
             (cJSON_IsString(jv) && jv->valuestring) ? jv->valuestring : "?");
    snprintf(out->entry, sizeof out->entry, "%s",
             (cJSON_IsString(je) && je->valuestring) ? je->valuestring : "run");
    out->ram_budget = clamp_u32(json_u32(root, "ram_budget", 256u * 1024), kMinHeap, kMaxHeap);
    out->stack_kb   = clamp_u32(json_u32(root, "stack_kb", kDefStackKb), kMinStackKb, kMaxStackKb);
    out->timeout_ms = clamp_u32(json_u32(root, "timeout_ms", kDefTimeoutMs),
                                kMinTimeoutMs, kMaxTimeoutMs);
    out->abi        = json_u32(root, "abi", 1);
    out->perms      = parse_perms(cJSON_GetObjectItem(root, "permissions"));
    out->canvas_w   = json_u32(root, "canvas_w", 0);
    out->canvas_h   = json_u32(root, "canvas_h", 0);
    snprintf(out->wasm_path, sizeof out->wasm_path, "%s/%s/app.wasm", dir, id);
    cJSON_Delete(root);

    struct stat st;
    return stat(out->wasm_path, &st) == 0;   // must have an actual app.wasm alongside
}

#include "tanks_seed.inc"   // embedded Nucleo Tanks (ABI v2 game) — kTanks{Version,Manifest,Wasm,WasmLen}
}  // namespace

// Seed the built-in Nucleo Tanks game to /sdcard/apps/tanks (the web-OS FS API can't reach
// /sdcard/apps, so first-party games ship embedded and self-install at boot). No-op when current.
void nv_wasm_seed_tanks(void) {
    if (!nv_sd_is_mounted()) return;
    nv_wasm_app_t tmp;
    if (read_manifest(kAppsDir, "tanks", &tmp) && !strcmp(tmp.version, kTanksVersion)) return;

    mkdir(kAppsDir, 0777);
    char dir[160];
    snprintf(dir, sizeof dir, "%s/tanks", kAppsDir);
    mkdir(dir, 0777);

    char path[192];
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    FILE *f = fopen(path, "wb");
    if (!f) { NV_LOGW(TAG, "seed tanks: cannot write %s", path); return; }
    fwrite(kTanksManifest, 1, strlen(kTanksManifest), f);
    fclose(f);

    snprintf(path, sizeof path, "%s/app.wasm", dir);
    f = fopen(path, "wb");
    if (f) { fwrite(kTanksWasm, 1, kTanksWasmLen, f); fclose(f); }
    NV_LOGI(TAG, "seeded Nucleo Tanks v%s at %s", kTanksVersion, dir);
}

void nv_wasm_seed_demo(void) {
    if (!nv_sd_is_mounted()) return;
    nv_wasm_app_t tmp;
    if (read_manifest(kAppsDir, "hello", &tmp) && !strcmp(tmp.version, kHelloVersion))
        return;   // current bundled demo already installed

    mkdir(kAppsDir, 0777);   // ok if it exists
    char dir[160];
    snprintf(dir, sizeof dir, "%s/hello", kAppsDir);
    mkdir(dir, 0777);

    char path[192];
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    FILE *f = fopen(path, "wb");
    if (!f) { NV_LOGW(TAG, "seed: cannot write %s (LFN off?)", path); return; }
    fprintf(f,
            "{\n  \"id\": \"hello\",\n  \"name\": \"Hello WASM\",\n  \"version\": \"%s\",\n"
            "  \"entry\": \"run\",\n  \"abi\": 1,\n  \"ram_budget\": 262144,\n"
            "  \"stack_kb\": 16,\n  \"timeout_ms\": 5000,\n"
            "  \"permissions\": [\"log\", \"ui\"]\n}\n",
            kHelloVersion);
    fclose(f);

    snprintf(path, sizeof path, "%s/app.wasm", dir);
    f = fopen(path, "wb");
    if (f) { fwrite(kHelloWasm, 1, sizeof(kHelloWasm), f); fclose(f); }
    NV_LOGI(TAG, "seeded demo app v%s at %s", kHelloVersion, dir);
}

bool nv_wasm_uninstall(const char *id, char *err, size_t err_n) {
    if (!id || !id_valid(id)) { set_err(err, err_n, "bad id"); return false; }
    if (!nv_sd_is_mounted()) { set_err(err, err_n, "no SD card"); return false; }
    if (nv_wasm_exec_state() != NV_WRUN_IDLE && !strcmp(nv_wasm_exec_app_id(), id)) {
        set_err(err, err_n, "app is running");
        return false;
    }
    char path[192], dir[160];
    snprintf(dir, sizeof dir, "%s/%s", kAppsDir, id);
    snprintf(path, sizeof path, "%s/app.wasm", dir);      unlink(path);
    snprintf(path, sizeof path, "%s/manifest.json", dir); unlink(path);
    if (rmdir(dir) != 0) {
        struct stat st;
        if (stat(dir, &st) == 0) { set_err(err, err_n, "could not remove app folder"); return false; }
    }
    NV_LOGI(TAG, "uninstalled '%s'", id);
    return true;
}

bool nv_wasm_load_manifest(const char *id, nv_wasm_app_t *out) {
    if (!id || !out || !nv_sd_is_mounted()) return false;
    return read_manifest(kAppsDir, id, out);
}

int nv_wasm_scan(nv_wasm_app_t *out, int max) {
    if (!out || max <= 0 || !nv_sd_is_mounted()) {
        NV_LOGI(TAG, "scan: skipped (sd_mounted=%d)", nv_sd_is_mounted());
        return 0;
    }
    DIR *d = opendir(kAppsDir);
    if (!d) { NV_LOGW(TAG, "scan: opendir(%s) failed", kAppsDir); return 0; }
    int count = 0, skipped = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        if (count >= max) { skipped++; continue; }
        const bool ok = read_manifest(kAppsDir, e->d_name, &out[count]);
        NV_LOGI(TAG, "scan: '%s' -> %s", e->d_name, ok ? "app" : "skip");
        if (ok) count++;
    }
    closedir(d);
    if (skipped) NV_LOGW(TAG, "scan: %d app(s) beyond the %d cap were ignored", skipped, max);
    NV_LOGI(TAG, "scan: %d app(s) in %s", count, kAppsDir);
    return count;
}

const char *nv_wasm_perm_str(uint32_t perms, char *buf, size_t n) {
    if (!buf || !n) return "";
    buf[0] = '\0';
    const char *names[] = { "log", "ui", "net", "fs", "gfx" };
    const uint32_t bits[] = { NV_WPERM_LOG, NV_WPERM_UI, NV_WPERM_NET, NV_WPERM_FS, NV_WPERM_GFX };
    size_t len = 0;
    for (int i = 0; i < 5; i++) {
        if (!(perms & bits[i])) continue;
        int k = snprintf(buf + len, n - len, "%s%s", len ? ", " : "", names[i]);
        if (k > 0) len += (size_t)k;
    }
    if (!len) snprintf(buf, n, "%s", "none");
    return buf;
}

// ---- asynchronous runner ------------------------------------------------------------------------

nv_wrun_state_t nv_wasm_exec_state(void) {
    pthread_mutex_lock(&s_exec.lock);
    const nv_wrun_state_t st = s_exec.state;
    pthread_mutex_unlock(&s_exec.lock);
    return st;
}

const char *nv_wasm_exec_app_id(void) {
    static char id[sizeof s_exec.app.id];
    pthread_mutex_lock(&s_exec.lock);
    snprintf(id, sizeof id, "%s", s_exec.state == NV_WRUN_IDLE ? "" : s_exec.app.id);
    pthread_mutex_unlock(&s_exec.lock);
    return id;
}

bool nv_wasm_exec_collect(bool *ok, uint32_t *elapsed_ms, char *err, size_t err_n) {
    // The whole collect (join included) runs under the lock: setting DONE is the worker's LAST
    // locked action and it never re-acquires the lock afterwards, so joining here cannot
    // deadlock — and a concurrent collect/start can never double-join or see a half-freed run.
    pthread_mutex_lock(&s_exec.lock);
    if (s_exec.state != NV_WRUN_DONE) {
        pthread_mutex_unlock(&s_exec.lock);
        return false;
    }
    if (s_exec.thread_valid) { pthread_join(s_exec.thread, nullptr); s_exec.thread_valid = false; }
    if (ok) *ok = s_exec.req.ok;
    if (elapsed_ms) *elapsed_ms = s_exec.elapsed_ms;
    if (err && err_n) snprintf(err, err_n, "%s", s_exec.req.ok ? "" : s_exec.req.err);
    free(s_exec.bytes);
    s_exec.bytes = nullptr;
    s_exec.inst = nullptr;
    s_exec.state = NV_WRUN_IDLE;
    s_gfx.open = false;   // canvas buffers stay allocated for reuse; UI must stop drawing them now
    if (s_gfx.bl_touched) {   // ABI v4: a backlight app ran — restore the user's saved brightness
        nv_hal_backlight_set(nv_config_get_int("brightness", 90));
        s_gfx.bl_touched = false;
    }
    img_cache_flush();    // assets are per-app (name-only key): never let them leak into the next run
    pthread_mutex_unlock(&s_exec.lock);
    return true;
}

// Failure path once the engine is claimed (state == RUNNING but no worker yet): release the claim.
static void exec_unclaim(void) {
    pthread_mutex_lock(&s_exec.lock);
    free(s_exec.bytes);
    s_exec.bytes = nullptr;
    s_exec.state = NV_WRUN_IDLE;
    pthread_mutex_unlock(&s_exec.lock);
}

bool nv_wasm_exec_start(const nv_wasm_app_t *app, char *err, size_t err_n) {
    if (!app) { set_err(err, err_n, "no app"); return false; }
    if (!nv_wasm_init()) { set_err(err, err_n, "runtime init failed"); return false; }

    if (app->abi > NV_WASM_ABI) {
        char m[64];
        snprintf(m, sizeof m, "app needs host ABI v%u (OS has v%d)", (unsigned)app->abi, NV_WASM_ABI);
        set_err(err, err_n, m);
        return false;
    }

    // A finished-but-uncollected run (e.g. the viewer was torn down mid-run) is discarded here so
    // an orphan can never wedge the engine.
    nv_wasm_exec_collect(nullptr, nullptr, nullptr, 0);

    // Claim the engine atomically BEFORE the slow file I/O (no check-then-claim window).
    pthread_mutex_lock(&s_exec.lock);
    if (s_exec.state != NV_WRUN_IDLE) {
        pthread_mutex_unlock(&s_exec.lock);
        set_err(err, err_n, "busy");
        return false;
    }
    s_exec.state = NV_WRUN_RUNNING;
    s_exec.bytes = nullptr;
    pthread_mutex_unlock(&s_exec.lock);

    FILE *f = fopen(app->wasm_path, "rb");
    if (!f) { exec_unclaim(); set_err(err, err_n, "open app.wasm failed"); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > kMaxModuleSize) {
        fclose(f);
        exec_unclaim();
        set_err(err, err_n, "bad app.wasm size");
        return false;
    }
    uint8_t *bytes = (uint8_t *)malloc((size_t)sz);   // >16KB lands in PSRAM (SPIRAM malloc)
    if (!bytes) { fclose(f); exec_unclaim(); set_err(err, err_n, "oom"); return false; }
    const size_t rd = fread(bytes, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        free(bytes);
        exec_unclaim();
        set_err(err, err_n, "read app.wasm failed");
        return false;
    }

    if (!s_exec.outbuf) {
        s_exec.outbuf = (char *)heap_caps_malloc(kOutCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_exec.outbuf) s_exec.outbuf = (char *)malloc(kOutCap);
        if (!s_exec.outbuf) { free(bytes); exec_unclaim(); set_err(err, err_n, "oom"); return false; }
    }

    pthread_mutex_lock(&s_exec.lock);
    s_exec.app = *app;
    s_exec.bytes = bytes;
    s_exec.inst = nullptr;
    s_exec.abort_req = false;
    s_exec.out_w = s_exec.out_r = 0;
    s_exec.out_trunc = s_exec.out_trunc_told = false;
    s_exec.toast_n = 0;
    s_exec.elapsed_ms = 0;
    s_exec.t_start_us = esp_timer_get_time();

    req_defaults(&s_exec.req);
    s_exec.req.mod        = bytes;
    s_exec.req.mod_size   = (uint32_t)sz;
    s_exec.req.fn         = s_exec.app.entry[0] ? s_exec.app.entry : "run";
    s_exec.req.perms      = app->perms;
    s_exec.req.heap_size  = clamp_u32(app->ram_budget, kMinHeap, kMaxHeap);
    s_exec.req.stack_size = clamp_u32(app->stack_kb, kMinStackKb, kMaxStackKb) * 1024;
    s_exec.req.ex         = &s_exec;
    s_exec.req.game       = nv_wasm_app_is_game(app);
    snprintf(s_exec.req.tag, sizeof s_exec.req.tag, "app:%s", app->id);

    // A game gets its double-buffered canvas up before the worker can draw into it.
    bool gfx_fail = false;
    if (s_exec.req.game) gfx_fail = !gfx_open((int)app->canvas_w, (int)app->canvas_h);
    else                 s_gfx.open = false;

    // Spawn + thread_valid under the SAME lock hold: a worker that finishes instantly cannot get
    // its DONE collected before thread_valid is set (which would skip the join and leak).
    int rc = -1;
    if (!gfx_fail) {
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, kWorkerNativeStk);
        worker_stack_to_psram();
        rc = pthread_create(&s_exec.thread, &at, run_worker, &s_exec.req);
        pthread_attr_destroy(&at);
        if (rc == 0) s_exec.thread_valid = true;
    }
    pthread_mutex_unlock(&s_exec.lock);

    if (gfx_fail) { exec_unclaim(); set_err(err, err_n, "canvas alloc failed"); return false; }
    if (rc != 0) {
        exec_unclaim();
        set_err(err, err_n, "worker thread failed");
        return false;
    }
    NV_LOGI(TAG, "run '%s' (%u KB module, heap %u KB, stack %u KB, timeout %u ms)",
            app->id, (unsigned)(sz / 1024), (unsigned)(s_exec.req.heap_size / 1024),
            (unsigned)(s_exec.req.stack_size / 1024), (unsigned)app->timeout_ms);
    return true;
}

size_t nv_wasm_exec_read(char *dst, size_t n) {
    if (!dst || n == 0) return 0;
    pthread_mutex_lock(&s_exec.lock);
    size_t avail = s_exec.out_w - s_exec.out_r;
    size_t k = avail < n ? avail : n;
    if (k) {
        memcpy(dst, s_exec.outbuf + s_exec.out_r, k);
        s_exec.out_r += k;
    } else if (s_exec.out_trunc && !s_exec.out_trunc_told && s_exec.out_r == s_exec.out_w) {
        const char note[] = "[output truncated]\n";
        k = sizeof(note) - 1 < n ? sizeof(note) - 1 : n;
        memcpy(dst, note, k);
        s_exec.out_trunc_told = true;
    }
    pthread_mutex_unlock(&s_exec.lock);
    return k;
}

bool nv_wasm_exec_take_toast(int *kind, char *msg, size_t n) {
    bool got = false;
    pthread_mutex_lock(&s_exec.lock);
    if (s_exec.toast_n > 0) {
        if (kind) *kind = s_exec.toasts[0].kind;
        if (msg && n) snprintf(msg, n, "%s", s_exec.toasts[0].msg);
        s_exec.toast_n--;
        memmove(&s_exec.toasts[0], &s_exec.toasts[1], sizeof(s_exec.toasts[0]) * s_exec.toast_n);
        got = true;
    }
    pthread_mutex_unlock(&s_exec.lock);
    return got;
}

void nv_wasm_exec_abort(void) {
    pthread_mutex_lock(&s_exec.lock);
    if (s_exec.state == NV_WRUN_RUNNING) {
        s_exec.abort_req = true;
        s_gfx.want_stop = true;          // wakes a game blocked in gfx_present() -> it returns 0
        if (s_exec.inst) wasm_runtime_terminate(s_exec.inst);
        NV_LOGW(TAG, "abort requested for '%s'", s_exec.app.id);
    }
    pthread_mutex_unlock(&s_exec.lock);
}

// ---- ABI v2 game surface — UI accessors ---------------------------------------------------------
bool nv_wasm_app_is_game(const nv_wasm_app_t *a) {
    return a && a->abi >= 2 && (a->perms & NV_WPERM_GFX) && a->canvas_w > 0 && a->canvas_h > 0;
}

bool nv_wasm_gfx_is_open(void) {
    pthread_mutex_lock(&s_exec.lock);
    const bool o = s_gfx.open;
    pthread_mutex_unlock(&s_exec.lock);
    return o;
}

void nv_wasm_gfx_size(int *w, int *h) {
    pthread_mutex_lock(&s_exec.lock);
    if (w) *w = s_gfx.w;
    if (h) *h = s_gfx.h;
    pthread_mutex_unlock(&s_exec.lock);
}

uint16_t *nv_wasm_gfx_current(void) {
    pthread_mutex_lock(&s_exec.lock);
    uint16_t *p = s_gfx.open ? s_gfx.buf[s_gfx.ready_idx] : nullptr;
    pthread_mutex_unlock(&s_exec.lock);
    return p;
}

uint16_t *nv_wasm_gfx_take_frame(void) {
    uint16_t *p = nullptr;
    pthread_mutex_lock(&s_exec.lock);
    if (s_gfx.open && s_gfx.frame_ready) { p = s_gfx.buf[s_gfx.ready_idx]; s_gfx.frame_ready = false; }
    pthread_mutex_unlock(&s_exec.lock);
    return p;
}

void nv_wasm_gfx_set_input(int x, int y, int state) {
    pthread_mutex_lock(&s_exec.lock);
    s_gfx.in_x = x; s_gfx.in_y = y; s_gfx.in_state = state;
    pthread_mutex_unlock(&s_exec.lock);
}
// UI thread: publish the full multi-touch set (canvas coords), read by the ABI v3 gfx_touch_* imports.
void nv_wasm_gfx_set_multi(const int *xs, const int *ys, int n) {
    if (n < 0) n = 0; else if (n > 5) n = 5;
    pthread_mutex_lock(&s_exec.lock);
    for (int i = 0; i < n; i++) { s_gfx.mt_x[i] = xs[i]; s_gfx.mt_y[i] = ys[i]; }
    s_gfx.mt_cnt = n;
    pthread_mutex_unlock(&s_exec.lock);
}
// UI thread: post an OS back-gesture to the running game (it pops its own screen; the game view
// closes the app only when the game exits on its own).
void nv_wasm_gfx_request_back(void) {
    pthread_mutex_lock(&s_exec.lock);
    s_gfx.back_req++;
    pthread_mutex_unlock(&s_exec.lock);
}

uint32_t nv_wasm_gfx_present_seq(void) { return s_gfx.present_seq; }   // volatile u32, lock-free read
