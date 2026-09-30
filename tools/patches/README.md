# Local patches to third-party trees

`reference/` is git-ignored, so changes made inside the cloned repos there are not versioned by
this project. Each patch below is the full local diff of one of those clones; re-apply it after a
fresh clone or an upstream bump.

## wamr-nucleov2.patch — `reference/wasm-micro-runtime`

Base commit: `5ee03eaf6b7bf93781a9e7717b66dc81d9eefa9f`

```
git -C reference/wasm-micro-runtime checkout 5ee03eaf6b7bf93781a9e7717b66dc81d9eefa9f
git -C reference/wasm-micro-runtime apply ../../tools/patches/wamr-nucleov2.patch
```

What it changes:

- `build-scripts/esp-idf/wamr/CMakeLists.txt` — `WAMR_BUILD_THREAD_MGR` (lets
  `wasm_runtime_terminate()` kill a runaway guest), `WASM_ENABLE_INSTRUCTION_METERING`, and
  `esp_mm` in `REQUIRES` (for `esp_cache_msync`).
- `core/shared/platform/esp-idf/espidf_memmap.c` — ESP32-P4 AOT support: executable memory for
  AOT code comes from PSRAM (with `CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT` no heap region has
  `MALLOC_CAP_EXEC`, but PSRAM is instruction-fetchable), cache-line aligned, and
  `os_icache_flush()` writes the D-cache back and invalidates the I-caches after loading.
- `core/shared/platform/esp-idf/platform_internal.h` — makes `CONFIG_WAMR_ENABLE_LIBC_WASI`
  compile on ESP-IDF: `os_timespec` / `os_poll_file_handle` / `os_nfds_t` were `int`
  placeholders upstream, but libc-wasi uses them as `struct timespec` / `struct pollfd`. The rest
  of the WASI-on-FATFS support (directory fds, the `*at()` calls, `nanosleep`,
  `os_compare_file_handle`) lives in the tracked `components/nv_wasm/nv_wasm_wasi.c`, not here.
- `core/iwasm/libraries/libc-wasi/sandboxed-system-primitives/src/posix.c` — `readlinkat_dup()`
  (run on every path component to detect symlinks) sized its first buffer from `st_size`, i.e. a
  full-file-sized malloc for every open/stat of a regular file: opening a 28 MB WAD failed with
  ENOMEM once the file outgrew the largest free PSRAM block. Only a symlink's size is used now.
- `core/iwasm/aot/arch/aot_reloc_riscv.c` — registers `__fixsfdi` / `__fixunssfdi` for rv32f: an
  AOT module converting a float to a 64-bit integer was refused at load and ran interpreted.

After a WAMR bump, also rebuild `wamrc` from the same tree (the AOT file format version must
match the runtime), e.g. in WSL:

```
cmake -S reference/wasm-micro-runtime/wamr-compiler -B /root/wamrc-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DWAMR_BUILD_WITH_CUSTOM_LLVM=1 \
  -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
ninja -C /root/wamrc-build
```
