# ZenFactor Spa

## Il gioco

Una breve avventura tutorial: visiti un'azienda di Torino per scoprire cosa sono le avventure testuali.

- **Autore:** Tristano Ajmone (2010)
- **Lingua del gioco:** italiano
- **Formato:** Z-machine v8, interprete Frotz 2.55

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
dell'app sulla SD (`/sdcard/apps/if-zenfactor-spa/data`). `RESTORE` lo ricarica, anche dopo aver chiuso
il Terminale. `QUIT` esce dal gioco.

## Nel Terminale

- **Tocca l'icona** per aprire il Terminale con il gioco già avviato, oppure apri **Terminale** e scrivi `if-zenfactor-spa`.
- Scrivi nella riga in basso e premi Invio: la riga va al gioco.
- **STOP** interrompe subito il gioco (senza salvare).

## Crediti e licenza

**ZenFactor Spa** © Tristano Ajmone, 2010. Licenza: **Public domain**.
Testo della licenza: <https://ifarchive.org/indexes/if-archive/games/zcode/italian/>
Fonte del gioco: <https://ifdb.org/viewgame?id=kj5hyq3wkvl8x8yf>

> ZenFactor Spa, by Tristano Ajmone. Public domain. (IF Archive index, games/zcode/italian)

Interprete: **Frotz 2.55**, licenza GPL-2.0-or-later (<https://gitlab.com/DavidGriffith/frotz>). Gioco e interprete sono ridistribuiti invariati; NucleoOS aggiunge solo l'avvio e l'accesso al file della storia incorporato.
