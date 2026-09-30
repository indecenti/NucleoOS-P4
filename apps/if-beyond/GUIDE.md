# Beyond (Aldilà)

## Il gioco

Una morte misteriosa e un segreto da svelare, tra scatole di vetro, nell'aldilà. Versione solo testo.

- **Autore:** Roberto Grassi, Paolo Lucchesi & Alessandro Peretti (2005)
- **Lingua del gioco:** italiano
- **Formato:** Z-machine (Blorb), interprete Frotz 2.55

## Come si gioca

È un'**avventura testuale** (interactive fiction): il gioco descrive dove sei, tu scrivi cosa fare
in frasi brevi, in italiano, e premi Invio. Si comincia quasi sempre guardandosi intorno.

| Comando | Abbreviazione | Cosa fa |
|---|---|---|
| `GUARDA` | `G / L` | descrive dove sei |
| `INVENTARIO` | `I` | cosa porti con te |
| `NORD, SUD, EST, OVEST, SU, GIÙ` | `N S E O ALTO BASSO` | ti sposti |
| `ESAMINA lampada` | `X / ESA` | guarda da vicino un oggetto |
| `PRENDI lampada / LASCIA lampada` |  | prendi / lascia |
| `APRI porta, PARLA CON uomo` |  | agisci su cose e persone |
| `SALVA / CARICA` | `SAVE / RESTORE` | salva / carica la partita |
| `FINE` | `QUIT` | esci dal gioco |

Consigli: esamina tutto (`EXAMINE` / `X`), prendi quello che puoi, prova le direzioni elencate nelle
descrizioni, e se ti blocchi rileggi la stanza con `LOOK`. Molti giochi capiscono `HELP` o `HINT`.
Con `UNDO` torni indietro di una mossa.

## Salvare la partita

`SAVE` chiede un nome di file (Invio accetta quello proposto) e lo scrive nella cartella dati
dell'app sulla SD (`/sdcard/apps/if-beyond/data`). `RESTORE` lo ricarica, anche dopo aver chiuso
il Terminale. `QUIT` esce dal gioco.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il gioco già avviato, oppure apri **Terminale** e scrivi `if-beyond`.
- Scrivi nella riga in basso e premi Invio: la riga va al gioco.
- **STOP** interrompe subito il gioco (senza salvare).

## Crediti e licenza

**Beyond (Aldilà)** © Roberto Grassi, Paolo Lucchesi & Alessandro Peretti, 2005. Licenza: **CC BY-NC-ND 2.5** (solo uso non commerciale).
Testo della licenza: <https://creativecommons.org/licenses/by-nc-nd/2.5/>
Fonte del gioco: <https://ifdb.org/viewgame?id=80s6vtj6yjwmt7sn>

> Beyond e' rilasciato sotto sotto i termini della licenza CreativeCommons Attribuzione - Non commerciale - Non opere derivate versione 2.5. (README of the IF Archive release)

Interprete: **Frotz 2.55**, licenza GPL-2.0-or-later (<https://gitlab.com/DavidGriffith/frotz>). Gioco e interprete sono ridistribuiti invariati; NucleoOS aggiunge solo l'avvio e l'accesso al file della storia incorporato.
