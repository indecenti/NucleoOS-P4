---
name: python
description: write, run and fix Python scripts on the device (MicroPython in the Terminal)
triggers: python, script python, programma python, micropython, file .py, scrivi uno script, calcola con python, automatizza, write a python script, run python, py script
offline: Per scrivere e provare script serve un modello (cloud o un server Ollama da 7B in su): configuralo in Impostazioni web > IA.
---
The device has Python 3 as MicroPython 1.26, a Terminal program ("python" in the store). Check it
with ACT sh store info python; if it is missing: ACT sh store install python (asks permission).
It sees /sdcard/home as "/": the file ~/py/x.py is /py/x.py for python.

Workflow (one ACT per reply):
1) ACT write ~/py/<name>.py with the whole script;
2) run it: ACT sh python /py/<name>.py [args] (output and any Traceback come back to you);
3) on a Traceback read the last line and the line number, fix with ACT edit, run again;
4) when it works, answer with the result and how to run it (python /py/<name>.py in the Terminal).
Quick checks need no file: ACT sh python -c "print(sum(range(10)))".

Rules: no input() (the run has no keyboard: use sys.argv), finish in seconds (no endless loops),
print the results. Modules (CPython names): os os.path pathlib shutil tempfile sys io json re
string textwrap html base64 struct pprint math random datetime time bisect heapq collections
itertools functools operator copy contextlib gzip tarfile hashlib hmac
argparse logging unittest traceback. Not available: pip, numpy, requests, threading, socket,
asyncio, subprocess. Own modules go in ~/py or ~/lib (on sys.path). Tests: write test_x.py with
unittest and run ACT sh python /py/test_x.py.
Files: open('/py/data.txt') reads ~/py/data.txt; os.listdir('/'), os.mkdir, os.remove work.

Minimal example:
import sys, json
nums = [int(a) for a in sys.argv[1:]] or [3, 1, 2]
print(json.dumps({"sorted": sorted(nums), "sum": sum(nums)}))
