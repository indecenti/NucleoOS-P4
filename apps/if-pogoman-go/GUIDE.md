# Pogoman GO!

## Il gioco

Niente lavoro, niente famiglia: è ora di catturare tutti i Pogoman della città. Satira dei giochi per telefono.

- **Autore:** Jack Welch & Ben Collins-Sussman (2016)
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
dell'app sulla SD (`/sdcard/apps/if-pogoman-go/data`). `RESTORE` lo ricarica, anche dopo aver chiuso
il Terminale. `QUIT` esce dal gioco.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il gioco già avviato, oppure apri **Terminale** e scrivi `if-pogoman-go`.
- Scrivi nella riga in basso e premi Invio: la riga va al gioco.
- **STOP** interrompe subito il gioco (senza salvare).

## Crediti e licenza

**Pogoman GO!** © Jack Welch & Ben Collins-Sussman, 2016. Licenza: **CC BY-NC-SA 3.0** (solo uso non commerciale).
Testo della licenza: <https://github.com/sussman/pogoman-go>
Fonte del gioco: <https://ifdb.org/viewgame?id=hc6dudpdc9i2pmja>

> The code is released under the 'Creative Commons Attribution - Noncommercial - Share Alike 3.0'

Interprete: **Glulxe + CheapGlk**, licenza MIT (<https://github.com/erkyrath/glulxe>). Il file della storia è ridistribuito senza le immagini, che qui non si possono mostrare (resta il codice di gioco), come la licenza consente; l'interprete è invariato.
