# html2text

## A cosa serve

Trasforma una pagina web o un file HTML in testo semplice da leggere: titoli, paragrafi, elenchi e tabelle restano, script, stili e menu spariscono. Con `curl` legge le pagine direttamente da internet.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `curl -s https://example.org \| html2text` | una pagina web come testo |
| `curl -s URL \| html2text \| less` | da sfogliare con calma |
| `html2text pagina.html` | un file HTML della home |
| `html2text -links pagina.html` | in fondo l'elenco degli indirizzi dei link |
| `html2text -width 60 pagina.html` | va a capo a 60 colonne |
| `curl -s URL \| html2text > pagina.txt` | salva il testo in un file |

## Con Anima

È un programma di sistema: Anima lo usa da sola quando serve una risposta esatta. Puoi anche chiederle di usarlo, per esempio "leggimi le notizie da questa pagina".

## Limiti

Le pagine costruite da JavaScript (molti siti moderni) arrivano quasi vuote: il testo lo crea il browser, non c'è nell'HTML. Le pagine senza codifica dichiarata sono lette come UTF-8.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
