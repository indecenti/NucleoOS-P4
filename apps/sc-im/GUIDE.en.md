# sc-im, spreadsheet

## What it is for

sc-im is a real full-screen spreadsheet: cells with numbers, text and formulas that recalculate by themselves, functions (`@sum`, `@avg`, `@max`, `@if`, dates and times), number formats, column widths, sorting, filters, undo and redo. It opens **Excel `.xlsx` and `.xls`**, **LibreOffice `.ods`**, **`.csv`** and **`.tsv`** files, and saves `.xlsx`, `.csv` and `.tsv`: a list of expenses, an inventory, a loan plan, a sheet someone emailed you.

## How to open it

- **Tap the icon** on Home: an empty sheet opens.
- In **Files**, tap an `.xlsx`, `.xls`, `.ods` or `.csv` file and choose **Open with › sc-im**.
- In the Terminal: `sc-im expenses.xlsx`.

## Touch and keyboard

- **Tap a cell** to select it. **Drag up and down** to scroll the sheet.
- The key row above the keyboard has **Esc**, **Tab**, **Ctrl** and the **arrows**.
- A USB or KeyDeck keyboard also gives you Ctrl, Shift and the function keys.

## The main commands

sc-im works like the vim editor: a letter is a command, long commands start with a colon.

| Keys | What they do |
|---|---|
| arrows | move the selection |
| `=` | type a number or a formula, e.g. `=@sum(B0:B9)` then Enter |
| `<` `>` `\` | type text aligned left, right, centred |
| `e` / `E` | edit the cell's number / text |
| `x` | delete the cell |
| `u` / Ctrl+R | undo / redo |
| `yy` then `p` | copy the cell and paste it |
| `ir` `ic` | insert a row / a column |
| `dr` `dc` | delete the row / the column |
| `f>` `f<` | wider / narrower column |
| `f+` `f-` | more / fewer decimals |
| `goB12` | go to cell B12 |
| `:w` | save; `:w accounts.xlsx` saves as Excel |
| `:e csv accounts.csv` | export to CSV (also `tab`, `txt`, `mkd`) |
| `:load file.ods` | open another file |
| `:q` | quit (`:q!` without saving) |
| `:h` | the full manual (`q` closes it) |

Cells are named by column and row, **counting rows from 0**: `A0` is the top-left cell. Ranges in formulas are written `A0:A9`.

## With Anima

sc-im is a system program: Anima uses it to read your sheets. Ask for example "what is the total in expenses.xlsx?" or "summarise the orders.ods table".

## Limits

- Charts, macros, pictures and comments in Excel files are not read; from an `.xlsx` you get values, text and (with the `--xlsx_readformulas` option) formulas.
- `.ods` files open but save as `.xlsx`, `.csv` or sc-im's own format (`.sc`).
- From an old `.xls` file the first sheet is read; it saves as `.xlsx`.
- The system clipboard and `!` commands (external programs) are not available.
- Very large sheets (tens of thousands of rows) may not fit in the device's memory: sc-im says so and the original file is left untouched, because it saves to a temporary file first.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- Read a sheet without opening it: `sc-im --nocurses --quit_afterload --export_csv expenses.xlsx` (`--export_mkd` gives a table).
- `apps` lists the installed terminal programs, `help sc-im` its syntax.

---

*This product includes software developed by Andrés Martinelli <andmarti@gmail.com>.*
