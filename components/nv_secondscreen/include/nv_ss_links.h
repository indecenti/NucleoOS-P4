// nv_ss_links — status + actions of the Second Screen transports, for the connection wizard.
// Everything here is safe to poll from the LVGL thread (short internal locks, no I/O) unless
// noted. Transports run on their own tasks; see nv_ss.h for the display engine.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Boot-time init (event subscriptions + network listener). Idempotent.
void nv_ss_init(void);

// ------------------------------------------------------------------------------ USB
typedef struct {
    bool     device_mode;        // OTG-HS runs as a USB device (PC link) this boot
    bool     device_mode_saved;  // what the NEXT boot will use (pending reboot when != device_mode)
    bool     mounted;            // a host configured us (cable + powered PC)
    uint32_t mounted_for_ms;     // since the last mount edge (0 when unplugged)
    uint32_t frames;             // desktop frames received since boot
    uint32_t last_frame_ago_ms;  // UINT32_MAX = never
} nv_ss_usb_info_t;
void nv_ss_usb_info(nv_ss_usb_info_t *out);
// Persist the OTG-HS personality (true = PC second screen, false = USB speaker/keyboard host).
// Applies on the next boot. Writes NVS: call from the LVGL thread.
void nv_ss_usb_set_device_mode(bool device);

// ------------------------------------------------------------------------------ Cast
// NucleoCast: a browser page (or helper) on the PC captures a screen/window and streams JPEG
// tiles over a WebSocket. Served by the board on port 7070 (http) and 7443 (https).
#define NV_SS_CAST_PORT       7070
#define NV_SS_CAST_TLS_PORT   7443
typedef struct {
    bool     net_up;             // Wi-Fi or Ethernet has an IP
    char     ip[16];             // preferred address to show (Ethernet first)
    char     iface[8];           // "wifi" / "eth" / ""
    bool     listening;          // http listener up
    bool     tls_ready;          // https listener up (certificate ready)
    bool     client;             // a sender is connected (any state)
    char     client_label[48];   // "192.168.0.216 · Chrome · Windows"
    bool     pending;            // a new sender waits for the user's approval
    uint32_t frames;             // frames received this session
} nv_ss_cast_info_t;
void nv_ss_cast_info(nv_ss_cast_info_t *out);
// Answer the pending approval. remember=true trusts that browser/device from now on (NVS write:
// call from the LVGL thread).
void nv_ss_cast_answer(bool allow, bool remember);
void nv_ss_cast_forget_all(void);
bool nv_ss_cast_ask_enabled(void);           // "ask before a new device connects" (default on)
void nv_ss_cast_set_ask(bool on);

// ------------------------------------------------------------------------------ VNC
#define NV_SS_VNC_LISTEN_PORT 5500
typedef struct {
    char     name[48];           // mDNS instance ("MacBook di Nicola")
    char     host[40];           // IPv4 address
    uint16_t port;
} nv_ss_vnc_server_t;

typedef enum {
    NV_SS_VNC_OFF = 0,
    NV_SS_VNC_CONNECTING,        // TCP connect / handshake
    NV_SS_VNC_RUNNING,           // desktop on screen (or paused)
    NV_SS_VNC_FAILED,            // see error
} nv_ss_vnc_state_t;

typedef enum {
    NV_SS_VNC_ERR_NONE = 0,
    NV_SS_VNC_ERR_CONNECT,       // host unreachable / refused (server off, firewall)
    NV_SS_VNC_ERR_PROTOCOL,      // not an RFB server / unsupported version
    NV_SS_VNC_ERR_SECURITY,      // server offers no security type we support (e.g. only TLS/Apple)
    NV_SS_VNC_ERR_AUTH,          // wrong password
    NV_SS_VNC_ERR_NEED_PASSWORD, // server wants a password and none was given
    NV_SS_VNC_ERR_CLOSED,        // server closed the link
    NV_SS_VNC_ERR_MEMORY,
} nv_ss_vnc_err_t;

typedef struct {
    nv_ss_vnc_state_t state;
    nv_ss_vnc_err_t   err;
    char     detail[64];         // server-provided reason or short diagnostic
    char     desktop[48];        // desktop name reported by the server
    char     host[40];
    int      fb_w, fb_h;         // remote framebuffer size
    const char *encoding;        // last encoding seen ("Tight", "ZRLE", ...)
    bool     listening;          // reverse-connection listener (port 5500) is up
} nv_ss_vnc_info_t;

void nv_ss_vnc_info(nv_ss_vnc_info_t *out);
// Start a connection (async). password may be NULL/"". `user` is only used by Apple's own
// authentication (macOS Screen Sharing without the separate VNC password: Mac account + password).
// Fails if one is already running.
bool nv_ss_vnc_connect(const char *host, uint16_t port, const char *user, const char *password);
void nv_ss_vnc_disconnect(void);
// mDNS browse for _rfb._tcp (macOS Screen Sharing, some Linux servers). Async; results via
// nv_ss_vnc_discovered(). Returns the number copied.
void nv_ss_vnc_discover(void);
int  nv_ss_vnc_discovered(nv_ss_vnc_server_t *out, int max);
bool nv_ss_vnc_discovering(void);
// Reverse connections: droidVNC-NG / TightVNC / x11vnc "connect to viewer" reach the board on
// port 5500. On while the app is open.
void nv_ss_vnc_set_listen(bool on);

#ifdef __cplusplus
}
#endif
