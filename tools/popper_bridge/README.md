# nucleo_bridge: PinUP Popper ↔ scheda "DMD Flipper" via USB

Il ponte gira sul PC del cabinato, parte con Windows e pesa pochissimo (priorità bassa, ~27 MB, immagini
convertite una sola volta e tenute in cache, tutto il lavoro grafico lo fa la scheda). Cosa fa:

- **A tavoli chiusi** la scheda (7", touch) è un menu di PinUP Popper in stile Popper: tavolo selezionato
  con backglass, wheel, dati, punteggi, Gioca, istruzioni; barra delle wheel da trascinare; due temi
  (Cabinato e DMD).
- **Quando usi i comandi del cabinato in Popper** (il joystick), la scheda mostra la **vetrina** del tavolo
  selezionato: backglass, DMD e topper uno dopo l'altro. Volantini e istruzioni no: in Popper sono pagine
  verticali salvate di lato; le istruzioni si aprono dal pulsante Istruzioni, raddrizzate e da scorrere.
  Il ponte legge la selezione dalla Web Remote di Popper una volta al secondo, solo a tavoli chiusi.
- **Tavoli VPX**: il ponte cede la porta a dmdext e la scheda fa da DMD (ZeDMD), anche con il backglass
  sopra (modalità Backglass + DMD).
- **Pinball FX2, MAME, giochi PC e Godot**: non hanno un DMD per dmdext; il ponte tiene la porta e la scheda
  mostra la vetrina del gioco in corso, con i comandi rapidi via cavo.
- **In partita**: un tocco sulla scheda apre i comandi rapidi (indietro, tavoli, esci dal gioco,
  impostazioni). Per i tavoli VPX passano dal Wi-Fi direttamente alla Web Remote di Popper.

Il collegamento principale è la porta USB della scheda (la porta di programmazione, 303A:1001). Una porta
COM ha un solo proprietario alla volta: gli script di avvio e chiusura degli emulatori in Popper chiamano
il ponte, che cede e riprende la porta.

Studio, scelte e dettagli: `docs/DMD_POPPER.md` nel repository NucleoOS-P4.

## Cosa serve

- Python 3.8 o successivo, con `pip install pyserial pillow` (numpy facoltativo: rende le immagini più veloci).
- PinUP Popper (il ponte legge `PUPDatabase.db` in **sola lettura**, non scrive mai nulla).
- Il Web Remote di Popper attivo: `useWEB = true ;` in `PUPMenuScriptSysOptions.txt` e in
  `PuPMenuScript.pup` (le "opzioni script" di Popper), poi Popper riavviato. Il componente COM
  `PuPServer.WebServer` deve essere registrato (sul cabinato lo era già; altrimenti `PUPServerRegisterCOM.bat`
  come amministratore). Porta 80, altrimenti `--web-port`. Serve per Gioca, per uscire dal gioco
  (`/pupkey/15`) e per la vetrina (`/function/getcuritem`).
- dmdext con `[zedmd] enabled = true` in `DmdDevice.ini` (il DMD via USB).
- Sulla scheda: l'app **DMD Flipper** aperta (meglio con "Avvia all'accensione").

## Installazione

1. Copia la cartella `popper_bridge` sul PC, per esempio in `C:\vPinball\nucleo_bridge`.
2. `pip install pyserial pillow`
3. Controllo: `python nucleo_bridge.py --check --report report.txt`. Dice cosa trova: cartella di
   Popper, colonne del database, liste, media, Web Remote, porta della scheda, `DmdDevice.ini`.
4. In **Popper Setup → Emulators**, per **ogni** emulatore visibile (sul cabinato è già fatto):
   - **Launch Script**: come **prima** riga
     `curl -s -m 2 -G --data-urlencode "name=[GAMEFULLNAME]" http://127.0.0.1:58090/game/start >nul 2>&1`
   - **Close Script**: come **prima** riga
     `curl -s -m 2 http://127.0.0.1:58090/game/end >nul 2>&1`
   (`curl` è incluso in Windows 10 dal 1803. Se il ponte non gira, le righe non bloccano niente: falliscono
   in 2 secondi.)
5. Avvio con Windows: `powershell -ExecutionPolicy Bypass -File ponte.ps1 installa` (attività pianificata
   all'accesso, senza finestre, priorità bassa, riavviata se si chiude; `rimuovi` la toglie). Altri comandi:
   `avvia`, `ferma`, `riavvia`, `stato`, `controllo`. Registro: `%LOCALAPPDATA%\nucleo_bridge\ponte.log`
   (oltre 5 MB diventa `ponte.log.1`, ne resta una sola copia vecchia). `ferma` chiude anche il dmdext
   che il ponte ha avviato per il DMD di Future Pinball (altrimenti terrebbe occupata la porta della
   scheda); se il ponte viene chiuso in altro modo, al riavvio successivo chiude lui quel dmdext rimasto.
   Senza Python `stato` e `ferma` funzionano lo stesso.
   Riavviarlo durante una partita si può: all'avvio guarda una volta i processi e, se un tavolo è aperto,
   lo riconosce (riga "a game was already running" nel registro); con VPX lascia la porta a dmdext.
   Se Popper non avvisa la fine di un gioco (emulatore andato in crash o chiuso a forza), il ponte se ne
   accorge da solo quando il processo dell'emulatore termina (riga "exited without Popper's end hook").
   Chiude anche il server del backglass B2S rimasto aperto da solo dopo un crash di VPX (altrimenti Popper
   non avvia più nulla): riga "closed b2sbackglassserverexe" nel registro.
6. Prova: a tavoli chiusi la scheda mostra il menu. Tocca un tavolo, poi **Gioca**: il tavolo parte e la
   scheda passa al DMD. Esci dal tavolo: torna il menu.

## Cache e reattività

- Sul PC: ogni immagine è convertita una volta sola e tenuta in `%LOCALAPPDATA%\nucleo_bridge\cache`.
  Una volta per avvio (20 minuti dopo, solo a cabinato fermo) il ponte cancella i file della cache non
  usati da 60 giorni (versioni vecchie di immagini cambiate, tavoli tolti); i file `.json` (indici e
  composizioni del secondo schermo, `s2.json`) non vengono mai toccati.
- Le richieste della scheda sono servite una alla volta (le modifiche a `B2STableSettings.xml`,
  `DmdDevice.ini` e `s2.json` non si sovrappongono più); Gioca, Esci e il saluto hanno una coda propria,
  così una conversione d'immagine non li fa aspettare.
- Sulla scheda: ogni immagine ricevuta resta sulla microSD (`/sdcard/data/dmd`), insieme alle ultime liste.
  La scheda la rilegge da lì e poi chiede al ponte solo "è ancora questa?" (la chiave dell'immagine): il
  ponte risponde in pochi byte senza convertire nulla, o manda quella nuova se in Popper è cambiata.
- A menu fermo la scheda si scarica una alla volta le wheel della lista: dopo la prima volta è istantaneo.

## Opzioni

```
python nucleo_bridge.py [--popper CARTELLA] [--port COMx] [--web-port 80]
python nucleo_bridge.py --check [--report FILE]
```

`http://127.0.0.1:58090/status` dice se il ponte ha la porta USB e se c'è una partita in corso.

## Note

- Il ponte apre la porta con DTR e RTS bassi: sulla porta USB-JTAG dell'ESP32-P4 certe sequenze di
  DTR/RTS resettano la scheda.
- Le immagini (wheel, istruzioni GameHelp) arrivano alla scheda già ridimensionate, in RGB565.
- Nomi di tavolo con accenti: lo script di Popper li passa a curl nella codifica del batch (UTF-8, cp1252
  o cp850); il ponte le prova tutte e confronta i nomi senza maiuscole né accenti.
- Le categorie del filtro "Vai a" arrivano alla scheda nel messaggio di saluto (`H`, campo `emus`): ognuna
  ha il nome mostrato (`name`, in italiano) e una chiave fissa ASCII (`key`: `vpx`, `fp`, `fx`, `shooter`,
  `maze`, `platform`, `sport`, `arcade`, `pc`) per tradurla sulla scheda. In `categories.json` si può dare
  `key`; senza, è il nome senza accenti in minuscolo.
- Sono esclusi i tavoli nascosti (`Visible` diverso da 1). Preferiti: `PlayListDetails.isFav` = 1
  (playlist) o 2 (globali).
- Prova senza cabinato: `python make_test_popper.py C:\temp\FakePUP --serve 8099` crea un finto Popper
  (database, immagini, Web Remote finto), poi `python nucleo_bridge.py --popper C:\temp\FakePUP --web-port 8099`.
