// nv_wasm_net — ABI v12 network imports for WASM apps (component-private).
#pragma once
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

#ifdef __cplusplus
}
#endif
