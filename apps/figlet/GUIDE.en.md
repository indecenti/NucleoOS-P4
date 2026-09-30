# FIGlet

## What it is

FIGlet writes text in giant letters made of characters, the style of old terminal banners. Handy for titles, text file headers, funny messages. 15 fonts are built in.

## How to use it

| Command | What it does |
|---|---|
| `figlet Hello` | writes "Hello" in big letters with the standard font |
| `figlet -f slant NucleoOS` | uses another font |
| `figlet -c -f small centered` | centres the text on the line |
| `figlet -w 40 some long text` | wraps at 40 columns |
| `figlet --list` | lists the built-in fonts |
| `figlet` | writes every line you type in big letters, until **EOF** |

It works with the Terminal's shell: `echo Happy birthday | figlet -f big`, or `figlet Hello > banner.txt` to save it to a file.

Built-in fonts: standard, big, block, bubble, digital, lean, mini, script, shadow, slant, small, smscript, smshadow, smslant, term. Other `.flf` fonts (there are plenty online) work when you put them in the `figlet-fonts` folder of your home and type `figlet -f name text`, or give the file: `figlet -f /my/font.flf text`.

## Limits

The starting width is the Terminal's (the `COLUMNS` variable), otherwise 80 columns; `-t` is not available. Zip-compressed fonts don't open. Only Latin letters (ISO Latin-1) in the built-in fonts.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
