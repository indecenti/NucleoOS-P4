# ScummVM for NucleoOS

ScummVM 2.9.1 built as a WASI app (WAMR guest) with a NucleoOS backend: one small module per engine,
shipped as a store library `apps/scummvm-<engine>` (`"kind": "library"`, no Home tile, installed with
the first game that requires it), and one store package per freeware game (`apps/svm-*`) that runs its
engine's module through `"engine": "scummvm-<engine>"`. One engine per module keeps the riscv32 AOT
image at ~8.5-9.5 MB; with all eight it was 16 MB and did not fit next to a game's memory in PSRAM.
`apps/scummvm` (every engine, own launcher) is kept for development, hidden in the store.

**Licence: this whole directory is GPL-3.0-or-later (`COPYING`)**, like ScummVM itself; the rest of
the repository keeps its own licence. The app binary is built only from ScummVM 2.9.1 (upstream
tarball, pinned), the files here and the libraries in `deps.sh`.

## Engines

The eight engines of the freeware games scummvm.org hosts: `sky` (Beneath a Steel Sky), `queen`
(Flight of the Amazon Queen), `lure`, `drascula`, `dreamweb`, `cge` (Sołtys), `cge2` (Sfinx),
`parallaction` (Nippon Safes). 320x200 only (`--disable-highres`). Audio: Ogg Vorbis (Tremor), MP3
(libmad), zlib; no FLAC, no MT-32 / FluidSynth (AdLib is emulated).

## Build

```bash
bash ports/scummvm/build_engines.sh     # WSL: apps/scummvm-<engine>/app.wasm + app.aot, all eight
bash ports/scummvm/build_engines.sh sky # one engine (wamrc ~20 min per AOT image)
python ports/scummvm/gen_games.py       # backend/nucleo-games.h + apps/svm-*/ + apps/scummvm-*/manifest
```

Each module is `-Os` with a fixed linear memory (`--initial-memory` = `--max-memory` = MEM_MB, 10 MB,
Drascula 13): firmware 1.1.141 reads the app.aot into a block that size and hands it over as the
memory, so a relaunch never has to find a fresh 10 MB contiguous block. `ram_mb` in games.json must
match. Packages require `"wasi": "1.2"` (firmware 1.1.141+).

`build.sh` needs the wasi-sdk 34 Linux toolchain in `/opt/wasi-sdk-34.0-x86_64-linux` (its clang,
wasm-ld and libc++) and `ports/_src/scummvm-2.9.1.tar.gz` (github.com/scummvm/scummvm tag v2.9.1,
sha256 `934762207a78193cd7dada4947f1cebe06575db452b3cd0f247b229a7d8b4c1f`). It unpacks it into
`~/svm`, copies `backend/` to `backends/platform/nucleo`, patches `configure` (`patch_configure.py`:
a `wasm32-wasi` host that is POSIX but not emscripten, the `nucleo` backend) and builds out of tree
in `~/svm/build-<app>`. `deps.sh` cross-builds zlib, libogg, Tremor and libmad into `~/svm/prefix`.

## Backend (`backend/`)

| file | what |
|---|---|
| `nucleo.cpp` | `OSystem`: timers + mixer pumped from `pollEvent`/`delayMillis` (no threads), 22 kHz stereo into `nv_audio_*`, input, `main` |
| `nucleo-graphics.*` | 320x200 RGB565 canvas (`canvas_scale: fit` = 3x on the panel): game screen (CLUT8/RGB565), overlay, cursor |
| `nucleo-installer.*` | game packages: download the original archives (`nv_http_req`, 960 KB ranges, resumable, SHA-256), stream-unzip with zlib, progress UI |
| `nucleo-games.h` | generated from `games.json`: URLs, sizes, hashes, variants |
| `nucleo-keymap.h` | USB HID usage → ScummVM key (US layout) |
| `nucleo-imports.h` | the `nv` host imports used |

Input, all at once, each as soon as it is connected: touch (tap = left click, long press or
two-finger tap = right click, drag, three-finger tap = Esc, Back gesture = ScummVM menu), keyboard
and mouse (ABI 14), game controller (ABI 11).

Files: the standalone app has `home` + `fs` (`/` = `/sdcard/home`, games in `/ScummVM`, config and
saves in `/appdata`). A game package has `fs` only: `/` is its own folder, with `variant` (written by
the store page's language picker), `game/`, `saves/`, `scummvm.ini`.

## Game packages (`games.json`)

One entry per game, one variant per language (max 6, store spec: id `^[a-z0-9_-]{1,8}$`, name ≤ 20
Latin-1 characters). `lang` is ScummVM's `--language` for multi-language data (BASS subtitles,
Drascula), empty for single-language archives. Only archives scummvm.org distributes as freeware
with a redistribution licence are listed; they are fetched unmodified at first start, never
repackaged. `engine` names the module, `ram_mb` its memory (Drascula needs 13; 12 ran out).

## Testing on the PC

`host/svmhost.c` is WAMR (the firmware's feature set) with the gfx/touch/pad/audio/http/kbd/mouse
imports, headless, driven by a script (`tap`, `hold`, `drag`, `key`, `mouse`, `back`, `shot`,
`quit`): `bash ports/scummvm/host/build.sh`, then e.g.

```bash
/root/svmhost --dir=/::/root/g-lure --mem=12 --script=s.txt --out=out \
    ~/svm/build-scummvm/scummvm --nucleo-game=lure
```

Tested on the PC host: all eight games install (one variant each) and start; BASS gameplay with
keyboard Esc, touch walk and mouse.
