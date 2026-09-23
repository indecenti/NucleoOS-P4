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
//
// Lifecycle: nv_web_init() spawns a small task that waits for Wi-Fi, then starts the server
// once and advertises _http._tcp over mDNS (http://nucleov2.local). Never touches LVGL.
//
// SECURITY: there is NO authentication. Every endpoint — file write/delete, app run, reboot,
// OTA-relevant settings via /api/ui/* remote taps — is open to anyone on the LAN. (An nv_config
// "web_token" was once documented here but was never implemented; nothing checks it.) Keep the
// board on a trusted network until real API auth exists.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void nv_web_init(void);

#ifdef __cplusplus
}
#endif
