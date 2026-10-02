# dateutils

## What it is for

Date arithmetic without mistakes: how many days are left, which day it will be in 45 days or in 10 business days, series of deadlines, format conversions.

## How to use it

| Command | What it does |
|---|---|
| `datediff 2026-10-02 2026-12-25` | days between two dates (`-f '%w weeks %d days'` for weeks and days) |
| `dateadd 2026-10-02 +45d` | the date in 45 days (`+3w` weeks, `+2mo` months, `-1y` years) |
| `dateadd 2026-10-02 +10b` | in 10 business days (skips weekends) |
| `dateseq 2026-10-01 +1w 2026-11-01` | one date per week between the two |
| `dateconv 2026-10-02 -f '%A %d %B %Y'` | the date written out |
| `dateround 2026-10-02 Mon` | the next Monday |
| `datetest 2026-10-02 --lt 2026-12-25` | comparison (answers with the exit status) |
| `dategrep '>=2026-10-01' < deadlines.txt` | lines with dates from October on |
| `strptime -i '%d/%m/%Y' 02/10/2026` | reads dates in other formats |

`now` and `today` mean today. `dateutils` alone lists the tools; each one takes `--help`.

## With Anima

It is a system program: Anima uses it by itself when an exact answer is needed. You can also ask for it, for example "how many business days until Christmas?".

## Limits

Times are UTC and time zones (`datezone`) need the zoneinfo files, which are not included. Day and month names are English. `datesort` is not included.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
