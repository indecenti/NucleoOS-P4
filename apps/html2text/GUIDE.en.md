# html2text

## What it is for

It turns a web page or an HTML file into plain readable text: headings, paragraphs, lists and tables stay, scripts, styles and menus go. With `curl` it reads pages straight from the internet.

## How to use it

| Command | What it does |
|---|---|
| `curl -s https://example.org \| html2text` | a web page as text |
| `curl -s URL \| html2text \| less` | to page through it |
| `html2text page.html` | an HTML file in the home folder |
| `html2text -links page.html` | the link targets listed at the end |
| `html2text -width 60 page.html` | wraps at 60 columns |
| `curl -s URL \| html2text > page.txt` | saves the text to a file |

## With Anima

It is a system program: Anima uses it by itself when an exact answer is needed. You can also ask for it, for example "read me the news on this page".

## Limits

Pages built by JavaScript (many modern sites) come out nearly empty: the browser makes that text, it is not in the HTML. Pages that declare no encoding are read as UTF-8.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
