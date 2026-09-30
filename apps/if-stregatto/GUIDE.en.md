# Salvate lo Stregatto!

## The game

The Cheshire Cat, mascot of a games club, is missing: find it before it becomes a fur coat. Short comedy.

- **Author:** Marco Vallarino (2013)
- **Game language:** Italian
- **Format:** Z-machine v5, Frotz 2.55 interpreter

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
on the SD card (`/sdcard/apps/if-stregatto/data`). `RESTORE` loads it back, even after closing the
Terminal. `QUIT` leaves the game.

## In the Terminal

- **Tap the icon** to open the Terminal with the game already running, or open **Terminal** and type `if-stregatto`.
- Type in the bottom line and press Enter: the line goes to the game.
- **STOP** kills the game right away (without saving).

## Credits and licence

**Salvate lo Stregatto!** © Marco Vallarino, 2013. Licence: **CC BY-SA 4.0 (source code licence)**.
Licence text: <https://ifarchive.org/indexes/if-archive/games/zcode/italian/>
Game source: <https://ifdb.org/viewgame?id=1elq80q2fb91e9dd>

> Il file Stregatto.inf è il listato del programma, scritto in Inform 6, rilasciato con licenza CC-BY-SA 4.0 (Stregatto_leggimi.txt; the story file is its compiled form)

Interpreter: **Frotz 2.55**, GPL-2.0-or-later licence (<https://gitlab.com/DavidGriffith/frotz>). Game and interpreter are redistributed unmodified; NucleoOS only adds the launcher and access to the embedded story file.
