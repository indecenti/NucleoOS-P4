// nv_wasm_wasi — WASI preview1 support for WAMR on NucleoOS (private to nv_wasm).
//
// WAMR's libc-wasi only runs its filesystem on ESP-IDF when the storage is LittleFS: its
// esp-idf platform layer turns openat/fstatat/mkdirat/unlinkat/renameat/fdopendir into ENOSYS
// stubs and opens preopened directories with open(O_DIRECTORY), which FATFS cannot do. Our SD
// is FATFS, so this module supplies the missing pieces without patching the vendored runtime:
//   * a tiny VFS at "/wasi" that hands out directory descriptors (plus the stdio sinks), and
//   * linker --wrap replacements for the *at() calls, fdopendir/closedir and nanosleep (the WAMR
//     stub of nanosleep is ENOSYS, which would turn every WASI sleep into a busy loop).
// A guest only ever sees its own folder: the "fs" permission preopens "/" as
// /sdcard/apps/<id>/data, and without it the guest gets no preopen at all.
#pragma once

#include "sdkconfig.h"

#if CONFIG_WAMR_ENABLE_LIBC_WASI

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wasm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

// Receives what the guest writes to stdout (stream 1) / stderr (stream 2). Runs on the worker.
typedef void (*nv_wasi_sink_fn)(void *ctx, int stream, const char *data, size_t len);

// Per-run WASI state. Must stay alive from nv_wasi_prepare() until after the instance is
// deinstantiated: WAMR keeps pointers to the argv/env/map strings until instantiation.
typedef struct {
    bool        active;
    int         fd_in, fd_out, fd_err;
    char        argv0[48];
    char        env0[64];
    char        map0[112];
    char       *argv[1];
    const char *env[1];
    const char *map[1];
} nv_wasi_run_t;

// Registers the /wasi VFS. Idempotent; call once from nv_wasm_init().
bool nv_wasi_init(void);

// True if the module imports anything from "wasi_snapshot_preview1".
bool nv_wasi_module_uses_wasi(wasm_module_t module);

// Sets the module's WASI args (stdio -> sink, argv = app id, preopen "/" -> the app's data folder
// when allow_fs). Call on the worker thread, after wasm_runtime_load and before instantiate.
bool nv_wasi_prepare(nv_wasi_run_t *st, wasm_module_t module, const char *app_id, bool allow_fs,
                     nv_wasi_sink_fn sink, void *sink_ctx, char *err, size_t err_n);

// Releases the run's stdio descriptors. Call after wasm_runtime_deinstantiate.
void nv_wasi_finish(nv_wasi_run_t *st);

// Wakes a guest sleeping in WASI poll_oneoff/nanosleep so an abort lands promptly.
void nv_wasi_abort(void);

// After a failed call: true if the trap was the guest calling proc_exit (exit()). *code gets the
// exit status; exit(0) is a normal end of a WASI command, not an error.
bool nv_wasi_exited(wasm_module_inst_t inst, uint32_t *code);

#ifdef __cplusplus
}
#endif

#endif  // CONFIG_WAMR_ENABLE_LIBC_WASI
