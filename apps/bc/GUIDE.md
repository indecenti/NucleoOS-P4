# bc

## A cosa serve

bc è la calcolatrice a precisione arbitraria dei sistemi Unix: i numeri possono avere tutte le cifre che vuoi, prima e dopo la virgola. Ha variabili, `if`, cicli, funzioni definite da te e, con `-l`, una libreria matematica (seno, coseno, arcotangente, logaritmo, esponenziale). Questa è la versione di Gavin Howard.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `bc` | apre la calcolatrice interattiva (`>>>`) |
| `bc -l` | con la libreria matematica e 20 decimali |
| `bc conti.bc` | esegue un file dalla cartella home, poi resta interattiva |
| `echo "2^64" \| bc` | calcola al volo con una pipe della shell |

Scrivi un'espressione e premi Invio:

```bc
>>> 2^200
1606938044258990275541962092341162602522202993782792835301376
>>> scale = 30
>>> 1/7
.142857142857142857142857142857
>>> sqrt(2)
1.414213562373095048801688724209
>>> obase = 16
>>> 255
FF
```

Con `bc -l`: `4*a(1)` è pi greco, `s(x)` seno, `c(x)` coseno, `l(x)` logaritmo naturale, `e(x)` esponenziale. Una funzione tua:

```bc
define fatt(n) {
  if (n < 2) return 1
  return n * fatt(n - 1)
}
fatt(50)
```

Per uscire: `quit`, oppure **EOF**.

## Limiti

Un errore (per esempio `1/0`) nel prompt viene segnalato e si continua; un errore dentro un file lo interrompe. Niente cronologia né editor di riga. Memoria disponibile: fino a 8 MB.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
