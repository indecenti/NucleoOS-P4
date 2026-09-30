# Berry

## What it is

Berry is a small, fast scripting language made for microcontrollers (Tasmota uses it): classes, closures, exceptions, lists and maps, modules for strings, math, JSON and files. You get the interactive prompt and `.be` file execution.

## How to use it

| Command | What it does |
|---|---|
| `berry` | opens the interactive prompt (`>`) |
| `berry script.be` | runs a file from the home folder |
| `berry -i script.be` | runs the file, then stays at the prompt |
| `berry -e "print(6*7)"` | runs one line |

At the prompt type an expression or a statement and press Enter; a block left open (`def`, `for`, `if`...) continues on the next line with `>>`:

```berry
> 6 * 7
42
> def square(x) return x * x end
> square(12)
144
> import math
> math.sqrt(2)
1.41421
> try raise "oops", "something" except .. as e, m print(e, m) end
oops something
```

Files and JSON:

```berry
f = open("/notes.txt", "w") f.write("hello") f.close()
import json
print(json.dump({"a": [1, 2]}))
```

To quit: **EOF**, or `import os os.exit()`.

## Limits

No external processes: `os.system` answers -1. No loadable native modules (`.so`). Memory: up to 8 MB.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
