# JavaScript

## What it is

Modern JavaScript (ES2024) on the QuickJS-ng engine: interactive prompt, scripts and ES modules, file I/O through the `std` and `os` modules, timers.

## How to use it

| Command | What it does |
|---|---|
| `js` | opens the interactive prompt (`>`; `...` when the line continues) |
| `js file.js` | runs a script from the home folder |
| `js -m file.js` | runs it as an ES module (automatic for `.mjs`) |
| `js -e "code"` | evaluates the code and exits |
| `js -h` | shows the help |

```js
> [1, 2, 3].map(x => x * 2)
[ 2, 4, 6 ]
> setTimeout(() => print("done"), 1000)
```

Open brackets continue on the next line. Timers run before the next prompt. To quit: **EOF**.

## Limits

The prompt is line based: no arrow-key history inside the program and no Tab completion. Memory: up to 8 MB.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
