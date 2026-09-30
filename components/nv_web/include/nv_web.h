// nv_web — NucleoOS Anima web console (dev + remote admin over Wi-Fi/Ethernet).
//
// A small esp_http_server serving the web OS shell (docroot /sdcard/web, cached in PSRAM) and a
// REST API; the full list is the routes[] table in server_start(). Main dev endpoints:
//   GET  /api/info              device info JSON (name, version, ip, heap, uptime)
//   GET  /api/fs/list?path=     directory listing JSON
//   GET  /api/fs/read?path=     file download
//   POST /api/fs/write?path=    file upload (raw body -> file; parent dirs auto-created)
//   POST /api/fs/delete?path=   delete file / empty dir
//   POST /api/app/run?id=       run an installed WASM app via the async engine, return its
//                               output text (fresh manifest read -> hot-reload dev loop:
//                               build on PC -> upload -> run, no reboot, no reflash)
//   GET  /api/logs              nv_log ring snapshot (text)
//   GET  /api/pads              connected game controllers (USB HID / XInput / BLE): name,
//                               source, vid/pid, mapped, battery, live buttons + 6 axes
//   GET  /api/bt                Bluetooth LE state, paired pads, current scan results
//   POST /api/bt?action=        on|off|scan|stop|connect|forget (+ addr=aa:bb:..[&type=0|1]);
//                               same keys also accepted as a JSON body. Async: poll GET /api/bt
//
// Lifecycle: nv_web_init() spawns a small task that waits for Wi-Fi, then starts the server
// once and advertises _http._tcp over mDNS (http://nucleov2.local). Never touches LVGL.
//
// SECURITY: every route except /api/info, /api/auth/status and /api/pair (and the static web files)
// needs a paired session (nv_auth): the HttpOnly nv_s cookie for browsers, "Authorization: Bearer
// <token>" for tools. Pairing: GET /api/auth/status from an unpaired client puts a 6-digit code on
// the device screen (and the USB serial console); POST /api/pair {"pin":...} returns the session.
// /ws is checked before the WebSocket upgrade. PC tools pair once with `python tools/pair.py`.
// Transport is plain HTTP on the LAN: the token is only as private as the Wi-Fi.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void nv_web_init(void);

#ifdef __cplusplus
}
#endif
