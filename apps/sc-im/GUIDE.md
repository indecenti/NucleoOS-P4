# sc-im, foglio di calcolo

> **Il foglio di calcolo con tocco, mouse e ANIMA è l'app Fogli** (nella Home): sc-im è il suo motore di calcolo e si installa con il sistema. Questa guida è per chi preferisce sc-im da tastiera, nel Terminale.

## A cosa serve

sc-im è un foglio di calcolo vero, a schermo intero: celle con numeri, testi e formule che si ricalcolano da sole, funzioni (`@sum`, `@avg`, `@max`, `@if`, date e ore), formati dei numeri, larghezza delle colonne, ordinamento, filtri, annulla e ripeti. Apre file **Excel `.xlsx` e `.xls`**, **LibreOffice `.ods`**, **`.csv`** e **`.tsv`**, e salva in `.xlsx`, `.csv` e `.tsv`: un elenco di spese, un inventario, un piano rate, un foglio ricevuto per mail.

## Come si apre

- **Tocca l'icona** in Home: si apre il foglio vuoto.
- Da **File**, tocca un `.xlsx`, `.xls`, `.ods` o `.csv` e scegli **Apri con › sc-im**.
- Dal Terminale: `sc-im spese.xlsx`.

## Tocco e tastiera

- **Tocca una cella** per selezionarla. **Trascina su e giù** per scorrere il foglio.
- La riga di tasti sopra la tastiera ha **Esc**, **Tab**, **Ctrl** e le **frecce**.
- Con una tastiera USB o KeyDeck funzionano anche Ctrl, Shift e i tasti funzione.

## I comandi principali

sc-im si usa come l'editor vim: una lettera è un comando, i comandi lunghi cominciano con i due punti.

| Tasti | Cosa fanno |
|---|---|
| frecce | spostano la selezione |
| `=` | scrivi un numero o una formula, es. `=@sum(B0:B9)` poi Invio |
| `<` `>` `\` | scrivi un testo allineato a sinistra, a destra, centrato |
| `e` / `E` | modifica il numero / il testo della cella |
| `x` | cancella la cella |
| `u` / Ctrl+R | annulla / ripeti |
| `yy` poi `p` | copia la cella e incollala |
| `ir` `ic` | inserisci una riga / una colonna |
| `dr` `dc` | elimina la riga / la colonna |
| `f>` `f<` | colonna più larga / più stretta |
| `f+` `f-` | più / meno decimali |
| `goB12` | vai alla cella B12 |
| `:w` | salva; `:w conti.xlsx` salva come Excel |
| `:e csv conti.csv` | esporta in CSV (anche `tab`, `txt`, `mkd`) |
| `:load file.ods` | apre un altro file |
| `:q` | esce (`:q!` senza salvare) |
| `:h` | il manuale completo (`q` per chiuderlo) |

Le celle si chiamano con la colonna e la riga, **contando le righe da 0**: `A0` è la prima cella in alto a sinistra. Nelle formule gli intervalli si scrivono `A0:A9`.

## Con Anima

sc-im è un programma di sistema: Anima lo usa per leggere i tuoi fogli. Chiedi per esempio "quanto fa il totale nel file spese.xlsx?" o "riassumi la tabella ordini.ods".

## Limiti

- Grafici, macro, immagini e commenti dei file Excel non vengono letti; di un `.xlsx` si leggono valori, testi e (con l'opzione `--xlsx_readformulas`) le formule.
- I file `.ods` si aprono ma si salvano in `.xlsx`, `.csv` o nel formato di sc-im (`.sc`).
- Di un vecchio `.xls` si legge il primo foglio; si salva in `.xlsx`.
- Gli appunti di sistema e i comandi `!` (programmi esterni) non sono disponibili.
- Fogli molto grandi (decine di migliaia di righe) possono non entrare nella memoria del dispositivo: sc-im lo dice e il file originale resta intatto, perché salva prima in un file temporaneo.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Leggere un foglio senza aprirlo: `sc-im --nocurses --quit_afterload --export_csv spese.xlsx` (con `--export_mkd` esce una tabella).
- Con `apps` il Terminale elenca i programmi installati, con `help sc-im` la sintassi.

---

*This product includes software developed by Andrés Martinelli <andmarti@gmail.com>.*
