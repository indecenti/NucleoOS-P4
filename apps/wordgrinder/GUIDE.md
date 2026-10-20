# WordGrinder, videoscrittura

## A cosa serve

WordGrinder è un programma per **scrivere**: lettere, relazioni, appunti, capitoli. Ti fa concentrare sul testo, senza distrazioni, ma ha quello che serve: titoli, paragrafi, elenchi puntati e numerati, **grassetto**, *corsivo*, sottolineato, trova e sostituisci, conteggio delle parole, annulla e ripeti, più documenti nello stesso file. Apre ed esporta **OpenDocument `.odt`** (LibreOffice, Word lo apre), **Markdown**, **HTML** e testo; esporta anche LaTeX, troff e Org.

## Come si apre

- **Tocca l'icona** in Home: si apre un documento vuoto.
- Da **File**, tocca un `.odt`, `.md`, `.html` o `.txt` e scegli **Apri con › WordGrinder**.
- Dal Terminale: `wordgrinder lettera.odt`.

Un `.odt`, `.md`, `.html` o `.txt` viene **importato**: quando salvi, WordGrinder scrive un file `.wg` accanto (il suo formato) e l'originale non viene toccato. Per riscriverlo in `.odt` usa **ESC › File › Export**.

## Tocco e tastiera

- **Tocca il testo** per spostare il cursore lì. **Trascina su e giù** per scorrere.
- **ESC** (nella riga di tasti sopra la tastiera) apre il **menu**: File, Edit, Style, Documents, Navigation. Ogni voce ha una lettera, e i comandi più usati hanno una scorciatoia con Ctrl.
- Con una tastiera USB o KeyDeck funzionano anche Shift+frecce (selezione) e Ctrl+frecce (parola per parola).

| Tasti | Cosa fanno |
|---|---|
| ESC | menu |
| Ctrl+S | salva |
| Ctrl+O | apre un documento |
| Ctrl+B / Ctrl+U | grassetto / sottolineato (corsivo: ESC › Style › Set italic) |
| Ctrl+N | testo normale |
| Ctrl+P | stile del paragrafo: titoli H1–H4, elenchi, citazioni |
| Ctrl+Z / Ctrl+Y | annulla / ripeti |
| Ctrl+X / Ctrl+C / Ctrl+V | taglia / copia / incolla |
| Ctrl+F / Ctrl+K | trova e sostituisci / trova il successivo |
| Ctrl+G | vai a una pagina o a un titolo |
| Ctrl+Q | esce |

I menu di WordGrinder sono in inglese.

## Con Anima

WordGrinder è un programma di sistema: Anima lo usa per leggere un `.odt` o trasformare appunti in un documento. Chiedi per esempio "leggimi il file relazione.odt" o "trasforma note.md in un documento odt".

## Limiti

- I file Word `.docx` e `.doc` non si aprono: salvali come `.odt`.
- Niente immagini, tabelle, colonne o impaginazione: è un programma per il testo. Le immagini di un `.odt` importato vanno perse.
- Il controllo ortografico non ha dizionari installati.
- Gli appunti sono interni al programma.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Convertire senza aprire: `wordgrinder --convert lettera.odt lettera.txt` (il formato lo decide l'estensione: `.odt` `.md` `.html` `.txt` `.wg`, in uscita anche `.tex`).
- Con `apps` il Terminale elenca i programmi installati, con `help wordgrinder` la sintassi.
