# Python

## What it is

Python 3 as MicroPython 1.26: the same language, built for small devices. You get the interactive prompt, to try things out, and `.py` file execution with your own modules.

## How to use it

| Command | What it does |
|---|---|
| `python` | opens the interactive prompt (`>>>`) |
| `python script.py` | runs a file from the home folder |
| `python script.py a b` | passes arguments, read from `sys.argv` |
| `python -c "print(2**100)"` | runs one line of code |
| `python -m module` | imports and runs a module |

At the prompt, blocks (`for`, `def`, `if`) continue with `...`: an empty line closes them.

```python
>>> sum(x * x for x in range(10))
285
>>> import json; json.dumps({"a": [1, 2]})
'{"a": [1, 2]}'
```

Built-in modules, ready to import:

- **system and files**: `os` (with `os.path`, `os.walk`, `os.makedirs`), `pathlib`, `shutil`, `tempfile`, `stat`, `sys`, `io`, `gc`, `errno`
- **data and text**: `json`, `re`, `string`, `textwrap`, `html`, `base64`, `binascii`, `struct`, `pprint`
- **numbers and time**: `math`, `cmath`, `random`, `datetime`, `time`, `bisect`, `heapq`
- **structures and functions**: `collections` (also `defaultdict`), `itertools`, `functools`, `operator`, `copy`, `contextlib`, `abc`, `types`, `inspect`
- **archives and security**: `gzip`, `tarfile`, `deflate`, `hashlib`, `hmac`
- **programs**: `argparse`, `logging`, `unittest`, `traceback`, `warnings`, `locale`, `keyword`

Your own modules go next to the script, in `/lib` or in `/py` (that is `/sdcard/home/lib`, `/sdcard/home/py`).

To leave: **EOF**, or `exit()`.

## Limits

It is MicroPython, not CPython: no `pip`, `threading`, `socket`, `asyncio`, `subprocess`. Recursion up to about 1000 levels (like CPython), then `RuntimeError`. Memory: up to 8 MB.

## In the Terminal

- **Tap the icon** to open the Terminal with the program already running, or open **Terminal** and type the command.
- Type in the bottom line and press Enter: the line goes to the program.
- **EOF** closes the input (like Ctrl-D). **STOP** stops the program at once.
- Your files live in the **home** folder of the SD card (`/sdcard/home`): to the program that folder is `/`.
