// nv_secondscreen internals shared between the engine and its transports.
#pragma once
#include "nv_ss.h"

// engine
void ss_engine_pause_from_touch(void);   // touch task: edge swipe -> PAUSED

// USB transport (ss_usb.cpp): frame sink registered only while the app page is open.
void ss_usb_attach(void);
void ss_usb_detach(void);
void ss_usb_init(void);                  // boot: track mount edges (event bus)

// Network cast transport (ss_cast.cpp): listens from boot once any network is up.
void ss_cast_init(void);
void ss_cast_on_open(void);              // app page opened (a pending/waiting sender may start)
// Render the sender page for use from a file (USB drive / download) with the board address baked
// in. fixed=true pads the address fields so the size never depends on the IP (the USB drive's
// FAT entry is built once). Writes up to cap bytes; returns the full length.
size_t ss_cast_render_page(char *out, size_t cap, bool fixed);

// VNC transport (ss_vnc.cpp)
void ss_vnc_init(void);

// Ask SystemUI to bring the app forward (reuses the USB-display auto-open path).
void ss_request_foreground(void);

// Small helpers
uint32_t ss_now_ms(void);
