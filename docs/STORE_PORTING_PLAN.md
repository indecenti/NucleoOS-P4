# Store: app da portare con il minimo sforzo

Ricerca del 2026-09-30 (4 filoni: software per JC1060P470C/ESP32-P4, C/WASI portabile, emulatori +
homebrew, app da pannello). Solo cose ridistribuibili nello Store pubblico. GPL = OK pubblicando il
sorgente dell'app (il repo è pubblico: basta tenerlo in `apps/<id>/` o `ports/`). NC (CC BY-NC-*) =
OK finché lo Store è gratuito, come le cart WASM-4.

Nota tecnica: un'app compilata con `-Wasi` (wasi-libc: malloc, stdio, file) può usare anche il canvas
`nv.gfx_*` (manifest `canvas_w/h` + permesso `gfx`, entry reactor `run`). È il "ponte" che rende
portabile quasi tutto il C qui sotto.

## Onda 1 — ore / 1-2 giorni, catalogo grande

| App | Sorgente | Licenza | Modello | Contenuto incluso |
|---|---|---|---|---|
| **Avventure testuali** (Z-machine) | Bocfel (garglk/terps/bocfel) o MojoZork | MIT / zlib | WASI Terminale | **Zork I, II, III** (`COMPILED/zorkN.z3`, MIT Microsoft 2025) |
| **Glulx** (Inform 7) | glulxe + cheapglk (erkyrath) | MIT | WASI Terminale | storie IF Archive con licenza libera (per titolo) |
| **Rompicapi** (~40 giochi) | Simon Tatham's Puzzles | MIT | WASI + canvas (drawing_api -> gfx) | tutti inclusi |
| **Game Boy** | Peanut-GB (single header) | MIT | WASI + canvas + pad + PCM | Tobu Tobu Girl (MIT/CC BY), µCity (GPLv3/CC BY-SA), Libbet (zlib) |
| **CHIP-8/XO-CHIP** | da scrivere (poche centinaia di righe) | nostra | canvas | chip8Archive (licenza da verificare, probabilmente CC0) |
| Terminale extra | bc (Gavin Howard), 2048-cli, cowsay, sl, robotfindskitten, kilo | BSD/MIT/GPL | WASI | — |

## Onda 2 — pochi giorni

| App | Sorgente | Licenza | Note |
|---|---|---|---|
| **Casa** (Home Assistant) | nostra; UX ispirata a openHASP (MIT) — NON espcontrol (PolyForm NC) | nostra | `/api/template` per la lista, `subscribe_entities` per gli aggiornamenti, `call_service` |
| **Dispositivi locali** | nostra | nostra | Shelly `/rpc/…`, Tasmota `/cm?cmnd=`, WLED `/json/state`, ESPHome `web_server`, Zigbee2MQTT via mqtt |
| **Orologio / sveglia / Pomodoro / fusi** | nostra | — | offline |
| **Meteo+**: qualità aria, pollini, alba/luna | Open-Meteo AQ + calcolo SunCalc offline | CC BY 4.0, gratis solo non commerciale | estensione dell'app Meteo |
| **Cornice Immich** | API Immich (`search/random` + thumbnail JPEG) | — | anche come salvaschermo |
| **Doom** | doomgeneric (5 funzioni) | GPLv2 | con **Freedoom** (BSD); prestazioni da verificare in AOT |
| **NES** | core nofrendo di retro-go | GPLv2 | homebrew: Super Tilt Bro. (WTFPL), Thwaite (GPLv3) |
| **Gume/Braino!** 40 giochi educativi | shim TFT_eSPI -> gfx | GPLv3 | 320x240 scalato |

## Onda 3 — più avanti

Frontend libretro minimo (smsplus-gx SMS/GG, gambatte), Genesis (gwenesis, fps a rischio), radio web
(serve decoder MP3/AAC in streaming lato firmware), telecamere Frigate/go2rtc (snapshot JPEG), calendario
ICS, RSS, Pi-hole/AdGuard, Moonraker/OctoPrint, nethack.

## Da NON usare

fMSX e Snes9x (non commerciali), PicoDrive (licenza MAME NC), demo del produttore Guition (licenza non
chiara), espcontrol/espframe (PolyForm Noncommercial), raccolte retrobrews/EmuDeck (redistribuzione
solo sul loro sito), cart TIC-80/PICO-8 (licenze per autore), ROM/artwork Game & Watch, Spotify Web API
(dal 2026 inutilizzabile per un device), ViaggiaTreno (ToS non chiari), Transitous e CoinGecko Demo (NC).
