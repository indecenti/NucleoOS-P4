# Strumenti PDF (PDFio)

## A cosa serve

Legge i PDF dal Terminale: estrae il testo di tutte le pagine, mostra titolo, autore, date e numero di pagine, e unisce più PDF in uno solo. Con il testo estratto Anima può riassumere un documento, cercare una cifra in una bolletta o rispondere a domande su un manuale.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `pdftotext bolletta.pdf` | il testo di tutte le pagine |
| `pdftotext manuale.pdf \| less` | da sfogliare |
| `pdftotext manuale.pdf \| grep -i garanzia` | cerca una parola |
| `pdftotext doc.pdf > doc.txt` | salva il testo in un file |
| `pdfinfo doc.pdf` | titolo, autore, date, pagine e formato |
| `pdfmerge -o tutto.pdf a.pdf b.pdf` | unisce più PDF |

## Con Anima

È un programma di sistema: chiedi per esempio "riassumimi il PDF che ho scaricato" o "quanto devo pagare nella bolletta?".

## Limiti

I PDF scansionati sono solo immagini: non contengono testo da estrarre. L'ordine del testo segue quello del file, quindi colonne e tabelle complesse possono uscire mescolate. Niente immagini né moduli.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
