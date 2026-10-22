# Store: apps to port with minimal effort

Research from 2026-09-30 (4 tracks: software for JC1060P470C/ESP32-P4, portable C/WASI, emulators +
homebrew, panel apps). Only things that can be redistributed in the public Store. GPL = OK if the app's
source is published (the repo is public: just keep it in `apps/<id>/` or `ports/`). NC (CC BY-NC-*) =
OK as long as the Store is free, like the WASM-4 carts.

Technical note: an app compiled with `-Wasi` (wasi-libc: malloc, stdio, files) can also use the
`nv.gfx_*` canvas (manifest `canvas_w/h` + `gfx` permission, reactor entry `run`). This is the "bridge"
that makes almost all the C code below portable.

## Wave 1 — hours / 1-2 days, large catalog

| App | Source | License | Model | Included content |
|---|---|---|---|---|
| **Text adventures** (Z-machine) | Bocfel (garglk/terps/bocfel) or MojoZork | MIT / zlib | WASI Terminal | **Zork I, II, III** (`COMPILED/zorkN.z3`, MIT Microsoft 2025) |
| **Glulx** (Inform 7) | glulxe + cheapglk (erkyrath) | MIT | WASI Terminal | IF Archive stories with a free license (per title) |
| **Puzzles** (~40 games) | Simon Tatham's Puzzles | MIT | WASI + canvas (drawing_api -> gfx) | all included |
| **Game Boy** | Peanut-GB (single header) | MIT | WASI + canvas + pad + PCM | Tobu Tobu Girl (MIT/CC BY), µCity (GPLv3/CC BY-SA), Libbet (zlib) |
| **CHIP-8/XO-CHIP** | to be written (a few hundred lines) | ours | canvas | chip8Archive (license to be verified, probably CC0) |
| Extra terminal tools | bc (Gavin Howard), 2048-cli, cowsay, sl, robotfindskitten, kilo | BSD/MIT/GPL | WASI | — |

## Wave 2 — a few days

| App | Source | License | Notes |
|---|---|---|---|
| **Home** (Home Assistant; app `casa`) | ours; UX inspired by openHASP (MIT) — NOT espcontrol (PolyForm NC) | ours | `/api/template` for the list, `subscribe_entities` for updates, `call_service` |
| **Local devices** | ours | ours | Shelly `/rpc/…`, Tasmota `/cm?cmnd=`, WLED `/json/state`, ESPHome `web_server`, Zigbee2MQTT via mqtt |
| **Clock / alarm / Pomodoro / time zones** | ours | — | offline |
| **Weather+**: air quality, pollen, sunrise/moon | Open-Meteo AQ + offline SunCalc computation | CC BY 4.0, free for non-commercial use only | extension of the Weather app |
| **Immich frame** | Immich API (`search/random` + JPEG thumbnail) | — | also as a screensaver |
| **Doom** | doomgeneric (5 functions) | GPLv2 | with **Freedoom** (BSD); performance to be verified in AOT |
| **NES** | nofrendo core from retro-go | GPLv2 | homebrew: Super Tilt Bro. (WTFPL), Thwaite (GPLv3) |
| **Gume/Braino!** 40 educational games | TFT_eSPI shim -> gfx | GPLv3 | 320x240 scaled |

## Wave 3 — later

Minimal libretro frontend (smsplus-gx SMS/GG, gambatte), Genesis (gwenesis, fps at risk), web radio
(needs a streaming MP3/AAC decoder on the firmware side), Frigate/go2rtc cameras (JPEG snapshot), ICS
calendar, RSS, Pi-hole/AdGuard, Moonraker/OctoPrint, nethack.

## NOT to be used

fMSX and Snes9x (non-commercial), PicoDrive (MAME NC license), Guition manufacturer demos (unclear
license), espcontrol/espframe (PolyForm Noncommercial), retrobrews/EmuDeck collections (redistribution
only on their own site), TIC-80/PICO-8 carts (licenses per author), Game & Watch ROMs/artwork, Spotify
Web API (unusable for a device since 2026), ViaggiaTreno (unclear ToS), Transitous and CoinGecko Demo
(NC).
