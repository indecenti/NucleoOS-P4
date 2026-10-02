# Eigenmath

## What it is for

Symbolic math: derivatives, integrals, polynomial roots, matrices, Taylor series and exact numbers of any length (fractions, radicals, huge integers). Where a calculator says 0.333, Eigenmath answers 1/3.

## How to use it

| Command | What it does |
|---|---|
| `eigenmath` | `?` prompt: one expression per line, **EOF** to leave |
| `eigenmath -e 'd(sin(x)^2,x)'` | derivative, result on one line |
| `eigenmath -e 'integral(x^2*exp(x),x)'` | indefinite integral |
| `eigenmath -e 'defint(x^2,x,0,1)'` | definite integral |
| `eigenmath -e 'roots(x^2-5x+6)'` | exact roots (`nroots` for numeric ones) |
| `eigenmath -e 'inv(((1,2),(3,4)))'` | inverse matrix (`det` for the determinant) |
| `eigenmath -e '212^17'` | exact integers of any size |
| `eigenmath sums.txt` | runs a file of expressions from the home folder |

More: `simplify`, `rationalize`, `float` (decimal value), `taylor(f,x,n)`, `sum(k,1,10,f)`, `product(k,1,10,f)`. Define functions: `f(x)=x^2+1`, then `f(3)`.

## With Anima

It is a system program: Anima uses it by itself when an exact answer is needed. You can also ask for it, for example "what is the integral of x^2 e^x?".

## Limits

No plots (`draw` does nothing). Results are always expanded: there is no factoring function. At the prompt results are drawn over several lines (fractions, matrices); with `-e` they stay on one line.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
