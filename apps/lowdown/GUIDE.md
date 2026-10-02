# lowdown

## A cosa serve

Legge i file Markdown (`.md`, quelli delle guide e dei LEGGIMI) e li mostra formattati nel Terminale, con titoli, grassetti, elenchi e tabelle. Li converte anche in HTML, pagine man, LaTeX e OpenDocument (`.fodt`, si apre con LibreOffice).

## Come si usa

| Comando | Cosa fa |
|---|---|
| `lowdown -tterm note.md` | il file formattato nel Terminale |
| `lowdown -tterm note.md \| less` | da sfogliare |
| `lowdown -thtml -s note.md -o note.html` | una pagina HTML completa |
| `lowdown -s -tfodt note.md -o note.fodt` | un documento per LibreOffice |
| `lowdown -tman pagina.md` | una pagina man |
| `cat note.md \| lowdown -tterm` | legge dall'input |

## Con Anima

È un programma di sistema: Anima lo usa da sola quando serve una risposta esatta. Puoi anche chiederle di usarlo, per esempio "trasforma i miei appunti in una pagina HTML".

## Limiti

Le immagini nei documenti diventano un riferimento testuale nel Terminale. I colori seguono il tema del Terminale.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
