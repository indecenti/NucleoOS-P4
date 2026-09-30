// nucleo_sdk.h — NucleoOS Anima WASM app SDK (host ABI v12).
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
#define NUCLEO_SDK_ABI 12

#ifdef NV_SIM   // native build against the PC simulator (tools/vertice): plain C declarations
#define NV_IMPORT(mod, sym)
#define NV_EXPORT(sym)
#else
#define NV_IMPORT(mod, sym) __attribute__((import_module(mod), import_name(sym)))
#define NV_EXPORT(sym)      __attribute__((export_name(sym), visibility("default")))
#endif

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

// ---- ABI v5 UDP networking (manifest "abi": 5, permission "net") ---------------------------------
// A tiny non-blocking UDP surface for LAN multiplayer. IPs are OPAQUE tokens: you get them from
// nv_net_recv (nv_net_from_ip) or nv_net_ip and pass them straight back to nv_net_send — never parse
// them. One socket per app; the OS closes it when the app exits. Typical flow: open a port on both
// devices, the host broadcasts a beacon, the guest replies to nv_net_from_ip, then both unicast.
NV_IMPORT("nv", "net_open")      int32_t nv_net_open(int32_t port);          // bind; 0 ok, <0 error
NV_IMPORT("nv", "net_close")     void    nv_net_close(void);
NV_IMPORT("nv", "net_send")      int32_t nv_net_send(int32_t ip, int32_t port, const void *buf, int32_t len);
NV_IMPORT("nv", "net_bcast")     int32_t nv_net_bcast(int32_t port, const void *buf, int32_t len);
NV_IMPORT("nv", "net_recv")      int32_t nv_net_recv(void *buf, int32_t maxlen);   // >0 bytes, 0 none, <0 err
NV_IMPORT("nv", "net_from_ip")   int32_t nv_net_from_ip(void);                // sender of the last recv
NV_IMPORT("nv", "net_from_port") int32_t nv_net_from_port(void);
NV_IMPORT("nv", "net_ip")        int32_t nv_net_ip(void);                     // our IPv4 token (0 = offline)
// Fetch a remote HTTP/HTTPS URL into buf (NUL-terminated). Returns bytes written, or <0 on error:
// -1 error/no-net-perm, -2 connect/DNS/TLS err, -3 HTTP status!=200, -4 buffer overflow. [permission: net]
NV_IMPORT("nv", "http_get")      int32_t nv_http_get(const char *url, void *buf, uint32_t maxlen);

// ---- ABI v6 dirty-rect engine (manifest "abi": 6, permission "gfx") ------------------------------
// The pro way to hit high FPS on this hardware. Call nv_gfx_persist(1) once: the OS then keeps ONE
// persistent buffer (no swap, no auto-clear) and re-blits only the pixels you actually draw each
// frame (auto-tracked). Draw the static scene once, nv_gfx_bg_save() it, then each frame erase the
// moving objects with nv_gfx_bg_restore(x,y,w,h) (copies the saved background back) and redraw them.
// A full-screen repaint (bandwidth-bound, ~2 fps) becomes a few tiny blits.
NV_IMPORT("nv", "gfx_persist")    void nv_gfx_persist(int32_t on);
NV_IMPORT("nv", "gfx_bg_save")    void nv_gfx_bg_save(void);                  // snapshot current buffer as background
NV_IMPORT("nv", "gfx_bg_restore") void nv_gfx_bg_restore(int32_t x, int32_t y, int32_t w, int32_t h);

// ---- ABI v7 opening files (manifest "abi": 7, NO permission needed) -----------------------------
// Declare the MIME types your app opens in the manifest ("opens": ["text/plain", "image/*"]); the OS
// then offers it in Files > Open with (and as a default app). When the user opens a file with your
// app, you may read exactly THAT file, read-only — no other path is reachable, so no "fs" permission
// is involved. Opened normally (from Home), nv_open_path() returns 0 and the others return -1.
// Absolute path of the file the app was opened with, NUL-terminated and truncated to len. Returns
// the path's full length (so a return >= len means it was truncated); 0 = not opened with a file.
NV_IMPORT("nv", "open_path")     int32_t nv_open_path(char *buf, int32_t len);
// Size of that file in bytes; -1 when there is none or it cannot be read.
NV_IMPORT("nv", "open_size")     int32_t nv_open_size(void);
// Read up to len bytes (the OS caps one call at 64 KB) at byte offset into buf. Returns bytes read,
// 0 at end of file, -1 on error / no file / negative offset. Loop it in chunks: the whole file never
// has to fit in your 64 KB linear memory.
NV_IMPORT("nv", "open_read")     int32_t nv_open_read(int32_t offset, void *buf, int32_t len);

// ---- ABI v9 game pad (permission "gfx") ----------------------------------------------------------
// USB keyboard and gamepads merged into one SNES-style pad. NV_PAD_KEYBOARD / NV_PAD_GAMEPAD say
// one is connected: hide your on-screen controls then. Keys: arrows/WASD, Space/X/Enter = A,
// Z/C/Backspace = B, V = X, B = Y, Q/E = L/R, P/Tab = Start, Esc = Select.
enum { NV_PAD_UP = 1, NV_PAD_DOWN = 2, NV_PAD_LEFT = 4, NV_PAD_RIGHT = 8, NV_PAD_A = 16, NV_PAD_B = 32,
       NV_PAD_X = 64, NV_PAD_Y = 128, NV_PAD_L = 256, NV_PAD_R = 512, NV_PAD_START = 1024,
       NV_PAD_SELECT = 2048, NV_PAD_GAMEPAD = 1 << 29, NV_PAD_KEYBOARD = 1 << 30 };
NV_IMPORT("nv", "gfx_pad")         int32_t nv_gfx_pad(void);

// ---- ABI v10 raw audio stream (manifest "abi": 10, permission "gfx") -----------------------------
// For synths, trackers and emulators: the app generates 16-bit signed PCM itself and feeds the
// speaker. One stream per run; the OS closes it when the app exits.
//   nv_audio_open(rate, channels)  rate 8000..48000, channels 1 or 2 (interleaved). 1 = ok, 0 = the
//                                  speaker is busy (e.g. the Music app is playing) or unavailable.
//   nv_audio_write(pcm, bytes)     queue samples; returns bytes taken (<0 = stream gone). Blocks
//                                  only if ~3 s are already queued, so...
//   nv_audio_backlog()             ...keep latency low: write only while the queued bytes are under
//                                  your target (e.g. 2048 frames ≈ 43 ms at 48 kHz). Call every frame.
//   nv_audio_close()               stop now (drops what is queued).
NV_IMPORT("nv", "audio_open")    int32_t nv_audio_open(int32_t rate, int32_t channels);
NV_IMPORT("nv", "audio_write")   int32_t nv_audio_write(const void *pcm, int32_t bytes);
NV_IMPORT("nv", "audio_backlog") int32_t nv_audio_backlog(void);
NV_IMPORT("nv", "audio_close")   void    nv_audio_close(void);

// ---- ABI v11 game controllers (manifest "abi": 11, permission "gfx") -----------------------------
// Each controller as its own player, already in the standard Xbox layout whatever the model: USB
// pads (generic HID mapped with the SDL GameControllerDB, Xbox 360/One/Series via XInput, Switch Pro,
// DualShock 4, DualSense) and Bluetooth LE pads (Xbox Series/One S, 8BitDo, Stadia...). Players are
// numbered in connection order. nv_gfx_pad() still works and ORs every controller together.
//   nv_pad_count()                     connected controllers, 0..4
//   nv_pad_state(i, &st, sizeof st)    fills nv_pad_state_t; returns bytes written, 0 = no player i
//   nv_pad_name(i, buf, len)           product name (NUL-terminated); returns its length
//   nv_pad_rumble(i, low, high, ms)    motors 0..65535 for ms (0 = stop); 1 = done, 0 = can't rumble.
//                                      The OS stops the motors when your app exits.
enum { NV_PADB_A = 1 << 0, NV_PADB_B = 1 << 1, NV_PADB_X = 1 << 2, NV_PADB_Y = 1 << 3,
       NV_PADB_BACK = 1 << 4, NV_PADB_GUIDE = 1 << 5, NV_PADB_START = 1 << 6,
       NV_PADB_LSTICK = 1 << 7, NV_PADB_RSTICK = 1 << 8, NV_PADB_LB = 1 << 9, NV_PADB_RB = 1 << 10,
       NV_PADB_UP = 1 << 11, NV_PADB_DOWN = 1 << 12, NV_PADB_LEFT = 1 << 13, NV_PADB_RIGHT = 1 << 14,
       NV_PADB_MISC = 1 << 15, NV_PADB_PADDLE1 = 1 << 16, NV_PADB_PADDLE2 = 1 << 17,
       NV_PADB_PADDLE3 = 1 << 18, NV_PADB_PADDLE4 = 1 << 19, NV_PADB_TOUCHPAD = 1 << 20,
       NV_PADB_LT = 1 << 21, NV_PADB_RT = 1 << 22 };   // LT / RT: trigger past half way
enum { NV_PAD_SRC_USB_HID = 1, NV_PAD_SRC_XINPUT = 2, NV_PAD_SRC_BLE = 3 };
typedef struct {
    uint32_t buttons;                  // NV_PADB_*
    int16_t  lx, ly, rx, ry;           // sticks -32768..32767, y grows DOWN
    int16_t  lt, rt;                   // triggers 0..32767
    uint8_t  source;                   // NV_PAD_SRC_*
    uint8_t  battery;                  // 0..100, 255 = wired / unknown
    uint8_t  mapped;                   // 1 = known model, 0 = layout guessed
    uint8_t  rumble;                   // 1 = nv_pad_rumble works
    uint16_t vid, pid;
} nv_pad_state_t;
NV_IMPORT("nv", "pad_count")     int32_t nv_pad_count(void);
NV_IMPORT("nv", "pad_state")     int32_t nv_pad_state(int32_t index, nv_pad_state_t *st, int32_t len);
NV_IMPORT("nv", "pad_name")      int32_t nv_pad_name(int32_t index, char *buf, int32_t len);
NV_IMPORT("nv", "pad_rumble")    int32_t nv_pad_rumble(int32_t index, int32_t low, int32_t high, int32_t ms);

// ---- ABI v12 network (manifest "abi": 12) -----------------------------------------------------
// Nothing here blocks: a request returns a handle at once and you poll it from your loop (at most
// 4 handles open; all of them are closed when the app exits). Permissions:
//   "net"  public Internet            "lan"  devices on the home network (private IPs, *.local)
//   "ws"   WebSocket (plus net/lan)   "mqtt" the system MQTT broker   "ha" Home Assistant
// The store shows these to the user before install; the user can revoke them in Settings.
// Redirects are never followed (http_status tells you 301/302...). Errors are negative:
enum {
    NV_NET_E_PERM = -1, NV_NET_E_ARG = -2, NV_NET_E_BUSY = -3, NV_NET_E_DEST = -4,
    NV_NET_E_CONNECT = -5, NV_NET_E_TOOBIG = -6, NV_NET_E_TIMEOUT = -7, NV_NET_E_CLOSED = -8,
    NV_NET_E_NOTCONF = -9,     // Home Assistant / MQTT not set up in Settings > Home
};
// HTTP: spec is JSON, e.g. {"url":"http://192.168.1.20/rpc/Switch.Set?id=0&on=true",
// "method":"POST","headers":{"Content-Type":"application/json"},"timeout":8000,"max":65536}.
NV_IMPORT("nv", "http_req")     int32_t nv_http_req(const char *spec_json, const void *body, uint32_t len);
NV_IMPORT("nv", "http_state")   int32_t nv_http_state(int32_t h);    // 0 running, 1 done, <0 error
NV_IMPORT("nv", "http_status")  int32_t nv_http_status(int32_t h);   // HTTP status once done
NV_IMPORT("nv", "http_read")    int32_t nv_http_read(int32_t h, void *buf, uint32_t len);   // 0 = end
NV_IMPORT("nv", "http_close")   void    nv_http_close(int32_t h);
// WebSocket
NV_IMPORT("nv", "ws_open")      int32_t nv_ws_open(const char *url, const char *headers_json);
NV_IMPORT("nv", "ws_state")     int32_t nv_ws_state(int32_t h);      // 0 connecting, 1 open, <0 closed
NV_IMPORT("nv", "ws_send")      int32_t nv_ws_send(int32_t h, const void *buf, uint32_t len, int32_t binary);
NV_IMPORT("nv", "ws_recv")      int32_t nv_ws_recv(int32_t h, void *buf, uint32_t len);   // msg length, 0 none
NV_IMPORT("nv", "ws_close")     void    nv_ws_close(int32_t h);
// MQTT through the OS connection (no broker address or password in your app). You can't publish
// under homeassistant/, nucleo/ or $..., nor subscribe to "#" alone. Payloads up to 8 KB.
NV_IMPORT("nv", "mqtt_sub")     int32_t nv_mqtt_sub(const char *filter);
NV_IMPORT("nv", "mqtt_pub")     int32_t nv_mqtt_pub(const char *topic, const void *buf, uint32_t len, int32_t retain);
NV_IMPORT("nv", "mqtt_recv")    int32_t nv_mqtt_recv(char *topic, uint32_t tcap, void *buf, uint32_t cap);  // -1 none
// Home Assistant with the token the user saved in Settings > Home — your app never sees it.
// ha_req returns an HTTP handle (read it with nv_http_*); path must start with "/api/".
// ha_ws opens {HA}/api/websocket already authenticated: the first message you get is auth_ok.
NV_IMPORT("nv", "ha_available") int32_t nv_ha_available(void);
NV_IMPORT("nv", "ha_req")       int32_t nv_ha_req(const char *method, const char *path, const void *body, uint32_t len);
NV_IMPORT("nv", "ha_ws")        int32_t nv_ha_ws(void);

// ---- ABI v9 Vertice — the OS 3D engine (manifest "abi": 9, permission "gfx") -------------------
// The scene lives in the OS and renders natively on BOTH cores straight into your canvas; your app
// only builds and moves things. Typical manifest: "canvas_w": 512, "canvas_h": 300,
// "canvas_scale": "fit" (the OS PPA-scales the canvas to the whole panel — exact 2x). Frame:
//     vx_render();                       // 3D into the canvas
//     nv_gfx_text(...);                  // 2D HUD on top (any nv_gfx_* call)
//     nv_gfx_present();
// World: integer units, Y up; angles in integer degrees; colours RGB565 (NV_RGB) unless "rgb888".
// Handles are small ints; -1 = refused (bad argument, a cap reached — 256 objects, 96 materials,
// 32 textures, 24000 triangles / 32000 vertices per scene, 8 emitters x 512 particles).
enum { VX_FLAT = 0, VX_GOURAUD = 1, VX_PHONG = 2, VX_WIRE = 3, VX_UNLIT = 4, VX_ADDITIVE = 5 };
enum { VX_CUBE = 0,       // a,b,c = width, height, depth
       VX_SPHERE = 1,     // a = radius, b = segments (3..48)
       VX_CYLINDER = 2,   // a = radius, b = height, c = segments
       VX_CAPSULE = 3,    // a = radius, b = total height, c = segments
       VX_PYRAMID = 4,    // a = base, b = height
       VX_PLANE = 5,      // a = width (X), b = depth (Z)
       VX_GRID = 6,       // a = width, b = depth, c = cells per side (1..64); mat/mat2 checkerboard
       VX_QUAD = 7,       // a = width, b = height (XY plane)
       VX_BILLBOARD = 8 };// a = width, b = height, always faces the camera
#define VX_TEX_KEY       1   // texture: magenta 0xF81F is transparent
#define VX_TEX_CLAMP     2   // texture: clamp instead of repeat
#define VX_MESH_SMOOTH   1   // mesh/model: smooth vertex normals (else faceted)
#define VX_PART_ADDITIVE 1   // emitter: glow (sparks, fire); else alpha (smoke, dust)
#define VX_PART_NODEPTH  2   // emitter: always on top
#define VX_DEPTH_NOTEST  1   // object: skips the depth test (overlays)
#define VX_DEPTH_NOWRITE 2   // object: does not write depth (decals, glass)
enum { VX_STAT_US = 0, VX_STAT_TRIS = 1, VX_STAT_QUEUED = 2, VX_STAT_BAND0_US = 3, VX_STAT_BAND1_US = 4,
       VX_STAT_SPLIT = 5, VX_STAT_SCENE_TRIS = 6, VX_STAT_MEM = 7, VX_STAT_PREP_US = 8,
       VX_STAT_PARTICLES = 9, VX_STAT_OBJECTS = 10 };

NV_IMPORT("nv", "vx_texture")      int32_t vx_texture_raw(const void *px, int32_t len, int32_t w, int32_t h, int32_t flags);
NV_IMPORT("nv", "vx_texture_load") int32_t vx_texture_load(const char *name, int32_t flags);  // img/<name>.565
// A blank texture, then filled a rectangle at a time: build big textures from a small buffer.
NV_IMPORT("nv", "vx_texture_new")  int32_t vx_texture_new(int32_t w, int32_t h, int32_t color565, int32_t flags);
NV_IMPORT("nv", "vx_texture_write") void   vx_texture_write_raw(int32_t tex, int32_t x, int32_t y, int32_t w, int32_t h,
                                                             const void *px, int32_t len);
NV_IMPORT("nv", "vx_material")     int32_t vx_material(int32_t color, int32_t shading, int32_t alpha, int32_t tex,
                                                       int32_t specular);                      // tex -1 = none
NV_IMPORT("nv", "vx_mat_color")    void    vx_mat_color(int32_t mat, int32_t color);
NV_IMPORT("nv", "vx_prim")         int32_t vx_prim(int32_t kind, int32_t a, int32_t b, int32_t c, int32_t mat, int32_t mat2);
NV_IMPORT("nv", "vx_mesh")         int32_t vx_mesh_raw(const void *xyz, int32_t xyz_len, const void *idx, int32_t idx_len,
                                                       const void *uv, int32_t uv_len, const void *mats, int32_t mats_len,
                                                       int32_t mat, int32_t flags);
NV_IMPORT("nv", "vx_model")        int32_t vx_model(const char *name, int32_t flags);          // models/<name>.vxm
NV_IMPORT("nv", "vx_clone")        int32_t vx_clone(int32_t id);
NV_IMPORT("nv", "vx_obj_free")     void    vx_obj_free(int32_t id);
NV_IMPORT("nv", "vx_obj_pos")      void    vx_obj_pos(int32_t id, int32_t x, int32_t y, int32_t z);
NV_IMPORT("nv", "vx_obj_rot")      void    vx_obj_rot(int32_t id, int32_t rx, int32_t ry, int32_t rz);
NV_IMPORT("nv", "vx_obj_show")     void    vx_obj_show(int32_t id, int32_t on);
// Depth: bias (0..127) pulls it toward the camera — road markings on a road; flags VX_DEPTH_*.
NV_IMPORT("nv", "vx_obj_depth")    void    vx_obj_depth(int32_t id, int32_t bias, int32_t flags);
// Level of detail: past `dist` world units from the camera `lod` (a simpler model) is drawn instead
// of `id`, following its position/rotation/visibility. Call again, farther, for a second level.
NV_IMPORT("nv", "vx_obj_lod")      int32_t vx_obj_lod(int32_t id, int32_t lod, int32_t dist);
NV_IMPORT("nv", "vx_obj_scale")    void    vx_obj_scale(int32_t id, int32_t percent);   // 100 = as built
NV_IMPORT("nv", "vx_camera")       void    vx_camera(int32_t x, int32_t y, int32_t z, int32_t rx, int32_t ry, int32_t rz);
NV_IMPORT("nv", "vx_look_at")      void    vx_look_at(int32_t x, int32_t y, int32_t z);
NV_IMPORT("nv", "vx_lens")         void    vx_lens(int32_t fov_deg, int32_t znear, int32_t zfar);
NV_IMPORT("nv", "vx_sun")          void    vx_sun(int32_t azimuth, int32_t elevation, int32_t rgb888, int32_t intensity);
NV_IMPORT("nv", "vx_ambient")      void    vx_ambient(int32_t rgb888);
NV_IMPORT("nv", "vx_sky")          void    vx_sky(int32_t top565, int32_t bottom565);        // gradient clear
NV_IMPORT("nv", "vx_fog")          void    vx_fog(int32_t znear, int32_t zfar);              // 0,0 = off
NV_IMPORT("nv", "vx_depth")        void    vx_depth(int32_t on);                            // z-buffer / painter
// Mode-7 floor: infinite textured ground at height y with NO triangles (per-row, lit, fogged).
// tex -1 = flat color565; repeat = world units per texture tile; 0 = off. Use vx_look_at cameras.
NV_IMPORT("nv", "vx_floor")        void    vx_floor(int32_t y, int32_t tex, int32_t repeat, int32_t color565);
// 360° panorama (mountains/clouds) around the horizon; texture row horizon_row on the horizon,
// magenta texels show the sky gradient. tex -1 = off.
NV_IMPORT("nv", "vx_panorama")     void    vx_panorama(int32_t tex, int32_t horizon_row);
// Particles: colour color0->color1 and size size0->size1 (world units) over life_ms; gravity in
// world units/s² (positive falls). vx_emit spawns `count` at (x,y,z), velocity (vx,vy,vz) units/s
// each randomised by ±spread.
NV_IMPORT("nv", "vx_emitter")      int32_t vx_emitter(int32_t max, int32_t color0, int32_t color1, int32_t size0,
                                                      int32_t size1, int32_t life_ms, int32_t gravity, int32_t flags);
NV_IMPORT("nv", "vx_emit")         void    vx_emit(int32_t em, int32_t x, int32_t y, int32_t z, int32_t vx, int32_t vy,
                                                   int32_t vz, int32_t spread, int32_t count);
NV_IMPORT("nv", "vx_reset")        void    vx_reset(void);                                  // drop the whole scene
NV_IMPORT("nv", "vx_render")       int32_t vx_render(void);                                 // -> triangles drawn
// Picking: arm a query at canvas pixel (x,y); after the next vx_render, vx_picked() is the handle
// of the nearest object drawn there (-1 = none).
NV_IMPORT("nv", "vx_pick_at")      void    vx_pick_at(int32_t x, int32_t y);
NV_IMPORT("nv", "vx_picked")       int32_t vx_picked(void);
NV_IMPORT("nv", "vx_stat")         int32_t vx_stat(int32_t what);                           // VX_STAT_*

static inline void vx_texture_write(int32_t tex, int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *px) {
    vx_texture_write_raw(tex, x, y, w, h, px, w * h * 2);
}
// A texture from pixels in your memory (RGB565, w/h power of two 8..256). Copied by the OS.
static inline int32_t vx_texture(const uint16_t *px, int32_t w, int32_t h, int32_t flags) {
    return vx_texture_raw(px, w * h * 2, w, h, flags);
}
// A mesh: nverts xyz triples, ntris uint16 index triples; uv (u,v int16 per vertex, 1024 = one
// texture repeat) and tri_mats (one material handle per triangle) are optional (NULL). Arrays must
// be naturally aligned (plain C arrays are). Wind triangles like the primitives do.
static inline int32_t vx_mesh(const int32_t *xyz, int32_t nverts, const uint16_t *idx, int32_t ntris,
                              const int16_t *uv, const uint8_t *tri_mats, int32_t mat, int32_t flags) {
    return vx_mesh_raw(xyz, nverts * 12, idx, ntris * 6, uv, uv ? nverts * 4 : 0,
                       tri_mats, tri_mats ? ntris : 0, mat, flags);
}

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
// Same verbs into a buffer (always NUL-terminated); returns the length written. Handy for the
// ABI v12 request specs: nv_snprintf(spec, sizeof spec, "{\"url\":\"http://%s/api\"}", host).
int  nv_snprintf(char *out, size_t n, const char *fmt, ...);

// Freestanding essentials (the compiler may emit calls to these for struct copies etc.).
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
size_t strlen(const char *s);

#ifdef __cplusplus
}
#endif
