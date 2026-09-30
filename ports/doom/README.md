# Doom for NucleoOS

The Doom engine as a store app (`apps/doom`, "Doom Engine") plus the Doom games published on the
store as packages that run it (`"engine": "doom"` in their manifest, firmware ABI 14).

## Pieces

| File | What it is |
|---|---|
| `fetch.sh` | pinned sources: doomgeneric, Chocolate Doom's OPL music player, rp2040-doom's emu8950, Freedoom, the community WADs |
| `patch_sources.py` | copies them into `ports/_src/doom/build` and applies the NucleoOS edits (analog sticks, config folder, bigger vanilla limits, missing textures no longer fatal, headers for the OPL player) |
| `nv_doom.c` | front-end: 320x200 → 320x240 (4:3) RGB565 through a palette LUT, the OS PPA-scales it to 800x600; keyboard (ABI 14 `nv_kbd_state`), mouse (`nv_mouse_read`), gamepads (ABI 11, analog), touch; the launcher and the store-game downloader |
| `nv_sound.c` | 16-voice SFX mixer + OPL music on one 22050 Hz stereo PCM stream (ABI 10) |
| `opl_nv.c` | Chocolate Doom's `opl.h` on emu8950 (OPL2 at 49716 Hz, box-filtered to the output rate), single-threaded |
| `games.py` | the store games: hosted files, `<id>.game` descriptors, manifests, guides, catalog entries |
| `make_icons.py` | icons from each game's title screen (captured by the harness) |
| `host/harness.c` | headless PC harness (WSL gcc): boots a game, plays demos or warps to a map, dumps frames, measures CPU |

## Build and test

```
bash ports/doom/build.sh            # apps/doom/app.wasm + app.aot, then a 2500-frame harness run
bash ports/doom/build.sh games      # every store game: warp to two maps, report errors + CPU/frame
python ports/doom/games.py data <store-checkout>/data/doom   # the hosted WADs + descriptors
python ports/doom/games.py packages                          # apps/<id>/manifest.json + guides
python ports/doom/make_icons.py ports/_src/doom/test         # icons (after `build.sh games`)
```

## How a store game starts

`NUCLEO_APP` is the game's id. The engine fetches `<store>/data/doom/<id>.game` (cached in the
game's private folder for offline starts), downloads every missing file into `/home/doom` in 1 MB
ranges (resumable `.part`, SHA-1 checked; Freedoom is shared by every game built on it), then
starts Doom with `-iwad` and `-file` from the descriptor. Saves and settings stay in the game's
own folder.
