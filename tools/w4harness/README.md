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
bash tools/w4harness/store.sh          # the carts that passed -> an app-store folder (+ app.aot)
```

## w4run

```
w4run cart.wasm [--frames N] [--stack KB] [--screen f.ppm] [--canvas f.ppm] [--icon f.argb]
                [--prep f.wasm] [--verify] [--keyboard F] [--mouse] [--quiet]
```

- Loads the cart exactly like `nv_wasm` (prepare, load, instantiate with one fixed page, begin),
  runs N frames (default 600) with autoplay input and prints one `RESULT` line with the status,
  how many frames changed the screen, the average/max `update()` time and any import the OS
  doesn't provide.
- `--screen` / `--canvas`: the screen as shown (480x480 with the touch gamepad, 600x600 with a
  keyboard) or the whole 1024x600 canvas at the last frame.
- `--verify`: at the end, compare the incrementally redrawn screen (only changed boxes are
  re-upscaled on the device) with a full render: `MISMATCH` if they differ.
- `--keyboard F`: a USB keyboard is plugged in at frame F (`0` = from the start): the layout
  switches live to the 600 px screen and the autoplay types the same pattern (X, Z, arrows).
  `--mouse` adds a USB mouse that wanders over the screen and right/middle-clicks now and then.
  `--pads N`: N USB gamepads (players 1..N) with random directions and buttons from the start.
  The HID state comes from `shim/nv_hid_host.h`, implemented in `w4run.cpp`.
- `--icon`: 80x80 launcher icon (`icon.argb`) of the last frame — used by `sdk/w4_import.ps1`.
- `--prep`: only write the prepared bytes. AOT carts must be compiled from these: wamrc bakes
  WAMR's memory shrink into the image, which a cart (owning all 64 KB) can't live with.

## sweep.sh

Fetches the official gallery (`github.com/aduros/wasm4`, `site/static/carts`, ~150 carts, sparse
clone into `~/w4harness`; they're other people's work and are only run locally) and runs each
for 10 s of game time in its own process, three times: touch gamepad, keyboard plugged in halfway
+ mouse, two USB gamepads (`[touch]` / `[kbd]` / `[pads]` at the end of each line), always with
`--verify`. Status: `OK`,
`TRAP` (the cart itself faulted, e.g. an out-of-bounds access), `SLOW` (didn't finish in 60 s on
the PC interpreter), and three host bugs that make the script exit non-zero: `CRASH` (**the device
would reboot**), `MISMATCH` (partial redraw differs from a full one) and `QUIT` (the host asked to
quit although Esc was never typed). It also lists the heaviest carts: anything above ~1 ms per
frame here needs `app.aot` on the P4.

## store.sh

Builds `~/w4harness/store-apps/<id>/{manifest.json, app.wasm, app.aot}` for every cart that was
`OK` in every mode of the last sweep, with name / author / description from the cart's gallery
page, and the prepared bytes compiled by wamrc (`/root/wamrc-build/wamrc`, override with
`WAMRC=`). Serve it with `server/appstore/appstore_server.py --apps-dir` (see its README). The
carts are CC BY-NC-SA 4.0: for your own device only, never committed here.

State at 2026-09-23: 150/151 run with touch, 149/151 with keyboard + mouse (`text-input` traps on
its own out-of-bounds access in both; `pocket-dust` traps the same way when the simulated mouse
right-clicks in its first frame, a cart bug, and plays fine otherwise); the sweep found
and fixed 4 host bugs before they reached the board (see `nv_w4_prepare_module` and
`call_checked` in `nv_wasm.cpp`). `endless-runner` imports `blit` with 5 parameters instead of 6:
WAMR can't link that import (the web runtime tolerates it); it runs as long as it never calls it.
