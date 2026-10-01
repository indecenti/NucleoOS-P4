# GNU units

## What it is for

It converts between more than 3000 units of measure and computes with them: lengths, weights, speeds, energy, pressure, cooking measures, temperatures, currencies, chemical elements, physical constants. It understands whole expressions, such as `230 V * 10 A` in kW.

## How to use it

| Command | What it does |
|---|---|
| `units '5 mph' 'km/hr'` | converts (shows the factor and its inverse) |
| `units -t '3 cups' ml` | the result only (`-t`, handy in scripts) |
| `units -t 'tempF(100)' tempC` | temperatures: `tempC`, `tempF`, `tempK` with brackets |
| `units -t '230 V * 10 A' kW` | computes with units |
| `units -t '2 hours + 30 min' s` | adds quantities of the same kind |
| `units -t '100 euro' 'US$'` | currencies |
| `units -t oxygen 'g/mol'` | element masses |
| `units` | interactive prompt: type the unit you have, Enter, then the one you want |

## Good to know

- `h` is Planck's constant: for hours write `hr` or `hour` (`km/hr`, not `km/h`).
- `degF` and `degC` are temperature differences; for readings use `tempF(...)` and `tempC(...)`.
- Your own units go in the `.units` file in the home folder, one per line (`pizza 32 cm`).

## Limits

Exchange rates are the ones shipped with version 2.24 (November 2024) and don't update by themselves: for current rates put your own `currency.units` in the `units` folder of the home. The built-in `help` command is not available (it needs an external viewer).

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
