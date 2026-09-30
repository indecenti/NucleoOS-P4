# Lua

## What it is

Lua 5.4 is a small, fast scripting language, popular in games and automation. You get both the interactive prompt, to try things out, and `.lua` file execution.

## How to use it

| Command | What it does |
|---|---|
| `lua` | opens the interactive prompt (`>`) |
| `lua script.lua` | runs a file from the home folder |
| `lua script.lua a b` | passes arguments, read as `arg[1]`, `arg[2]` |

At the prompt type an expression and press Enter:

```lua
> 2^10
1024.0
> for i = 1, 3 do print(i) end
```

To quit: **EOF**, or `os.exit()`.

## Limits

There are no external processes: `os.execute`, `io.popen`, `os.tmpname` and `io.tmpfile` answer "not supported". Memory: up to 8 MB.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
