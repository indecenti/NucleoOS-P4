# ZenFactor Spa

## The game

A short tutorial adventure: visit a company in Turin to find out what text adventures are about.

- **Author:** Tristano Ajmone (2010)
- **Game language:** Italian
- **Format:** Z-machine v8, Frotz 2.55 interpreter

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
on the SD card (`/sdcard/apps/if-zenfactor-spa/data`). `RESTORE` loads it back, even after closing the
Terminal. `QUIT` leaves the game.

## In the Terminal

- **Tap the icon** to open the Terminal with the game already running, or open **Terminal** and type `if-zenfactor-spa`.
- Type in the bottom line and press Enter: the line goes to the game.
- **STOP** kills the game right away (without saving).

## Credits and licence

**ZenFactor Spa** © Tristano Ajmone, 2010. Licence: **Public domain**.
Licence text: <https://ifarchive.org/indexes/if-archive/games/zcode/italian/>
Game source: <https://ifdb.org/viewgame?id=kj5hyq3wkvl8x8yf>

> ZenFactor Spa, by Tristano Ajmone. Public domain. (IF Archive index, games/zcode/italian)

Interpreter: **Frotz 2.55**, GPL-2.0-or-later licence (<https://gitlab.com/DavidGriffith/frotz>). Game and interpreter are redistributed unmodified; NucleoOS only adds the launcher and access to the embedded story file.
