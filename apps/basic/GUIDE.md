# BASIC

## A cosa serve

Il BASIC classico a righe numerate, come sui computer di una volta: ottimo per imparare le basi della programmazione (variabili, condizioni, cicli, salti).

## Come si usa

Scrivi il programma in un file di testo, per esempio `conta.bas`, nella cartella home:

```basic
10 print "quanti numeri?"
20 input n
30 for i = 1 to n
40 print i
50 next i
60 end
```

Poi nel Terminale:

```
basic conta.bas
```

Non c'è una modalità immediata: il programma si scrive in un file e si esegue intero.

## Istruzioni

`print`, `input`, `if ... then`, `goto`, `gosub` / `return`, `for` / `next`, `peek` / `poke`, `end`.

## Regole da ricordare

- Parole chiave e variabili **solo minuscole** (`print`, non `PRINT`).
- Variabili: una lettera da `a` a `z`, solo numeri interi.
- Il testo si può stampare (`print "ciao"`) ma non salvare in una variabile.
- `peek` e `poke` usano un array di appoggio di 1024 celle, non la memoria vera.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
