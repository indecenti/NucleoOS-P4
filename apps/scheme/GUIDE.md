# Scheme

## A cosa serve

Scheme è il dialetto del Lisp nato per insegnare a programmare: tutto è un'espressione tra parentesi, le funzioni sono valori, la ricorsione è di casa. Questo è **TinyScheme**, un interprete piccolo e veloce ad avviarsi che copre buona parte di R5RS: liste, vettori, stringhe, caratteri, `let`, `lambda`, macro, continuazioni, porte su file e su stringa.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `scheme` | apre il prompt interattivo (`ts>`) |
| `scheme programma.scm` | esegue un file dalla cartella home |
| `scheme programma.scm a b` | passa argomenti, letti in `*args*` |
| `scheme -i programma.scm` | esegue il file e poi resta nel prompt |
| `scheme -e '(display (* 6 7))'` | valuta un'espressione al volo |

Nel prompt scrivi un'espressione e premi Invio; se lasci parentesi aperte continua alla riga dopo:

```scheme
ts> (+ 1 2 3)
6
ts> (define (fatt n) (if (< n 2) 1 (* n (fatt (- n 1)))))
ts> (fatt 20)
2432902008176640000
ts> (map (lambda (x) (* x x)) (list 1 2 3 4))
(1 4 9 16)
ts> (let loop ((i 0) (s 0)) (if (> i 100) s (loop (+ i 1) (+ s i))))
5050
ts> (load "/programma.scm")
```

File: `(define p (open-output-file "/dati.txt")) (write '(1 2 3) p) (close-output-port p)` e poi `(read (open-input-file "/dati.txt"))`.

Per uscire: `(exit)`, oppure **EOF**. Dopo `display` il prompt mostra anche `#t`: è il valore restituito, non un errore.

## Limiti

Gli interi sono a 64 bit (niente numeri enormi: `(fatt 25)` sfora; per quelli c'è `bc`) e non ci sono frazioni (`1/3`). Niente `syntax-rules`: le macro si scrivono con `define-macro`. Niente moduli R7RS (`define-library`, `import`) né processi esterni. Memoria: circa 300 000 celle (liste fino a un centinaio di migliaia di elementi), in tutto fino a 12 MB.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
