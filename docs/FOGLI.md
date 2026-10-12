# Fogli — the spreadsheet

Fogli (`sheets`, *Sheets / Hojas / Feuilles / Tabellen*) is NucleoOS' spreadsheet: a native touch
and mouse app (`components/nv_apps/sheets_app.cpp`) in front of **sc-im** running as its engine
(system app `sc-im`, `ports/tui/scim/nv_engine.c`). The app draws the grid and takes the input; the
engine holds the document, evaluates the formulas, keeps the undo history and reads and writes the
files.

## What the user gets

- **Files**: opens `.xlsx .xls .ods .csv .tsv .sc`; saves `.xlsx` (formulas, bold/italic/underline
  and number formats kept, so Excel and LibreOffice reopen them as they were), `.csv`, `.tsv`. `.xls`
  and `.ods` open read-only: Save makes an `.xlsx`.
- **Excel's notation, in the user's language**: rows from 1, `=SOMMA(B2:B8;C2:C8)` in Italian,
  `=SUMA` in Spanish, `=SUM` in English (any of the five is accepted whatever the UI language),
  `;` with the decimal comma, `&` to join text. 26 functions (SUM AVERAGE COUNT MAX MIN PRODUCT
  ROUND ABS SQRT POWER IF STDEV INT EXP LN LOG10 NOW YEAR MONTH DAY HOUR MINUTE SECOND UPPER LOWER
  LEN); several ranges in one SUM/AVERAGE/COUNT/MAX/MIN/PRODUCT. Translation: `ports/tui/scim/nv_fx.c`.
- **Smart entry**: `1.234,5` is a number, `12,5%` a percentage, `12/10/2026` a date, `=...` a
  formula, `'007` text; an empty entry clears the cell.
- **Editing in the cell** with a real caret, mirrored in the formula bar; Enter / Tab go on to the
  next cell with the keyboard up (typing a column of values); while a formula is being written a tap
  (or a drag) on cells puts their reference in.
- **Touch and mouse**: tap selects, a second tap or a double tap edits, drag scrolls (fling), the
  corner handles stretch the selection, long press or right click opens the context menu, drag a
  column header's edge to resize it, the mouse wheel scrolls, mouse drag selects. Physical keyboard:
  arrows (Shift extends), F2/Enter, Ctrl+C/X/V/Z/Y/S/O/N/F/B/I/U/A, Del, PgUp/PgDn, typing replaces.
- **Toolbar**: open, save, undo/redo, cut/copy/paste (cells keep their formulas, relative references
  move; text from other apps pastes as cells), bold/italic/underline, number formats (general,
  0.00, thousands, currency €, percent), Σ (sum of the numbers above, or totals under a range), sort
  (the table around the active cell keeps its header row), find, ANIMA, more (insert/delete rows and
  columns, fit width, shortcuts). Sheet tabs with add/rename/delete; the status bar shows sum,
  average and count of the selection.
- **Start page**: blank sheet, three templates in the UI language (monthly budget, shopping list,
  timesheet), open a file (every spreadsheet on the card, newest first), recent files, and the
  **recovery copy**: while there are unsaved changes the document is copied every 30 s to
  `/sdcard/home/.fogli/ripristino.sc`, offered back on the next start.
- **ANIMA inside**: the panel reads the selected cells (or the whole table) with their formulas, and
  answers in the UI language: summarize, check for mistakes, explain a formula, or a free question.
  When the answer changes the sheet it proposes actions (`SET cell content`, `FORMAT range kind`,
  `BOLD range`): each formula is checked by the engine first, the changes are drawn on the grid,
  and nothing is written until **Apply** (Undo puts it back). Follow-up questions carry the previous
  exchange. Without a reachable model, on Cancel or after 90 s, the device answers what it can
  compute itself (a summary per column). The prompt is in `ai_send()`.

## In the OS

- **Files**: spreadsheets are their own kind (`NV_FILE_SHEET`, table icon, "Foglio di calcolo");
  Fogli opens them (opener `sheets.edit`, priority 50; sc-im's terminal UI stays in "Open with");
  a file's details have **Analizza con ANIMA** (action `sheets.analyze`); the empty part of a folder
  (right click or long press) has **Nuovo foglio di calcolo**, which opens Fogli blank and saves in
  that folder. Back returns to the app that opened Fogli.
- **Search**: `NvApp.keywords` — "excel", "xlsx", "tabella"... find Fogli in Home and Start search.
- **ANIMA**: "apri fogli / excel / foglio" opens it (aliases in `registry/app-aliases.json`), `ACT
  open_app sheets`, the shell's `open F` shows a file in its app, and Fogli installs the skill
  `/sdcard/data/anima/skills/fogli.md` (text in `sheets_skill.h`, copy in `sd/data/anima/skills/`):
  how to open, read (`sc-im --nocurses ... --export_csv`) and make spreadsheets (a CSV in
  ~/Documenti, then `open`). The open file is ANIMA's "current file" (`nucleo_anima_note_file`).

## The engine protocol

`sc-im --nocurses --nv_engine [file]` reads one command per line and answers tab-separated records;
a batch ends with `nv sync N`, answered `\x1eN`. Rows and columns are sc-im's (from 0). Commands:
`lang`, `info` (D/S records), `cols` (W), `view` (C: row col kind align style text), `cell` (X: the
editable text), `set`, `clear`, `copy`/`cut`/`paste [values]`, `insrow`/`delrow`/`inscol`/`delcol`,
`sort ... [auto]`, `width`, `autofit`, `fmt`, `bold`/`italic`/`underline`, `align`, `undo`/`redo`
(references relinked after both), `sheet`/`newsheet`/`rensheet`/`delsheet`, `stats` (T), `tsv
[fx]` (L), `find` (F), `check` (K), `save`, `backup`, `name`, `saved`. Messages: E (error), I, V.
The full list is at the top of `nv_engine.c`; `ports/tui/test.sh` exercises it on the PC.

The engine's memory follows the file (6 MB for a new sheet, more for big files, the manifest's
16 MB on a retry): the rest of the system keeps its PSRAM.

## Building and testing

- Engine: `bash ports/tui/build.sh sc-im` (WSL, wasi-sdk 34; AOT with wamrc), `bash ports/tui/test.sh`
  (formula notation unit tests, engine protocol, xlsx round trips).
- App: part of the firmware. Fonts `nv_font_sheet_14/18` come from `tools/gen_fonts.ps1`; the icon
  is `data_sheet` in `tools/gen_icons.py`.
