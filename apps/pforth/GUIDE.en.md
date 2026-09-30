# pForth

## What it is

Forth is a stack language: numbers go on a stack and "words" consume them, e.g. `2 3 +` leaves `5`. You define new words with `:` and `;` and try everything on the fly. pForth is a portable ANS Forth with floating point, strings, variables, loops and words to read and write files.

## How to use it

| Command | What it does |
|---|---|
| `pforth` | opens the interactive prompt |
| `pforth -q` | same, without the banner and the `ok` after each line |
| `pforth program.fth` | runs a file from the home folder and exits |

At the prompt type words separated by spaces and press Enter; after each line pForth answers `ok` and shows the stack (`Stack<10>` means base 10, empty stack):

```forth
2 3 + .                            \ prints 5
: SQUARE ( n -- n*n ) DUP * ;      \ a new word
12 SQUARE .                        \ prints 144
10 0 DO I . LOOP                   \ prints 0 1 2 ... 9
VARIABLE COUNTER  5 COUNTER !  COUNTER @ 2 * .
2.5e0 3.0e0 F* F.                  \ prints 7.500000
```

To load a file and stay: `include program.fth`. To list the available words: `words`. To quit: `bye`, or **EOF**.

## Limits

Cells are 32-bit. No line editor or history (the Terminal sends whole lines). Division by zero gives the error `THROW code = -10` instead of killing the program. `RESIZE-FILE` cannot shrink files. Memory: up to 4 MB.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
