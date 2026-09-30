# BASIC

## What it is

Classic line-numbered BASIC, like on the home computers of old: a good way to learn the basics of programming (variables, conditions, loops, jumps).

## How to use it

Write the program in a text file, e.g. `count.bas`, in the home folder:

```basic
10 print "how many numbers?"
20 input n
30 for i = 1 to n
40 print i
50 next i
60 end
```

Then in the Terminal:

```
basic count.bas
```

There is no immediate mode: the program is written in a file and run as a whole.

## Statements

`print`, `input`, `if ... then`, `goto`, `gosub` / `return`, `for` / `next`, `peek` / `poke`, `end`.

## Rules to remember

- Keywords and variables **lowercase only** (`print`, not `PRINT`).
- Variables: one letter `a` to `z`, integers only.
- Text can be printed (`print "hi"`) but not stored in a variable.
- `peek` and `poke` use a 1024-cell scratch array, not real memory.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
