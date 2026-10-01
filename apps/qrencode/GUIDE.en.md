# qrencode

## What it is for

It turns text into a QR code drawn right in the Terminal, to scan with a phone: a link to open, a number, a note, or your Wi-Fi network so guests join without typing the password. The code can also be saved as an SVG picture.

## How to use it

| Command | What it does |
|---|---|
| `qrencode https://example.org` | draws the link's QR code in the Terminal |
| `qrencode 'WIFI:S:NetworkName;T:WPA;P:password;;'` | QR code to join a Wi-Fi network |
| `qrencode -o code.svg 'text'` | saves the QR code to `code.svg` in the home folder |
| `qrencode -l H 'text'` | more error correction (L, M, Q, H): still reads when damaged |
| `echo text \| qrencode` | takes the text from the input |
| `qrencode -t ascii 'text'` | made of `#`, to paste where the blocks don't show |

With the shell: `cat notes.txt | qrencode` puts a whole file in the code (up to about 2900 characters).

## Limits

No PNG: for a picture use `-o name.svg` (also `.eps` and `.xpm`). Long text makes a code bigger than the Terminal: keep it short, or save it as SVG. The on-screen code is white on black: if the phone struggles, move it back a little or raise the brightness.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
