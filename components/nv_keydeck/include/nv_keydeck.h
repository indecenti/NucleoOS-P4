// nv_keydeck — remote keyboard + telemetry service for NucleoOS Anima.
//
// A companion device (the M5Stack Cardputer running NucleoOS's "KeyDeck" native app)
// connects over Wi-Fi and acts as a physical keyboard for this OS: every key typed on
// the Cardputer lands in the focused textarea through the IME injection API
// (nv_ime_inject_text / nv_ime_inject_key). In return, the service streams live board
// telemetry (free PSRAM, per-core CPU load, free SRAM, uptime) so the Cardputer can
// render a small system monitor on its 240x135 screen.
//
// ── KeyDeck wire protocol v1 (shared contract with G:\Nucleo docs/keydeck.md) ────────
// Transport: plain TCP, port 5588, one client at a time (a new connection replaces the
// old one — "latest wins"). Lines are UTF-8, LF-terminated (CR ignored), max 160 bytes.
// Discovery: this service advertises mDNS `_keydeck._tcp` (hostname "nucleov2") so the
// client can find the board without typing an IP.
//
//   client → server                      server → client
//   ---------------                      ---------------
//   HELLO v1 <client-name>               WELCOME v1 <os> <host>
//   TXT <text…to end of line>            STAT ps_free=<B> ps_total=<B> sram_free=<B>
//   KEY <NAME> [mods]                         cpu0=<pct> cpu1=<pct> up=<s>   (every ~1s)
//   PING                                 PONG
//                                        ERR <token>   (e.g. "nofocus": no field focused)
//
//   KEY names: ENTER ESC BACKSPACE DELETE TAB LEFT RIGHT UP DOWN. `mods` is an optional
//   decimal bitmask (1=shift 2=ctrl 4=alt 8=gui) — reserved, ignored by v1.
//   cpu0/cpu1 are -1 when CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is off.
//
// Keepalive: the client PINGs every ~2 s when idle; the server drops a client silent for
// >8 s. No authentication in v1 — LAN-only, same trust domain as the OTA updater.
//
// Threading: the TCP task never touches LVGL directly — every injection/toast happens
// inside lvgl_port_lock(). Telemetry reads (heap counters, CPU load) are lock-free.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Spawn the KeyDeck server task. It idles until Wi-Fi is connected, then listens on
// TCP 5588 and advertises _keydeck._tcp over mDNS. Call once, after nv_ui_start()
// (the IME must exist before keys can be injected). Safe no-op on second call.
void nv_keydeck_init(void);

// True while a KeyDeck client is connected (for a future status-bar glyph / settings row).
bool nv_keydeck_client_connected(void);

#ifdef __cplusplus
}
#endif
