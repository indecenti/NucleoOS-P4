# Scheme

## What it is

Scheme is the Lisp dialect made for teaching programming: everything is an expression in parentheses, functions are values, recursion feels at home. This is **TinyScheme**, a small interpreter that starts fast and covers much of R5RS: lists, vectors, strings, characters, `let`, `lambda`, macros, continuations, file and string ports.

## How to use it

| Command | What it does |
|---|---|
| `scheme` | opens the interactive prompt (`ts>`) |
| `scheme program.scm` | runs a file from the home folder |
| `scheme program.scm a b` | passes arguments, read from `*args*` |
| `scheme -i program.scm` | runs the file, then stays at the prompt |
| `scheme -e '(display (* 6 7))'` | evaluates one expression |

At the prompt type an expression and press Enter; with parentheses left open the input continues on the next line:

```scheme
ts> (+ 1 2 3)
6
ts> (define (fact n) (if (< n 2) 1 (* n (fact (- n 1)))))
ts> (fact 20)
2432902008176640000
ts> (map (lambda (x) (* x x)) (list 1 2 3 4))
(1 4 9 16)
ts> (let loop ((i 0) (s 0)) (if (> i 100) s (loop (+ i 1) (+ s i))))
5050
ts> (load "/program.scm")
```

Files: `(define p (open-output-file "/data.txt")) (write '(1 2 3) p) (close-output-port p)`, then `(read (open-input-file "/data.txt"))`.

To quit: `(exit)`, or **EOF**. After `display` the prompt also shows `#t`: that's the returned value, not an error.

## Limits

Integers are 64-bit (no huge numbers: `(fact 25)` overflows; use `bc` for those) and there are no fractions (`1/3`). No `syntax-rules`: macros are written with `define-macro`. No R7RS libraries (`define-library`, `import`) or external processes. Memory: about 300,000 cells (lists up to a hundred thousand or so items), up to 12 MB in all.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
