# zstd (con gzip e xz)

## A cosa serve

Comprime e decomprime file in tre formati: Zstandard (`.zst`, veloce e compatto), gzip (`.gz`, il più diffuso) e xz (`.xz`, quello dei pacchetti Linux). Lo stesso programma risponde ai comandi classici: `gzip`, `gunzip`, `zcat`, `xz`, `unxz`, `xzcat`, `zstd`, `unzstd`, `zstdcat`.

## Come si usa

| Comando | Cosa fa |
|---|---|
| `gunzip dati.csv.gz` | decomprime (e toglie il `.gz`); `-k` tiene anche l'originale |
| `zcat log.gz \| grep errore` | legge un file compresso senza scompattarlo |
| `gzip -k note.txt` | crea `note.txt.gz` tenendo l'originale |
| `unxz archivio.xz` / `xzcat file.xz` | lo stesso per i `.xz` |
| `xz -k foto.bmp` | comprime in `.xz` |
| `zstd -19 backup.sqlite` | comprime al massimo in `.zst` |
| `zstd -d backup.sqlite.zst` | decomprime |
| `zstd --list file.zst` | dimensioni e rapporto di compressione |

## Con Anima

È un programma di sistema: Anima lo usa da sola, per esempio per aprire un file `.gz` scaricato. Puoi chiederle "scompatta il file che ho scaricato".

## Limiti

Non c'è `tar`: un `.tar.gz` diventa un `.tar` e non viene estratto in cartelle (per gli archivi `.zip` c'è `zip`/`unzip`). La compressione xz è limitata al livello 1 per stare nella memoria di un'app (un file `.xz` compresso altrove si apre comunque). Nessuna compressione su più thread.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il programma già avviato, oppure apri **Terminale** e scrivi il comando.
- Scrivi nella riga in basso e premi Invio: la riga va al programma.
- **EOF** chiude l'input (come Ctrl-D): molti programmi a quel punto finiscono. **STOP** lo interrompe subito.
- I tuoi file stanno nella cartella **home** della scheda SD (`/sdcard/home`): per il programma quella cartella è `/`. Puoi copiarci file dal PC mettendo la SD nel lettore.
- Con `apps` il Terminale elenca i programmi installati, con `help` i suoi comandi.
