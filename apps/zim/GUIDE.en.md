# zim

## What it is for

`zim` reads the ZIM files of the Kiwix library: Wikipedia, Wiktionary, Wikivoyage, the medical encyclopedia, iFixit's repair guides, programming language documentation (DevDocs), Gutenberg's books and much more, with no Internet. The catalog has thousands of them, in every language, updated every month.

An article comes out as clean text: title, paragraphs, lists, tables as rows. Footnotes, edit links and navigation menus are left out. ANIMA uses the same program when an answer is in the library.

## How to use it

| Command | What it does |
|---|---|
| `zim ls` | lists the collections in `~/zim` |
| `zim search diabetes` | titles starting like that, in every collection (any case) |
| `zim get Insulin` | the whole article as text |
| `zim get 'Albert Einstein' --intro` | the introduction only |
| `zim get 'Diabetes' --sections` | the list of sections |
| `zim get 'Diabetes' --section Treatment` | one section |
| `zim get -z medicine Insulin` | only in that collection (part of its name is enough) |
| `zim get Insulin --max 2000` | at most 2000 characters |
| `zim get Insulin --all` | keeps notes, references and links too |
| `zim random` | the beginning of a random article |
| `zim info medicine` | title, language, date, number of articles of a collection |
| `zim check medicine` | checks the file is not damaged |

It works with the shell: `zim get Insulin > insulin.txt` saves the article to a file, `zim get -z wikipedia img/photo.webp --out photo.webp` extracts an image or another file.

## Where to get the files

From the Kiwix library site (library.kiwix.org) download the `.zim` files you want on a PC, then copy them into the `zim` folder of the SD card's home (`/sdcard/home/zim`) with a card reader. FAT32 cards and files over 2 GB need the files split in parts (`name.zimaa`, `name.zimab`…): `zim` reads them as one file.

To start, the `nopic` (text only) or `mini` (introductions only) versions are fine: English Wikipedia `top_mini` is about 280 MB, the medical encyclopedia about 90 MB.

## Limits

Search finds titles that **start** with the words you type; the full text of the articles is not indexed yet. If an article has no title written the way you type it and no redirect, try the right capitals or `zim search`. Images and formulas cannot be shown in the Terminal: formulas appear as their TeX code. Collections made with zimit (copies of whole websites) give rougher results.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
