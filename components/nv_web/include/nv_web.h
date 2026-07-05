// nv_web — NucleoOS Anima web console (dev + remote admin over Wi-Fi/Ethernet).
//
// A small esp_http_server serving:
//   GET  /                    embedded single-page console (Files / Run app / Logs / Info)
//   GET  /api/info            device info JSON (name, version, ip, heap, uptime)
//   GET  /api/fs?path=        directory listing JSON [{n,s,d}]
//   GET  /api/fs/dl?path=     file download (chunked)
//   POST /api/fs/up?path=     file upload (raw body -> file; parent dirs auto-created)
//   POST /api/fs/del?path=    delete file / empty dir
//   POST /api/app/run?id=     run an installed WASM app via the async engine, return its
//                             output text (fresh manifest read -> hot-reload dev loop:
//                             build on PC -> upload -> run, no reboot, no reflash)
//   GET  /api/logs            nv_log ring snapshot (text)
//
// Lifecycle: nv_web_init() spawns a small task that waits for Wi-Fi, then starts the server
// once and advertises _http._tcp over mDNS (http://nucleov2.local). Like KeyDeck it is
// LAN-open by default; set nv_config "web_token" to require ?k=<token> (or X-Token header)
// on every request. Never touches LVGL.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void nv_web_init(void);

#ifdef __cplusplus
}
#endif
