# DMD Flipper — Guida per l'utente

**DMD Flipper** trasforma la scheda NucleoOS (ESP32-P4 con schermo touch da 1024x600) nel DMD del tuo
cabinato di flipper virtuale e, quando nessun tavolo è in gioco, in un menu touch dei tavoli di
PinUP Popper.

Questa guida è per chi possiede il cabinato: spiega come collegare la scheda, come installare il piccolo
programma "ponte" sul PC e come usare ogni schermata. Non serve essere programmatori.

![L'orologio a puntini quando il PC tace](../../screenshots/dmd_clock.jpg)

---

## Indice

1. [Cos'è e cosa serve](#1-cosè-e-cosa-serve)
2. [Avvio rapido in 5 passi](#2-avvio-rapido-in-5-passi)
3. [Collegare il PC](#3-collegare-il-pc)
4. [Installare il ponte per PinUP Popper](#4-installare-il-ponte-per-pinup-popper)
5. [Il menu dei tavoli](#5-il-menu-dei-tavoli)
6. [Durante la partita](#6-durante-la-partita)
7. [Il pannello delle impostazioni](#7-il-pannello-delle-impostazioni)
8. [La schermata Composizione](#8-la-schermata-composizione)
9. [Risoluzione dei problemi](#9-risoluzione-dei-problemi)
10. [Aggiornare il firmware in sicurezza](#10-aggiornare-il-firmware-in-sicurezza)
11. [Limiti noti](#11-limiti-noti)
12. [Privacy e rete](#12-privacy-e-rete)

---

## 1. Cos'è e cosa serve

L'app fa due lavori:

- **DMD del cabinato.** Durante una partita il PC (Visual Pinball X + VPinMAME + dmdext) manda il DMD alla
  scheda, che lo disegna come puntini LED/plasma. I fotogrammi possono arrivare via **USB** o via **Wi-Fi**,
  anche tutti e due insieme.
- **Menu touch di PinUP Popper.** A tavoli chiusi la scheda mostra i tuoi tavoli (recenti, novità, più
  giocati, preferiti, tutti) con wheel, backglass, statistiche e punteggi: tocchi **Gioca** e il tavolo
  parte sul cabinato. Per questo serve il **ponte**, un piccolo programma che gira sul PC (capitolo 4).

### Cosa serve

| Cosa | Dettagli |
|---|---|
| Scheda NucleoOS | Con l'app **DMD Flipper**. La microSD è facoltativa: se c'è, la scheda ci tiene le immagini e le ultime liste e il menu diventa istantaneo; senza, il menu funziona lo stesso, ma le immagini passano dal cavo ogni volta e nulla resta da una sessione all'altra. |
| Cavo USB | Dalla porta USB di programmazione della scheda (Windows la vede come dispositivo USB 303A:1001) **direttamente al PC**. Niente hub senza alimentazione: la scheda resterebbe senza corrente e si riavvierebbe. |
| PC del cabinato | Windows 10 (versione 1803 o successiva) o 11, Visual Pinball X, VPinMAME con **dmdext 2.x** (il suo `DmdDevice.dll`). |
| PinUP Popper | Per il menu dei tavoli, con il suo **Web Remote** attivo (capitolo 4). |
| Python | 3.8 o successivo, per il ponte. |
| Wi-Fi | Facoltativo per il DMD (basta l'USB), **consigliato** con Popper: serve per uscire da un tavolo VPX o avviarne un altro dalla scheda mentre si gioca. |

---

## 2. Avvio rapido in 5 passi

1. **Collega** la scheda al PC con il cavo USB, direttamente (niente hub senza alimentazione).
2. **Apri DMD Flipper** sulla scheda. Tieni premuto lo schermo 2 secondi, apri il pannello e attiva
   **Avvia all'accensione**: l'app deve essere già aperta quando parte un tavolo.
3. **Sul PC**, nel `DmdDevice.ini` di dmdext, attiva l'uscita USB:
   ```ini
   [zedmd]
   enabled = true
   ```
4. **Installa il ponte** per Popper (capitolo 4): copia la cartella, `pip install pyserial pillow`, aggiungi
   le due righe agli script degli emulatori in Popper, poi
   `powershell -ExecutionPolicy Bypass -File ponte.ps1 installa`.
5. **Prova**: a tavoli chiusi la scheda mostra il menu. Scegli un tavolo, tocca **Gioca**: il tavolo parte e
   la scheda passa al DMD. Esci dal tavolo: torna il menu.

---

## 3. Collegare il PC

La scheda accetta il DMD in due modi, attivi contemporaneamente. Si sceglie nel `DmdDevice.ini` di dmdext sul
PC (il file accanto a `DmdDevice.dll` di VPinMAME). La pagina di attesa dell'app mostra entrambe le
configurazioni, con l'indirizzo esatto della tua scheda.

### USB (consigliato)

```ini
[zedmd]
enabled = true
```

- La scheda si presenta a dmdext come un **ZeDMD 128x32**: dmdext cerca da solo tra le porte seriali e la
  trova. Se vuoi fissare la porta, aggiungi sotto `[zedmd]` la riga `port = COMx` (il numero della porta
  COM della scheda in Gestione dispositivi; `ponte.ps1 stato` lo mostra alla riga "Scheda").
- **L'app deve essere aperta quando il tavolo parte**: dmdext cerca il dispositivo in quel momento. Per
  questo conviene **Avvia all'accensione** (sotto).
- DMD di altre dimensioni (per esempio 128x16 o 192x64) vengono adattati da dmdext a 128x32.
- Con il ponte per Popper, l'USB è anche il canale del menu: il ponte e dmdext si passano la porta a ogni
  inizio e fine partita, da soli.

### Wi-Fi

```ini
[networkstream]
enabled = true
url = ws://<IP della scheda>:9090/dmd
retry = true
```

- L'indirizzo esatto (`ws://…:9090/dmd`) è scritto sulla pagina di attesa dell'app e in cima al pannello
  delle impostazioni, sotto la riga di stato.
- Via Wi-Fi la scheda mostra il DMD alla sua dimensione vera, fino a 512x256.
- Si collega un PC alla volta: una nuova connessione sostituisce la precedente.
- La porta 9090 si può cambiare nel pannello (**Porta di rete**); con `retry = true` il PC si ricollega da solo.

> **Suggerimento — IP fisso.** Nel router dai alla scheda un indirizzo fisso (prenotazione DHCP): così l'URL
> nel `DmdDevice.ini` non cambia mai.

### Avvia all'accensione

Nel pannello (sezione **Sistema**) attiva **Avvia all'accensione** (spento di serie): un secondo dopo
l'accensione NucleoOS apre direttamente DMD Flipper. Se è attiva la schermata di blocco, l'app non si apre da
sola finché non la sblocchi: su un cabinato conviene non usare il blocco.

---

## 4. Installare il ponte per PinUP Popper

### Cosa fa

Il ponte (`nucleo_bridge.py`) gira sul PC del cabinato, parte con Windows e pesa pochissimo (priorità
bassa, circa 27 MB, nessuna finestra). Tutto il lavoro grafico lo fa la scheda.

- **A tavoli chiusi** manda alla scheda le liste dei tavoli, le schede dei tavoli e le immagini (convertite
  una sola volta e tenute in cache).
- **Quando parte un tavolo VPX** cede la porta USB a dmdext: la scheda fa da DMD. A fine partita la riprende.
- **Tavoli Future Pinball con un DMD**: il ponte lo copia sulla scheda con la modalità mirror di dmdext.
- **Pinball FX, MAME, giochi PC**: il ponte tiene la porta e la scheda mostra la vetrina del gioco in corso,
  con i comandi rapidi.
- **Avvio e uscita** dei tavoli passano dal Web Remote di Popper.
- Legge il database di Popper (`PUPDatabase.db`) **in sola lettura**: non vi scrive mai nulla. I tavoli
  nascosti in Popper non compaiono.
- Se Popper non avvisa la fine di un gioco (emulatore andato in crash o chiuso a forza), il ponte se ne
  accorge da solo quando il processo dell'emulatore termina e il menu torna.
- Chiude il server del backglass B2S rimasto aperto da solo dopo un crash di VPX (altrimenti Popper non
  avvierebbe più nulla).
- Si può riavviare anche durante una partita: riconosce il tavolo aperto.

### Requisiti

- Python 3.8 o successivo, con `pip install pyserial pillow` (numpy è facoltativo: rende le immagini più veloci).
- PinUP Popper, con il **Web Remote** attivo:
  - `useWEB = true ;` in `PUPMenuScriptSysOptions.txt` e in `PuPMenuScript.pup` (le "opzioni script" di
    Popper), poi riavvia Popper;
  - il componente COM `PuPServer.WebServer` registrato (se non lo è: `PUPServerRegisterCOM.bat` come
    amministratore);
  - porta 80 (se Popper usa un'altra porta, avvia il ponte con `--web-port`).
  Il Web Remote serve per **Gioca**, per uscire dal gioco e per la vetrina.
- dmdext con `[zedmd] enabled = true` nel `DmdDevice.ini`.
- Sulla scheda: l'app **DMD Flipper** aperta (meglio con **Avvia all'accensione**).

### Installazione

1. Copia la cartella `popper_bridge` sul PC, per esempio in `C:\vPinball\nucleo_bridge`.
2. Installa le librerie:
   ```
   pip install pyserial pillow
   ```
3. Controlla che trovi tutto:
   ```
   python nucleo_bridge.py --check --report report.txt
   ```
   Il rapporto dice cosa trova: cartella di Popper, colonne del database, liste, media, Web Remote, porta
   della scheda, `DmdDevice.ini`.
4. In **Popper Setup → Emulators**, per **ogni** emulatore visibile, aggiungi come **prima** riga:
   - nel **Launch Script**:
     ```
     curl -s -m 2 -G --data-urlencode "name=[GAMEFULLNAME]" http://127.0.0.1:58090/game/start >nul 2>&1
     ```
   - nel **Close Script**:
     ```
     curl -s -m 2 http://127.0.0.1:58090/game/end >nul 2>&1
     ```
   `curl` è incluso in Windows 10 dalla versione 1803. Se il ponte non gira, queste righe non bloccano
   niente: falliscono in 2 secondi.
5. Fai partire il ponte con Windows (dalla cartella del ponte):
   ```
   powershell -ExecutionPolicy Bypass -File ponte.ps1 installa
   ```
   Crea un'attività pianificata all'accesso di Windows, senza finestre, a priorità bassa, riavviata se si
   chiude; e avvia subito il ponte.
6. Prova: a tavoli chiusi la scheda mostra il menu. Tocca un tavolo, poi **Gioca**.

### Comandi di `ponte.ps1`

Tutti si lanciano così, dalla cartella del ponte:
`powershell -ExecutionPolicy Bypass -File ponte.ps1 <comando>`

| Comando | Cosa fa |
|---|---|
| `installa` | Avvio con Windows (attività pianificata) e avvio immediato |
| `rimuovi` | Toglie l'avvio con Windows e ferma il ponte |
| `avvia` | Avvia il ponte |
| `ferma` | Ferma il ponte (e un eventuale dmdext avviato da lui): la porta della scheda è libera |
| `riavvia` | Ferma e riavvia |
| `stato` | Acceso/spento, CPU e memoria, avvio con Windows sì/no, porta COM della scheda, ultime righe del registro |
| `controllo` | Esegue il controllo (`--check`) e salva il rapporto in `%LOCALAPPDATA%\nucleo_bridge\controllo.txt` |

- Registro: `%LOCALAPPDATA%\nucleo_bridge\ponte.log` (oltre 5 MB viene spostato in `ponte.log.1`).
- Stato dal browser del PC: `http://127.0.0.1:58090/status` dice se il ponte ha la porta USB e se c'è una
  partita in corso.
- Opzioni a mano: `python nucleo_bridge.py [--popper CARTELLA] [--port COMx] [--web-port 80]`.
  `--popper` serve se Popper non sta in una cartella standard (`C:\vPinball\PinUPSystem`, `C:\PinUPSystem`,
  `D:\vPinball\PinUPSystem`).

### Aggiornare il ponte

1. `ponte.ps1 ferma`
2. Copia i file nuovi nella cartella del ponte (le cache restano in `%LOCALAPPDATA%\nucleo_bridge`).
3. `ponte.ps1 avvia`

`installa` serve solo la prima volta, o se l'attività pianificata è stata tolta.

### Fermarlo o toglierlo

- Per un momento: `ponte.ps1 ferma` (riparte al prossimo accesso di Windows, o con `avvia`).
- Per sempre: `ponte.ps1 rimuovi`, poi togli le due righe `curl` dagli script degli emulatori in Popper.

---

## 5. Il menu dei tavoli

A tavoli chiusi, con il ponte acceso, la scheda è un menu di Popper. In alto le liste e tre pulsanti; al
centro il tavolo selezionato; in basso la barra delle wheel.

### I due stili

Si sceglie nel pannello, **Stile del menu**:

| Stile | Aspetto |
|---|---|
| **Cabinato** (predefinito) | Come il tema predefinito di Popper: barra delle wheel tra binari cromati con un tubo di luce azzurro, la wheel scelta dentro una sfera di vetro cromata, **Gioca** azzurro acceso. |
| **DMD** | Titoli, dati, liste e pulsanti disegnati a puntini colorati come un ColorDMD, il backglass visto attraverso una griglia di LED, cornici squadrate. Le scritte a puntini sono in maiuscolo e scrivono l'accento finale all'italiana (PIU', NOVITA'). |

### Le liste

| Lista | Cosa contiene |
|---|---|
| **Recenti** | I tavoli giocati, dall'ultimo |
| **Novità** | Tutti i tavoli, dagli ultimi aggiunti in Popper |
| **Più giocati** | I tavoli giocati almeno una volta, per numero di partite |
| **Preferiti** | I preferiti segnati in Popper (se è vuota: "Ancora nessun preferito: aggiungili in Popper.") |
| **Tutti** | Tutti i tavoli visibili, in ordine alfabetico |

In alto a destra il conteggio (per esempio "Visual Pinball X 28 / 72") e tre pulsanti:

- **A-Z** — apre **Vai a** (sotto);
- **mescola** (icona a frecce incrociate) — **Sorprendimi**: un tavolo a caso;
- **ingranaggio** — il pannello delle impostazioni del DMD (capitolo 7).

### La barra delle wheel

- **Trascina** la barra: la wheel al centro, più grande e illuminata, è il tavolo selezionato. La barra gira
  in tondo.
- **Tocca** una wheel: viene al centro.
- **Lancia** con uno scatto veloce: la barra continua da sola, più lontano quanto più è veloce (al massimo un
  terzo della lista); un trascinamento lento si sposta di quanto hai percorso.
- **Scorri** a destra o a sinistra sulla parte centrale: un tavolo avanti o indietro.
- **Con tastiera o telecomando**: le frecce sinistra/destra muovono la barra, Invio attiva il pulsante
  selezionato (per primo **Gioca**), Esc / Indietro chiude prima le finestre aperte (Vai a, istruzioni,
  conferme).

Un tavolo senza wheel mostra il suo nome al posto dell'immagine.

### Vai a (A-Z)

- In alto le **categorie** della lista aperta, nella lingua della scheda (Visual Pinball X, Future Pinball,
  Pinball FX, Sparatutto, Labirinti, Piattaforme, Sport e corse, Altri arcade, Giochi PC), ognuna con quanti
  tavoli di **questa** lista contiene; la prima, **Tutti i giochi**, toglie il filtro. Una categoria filtra
  la lista.
- Sotto le **lettere**: una lettera salta al primo tavolo che comincia con quella. Le lettere con cui non
  comincia nessun tavolo sono spente; cifre e simboli sono **0-9**.
- Per chiudere: la **✕**, un tocco fuori dal riquadro, uno scorrimento verso il basso o Indietro.

### Sorprendimi

Il pulsante mescola mostra un tavolo a caso. Sulla sua scheda compare **Un altro** per pescarne subito un
altro.

### La scheda del tavolo

Il tavolo selezionato riempie lo schermo: il suo backglass dietro (scurito), la wheel grande, il nome,
costruttore e anno, partite e tempo di gioco ("10 partite, 7 h 16 min di gioco"), l'ultima partita (date nel
formato gg/mm/aaaa), la dicitura **Preferito** se lo è, i primi punteggi. I pulsanti:

| Pulsante | Cosa fa |
|---|---|
| **Gioca** | Avvia il tavolo sul cabinato (tramite il Web Remote di Popper) |
| **Istruzioni** | Solo se il tavolo ha la scheda istruzioni in Popper: la mostra raddrizzata, a tutta larghezza; scorri con il dito, tocca (o Indietro) per chiudere. Finché la pagina non arriva mostra "Caricamento..."; si chiude anche con la **✕**. Se la pagina si perde la scheda la chiede altre due volte, poi si chiude con "Le istruzioni non sono arrivate dal PC: riprova."; se il PC non ha istruzioni per quel tavolo: "Nessuna istruzione per questo tavolo." |
| **Composizione** | Come il tavolo usa il monitor del PC e questo schermo (capitolo 8), anche senza giocarlo |
| **Un altro** | Dopo Sorprendimi: un altro tavolo a caso |

Dopo **Gioca** compare "Avvio in corso..." con il nome del tavolo; **Indietro** nasconde solo questa pagina.
La scheda aspetta fino a circa 2 minuti (il ponte riprova finché Popper rifiuta gli avvii, per esempio subito dopo un crash); se Popper non ha avviato nulla: "Il tavolo non è partito: controlla il Web Remote di
Popper."

### Vetrina: quando usi i comandi del cabinato in Popper

Quando qualcuno usa il joystick/i pulsanti del cabinato in Popper, la scheda segue la sua selezione e mostra
la **vetrina** del tavolo: backglass, DMD e topper uno dopo l'altro, ogni 7 secondi, ciascuno in un alone dei
suoi colori, con "Tocca per scegliere un tavolo". Un tocco apre il menu su quel tavolo.

- Volantini e istruzioni non sono nella vetrina (in Popper sono pagine verticali salvate di lato): le
  istruzioni si aprono dal pulsante **Istruzioni**.
- Se in Popper è selezionata una voce che non è un tavolo ("Go Back", "Exit", una playlist), la scheda mostra
  la sua azione in grande (per esempio "INDIETRO", "ESCI", o il nome della playlist) e sotto "Su Popper: …".
- Quando Popper torna nella sua modalità attract, torna il menu della scheda.
- Se un tavolo parte da Popper (non dalla scheda), la scheda passa comunque al DMD.

### Modalità attract

Dopo 2 minuti senza tocchi la barra gira da sola (un tavolo ogni 8 secondi) e lampeggia "Tocca per
scegliere". Non succede durante una partita; a fine partita i 2 minuti ripartono, così il tavolo appena
giocato resta sullo schermo con i punteggi aggiornati.

---

## 6. Durante la partita

### Cosa mostra la scheda

| Situazione | Sullo schermo |
|---|---|
| Tavolo VPX con DMD | Il DMD a puntini, nella disposizione scelta (capitolo 7) |
| Tavolo VPX che non ha ancora mandato il DMD | "Caricamento..." fino a un minuto, poi la pagina del gioco ("In partita da 12 min"): è un tavolo senza DMD, per esempio un alfanumerico (vedi capitolo 9) |
| Tavolo Future Pinball con DMD | Il DMD, copiato dal ponte |
| Pinball FX, MAME, giochi PC | La vetrina del gioco in corso |
| PC silenzioso da 5 s (senza Popper) | L'orologio a puntini, a 12 o 24 ore come l'orologio di sistema, se **Orologio quando il PC tace** è attivo |

Mentre il DMD di una partita è sullo schermo, i gesti di sistema di NucleoOS (tendina, Home, App recenti)
sono bloccati: uno sfioramento non può nascondere il punteggio.

### Gesti e tasti

- **Tocco breve** sul DMD (o sulla vetrina di un gioco senza DMD, o sulla pagina "In partita" di un tavolo che non ha ancora mandato il DMD): i **comandi rapidi**.
- **Tieni premuto 2 secondi** (o Invio / Spazio da tastiera, anche sull'orologio e sulla pagina di attesa):
  il **pannello delle impostazioni**. Con il pannello aperto i gesti di sistema tornano attivi.
- **Indietro** (o Esc) sul DMD apre e chiude il pannello: non chiude mai l'app.

### Comandi rapidi

| Voce | Cosa fa |
|---|---|
| **Torna al DMD** | Chiude i comandi rapidi |
| **Menu tavoli** | Sfoglia i tavoli durante la partita; in alto il pulsante **Torna al DMD** |
| **Esci dal gioco** | Chiude il tavolo in corso ("Chiusura del gioco...") e torna il menu |
| **Impostazioni DMD** | Apre il pannello delle impostazioni |
| **Composizione di questo tavolo** | La composizione del tavolo in gioco (capitolo 8), aperta sulla scheda **Questo schermo** |

Per chiudere i comandi rapidi senza scegliere: tocca fuori dal riquadro o scorri verso il basso.

Per i tavoli VPX l'uscita passa dal **Wi-Fi**, direttamente al Web Remote di Popper (il cavo in quel momento è
di dmdext): la scheda deve essere sulla stessa rete del PC. Se il PC non risponde: "Il PC non risponde in
Wi-Fi: chiudi il gioco dal cabinato." Per gli altri emulatori l'uscita passa dal cavo, tramite il ponte. Se dopo i tentativi il gioco non si chiude (anche quando cambi tavolo): "Il PC non ha chiuso il gioco: chiudilo dal cabinato."

### Avviare un altro tavolo

Da **Menu tavoli** scegli un tavolo e tocca **Gioca**. La scheda chiede "*X* è in gioco. Chiuderlo e avviare
*Y*?": con **Chiudi e avvia** chiude il tavolo in corso e avvia il nuovo appena il ponte segnala la fine;
**Annulla** lascia tutto com'è.

### Quando il cavo serve al DMD

Durante una partita il cui DMD passa da dmdext (i tavoli VPX) il cavo è occupato dal DMD: **Sorprendimi**, le schede degli altri tavoli, le **Istruzioni** e la scelta della versione del backglass dicono "Disponibile a fine partita: durante il gioco il cavo serve al DMD."

### Un tavolo che si chiude da solo

Se il tavolo va in crash o viene chiuso a forza (senza l'avviso di fine di Popper), la scheda lo dice — "*Nome* si è chiuso da solo: puoi riavviarlo da qui." — e lo lascia selezionato, pronto per **Gioca**.

---

## 7. Il pannello delle impostazioni

Si apre tenendo premuto lo schermo 2 secondi (o con Invio / Spazio / Esc), dall'ingranaggio del menu, o da
**Impostazioni DMD** nei comandi rapidi. In alto:

- **la riga di stato**: USB / Wi-Fi, formato in parole semplici ("16 tonalità", "16 colori", "RGB",
  "orologio", "Schermata di prova", "display dei punteggi"), dimensione, fotogrammi al secondo, gioco;
- **sotto, l'indirizzo Wi-Fi** da mettere nel `DmdDevice.ini` (`ws://…:9090/dmd`; "nessun indirizzo ancora"
  se la scheda non è in rete); quando il ponte per Popper ha salutato la scheda, sulla stessa riga anche "Ponte <versione> · Popper: pronto" (o "database non trovato");
- tre pulsanti: **Torna al DMD** (si legge **Indietro** quando nessun PC sta pilotando il DMD) chiude il
  pannello; **Schermata di prova** (sotto); **Chiudi il DMD** chiude l'app, dopo la domanda descritta qui
  sotto.

Ogni modifica vale subito e resta salvata nella scheda.

### Chiudere l'app

Ogni modo di uscire da DMD Flipper — **Chiudi il DMD**, i gesti Home / App recenti, i tasti di sistema, il pulsante di chiusura della finestra nella modalità desktop classica —
chiede prima **"Chiudere il DMD?"**: durante una partita "Un tavolo è in gioco: il cabinato resterà senza DMD
finché l'app non viene riaperta.", altrimenti "Il cabinato resterà senza DMD e senza menu dei tavoli finché
l'app non viene riaperta." **Resta** (il pulsante già selezionato) annulla; **Chiudi** esce. Indietro non
chiude mai l'app.

### Schermo

| Impostazione | Cosa fa | Predefinito |
|---|---|---|
| **Disposizione per tutti i tavoli** | Come il DMD occupa lo schermo (tabella sotto). Il riquadro mostra nome e descrizione della disposizione; se il tavolo in gioco ne ha una sua lo dice ("Questo tavolo ne ha una sua: …"). Tocca il riquadro o **Scegli con le anteprime** per scegliere (capitolo 8). | DMD classico |
| **Rotazione** | 0°, 90°, 180°, 270° in senso orario, per come è montata la scheda | 0° |

Le disposizioni, nell'ordine in cui compaiono le anteprime:

| Disposizione | Cosa fa |
|---|---|
| **Automatica** | Per ogni tavolo la migliore per le immagini che ha: Grafica con finestra se ha la grafica FullDMD, Backglass con alloggio se ha la griglia, altrimenti DMD classico. Un DMD 128x16 prende DMD a tutto schermo (righe ricostruite). |
| **DMD a tutto schermo** | Tutto lo schermo senza vuoti: le righe sono ricostruite (128x64) e i puntini restano tondi e grandi. |
| **DMD classico** | Puntini quadrati e proporzioni vere, il più grandi possibile; fasce sopra e sotto. |
| **DMD alto** | Il DMD su tutta la larghezza e un po' più alto; fasce sopra e sotto. |
| **DMD stirato** | Tutto lo schermo con i punti originali: la figura si allarga, i puntini restano piccoli e le righe distanti. |
| **Backglass + DMD** | Il backglass del tavolo in alto e il DMD sotto, come in un cabinato vero. Senza backglass il DMD è centrato. |
| **Solo backglass** | Solo il backglass del tavolo, niente DMD: la scheda fa da schermo del backglass. |
| **Grafica del tavolo** | La grafica FullDMD del tavolo in alto e il DMD in una fascia larga sotto. |
| **Grafica con finestra** | La grafica FullDMD del tavolo su tutto lo schermo e il DMD dentro la sua finestra. |
| **Backglass con alloggio** | Il backglass con la griglia su tutto lo schermo e il DMD nel suo alloggio. |

- Le disposizioni con backglass o grafica usano le immagini che il ponte manda all'inizio della partita:
  senza ponte, o per un tavolo che non le ha, non sono disponibili.
- **Schermo ruotato** (90°, 180°, 270°): le disposizioni con immagini mostrano il DMD classico; a 90° e 270°
  anche DMD alto e DMD a tutto schermo. Il pannello e la Composizione lo dicono: "Schermo ruotato: le
  disposizioni con immagini mostrano il DMD classico."
- L'orologio usa sempre tutto lo schermo a puntini tondi, qualunque sia la disposizione.

### Menu dei tavoli

| Impostazione | Valori | Predefinito |
|---|---|---|
| **Stile del menu** | **Cabinato** / **DMD** (capitolo 5) | Cabinato |

### Puntini

| Impostazione | Cosa fa | Predefinito |
|---|---|---|
| **Colore** | Il colore dei DMD monocromatici, scelto da una fila di cerchi colorati; la didascalia dice quello scelto ("Colore · Arancio"). Il cerchio vuoto è **Dal PC** (il colore mandato dal PC); poi Arancio, Rosso, Giallo, Verde, Azzurro, Bianco. Vale anche via USB: i fotogrammi monocromatici vengono ricolorati, mentre i DMD colorati (ColorDMD, Serum, palette) tengono i loro colori; **Dal PC** lascia il colore di dmdext. | Arancio |
| **Luminosità** | Dal 10 al 100%, con il dito o con i tasti; viene salvata in entrambi i casi | 100% |
| **Forma del puntino** | **Tondo** o **Quadrato** | Tondo |
| **Alone** | Un leggero alone luminoso negli spazi tra i puntini | Spento |

### Sistema

| Impostazione | Cosa fa | Predefinito |
|---|---|---|
| **Orologio quando il PC tace** | Se il PC non manda nulla per 5 secondi (dopo aver mostrato qualcosa), la scheda mostra l'ora a puntini (12 o 24 ore, come l'orologio di sistema) con la data sotto. Non compare sopra il menu di Popper né durante una partita. | Acceso |
| **Avvia all'accensione** | NucleoOS apre DMD Flipper all'accensione | Spento |
| **Porta di rete** | La porta per il Wi-Fi. Vale quando confermi o chiudi il pannello, e viene salvata solo quando la scheda è davvero in ascolto su quella porta. Gli errori compaiono sotto il campo ("Porta: un numero da 1 a 65535.", "Impossibile aprire la porta …"): in quel caso resta la porta in uso. | 9090 |

### Schermata di prova

Per 10 secondi la scheda mostra "TEST" con il conto alla rovescia dei secondi, il colore scelto in 16
gradini, barre di colore (rosso, verde, blu, giallo, azzurro, magenta, bianco, grigio), un bordo e un blocco
pieno nell'angolo in alto a sinistra. Intanto i fotogrammi del PC vengono ignorati. Un tocco, Invio, Spazio o
Esc la chiudono prima. Serve a controllare orientamento, colori e luminosità del cabinato **senza PC**: se il
blocco non è in alto a sinistra, cambia **Rotazione**.

---

## 8. La schermata Composizione

Qui scegli, con anteprime fatte con le immagini vere del tavolo, come il tavolo usa gli schermi del
cabinato. Si apre in tre modi:

| Da dove | Cosa sceglie |
|---|---|
| Pannello → **Scegli con le anteprime** | La disposizione di **questo schermo** per **Tutti i tavoli**: le dieci disposizioni in una griglia, tutte in vista. Le anteprime usano le immagini del tavolo in gioco o di quello selezionato nel menu. |
| Scheda del tavolo → **Composizione** | Monitor del PC e questo schermo per **quel** tavolo, senza doverlo giocare |
| Comandi rapidi → **Composizione di questo tavolo** | Lo stesso, per il tavolo in gioco; si apre sulla scheda **Questo schermo** |

Il monitor del PC si sceglie tavolo per tavolo: dal pannello si sceglie solo questo schermo. Chiudi con la
**✕** in alto a destra o con Indietro. In alto, la riga "Immagini di questo tavolo: …" dice cosa ha il
tavolo (backglass e sua dimensione, con griglia, grafica FullDMD, backglass doppio); mentre arrivano:
"Le immagini del tavolo arrivano dal PC...".

### Scheda "Questo schermo"

- **Puntini veri, 1:1**: una lente che mostra il centro del DMD disegnato alla dimensione reale dello
  schermo, così vedi la grandezza dei puntini e gli spazi tra le righe prima di uscire (una miniatura non
  può mostrarli).
- Il nome della disposizione scelta e cosa fa. Per **Automatica** su un tavolo che non è in gioco: "decisa
  quando il tavolo parte".
- **Dimensione dei puntini**: **Adattati** (alla disposizione) o **Originali** (quadrati, a pixel interi, mai
  stirati).
- **Margine**: 0, 1, 2 o 3 righe e colonne di puntini spenti intorno al DMD.
- Solo in partita, con un DMD 128x16: **DMD 128x16: ricostruito** (portato a tutta altezza, predefinito) o
  **DMD 128x16: originale**.
- Le disposizioni come schede: per un tavolo la prima è **Come tutti i tavoli** (segue la scelta del
  pannello), poi solo quelle possibili con le immagini che il tavolo ha.

### Scheda "Secondo schermo (PC)"

Decide cosa mostra il **monitor del backglass del PC** per questo tavolo. Il ponte scrive la scelta dove B2S
e dmdext la leggono all'avvio del tavolo (il file `.res` del tavolo, `B2STableSettings.xml`, la sezione del
gioco nel `DmdDevice.ini`), dopo averne fatto **una copia di sicurezza** con il suffisso `.prima-di-s2`.

| Scelta | Sul monitor del PC |
|---|---|
| **Come in origine** | Come lo dispone il file del backglass del tavolo (B2S), senza modifiche |
| **Backglass** | Solo il backglass, su tutto il monitor |
| **Backglass + DMD** | Il backglass in alto e il DMD sotto |
| **Backglass con griglia** | Il backglass intero con la griglia dell'altoparlante |
| **Grafica FullDMD** | La grafica FullDMD del tavolo (la seconda immagine, più piccola, del file B2S) |
| **Backglass + grafica** | Il backglass in alto e sotto la grafica FullDMD |

Compaiono solo le scelte possibili per quel tavolo. Inoltre:

- **DMD anche sul monitor**: **No** (il DMD resta solo su questa scheda) o **Sì** (anche il DMD virtuale di
  dmdext sul monitor). Con **Come in origine** questi pulsanti sono spenti: "Prima scegli una delle
  composizioni qui sotto."
- **Versione del backglass**: **Autentica** o **Fantasy**, per i backglass doppi: B2S accende l'altra serie
  di immagini dalla prossima partita.
- Le modifiche valgono **dalla prossima partita** ("Secondo schermo pronto per la prossima partita"). Se stai
  giocando, compare **Riavvia il tavolo ora** per chiuderlo e riaprirlo subito con la nuova composizione.
- "Nessun secondo schermo sul PC": il ponte non ha trovato un secondo monitor. "Questo tavolo non ha il file
  del backglass (B2S)": il monitor mostra il backglass di Popper e non c'è nulla da scegliere. "Il PC non ha risposto: impossibile descrivere il file del backglass.": il PC non ha descritto il file del backglass dopo 3 tentativi; riapri la Composizione.

---

## 9. Risoluzione dei problemi

| Problema | Cosa fare |
|---|---|
| **Nessun DMD sulla scheda** | Controlla che l'app sia aperta **prima** di avviare il tavolo; che nel `DmdDevice.ini` ci sia `[zedmd] enabled = true` (USB) o `[networkstream]` con l'URL giusto (Wi-Fi); apri il pannello e guarda la riga di stato (USB / Wi-Fi, fotogrammi al secondo). |
| **dmdext dice "ZeDMD is not available"** | L'app non era aperta quando il tavolo è partito, oppure un altro programma tiene la porta COM. Attiva **Avvia all'accensione**, chiudi i programmi che usano la porta, e se serve fissa la porta con `port = COMx` sotto `[zedmd]`. |
| **La scheda si riavvia quando parte un tavolo** | Aprendo la porta, dmdext manda alla scheda un segnale di reset: l'app lo blocca **solo mentre è aperta**. Tienila aperta (Avvia all'accensione). Se si riavvia lo stesso, togli l'hub: un hub senza alimentazione lascia la scheda senza corrente. |
| **Il menu dei tavoli non compare** | Apri il pannello: se nella riga dell'indirizzo manca la parte "Ponte …", il ponte non sta parlando con la scheda. Il ponte deve essere acceso: `ponte.ps1 stato` (riga "Ponte ACCESO" e "Scheda: COMx"). Se la scheda risulta "non collegata", controlla il cavo. Fai `ponte.ps1 controllo` e leggi il rapporto. Durante una partita il menu non c'è: compare a fine partita. Con il ponte sul cavo la scheda gli chiede da sola il saluto ogni 3 secondi, quindi riaprendo l'app il menu torna da solo. |
| **"Popper: database non trovato"** nel pannello | Il ponte non trova il database di Popper: controlla la cartella di Popper con `ponte.ps1 controllo` (se è in una cartella non standard, avvia il ponte con `--popper`). |
| **"Nessun tavolo in questa lista."** | Normale per **Recenti** o **Più giocati** se non hai ancora giocato; prova **Tutti**. |
| **"Il PC non ha mandato l'elenco dei tavoli: controlla Popper."** | `ponte.ps1 controllo`: la cartella di Popper e il database sono stati trovati? |
| **Mancano immagini (wheel, backglass)** | La prima volta le immagini arrivano dal PC una alla volta; con la microSD, a menu fermo, la scheda le scarica da sola e poi è istantaneo. Il file in Popper deve chiamarsi come il gioco (o "Nome(1).png", "Nome (2).png"). Un'immagine aggiunta in Popper compare entro 10 minuti, o riaprendo l'app. |
| **"Il cavo verso il PC è occupato: riprova tra un attimo."** | Il cavo stava trasferendo un'immagine grande: riprova. |
| **"Le istruzioni non sono arrivate dal PC: riprova."** | La pagina si è persa sul cavo anche dopo due nuove richieste: riprova tra un attimo. |
| **"Disponibile a fine partita: durante il gioco il cavo serve al DMD."** | Normale durante un tavolo VPX: la funzione torna disponibile a fine partita. |
| **"… si è chiuso da solo: puoi riavviarlo da qui."** | Il tavolo è andato in crash o è stato chiuso a forza: tocca **Gioca** per riavviarlo. |
| **"Il PC non ha risposto: impossibile descrivere il file del backglass."** | Il ponte non ha risposto per la Composizione: controlla che sia acceso (`ponte.ps1 stato`) e riapri la Composizione. |
| **"Nessuna istruzione per questo tavolo."** | Il PC non ha una scheda istruzioni da mandare per quel tavolo: aggiungila in Popper (schermo GameHelp). |
| **Il tavolo non parte** ("Il tavolo non è partito: controlla il Web Remote di Popper." / "Il PC non ha avviato il tavolo: controlla Popper.") | Popper deve essere in esecuzione con il Web Remote attivo (`useWEB = true ;`, Popper riavviato, componente COM registrato, porta 80). Prova a lanciare lo stesso tavolo da Popper: se non parte neanche lì, il problema è nel tavolo o in Popper. |
| **Non riesco a uscire dal gioco dalla scheda** ("Il PC non risponde in Wi-Fi: chiudi il gioco dal cabinato." / "Il PC non ha chiuso il gioco: chiudilo dal cabinato.") | Per i tavoli VPX l'uscita passa dal Wi-Fi: la scheda deve essere collegata alla rete del cabinato e raggiungere il Web Remote di Popper (TCP 80) sul PC. Di solito è raggiungibile così com'è installato; se non lo è, in Windows Defender Firewall consenti le connessioni in entrata TCP 80 per PinUP Popper sulla rete privata. Intanto esci con i pulsanti del cabinato. |
| **Quale URL metto per il Wi-Fi?** | Quello scritto sulla pagina di attesa dell'app o in cima al pannello: `ws://<IP della scheda>:9090/dmd`. Se l'IP cambia a ogni accensione, prenota un IP fisso nel router. "Wi-Fi non collegato": collega la scheda alla rete del cabinato o usa l'USB. |
| **"Porta: un numero da 1 a 65535." / "Impossibile aprire la porta …"** | Hai scritto una porta non valida, o la scheda non riesce ad aprirla: scegline un'altra in **Porta di rete**. Finché non funziona resta in uso quella di prima. |
| **Tavoli alfanumerici o a segmenti (anni '80) senza DMD sulla scheda** | dmdext converte da solo i display a segmenti, ma solo se VPinMAME gli passa il display: per quella ROM la chiave di registro `HKCU\Software\Freeware\Visual PinMame\<rom>\showpindmd` deve valere 1. Fai un backup del registro prima di cambiarla. |
| **"Questo DMD (…) non entra nello schermo."** | Via Wi-Fi il DMD supera 512x256: usa una dimensione più piccola in dmdext o l'USB. |
| **Immagine capovolta o colori strani** | **Schermata di prova**: il blocco pieno deve stare in alto a sinistra (altrimenti **Rotazione**); per i DMD monocromatici scegli il **Colore**. |
| **Puntini piccoli o "troppo nero" tra le righe** | Usa **DMD a tutto schermo** o **DMD classico** invece di **DMD stirato**; controlla con la lente **Puntini veri, 1:1**. |
| **Il backglass non compare anche se l'ho scelto** | Con lo schermo ruotato le disposizioni con immagini mostrano il DMD classico: riporta la **Rotazione** a 0°. |
| **Il pannello non si apre durante la partita** | Tieni il dito fermo sullo schermo per 2 secondi interi (un tocco breve apre i comandi rapidi), oppure premi Indietro. |
| **Dopo un crash di VPX Popper non avvia più nulla** | Il ponte chiude da solo il server B2S rimasto aperto (riga "closed b2sbackglassserverexe" nel registro). Se non succede, controlla che il ponte sia acceso. |

Per segnalare un problema allega l'uscita di `ponte.ps1 stato` e le ultime righe di
`%LOCALAPPDATA%\nucleo_bridge\ponte.log`.

---

## 10. Aggiornare il firmware in sicurezza

Le impostazioni di DMD Flipper restano nella memoria interna della scheda e sopravvivono agli aggiornamenti.

- **Via Wi-Fi** (il modo normale): **Impostazioni → Aggiornamento sistema**. Non serve fare nulla di
  particolare per il DMD.
- **Via cavo USB con esptool**: mentre DMD Flipper è aperta, la scheda **ignora il reset dalla porta USB** (è
  ciò che la protegge da dmdext), quindi esptool non riuscirebbe a riavviarla. Prima di scrivere:
  1. chiudi l'app: torna alla Home e rispondi **Chiudi** a "Chiudere il DMD?", oppure chiama
     `GET /api/ui/home`, che la chiude senza chiedere;
  2. sul PC ferma il ponte, che tiene la porta COM: `ponte.ps1 ferma`;
  3. non avviare tavoli durante la scrittura;
  4. a scrittura finita: `ponte.ps1 avvia` e riapri DMD Flipper (o lascia fare ad **Avvia all'accensione**).

---

## 11. Limiti noti

- Via USB la scheda è un DMD 128x32: dmdext adatta le altre dimensioni. Via Wi-Fi le mostra come sono, fino
  a 512x256.
- Via Wi-Fi un solo PC alla volta.
- Con **DMD a tutto schermo** il DMD si aggiorna a circa 30 fotogrammi al secondo; con le disposizioni a
  puntini quadrati circa 52-60.
- Da dove arriva il DMD: i tavoli VPX lo mandano con dmdext via USB; i tavoli Future Pinball che hanno un DMD
  vengono copiati dal ponte con la modalità mirror di dmdext; Pinball FX, MAME e i giochi PC mostrano le
  immagini della vetrina del gioco.
- Sul cavo, i messaggi grandi (immagini) ogni tanto arrivano rovinati: il ponte li rimanda finché la scheda
  li conferma; raramente un'immagine può mancare fino al passaggio successivo.
- Senza microSD le immagini passano dal cavo ogni volta.
- Ogni lista contiene al massimo 2000 tavoli.
- Con lo schermo ruotato le disposizioni con immagini mostrano il DMD classico.
- Volantini e istruzioni non compaiono nella vetrina; i tavoli nascosti in Popper non compaiono nel menu.
- Le modifiche al monitor del PC valgono dalla partita successiva (o con **Riavvia il tavolo ora**).
- Il Web Remote di Popper funziona solo mentre Popper è in esecuzione.

---

## 12. Privacy e rete

- La scheda parla **solo con il PC del cabinato, sulla rete locale**: riceve il DMD (USB, o Wi-Fi sulla porta
  9090) e, per uscire o avviare un tavolo durante una partita VPX, chiama il Web Remote di Popper sul PC
  (porta 80). L'app non manda nulla su Internet.
- **Il server Wi-Fi del DMD (porta 9090) non ha autenticazione** e **il Web Remote di Popper non ha
  password**: usali solo su una rete di casa fidata, mai su una rete pubblica e mai esposti su Internet.
- Il ponte ascolta solo sul PC stesso (`127.0.0.1:58090`), legge il database di Popper in sola lettura e
  tiene le sue immagini convertite in `%LOCALAPPDATA%\nucleo_bridge\cache`. Scrive i file di B2S e dmdext
  solo quando scegli una composizione del monitor (capitolo 8), con copia di sicurezza.
- La scheda tiene le immagini ricevute e le ultime liste sulla microSD, in `/sdcard/data/dmd`.
