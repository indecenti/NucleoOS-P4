# The Life (and Deaths) of Doctor M

## The game

You land in an endless, windy landscape and piece together the life of a mad scientist.

- **Author:** Michael D. Hilborn (2011)
- **Game language:** English
- **Format:** Glulx (Blorb), Glulxe + CheapGlk interpreter

## How to play

This is a **text adventure** (interactive fiction): the game tells you where you are, you type what to
do in short sentences, in English, and press Enter. Start by looking around.

| Command | Short | What it does |
|---|---|---|
| `LOOK` | `L` | describe where you are |
| `INVENTORY` | `I` | what you are carrying |
| `NORTH, SOUTH, EAST, WEST, UP, DOWN` | `N S E W U D` | move (also NE, NW, SE, SW, IN, OUT) |
| `EXAMINE lamp` | `X LAMP` | look closely at something |
| `TAKE lamp / DROP lamp` | `GET` | pick up / put down |
| `OPEN door, PUSH button, TALK TO man` |  | act on things and people |
| `AGAIN` | `G` | repeat the last command |
| `WAIT` | `Z` | let time pass |
| `SAVE / RESTORE` |  | save / load the game |
| `UNDO` |  | take back one move |
| `QUIT` | `Q` | leave the game |

Tips: examine everything (`EXAMINE` / `X`), take what you can, try the exits the descriptions
mention, and if you are stuck read the room again with `LOOK`. Many games understand `HELP` or `HINT`.
`UNDO` takes back one move.

## Saving

`SAVE` asks for a file name (Enter accepts the suggested one) and writes it to the app's data folder
on the SD card (`/sdcard/apps/if-doctor-m/data`). `RESTORE` loads it back, even after closing the
Terminal. `QUIT` leaves the game.

## In the Terminal

- **Tap the icon** to open the Terminal with the game already running, or open **Terminal** and type `if-doctor-m`.
- Type in the bottom line and press Enter: the line goes to the game.
- **STOP** kills the game right away (without saving).

## Credits and licence

**The Life (and Deaths) of Doctor M** © Michael D. Hilborn, 2011. Licence: **CC BY-NC-ND 3.0** (non-commercial use only).
Licence text: <https://creativecommons.org/licenses/by-nc-nd/3.0/>
Game source: <https://ifdb.org/viewgame?id=uqy4x2pm6cslbrs0>

> "The Life (and Deaths) of Doctor M" by Michael D. Hilborn is licensed under a Creative Commons Attribution-NonCommercial-NoDerivs 3.0 Unported License. (README.txt in the IF Archive release zip)

Interpreter: **Glulxe + CheapGlk**, MIT licence (<https://github.com/erkyrath/glulxe>). Game and interpreter are redistributed unmodified; NucleoOS only adds the launcher and access to the embedded story file.
