# MicroPython for NucleoOS (`apps/python`)

Python 3 as MicroPython 1.26.1 (MIT), a WASI console program run by the Terminal (host ABI 8):
`python`, `python file.py [args]`, `python -c code`, `python -m module`. `/sdcard/home` is `/`;
`sys.path` is `['', '/lib', '/py']` (the script's folder first).

```bash
bash ports/micropython/build.sh        # apps/python/{app.wasm,icon.z} (Linux or WSL)
bash ports/micropython/build.sh test   # upstream test suite, native + wasm under ports/host (WAMR)
```

Everything is pinned and fetched into `ports/_src`: the MicroPython release tarball (sha256),
wasi-sdk 27 and Binaryen 123. `micropython.patch` is applied to the tarball once.

## Why a patch: no setjmp

MicroPython raises exceptions with `nlr_push`/`nlr_jump`, a setjmp/longjmp pair. wasi-sdk
implements setjmp with the WebAssembly exception-handling proposal, which the device runtime
(WAMR fast-interp + AOT) does not execute. Lua had the same problem and uses the host's
protected call (`ports/common/nv_sjlj.h`); MicroPython uses it too:

- `MICROPY_NLR_NV` (patched `py/nlr.h`): no `nlr_push()`. A protected region is a callback of
  `nlr_nv_try(buf, fn, ud)` (`nlr_nv.c`), which runs it through `nv_try_call`; `nlr_jump()` is
  `nv_throw()`. A jump aimed at an outer buffer is re-thrown until it reaches that buffer's call.
- The core's protected regions become callbacks: `objgetitemiter.c`, `runtime.c` (2),
  `parse.c`, `runtime_utils.c` (2), `extmod/vfs.c` (2), and the bytecode VM: `vm.c`'s dispatch
  loop runs as `nv_vm_body()` and reports how it ended (return, yield, local raise, stackless
  call/return) in a context struct, so the exception handler stays in `mp_execute_bytecode`.
  Each changed site keeps the upstream code under `#else`.
- `MICROPY_STACKLESS` + pystack: Python-to-Python calls neither nest C frames nor host calls,
  so recursion is bounded by the 64 KB pystack (~1000 levels, like CPython, then `RuntimeError`).

## Garbage collection

MicroPython's collector scans the C stack for object pointers, but WebAssembly keeps most values
in locals and on the operand stack, which memory cannot see. The build runs Binaryen on the
linked module: `--flatten` moves every intermediate value into a local, `--spill-pointers` copies
every local live across a call to the C shadow stack, which `gc_collect()` scans. Cost measured
under WAMR: about +18% time, +86 KB. `build.sh test` proves it with a GC-stress variant
(`NV_GC_STRESS=4`: a collection every 4 allocated blocks): without the pass it crashes, with it
the suite passes. The heap starts at 1 MB and grows in chunks up to the app's 8 MB budget
(`MICROPY_GC_SPLIT_HEAP_AUTO`).

## Test results (`build.sh test`, 757 upstream tests from basics, micropython, float, misc, import, extmod, stress)

- Native (setjmp-emulated host, `native/`, no frozen modules): 757 of 760 pass. `memoryview_slice_size` expects 32-bit
  sizes; `extreme_exc` fills an unbounded PC heap.
- wasm under WAMR with GC stress: 752 of 759 pass. The rest are WASI semantics: errno numbers
  (`EROFS` is 69, not 30: use `errno.EROFS`), wasi-libc's emulated cwd (`vfs_posix_enoent`,
  `vfs_posix_paths`), `deflate_stream_error` (errno again), heap-filling tests, overriding a built-in module from a file (`builtin_ext`), `extreme_exc`.

## Batteries: frozen modules (`manifest.py`)

Bytecode frozen into `app.wasm` (+150 KB), imported like built-ins: micropython-lib's pure-Python
stdlib (argparse, datetime, logging, unittest, functools, itertools, contextlib, copy, shutil,
tempfile, tarfile, gzip, hmac, base64, html, pprint, inspect, traceback, ...) and, in `lib/`,
NucleoOS versions where micropython-lib falls short: `os` (+ `walk`, `makedirs`), `os.path` (the
posixpath API: `splitext`, `normpath`, `relpath`, `getsize`, ...), `pathlib` (`iterdir`, fnmatch
globs yielding `Path`), `textwrap` (micropython-lib's needs regexes MicroPython cannot compile).

## Not included

No `select`, `socket`, `ssl`, `asyncio`, `_thread`, `machine`, native/viper emitters, `dataclasses`, `typing`. `input()` reads one line from the Terminal; at EOF it raises `EOFError`.
