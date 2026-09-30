# jq

## What it is

jq is the Swiss army knife for JSON: it reads JSON files and extracts, filters, transforms and reformats their content with a small filter language. Handy to make sense of a config file, a data export or an API response saved with `curl`. Regular expressions are there too (`test`, `match`, `sub`, `gsub`, `capture`).

## How to use it

`jq FILTER file.json` applies the filter to the file (or to the pipe's input when no file is given):

| Command | What it does |
|---|---|
| `jq . data.json` | prints the JSON indented and readable |
| `jq .name data.json` | the `name` field |
| `jq -r '.apps[].id' data.json` | every `id` of the `apps` list, as plain text |
| `jq '.apps[] \| select(.size > 400)' data.json` | only the items matching the condition |
| `jq -c 'map(.size) \| add' list.json` | sums the sizes, result on one line |
| `jq --arg n lua '.apps[] \| select(.id == $n)' data.json` | uses a value passed from outside |
| `jq -n '[range(5)] \| map(. * .)'` | no input: just compute |
| `cat data.json \| jq keys` | through a shell pipe |

Useful options: `-r` raw text without quotes, `-c` one line per result, `-s` reads all files as one list, `-S` sorts keys, `-f filter.jq` reads the filter from a file.

## Limits

The Terminal's command line does not pass double quotes inside an argument well: if the filter has `"..."` strings, write it to a file and use `jq -f filter.jq data.json`, or pass the value with `--arg`. ANSI colours are turned off with `-M`. Memory: up to 8 MB (big JSON files, say over 1-2 MB, may not fit).

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
