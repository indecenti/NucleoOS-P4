# JavaScript

## A cosa serve

JavaScript moderno (ES2024) con il motore QuickJS-ng: prompt interattivo, script e moduli ES, lettura e scrittura di file con i moduli `std` e `os`, timer.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `js` | apre il prompt interattivo (`>`; `...` se la riga continua) |
| `js file.js` | esegue uno script dalla cartella home |
| `js -m file.js` | lo esegue come modulo ES (automatico per `.mjs`) |
| `js -e "codice"` | valuta il codice ed esce |
| `js -h` | mostra l'aiuto |

```js
> [1, 2, 3].map(x => x * 2)
[ 2, 4, 6 ]
> setTimeout(() => print("fatto"), 1000)
```

Le parentesi aperte continuano sulla riga dopo. I timer partono prima del prompt successivo. Per uscire: **EOF**.

## Limiti

Il prompt è a righe: niente frecce per la cronologia dentro il programma né completamento con Tab. Memoria: fino a 8 MB.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
