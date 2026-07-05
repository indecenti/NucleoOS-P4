# Game development on NucleoOS (ESP32-P4)

Analysis of the graphics stack, the technology chosen for games, and the first prototype
(**Nucleo Tanks**, a Pocket-Tanks-style artillery duel).

## 1. What the hardware gives us

ESP32-P4, board Guition JC1060P420C, 7" **1024×600** MIPI-DSI (JD9165), RGB565.

| Block | Use for games |
|-------|---------------|
| 2× RISC-V @ 360 MHz (400 MHz **bricks** the board — stay 360) | game logic + render split |
| **No 3D GPU** | 3D/OpenGL is out. 2D only. |
| **PPA** (Pixel Processing Accelerator) | HW blit / scale / rotate / blend of 2D surfaces |
| 2D-DMA | fast framebuffer/tile copies |
| HW JPEG codec (dec+enc) | load sprite atlases instantly |
| H.264 enc+dec (esp_h264) | animated cutscenes / recording |
| ES8311 DAC + I2S (nv_audio) | SFX + music |
| 32 MB PSRAM | off-chip surfaces, multi-buffer |
| IRAM | pin per-pixel hot loops (no cache stall) |

**Verdict:** strong 2D machine, zero 3D. Games live or die on how well they use the PPA and
the pixel buffers in PSRAM.

## 2. The two rendering paths

**A. LVGL Canvas (what the prototype uses).** Allocate an RGB565 buffer in PSRAM, hand it to an
`lv_canvas`, and write every pixel yourself. LVGL composites the finished canvas as a **single
image**, which the `esp_lvgl_port` PPA path blits in one shot. You get full per-pixel control
(mandatory for destructible terrain) while staying inside the app model — teardown on
`LV_EVENT_DELETE`, the system gesture standard, the memory broker, the theme. HUD is normal
LVGL widgets layered on top.

**B. Direct panel framebuffer + PPA (the future `nv_gfx2d`).** Take the DSI framebuffer, stop
the LVGL port for the app's lifetime (as `secondscreen` already does with `lvgl_port_stop`),
and drive sprite blits straight through the PPA with double/triple buffering. Faster and the
only way to hit 60 fps with many moving sprites, but you own the whole screen and give up the
OS chrome for that app.

### Why the prototype picked A

- Pocket Tanks is **turn-based**; only the ~1–2 s shell flight animates. A full-canvas redraw
  at 30 fps during flight is trivial, and an idle aiming turn costs **zero** (no timer work).
- It proves the pipeline — own pixel buffer in PSRAM, per-pixel raster, LVGL/PPA compositing —
  **without** fighting panel-framebuffer ownership. That same raster code drops straight into
  `nv_gfx2d` later.
- Full OS integration for free: back gesture, memory budget, theme colors, clean teardown.

**Rule of thumb:** turn-based / low-motion / UI-heavy → **Canvas**. Fast action, scrolling,
many sprites, 60 fps → graduate to **direct framebuffer + PPA** (`nv_gfx2d`).

### Hard constraints (learned, do not relearn)

- **No big buffers in static `.bss`** — internal SRAM exhausts at boot ("Could not reserve DMA
  pool"). Allocate pixel/state buffers lazily in **PSRAM** on app open, free on close.
- **No LVGL draw-layers** on the P4 software renderer (shadow / transform / opacity / clip-
  corner) — they hang the renderer (WDT). The prototype disables `clip_corner` on the canvas.
- **Never 400 MHz.**

## 3. Nucleo Tanks — prototype architecture

File: [components/nv_apps/tanks_app.cpp](components/nv_apps/tanks_app.cpp). One self-registering
`NvApp`, ~600 lines, no new dependencies.

```
content (app plane, 1024×498)
├── HUD row  (48px)  P1 health | wind + weapon + banner | P2 health   ← LVGL widgets
├── Canvas   (960×340 RGB565 in PSRAM)  sky · terrain · tanks · shell · blast  ← own pixels
└── Controls (44px)  A- A+ P- P+ | ANG/PWR | < weapon > | FIRE | CPU | ↻      ← LVGL widgets
```

**Loop + double-buffer (the optimization).** A single `lv_timer` at ~36 fps (`DT = 28 ms`).
Two PSRAM buffers: `s_bg` holds the static scene (sky + terrain + tanks + wind), repainted only
when that scene actually changes (aim, turn, explosion). Each animation frame then (1) restores
the small rectangle it dirtied last frame from `s_bg`, (2) draws the moving bits (shell, trail,
particles, blast), and (3) invalidates **only** that rectangle via `lv_obj_invalidate_area` — so
both the pixel work and LVGL's flush scale with the moving region, not the whole 960×340 field.
No per-frame full recompute. In `AIM`/`OVER` the loop early-returns (idle turn = free).

**UX.** Drag anywhere on the field to aim the active tank slingshot-style (direction → angle,
distance → power); +/- buttons for fine control. Dotted trajectory preview, shell trail,
explosion particles + expanding ring, on-canvas wind arrow, and `nv_audio_tone` SFX (fire thump,
boom, aim clicks, win chime).

**Terrain = height field.** `int16_t terr[960]` — one surface-y per column; solid fills
`terr[x]..GH`. Drawing is a per-column vertical fill (grass cap + banded dirt). **Destructible:**
an explosion calls `carve()`, which pushes each covered column's surface down to the bottom of
the blast circle. No overhangs possible, trivial collision (`shell.y ≥ terr[x]`).

**Physics.** Euler integration with `SUBSTEP=4` sub-steps per frame for collision precision:
`vx += wind·dt; vy += G·dt`. Launch speed = `power · POW_K`; angle 0..180° (0 = right, 90 = up).
Wind is a random horizontal accel (±90) re-rolled each turn. Direct enemy hit, terrain hit, and
out-of-bounds (miss) all resolve the turn.

**Weapons** (name / blast radius / peak damage): Shell 30/34 · Big Shot 46/52 · Digger 40/16 ·
Nuke 72/82. Damage falls off linearly with distance from the blast center; self-damage possible.

**Opponent.** Toggle **CPU / 2P**. The CPU aims with a ballistic estimate at ~45° elevation
(`v0 = √(range·G / sin2θ)`), nudges power for wind, and adds ±5 spread — plausible, beatable.
2P is hotseat on the same controls.

**Aiming aid.** During `AIM` a faint dotted arc previews the current angle/power/wind
trajectory until it meets terrain — makes the touch controls usable.

**Teardown.** `page_deleted` (`LV_EVENT_DELETE`) deletes both timers and frees the PSRAM pixel
buffer and game struct. Follows the memory rule exactly.

## 4. Roadmap — toward a real game platform

1. **`nv_gfx2d`** — reusable 2D lib: PSRAM surfaces, PPA blit/scale/rotate/alpha, tile-maps,
   dirty-rect, IRAM hot paths. Lift the raster helpers out of `tanks_app.cpp`.
2. **`nv_game`** — engine over gfx2d: dual-core split (logic ‖ render/audio via lock-free ring,
   like `nv_vplayer`), fixed-timestep loop, abstract input (`nv_input_poll` unifying GT911
   touch, KeyDeck gamepad over TCP, future USB-HID), PCM SFX mixer on ES8311.
3. **`nv_game.h` SDK + template** — `game_init/update(dt)/draw`. Two build targets: **native**
   (full speed) and **WASM** via WAMR (sandboxed, hot-reload, game jams).
4. **Asset tool (PC)** — PNG → JPEG atlas + tilemap binaries, loaded through the HW JPEG decoder.

## 5. Prototype: still to do

- Custom launcher icon (currently reuses `nv_icon_apps`). One line in `tools/gen_icons.py`
  (`('crosshairs', 'game', {'dir':'mdi', ...})` — the `crosshairs` MDI glyph exists) + rerun.
- Per-weapon ammo (Pocket Tanks: each weapon once) — currently unlimited.
- More weapons (cluster/roller/tracer), tank fall damage, scrolling for a wider battlefield.
- Move raster helpers into `nv_gfx2d` when step 1 begins.

**Status:** builds clean (partition 43% free), registered in the launcher as **Tanks**,
**not yet hardware-verified on screen.**

## 6. Games on SD via the store — ABI v2 game surface

The store used to run only headless scripts (one `run()` call, text output). It now hosts real
games: **host ABI v2** adds a draw-command game surface, so a whole game ships as `app.wasm` on
the SD card — **no reflash**.

**Why draw-commands, not a raw framebuffer:** WAMR runs interpreted, so per-pixel loops in WASM
are slow. Instead the guest calls `nv_gfx_rect/circle/line/blit/clear` and the **OS executes the
pixels natively**; the interpreted guest only runs game logic (cheap).

**Lifecycle:** the guest owns its loop —
`run(){ setup(); while (nv_gfx_present()) { input(); update(); draw(); } }`. `nv_gfx_present()`
publishes the frame and blocks until the OS has shown it (frame pacing), returning 0 when the OS
wants the app closed (cooperative stop). Games get **no opcode cap** — present() is their liveness
point.

**Plumbing:**
- `nv_wasm.cpp` — gfx imports + a **ping-pong double buffer**: guest draws into buffer A while the
  UI shows buffer B, swapped at present(); no tearing, no per-pixel locking. Touch is a packed
  `gfx_input()` snapshot; `gfx_tone()` bridges to `nv_audio`.
- `apps_app.cpp` — a game app (`nv_wasm_app_is_game`) opens as a **full canvas** (not the console):
  a timer does take_frame → `lv_canvas_set_buffer` → invalidate; canvas touch → `gfx_set_input`;
  teardown aborts the run.
- `sdk/include/nucleo_sdk.h` — ABI 2 wrappers: `nv_gfx_*`, `NV_RGB`, `nv_touch`. Guests stay
  **libm-free** (the demo aims by vector velocity, no trig).
- Manifest: `"abi": 2`, `"permissions": ["gfx"]`, `"canvas_w"`, `"canvas_h"`.

**Demo:** [apps/cannon](apps/cannon) — a 640×360 drag-to-fire artillery range (2.2 KB `app.wasm`),
built with `sdk/build_app.ps1` and deployed with `sdk/push_app.ps1` (Wi-Fi, no card pull). Proves
the whole surface: draw commands, touch, tone, frame loop.

**Next:** port the full **Tanks** logic onto `nv_gfx_*` and ship it as `app.wasm` — its logic is
already separated from its pixels, so it's a direct port.
