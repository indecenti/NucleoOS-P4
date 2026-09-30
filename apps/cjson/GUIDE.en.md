# JSON

## What it is

Checks whether a JSON file is valid, pretty-prints it, compacts it onto one line or pulls out a single value. Handy for inspecting configs, API replies and logs.

## How to use it

| Command | What it does |
|---|---|
| `cjson file.json` | validates and pretty-prints |
| `cjson -c file.json` | prints compact, on one line |
| `cjson -q .user.name file.json` | extracts the value at that path |
| `cjson -q .list.0 file.json` | first element of an array |
| `cjson -l file.jsonl` | one JSON value per line (JSON Lines) |
| `cjson` | reads input: paste the JSON, then **EOF** |

If the file is invalid, `cjson` reports an error.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
