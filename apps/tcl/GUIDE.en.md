# Tcl

## What it is

Tcl is a scripting language where everything is a command and everything is a string: easy to learn, great for automation and small tools. This is **Jim Tcl**, a compact Tcl with lists, dicts, regular expressions, `proc`, lambdas, objects (`oo`), namespaces and files. You get the interactive prompt and `.tcl` file execution.

## How to use it

| Command | What it does |
|---|---|
| `tcl` | opens the interactive prompt (`.`) |
| `tcl script.tcl` | runs a file from the home folder |
| `tcl script.tcl a b` | passes arguments, read from `$argv` |
| `tcl -e 'expr {6 * 7}'` | runs one line |

At the prompt each line is a command and its result is shown; a `{` block left open continues on the next line:

```tcl
. expr {6 * 7}
42
. set names {anna bruno carla}
. lsort -decreasing $names
carla bruno anna
. proc square {x} { expr {$x * $x} }
. square 12
144
. dict set age anna 30
. dict get $age anna
30
. foreach i {1 2 3} { puts "line $i" }
```

Files:

```tcl
set f [open /note.txt w]; puts $f "hello from Tcl"; close $f
set f [open /note.txt]; puts [gets $f]; close $f
```

To quit: `exit`, or **EOF**.

## Limits

No external processes (`exec`), sockets, signals or event loop (`after`, `vwait`). Integers are 64-bit (no huge numbers: use `bc` for those). Memory: up to 8 MB.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
