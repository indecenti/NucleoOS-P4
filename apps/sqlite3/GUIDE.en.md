# SQLite

## What it is

The official SQLite shell: create and query real SQL databases stored as files on the SD card. Handy for notes, inventories, logs, learning SQL. JSON and math functions are included.

## How to use it

```
sqlite3 notes.db
```

Opens (or creates) `notes.db` in the home folder. Then type SQL, ending each statement with `;`:

```sql
CREATE TABLE notes(id INTEGER PRIMARY KEY, text TEXT, at TEXT DEFAULT CURRENT_TIMESTAMP);
INSERT INTO notes(text) VALUES ('buy milk');
SELECT * FROM notes;
```

## Useful commands

| Command | What it does |
|---|---|
| `.tables` | lists the tables |
| `.schema` | shows their structure |
| `.mode box` | results as a readable table |
| `.headers on` | column names |
| `.import file.csv t` | imports a CSV (with `.mode csv`) |
| `.help` | every command |
| `.quit` | quits (or **EOF**) |

## Limits

Trimmed for the board: no WAL, mmap, threads, loadable extensions or FTS5. Without a file name (`sqlite3` alone) the database lives in memory and is lost on exit.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** ends the input (like Ctrl-D): many programs then finish. **STOP** kills it right away.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): the program sees that folder as `/`. Copy files from a PC by putting the SD in a card reader.
- `apps` lists the installed terminal programs, `help` the Terminal's own commands.
