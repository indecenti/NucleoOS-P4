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
// ---- Host-import ABI v5: UDP networking (permission "net") — LAN multiplayer --------------------
//   nv.net_open(port) -> i32                 (i)i         [ABI 5] bind a UDP socket (0 ok, <0 err)
//   nv.net_close()                           ()           [ABI 5] close it (also auto-closed on exit)
//   nv.net_send(ip,port,ptr,len) -> i32      (ii*~)i      [ABI 5] datagram to ip:port (ip = opaque token)
//   nv.net_bcast(port,ptr,len) -> i32        (i*~)i       [ABI 5] broadcast to 255.255.255.255:port
//   nv.net_recv(ptr,maxlen) -> i32           (*~)i        [ABI 5] non-blocking; >0 bytes, 0 none, <0 err
//   nv.net_from_ip() / net_from_port() -> i32 ()i         [ABI 5] sender of the last net_recv
//   nv.net_ip() -> i32                       ()i          [ABI 5] our STA IPv4 (opaque token; 0 offline)
// ---- Host-import ABI v7: the launch file (NO permission bit — the user's choice of app is the grant)
// An app declaring "opens" MIME patterns is offered by the OS "Open" / "Open with" for those files.
// When launched on a file it may read exactly THAT file, read-only; nothing else on the card is
// reachable. Apps not opened with a file get 0 / -1 back.
//   nv.open_path(buf,len) -> i32             (*~)i        [ABI 7] granted absolute path (NUL-terminated,
//                                                         truncated to len); returns its full length, 0 = none
//   nv.open_size() -> i32                    ()i          [ABI 7] granted file size in bytes; -1 none/unreadable
//   nv.open_read(off,buf,len) -> i32         (i*~)i       [ABI 7] read <= len (max 64 KB) at off; bytes read,
//                                                         0 at EOF, -1 on error / no grant / off < 0
// ---- Host-import ABI v8: non-local exit for ported C code (NO permission) -----------------------
// wasi-sdk's setjmp/longjmp needs the WebAssembly exception-handling proposal, which WAMR's fast
// interpreter and AOT don't run. Code that recovers from errors with longjmp (Lua) goes through
// the host instead (ports/common/nv_sjlj.h):
//   nv.try_call(fn,ud) -> i32                (ii)i        [ABI 8] call table entry fn(ud); 0 = it returned,
//                                                         1 = nv.throw unwound it
//   nv.throw()                               ()           [ABI 8] unwind to the innermost try_call
//
// ---- Host-import ABI v9: Vertice, the OS 3D engine (permission "gfx") — see vertice.h ------------
// Scene state lives in the OS (rendered on both cores straight into the canvas draw buffer); the
// guest only builds and moves things. Handles are small ints, -1 = refused. Pair it with the
// manifest "canvas_scale": "fit" (small canvas, e.g. 512x300, PPA-scaled to the whole panel).
//   nv.gfx_pad() -> i32                      ()i        USB keyboard + gamepads as one SNES-style pad (NV_PAD_*)
//   nv.vx_texture(px,len,w,h,flags) -> i32   (*~iii)i   RGB565 w×h (pow2 8..256), copied; flags 1 key, 2 clamp
//   nv.vx_texture_load(name,flags) -> i32    ($i)i      the app's img/<name>.565
//   nv.vx_texture_new(w,h,c565,flags) -> i32 (iiii)i    blank texture; nv.vx_texture_write(t,x,y,w,h,px,len) (iiiii*~)
//   nv.vx_material(c565,shade,alpha,tex,spec) -> i32 (iiiii)i  0 flat 1 gouraud 2 phong 3 wire 4 unlit 5 add
//   nv.vx_mat_color(mat,c565)                (ii)
//   nv.vx_prim(kind,a,b,c,mat,mat2) -> i32   (iiiiii)i  cube/sphere/cylinder/capsule/pyramid/plane/grid/quad/billboard
//   nv.vx_mesh(xyz,idx,uv,mats,mat,flags) -> i32 (*~*~*~*~ii)i  int32 xyz, uint16 tris, int16 uv, uint8 mat/tri
//   nv.vx_model(name,flags) -> i32           ($i)i      the app's models/<name>.vxm
//   nv.vx_clone(id) -> i32 (i)i   nv.vx_obj_free(id) (i)   nv.vx_obj_show(id,on) (ii)
//   nv.vx_obj_pos/obj_rot(id,x,y,z)          (iiii)     position / Euler degrees
//   nv.vx_obj_depth(id,bias,flags)           (iii)      z bias (decals), 1 no test, 2 no write
//   nv.vx_obj_lod(id,lod,dist) -> 0/-1       (iii)i     simpler stand-in past dist (2 levels)
//   nv.vx_obj_scale(id,percent)              (ii)       uniform size, 100 = as built
//   nv.vx_camera(x,y,z,rx,ry,rz) (iiiiii)   nv.vx_look_at(x,y,z) (iii)   nv.vx_lens(fov,near,far) (iii)
//   nv.vx_sun(az,el,rgb888,intensity) (iiii) nv.vx_ambient(rgb888) (i)
//   nv.vx_sky(top565,bottom565) (ii)        nv.vx_fog(near,far) (ii)     nv.vx_depth(on) (i)
//   nv.vx_floor(y,tex,repeat,c565) (iiii)   Mode-7 floor     nv.vx_panorama(tex,horizon_row) (ii)
//   nv.vx_water(strength,wave)       (ii)     the floor mirrors panorama + sky (Vertice 1.2)
//   nv.vx_emitter(max,c0,c1,s0,s1,life,grav,flags) -> i32 (iiiiiiii)i
//   nv.vx_emit(em,x,y,z,vx,vy,vz,spread,count) (iiiiiiiii)
//   nv.vx_reset() ()   nv.vx_render() -> i32 ()i   nv.vx_pick_at(x,y) (ii)   nv.vx_picked() -> i32 ()i
//   nv.vx_stat(what) -> i32                  (i)i       timings / counts (VX_STAT_*)
//
// ---- Host-import ABI v11: game controllers (permission "gfx") — see nv_pad.h -------------------
// Every controller the OS knows (USB HID with the SDL GameControllerDB mappings, USB XInput,
// Bluetooth LE HID) as its own pad in the standard Xbox layout; index = player, connection order.
//   nv.pad_count() -> i32                    ()i        connected controllers (0..4)
//   nv.pad_state(i,buf,len) -> i32           (i*~)i     nv_pad_state_t (24 bytes: buttons u32, lx ly rx ry
//                                                       lt rt i16, source battery mapped rumble u8, vid pid
//                                                       u16); bytes written, 0 = no such pad
//   nv.pad_name(i,buf,len) -> i32            (i*~)i     product name, NUL-terminated; its length
//   nv.pad_rumble(i,low,high,ms) -> i32      (iiii)i    motors 0..65535 for ms (0 = stop); 1 = done.
//                                                       Stopped when the app exits.
//
// ---- Host-import ABI v12: network (docs/HOME_AUTOMATION_PLAN.md F3) ---------------------------
// Never blocking: a request returns a handle at once, the app polls it (at most 4 handles open).
// Destinations: "net" = public Internet, "lan" = private addresses (home network, *.local); the
// host resolves and classifies the name first and never follows redirects. Errors are negative:
// -1 permission, -2 bad argument, -3 busy/no handle, -4 destination not allowed, -5 connect/TLS,
// -6 response too big, -7 timeout, -8 closed, -9 not configured in Settings.
//   nv.http_req(spec,body,len) -> h          ($*~)i     spec JSON {"url","method","headers":{..},
//                                                       "timeout" ms,"max" bytes (64 KB, <= 1 MB)}
//   nv.http_state(h) -> i32                  (i)i       0 running, 1 done, <0 error
//   nv.http_status(h) -> i32                 (i)i       HTTP status once done (3xx not followed)
//   nv.http_read(h,buf,len) -> i32           (i*~)i     next part of the body, 0 at the end
//   nv.http_close(h) (i)                     frees the handle (also: everything at app exit)
//   nv.ws_open(url,headers_json) -> h        ($$)i      ws:// wss:// ("ws" + "net"/"lan")
//   nv.ws_state(h) -> i32                    (i)i       0 connecting, 1 open, <0 closed/error
//   nv.ws_send(h,buf,len,binary) -> i32      (i*~i)i    queued bytes
//   nv.ws_recv(h,buf,len) -> i32             (i*~)i     next message length (truncated to len), 0 none
//   nv.ws_close(h) (i)
//   nv.mqtt_sub(filter) -> i32               ($)i       "mqtt": the system broker (Settings > Home)
//   nv.mqtt_pub(topic,buf,len,retain) -> i32 ($*~i)i    not under homeassistant/ nucleo/ $...
//   nv.mqtt_recv(topic,tcap,buf,cap) -> i32  (*~*~)i    payload length, -1 = nothing waiting
//   nv.ha_available() -> i32                 ()i        "ha": Home Assistant URL + token configured
//   nv.ha_req(method,path,body,len) -> h     ($$*~)i    {ha_url}/api/... with the system token; read
//                                                       it with http_state/status/read/close
//   nv.ha_ws() -> h                          ()i        {ha_url}/api/websocket, authenticated by the
//                                                       host (the app receives auth_ok, never the token)

//
// ---- ABI v13: LAN discovery -------------------------------------------------------------------
//   nv.mdns_browse(service,proto) -> h      ($$)i      "lan": mDNS instances as text lines
//                                                       "instance|host|ipv4|port|k=v;k=v" via http_read
//
// ---- ABI v14: raw keyboard + mouse, engine packages ----------------------------------------------
//   nv.kbd_state(buf,len) -> n                  (*~)i      "gfx": modifiers + held HID usages, -1 none
//   nv.mouse_read(m,len) -> 1|0                 (*~)i      "gfx": {dx,dy,wheel,buttons} since last call;
//                                                           first call captures the pointer for the run
//   manifest "engine": "<id>"  a game/content package with no module of its own: it runs the module
//     of the package <id> (which it must also list in "requires"), with its own id, permissions,
//     save/data folder and canvas. WASI apps then also get "/engine" = that package's data folder
//     and env NUCLEO_ENGINE=<id>. manifest "args": ["..."] = argv[1..] for any WASI app.
//
// ---- ABI v18: the system downloader (nv_dl; docs/ZIM_READER_PLAN.md §4f) ------------------------
// Big files in the background, checked piece by piece, resumed after a dropped connection or a restart;
// a download goes on after the app closes. Permission "net"; the file lands where the app sees it
// ("home": the shared workspace, else "fs": its data folder), never over an existing file. An app sees
// only its own downloads. Errors: -1 permission, -2 bad spec/id/path, -3 too many listed, -4 path kept
// by the system, -5 the file exists or is being downloaded, -6 no card, -7 answer bigger than the buffer.
//   nv.dl_start(spec) -> id                 ($)i       spec JSON {"meta4": https URL} or {"url": https URL,
//                                                       "size": bytes, "sha256": hex}, + "dest" (the app's
//                                                       path, e.g. "/zim/x.zim") and "title"
//   nv.dl_info(id,buf,len) -> n             (i*~)i     JSON {"id","state" (queued|paused|done|failed|
//                                                       running),"title","dest","size","done","verified",
//                                                       "bps","phase","error"}
//   nv.dl_list(buf,len) -> n                (*~)i      JSON array of those, oldest first
//   nv.dl_pause(id,on) -> 1|0               (ii)i      on=0 resumes a paused one or retries a failed one
//   nv.dl_cancel(id) -> 1                   (i)i       stops and deletes the partial file; a finished
//                                                       download is only forgotten (the file stays)
//   nv.dl_free() -> MiB                     ()i        room for a new download: free space less the OTA
//                                                       reserve and what listed downloads still need
//
// ---- ABI v20: VoxelSpaceNG terrain engine (permission "gfx"; nv_wasm_vs.cpp, components/voxel) ----
// A Voxel Space heightfield renderer on both cores. Files: /sdcard/apps/<id>/data/<name>.vsw (world)
// and .vss (sky), made by the voxelspace-esp32 baker. The frame is column-major = the buffer of a
// quarter-turned canvas: call gfx_set_rotation(1) first (vs_render returns -2 otherwise).
//   nv.vs_open(world, sky) -> i32          ($$)i      0, -1 args/perm, -2 file, -3 format, -4 memory
//   nv.vs_close()                           ()         also done when the run ends
//   nv.vs_camera(x,y,h,yaw,pitch,roll,t)    (iiiiiii)  x,y Q16 cells; h Q4 height units; yaw 0..4095;
//                                                       pitch px (+ looks down); roll Q16 px/px; t ms
//   nv.vs_render() -> i32                   ()i        whole frame into the canvas; render time in us
//   nv.vs_height(x,y) -> i32                (ii)i      ground height at Q16 cells, Q4 (bilinear)
//   nv.vs_config(key, value) -> i32         (ii)i      0 fov, 1 zfar, 2 quality, 3 near, 4 smooth,
//                                                       5 detail, 6 target ms (adaptive), 7 cores, 8 fog,
//                                                       9 direct (1 = no SRAM strips + DMA), 10 xscale
//                                                       (0 auto, 1 every column, 2 every other, interpolated);
//                                                       returns the value in effect (out of range: kept)
//   nv.vs_info(key) -> i32                  (i)i       0 log2 size, 1 water, 2 max height, 3 samples,
//                                                       4 textured px, 5 render us, 6 quality, 7 SRAM+DMA,
//                                                       8 xscale in use
//
// ---- ABI v19: turned canvas (permission "gfx"; manifest "canvas_scale" fit/stretch/zoom) ---------
// The PPA turns the canvas while it scales it to the panel, so a portrait app draws upright and pays
// nothing for the turn (from WASM it costs ~95 ms a full frame: PSRAM-bound).
//   nv.gfx_set_rotation(q) -> i32           (i)i       q quarter turns, 0..3; returns q, or -1 refused
//                                                       (no gfx, 1:1 canvas, WASM-4 cart, Vertice bound).
//                                                       Odd q swaps the canvas sides (gfx_width/height
//                                                       report the new ones: 1024x600 -> 600x1024); the
//                                                       canvas is cleared and wholly redrawn on the next
//                                                       present. Upright pixel (x,y) shows on the panel at
//                                                       q=1 (y, W-1-x), q=2 (W-1-x, H-1-y), q=3 (H-1-y, x)
//                                                       (W x H = the upright canvas): q=1 reads upright
//                                                       with the device's left edge up, like the system's
//                                                       portrait mode. Touch, mouse and remote taps arrive
//                                                       in upright canvas pixels. Every run starts at 0.
//
// ---- ABI v19: hardware JPEG decoder (permission "gfx") -------------------------------------------
// The P4's decoder takes baseline JPEG (8-bit, grey or YCbCr 4:4:4 / 4:2:2 / 4:2:0, up to 4096 a side);
// the result is area-averaged by the CPU to the exact size asked (nv_wasm_jpeg.cpp). Anything else is
// refused with a code, so an app keeps its software decoder as the fallback. Errors (NV_JPEG_E_*):
// -1 no "gfx", -2 bad argument, -3 not decodable by the hardware, -4 no memory, -5 damaged stream.
//   nv.jpeg_info(jpg,len) -> i32             (*~)i      (w << 16) | h when the hardware decodes it, else <0
//   nv.jpeg_decode(jpg,len,out,cap,w,h) -> i32 (*~*~ii)i  0: out holds the picture scaled to exactly w x h
//                                                       (1..4096), RGB565 in the canvas byte order, rows of
//                                                       w pixels; cap >= w*h*2, out 2-byte aligned.
//                                                       Synchronous; all its buffers are freed on return.
//
// ---- Console programs (Terminal) ---------------------------------------------------------------
// A WASI command whose manifest says "console": true is a terminal program: the Terminal runs it
// with a command line (argv), a live stdin (what the user types, line by line) and no opcode cap
// or timeout — the user stops it. Permission "home" preopens the shared workspace /sdcard/home as
// "/" (with "fs" too, the private data folder is "/appdata").
// "tty": true too makes it a full-screen program (curses): it gets TERM=xterm-256color,
// LANG=C.UTF-8 and the screen size as COLUMNS/LINES, stdin can be polled with a timeout
// (poll/select: ESC and key timeouts), and the Terminal keeps that grid size while it runs (the
// on-screen keyboard scrolls the view instead of resizing the screen) and turns taps into xterm
// mouse clicks when the program asks for the mouse.
//
// Bump NV_WASM_ABI when the table above changes incompatibly; apps declare the ABI they need in
// their manifest and the runner refuses newer-than-OS apps with a clear error. New imports are
// additive (old apps that don't import them are unaffected), so ABI 3 stays backward compatible.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Version of the host-import ABI implemented by this OS build (manifest "abi" is checked
// against it at run time).
#define NV_WASM_ABI 20   // 20: VoxelSpaceNG terrain engine (vs_*); 19: turned scaled canvas (gfx_set_rotation); 18: the system downloader (dl_*); 17: full-screen terminal programs (tty); 16: Vertice 1.5

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
    NV_WPERM_NET = 1u << 2,   // public Internet (nv.http_get, ABI v12 http/ws; UDP: see the SDK)
    NV_WPERM_FS  = 1u << 3,   // WASI: the app's private data folder
    NV_WPERM_GFX = 1u << 4,   // ABI v2 game surface (nv.gfx_* / present / input / tone)
    NV_WPERM_HOME = 1u << 5,  // WASI: the user's shared workspace /sdcard/home as "/"
    // ABI v12 (network for apps; docs/HOME_AUTOMATION_PLAN.md F3). NET = public Internet only.
    NV_WPERM_LAN  = 1u << 6,  // HTTP/WS to private addresses (home devices, Shelly, Tasmota, ...)
    NV_WPERM_WS   = 1u << 7,  // WebSocket client (nv.ws_*); destination still needs net or lan
    NV_WPERM_MQTT = 1u << 8,  // publish/subscribe through the system MQTT connection (nv.mqtt_*)
    NV_WPERM_HA   = 1u << 9,  // Home Assistant through the system token (nv.ha_*)
    NV_WPERM_CAMERA = 1u << 10,  // reserved: camera frames
    NV_WPERM_MIC  = 1u << 11,    // reserved: microphone
} nv_wperm_t;

// Permissions the store shows before install and Settings > Security lets the user revoke.
#define NV_WPERM_SENSITIVE (NV_WPERM_NET | NV_WPERM_LAN | NV_WPERM_WS | NV_WPERM_MQTT | NV_WPERM_HA | \
                            NV_WPERM_FS | NV_WPERM_HOME | NV_WPERM_CAMERA | NV_WPERM_MIC)

// Manifest/catalog name <-> bit ("net" <-> NV_WPERM_NET). 0 / nullptr when unknown.
uint32_t    nv_wasm_perm_bit(const char *name);
const char *nv_wasm_perm_name(uint32_t bit);

// Per-app revocations (Settings > Security > App permissions), persisted in
// /sdcard/nucleos/perms.json. A run gets manifest permissions minus the revoked ones. Any task.
uint32_t nv_wasm_perm_revoked(const char *app_id);
void     nv_wasm_perm_set_revoked(const char *app_id, uint32_t mask);

// One dependency from a manifest "requires" map: package or system component id + minimum version.
typedef struct { char id[32]; char version[12]; } nv_wasm_dep_t;

// One installed app, read from /sdcard/apps/<id>/manifest.json. All fields are validated and
// clamped at scan time, so consumers may trust them.
typedef struct {
    char     id[32];           // directory name; [A-Za-z0-9_-] only
    char     name[40];
    char     version[16];
    char     wasm_path[160];   // absolute path to app.wasm on the SD card
    char     entry[24];        // exported entry function (default "run")
    uint32_t ram_budget;       // bytes; also the module instance heap (clamped 64 KB … 16 MB)
    uint32_t stack_kb;         // WASM operand stack, KB (manifest "stack_kb", clamped 4 … 256)
    uint32_t timeout_ms;       // run watchdog (manifest "timeout_ms", clamped 1 s … 120 s)
    uint32_t abi;              // required host ABI (manifest "abi", default 1)
    uint32_t perms;            // OR of nv_wperm_t
    uint32_t canvas_w;         // ABI v2 game canvas width  (manifest "canvas_w", 0 = not a game)
    uint32_t canvas_h;         // ABI v2 game canvas height (manifest "canvas_h")
    // ABI v7 file associations. "opens": MIME patterns ("type/sub" or "type/*", lowercase
    // [a-z0-9.+-], at most NV_WASM_OPENS_MAX), space-separated; "" = the app opens no files.
    char     opens[256];
    // "file_types": extensions the app teaches the OS (registered at boot next to its tile).
    // kind is one of "text" "image" "audio" "video" "app" "archive" "other" (kept as a string:
    // nv_wasm stays UI-agnostic; the launcher maps it to nv_file_kind_t).
    struct {
        char ext[12];          // [a-z0-9]{1,11}, no dot
        char mime[80];         // "type/sub", no wildcard (Office types are long)
        char kind[8];
    } file_types[4];
    uint8_t  n_file_types;
    // WASM-4 cart (manifest "wasm4": true, https://wasm4.org): the OS runs the fantasy console —
    // 160x160 screen upscaled, touch gamepad, update() at 60 Hz. Implies a gfx game (1024x600).
    bool     w4;
    // Terminal program (manifest "console": true): run from the Terminal with argv + stdin.
    bool     console;
    // Full-screen terminal program (manifest "tty": true, with "console"): curses (see above).
    bool     tty;
    // No Home tile (manifest "home": false): an app another one fronts (sc-im is Fogli's engine);
    // it stays reachable from the Terminal, search and Apps.
    bool     no_home;
    // ABI v9 scaled canvas (manifest "canvas_scale": "fit" | "stretch" | "zoom"): the game view
    // PPA-scales the canvas to the whole panel, straight into the display framebuffer, instead of
    // showing it 1:1 through LVGL. Lets a game render a small frame (3D: 512x300 = 2x exact) and
    // still fill the screen. One of NV_WASM_SCALE_*.
    uint8_t  canvas_scale;
    // Dependencies (manifest "requires": {"vertice": "1.0", "vxkit": "1.2"}): each is either a
    // component built into this OS (nv_wasm_sys_component) or another package from the Store
    // (installed first, under /sdcard/apps/<id>). Checked by the Store before installing and by
    // nv_wasm_exec_start before running.
    nv_wasm_dep_t deps[4];      // NV_WASM_DEPS_MAX ("requires" is a C++20 keyword)
    uint8_t  n_deps;
    // Manifest "kind": "library": a package other apps depend on (shared assets, data). Installed
    // like an app but never shown in the launcher and never run.
    bool     library;
    // Manifest "system_gestures": false — while the game is on screen the OS edge gestures (back,
    // home, shade) are off, so fast swipes in play can't leave the game; it must offer its own exit.
    bool     no_gestures;
    // ABI v14 manifest "engine": "<id>" — a game/content package without a module: it runs the
    // module of package <id> (also listed in "requires"); wasm_path then points into that package.
    // "" = the app has its own module.
    char     engine[32];
    char     category[16];   // store category ("games", "utilities"...): manifest, else <dir>/category
    // ABI v14 manifest "args": ["-iwad", "x.wad"] — argv[1..] for a WASI run, joined into one
    // command line (words with blanks are "quoted"). "" = none.
    char     args[160];
} nv_wasm_app_t;

// Whether an installed package gets a launcher icon. A library is never opened. A terminal program
// ("console") with no window of its own is a tool the Terminal runs: an OS system app
// (nv_wasm_is_system_app, installed by the OS itself) or one the store files under "terminal".
// Console games (interactive fiction, "games") and console apps of unknown category (installed
// before the store wrote categories, until its next catalog fill) keep their icon, and so does a
// full-screen program ("tty": a spreadsheet, a word processor), system app or not: it is an app
// the user opens, which then runs in the Terminal.
static inline bool nv_wasm_app_on_home(const nv_wasm_app_t *a, bool system_app) {
    if (!a || a->library || a->no_home) return false;
    if (!a->console || a->tty) return true;
    return !system_app && strcmp(a->category, "terminal") != 0;
}

enum { NV_WASM_SCALE_NONE = 0, NV_WASM_SCALE_FIT = 1, NV_WASM_SCALE_STRETCH = 2, NV_WASM_SCALE_ZOOM = 3 };
#define NV_WASM_DEPS_MAX 4
// Installed apps a scan returns (Home tiles, Settings, Terminal, dependents). With the natives they
// must fit nv_ui's registry (kMaxApps) and the launcher order string (kMaxEntries * 32 < 4000).
#define NV_WASM_MAX_APPS 84

// Components built into this OS that an app may require, and their versions:
//   "vertice" (the 3D engine, ABI v9), "wasi", "wasm4". NULL when `id` is not built in.
const char *nv_wasm_sys_component(const char *id);
// System apps: Store packages the OS itself relies on (Lua, JavaScript, SQLite — the Terminal's
// programs). Installed automatically when missing, never uninstallable, only updatable.
bool nv_wasm_is_system_app(const char *id);
// The system app ids (static storage) into *ids; returns how many.
int  nv_wasm_system_apps(const char *const **ids);
// Human name of a dependency id ("vertice" -> "Vertice"; a package id is returned as is).
const char *nv_wasm_dep_name(const char *id);
// Dotted-version compare (up to 4 numeric fields): have >= want.
bool nv_wasm_version_ge(const char *have, const char *want);
// Are all of `a`'s requirements met by this OS and the installed packages? On false, `err` gets a
// short reason: "system:<id> <ver>" (needs an OS update) or "pkg:<id> <ver>" (install / update it).
bool nv_wasm_requires_met(const nv_wasm_app_t *a, char *err, size_t n);
// Installed apps that require package `id` (for uninstall guards): writes the first one's name
// into `who` and returns the count.
int  nv_wasm_dependents(const char *id, char *who, size_t n);

#define NV_WASM_OPENS_MAX      8   // patterns kept from manifest "opens"
#define NV_WASM_FILE_TYPES_MAX 4   // entries kept from manifest "file_types" (== file_types[] size)

// True when this app is an ABI v2 game (abi>=2, "gfx" permission, and a non-zero canvas).
bool nv_wasm_app_is_game(const nv_wasm_app_t *a);

// Discover installed apps: scan /sdcard/apps/<id>/manifest.json. Fills up to `max` entries and
// returns the count (0 if none / no SD). Invalid manifests/ids are skipped with a log line.
int nv_wasm_scan(nv_wasm_app_t *out, int max);

// Load ONE app's manifest fresh from /sdcard/apps/<id>/ (validated + clamped like the scan).
// Lets callers run an app pushed after the boot scan (web console hot-reload).
bool nv_wasm_load_manifest(const char *id, nv_wasm_app_t *out);

// app.aot is native RISC-V code: it runs outside the WASM sandbox. Before an app's app.aot runs,
// the launcher hands its bytes (exactly as read, so nothing can swap the file in between) to this
// check; false = the app runs from app.wasm in the interpreter instead. nv_appstore registers the
// check (the store key's signature over the app's package.sig). With none registered no app.aot
// runs, except in developer mode (nv_config "store_unsigned"), which also lets an unsigned one run.
typedef bool (*nv_wasm_aot_verifier_t)(const char *id, const char *dir, const uint8_t *aot, size_t len);
void nv_wasm_set_aot_verifier(nv_wasm_aot_verifier_t fn);

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
//   id = start -> poll state_of(id) / drain read_run+take_toast_run -> (abort_run) -> DONE
//              -> collect_run(id) -> IDLE
//
// Every run has an id, returned by start. Whoever started a run (the Apps runner, the game view,
// the Terminal, the web hot-reload) drives it through the *_run(id) calls only: they act on that
// run and nothing else, so one owner can never abort, collect or read another owner's run. The
// two calls without an id, state() and collect(), are for the system (uninstall, the store, the
// memory broker): they see whichever run is there and clear a finished one nobody collects.
//
// All functions are safe to call from the LVGL thread; host imports write from the worker side
// under an internal lock.

typedef enum {
    NV_WRUN_IDLE = 0,   // no run; start() allowed
    NV_WRUN_RUNNING,    // worker executing (drain output, may abort)
    NV_WRUN_DONE,       // finished; result waits for collect()
} nv_wrun_state_t;

typedef uint32_t nv_wrun_id_t;
#define NV_WRUN_NONE 0u   // no run (a failed start)

// State of the engine, whoever's run it is.
nv_wrun_state_t nv_wasm_exec_state(void);
// State of run `id`: RUNNING or DONE while it is the engine's run (DONE also when the system
// collected it before its owner: collect_run still returns its result once), IDLE once collected.
nv_wrun_state_t nv_wasm_exec_state_of(nv_wrun_id_t id);
// The id of the run in the engine, NV_WRUN_NONE when idle.
nv_wrun_id_t nv_wasm_exec_current(void);

// ABI v7 launch-file grant: the file the NEXT run may read through nv.open_* (NULL / "" = none).
// Call immediately before nv_wasm_exec_start, on the same task: start consumes it (success or
// failure), and a pending grant set by another task is never handed to this task's run. The path
// must be absolute and shorter than 256 bytes (never truncated into another file's name). The
// grant ends when the run is collected, and is void once the SD card is remounted.
void nv_wasm_exec_set_launch_file(const char *path);

// Console run (Terminal): the NEXT nv_wasm_exec_start on the calling task runs the app as a
// terminal program — `args` is its command line after the program name (split on blanks, quotes
// group words), stdin is fed through nv_wasm_exec_write_stdin, output never truncates (the guest
// waits for nv_wasm_exec_read to drain it) and there is no opcode cap. Consumed by that start.
void nv_wasm_exec_set_console(const char *args);
// The shell's folder for console runs (an absolute /sdcard path): a "home" program gets it as PWD.
void nv_wasm_exec_set_console_cwd(const char *cwd);
// The Terminal's grid for the next console run of a "tty" app: COLUMNS / LINES (0 = unknown).
void nv_wasm_exec_set_console_size(int cols, int rows);

// True while the in-flight run is a console run.
bool nv_wasm_exec_is_console(void);

// Console stdin of run `id`: queue typed bytes (returns how many fit; the pipe holds 4 KB) / signal
// end of input (Ctrl-D: the guest's next read returns 0 once). No-ops unless `id` is the engine's
// run, still executing, and a console run.
size_t nv_wasm_exec_write_stdin(nv_wrun_id_t id, const char *data, size_t n);
// All n bytes or none (a control sequence must never arrive cut): false = no room right now.
bool   nv_wasm_exec_write_stdin_whole(nv_wrun_id_t id, const char *data, size_t n);
void   nv_wasm_exec_close_stdin(nv_wrun_id_t id);
// The input came from a pipe or a file and is over: EOF for good (close_stdin is the tty's one-shot ^D).
void   nv_wasm_exec_end_stdin(nv_wrun_id_t id);
// True while console run `id` is blocked reading stdin (waits for the keyboard right now).
bool   nv_wasm_exec_stdin_blocked(nv_wrun_id_t id);

// Begin an async run of app->entry. Auto-collects (discards) a stale DONE run first. Returns the
// run's id, or NV_WRUN_NONE (msg in err) when a run is still executing or the app/ABI is unusable.
nv_wrun_id_t nv_wasm_exec_start(const nv_wasm_app_t *app, char *err, size_t err_n);

// id of the app currently started (valid in RUNNING/DONE; "" when idle).
const char *nv_wasm_exec_app_id(void);

// Drain run `id`'s new nv.print output into dst (nothing when quiet, or when `id` is not the
// engine's run). Returns bytes copied.
size_t nv_wasm_exec_read_run(nv_wrun_id_t id, char *dst, size_t n);

// Pop one of run `id`'s queued nv.toast(kind,msg); false when the queue is empty or `id` is not
// the engine's run.
bool nv_wasm_exec_take_toast_run(nv_wrun_id_t id, int *kind, char *msg, size_t n);

// Ask run `id` to stop (wasm_runtime_terminate). The worker then finishes -> DONE. No-op unless
// `id` is the engine's run and RUNNING.
void nv_wasm_exec_abort_run(nv_wrun_id_t id);

// True once run `id` is executing guest code: its module is loaded and instantiated (reading and
// loading a big module - ScummVM's 8 MB app.aot, or the interpreter preparing its bytecode - takes
// seconds before that). False while loading, and once the run is DONE.
bool nv_wasm_exec_guest_live(nv_wrun_id_t id);

// True while an aborted run is still unwinding (RUNNING with an abort requested): a start refused
// with "busy" right now will succeed once that worker lands in DONE. An app opened straight out of
// another WASM app hits this — the old app's teardown aborts its run just before the new one starts,
// and a guest inside a native call (nv.http_get, up to its 10 s timeout) only stops when it returns.
bool nv_wasm_exec_stopping(void);

// When DONE: fetch the result, join + free the run, return to IDLE. Returns true exactly once
// per run; false while IDLE/RUNNING. err receives the failure text ("" on success).
// collect (no id) discards whichever run is DONE; its result is kept for the owner's collect_run.
bool nv_wasm_exec_collect(bool *ok, uint32_t *elapsed_ms, char *err, size_t err_n);
bool nv_wasm_exec_collect_run(nv_wrun_id_t id, bool *ok, uint32_t *elapsed_ms, char *err, size_t err_n);

// ---- ABI v2 game surface — UI side (call from the LVGL thread) ----------------------------------
// The runner allocates the canvas (double-buffered, PSRAM) when a game run starts; the guest draws
// into it via the nv.gfx_* imports on the worker thread. The UI shows frames like this:
//   size() to build an lv_canvas over current(); each LVGL tick, take_frame_ex() returns the next
//   ready buffer (or NULL) — set it on the canvas and invalidate. input() feeds the latest touch.
void      nv_wasm_gfx_size(int *w, int *h);
// ABI v19: size and the quarter turns the canvas is shown with (nv.gfx_set_rotation), read together.
// w x h is the canvas as the app draws it (upright); the panel shows it turned `rot` quarter turns.
void      nv_wasm_gfx_shape(int *w, int *h, int *rot);
uint16_t *nv_wasm_gfx_current(void);        // a valid buffer to seed the canvas at build time
// ABI v6: next ready frame buffer (or NULL if none pending), plus the dirty rect to re-blit (full
// canvas for legacy games). The UI blits only this region — the dirty-rect fast path.
uint16_t *nv_wasm_gfx_take_frame_ex(int *dx, int *dy, int *dw, int *dh);
// Asynchronous present for a UI that blits each taken frame synchronously (the scaled PPA path):
// set_async(true) lets the game draw the next frame while the UI shows this one; frame_done() after
// every blit (or skipped blit) of a taken frame hands its buffer back. set_async(false) to stop.
void nv_wasm_gfx_set_async(bool on);
void nv_wasm_gfx_frame_done(void);
void      nv_wasm_gfx_set_input(int x, int y, int state);   // 0 = up, 1 = down
void      nv_wasm_gfx_set_multi(const int *xs, const int *ys, int n);   // full multi-touch (canvas coords)
void      nv_wasm_gfx_request_back(void);                   // UI: forward an OS back gesture to the game
// Liveness heartbeat: bumps on EVERY gfx_present call (including the static-screen skip path) —
// present is a game's one cooperative point, so a stalled counter while RUNNING means the guest
// is stuck in a loop that will never see want_stop. The watcher then calls nv_wasm_exec_abort_run(),
// which (THREAD_MGR) forcibly interrupts the interpreter; the run lands in DONE for collection.
uint32_t  nv_wasm_gfx_present_seq(void);
// ABI v4: pending guest backlight request (0..100), or -1 if none since the last call. The UI drains
// this and applies it via LEDC on its own thread, then restores the user's brightness on teardown.
int       nv_wasm_gfx_take_backlight(void);

#ifdef __cplusplus
}
#endif
