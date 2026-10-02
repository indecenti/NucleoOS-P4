# PDF tools (PDFio)

## What it is for

It reads PDFs from the Terminal: extracts the text of every page, shows title, author, dates and page count, and joins several PDFs into one. With the extracted text Anima can summarise a document, find an amount in a bill or answer questions about a manual.

## How to use it

| Command | What it does |
|---|---|
| `pdftotext bill.pdf` | the text of every page |
| `pdftotext manual.pdf \| less` | to page through it |
| `pdftotext manual.pdf \| grep -i warranty` | finds a word |
| `pdftotext doc.pdf > doc.txt` | saves the text to a file |
| `pdfinfo doc.pdf` | title, author, dates, pages and size |
| `pdfmerge -o all.pdf a.pdf b.pdf` | joins several PDFs |

## With Anima

It is a system program: ask for example "summarise the PDF I downloaded" or "how much do I owe on this bill?".

## Limits

Scanned PDFs are only images: there is no text to extract. Text comes out in the file's order, so columns and complex tables may come out mixed. No images or forms.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
