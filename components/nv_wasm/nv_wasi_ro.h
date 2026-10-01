// nv_wasi_ro — the read-only rules of the WASI "/package" preopen (pure, host-testable).
//
// "wasi" 1.3: a package that runs another package's module (ABI v14 "engine", e.g. a Lua app on the
// luaapp engine) sees its own package folder /sdcard/apps/<id> as "/package", READ-ONLY: the code
// and assets the store installed and verified (package.sig) can be read, never changed by the app.
// nv_wasm_wasi.c marks the preopened directory descriptor read-only (nv_wasi_is_pkg_root), every
// directory opened beneath it inherits the mark, and any create / write / truncate / mkdir /
// unlink / rename through a read-only descriptor fails with EROFS (nv_wasi_open_writes).
#pragma once

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define NV_WASI_PKG_ROOT "/sdcard/apps/"

// True for exactly "/sdcard/apps/<id>" (no trailing '/', no deeper component), <id> a valid app id
// (1..32 of [A-Za-z0-9_-]). That is the only host path the "/package" preopen ever names.
static inline bool nv_wasi_is_pkg_root(const char *p) {
    const size_t rl = sizeof(NV_WASI_PKG_ROOT) - 1;
    if (!p || strncmp(p, NV_WASI_PKG_ROOT, rl) != 0) return false;
    const char *id = p + rl;
    size_t n = 0;
    for (; id[n]; n++) {
        const char c = id[n];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok) return false;
    }
    return n >= 1 && n <= 32;
}

// True when an open() with these flags could change the file system (write access, create,
// truncate, append). A read-only descriptor only allows plain O_RDONLY opens.
static inline bool nv_wasi_open_writes(int flags) {
    return (flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND)) != 0;
}
