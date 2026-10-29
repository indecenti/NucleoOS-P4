// nv_wasm_net — ABI v12 network imports for WASM apps (component-private).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register the "nv" network natives (http_req, ws_*, mqtt_*, ha_*). Called from nv_wasm_init.
void nv_wasm_net_register(void);
// Close every handle, subscription and queued message of the finished run (nv_wasm_exec_collect).
// Returns at once; an HTTP request still in flight is freed by the worker when it ends.
void nv_wasm_net_cleanup(void);

// nv_wasm.cpp: the permissions (NV_WPERM_*) of the run executing `env`, 0 when unknown.
uint32_t nv_wasm_env_perms(wasm_exec_env_t env);
// nv_wasm.cpp: the app id of the run executing `env`, "" when unknown (a demo run).
const char *nv_wasm_env_app_id(wasm_exec_env_t env);

// ABI v18: the system downloader's natives (dl_*, nv_wasm_dl.cpp). Called from nv_wasm_init.
void nv_wasm_dl_register(void);
// ABI v19: the hardware JPEG decoder (jpeg_info, jpeg_decode; nv_wasm_jpeg.cpp). Called from nv_wasm_init.
void nv_wasm_jpeg_register(void);
// ABI v20: the VoxelSpaceNG terrain engine (vs_*; nv_wasm_vs.cpp). Called from nv_wasm_init.
void nv_wasm_vs_register(void);
// nv_wasm.cpp, for ABI v20: the canvas buffer the next frame goes into (wholly dirty), its upright
// size and quarter turn; nullptr when no canvas is open.
uint16_t *nv_wasm_gfx_frame_target(int *w, int *h, int *rot);
// nv_wasm.cpp: path of the running app's asset `name` (own folder, or "lib:name" of a declared
// package) under <sub>/ with extension ext; false for a name that is not acceptable.
bool nv_wasm_app_asset(const char *name, const char *sub, const char *ext, char *out, size_t n);

// The ABI v12 destination policy for an http(s) URL: public hosts need "net", private ones "lan".
// For the older nv.http_get, which predates the net/lan split. Resolves the host (blocking).
bool nv_wasm_net_http_url_ok(const char *url, uint32_t perms);

#ifdef __cplusplus
}
#endif
