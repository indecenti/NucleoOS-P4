# lowdown

## What it is for

It reads Markdown files (`.md`, like guides and READMEs) and shows them formatted in the Terminal, with headings, bold, lists and tables. It also converts them to HTML, man pages, LaTeX and OpenDocument (`.fodt`, opens in LibreOffice).

## How to use it

| Command | What it does |
|---|---|
| `lowdown -tterm notes.md` | the file formatted in the Terminal |
| `lowdown -tterm notes.md \| less` | to page through it |
| `lowdown -thtml -s notes.md -o notes.html` | a complete HTML page |
| `lowdown -s -tfodt notes.md -o notes.fodt` | a document for LibreOffice |
| `lowdown -tman page.md` | a man page |
| `cat notes.md \| lowdown -tterm` | reads from the input |

## With Anima

It is a system program: Anima uses it by itself when an exact answer is needed. You can also ask for it, for example "turn my notes into an HTML page".

## Limits

Images in documents become a text reference in the Terminal. Colours follow the Terminal theme.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
