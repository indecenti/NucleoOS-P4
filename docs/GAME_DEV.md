# Making a game for NucleoOS (Vertice)

A practical guide, drawn from developing **Vertice GP** (kart) and **Vertice Bass** (arcade fishing).
The engine is described in [VERTICE.md](VERTICE.md), the app ABI in [WASM_APPS.md](WASM_APPS.md).
This file covers the *how*: structure, assets, testing, publishing, and the mistakes already made.

## 1. Anatomy of a game

```
apps/<id>/
  manifest.json     id, version, abi, requires, canvas, permissions, it/en descriptions
  main.c            game states, input, HUD, screens
  *.c / *.h         the rest (world, AI, mesh builders…): every .c file is compiled
  icon.z            icon (see the other apps)
  img/*.565         images and textures (RGB565, uint16 w,h little-endian header)
  snd/*.wav         sounds, 48 kHz MONO 16-bit
  models/*.vxm      models (tools/vertice/obj2vxm.py)
```

Typical manifest:

```json
{ "id": "bass", "name": "Vertice Bass", "version": "1.6.0", "entry": "run", "abi": 9,
  "requires": { "vertice": "1.2" },
  "ram_budget": 262144, "stack_kb": 32, "timeout_ms": 120000,
  "permissions": ["gfx", "log"], "canvas_w": 512, "canvas_h": 300, "canvas_scale": "fit",
  "system_gestures": false, "category": "games",
  "descriptions": { "it": "…", "en": "…" } }
```

- `requires.vertice` = the engine version you need (`VX_VERSION` in
  `components/vertice/include/vertice.h`). If you use a new engine function, raise the version
  in the engine **and** in the manifest: on older firmware the app is rejected with a clear message
  instead of failing to start.
- `system_gestures: false`: the game has its own buttons to exit; the OS edge gestures are off
  while the game is on screen (firmware ≥ 1.1.129).
- A 512×300 canvas scaled to 1024×600: half the pixels to fill, same result.

## 2. The loop

```c
NV_EXPORT("run") void run(void) {
    build_world();                          // everything is built ONCE
    while (nv_gfx_present()) {
        read_input();                       // touch + pad into one struct
        update(dt);                         // state machine
        if (!full_screen_painting) vx_render();
        draw_2d();                          // HUD, panels, buttons on top of the 3D
    }
}
```

- **State machine** (`ST_TITLE`, `ST_SELECT`, `ST_AIM`, …): each state has an update block and a
  draw block. `go(state, now)` records the entry time: animations are functions of
  `now - s_state_ms`, never counters.
- If a screen is covered entirely by a painting, **don't call `vx_render()`**: a free frame.
- `dt` clamped to 50 ms: a slow frame must not teleport anything.

## 3. Input: touch, keyboard, joypad

- Touch: fixed zones (d-pad on the left, A/B on the right) read with `nv_touch_count/at` (multitouch),
  plus the "tap" on release for menu buttons.
- `nv_gfx_pad()` merges USB keyboard and joypad into a SNES mask. If `NV_PAD_KEYBOARD` or
  `NV_PAD_GAMEPAD` is set: **hide all touch controls** and draw a hint strip with the real keys
  (keyboard: SPACE=A, Z=B, P=START, ESC=SELECT). Every action must have a key: menus included
  (A confirms, B goes back, START pauses).
- Think of the controls as the real gesture: in fishing, DOWN = rod back (jerk, hook set), B = release
  the line. Players get it without reading.

## 4. The 3D world (Vertice)

- Build meshes with a builder (see `apps/bass/mesh.c`): vertices + oriented triangles +
  a material per triangle, **one mesh per material/zone**, clones for copies.
- Textures on boxes: `mb_box_uv(..., tile)` maps world units to texels, so a texture has
  the same scale on objects of any size.
- **UV limit**: a triangle with a UV span > 8192 falls into the slow path (~2000 cycles/pixel).
  Keep `side / tile * 1024` under 8192 (the engine already shifts the UVs for whole repetitions).
- Backgrounds without triangles: `vx_floor` (Mode-7, water or grass), `vx_panorama` (360° around
  the horizon, magenta texel = gradient sky), `vx_water(strength, wave)` (Vertice 1.2: the
  floor reflects the panorama and sky with Fresnel and ripples; 0 underwater).
- Distant trees, reeds, bushes: `VX_BILLBOARD` with a keyed texture (transparent magenta).
- Particles: `vx_emitter` once, `vx_emit` when needed (splashes, bubbles, additive sparks).
- To show/hide groups (above/below the water) keep the ids in lists and use `vx_obj_show`.
- Per-object size: `vx_obj_scale(id, percent)` (e.g. fish by weight).

## 5. Assets with the local models

Everything runs on the PC, no external services.

### Images — Qwen-Image 2.1 in ComfyUI (`tools/qwen_assets.py`)

- **2×2 grids**: one 1024×1024 image = four assets that are consistent with each other (`split_grid`).
  Prompt: "A 2x2 grid of four separate, equally sized square panels, separated by thin white
  borders…", then one panel per position (Top left…, Bottom right…).
- **Cut-out objects** (fish, lures, trees, icons): ask for "on a plain flat deep navy blue
  background", then flood-fill from the edges to magenta (`0xF81F`) = transparent. Inner holes
  (e.g. a tire) have to be filled by hand.
- **Tileable textures**: "seamless tileable texture, flat orthographic view, even lighting";
  then `tileable()` (cross-fade with the copy shifted by half a tile) and 128×128.
- **Panoramas**: 2048×512, "the shoreline runs along the very bottom edge"; wrap horizontally
  (fade the ends), make the sky magenta by flood-filling from the top, then crop the
  1024×128 strip with the shore at the bottom (horizon row 124).
- **Full-screen paintings** (title, lakes, victory, intro): 512×300 with `fit(..., "cover")`.
- Intermittent `HostBuffer.read_file_slice` error: `generate()` retries on its own.
- Ready-made tool: `tools/game_assets.py` (`grid` / `pano` / `art` / `music`), full instructions
  in the skill `.claude/skills/game-assets/SKILL.md`. Bass's exact recipes (prompt + seed) in
  `apps/bass/art/`: regenerating with the same seed gives the same asset.
- The GPU is shared: if ComfyUI has little free VRAM (other user processes) generation
  stalls; check `system_stats` before launching a batch.

### Music — ACE-Step 1.5 turbo via ComfyUI (`tools/ace_music.py`)

- `generate(tags, seconds, bpm, key, seed)`; instrumental tags ("instrumental, 1990s arcade video
  game music, …"), **always bpm and key**. Then `to_device_wav` → 48 kHz mono, peak 0.35.
- Short tracks (7–30 s): intro, menu (looping), one theme per level, fanfares.
- Don't run ComfyUI and the standalone ACE-Step server together (same VRAM).

### Effects — synthesis (`tools/gen_bass_sfx.py`)

- numpy: filtered noise (water), resonant clicks (reel), inharmonic bells, brass,
  pulse engine. `python tools/gen_bass_sfx.py motor fish_on` regenerates only those.
- Low peak (-10 dBFS): loud sounds through the board's amplifier can cause voltage
  drops. `nv_sound` plays **one at a time**: the new one interrupts the old one.

## 6. Testing on the PC (simulator)

```bash
bash tools/vertice/sim/run.sh apps/bass 600
```

- Compiles the real engine + the app as native C, saves the frames as PNG (1024×600).
- `VX_PAD="10-11:16;90-150:1"` = pad bits by frame ranges (UP=1 DOWN=2 LEFT=4 RIGHT=8
  A=16 B=32 START=1024; `1<<30` simulates a connected keyboard). `VX_DUMP="50,120"` = frames to save.
- `APP_CFLAGS="-DBASS_TEST_CATCH=2 ..."`: a test build that opens a screen directly
  (see the `#ifdef BASS_TEST_*` in `apps/bass/main.c`), so rare screens can be checked.
- From Windows run the scripts with `wsl bash /mnt/c/...` from PowerShell (Git Bash mangles `/mnt/c`).
- Always look at the frames (contact sheets with PIL): that is how you find overlaps,
  artifacts, objects out of scale.

## 7. On the board

```powershell
.\sdk\push_app.ps1 -AppDir apps\bass -Aot -NoRun          # wasm + AOT + manifest
```

- New assets (img/snd) are uploaded separately with `curl --data-binary` to
  `/api/fs/write?path=/apps/<id>/img/<name>` (header `Authorization: Bearer <token>`).
- Anyone who has the game open must **close and reopen it**.
- If the game uses a new engine function, the new firmware (OTA release) is needed first.

## 8. Publishing

1. `git fetch` + `git rebase origin/main` (other sessions work in parallel), commit, push.
2. Firmware if needed (signed release; see the OTA notes).
3. Store: `python tools/dist.py store` **after** the rebase (it exports the current catalog).
4. Privacy check before every push: no tokens, home IPs, network names, personal paths, and
   cleaned-up screenshots.

## 9. Performance: measured rules

- The bottleneck is PSRAM bandwidth: the SRAM tile renderer (two cores + DMA) works around it; keep
  a few big meshes instead of many small ones.
- RV32 has no int64→float conversion: no 64-bit math per pixel or per row.
- No `float` in 2D per-pixel loops; 2D costs per call (`nv_gfx_*` crosses the WASM
  boundary): fewer calls, whole paintings instead of a thousand rectangles.
- `NV_RGB` is a function: for constants use a `C565(r,g,b)` macro.
- 64 KB of WASM linear memory for the app's stack/data: use big static buffers sparingly.
- The 5×7 font has a limited character set (see WASM_APPS.md): no `^`, accents as `'`.

## 10. "Arcade" gameplay: what worked

- Every success has a **moment**: a banner that slams in ("FISH ON!"), a flash, a small freeze frame
  (hit-stop 120–150 ms), camera shake, a dedicated sound.
- Numbers that count up (weight, points) instead of appearing; bars that fill.
- Records: a special screen for the podium (sunburst, falling medals, bouncing letters).
- Real rarity: most catches small, giants rare (`r^5` distribution, 2% monsters).
- Rules readable on screen in the first seconds and contextual hints ("RED! STOP
  REELING").
- Copy the **mechanics** of the classics (here Fisherman's Bait: a live well for the 3 heaviest, bonus
  time per weight, "CLEAN UP" trash, sonar) — never other people's names, trademarks or graphics.
