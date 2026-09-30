# Zip

## What it is

Creates, lists and extracts `.zip` archives right on the SD card. Useful to bundle files to take elsewhere or to open downloaded archives.

## How to use it

| Command | What it does |
|---|---|
| `zip archive.zip file1 file2` | creates the archive with those files |
| `zip -l archive.zip` | lists the contents |
| `zip -x archive.zip` | extracts into the current folder |
| `zip -x archive.zip folder` | extracts into `folder` |

Paths are relative to the home folder: `zip -x games.zip games` extracts into `/sdcard/home/games`.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
