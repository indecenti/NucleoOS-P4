# Edge Valley Bigfoot Society

## Il gioco

Un Bigfoot più sveglio del solito viene catturato dai cacciatori: riuscirà a fuggire? Sei finali.

- **Autore:** NegSec (2025)
- **Lingua del gioco:** inglese
- **Formato:** Glulx (Blorb), interprete Glulxe + CheapGlk

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
dell'app sulla SD (`/sdcard/apps/if-bigfoot-society/data`). `RESTORE` lo ricarica, anche dopo aver chiuso
il Terminale. `QUIT` esce dal gioco.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il gioco già avviato, oppure apri **Terminale** e scrivi `if-bigfoot-society`.
- Scrivi nella riga in basso e premi Invio: la riga va al gioco.
- **STOP** interrompe subito il gioco (senza salvare).

## Crediti e licenza

**Edge Valley Bigfoot Society** © NegSec, 2025. Licenza: **GPL (version not stated)**.
Testo della licenza: <https://ifarchive.org/indexes/if-archive/games/glulx/>
Fonte del gioco: <https://ifdb.org/viewgame?id=vefh91bon5y6jfp0>

> Edge Valley Bigfoot Society, by NegSec. Version 1.0.2 (Release 1 / Serial number 251022). General Public License. (IF Archive index, games/glulx)

Interprete: **Glulxe + CheapGlk**, licenza MIT (<https://github.com/erkyrath/glulxe>). Gioco e interprete sono ridistribuiti invariati; NucleoOS aggiunge solo l'avvio e l'accesso al file della storia incorporato.
