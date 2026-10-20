// nv_dmd — the receiver of the DMD app: the board as an external pinball DMD for a PC running dmdext
// (VPinMAME / VPX), over either link, both live at once:
//   * Wi-Fi: a WebSocket server on its own port speaks dmdext's "networkstream" output
//     (DmdDevice.ini: [networkstream] enabled = true, url = ws://<board>:<port>/dmd, retry = true).
//     One client at a time: a new connection replaces the old one (a PC reconnecting after a restart
//     must never find the board busy).
//   * USB: the board answers as a ZeDMD (128x32) on its USB-Serial-JTAG port (the programming port,
//     303A:1001), which dmdext's [zedmd] output finds by itself. While a PC streams there, serial logs
//     are muted (they would land inside the protocol); the log ring keeps recording.
// The newest frame is kept as one 0x00RRGGBB value per dot. Parsing is in dmd_core.c (host-tested and
// fuzzed). The tasks run on PSRAM stacks on core 0 and never touch flash or LVGL.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV_DMD_PORT_DEFAULT 9090

typedef struct {
    bool        listening;      // the port is open
    int         port;
    bool        client;         // a PC is connected over Wi-Fi
    char        peer[16];       // its IPv4 address
    bool        usb;            // a PC drives the board as a ZeDMD over USB
    bool        usb_ready;      // the USB port is ours (driver installed)
    bool        last_usb;       // the newest frame came over USB
    bool        bridge;         // the PinUP Popper bridge talks on the USB port (no table running)
    int64_t     last_bridge_us; // its last message (it pings every 2 s)
    int         w, h;           // dimensions of the newest frame (0 before the first one)
    char        game[48];       // ROM name sent by dmdext ("" if none)
    const char *fmt;            // gray2 / gray4 / coloredGray2 / coloredGray4 / rgb24 / ZeDMD / "-"
    uint32_t    frames;         // frames received since start (duplicates included)
    uint32_t    fps10;          // frames per second x10 over the last second
    int64_t     last_frame_us;  // esp_timer time of the newest frame, 0 = none yet
    int64_t     last_pc_us;     // ... of the newest frame from a PC (not nv_dmd_show), 0 = none yet
    bool        blank;          // the newest frame is all black (dmdext clearing the panel at a table's start)
    uint32_t    bad;            // messages or connections dropped as malformed
    uint32_t    zedmd_resets;   // USB transfers the PC gave up on half-way (no ack in time), since start
} nv_dmd_status_t;

// Start both receivers, Wi-Fi on `port` (allocates ~0.6 MB of PSRAM). True if they are running (already
// running on the same port counts; already running on another port moves the Wi-Fi server only, as
// nv_dmd_set_port()).
bool nv_dmd_start(int port);
// Move the Wi-Fi server to another port while running: only its task restarts (the Wi-Fi PC is dropped and
// reconnects with retry = true); USB, the newest frame and the buffers the display holds stay. False when
// not running, or when the old task did not stop within 2 s (it then keeps the old port).
bool nv_dmd_set_port(int port);
// Stop them, drop the clients, give the USB port back to the console, free the buffers. Waits for the
// tasks (normally < 0.2 s), 2 s each at most: a task still running then keeps its buffers (logged), and the
// next nv_dmd_stop() / nv_dmd_start() waits for it again.
void nv_dmd_stop(void);
// Monochrome colour for gray frames: -1 = the PC's (dmdext "color"), else 0x00RRGGBB.
void nv_dmd_set_color(int32_t override_rgb);

// Wait up to `ms` for a new frame. True when one arrived.
bool nv_dmd_wait(uint32_t ms);
// Generation of the newest frame (0 = none). Changes only when the picture changes.
uint32_t nv_dmd_latest(void);
// The newest frame (w*h values, 0x00RRGGBB, row-major), copied for the display: it stays valid until the
// next nv_dmd_acquire() or nv_dmd_stop(). NULL if none. One caller (the display); pair it with
// nv_dmd_release() (kept for the API's symmetry: the receivers never wait on the display).
const uint32_t *nv_dmd_acquire(int *w, int *h, uint32_t *gen);
void nv_dmd_release(void);
// The newest frame copied into dst (cap values) under the lock, for anyone but the display (a snapshot): it
// never touches the display's copy. False when there is none or it is over cap.
bool nv_dmd_copy(uint32_t *dst, size_t cap, int *w, int *h);

void nv_dmd_get_status(nv_dmd_status_t *out);

// Show a picture made on the board (the idle clock, the test screen) as the newest frame: w*h values,
// 0x00RRGGBB, at most DMD_MAX_PX (dmd_core.h). `what` names it on the status page. It does not count as a
// PC frame (last_pc_us, fps). Any task; false when the receiver is not running.
bool nv_dmd_show(const uint32_t *px, int w, int h, const char *what);

// ---- the PinUP Popper bridge (tools/popper_bridge) on the USB port, while no table runs
// Take the next message from the bridge: type ('H' hello, 'L' list, 'T' table, 'I' image, 'G' game, 'R'
// launch result, 'E' error) and its payload (JSON, NUL-terminated; for 'I': u16 LE json_len, JSON, RGB565
// pixels). False when none is waiting. Free the payload with nv_dmd_bridge_free(). Any task.
bool nv_dmd_bridge_take(uint8_t *type, uint8_t **data, uint32_t *len);
void nv_dmd_bridge_free(uint8_t *data);
// Send a request to the bridge (type 'Q', JSON up to 900 bytes). False when the USB link is not running.
bool nv_dmd_bridge_send(uint8_t type, const char *json);

#ifdef __cplusplus
}
#endif
