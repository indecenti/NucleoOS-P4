# Wren

## What it is

Wren is a small, fast, tidy class-based scripting language (a bit Smalltalk, a bit Lua, with a C-like syntax). It has fibers, closures, lists, maps and ranges. You get the interactive prompt and `.wren` file execution.

## How to use it

| Command | What it does |
|---|---|
| `wren` | opens the interactive prompt (`>`) |
| `wren script.wren` | runs a file from the home folder |
| `wren script.wren a b` | passes arguments, read with `Process.arguments` |
| `wren -e 'System.print(6 * 7)'` | runs one line |

At the prompt an expression shows its value; leave a `(`, `[` or `{` open and the input continues with `...`:

```wren
> 1 + 2 * 3
7
> var list = [1, 2, 3, 4]
> list.map {|n| n * n }.toList
[1, 4, 9, 16]
> System.print("two plus two is %(2 + 2)")
two plus two is 4
> class Dog {
...   construct new(name) { _name = name }
...   bark() { "%(_name): woof!" }
... }
> Dog.new("Rex").bark()
Rex: woof!
```

The `io` module reads and writes files and reads input:

```wren
import "io" for File, Stdin
File.write("/note.txt", "hello from Wren")
System.print(File.read("/note.txt"))
var line = Stdin.readLine()
```

In scripts `import "name"` loads `name.wren` from the same folder. The `random` and `meta` modules are there too. To quit: **EOF**.

## Limits

This is not the official `wren_cli` (which needs libuv): no timers, network, processes or `os` module. Of the `io` module there are only `File.read`, `File.write`, `File.exists`, `Stdin.readLine` and `Process.arguments`. Memory: up to 8 MB.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
