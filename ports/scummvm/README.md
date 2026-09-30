# ScummVM for NucleoOS

ScummVM 2.9.1 built as a WASI app (WAMR guest) with a NucleoOS backend: `apps/scummvm` (the engine,
with its own launcher) and one store package per freeware game (`apps/svm-*`), which runs the same
module through the manifest field `"engine": "scummvm"`.

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
bash ports/scummvm/build.sh             # WSL: apps/scummvm/app.wasm (~4.5 MB)
AOT=1 bash ports/scummvm/build.sh       # + app.aot for the P4 (~15 MB, wamrc takes ~40 min)
python ports/scummvm/gen_games.py       # Windows or WSL: backend/nucleo-games.h + apps/svm-*/
```

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
repackaged. `ram_mb` overrides the 12 MB default (Drascula needs 16).

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
