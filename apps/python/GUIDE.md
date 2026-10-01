# Python

## A cosa serve

Python 3 nella versione MicroPython 1.26: lo stesso linguaggio, pensato per dispositivi piccoli. Qui hai il prompt interattivo, per provare al volo, e l'esecuzione di file `.py` con i loro moduli.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `python` | apre il prompt interattivo (`>>>`) |
| `python script.py` | esegue un file dalla cartella home |
| `python script.py a b` | passa argomenti, letti in `sys.argv` |
| `python -c "print(2**100)"` | esegue una riga di codice |
| `python -m modulo` | importa ed esegue un modulo |

Nel prompt i blocchi (`for`, `def`, `if`) continuano con `...`: una riga vuota li chiude.

```python
>>> sum(x * x for x in range(10))
285
>>> import json; json.dumps({"a": [1, 2]})
'{"a": [1, 2]}'
```

Moduli inclusi, pronti da importare:

- **sistema e file**: `os` (con `os.path`, `os.walk`, `os.makedirs`), `pathlib`, `shutil`, `tempfile`, `stat`, `sys`, `io`, `gc`, `errno`
- **dati e testo**: `json`, `re`, `string`, `textwrap`, `html`, `base64`, `binascii`, `struct`, `pprint`
- **numeri e tempo**: `math`, `cmath`, `random`, `datetime`, `time`, `bisect`, `heapq`
- **strutture e funzioni**: `collections` (anche `defaultdict`), `itertools`, `functools`, `operator`, `copy`, `contextlib`, `abc`, `types`, `inspect`
- **archivi e sicurezza**: `gzip`, `tarfile`, `deflate`, `hashlib`, `hmac`
- **programmi**: `argparse`, `logging`, `unittest`, `traceback`, `warnings`, `locale`, `keyword`

I tuoi moduli vanno accanto allo script, in `/lib` o in `/py` (cioè `/sdcard/home/lib`, `/sdcard/home/py`).

Per uscire: **EOF**, oppure `exit()`.

## Limiti

È MicroPython, non CPython: niente `pip`, `threading`, `socket`, `asyncio`, `subprocess`. Ricorsione fino a circa 1000 livelli (come CPython), poi `RuntimeError`. Memoria: fino a 8 MB.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D). **STOP** interrompe subito il programma.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`.
