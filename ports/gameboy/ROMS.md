# Game Boy apps — emulator and bundled ROMs

The Game Boy apps are one front-end (`gb_frontend.c`) around third-party code. Everything below is
downloaded by `fetch.sh` into `ports/_src/gameboy` (not committed), pinned by sha256.

## Emulator

| Component | Author | License | Source (pinned) |
|---|---|---|---|
| Peanut-GB (`peanut_gb.h`) | Mahyar Koshkouei | MIT | https://github.com/deltabeard/Peanut-GB @ `d0bcca771c83a2638c93a9ae61f3d51f226dc905` |
| MiniGB APU (`minigb_apu.c/.h`) | Alex Baines, Mahyar Koshkouei | MIT | same commit, `examples/sdl2/minigb_apu/` (based on https://github.com/baines/MiniGBS) |

## Bundled games (one app each)

Only ROMs whose license explicitly allows redistribution. Checked on the upstream repository (the
LICENSE file and README) on 2026-09-30.

| App | Game | Author | License | Source code | ROM file (pinned) |
|---|---|---|---|---|---|
| `gbtobu` | Tobu Tobu Girl (2017) | Tangram Games (Simon Larsen and team; sound by potato-tan) | code **MIT**; graphics, text, sound and music **CC BY 4.0** | https://github.com/SimonLarsen/tobutobugirl | `tobu.gb` from the Homebrew Hub database, https://github.com/gbdev/database @ `50293559a496a3e20382fbf6a2e84b70ec622f88` `entries/tobutobugirl/tobu.gb` (the official binary is on https://tangramgames.itch.io/tobutobugirl) |
| `gb2048` | 2048-gb (2014) | Sanqui; tile graphics by beware | **zlib** | https://github.com/Sanqui/2048-gb | `2048.gb`, gbdev/database @ same commit, `entries/2048gb/2048.gb` |
| `gblibbet` | Libbet and the Magic Floor v0.08 | Damian Yerrick (pinobatch); portions (c) Martin Korth | **zlib** | https://github.com/pinobatch/libbet | https://github.com/pinobatch/libbet/releases/download/v0.08/libbet.gb |

Attribution requirements:
- **Tobu Tobu Girl** — CC BY 4.0 needs credit: "Tobu Tobu Girl by Tangram Games, assets licensed
  CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/)". It is in the app description, author
  and license fields of `apps/gbtobu/manifest.json`; the game's own intro also shows the credits.
  The MIT notice ships with the source (repository link in the manifest `source`).
- **2048-gb**, **Libbet** — zlib: no attribution required, origin not misrepresented; credited in
  the manifests anyway. The ROMs are unmodified.

## Considered and not bundled

- **µCity** (AntonioND, GPLv3+ / CC BY-SA 4.0, https://github.com/AntonioND/ucity): the license
  allows it, but the game is **Game Boy Color only** (it needs the 32 KB CGB work RAM, its README says
  a monochrome port is impossible) and Peanut-GB emulates only the original DMG. Revisit if the core
  gains CGB support.

## Generic player

`apps/gameboy` bundles no ROM: it lists the `.gb` files the user copies to `/sdcard/home/roms`
(permission `home`) and writes battery saves next to them (`<name>.sav`).
