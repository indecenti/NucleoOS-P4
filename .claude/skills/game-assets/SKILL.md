---
name: game-assets
description: Use when making art, textures, sprites, panoramas, music or sound effects for a NucleoOS game or app with the local models (Qwen-Image 2.1 and ACE-Step 1.5 in ComfyUI) — grids of four assets, keyed sprites, seamless textures, 360-degree panoramas, full-screen paintings, looping tracks, synthesized SFX, all converted to the device formats.
---

# Game assets from the local models

Everything is generated on this PC, nothing goes to external services. Worked example: Vertice Bass
(`apps/bass/img`, `apps/bass/snd`). The full game guide is `docs/GAME_DEV.md`.

## 0. Before generating

- ComfyUI must be up: `curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:8188/system_stats` → 200.
- **The GPU is shared (RTX 3070 Ti, 8 GB).** Other jobs of the user (e.g. an audio stem-separation run, ACE-Step
  standalone on :7861, llama-server) starve ComfyUI: a 1024² image goes from ~60–110 s to "never"
  and `qwen_assets.generate` times out after 15 min. Check first:
  `curl -s http://127.0.0.1:8188/system_stats` → `vram_free`; if it is a few hundred MB, tell the
  user what is holding the GPU (Get-CimInstance Win32_Process for the python PIDs) and wait — never
  kill their processes.
- Run long batches in the background and check the preview PNGs before wiring them in.

## 1. Images — Qwen-Image 2.1 (`tools/qwen_assets.py`, `tools/game_assets.py`)

Models in ComfyUI: `qwen_image_2.1-Q8_0.gguf` + `qwen3vl_8b_fp8_scaled` + `qwen_image_2.1_vae_bf16`,
turbo LoRA `qwen21_viggle_turbo_6step_r128` (6 steps, cfg 1). The driver retries the intermittent
`HostBuffer.read_file_slice` error by itself.

**One 2x2 grid = four coherent assets** (same style, same lighting). `tools/game_assets.py grid`:

| kind | prompt it builds | post-processing | typical size |
|---|---|---|---|
| `sprite` | "each panel on a plain flat deep navy blue background" + `--subject` | fit into the cell's own bg, flood-fill from the borders → magenta 0xF81F (transparent) | fish 150x100, icons 22–64, trees 64x128 |
| `tex` | "seamless tileable texture, flat orthographic view, even lighting, no objects" | crop the grid border, half-tile cross-fade (`tileable`), resize | 128x128 |
| `art` | "each panel is a painting" | cover-fit | 512x300 |

```bash
python tools/game_assets.py grid apps/mygame/img --kind sprite --size 64x128 --seed 84 \
  --names b_pine,b_snow,b_maple,b_cypress \
  --subject "one whole tree standing, side view, centred, filling the panel height" \
  --cells "a tall dark green pine fir" "a pine fir covered in snow" "an autumn maple, orange and red" "a swamp cypress with moss"
python tools/game_assets.py pano apps/mygame/img/p0.565 --seed 91 --scene "pine forest, snowy peaks, pale blue sky"
python tools/game_assets.py art apps/mygame/img/title.565 --seed 7 --scene "an angler at dawn on a misty lake"
```

Rules learned the hard way:
- Say "no text" (it is in the style string) or it paints titles and labels.
- Holes inside a keyed sprite (a tyre, a ring) keep the navy: flood-fill them by hand
  (`ImageDraw.floodfill(im, (x, y), (255, 0, 255), thresh=60)`).
- Panoramas: 2048x512, "ground line along the very bottom edge"; wrap first, key the sky from the
  **top of the full image**, then crop the 1024x128 band (flood-filling a cropped band starts on the
  mountains and eats them). Engine: `vx_panorama(tex, 124)` with the texture loaded `VX_TEX_KEY`.
- Size classes of the same thing (small / regular / monster fish) = a second and third grid with
  "a small young slim juvenile …" / "a huge fat trophy-sized old monster …".
- Scenes that must match a gameplay screen (a catch backdrop) ask for an "empty centre".
- Same prompt + same seed = same image: keep the calls in a script next to the game
  (Bass: the `gen*.py` steps listed in `docs/GAME_DEV.md`).
- Look at every image (contact sheet with PIL) before shipping; check screenshots for privacy too.

## 2. Music — ACE-Step 1.5 turbo through ComfyUI (`tools/ace_music.py`)

Graph: UNETLoader `acestep_v1.5_turbo` → DualCLIPLoader `qwen_0.6b_ace15` + `qwen_1.7b_ace15`
(type "ace") → VAELoader `ace_1.5_vae` → TextEncodeAceStepAudio1.5 (bpm, keyscale, duration, lyrics
`[Instrumental]`) → ConditioningZeroOut (negative) → EmptyAceStep1.5LatentAudio → KSampler 8 steps
cfg 1 → VAEDecodeAudio → SaveAudio (not SaveAudioAdvanced).

```bash
python tools/game_assets.py music apps/mygame/snd/menu.wav --seconds 30 --bpm 100 --key "D major" --seed 3 \
  --tags "relaxed title theme, electric piano, soft drums, loopable"
```

- **Always pass bpm and key.** Tags: "instrumental, 1990s arcade video game music, 16-bit era …" + mood
  and instruments. Seeds differ per track so the pieces differ.
- Lengths that worked: intro 20–22 s, menu 30 s (replay every 30.5 s), one 16 s theme per level,
  7 s fanfares (victory, new record).
- Output is converted to the device format: 48 kHz MONO 16-bit, peak 0.35, short fade
  (`to_device_wav`). You cannot hear it: ask the user how it sounds.
- Don't run the standalone ACE-Step server (:7861) and ComfyUI at the same time (VRAM).

## 3. Sound effects — synthesis (`tools/gen_bass_sfx.py`)

Short effects are better synthesised than generated: numpy building blocks (filtered noise +
envelopes for water, resonant clicks for a reel ratchet, a pulse train for an outboard motor,
inharmonic bell partials, a soft brass voice for stabs). `python tools/gen_bass_sfx.py motor fish_on`
regenerates only the named ones. Keep peaks at -10 dBFS (loud streams can brown out the board's
amplifier) and effects short: `nv_sound` plays one stream at a time, a new one cuts the previous.

## 4. Into the game

- Files: `apps/<id>/img/<name>.565`, `apps/<id>/snd/<name>.wav`; `nv_gfx_image(name, x, y, w, h)`,
  `vx_texture_load(name, VX_TEX_KEY?)`, `nv_sound(name)`.
- Always keep a fallback when an image is optional (`vx_texture_load` returns -1 if missing), so a
  partial asset set never breaks the game.
- Texture budget: `VX_MAX_TEXTURES` 32, sides power of two 8..1024. Mind the UV span (< 8192 per
  triangle, see GAME_DEV.md) when tiling big boxes (`mb_box_uv(..., tile)`).
- Push new assets to the board with curl to `/api/fs/write?path=/apps/<id>/img/<name>` (Bearer token
  from `%USERPROFILE%\.nucleo\token`), then reopen the game.
