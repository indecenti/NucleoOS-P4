---
name: nucleo-dmd
description: Il DMD del cabinato (scheda NucleoOS "DMD Flipper", ESP32-P4 7" touch su USB) e il ponte che la collega a PinUP Popper. Usala quando l'utente parla del DMD, della scheda, del menu touch dei tavoli, del ponte, di dmdext/ZeDMD o di problemi con il display del punteggio. Questo PC serve solo al pinball: ogni cosa deve pesare il meno possibile sulla CPU.
---

# DMD del cabinato e ponte Popper

Questo PC (DESKTOP-NVR308C, utente `pinball`) serve **solo** al pinball virtuale: VPX 10.8.1, VPinMAME,
dmdext 2.5, PinUP Popper in `C:\vPinball`. È un PC vecchio: non installare nulla che non sia
indispensabile, nessun processo in più in background, niente lavori pesanti mentre un tavolo è aperto.
Quello che si può far fare alla scheda (disegno dei punti, decodifica, scalatura) lo fa la scheda.

## Com'è fatto

- **La scheda** è un ESP32-P4 con schermo touch 1024x600 (Guition JC1060P470C), NucleoOS e l'app
  "DMD Flipper" che parte all'accensione. Un solo cavo USB-C a una porta USB del PC, **senza hub**
  (con l'hub perdeva corrente e si riavviava). In Windows è "Dispositivo seriale USB (COMx)", USB
  `303A:1001` (di solito COM3). Ha anche il Wi-Fi, ma il collegamento usato è il cavo.
- **Il DMD in partita**: VPinMAME carica `C:\vPinball\VisualPinball\VPinMAME\DmdDevice.dll` (dmdext 2.5),
  che con `[zedmd] enabled = true` in `DmdDevice.ini` (stessa cartella) trova la scheda come ZeDMD e le
  manda i frame. Backup dell'ini originale: `DmdDevice.ini.prima-di-nucleo`.
- **Il menu touch** (a tavoli chiusi: recenti, novità, più giocati, preferiti, tutti, "sorprendimi",
  scheda del tavolo con punteggi, istruzioni, GIOCA) lo serve **il ponte**:
  `C:\Users\pinball\nucleo_bridge\nucleo_bridge.py`. Legge `PUPDatabase.db` di Popper **in sola
  lettura** e parla con la scheda sulla stessa porta USB.
- Una porta COM ha un solo proprietario: quando parte un tavolo **VPX**, gli hook di Popper dicono al ponte
  di cedere la porta a dmdext; a fine partita il ponte la riprende e la scheda torna al menu. Pinball FX2,
  MAME, giochi PC e i giochi Godot (HAKKEN, Downwell, Upwhell) non usano dmdext: il ponte tiene la porta e
  la scheda mostra la **vetrina** del gioco (i suoi materiali orizzontali) con i comandi rapidi.
- **Vetrina**: quando qualcuno usa il joystick in Popper, il ponte (che legge la selezione una volta al
  secondo dalla Web Remote) manda il tavolo alla scheda, che ne mostra backglass, DMD e topper. Volantini e
  istruzioni di Popper sono pagine verticali salvate di lato: la scheda mostra le istruzioni solo dal pulsante
  Istruzioni, raddrizzate dal ponte e da scorrere col dito. Un tocco sulla vetrina apre il menu su quel tavolo.
- **In partita** un tocco sulla scheda apre i comandi rapidi: indietro, tavoli, esci dal gioco,
  impostazioni. Avviare un altro tavolo chiede conferma e chiude prima quello in corso.
- **Web Remote di Popper** attiva (`useWEB = true` in `C:\vPinball\PinUPSystem\PUPMenuScriptSysOptions.txt`
  e `PuPMenuScript.pup`, backup `.prima-di-nucleo`), porta 80: `/function/getcuritem` (selezione),
  `/function/launchgame/<GameID>` (avvia), `/pupkey/15` (chiude il gioco in corso).
- **Modalità della scheda** (pannello del DMD → Adatta): DMD a tutto schermo, Solo DMD, Backglass + DMD,
  Solo backglass. Temi del menu: Cabinato, DMD.

## Il ponte è sempre acceso (parte con Windows)

Attività pianificata "NucleoOS DMD ponte": all'accesso di Windows, pythonw senza finestre, priorità
bassa, riavviata se si chiude. Pesa pochissimo: a riposo si sveglia una volta al secondo (~25 MB di RAM);
le immagini le converte una volta sola (cache su disco) e solo a tavoli chiusi; in partita dorme.

```
powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\pinball\nucleo_bridge\ponte.ps1 stato
powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\pinball\nucleo_bridge\ponte.ps1 riavvia
powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\pinball\nucleo_bridge\ponte.ps1 controllo
```

- `stato`: acceso/spento, CPU e memoria usate, avvio con Windows sì/no, porta della scheda, registro.
- `avvia` / `ferma` / `riavvia`: a mano (per esempio per aggiornare il ponte o il firmware).
- `installa` / `rimuovi`: mette o toglie l'avvio con Windows. Deve restare installato.
- `controllo`: verifica completa (Python, Popper, database, liste, immagini, Web Remote, porta,
  DmdDevice.ini), scritta anche in `%LOCALAPPDATA%\nucleo_bridge\controllo.txt`.
- Registro: `%LOCALAPPDATA%\nucleo_bridge\ponte.log`. Cache immagini: `%LOCALAPPDATA%\nucleo_bridge\cache`
  (si può svuotare).
- Python: `C:\Users\pinball\AppData\Local\Programs\Python\Python312` (pyserial, pillow, esptool).

## Hook di Popper

Negli script di avvio (LaunchScript) e chiusura (PostScript) degli emulatori in Popper c'è una riga
`curl ... http://127.0.0.1:58090/game/start` / `/game/end`, preceduta da un commento "NucleoOS DMD".
Se il ponte non risponde falliscono in un attimo e non bloccano nulla. Gli script di VPX sono già
personalizzati (attesa tavolo pronto, BGFX, BGFX32): non toccare il resto. Prima di modificare il
database di Popper: Popper chiuso e copia di `PUPDatabase.db` (le copie fatte finora si chiamano
`PUPDatabase.db.prima-di-nucleo-*`).

## Problemi tipici

- **La scheda si riavvia da sola**: quasi sempre alimentazione. Cavo diretto al PC, niente hub non
  alimentati.
- **dmdext: "Device ZeDMD is not available"**: scheda scollegata, oppure porta occupata (un tavolo
  lanciato senza hook mentre il ponte la teneva, o un `dmdext.exe` rimasto aperto:
  `tasklist | findstr dmdext`). Prova del DMD senza aprire tavoli, a ponte fermo:
  `cd /d C:\vPinball\FuturePinball && dmdext.exe test -d zedmd --quit-after 8000`
  (sempre `--quit-after`, altrimenti resta acceso e tiene la porta), poi `ponte.ps1 avvia`.
- **Il menu non compare**: un tavolo è aperto, oppure il ponte si è fermato (`stato`, poi `riavvia`).
- **Popper non avvia più i tavoli**: quasi sempre un `B2SBackglassServerEXE` rimasto aperto da solo dopo un
  crash di VPX (`tasklist | findstr /I "b2s vpinball"`). Il ponte lo chiude da solo quando l'emulatore esce
  (riga "closed b2sbackglassserverexe" in `ponte.log`); se resta, e nessun VPX è aperto:
  `taskkill /F /IM B2SBackglassServerEXE.exe`.
- Non aprire la porta della scheda con monitor seriali o strumenti che alzano DTR/RTS.

## Aggiornare il firmware della scheda (solo se te lo chiedono, e solo dopo un sì esplicito)

Il file `nucleos-anima.bin` arriva da chi sviluppa NucleoOS. `ponte.ps1 ferma`; sulla scheda esci
dall'app DMD (mentre è aperta il reset via USB è bloccato apposta, perché dmdext non la riavvii);
poi `python -m esptool --chip esp32p4 -p COM3 -b 460800 write-flash 0x120000 nucleos-anima.bin`
e infine `ponte.ps1 avvia`.

## Non fare

- Non togliere l'avvio con Windows del ponte e non aggiungere altri processi in background.
- Non scrivere nel database di Popper senza backup e senza averlo detto all'utente.
- Non lasciare accesi processi di prova (dmdext test, Python) quando hai finito.
- Non scrivere mai il firmware sulla scheda senza aver chiesto e ricevuto un sì.
