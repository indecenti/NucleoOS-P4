# w4harness — WASM-4 layer on the PC

Runs WASM-4 carts through the firmware's own host code (`components/nv_wasm/nv_wasm_w4.cpp` +
the vendored rasterizer/APU in `components/nv_wasm/w4`) on a WAMR built from the same tree with
the device's configuration: fast interpreter, **software** bounds checks (the P4 has no guard
pages), WASI, reference types, bulk memory, thread manager. FreeRTOS, logging and audio are
shimmed (`shim/`); carts run silent.

Everything runs in WSL (Ubuntu): `cmake`, `ninja`, `gcc`, `git`.

```bash
bash tools/w4harness/build.sh          # -> ~/w4harness/w4run (WAMR is built once)
bash tools/w4harness/sweep.sh          # regression run over the wasm4.org gallery
```

## w4run

```
w4run cart.wasm [--frames N] [--stack KB] [--screen f.ppm] [--canvas f.ppm] [--icon f.argb]
                [--prep f.wasm] [--quiet]
```

- Loads the cart exactly like `nv_wasm` (prepare, load, instantiate with one fixed page, begin),
  runs N frames (default 600) with autoplay input and prints one `RESULT` line with the status,
  how many frames changed the screen, the average/max `update()` time and any import the OS
  doesn't provide.
- `--screen` / `--canvas`: the 480x480 screen or the whole 1024x600 canvas (touch gamepad
  included) at the last frame.
- `--icon`: 80x80 launcher icon (`icon.argb`) of the last frame — used by `sdk/w4_import.ps1`.
- `--prep`: only write the prepared bytes. AOT carts must be compiled from these: wamrc bakes
  WAMR's memory shrink into the image, which a cart (owning all 64 KB) can't live with.

## sweep.sh

Fetches the official gallery (`github.com/aduros/wasm4`, `site/static/carts`, ~150 carts, sparse
clone into `~/w4harness`; they're other people's work and are only run locally) and runs each
for 10 s of game time in its own process. Status: `OK`, `TRAP` (the cart itself faulted, e.g. an
out-of-bounds access), `SLOW` (didn't finish in 60 s on the PC interpreter), `CRASH` (**a host
bug: the device would reboot** — the script exits non-zero). It also lists the heaviest carts:
anything above ~1 ms per frame here needs `app.aot` on the P4.

State at 2026-09-23: 150/151 run (1 cart traps on its own out-of-bounds access); the sweep found
and fixed 4 host bugs before they reached the board (see `nv_w4_prepare_module` and
`call_checked` in `nv_wasm.cpp`). `endless-runner` imports `blit` with 5 parameters instead of 6:
WAMR can't link that import (the web runtime tolerates it); it runs as long as it never calls it.
