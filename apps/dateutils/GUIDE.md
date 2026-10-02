# dateutils

## A cosa serve

Calcoli con le date senza sbagliare: quanti giorni mancano, che giorno sarà fra 45 giorni o fra 10 giorni lavorativi, sequenze di scadenze, conversione di formati.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `datediff 2026-10-02 2026-12-25` | giorni tra due date (`-f '%w weeks %d days'` per settimane e giorni) |
| `dateadd 2026-10-02 +45d` | data fra 45 giorni (`+3w` settimane, `+2mo` mesi, `-1y` anni) |
| `dateadd 2026-10-02 +10b` | fra 10 giorni lavorativi (salta sabati e domeniche) |
| `dateseq 2026-10-01 +1w 2026-11-01` | una data ogni settimana tra le due |
| `dateconv 2026-10-02 -f '%A %d %B %Y'` | la data scritta per esteso |
| `dateround 2026-10-02 Mon` | il prossimo lunedì |
| `datetest 2026-10-02 --lt 2026-12-25` | confronto (risponde con lo stato di uscita) |
| `dategrep '>=2026-10-01' < scadenze.txt` | le righe con date da ottobre in poi |
| `strptime -i '%d/%m/%Y' 02/10/2026` | legge date scritte all'italiana |

`now` e `today` valgono come data di oggi. `dateutils` da solo elenca i comandi; ognuno ha `--help`.

## Con Anima

È un programma di sistema: Anima lo usa da sola quando serve una risposta esatta. Puoi anche chiederle di usarlo, per esempio "quanti giorni lavorativi mancano a Natale?".

## Limiti

Gli orari sono in UTC e i fusi orari (`datezone`) richiedono i file zoneinfo, che non sono inclusi. I nomi di giorni e mesi sono in inglese. `datesort` non c'è.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
