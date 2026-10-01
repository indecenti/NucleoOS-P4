# qrencode

## A cosa serve

Trasforma un testo in un codice QR disegnato direttamente nel Terminale, da inquadrare con il telefono: un link da aprire, un numero, un appunto, oppure la tua rete Wi-Fi così gli ospiti si collegano senza digitare la password. Il codice si può anche salvare come immagine SVG.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `qrencode https://example.org` | disegna il QR del link nel Terminale |
| `qrencode 'WIFI:S:NomeRete;T:WPA;P:password;;'` | QR per collegarsi a una rete Wi-Fi |
| `qrencode -o codice.svg 'testo'` | salva il QR in `codice.svg` nella home |
| `qrencode -l H 'testo'` | più correzione d'errore (L, M, Q, H): si legge anche se rovinato |
| `echo testo \| qrencode` | prende il testo dall'input |
| `qrencode -t ascii 'testo'` | versione fatta di `#`, per incollarla dove i blocchi non si vedono |

Con la shell: `cat appunti.txt | qrencode` mette un file intero nel QR (fino a circa 2900 caratteri).

## Limiti

Niente PNG: per un'immagine usa `-o nome.svg` (anche `.eps` e `.xpm`). Un testo lungo dà un QR più grande del Terminale: tienilo corto, o salvalo in SVG. Il QR a schermo è bianco su nero: se il telefono fa fatica, allontanalo un po' o alza la luminosità.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
