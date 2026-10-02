# zstd (with gzip and xz)

## What it is for

It compresses and decompresses files in three formats: Zstandard (`.zst`, fast and small), gzip (`.gz`, the most common) and xz (`.xz`, used by Linux packages). The same program answers to the classic commands: `gzip`, `gunzip`, `zcat`, `xz`, `unxz`, `xzcat`, `zstd`, `unzstd`, `zstdcat`.

## How to use it

| Command | What it does |
|---|---|
| `gunzip data.csv.gz` | decompresses (and drops the `.gz`); `-k` keeps the original too |
| `zcat log.gz \| grep error` | reads a compressed file without unpacking it |
| `gzip -k notes.txt` | makes `notes.txt.gz`, keeping the original |
| `unxz archive.xz` / `xzcat file.xz` | the same for `.xz` |
| `xz -k photo.bmp` | compresses to `.xz` |
| `zstd -19 backup.sqlite` | maximum compression to `.zst` |
| `zstd -d backup.sqlite.zst` | decompresses |
| `zstd --list file.zst` | sizes and compression ratio |

## With Anima

It is a system program: Anima uses it by itself, for example to open a downloaded `.gz` file. You can ask "unpack the file I downloaded".

## Limits

There is no `tar`: a `.tar.gz` becomes a `.tar` and is not extracted into folders (`.zip` archives have `zip`/`unzip`). xz compression is capped at level 1 to fit an app's memory (an `.xz` made elsewhere still opens). No multi-threaded compression.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
