# pForth

## A cosa serve

Forth è un linguaggio a stack: i numeri vanno su una pila e le "parole" li consumano, per esempio `2 3 +` lascia `5`. Si definiscono parole nuove con `:` e `;`, e si prova tutto al volo. pForth è un Forth ANS portabile, con numeri in virgola mobile, stringhe, variabili, cicli e parole per leggere e scrivere file.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `pforth` | apre il prompt interattivo |
| `pforth -q` | come sopra, senza intestazione né `ok` dopo ogni riga |
| `pforth programma.fth` | esegue un file dalla cartella home e termina |

Nel prompt scrivi parole separate da spazi e premi Invio; dopo ogni riga pForth risponde `ok` e mostra lo stack (`Stack<10>` vuol dire base 10, stack vuoto):

```forth
2 3 + .                            \ stampa 5
: QUADRATO ( n -- n*n ) DUP * ;    \ una parola nuova
12 QUADRATO .                      \ stampa 144
10 0 DO I . LOOP                   \ stampa 0 1 2 ... 9
VARIABLE CONTO  5 CONTO !  CONTO @ 2 * .
2.5e0 3.0e0 F* F.                  \ stampa 7.500000
```

Per caricare un file senza uscire: `include programma.fth`. Per vedere le parole disponibili: `words`. Per uscire: `bye`, oppure **EOF**.

## Limiti

Le celle sono a 32 bit. Niente editor di riga né cronologia (il Terminale manda righe intere). Una divisione per zero dà l'errore `THROW code = -10` invece di chiudere il programma. `RESIZE-FILE` non può accorciare i file. Memoria disponibile: fino a 4 MB.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
