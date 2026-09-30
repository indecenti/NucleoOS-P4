# Berry

## A cosa serve

Berry è un linguaggio di scripting piccolo e veloce, pensato per i microcontrollori (è quello che usa Tasmota): classi, closure, eccezioni, liste e mappe, moduli per stringhe, matematica, JSON e file. Qui hai il prompt interattivo e l'esecuzione di file `.be`.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `berry` | apre il prompt interattivo (`>`) |
| `berry script.be` | esegue un file dalla cartella home |
| `berry -i script.be` | esegue il file e poi resta nel prompt |
| `berry -e "print(6*7)"` | esegue una riga al volo |

Nel prompt scrivi un'espressione o un'istruzione e premi Invio; un blocco lasciato aperto (`def`, `for`, `if`...) continua alla riga dopo con `>>`:

```berry
> 6 * 7
42
> def quadrato(x) return x * x end
> quadrato(12)
144
> import math
> math.sqrt(2)
1.41421
> try raise "errore", "qualcosa" except .. as e, m print(e, m) end
errore qualcosa
```

File e JSON:

```berry
f = open("/note.txt", "w") f.write("ciao") f.close()
import json
print(json.dump({"a": [1, 2]}))
```

Per uscire: **EOF**, oppure `import os os.exit()`.

## Limiti

Niente processi esterni: `os.system` risponde -1. Niente moduli nativi caricabili (`.so`). Memoria disponibile: fino a 8 MB.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
