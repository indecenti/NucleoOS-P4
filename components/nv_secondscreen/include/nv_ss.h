// nv_ss — Second Screen engine: the board as a display for another device.
//
// One engine, several transports ("sources"), one owner at a time:
//   USB   — Windows IDD driver over the OTG-HS vendor endpoint (nv_usb), full JPEG frames.
//   CAST  — NucleoCast over the network (WebSocket, JPEG tiles from a browser page or helper).
//   VNC   — the board as an RFB viewer of a VNC server (macOS, Linux, Windows, Android).
//
// While a source is LIVE, LVGL is stopped (lvgl_port_stop) and the source owns the DSI
// framebuffer: JPEGs go through the P4 hardware decoder + PPA straight into it, VNC writes
// decoded rectangles into it directly. The GT911 is then read by the engine's touch task and
// fingers are handed to the source (HID back to the PC, WebSocket to the sender, RFB pointer
// events). A swipe that starts on the left edge strip pauses the session and gives the panel
// back to the app UI; Resume re-takes it and restores the last picture.
//
// Threading / locks: the app page calls nv_ss_open/close/resume/disconnect on the LVGL thread.
// Sources call begin/end/present from their own tasks. Lock order is LVGL port lock -> engine
// lock, never the reverse: no engine call takes the LVGL lock while holding the engine lock.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV_SS_PANEL_W 1024
#define NV_SS_PANEL_H 600
#define NV_SS_TOUCH_MAX 5

typedef enum {
    NV_SS_SRC_NONE = 0,
    NV_SS_SRC_USB,    // USB cable, Windows IDD driver (extended desktop)
    NV_SS_SRC_CAST,   // NucleoCast over the network (browser / helper)
    NV_SS_SRC_VNC,    // VNC viewer (board connects to, or is reached by, a VNC server)
} nv_ss_src_t;

typedef enum {
    NV_SS_IDLE = 0,   // no session; the app UI (wizard) is on screen
    NV_SS_LIVE,       // a source owns the panel
    NV_SS_PAUSED,     // session kept, panel handed back to the app (edge swipe)
} nv_ss_mode_t;

typedef struct {
    uint8_t id;       // stable per finger for the duration of the contact
    int16_t x, y;     // PANEL coordinates (0..1023, 0..599)
} nv_ss_touch_pt_t;

// Callbacks a source registers with nv_ss_begin(). All optional. They run on engine tasks
// (touch task / control task) and must not block for long or take the LVGL lock.
typedef struct {
    void (*touch)(const nv_ss_touch_pt_t *pts, int cnt, void *user);  // cnt==0: all released
    void (*pause)(void *user);    // user paused (edge swipe): stop sending if you can
    void (*resume)(void *user);   // user resumed: send a full picture again
    // Session over from the engine side. by_user=true: the user tapped Disconnect (drop the link);
    // false: the app page closed (a network source may keep its link and resume later). Async ok.
    void (*stop)(bool by_user, void *user);
    bool (*alive)(void *user);    // polled ~50 Hz while LIVE; false ends the session
    void *user;
} nv_ss_source_ops_t;

// ---- app page lifecycle (LVGL thread) ------------------------------------------------
bool nv_ss_open(void);        // allocate decode buffers, accept sessions. false = no memory/HW
void nv_ss_close(void);       // end any session (ops.stop), free buffers
bool nv_ss_is_open(void);
void nv_ss_resume(void);      // PAUSED -> LIVE (async: takes the panel from a worker task)
void nv_ss_disconnect(void);  // ask the current source to stop (ops.stop), session ends

// ---- source side (any task) ----------------------------------------------------------
// Claim the display. Fails when the engine is closed or another source owns a session.
// `peer` is a short human label ("PC (USB)", "192.168.0.216 · Chrome", "MacBook").
bool nv_ss_begin(nv_ss_src_t src, const nv_ss_source_ops_t *ops, const char *peer);
// Release (link lost / finished). Safe to call when not the owner (no-op).
void nv_ss_end(nv_ss_src_t src, const char *reason);
bool nv_ss_owner(nv_ss_src_t src);      // src holds the session (LIVE or PAUSED)
bool nv_ss_is_live(nv_ss_src_t src);    // src holds the session and the panel

// Decode a baseline JPEG with the hardware decoder and place it at (dx,dy). dw/dh = 0 keeps the
// native size; otherwise the picture is scaled (PPA, k/16 steps) into dw x dh. `dma_buf` = the
// JPEG already sits in memory from jpeg_alloc_decoder_mem(INPUT) — else it is copied first.
// Returns false (frame dropped) when src is not LIVE or the JPEG can't be decoded.
bool nv_ss_present_jpeg(nv_ss_src_t src, const uint8_t *jpg, uint32_t len, bool dma_buf,
                        int dx, int dy, int dw, int dh);

// Decode a JPEG into the engine's scratch buffer (RGB565) without drawing it — for sources that
// composite themselves (VNC Tight JPEG rectangles). *stride is in pixels. The pixels stay valid
// until the next decode/present by the same source. Only while src is LIVE.
bool nv_ss_decode_jpeg(nv_ss_src_t src, const uint8_t *jpg, uint32_t len,
                       const uint16_t **px, int *w, int *h, int *stride);

// Direct framebuffer access (RGB565, NV_SS_PANEL_W x NV_SS_PANEL_H, row stride = panel width).
// On success the engine lock is HELD until nv_ss_fb_end(): keep it short (one update batch).
bool nv_ss_fb_begin(nv_ss_src_t src, uint16_t **fb);
// Write back the CPU cache for the touched area (union of dirty rects) and release the lock.
void nv_ss_fb_end(int x, int y, int w, int h);
// Clear the whole panel to black (inside a session). Takes the lock itself.
void nv_ss_fb_clear(nv_ss_src_t src);

// Account for input bytes/updates (stats line) when a source does not go through present.
void nv_ss_stat_update(uint32_t bytes);

// ---- status (UI polling) -------------------------------------------------------------
typedef struct {
    nv_ss_mode_t mode;
    nv_ss_src_t  src;          // current owner (NONE when idle)
    char         peer[48];
    float        fps;          // updates presented per second (rolling)
    float        dec_ms;       // EMA of hardware JPEG decode time
    float        kbps;         // input bandwidth, kB/s
    uint32_t     updates;      // total presented updates this session
    uint32_t     session_s;    // seconds since the session began
    nv_ss_src_t  last_src;     // last ended session (for "why did it stop")
    char         last_reason[48];
    uint32_t     generation;   // bumps on every mode/owner change (cheap UI refresh test)
} nv_ss_status_t;
void nv_ss_get_status(nv_ss_status_t *st);

const char *nv_ss_src_name(nv_ss_src_t src);

#ifdef __cplusplus
}
#endif
