# RPN Calc

## What it is

A reverse Polish notation calculator in the HP tradition: numbers first, then the operation. No brackets.

## How to use it

Type a number and press ENTER: it goes on the stack. Type the second one and press an operation: it uses the two numbers on top. Example, (3 + 4) × 5: `3 ENTER 4 + 5 ×`.

- SWAP swaps x and y, DROP removes x, UNDO reverts the last operation, CLR clears everything, DEL deletes the last digit.
- STO and RCL store and recall the memory; DEG/RAD switches the angle unit.
- Tap the stack on the left to switch to asin, acos and atan.

The stack and memory are kept when you leave. Keyboard: digits, Enter, + - * / ^, Backspace, Esc to clear.

## License

NucleoOS, MIT license.
