# Villa Morgana

## The game

Locked in an old mansion, looking for a friend: an introduction to text adventures with a step-by-step guide.

- **Author:** Paolo Lucchesi (2006)
- **Game language:** Italian
- **Format:** Z-machine (Blorb), Frotz 2.55 interpreter

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
on the SD card (`/sdcard/apps/if-villa-morgana/data`). `RESTORE` loads it back, even after closing the
Terminal. `QUIT` leaves the game.

## In the Terminal

- **Tap the icon** to open the Terminal with the game already running, or open **Terminal** and type `if-villa-morgana`.
- Type in the bottom line and press Enter: the line goes to the game.
- **STOP** kills the game right away (without saving).

## Credits and licence

**Villa Morgana** © Paolo Lucchesi, 2006. Licence: **GPL-2.0**.
Licence text: <https://ifarchive.org/indexes/if-archive/games/zcode/italian/>
Game source: <https://ifdb.org/viewgame?id=57wq8ncspbsdktg2>

> I file villa.zblorb e villa.gblorb sono distribuiti secondo i termini della Gnu Public License (GPL) versione 2 (README of the IF Archive release)

Interpreter: **Frotz 2.55**, GPL-2.0-or-later licence (<https://gitlab.com/DavidGriffith/frotz>). Game and interpreter are redistributed unmodified; NucleoOS only adds the launcher and access to the embedded story file.
