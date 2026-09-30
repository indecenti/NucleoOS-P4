# Shade

## Il gioco

Un gioco in una sola stanza, il tuo appartamento, la notte prima di un viaggio nel deserto.

- **Autore:** Andrew Plotkin (2000)
- **Lingua del gioco:** inglese
- **Formato:** Z-machine v5, interprete Frotz 2.55

## Come si gioca

È un'**avventura testuale** (interactive fiction): il gioco descrive dove sei, tu scrivi cosa fare
in frasi brevi, in inglese, e premi Invio. Si comincia quasi sempre guardandosi intorno.

| Comando | Abbreviazione | Cosa fa |
|---|---|---|
| `LOOK` | `L` | descrive dove sei |
| `INVENTORY` | `I` | cosa porti con te |
| `NORTH, SOUTH, EAST, WEST, UP, DOWN` | `N S E W U D` | ti sposti (anche NE, NW, SE, SW, IN, OUT) |
| `EXAMINE lamp` | `X LAMP` | guarda da vicino un oggetto |
| `TAKE lamp / DROP lamp` | `GET` | prendi / lascia |
| `OPEN door, PUSH button, TALK TO man` |  | agisci su cose e persone |
| `AGAIN` | `G` | ripete l'ultimo comando |
| `WAIT` | `Z` | lascia passare il tempo |
| `SAVE / RESTORE` |  | salva / carica la partita |
| `UNDO` |  | annulla una mossa |
| `QUIT` | `Q` | esci dal gioco |

Consigli: esamina tutto (`EXAMINE` / `X`), prendi quello che puoi, prova le direzioni elencate nelle
descrizioni, e se ti blocchi rileggi la stanza con `LOOK`. Molti giochi capiscono `HELP` o `HINT`.
Con `UNDO` torni indietro di una mossa.

## Salvare la partita

`SAVE` chiede un nome di file (Invio accetta quello proposto) e lo scrive nella cartella dati
dell'app sulla SD (`/sdcard/apps/if-shade/data`). `RESTORE` lo ricarica, anche dopo aver chiuso
il Terminale. `QUIT` esce dal gioco.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il gioco già avviato, oppure apri **Terminale** e scrivi `if-shade`.
- Scrivi nella riga in basso e premi Invio: la riga va al gioco.
- **STOP** interrompe subito il gioco (senza salvare).

## Crediti e licenza

**Shade** © Andrew Plotkin, 2000. Licenza: **Free distribution, non-commercial (author's permission)**.
Testo della licenza: <https://ifarchive.org/if-archive/games/source/inform/shade-src.tar.Z>
Fonte del gioco: <https://ifdb.org/viewgame?id=hsfc7fnl40k4a30q>

> SHADE may be distributed for free, but not sold or included in any for-profit collection without written permission from the author.

Interprete: **Frotz 2.55**, licenza GPL-2.0-or-later (<https://gitlab.com/DavidGriffith/frotz>). Gioco e interprete sono ridistribuiti invariati; NucleoOS aggiunge solo l'avvio e l'accesso al file della storia incorporato.
