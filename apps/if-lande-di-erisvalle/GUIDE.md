# Le Lande di Erisvalle

## Il gioco

Esplora le lontane Lande di Erisvalle alla ricerca delle sette reliquie dell'antico regno e delle sette rune del potere.

- **Autore:** Paolo Lucchesi (2022)
- **Lingua del gioco:** italiano
- **Formato:** Glulx, interprete Glulxe + CheapGlk

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
dell'app sulla SD (`/sdcard/apps/if-lande-di-erisvalle/data`). `RESTORE` lo ricarica, anche dopo aver chiuso
il Terminale. `QUIT` esce dal gioco.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il gioco già avviato, oppure apri **Terminale** e scrivi `if-lande-di-erisvalle`.
- Scrivi nella riga in basso e premi Invio: la riga va al gioco.
- **STOP** interrompe subito il gioco (senza salvare).

## Crediti e licenza

**Le Lande di Erisvalle** © Paolo Lucchesi, 2022. Licenza: **GPL-2.0**.
Testo della licenza: <http://www.paololucchesi.it/at/erisvalle.html>
Fonte del gioco: <https://ifdb.org/viewgame?id=1jkbbgyvk7gyg0v>

> I file erisvalle.gblorb, erisvalle_nomouse.gblorb e erisvalle_acv.ulx sono distribuiti secondo i termini della Gnu Public License (GPL) vers. 2 (README of the release zip)

Interprete: **Glulxe + CheapGlk**, licenza MIT (<https://github.com/erkyrath/glulxe>). Gioco e interprete sono ridistribuiti invariati; NucleoOS aggiunge solo l'avvio e l'accesso al file della storia incorporato.
