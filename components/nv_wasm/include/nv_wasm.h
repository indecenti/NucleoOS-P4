// nv_wasm — WebAssembly app runtime for NucleoOS Anima (Phase 4).
//
// Wraps WAMR (WebAssembly Micro Runtime, interpreter mode) behind a small API:
//   * slice 1 — runtime bring-up + bundled demo modules (kept as a boot self-test),
//   * slice 2 — the real app platform: a versioned host-import ABI ("nv" module,
//     capability-gated, string-capable), manifest v2 (ram/stack/timeout/abi), and an
//     asynchronous runner so the LVGL thread never blocks on a running module.
//
// ---- Host-import ABI v1 (WASM import module "nv") ----------------------------------------------
// Strings are NUL-terminated in app memory; WAMR validates + converts them before the host runs
// (an out-of-bounds pointer traps the app, never the OS). Buffer params are (ptr, len) validated
// the same way. Gated imports silently no-op (and log a warning) without the permission bit.
//
//   import                          signature        permission  notes
//   nv.print(msg)                   ($)              log         append to the app's output panel
//   nv.log(level, msg)              (i$)             log         0=error 1=warn 2=info 3=debug
//   nv.toast(kind, msg)             (i$)             ui          0=info 1=ok 2=warn 3=error
//   nv.millis() -> i32              ()i              —           ms since boot
//   nv.time_unix() -> i64           ()I              —           wall clock (UTC epoch seconds)
//   nv.lang(buf, len) -> i32        (*~)i            —           active locale code ("en", "it", …)
//   nv.rand() -> i32                ()i              —           hardware RNG
//   nv.sleep_ms(ms)                 (i)              —           clamped to 1000 ms per call
//
//   env.host_log(i32)               (i)              log         legacy slice-1 import (kept so
//                                                                already-installed apps keep running)
//
// ---- Host-import ABI v2 additions: the game surface (import module "nv", permission "gfx") -----
// A "game" app declares "abi": 2, permission "gfx", and canvas_w/canvas_h in its manifest. The OS
// then hands it a full-screen RGB565 canvas and runs it frame-driven: the guest owns its loop
//
//     void run(void){ setup(); while (nv_gfx_present()) { input(); update(); draw(); } }
//
// and every draw command is executed NATIVELY by the OS (fast, PPA-friendly), so the interpreted
// guest only runs game logic. nv_gfx_present() blocks until the OS has shown the frame (frame
// pacing) and returns 0 when the OS wants the app to exit (cooperative stop).
//
//   import                                   signature    notes
//   nv.gfx_width() / gfx_height() -> i32     ()i          canvas size (== manifest canvas_w/h)
//   nv.gfx_clear(color)                      (i)          fill whole canvas (RGB565)
//   nv.gfx_rect(x,y,w,h,color)               (iiiii)      filled rectangle
//   nv.gfx_circle(cx,cy,r,color)             (iiii)       filled circle
//   nv.gfx_line(x0,y0,x1,y1,color)           (iiiii)      2px line
//   nv.gfx_blit(ptr,len,x,y,w,h)             (*~iiii)     copy an RGB565 image from guest memory
//   nv.gfx_present() -> i32                  ()i          show frame + pace; 0 => exit requested
//   nv.gfx_input() -> i32                    ()i          packed touch: (state<<24)|(y<<12)|x
//   nv.gfx_tone(freq,ms)                     (ii)         short beep (ES8311)
//   nv.gfx_touch_count() -> i32              ()i          [ABI 3] fingers down now (0..5)
//   nv.gfx_touch_point(idx) -> i32           (i)i         [ABI 3] finger idx: (valid<<24)|(y<<12)|x
//   nv.gfx_text_width(s,scale) -> i32        ($i)i        [ABI 4] px width nv.gfx_text would advance
//   nv.backlight(level)                      (i)          [ABI 4] panel brightness 0..100 (restored on exit)
//
// Bump NV_WASM_ABI when the table above changes incompatibly; apps declare the ABI they need in
// their manifest and the runner refuses newer-than-OS apps with a clear error. New imports are
// additive (old apps that don't import them are unaffected), so ABI 3 stays backward compatible.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Version of the host-import ABI implemented by this OS build (manifest "abi" is checked
// against it at run time).
#define NV_WASM_ABI 4

// Initialize the WAMR runtime once (idempotent). Returns false if it could not start.
bool nv_wasm_init(void);

// Run the bundled demo module: calls its exported add(a,b) and writes the result to *out.
// Synchronous boot self-test (blocks the caller for ~ms). Returns false on error (msg in err).
bool nv_wasm_run_demo(int a, int b, int *out, char *err, size_t err_n);

// Run the bundled "app" module (imports env.host_log), proving the host-import path.
// Synchronous boot self-test. Returns false on error (msg in err).
bool nv_wasm_run_app(char *err, size_t err_n);

// ---- installed WASM apps (manifest + SD) -------------------------------------------------------

// Capabilities an app may declare in its manifest ("permissions":["log","ui",...]). Host imports
// are gated on these — a call from an app lacking the bit is denied.
typedef enum {
    NV_WPERM_LOG = 1u << 0,   // nv.print / nv.log / env.host_log
    NV_WPERM_UI  = 1u << 1,   // nv.toast (on-screen output)
    NV_WPERM_NET = 1u << 2,   // network (future)
    NV_WPERM_FS  = 1u << 3,   // filesystem (future)
    NV_WPERM_GFX = 1u << 4,   // ABI v2 game surface (nv.gfx_* / present / input / tone)
} nv_wperm_t;

// One installed app, read from /sdcard/apps/<id>/manifest.json. All fields are validated and
// clamped at scan time, so consumers may trust them.
typedef struct {
    char     id[32];           // directory name; [A-Za-z0-9_-] only
    char     name[40];
    char     version[16];
    char     wasm_path[160];   // absolute path to app.wasm on the SD card
    char     entry[24];        // exported entry function (default "run")
    uint32_t ram_budget;       // bytes; also the module instance heap (clamped 64 KB … 8 MB)
    uint32_t stack_kb;         // WASM operand stack, KB (manifest "stack_kb", clamped 4 … 256)
    uint32_t timeout_ms;       // run watchdog (manifest "timeout_ms", clamped 1 s … 120 s)
    uint32_t abi;              // required host ABI (manifest "abi", default 1)
    uint32_t perms;            // OR of nv_wperm_t
    uint32_t canvas_w;         // ABI v2 game canvas width  (manifest "canvas_w", 0 = not a game)
    uint32_t canvas_h;         // ABI v2 game canvas height (manifest "canvas_h")
} nv_wasm_app_t;

// True when this app is an ABI v2 game (abi>=2, "gfx" permission, and a non-zero canvas).
bool nv_wasm_app_is_game(const nv_wasm_app_t *a);

// If /sdcard/apps has no demo app (or an older bundled one), write the current bundled demo
// (manifest.json + app.wasm using the ABI v1 imports). No-op when up to date or SD absent.
void nv_wasm_seed_demo(void);

// Write the built-in Nucleo Tanks game (ABI v2) to /sdcard/apps/tanks if missing/older. Games
// ship embedded because the web-OS FS API is sandboxed away from /sdcard/apps. Call before the scan.
void nv_wasm_seed_tanks(void);

// Discover installed apps: scan /sdcard/apps/<id>/manifest.json. Fills up to `max` entries and
// returns the count (0 if none / no SD). Invalid manifests/ids are skipped with a log line.
int nv_wasm_scan(nv_wasm_app_t *out, int max);

// Load ONE app's manifest fresh from /sdcard/apps/<id>/ (validated + clamped like the scan).
// Lets callers run an app pushed after the boot scan (web console hot-reload).
bool nv_wasm_load_manifest(const char *id, nv_wasm_app_t *out);

// Human list of permission names ("log, ui") into buf; returns buf.
const char *nv_wasm_perm_str(uint32_t perms, char *buf, size_t n);

// Delete an installed app: removes /sdcard/apps/<id>/ (app.wasm + manifest.json + the dir).
// Refuses while that app is the running one. Returns false (msg in err) on error. The launcher
// tile is registered at boot, so it disappears from Home only after a restart.
bool nv_wasm_uninstall(const char *id, char *err, size_t err_n);

// ---- asynchronous runner (one app at a time — solo-mode discipline) ----------------------------
// The module runs on a worker pthread; the UI polls from an LVGL timer. Nothing here touches
// LVGL, so the engine is UI-agnostic and the LVGL thread never blocks on a run.
//
//   start -> poll state / drain output+toasts -> (optionally abort) -> DONE -> collect -> IDLE
//
// All functions are safe to call from the LVGL thread; host imports write from the worker side
// under an internal lock.

typedef enum {
    NV_WRUN_IDLE = 0,   // no run; start() allowed
    NV_WRUN_RUNNING,    // worker executing (drain output, may abort)
    NV_WRUN_DONE,       // finished; result waits for collect()
} nv_wrun_state_t;

nv_wrun_state_t nv_wasm_exec_state(void);

// Begin an async run of app->entry. Auto-collects (discards) a stale DONE run first. Returns
// false (msg in err) when a run is still executing or the app/ABI is unusable.
bool nv_wasm_exec_start(const nv_wasm_app_t *app, char *err, size_t err_n);

// id of the app currently started (valid in RUNNING/DONE; "" when idle).
const char *nv_wasm_exec_app_id(void);

// Drain any new nv.print output into dst (appends nothing when quiet). Returns bytes copied.
size_t nv_wasm_exec_read(char *dst, size_t n);

// Pop one queued nv.toast(kind,msg); returns false when the queue is empty.
bool nv_wasm_exec_take_toast(int *kind, char *msg, size_t n);

// Ask the running module to stop (wasm_runtime_terminate). The worker then finishes -> DONE.
// No-op unless RUNNING.
void nv_wasm_exec_abort(void);

// When DONE: fetch the result, join + free the run, return to IDLE. Returns true exactly once
// per run; false while IDLE/RUNNING. err receives the failure text ("" on success).
bool nv_wasm_exec_collect(bool *ok, uint32_t *elapsed_ms, char *err, size_t err_n);

// ---- ABI v2 game surface — UI side (call from the LVGL thread) ----------------------------------
// The runner allocates the canvas (double-buffered, PSRAM) when a game run starts; the guest draws
// into it via the nv.gfx_* imports on the worker thread. The UI shows frames like this:
//   size() to build an lv_canvas over current(); each LVGL tick, take_frame() returns the next
//   ready buffer (or NULL) — set it on the canvas and invalidate. input() feeds the latest touch.
bool      nv_wasm_gfx_is_open(void);
void      nv_wasm_gfx_size(int *w, int *h);
uint16_t *nv_wasm_gfx_current(void);        // a valid buffer to seed the canvas at build time
uint16_t *nv_wasm_gfx_take_frame(void);     // next ready frame buffer, or NULL if none pending
void      nv_wasm_gfx_set_input(int x, int y, int state);   // 0 = up, 1 = down
void      nv_wasm_gfx_set_multi(const int *xs, const int *ys, int n);   // full multi-touch (canvas coords)
void      nv_wasm_gfx_request_back(void);                   // UI: forward an OS back gesture to the game
// Liveness heartbeat: bumps on EVERY gfx_present call (including the static-screen skip path) —
// present is a game's one cooperative point, so a stalled counter while RUNNING means the guest
// is stuck in a loop that will never see want_stop. The watcher then calls nv_wasm_exec_abort(),
// which (THREAD_MGR) forcibly interrupts the interpreter; the run lands in DONE for collection.
uint32_t  nv_wasm_gfx_present_seq(void);
// ABI v4: pending guest backlight request (0..100), or -1 if none since the last call. The UI drains
// this and applies it via LEDC on its own thread, then restores the user's brightness on teardown.
int       nv_wasm_gfx_take_backlight(void);

#ifdef __cplusplus
}
#endif
