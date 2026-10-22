# Vertice — the NucleoOS 3D engine

Vertice is the NucleoOS software 3D engine for the ESP32-P4: scene, lights, fog, particles, models,
Mode-7 floor, 360° panorama and picking, exposed to WASM apps through the `nv.vx_*` imports (ABI 9).
Games remain WASM packages distributed through the store; the engine lives in the firmware and is a
**system component** that apps declare as a dependency.

- Code: `components/vertice/` (`vertice.cpp` = engine, `core/` = rasterizer).
- C API: `components/vertice/include/vertice.h`. WASM API: *Vertice* section of `sdk/include/nucleo_sdk.h`.
- Reference game: `apps/vxgp` (*Vertice GP*, 3D kart).
- PC simulator: `tools/vertice/sim/` (same engine, compiled for Linux/WSL).
- License: the rasterizer in `core/` derives from **Jet** by CubeCoders (MIT, commit c56dfc0,
  notice in `core/LICENSE-Jet`, see `THIRD_PARTY.md`); everything else is NucleoOS.

## Why it is built this way: the P4 bottleneck

Measurements on the board, before writing a single line of optimization:

| Measured fact | Consequence |
|---|---|
| CPU writes to cached PSRAM ≈ **33 MB/s per core** (latency, not bandwidth) | A 512×300 canvas (300 KB) rewritten 2-3 times per frame costs tens of ms: **never rasterize in PSRAM** |
| 64 KB 2-way L1 D-cache **shared** by the two cores; 128 KB L2, 64 B lines | Small, contiguous hot data; the two cores must not step on the same lines |
| Free internal SRAM during a game ≈ 275 KB (max block ≈ 172 KB), needed by Wi-Fi and DMA | Take little SRAM, only for the hottest data, and never below the reserve |
| 2D DMA (`nv_2d_copy`) writes PSRAM without going through the CPU | The frame write-out goes through DMA while the CPU keeps rasterizing |
| WAMR interpreter: every host call costs | The app describes the scene once; per frame it only moves objects and the camera |

First real profile (1.1.118.1, banded rendering in PSRAM): **107 ms/frame** — prep 18 ms,
clear 7-11 ms, raster 75-79 ms per band. This led to the tile architecture described below.

## Frame pipeline

```
WASM app (1 thread)           core 0                           core 1
  vx_obj_pos / vx_camera ...
  vx_render() ─────────────▶  particles_update, floor_relight
                              bg_frame_setup (horizon, per-column panorama, sky)
                              prepare: cull, transform, sort front-to-back
                              vxBin: queue → per-tile lists (counting sort)
                              ┌── tile = atomic++ ──┐          ┌── tile = atomic++ ──┐
                              │ clear_rows (sky,    │          │   (same work,        │
                              │  panorama, Mode-7,  │          │    different tiles)  │
                              │  z) in SRAM         │          │                      │
                              │ vxRasterTile        │          │                      │
                              │ tile particles      │          │                      │
                              │ nv_2d_copy → canvas │          │                      │
                              └─────────────────────┘          └──────────────────────┘
                              ◀──────────── join ──────────────
  nv_gfx_present() → PPA scales the canvas to the panel (canvas_scale fit/stretch/zoom)
```

- **Tiles in SRAM**: 8-row tiles (4 if SRAM is scarce); each core has its own color + z buffer
  in a single contiguous SRAM block (512 canvas → 32 KB). The rasterizer works with virtual bases
  (`col - y0*w`), so the triangle code does not know it is inside a tile.
- **Work stealing**: an atomic counter hands out the tiles; no static split to balance.
- **DMA write-out** (`nv_2d_copy`), with a `memcpy` fallback.
- **Banded fallback**: if at `vx_open` the SRAM is not enough (less than 96 KB would remain free), the
  frame is split into two bands rendered directly into the target, with an adaptive cut based on measured cost.

## Tricks (old and new)

| Trick | Where it comes from | What it does in Vertice |
|---|---|---|
| **Mode 7** | SNES (F-Zero, Mario Kart) | Grass/terrain has no triangles: one division per row, perspective-exact texture, light, fog, and a blend toward the average color in the distance (no aliasing/flicker) |
| **360° panorama** | skyboxes of 16-bit games | Mountains/clouds as a texture strip anchored to the horizon, 1 lookup per column |
| **Impostors (billboards)** | Doom, Build engine, Mario Kart | Trees, coins: 2 triangles facing the camera instead of hundreds |
| **Background painter** | all 2D games | Road, curbs, lines, shadows: ground layers without depth test/write, drawn first in creation order → never z-fighting |
| **Texels instead of polygons** | pixel art | The grandstand crowd is a texture on the risers of the steps, not geometry |
| **Fog as a blend to the sky** | PS1/N64 | Triangle color fades toward the sky color *of that row*; the sky gradient follows the horizon, so fog and haze match any pitch |
| **Fast spans** (`VX_FAST_SPANS`) | 90s rasterizers | Q8 depth, Q16 light/UV at constant step, no per-pixel division |
| **Sampled perspective** (`vxPersp`) | Quake (every 16 px) | Perspective-correct UVs every 16 pixels, affine in between: no "bent" roads |
| **Front-to-back sorting** | z-buffer early-out | The nearest opaques fill z first: pixels covered afterward are not written |
| **Reciprocal instead of division** | classic fixed-point | Fog with one Q16 reciprocal per frame, not a division per pixel |
| **Texture streaming** | strip loading | `vx_texture_new` + `vx_texture_write`: a 512×64 texture (64 KB) built in strips by an app with 64 KB of memory |

## Memory

- **PSRAM with a budget** (`VX_MEM_BUDGET` 12 MB): every `new`/`malloc` of the engine (core included) is
  redirected by `objcopy --redefine-syms` (`vertice_alloc.syms`) to a counted PSRAM allocator;
  the library's `.bss` also ends up in PSRAM (`linker.lf`). `vx_stat(VX_STAT_MEM)` reports it.
- **Internal SRAM**: only the tile block, taken at open and only if ≥ 96 KB remain free.
- **Hard caps** (`vertice.h`, 1.5): 512 objects, 250 materials, 64 textures (side 8..1024, powers
  of 2), 24,000 triangles and 32,000 scene vertices, 8 emitters × 512 particles. Every call validates
  its arguments (they come from untrusted WASM code).
- The engine shuts itself down when the app ends (`vx_close` at the end of `run_worker`).

## API for apps (summary)

Manifest:

```json
{ "id": "mygame", "abi": 16, "requires": { "vertice": "1.5" },
  "canvas_w": 512, "canvas_h": 300, "canvas_scale": "fit", "permissions": ["gfx", "log"] }
```

- Scene: `vx_texture` / `vx_texture_load` (img/*.565) / `vx_texture_new` + `vx_texture_write`,
  `vx_material(color, shading, alpha, tex, specular)`, `vx_prim` (cube, sphere, cylinder, capsule,
  pyramid, plane, grid, quad, billboard), `vx_mesh`, `vx_model` (models/*.vxm, from an .obj + .mtl with
  `tools/vertice/obj2vxm.py model.obj apps/x/models/name.vxm --size 120 --base`; colors and
  transparency from the .mtl, UVs kept), `vx_clone`. Meshes in code: `sdk/include/vx_build.h`.
- Objects: `vx_obj_pos/rot/show/free`, `vx_obj_depth(id, bias, VX_DEPTH_NOTEST|VX_DEPTH_NOWRITE)`.
- Camera and atmosphere: `vx_camera`, `vx_look_at`, `vx_lens`, `vx_sun`, `vx_ambient`, `vx_sky`,
  `vx_fog`, `vx_depth`, `vx_floor`, `vx_panorama`, `vx_water` (1.2: panorama reflection on the
  floor, Fresnel + ripples), `vx_caustics(strength, speed)` and `vx_shafts(strength, slope)`
  (1.3: underwater — a net of light moving across the seabed, computed on the floor texture
  once per frame; oblique light rays from above, per tile), `vx_ceiling(y, tex, repeat)` (1.3:
  the surface seen from below as a Mode-7 plane above the horizon, ~10 cycles/pixel instead of
  obliquely textured quads; `tex` -1 turns it off). 1.4: `vx_panorama(tex, row | (n << 12))` makes
  the texture go around the horizon n times (2..15): n times the definition (1024 texels over
  360° are ~3 screen pixels each), the image repeats every 360/n degrees — on a painted sky it
  is not noticeable. A game that uses it asks for `"vertice": "1.4"` (older engines cut the row).
- Culling: a ring of decorations in ONE mesh has a bounding box that contains the
  camera and is never discarded — split it into angular sectors (Vertice Bass `SECTORS`).
- Effects: `vx_emitter` + `vx_emit` (dust, smoke, sparks, confetti; additive or alpha).
- Frame: `vx_render()` then the 2D on top (HUD with `nv_gfx_*`) and `nv_gfx_present()`.
- Input: `nv_gfx_pad()` — USB keyboard and gamepad in a SNES-style mask (`NV_PAD_*`); the bits
  `NV_PAD_GAMEPAD` / `NV_PAD_KEYBOARD` say whether a device is present, to hide the touch controls.
- Picking: `vx_pick_at(x,y)` → after the render `vx_picked()`.
- Profiling: `vx_stat(VX_STAT_US / PREP_US / TRIS / QUEUED / ...)`.
- **1.5 (ABI 16)**: `vx_project(x,y,z,out[3])` → pixel and depth of a point with the camera of
  the last render (a HUD attached to the 3D: markers, labels, the fishing line), `vx_texture_size`,
  `vx_obj_get_pos`, `vx_obj_fade(id, near, far)` / `vx_obj_appear` (distance fade:
  where the object is invisible it costs nothing), `vx_emitter_clear`, `vx_config(VX_CFG_*)`
  (quality settings: `MIP_BIAS` 0..3 smaller textures = faster; `NO_TEXTURES` for
  profiling). Raised limits (512 objects, 64 textures, 250 materials).
  Per object: `vx_obj_material(id, mat)` (a clone has its own faces: a red car among the blue ones),
  `vx_obj_alpha(id, 0..255)` (dissolve: ghosts, blinking after a hit), `vx_mat_set(mat,
  VX_MAT_COLOR/ALPHA/TEXTURE/SPECULAR/SHADING, v)` to change a material afterward (e.g. give the
  texture to a .vxm model). Hierarchy: `vx_obj_parent(child, parent)` — from then on the child's
  pos/rot are relative to the parent (turret, wheels, tail), exact for single-axis rotations. Shadows:
  `vx_obj_shadow(id, radius, y, alpha)` — a dark disc that follows the object. Diagnostics:
  `VX_CFG_SPAN_EXP` (cycles per pixel per raster part), `VX_CFG_EAGER_BG` (background A/B).

### Kit for a new game

- `sdk/include/nv_math.h`: math without libm (sin/cos/atan2, `NvVec3`, fps-independent `nv_smooth`,
  seeded `NvRand`, RGB565 colors).
- `sdk/include/vx_build.h`: mesh builder (orients the faces itself, UVs in world
  units): boxes, tapered branches/cylinders, lathes, rocks, discs (blob shadows), quads
  facing a point (rings of trees in a single mesh).
- `sdk/templates/vertice-game/`: a complete, short game to copy (world, car with shadow,
  cloned coins, particles, pad/touch input, chase camera, HUD with `vx_project`).

Step-by-step guide for a new game (assets with Qwen/ACE-Step, simulator, store): [GAME_DEV.md](GAME_DEV.md).

Rules for 60 fps: build everything once (meshes merged by material, clones for copies),
per frame only `vx_obj_pos/rot` + camera; ground decals as painter layers; distant trees and
objects as billboards; floor with `vx_floor`, horizon with `vx_panorama`.

## Dependencies between packages

A generic system, not only for Vertice:

- `"requires": { "<id>": "<minimum version>" }` in the manifest (up to 4). The id can be a
  **system component** (`vertice`, `wasi`, `wasm4`: versions come from the firmware) or a **package**
  installed in `/sdcard/apps/<id>/`.
- `"kind": "library"`: a package without `app.wasm` (shared textures, models, sounds); it does not appear
  in the launcher and cannot be started.
- Library assets: `"<id>:<name>"` (e.g. `"kartkit:pino"` → `/sdcard/apps/kartkit/img/pino.565`)
  in `vx_texture_load`, `vx_model`, `nv_gfx_image`, `nv_sound` — only if `<id>` is in `requires`.
- At startup `nv_wasm` rejects apps with missing or too-old dependencies with a localized
  message ("Requires …").

## PC simulator

```bash
bash tools/vertice/sim/run.sh apps/vxgp 520
```

```bash
VX_TOUCH='5-6:256,150;100-700:470,250' VX_DUMP='60,200,330' bash tools/vertice/sim/run.sh apps/vxgp 340
```

Compiles the real engine (symbols renamed `vxe_*`) + the app as native C, and saves the frames as PNG at
panel size (1024×600, scaled like the PPA). `VX_TOUCH` = touches per frame range,
`VX_PAD` = pad mask, `VX_FPS` = simulated clock. `profile.sh` does the same run under gprof.

## Status and next steps

- Done: dual-core SRAM tiles + DMA, binning, fast spans, sampled perspective, Mode-7,
  panorama, impostors, per-tile particles, texture streaming, dependencies, pad/keyboard.
- To measure on the board: the tile renderer (the sim confirms identical output).
- 1.3 (measured on the board with Vertice Bass): CLAMP textures (billboards, atlases) all ended up
  on the slow per-pixel path (~10× slower) because the fast span only accepted WRAP: now the fast
  span does the clamp itself. Floor water and fog blend the three channels in a single
  multiplication (RGB565 "spread" over 32 bits). The present of scaled canvases is asynchronous: the game
  draws frame N+1 while the UI shows N (before, it waited for the UI round, up to 16 ms).
  Advice for apps: static objects such as trees on a ring around the camera are best
  merged into one mesh of quads facing the center (Bass: 70 objects → 1). Textures have
  automatic mipmaps (chosen per triangle when a texel covers more than 1.5 pixels): less flicker
  and less PSRAM bandwidth on distant objects. Particles limited to 1/5 of the screen per tile.
- Video from the simulator: `VX_DUMP_RANGE=a-b` saves every frame, `VX_LANG=en|it` picks the language, and every
  audio call goes to the log (`snd @frame play|set|stop|master|sound`): `tools/vertice/sim/mixsim.py`
  rebuilds the mix like the board's mixer (volumes, pitch, loop, fade, limiter) so it can be assembled with ffmpeg.
- Object preparation (measured on the board, Bass, combat: ~60 objects, ~2000 vertices, ~1900
  triangles, 5-6 ms on one core): ~420 cycles per vertex and ~600 per triangle, almost all waiting on PSRAM
  (meshes read, triangle queue written). Since 1.3: transformed vertices use the tile SRAM as scratch,
  which is free in that phase (per-object setup ~3x faster). The same preparation split across the two
  cores (`Scene::PairRunner`, output identical to serial) does NOT gain: the two cores contend for
  PSRAM and every operation costs ~1.6x; it is off on the board (in the simulator `VX_PARALLEL_PREP=1`).
  Profile line `prof objs: drawn/verts/tris | kcyc setup/xform/tris`.
- Next: "hot" static meshes (positions + precomputed light) compact in SRAM, a smaller triangle
  queue, fills with PIE SIMD, dynamic resolution.
