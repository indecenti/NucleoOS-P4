# WordGrinder, word processor

## What it is for

WordGrinder is a program for **writing**: letters, reports, notes, chapters. It keeps you on the text, without distractions, yet has what you need: headings, paragraphs, bulleted and numbered lists, **bold**, *italic*, underline, find and replace, word count, undo and redo, several documents in one file. It opens and exports **OpenDocument `.odt`** (LibreOffice; Word opens it too), **Markdown**, **HTML** and plain text; it also exports LaTeX, troff and Org.

## How to open it

- **Tap the icon** on Home: an empty document opens.
- In **Files**, tap an `.odt`, `.md`, `.html` or `.txt` file and choose **Open with › WordGrinder**.
- In the Terminal: `wordgrinder letter.odt`.

An `.odt`, `.md`, `.html` or `.txt` file is **imported**: when you save, WordGrinder writes a `.wg` file next to it (its own format) and the original is left alone. To write it back as `.odt` use **ESC › File › Export**.

## Touch and keyboard

- **Tap the text** to move the cursor there. **Drag up and down** to scroll.
- **ESC** (in the key row above the keyboard) opens the **menu**: File, Edit, Style, Documents, Navigation. Every item has a letter, and the most used commands have a Ctrl shortcut.
- A USB or KeyDeck keyboard also gives you Shift+arrows (selection) and Ctrl+arrows (word by word).

| Keys | What they do |
|---|---|
| ESC | menu |
| Ctrl+S | save |
| Ctrl+O | open a document |
| Ctrl+B / Ctrl+U | bold / underline (italic: ESC › Style › Set italic) |
| Ctrl+N | normal text |
| Ctrl+P | paragraph style: headings H1–H4, lists, quotes |
| Ctrl+Z / Ctrl+Y | undo / redo |
| Ctrl+X / Ctrl+C / Ctrl+V | cut / copy / paste |
| Ctrl+F / Ctrl+K | find and replace / find next |
| Ctrl+G | go to a page or a heading |
| Ctrl+Q | quit |

## With Anima

WordGrinder is a system program: Anima uses it to read an `.odt` or to turn notes into a document. Ask for example "read me report.odt" or "turn notes.md into an odt document".

## Limits

- Word `.docx` and `.doc` files do not open: save them as `.odt`.
- No pictures, tables, columns or page layout: it is a program for text. Pictures in an imported `.odt` are lost.
- The spell checker has no dictionaries installed.
- The clipboard is internal to the program.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- Convert without opening: `wordgrinder --convert letter.odt letter.txt` (the extension picks the format: `.odt` `.md` `.html` `.txt` `.wg`, out also `.tex`).
- `apps` lists the installed terminal programs, `help wordgrinder` its syntax.
