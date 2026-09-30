# Tcl

## A cosa serve

Tcl è un linguaggio di scripting dove tutto è un comando e tutto è una stringa: semplice da imparare, ottimo per automatismi e piccoli strumenti. Questa è **Jim Tcl**, un Tcl compatto con liste, dict, espressioni regolari, `proc`, lambda, oggetti (`oo`), namespace e file. Qui hai il prompt interattivo e l'esecuzione di file `.tcl`.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `tcl` | apre il prompt interattivo (`.`) |
| `tcl script.tcl` | esegue un file dalla cartella home |
| `tcl script.tcl a b` | passa argomenti, letti in `$argv` |
| `tcl -e 'expr {6 * 7}'` | esegue una riga al volo |

Nel prompt ogni riga è un comando e il risultato viene mostrato; un blocco `{` lasciato aperto continua alla riga dopo:

```tcl
. expr {6 * 7}
42
. set nomi {anna bruno carla}
. lsort -decreasing $nomi
carla bruno anna
. proc quadrato {x} { expr {$x * $x} }
. quadrato 12
144
. dict set eta anna 30
. dict get $eta anna
30
. foreach i {1 2 3} { puts "riga $i" }
```

File:

```tcl
set f [open /nota.txt w]; puts $f "ciao da Tcl"; close $f
set f [open /nota.txt]; puts [gets $f]; close $f
```

Per uscire: `exit`, oppure **EOF**.

## Limiti

Niente processi esterni (`exec`), socket, segnali ed event loop (`after`, `vwait`). Gli interi sono a 64 bit (niente numeri enormi: per quelli c'è `bc`). Memoria disponibile: fino a 8 MB.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
