# NucleoOS Anima — WASM App SDK (host ABI v1)

Write apps for NucleoOS in plain C, compile to WebAssembly, drop them on the SD card. No ESP-IDF,
no reflash: the OS sandbox (WAMR) loads them at runtime with manifest-declared permissions.

## Requirements

- LLVM clang with the wasm32 backend: `winget install LLVM.LLVM`

## Quick start

```powershell
# build the demo app
.\sdk\build_app.ps1 -AppDir apps\ciao

# deploy: copy manifest.json + app.wasm to the device SD card
#   /sdcard/apps/ciao/manifest.json
#   /sdcard/apps/ciao/app.wasm
# reboot (the OS scans /sdcard/apps at boot) -> "Ciao SDK" tile on the launcher
```

## Writing an app

```c
#include "nucleo_sdk.h"

NV_EXPORT("run")
void run(void) {
    nv_printf("hello, uptime %d ms", nv_millis());
    nv_toast(NV_TOAST_OK, "done");
}
```

- Freestanding C: no libc, no malloc (v1). `memcpy/memset/strlen` and `nv_printf` come from the
  SDK runtime, linked in automatically.
- The entry point (default `run`, manifest `entry`) runs to completion; the OS enforces the
  manifest `timeout_ms` and shows a Stop button.
- Output: `nv_print`/`nv_printf` stream to the app's on-screen panel; `nv_toast` shows a system
  toast; `nv_log` writes to the OS log (tag `app:<id>`).

## Host ABI v1 (import module `nv`)

| call | permission | notes |
|---|---|---|
| `nv_print(msg)` | log | append to the output panel |
| `nv_log(level, msg)` | log | `NV_LOG_ERROR/WARN/INFO/DEBUG` |
| `nv_toast(kind, msg)` | ui | `NV_TOAST_INFO/OK/WARN/ERROR` |
| `nv_millis()` | — | ms since boot |
| `nv_time_unix()` | — | UTC epoch seconds |
| `nv_lang(buf, len)` | — | UI locale: `en it es fr de` |
| `nv_rand()` | — | hardware RNG |
| `nv_sleep_ms(ms)` | — | clamped to 1000 ms per call |

Calls without the manifest permission are dropped (warning in the OS log). Bad pointers trap the
app, never the OS.

## manifest.json

```json
{
  "id": "ciao",              // [A-Za-z0-9_-], = directory name on SD
  "name": "Ciao SDK",        // launcher label
  "version": "1.0",
  "entry": "run",            // exported entry function
  "abi": 1,                  // host ABI required (OS refuses newer-than-OS)
  "ram_budget": 262144,      // instance heap, bytes (64 KB - 8 MB)
  "stack_kb": 16,            // WASM operand stack (4 - 256)
  "timeout_ms": 5000,        // run watchdog (1 s - 120 s)
  "permissions": ["log", "ui"]
}
```

## Toolchain notes

`build_app.ps1` compiles every `.c` in the app dir + `sdk/src/nucleo_sdk.c` with
`--target=wasm32 -mcpu=mvp -O2 -ffreestanding -nostdlib` and links with
`--no-entry --export=<entry> -z stack-size=8192 --initial-memory=65536 --strip-all`.
`-mcpu=mvp` matters: the on-device WAMR interpreter is built without post-MVP extensions
(bulk-memory, etc.); default clang settings would emit opcodes it rejects. Module cap: 2 MB.
