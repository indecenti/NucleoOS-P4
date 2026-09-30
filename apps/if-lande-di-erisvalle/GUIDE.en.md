# Le Lande di Erisvalle

## The game

Explore the far lands of Erisvalle in search of seven relics of an ancient kingdom and seven runes of power.

- **Author:** Paolo Lucchesi (2022)
- **Game language:** Italian
- **Format:** Glulx, Glulxe + CheapGlk interpreter

## How to play

This is a **text adventure** (interactive fiction): the game tells you where you are, you type what to
do in short sentences, in Italian, and press Enter. Start by looking around.

| Command | Short | What it does |
|---|---|---|
| `GUARDA` | `G / L` | describe where you are |
| `INVENTARIO` | `I` | what you are carrying |
| `NORD, SUD, EST, OVEST, SU, GIÙ` | `N S E O ALTO BASSO` | move |
| `ESAMINA lampada` | `X / ESA` | look closely at something |
| `PRENDI lampada / LASCIA lampada` |  | pick up / put down |
| `APRI porta, PARLA CON uomo` |  | act on things and people |
| `SALVA / CARICA` | `SAVE / RESTORE` | save / load the game |
| `FINE` | `QUIT` | leave the game |

Tips: examine everything (`EXAMINE` / `X`), take what you can, try the exits the descriptions
mention, and if you are stuck read the room again with `LOOK`. Many games understand `HELP` or `HINT`.
`UNDO` takes back one move.

## Saving

`SAVE` asks for a file name (Enter accepts the suggested one) and writes it to the app's data folder
on the SD card (`/sdcard/apps/if-lande-di-erisvalle/data`). `RESTORE` loads it back, even after closing the
Terminal. `QUIT` leaves the game.

## In the Terminal

- **Tap the icon** to open the Terminal with the game already running, or open **Terminal** and type `if-lande-di-erisvalle`.
- Type in the bottom line and press Enter: the line goes to the game.
- **STOP** kills the game right away (without saving).

## Credits and licence

**Le Lande di Erisvalle** © Paolo Lucchesi, 2022. Licence: **GPL-2.0**.
Licence text: <http://www.paololucchesi.it/at/erisvalle.html>
Game source: <https://ifdb.org/viewgame?id=1jkbbgyvk7gyg0v>

> I file erisvalle.gblorb, erisvalle_nomouse.gblorb e erisvalle_acv.ulx sono distribuiti secondo i termini della Gnu Public License (GPL) vers. 2 (README of the release zip)

Interpreter: **Glulxe + CheapGlk**, MIT licence (<https://github.com/erkyrath/glulxe>). Game and interpreter are redistributed unmodified; NucleoOS only adds the launcher and access to the embedded story file.
