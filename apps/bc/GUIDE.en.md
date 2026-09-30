# bc

## What it is

bc is the Unix arbitrary-precision calculator: numbers can have as many digits as you like, before and after the point. It has variables, `if`, loops, your own functions and, with `-l`, a math library (sine, cosine, arctangent, logarithm, exponential). This is Gavin Howard's version.

## How to use it

| Command | What it does |
|---|---|
| `bc` | opens the interactive calculator (`>>>`) |
| `bc -l` | with the math library and 20 decimals |
| `bc sums.bc` | runs a file from the home folder, then stays interactive |
| `echo "2^64" \| bc` | quick calculation through a shell pipe |

Type an expression and press Enter:

```bc
>>> 2^200
1606938044258990275541962092341162602522202993782792835301376
>>> scale = 30
>>> 1/7
.142857142857142857142857142857
>>> sqrt(2)
1.414213562373095048801688724209
>>> obase = 16
>>> 255
FF
```

With `bc -l`: `4*a(1)` is pi, `s(x)` sine, `c(x)` cosine, `l(x)` natural log, `e(x)` exponential. A function of your own:

```bc
define fact(n) {
  if (n < 2) return 1
  return n * fact(n - 1)
}
fact(50)
```

To quit: `quit`, or **EOF**.

## Limits

An error at the prompt (e.g. `1/0`) is reported and you carry on; an error inside a file stops it. No history or line editor. Memory: up to 8 MB.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
