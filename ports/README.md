# ports — existing software as NucleoOS terminal programs

WASI builds of established command-line programs, run by the Terminal app (console WASM apps,
host ABI 8) and distributed through the Store (category "Terminal").

| id        | program                 | license        | notes                                          |
|-----------|-------------------------|----------------|------------------------------------------------|
| `lua`     | Lua 5.4.9               | MIT            | REPL + scripts; errors unwind via nv_try_call  |
| `js`      | QuickJS-ng 0.17.0       | MIT            | own line REPL (`qjs/nv_js.c`), std/os, timers  |
| `sqlite3` | SQLite 3.53.4 shell     | public domain  | no WAL/mmap/threads/extensions/FTS5 (AOT < 4 MB) |

All three see `/sdcard/home` as `/` (manifest permission `home`) and get up to 8 MB of linear
memory.

## Build

```bash
bash ports/build.sh              # all; or: bash ports/build.sh lua
bash ports/test.sh               # PC run (WSL): interpreted + x86_64 AOT, scripted stdin
```

`fetch.sh` downloads the pinned upstream tarballs (sha256-checked) into `_src/` (not committed).
`build.sh` compiles with the system clang + the wasi-sdk sysroot (like `sdk/build_app.ps1 -Wasi`),
writes `apps/<id>/app.wasm`, the riscv32 `app.aot` (wamrc in WSL, same flags as the SDK) and
`icon.z` (`make_icon.py`). The manifests in `apps/<id>/` are hand-written.

## What the ports change

- **Lua** — no setjmp/longjmp under WAMR (wasi-sdk implements them with the exception-handling
  proposal). `lua/nv_lua_port.h` (force-included) redefines `LUAI_TRY`/`LUAI_THROW` to the host
  imports `nv.try_call`/`nv.throw` (`common/nv_sjlj.h`): the protected call runs through the host,
  and an error is a trap the innermost try_call catches and clears. `lua/shim/setjmp.h` shadows the
  wasi-libc header; `os.execute`/`io.popen`/`os.tmpname`/`io.tmpfile` report "not supported".
- **QuickJS-ng** — upstream `qjs` drives a raw tty through `os.setReadHandler` + `poll()`, which
  the device's WASI layer does not implement, so `qjs/nv_js.c` is a small line-based REPL
  (bracket-balanced continuation, a result inspector, timers run before the next prompt).
- **SQLite** — the amalgamation shell, built without WAL, mmap, threads, `system()`/`popen` and
  loadable extensions.

## PC host

`host/nvhost.c` (built by `host/build.sh` in WSL) is WAMR with the firmware's feature set —
fast-interp + AOT **without hardware bound checks** (the P4 has none; with them, x86 AOT turns an
exception check into a signal + longjmp that skips the host's try_call frames) — plus the `nv`
imports above. `NVHOST_DEBUG=1` traces try_call/throw.
