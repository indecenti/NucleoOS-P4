---
name: wasm-app
description: Build, deploy and polish a NucleoOS WASM app for the ESP32-P4 board (games, tools, learning apps that draw to the screen via the ABI v2 gfx surface). Use when the user wants to create a new device app, "crea un'app wasm", "nuovo gioco per il nucleo", add a screen/mini-game, or improve an existing app's rendering/audio/voice. Covers the SDK ABI, the build+push loop over Wi-Fi, and the hard-won rules for fast rendering, sound, and offline voice. NOT for firmware/OS C++ (that's an OTA build) — see [[wasm-runtime]].
---

# NucleoOS WASM app

A device app = one C folder (`apps/<id>/`) compiled to `app.wasm` by clang (wasm32, MVP), plus a
`manifest.json`. The OS runs it in the WAMR interpreter and gives it a full-screen canvas via the
**ABI v2 gfx surface**. Reference app to copy patterns from: **`apps/abc123`** (7 mini-games, audio,
offline voice, persistence). SDK header (the ONLY source of truth for imports): **`sdk/include/nucleo_sdk.h`**.

## Anatomy

`apps/<id>/manifest.json`:
```json
{ "id":"myapp", "name":"My App", "version":"1.0", "entry":"run",
  "abi":2, "ram_budget":65536, "stack_kb":16, "timeout_ms":120000,
  "permissions":["gfx"], "canvas_w":1024, "canvas_h":600 }
```
`apps/<id>/main.c` skeleton (immediate-mode render loop — this shape is mandatory):
```c
#include "nucleo_sdk.h"
NV_EXPORT("run")
void run(void) {
    int W = nv_gfx_width(), H = nv_gfx_height();
    int redraw = 2;                                   // frame-skip: only draw when something changed
    int prev_down = 0;
    while (nv_gfx_present()) {                         // present() paces frames + returns 0 to close
        int x, y, down = nv_touch(&x, &y);
        int tap = (!down && prev_down); prev_down = down;
        if (nv_gfx_back()) break;                      // OS back gesture at your root -> return = close
        if (tap) { /* handle tap; */ redraw = 2; }
        if (redraw > 0) { /* draw everything */ redraw--; }   // 2 frames because of double-buffering
    }
}
```

## Build + deploy (one command)

```
.\.claude\skills\wasm-app\scripts\build_push.ps1 -AppDir apps\myapp            # build + push wasm+manifest
.\.claude\skills\wasm-app\scripts\build_push.ps1 -AppDir apps\myapp -Assets    # also push img\*.565 + snd\*.wav
```
It builds (`sdk\build_app.ps1`: clang wasm32 `-mcpu=mvp -O2`, **64 KB linear memory, 8 KB stack**),
finds the board with retries (mDNS `nucleov2.local` drops for seconds at a time — it falls back to the
IP), and uploads with `curl --data-binary`. **A running instance keeps the OLD wasm — the user must
reopen the app.** Always compile clean first:
```
& 'C:\Program Files\LLVM\bin\clang.exe' --target=wasm32 -mcpu=mvp -O2 -ffreestanding -nostdlib -fvisibility=hidden -Wall -Wextra -Isdk\include -c apps\myapp\main.c -o "$env:TEMP\chk.o"
```
(PowerShell eats `-Wl,` commas — use build_app.ps1's arg-array for the full link, this for warnings.)

## ABI cheat-sheet (module "nv", perm "gfx"; full list in the header)
- Frame: `nv_gfx_present`(loop cond) `nv_gfx_width/height` `nv_gfx_clear(col)` `nv_gfx_back`
- Draw: `nv_gfx_rect` `nv_gfx_circle` `nv_gfx_line` `nv_gfx_tri` (filled) `nv_gfx_text(x,y,s,col,scale)` `nv_gfx_image(name,x,y,w,h)`
- Input: `nv_touch(&x,&y)` (returns pressed 1/0; TAP = release edge)
- Audio: `nv_gfx_tone(hz,ms)` (synth beep) `nv_sound(name)` (WAV) `nv_speak(text,lang)` (voice)
- State: `nv_save(name,buf,len)` / `nv_load` (≤8 KB in the app's SD folder); `nv_millis` `nv_rand` `nv_lang(buf,len)`
- `NV_RGB(r,g,b)` -> RGB565. Text font: 5x7, chars ` 0-9 A-Z - . : % / < > ! + x` (lowercase auto-upper), advance 6*scale.

## Rendering rules — READ THIS (biggest source of lag)
Every `nv_gfx_*` call crosses the WASM→host boundary on the interpreter. **Cost = number of draw
calls per frame.** So:
- **Frame-skip when idle** (`redraw` counter). An idle screen must cost ~0 host calls. Redraw 2
  frames on a change (double-buffered — else a stale frame flickers back).
- **Animate ONLY inside bounded time windows** (feedback, transitions). Never animate an idle
  screen — it defeats the skip and burns CPU forever.
- **No alpha/opacity, no gradients cheaply.** A "gradient" = N horizontal `rect`s = N calls/frame —
  expensive. Prefer a flat `clear`. [[lvgl-draw-layers-wdt]] is LVGL-only but the "keep it flat" lesson holds.
- **No float in hot paths.** WAMR floats are slow. Use integer fixed-point + a small sine LUT for
  smooth motion (see abc123 `SINQ`/`isin`). `Date`/`rand`-free determinism not required (app has `nv_rand`).
- **Prefer 1-call primitives.** A star as `nv_gfx_image("star",…)` = 1 call vs 10 `nv_gfx_tri`.
  Cull work hidden behind a full-screen overlay (don't draw the scene under an opaque panel).
- **Colors:** helpers `lighten/darken(c565,amt)` (unpack 5/6/5, scale, repack). Card 3D look = a
  `darken(fill)` rounded lip offset down, NOT a square gray rect (square rects poke over rounded borders).
- **Static .bss is scarce.** Big buffers in static memory can exhaust internal SRAM at load — keep
  arrays modest inside the 64 KB linear memory. [[ram-static-bss-boot-crash]].

## Audio — and the golden rule: SFX and VOICE must never overlap
Three engines share the codec: `nv_gfx_tone` (synth), `nv_sound` (WAV), `nv_speak` (TTS). Voice
preempts SFX/tones. To make nothing step on anything, run a per-app **audio timeline clock**:
- Keep `int g_speak_until` = ms when the current audio ends. Every play advances it.
- A wrapper `say(text)` estimates duration (`~150 + letters*72 + spaces*150` ms) and sets it.
- A wrapper `schedule_sfx(name,dur)` reserves `[now..now+dur]` on the clock and fires the `nv_sound`
  from the loop when its slot arrives; the reward voice is scheduled at the SAME clock -> plays AFTER.
- The round/state advance waits until BOTH the pending SFX and voice have fired and finished.
See abc123 `schedule_sfx`/`schedule_vfx`/`g_speak_until`. This is app-side (no firmware change).

**Beautiful SFX**: generate polyphonic WAVs offline — **`tools/gen_abc_sfx.py`** (numpy additive synth,
inharmonic bell/celesta partials, chords, light reverb, soft-clip; 48 kHz **mono 16-bit** — the exact
format `nv_sound` streams). Keep them SHORT (~0.3–0.5 s) so they don't drag gameplay.

## Offline voice (nv_speak) — pack gotchas (learned the hard way)
Pack lives at `/sdcard/data/tts/<lang>/{index.bin,clips.pcm}` (NTI1, mono 16-bit @24 kHz). See
[[tts-voice]] and [[usb-audio-tts-crash]]. Rules:
- **Numbers = WORDS, not digits.** Say "TRE", not "3". nv_tts does NOT expand digits.
- **Letters**: the corpus has `lett_a..lett_z`; but the tokenizer splits on `_` and an all-caps
  word ≥2 letters gets SPELLED. So alias each `lett_<x>` under a **single-char slug** `"<x>"` and
  pass the 1-char string (`tools/merge_local.py` does this). nv_tts folds accents (pass ASCII).
- **The device pack is a SUBSET** (built on PC from the 437 MB G: corpus via `tools/build_abc_voice.py`
  + `merge_local.py`). ~27 IT object nouns (ape/dado/ananas…) have NO audio in ANY corpus — restrict
  spoken games to voiceable items (abc123 `OBJ_VOICE_IT` mask). EN has all.
- **Push packs with curl, not urllib** (urllib reset mid-transfer and zeroed the pack). Verify sizes
  after: `/api/fs/list?path=/data/tts/it`. Test a word with `/api/say?text=MELA&lang=it` -> `0 unknown`.

## Persistence
`nv_save/nv_load` a fixed-size blob. To add a field without wiping old saves, **pack it into an
existing slot** (abc123 stores the voice on/off flag in the high byte of the language int, with a
sentinel so legacy saves default correctly). A size change invalidates old saves.

## Debug loop over Wi-Fi (no cable)
- Screenshot: `GET /api/screen` -> JPEG (use the **ui-screenshot** skill to also open/drive the app).
- Logs: `GET /api/logs` (grep your tag / `nv_tts` / `audio`). Say test: `GET /api/say?text=…&lang=it`.
- Launch (doesn't restart a running instance): `POST /api/app/run?id=<id>`.
- **Board Wi-Fi is flaky** — expect drops; the build_push script retries. If `/api/info` is silent but
  ping works, the httpd is mid-restart — wait and retry.

## Checklist before "done"
1. Compiles CLEAN with `-Wall -Wextra`. 2. Idle screens cost ~0 draw calls (frame-skip works).
3. Animations are time-bounded. 4. Audio sequenced (no SFX/voice overlap). 5. Spoken items are all
voiceable. 6. Pushed with curl; sizes verified. 7. Told the user to REOPEN the app.
