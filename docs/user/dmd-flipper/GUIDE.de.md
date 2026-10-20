# DMD Flipper — Benutzerhandbuch

**DMD Flipper** macht deine NucleoOS-Platine (ESP32-P4 mit 1024x600-Touchscreen) zum DMD deines virtuellen
Flipperautomaten und, solange kein Tisch läuft, zu einem Touch-Menü deiner PinUP-Popper-Tische.

Dieses Handbuch richtet sich an Automatenbesitzer: Es erklärt, wie du die Platine anschließt, wie du das kleine
„Brücken“-Programm auf dem PC installierst und wie du jeden Bildschirm bedienst. Programmierkenntnisse sind
nicht nötig.

![Die Punkt-Uhr, wenn der PC schweigt](../../screenshots/dmd_clock.jpg)

---

## Inhalt

1. [Was es ist und was du brauchst](#1-was-es-ist-und-was-du-brauchst)
2. [Schnellstart in 5 Schritten](#2-schnellstart-in-5-schritten)
3. [Den PC verbinden](#3-den-pc-verbinden)
4. [Die Brücke für PinUP Popper installieren](#4-die-brücke-für-pinup-popper-installieren)
5. [Das Tischmenü](#5-das-tischmenü)
6. [Während des Spiels](#6-während-des-spiels)
7. [Das Einstellungsfeld](#7-das-einstellungsfeld)
8. [Der Bildschirm Komposition](#8-der-bildschirm-komposition)
9. [Fehlerbehebung](#9-fehlerbehebung)
10. [Firmware sicher aktualisieren](#10-firmware-sicher-aktualisieren)
11. [Bekannte Grenzen](#11-bekannte-grenzen)
12. [Datenschutz und Netzwerk](#12-datenschutz-und-netzwerk)

---

## 1. Was es ist und was du brauchst

Die App erledigt zwei Aufgaben:

- **Das DMD des Automaten.** Während eines Spiels schickt der PC (Visual Pinball X + VPinMAME + dmdext) das DMD
  an die Platine, die es als LED-/Plasma-Punkte zeichnet. Die Bilder kommen über **USB** oder **WLAN**, oder über
  beides gleichzeitig.
- **Ein Touch-Menü für PinUP Popper.** Solange kein Tisch läuft, zeigt die Platine deine Tische (zuletzt
  gespielt, neu, meistgespielt, Favoriten, alle) mit Wheels, Backglass, Statistiken und Highscores: Tippe auf
  **Spielen**, und der Tisch startet am Automaten. Dafür brauchst du die **Brücke**, ein kleines Programm auf
  dem PC (Kapitel 4).

### Was du brauchst

| Was | Details |
|---|---|
| NucleoOS-Platine | Mit der App **DMD Flipper**. Eine microSD-Karte ist optional: Mit ihr speichert die Platine dort die Bilder und die letzten Listen, und das Menü reagiert sofort; ohne sie funktioniert das Menü trotzdem, aber die Bilder kommen jedes Mal über das Kabel und nichts bleibt zwischen zwei Sitzungen erhalten. |
| USB-Kabel | Vom USB-Programmierport der Platine (Windows sieht ihn als USB-Gerät 303A:1001) **direkt zum PC**. Kein Hub ohne eigene Stromversorgung: Die Platine bekäme zu wenig Strom und würde neu starten. |
| PC des Automaten | Windows 10 (Version 1803 oder neuer) oder 11, Visual Pinball X, VPinMAME mit **dmdext 2.x** (dessen `DmdDevice.dll`). |
| PinUP Popper | Für das Tischmenü, mit eingeschaltetem **Web Remote** (Kapitel 4). |
| Python | 3.8 oder neuer, für die Brücke. |
| WLAN | Für das DMD optional (USB reicht), mit Popper **empfohlen**: Darüber beendet die Platine einen VPX-Tisch oder startet einen anderen, während gespielt wird. |

---

## 2. Schnellstart in 5 Schritten

1. **Verbinde** die Platine mit dem USB-Kabel direkt mit dem PC (kein Hub ohne Stromversorgung).
2. **Öffne DMD Flipper** auf der Platine. Halte den Bildschirm 2 Sekunden, um das Einstellungsfeld zu öffnen,
   und schalte **Beim Einschalten starten** ein: Die App muss schon offen sein, wenn ein Tisch startet.
3. **Auf dem PC** schaltest du in der `DmdDevice.ini` von dmdext den USB-Ausgang ein:
   ```ini
   [zedmd]
   enabled = true
   ```
4. **Installiere die Brücke** für Popper (Kapitel 4): Ordner kopieren, `pip install pyserial pillow`, die zwei
   Zeilen in die Skripte der Emulatoren in Popper eintragen, dann
   `powershell -ExecutionPolicy Bypass -File ponte.ps1 installa`.
5. **Ausprobieren**: Läuft kein Tisch, zeigt die Platine das Menü. Wähle einen Tisch, tippe auf **Spielen**: Der
   Tisch startet und die Platine wechselt zum DMD. Beende den Tisch: Das Menü kommt zurück.

---

## 3. Den PC verbinden

Die Platine empfängt das DMD auf zwei Wegen, beide gleichzeitig aktiv. Gewählt wird in der `DmdDevice.ini` von
dmdext auf dem PC (die Datei neben VPinMAMEs `DmdDevice.dll`). Die Warteseite der App zeigt beide
Einstellungen, mit der genauen Adresse deiner Platine.

### USB (empfohlen)

```ini
[zedmd]
enabled = true
```

- Die Platine meldet sich bei dmdext als **ZeDMD 128x32**: dmdext durchsucht die seriellen Ports und findet sie
  selbst. Um den Port festzulegen, ergänze unter `[zedmd]` die Zeile `port = COMx` (die COM-Portnummer der
  Platine aus dem Geräte-Manager; `ponte.ps1 stato` zeigt sie in der Zeile „Scheda“).
- **Die App muss offen sein, wenn der Tisch startet**: Genau dann sucht dmdext das Gerät. Deshalb lohnt sich
  **Beim Einschalten starten** (siehe unten).
- DMDs anderer Größen (zum Beispiel 128x16 oder 192x64) skaliert dmdext auf 128x32.
- Mit der Popper-Brücke ist USB auch der Kanal des Menüs: Brücke und dmdext reichen sich den Port bei jedem
  Spielstart und -ende selbst weiter.

### WLAN

```ini
[networkstream]
enabled = true
url = ws://<IP der Platine>:9090/dmd
retry = true
```

- Die genaue Adresse (`ws://…:9090/dmd`) steht auf der Warteseite der App und oben im Einstellungsfeld, unter
  der Statuszeile.
- Über WLAN zeigt die Platine das DMD in seiner echten Größe, bis 512x256.
- Immer nur ein PC: Eine neue Verbindung ersetzt die vorherige.
- Port 9090 lässt sich im Einstellungsfeld ändern (**Netzwerkport**); mit `retry = true` verbindet sich der PC
  von selbst neu.

> **Tipp — feste IP.** Gib der Platine im Router eine feste Adresse (DHCP-Reservierung): Dann ändert sich die URL
> in der `DmdDevice.ini` nie.

### Beim Einschalten starten

Schalte im Einstellungsfeld (Abschnitt **System**) **Beim Einschalten starten** ein (ab Werk aus): Eine Sekunde
nach dem Einschalten öffnet NucleoOS direkt DMD Flipper. Ist die Bildschirmsperre aktiv, öffnet sich die App erst
nach dem Entsperren: Am Automaten verzichtest du besser auf die Sperre.

---

## 4. Die Brücke für PinUP Popper installieren

### Was sie tut

Die Brücke (`nucleo_bridge.py`) läuft auf dem PC des Automaten, startet mit Windows und ist sehr sparsam
(niedrige Priorität, etwa 27 MB, kein Fenster). Die ganze Grafikarbeit erledigt die Platine.

- **Solange kein Tisch läuft**, schickt sie der Platine die Tischlisten, die Tischkarten und die Bilder (einmal
  umgewandelt und zwischengespeichert).
- **Wenn ein VPX-Tisch startet**, überlässt sie den USB-Port dmdext: Die Platine wird zum DMD. Am Spielende
  übernimmt sie den Port wieder.
- **Future-Pinball-Tische mit DMD**: Die Brücke spiegelt es mit dem Mirror-Modus von dmdext auf die Platine.
- **Pinball FX, MAME, PC-Spiele**: Die Brücke behält den Port, und die Platine zeigt das Schaufenster des
  laufenden Spiels, mit den Schnellaktionen.
- **Starten und Beenden** der Tische läuft über Poppers Web Remote.
- Sie liest Poppers Datenbank (`PUPDatabase.db`) **nur lesend**: Sie schreibt dort nie etwas hinein. In Popper
  ausgeblendete Tische erscheinen nicht.
- Meldet Popper das Ende eines Spiels nicht (ein abgestürzter oder gewaltsam beendeter Emulator), bemerkt die
  Brücke es selbst, sobald der Prozess des Emulators endet, und das Menü kommt zurück.
- Sie schließt den B2S-Backglass-Server, den VPX nach einem Absturz manchmal offen lässt (sonst würde Popper
  nichts mehr starten).
- Sie lässt sich auch während eines Spiels neu starten: Sie erkennt den laufenden Tisch.

### Voraussetzungen

- Python 3.8 oder neuer, mit `pip install pyserial pillow` (numpy ist optional: macht die Bilder schneller).
- PinUP Popper mit eingeschaltetem **Web Remote**:
  - `useWEB = true ;` in `PUPMenuScriptSysOptions.txt` und in `PuPMenuScript.pup` (Poppers „Skriptoptionen“),
    danach Popper neu starten;
  - die COM-Komponente `PuPServer.WebServer` registriert (falls nicht: `PUPServerRegisterCOM.bat` als
    Administrator ausführen);
  - Port 80 (nutzt Popper einen anderen Port, starte die Brücke mit `--web-port`).
  Das Web Remote wird für **Spielen**, zum Beenden eines Spiels und für das Schaufenster gebraucht.
- dmdext mit `[zedmd] enabled = true` in der `DmdDevice.ini`.
- Auf der Platine: die App **DMD Flipper** geöffnet (am besten mit **Beim Einschalten starten**).

### Installation

1. Kopiere den Ordner `popper_bridge` auf den PC, zum Beispiel nach `C:\vPinball\nucleo_bridge`.
2. Installiere die Bibliotheken:
   ```
   pip install pyserial pillow
   ```
3. Prüfe, ob sie alles findet:
   ```
   python nucleo_bridge.py --check --report report.txt
   ```
   Der Bericht nennt, was gefunden wurde: Poppers Ordner, die Spalten der Datenbank, Listen, Medien, Web Remote,
   den Port der Platine, die `DmdDevice.ini`.
4. Trage in **Popper Setup → Emulators** für **jeden** sichtbaren Emulator als **erste** Zeile ein:
   - im **Launch Script**:
     ```
     curl -s -m 2 -G --data-urlencode "name=[GAMEFULLNAME]" http://127.0.0.1:58090/game/start >nul 2>&1
     ```
   - im **Close Script**:
     ```
     curl -s -m 2 http://127.0.0.1:58090/game/end >nul 2>&1
     ```
   `curl` gehört seit Version 1803 zu Windows 10. Läuft die Brücke nicht, blockieren diese Zeilen nichts: Sie
   schlagen nach 2 Sekunden fehl.
5. Lass die Brücke mit Windows starten (aus dem Ordner der Brücke):
   ```
   powershell -ExecutionPolicy Bypass -File ponte.ps1 installa
   ```
   Das legt eine geplante Aufgabe bei der Windows-Anmeldung an, ohne Fenster, mit niedriger Priorität, die neu
   gestartet wird, falls sie sich beendet; und startet die Brücke sofort.
6. Ausprobieren: Läuft kein Tisch, zeigt die Platine das Menü. Tippe auf einen Tisch, dann auf **Spielen**.

### Befehle von `ponte.ps1`

Die Befehle sind italienische Wörter. Aufruf so, aus dem Ordner der Brücke:
`powershell -ExecutionPolicy Bypass -File ponte.ps1 <Befehl>`

| Befehl | Was er tut |
|---|---|
| `installa` (installieren) | Start mit Windows (geplante Aufgabe) und sofortiger Start |
| `rimuovi` (entfernen) | Entfernt den Start mit Windows und stoppt die Brücke |
| `avvia` (starten) | Startet die Brücke |
| `ferma` (stoppen) | Stoppt die Brücke (und ein von ihr gestartetes dmdext): Der Port der Platine ist frei |
| `riavvia` (neu starten) | Stoppt und startet erneut |
| `stato` (Status) | An/aus, CPU und Speicher, Start mit Windows ja/nein, COM-Port der Platine, letzte Zeilen des Protokolls |
| `controllo` (Prüfung) | Führt die Prüfung (`--check`) aus und speichert den Bericht in `%LOCALAPPDATA%\nucleo_bridge\controllo.txt` |

Die Meldungen des Skripts sind italienisch („Ponte ACCESO“ = Brücke an, „SPENTO“ = aus, „Scheda“ = Platine).

- Protokoll: `%LOCALAPPDATA%\nucleo_bridge\ponte.log` (ab 5 MB wird es nach `ponte.log.1` verschoben).
- Status im Browser des PCs: `http://127.0.0.1:58090/status` zeigt, ob die Brücke den USB-Port hat und ob ein
  Spiel läuft.
- Optionen von Hand: `python nucleo_bridge.py [--popper ORDNER] [--port COMx] [--web-port 80]`.
  `--popper` brauchst du, wenn Popper nicht in einem Standardordner liegt (`C:\vPinball\PinUPSystem`,
  `C:\PinUPSystem`, `D:\vPinball\PinUPSystem`).

### Die Brücke aktualisieren

1. `ponte.ps1 ferma`
2. Kopiere die neuen Dateien in den Ordner der Brücke (die Caches bleiben in `%LOCALAPPDATA%\nucleo_bridge`).
3. `ponte.ps1 avvia`

`installa` brauchst du nur beim ersten Mal, oder wenn die geplante Aufgabe entfernt wurde.

### Stoppen oder entfernen

- Vorübergehend: `ponte.ps1 ferma` (sie startet bei der nächsten Windows-Anmeldung wieder, oder mit `avvia`).
- Dauerhaft: `ponte.ps1 rimuovi`, dann die beiden `curl`-Zeilen aus den Skripten der Emulatoren in Popper
  entfernen.

---

## 5. Das Tischmenü

Läuft kein Tisch und ist die Brücke an, ist die Platine ein Popper-Menü. Oben die Listen und drei Tasten; in der
Mitte der gewählte Tisch; unten die Wheel-Leiste.

### Die zwei Stile

Gewählt im Einstellungsfeld, **Menüstil**:

| Stil | Aussehen |
|---|---|
| **Automat** (Standard) | Wie Poppers Standard-Theme: die Wheel-Leiste zwischen Chromschienen mit einer cyanfarbenen Leuchtröhre, das gewählte Wheel in einer Glaskugel mit Chromring, **Spielen** leuchtend cyan. |
| **DMD** | Titel, Angaben, Listen und Tasten aus farbigen Punkten wie ein ColorDMD, das Backglass hinter einem LED-Raster, eckige Rahmen. Punkttext steht in Großbuchstaben. |

### Die Listen

| Liste | Was sie enthält |
|---|---|
| **Zuletzt** | Die gespielten Tische, der zuletzt gespielte zuerst |
| **Neu** | Alle Tische, die zuletzt in Popper hinzugefügten zuerst |
| **Meistgespielt** | Mindestens einmal gespielte Tische, nach Anzahl der Spiele |
| **Favoriten** | Die in Popper markierten Favoriten (leer: „Noch keine Favoriten: in Popper markieren.“) |
| **Alle** | Alle sichtbaren Tische, alphabetisch |

Oben rechts der Zähler (zum Beispiel „Visual Pinball X 28 / 72“) und drei Tasten:

- **A-Z** — öffnet **Gehe zu** (siehe unten);
- **Zufall** (Symbol mit gekreuzten Pfeilen) — **Überrasch mich**: ein zufälliger Tisch;
- **Zahnrad** — das Einstellungsfeld des DMD (Kapitel 7).

### Die Wheel-Leiste

- **Ziehe** die Leiste: Das Wheel in der Mitte, größer und beleuchtet, ist der gewählte Tisch. Die Leiste läuft
  im Kreis.
- **Tippe** auf ein Wheel: Es kommt in die Mitte.
- **Schwungvoll wischen**: Die Leiste läuft von selbst weiter, je schneller, desto weiter (höchstens ein Drittel
  der Liste); langsames Ziehen bewegt sie um die zurückgelegte Strecke.
- **Wische** im mittleren Bereich nach links oder rechts: ein Tisch vor oder zurück.
- **Mit Tastatur oder Fernbedienung**: Die Pfeiltasten links/rechts bewegen die Leiste, Eingabe löst die
  ausgewählte Taste aus (zuerst **Spielen**), Esc / Zurück schließt zuerst offene Fenster (Gehe zu, Anleitung,
  Rückfragen).

Ein Tisch ohne Wheel zeigt seinen Namen statt des Bildes.

### Gehe zu (A-Z)

- Oben die **Kategorien** der geöffneten Liste, in der Sprache der Platine (Visual Pinball X, Future Pinball,
  Pinball FX, Shoot'em-ups, Labyrinthe, Jump'n'Runs, Sport und Rennen, Andere Arcade, PC-Spiele), jede mit der
  Anzahl ihrer Tische in **dieser** Liste; die erste, **Alle Spiele**, hebt den Filter auf. Eine Kategorie
  filtert die Liste.
- Darunter die **Buchstaben**: Ein Buchstabe springt zum ersten Tisch, der damit beginnt. Buchstaben, mit denen
  kein Tisch beginnt, sind ausgegraut; Ziffern und Zeichen stehen unter **0-9**.
- Schließen: das **✕**, außerhalb des Kastens tippen, nach unten wischen oder Zurück.

### Überrasch mich

Die Zufallstaste zeigt einen zufälligen Tisch. Seine Karte bekommt dann die Taste **Noch einer**, um gleich einen
weiteren zu ziehen.

### Die Tischkarte

Der gewählte Tisch füllt den Bildschirm: sein Backglass dahinter (abgedunkelt), sein großes Wheel, Name,
Hersteller und Jahr, Spiele und Spielzeit („10 Spiele, 7 h 16 min gespielt“), das letzte Spiel (Datum als
TT.MM.JJJJ), **Favorit**, wenn er einer ist, die besten Highscores. Die Tasten:

| Taste | Was sie tut |
|---|---|
| **Spielen** | Startet den Tisch am Automaten (über Poppers Web Remote) |
| **Anleitung** | Nur wenn der Tisch in Popper eine Anleitungskarte hat: zeigt sie aufrecht in voller Breite; mit dem Finger scrollen, tippen (oder Zurück) zum Schließen. Bis die Seite da ist, zeigt sie „Wird geladen...“; sie schließt sich auch mit dem **✕**. Geht die Seite verloren, fragt die Platine noch zweimal nach und schließt sie dann mit „Die Anleitung kam nicht vom PC: noch einmal versuchen.“; hat der PC keine Anleitung für den Tisch: „Keine Anleitung für diesen Tisch.“ |
| **Komposition** | Wie der Tisch den Monitor des PCs und diesen Bildschirm nutzt (Kapitel 8), ohne ihn spielen zu müssen |
| **Noch einer** | Nach Überrasch mich: ein weiterer zufälliger Tisch |

Nach **Spielen** erscheint „Wird gestartet...“ mit dem Namen des Tisches; **Zurück** blendet nur diese Seite aus.
Hat Popper nach etwa 2 Minuten nichts gestartet (so lange wartet die Platine: Die Brücke versucht es erneut, solange Popper Starts ablehnt, etwa direkt nach einem Absturz): „Der Tisch ist nicht gestartet: Prüfe Poppers Web Remote.“

### Schaufenster: wenn du in Popper die Tasten des Automaten benutzt

Benutzt jemand in Popper den Joystick oder die Tasten des Automaten, folgt die Platine der Auswahl und zeigt das
**Schaufenster** dieses Tisches: Backglass, DMD und Topper nacheinander, alle 7 Sekunden, jedes in einem Schein
seiner eigenen Farben, dazu „Tippen, um einen Tisch zu wählen“. Ein Tippen öffnet das Menü auf diesem Tisch.

- Flyer und Anleitungen sind nicht im Schaufenster (in Popper sind es Hochformatseiten, seitlich gespeichert):
  Die Anleitung öffnest du mit der Taste **Anleitung**.
- Wählt Popper einen Eintrag, der kein Tisch ist („Go Back“, „Exit“, eine Playlist), zeigt die Platine seine
  Aktion groß in Punkten (zum Beispiel „Zurück“, „Beenden“ oder den Namen der Playlist) und darunter
  „In Popper: …“.
- Kehrt Popper in seinen Attract-Modus zurück, kommt das Menü der Platine zurück.
- Wird ein Tisch aus Popper gestartet (nicht von der Platine), wechselt die Platine trotzdem zum DMD.

### Attract-Modus

Nach 2 Minuten ohne Berührung dreht sich die Leiste von selbst (alle 8 Sekunden ein Tisch), und „Zum Auswählen
tippen“ blinkt. Das passiert nie während eines Spiels; nach einem Spiel beginnen die 2 Minuten neu, sodass der
gerade gespielte Tisch mit den aktuellen Highscores auf dem Bildschirm bleibt.

---

## 6. Während des Spiels

### Was die Platine zeigt

| Situation | Auf dem Bildschirm |
|---|---|
| VPX-Tisch mit DMD | Das DMD in Punkten, in der gewählten Anordnung (Kapitel 7) |
| VPX-Tisch, der sein DMD noch nicht geschickt hat | „Wird geladen...“ bis zu einer Minute, dann die Spielseite („Seit 12 min im Spiel“): ein Tisch ohne DMD, zum Beispiel ein alphanumerischer (siehe Kapitel 9) |
| Future-Pinball-Tisch mit DMD | Das DMD, von der Brücke gespiegelt |
| Pinball FX, MAME, PC-Spiele | Das Schaufenster des laufenden Spiels |
| PC seit 5 s still (ohne Popper) | Die Punkt-Uhr, 12 oder 24 Stunden wie die Systemuhr, wenn **Uhr, wenn der PC schweigt** eingeschaltet ist |

Solange das DMD eines Spiels zu sehen ist, sind die Systemgesten von NucleoOS (Leiste, Home, Zuletzt verwendet)
gesperrt: Ein versehentliches Wischen kann den Punktestand nicht verdecken.

### Gesten und Tasten

- **Kurz tippen** auf das DMD (oder auf das Schaufenster eines Spiels ohne DMD, oder auf die Seite „Im Spiel“ eines Tisches, der sein DMD noch nicht geschickt hat): die **Schnellaktionen**.
- **2 Sekunden halten** (oder Eingabe / Leertaste auf einer Tastatur, auch auf der Uhr und der Warteseite): das
  **Einstellungsfeld**. Bei offenem Einstellungsfeld funktionieren die Systemgesten wieder.
- **Zurück** (oder Esc) auf dem DMD öffnet und schließt das Einstellungsfeld: Es schließt nie die App.

### Schnellaktionen

| Eintrag | Was er tut |
|---|---|
| **Zurück zum DMD** | Schließt die Schnellaktionen |
| **Tische** | Tische während des Spiels durchblättern; oben eine Taste **Zurück zum DMD** |
| **Spiel beenden** | Schließt den laufenden Tisch („Spiel wird beendet...“), das Menü kommt zurück |
| **DMD-Einstellungen** | Öffnet das Einstellungsfeld |
| **Komposition des Tisches** | Die Komposition des laufenden Tisches (Kapitel 8), geöffnet auf dem Reiter **Dieser Bildschirm** |

Schnellaktionen ohne Auswahl schließen: außerhalb des Kastens tippen oder nach unten wischen.

Bei VPX-Tischen läuft das Beenden über **WLAN**, direkt zu Poppers Web Remote (das Kabel gehört in diesem Moment
dmdext): Die Platine muss im selben Netz wie der PC sein. Antwortet der PC nicht: „Der PC antwortet nicht über
WLAN: Spiel am Automaten beenden.“ Bei den anderen Emulatoren läuft das Beenden über das Kabel, über die Brücke. Schließt sich das Spiel nach den Wiederholungen nicht (auch beim Tischwechsel): „Der PC hat das Spiel nicht beendet: am Automaten beenden.“

### Einen anderen Tisch starten

Wähle unter **Tische** einen Tisch und tippe auf **Spielen**. Die Platine fragt „*X* läuft. Beenden und *Y*
starten?“: **Beenden und starten** schließt den laufenden Tisch und startet den neuen, sobald die Brücke das Ende
meldet; **Abbrechen** lässt alles, wie es ist.

### Wenn das Kabel dem DMD gehört

Während eines Spiels, dessen DMD über dmdext läuft (VPX-Tische), ist das Kabel vom DMD belegt: **Überrasch mich**, die Karten anderer Tische, die **Anleitung** und die Wahl der Backglass-Version melden „Nach dem Spiel verfügbar: Während des Spiels gehört das Kabel dem DMD.“

### Ein Tisch, der sich selbst beendet

Stürzt der Tisch ab oder wird er gewaltsam beendet (ohne Poppers Endmeldung), sagt die Platine es — „*Name* hat sich selbst beendet: Du kannst ihn hier neu starten.“ — und lässt ihn ausgewählt, bereit für **Spielen**.

---

## 7. Das Einstellungsfeld

Es öffnet sich, wenn du den Bildschirm 2 Sekunden hältst (oder mit Eingabe / Leertaste / Esc), über das Zahnrad
im Menü oder über **DMD-Einstellungen** in den Schnellaktionen. Oben:

- **die Statuszeile**: USB / WLAN, das Format in Klartext („16 Stufen“, „16 Farben“, „RGB“, „Uhr“, „Testbild“,
  „Punkteanzeigen“), Größe, Bilder pro Sekunde, Spiel;
- **darunter die WLAN-Adresse** für die `DmdDevice.ini` (`ws://…:9090/dmd`; „noch keine Adresse“, wenn die
  Platine in keinem Netz ist); hat sich die Popper-Brücke bei der Platine gemeldet, zeigt dieselbe Zeile auch „Brücke <Version> · Popper: bereit“ (oder „Datenbank nicht gefunden“);
- drei Tasten: **Zurück zum DMD** (heißt **Zurück**, solange kein PC das DMD ansteuert) schließt das
  Einstellungsfeld; **Testbild** (siehe unten); **DMD schließen** schließt die App, nach der unten beschriebenen
  Rückfrage.

Jede Änderung gilt sofort und bleibt auf der Platine gespeichert.

### Die App schließen

Jeder Weg aus DMD Flipper — **DMD schließen**, die Gesten Home / Zuletzt verwendet, die Systemtasten, die Schließen-Taste des Fensters im klassischen Desktop-Modus — fragt
zuerst **„DMD schließen?“**: während eines Spiels „Ein Tisch läuft: Der Flipper hat kein DMD, bis die App wieder
geöffnet wird.“, sonst „Der Flipper hat kein DMD und kein Tischmenü, bis die App wieder geöffnet wird.“
**Bleiben** (die bereits ausgewählte Taste) bricht ab; **Schließen** beendet. Zurück schließt die App nie.

### Bildschirm

| Einstellung | Was sie tut | Standard |
|---|---|---|
| **Anordnung für alle Tische** | Wie das DMD den Bildschirm nutzt (Tabelle unten). Die Karte zeigt Name und Beschreibung der Anordnung; hat der laufende Tisch eine eigene, steht es dort („Dieser Tisch hat eine eigene: …“). Tippe auf die Karte oder auf **Mit Vorschau wählen** (Kapitel 8). | Klassischer DMD |
| **Drehung** | 0°, 90°, 180°, 270° im Uhrzeigersinn, je nach Einbau der Platine | 0° |

Die Anordnungen, in der Reihenfolge der Vorschauen:

| Anordnung | Was sie tut |
|---|---|
| **Automatisch** | Jeder Tisch seine beste Anordnung nach seinen Bildern: Grafik mit Fenster mit FullDMD, Backglass mit Aussparung mit Gitter, sonst klassischer DMD. Ein 128x16-DMD bekommt DMD bildschirmfüllend (Zeilen rekonstruiert). |
| **DMD bildschirmfüllend** | Der ganze Bildschirm ohne Lücken: Die Zeilen werden rekonstruiert (128x64), die Punkte bleiben rund und groß. |
| **Klassischer DMD** | Quadratische Punkte und echte Proportionen, so groß wie möglich; Streifen oben und unten. |
| **Hoher DMD** | Der DMD über die ganze Breite und etwas höher; Streifen oben und unten. |
| **Gestreckter DMD** | Der ganze Bildschirm mit den Original-Punkten: Das Bild wird gestreckt, die Punkte bleiben klein, die Zeilen weit auseinander. |
| **Backglass + DMD** | Das Backglass des Tisches oben und der DMD darunter, wie in einer echten Backbox. Ohne Backglass wird der DMD zentriert. |
| **Nur Backglass** | Nur das Backglass des Tisches, kein DMD: Die Platine ist der Backglass-Bildschirm. |
| **Tisch-Grafik** | Die FullDMD-Grafik des Tisches oben und der DMD in einem breiten Streifen darunter. |
| **Grafik mit Fenster** | Die FullDMD-Grafik des Tisches auf dem ganzen Bildschirm und der DMD in ihrem Fenster. |
| **Backglass mit Aussparung** | Das Backglass mit Gitter auf dem ganzen Bildschirm und der DMD in seiner Aussparung. |

- Die Anordnungen mit Backglass oder Grafik nutzen die Bilder, die die Brücke zu Spielbeginn schickt: Ohne
  Brücke oder bei einem Tisch, der sie nicht hat, sind sie nicht verfügbar.
- **Gedrehter Bildschirm** (90°, 180°, 270°): Die Anordnungen mit Bildern zeigen den klassischen DMD; bei 90° und
  270° auch Hoher DMD und DMD bildschirmfüllend. Einstellungsfeld und Komposition sagen es: „Bildschirm gedreht:
  Layouts mit Bildern zeigen das klassische DMD.“
- Die Uhr füllt immer den ganzen Bildschirm mit runden Punkten, egal welche Anordnung gewählt ist.

### Tischmenü

| Einstellung | Werte | Standard |
|---|---|---|
| **Menüstil** | **Automat** / **DMD** (Kapitel 5) | Automat |

### Punkte

| Einstellung | Was sie tut | Standard |
|---|---|---|
| **Farbe** | Die Farbe einfarbiger DMDs, gewählt aus einer Reihe farbiger Kreise; die Beschriftung nennt die gewählte („Farbe · Orange“). Der leere Ring ist **Vom PC** (die Farbe, die der PC schickt); dann Orange, Rot, Gelb, Grün, Cyan, Weiß. Das gilt auch über USB: Einfarbige Bilder werden umgefärbt, farbige DMDs (ColorDMD, Serum, Paletten) behalten ihre Farben; **Vom PC** behält die Farbe von dmdext. | Orange |
| **Helligkeit** | 10 bis 100 %, mit dem Finger oder den Tasten; in beiden Fällen gespeichert | 100 % |
| **Punktform** | **Rund** oder **Eckig** | Rund |
| **Leuchten** | Ein sanfter Lichtschein in den Lücken zwischen den Punkten | Aus |

### System

| Einstellung | Was sie tut | Standard |
|---|---|---|
| **Uhr, wenn der PC schweigt** | Schickt der PC 5 Sekunden lang nichts (nachdem er etwas gezeigt hat), zeigt die Platine die Uhrzeit in Punkten (12 oder 24 Stunden, wie die Systemuhr), darunter das Datum. Sie erscheint nie über dem Popper-Menü oder während eines Spiels. | An |
| **Beim Einschalten starten** | NucleoOS öffnet DMD Flipper beim Einschalten | Aus |
| **Netzwerkport** | Der Port für WLAN. Er gilt, sobald du bestätigst oder das Einstellungsfeld schließt, und wird erst gespeichert, wenn die Platine wirklich darauf lauscht. Fehler erscheinen unter dem Feld („Port: eine Zahl von 1 bis 65535.“, „Port … kann nicht geöffnet werden.“): Dann bleibt der bisherige Port in Gebrauch. | 9090 |

### Testbild

10 Sekunden lang zeigt die Platine „TEST“ mit einem Sekunden-Countdown, die gewählte Farbe in 16 Stufen,
Farbbalken (Rot, Grün, Blau, Gelb, Cyan, Magenta, Weiß, Grau), einen Rahmen und einen vollen Block in der linken
oberen Ecke. Bilder vom PC werden in dieser Zeit ignoriert. Ein Tippen, Eingabe, Leertaste oder Esc beenden es
früher. Damit prüfst du Ausrichtung, Farben und Helligkeit des Automaten **ohne PC**: Ist der Block nicht oben
links, ändere die **Drehung**.

---

## 8. Der Bildschirm Komposition

Hier legst du mit Vorschauen aus den echten Bildern des Tisches fest, wie ein Tisch die Bildschirme des
Automaten nutzt. Er öffnet sich auf drei Wegen:

| Von wo | Was er festlegt |
|---|---|
| Einstellungsfeld → **Mit Vorschau wählen** | Die Anordnung **dieses Bildschirms** für **Alle Tische**: die zehn Anordnungen in einem Raster, alle sichtbar. Die Vorschauen nutzen die Bilder des laufenden oder des im Menü gewählten Tisches. |
| Tischkarte → **Komposition** | Monitor des PCs und dieser Bildschirm für **diesen** Tisch, ohne ihn spielen zu müssen |
| Schnellaktionen → **Komposition des Tisches** | Dasselbe für den laufenden Tisch; öffnet sich auf dem Reiter **Dieser Bildschirm** |

Der Monitor des PCs wird Tisch für Tisch gewählt: Im Einstellungsfeld wählst du nur diesen Bildschirm. Schließen
mit dem **✕** oben rechts oder mit Zurück. Oben sagt die Zeile „Bilder dieses Tisches: …“, was der Tisch hat
(Backglass und seine Größe, mit Gitter, FullDMD-Grafik, Doppel-Backglass); solange sie unterwegs sind: „Die
Bilder des Tisches kommen vom PC...“.

### Reiter „Dieser Bildschirm“

- **Echte Punkte, 1:1**: eine Lupe, die die Mitte des DMD in der echten Größe des Bildschirms zeigt, damit du
  Punktgröße und Zeilenabstände beurteilen kannst, bevor du gehst (ein Vorschaubild kann sie nicht zeigen).
- Name und Wirkung der gewählten Anordnung. Bei **Automatisch** für einen Tisch, der nicht läuft: „wird beim
  Tischstart gewählt“.
- **Punktgröße**: **Angepasst** (an die Anordnung) oder **Original** (quadratisch, ganze Pixel, nie gestreckt).
- **Rand**: 0, 1, 2 oder 3 Zeilen und Spalten unbeleuchteter Punkte um den DMD.
- Nur während eines Spiels mit einem 128x16-DMD: **DMD 128x16: rekonstruiert** (auf volle Höhe gebracht,
  Standard) oder **DMD 128x16: original**.
- Die Anordnungen als Karten: Für einen Tisch ist die erste **Wie alle Tische** (folgt der Wahl im
  Einstellungsfeld), danach nur die, die mit den Bildern des Tisches möglich sind.

### Reiter „Zweiter Bildschirm (PC)“

Legt fest, was der **Backglass-Monitor des PCs** für diesen Tisch zeigt. Die Brücke schreibt die Wahl dorthin, wo
B2S und dmdext sie beim Tischstart lesen (die `.res`-Datei des Tisches, `B2STableSettings.xml`, der Abschnitt des
Spiels in der `DmdDevice.ini`), nachdem sie von jeder Datei **eine Sicherungskopie** mit der Endung `.prima-di-s2`
angelegt hat.

| Wahl | Auf dem Monitor des PCs |
|---|---|
| **Wie geliefert** | Wie die Backglass-Datei des Tisches (B2S) es anordnet, unverändert |
| **Backglass** | Nur das Backglass, auf dem ganzen Monitor |
| **Backglass + DMD** | Das Backglass oben und der DMD darunter |
| **Backglass mit Gitter** | Das ganze Backglass mit Lautsprechergitter |
| **FullDMD-Grafik** | Die FullDMD-Grafik des Tisches (das zweite, kleinere Bild der B2S-Datei) |
| **Backglass + Grafik** | Das Backglass oben, darunter die FullDMD-Grafik |

Es erscheinen nur die Möglichkeiten, die es für diesen Tisch gibt. Außerdem:

- **DMD auch auf dem Monitor**: **Nein** (der DMD bleibt nur auf dieser Platine) oder **Ja** (auch der virtuelle
  DMD von dmdext auf dem Monitor). Bei **Wie geliefert** sind diese Tasten gesperrt: „Wähle zuerst eine der
  Kompositionen unten.“
- **Backglass-Version**: **Authentisch** oder **Fantasy**, bei Doppel-Backglass: B2S schaltet ab dem nächsten
  Spiel die andere Bildreihe ein.
- Änderungen gelten **ab dem nächsten Spiel** („Zweiter Bildschirm bereit fürs nächste Spiel“). Läuft gerade ein
  Spiel, erscheint **Tisch jetzt neu starten**, um ihn zu schließen und sofort mit der neuen Komposition wieder
  zu öffnen.
- „Kein zweiter Bildschirm am PC“: Die Brücke hat keinen zweiten Monitor gefunden. „Dieser Tisch hat keine
  Backglass-Datei (B2S)“: Der Monitor zeigt Poppers Backglass, es gibt nichts zu wählen. „Der PC hat nicht geantwortet: Die Backglass-Datei konnte nicht beschrieben werden.“: Der PC hat die Backglass-Datei nach 3 Versuchen nicht beschrieben; öffne die Komposition erneut.

---

## 9. Fehlerbehebung

| Problem | Was tun |
|---|---|
| **Kein DMD auf der Platine** | Prüfe, ob die App **vor** dem Tischstart offen ist; ob in der `DmdDevice.ini` `[zedmd] enabled = true` (USB) oder `[networkstream]` mit der richtigen URL (WLAN) steht; öffne das Einstellungsfeld und sieh dir die Statuszeile an (USB / WLAN, Bilder pro Sekunde). |
| **dmdext meldet „ZeDMD is not available“** | Die App war beim Tischstart nicht offen, oder ein anderes Programm belegt den COM-Port. Schalte **Beim Einschalten starten** ein, schließe Programme, die den Port nutzen, und lege den Port bei Bedarf mit `port = COMx` unter `[zedmd]` fest. |
| **Die Platine startet neu, wenn ein Tisch startet** | Beim Öffnen des Ports schickt dmdext der Platine ein Reset-Signal: Die App blockiert es **nur, solange sie offen ist**. Lass sie offen (Beim Einschalten starten). Startet sie trotzdem neu, entferne den Hub: Ein Hub ohne Stromversorgung lässt die Platine ohne genug Strom. |
| **Das Tischmenü erscheint nicht** | Öffne das Einstellungsfeld: Fehlt in der Adresszeile der Teil „Brücke …“, spricht die Brücke nicht mit der Platine. Die Brücke muss laufen: `ponte.ps1 stato` („Ponte ACCESO“ und „Scheda: COMx“). Steht die Platine auf „non collegata“ (nicht verbunden), prüfe das Kabel. Führe `ponte.ps1 controllo` aus und lies den Bericht. Während eines Spiels gibt es kein Menü: Es erscheint am Spielende. Ist die Brücke am Kabel, bittet die Platine sie alle 3 Sekunden selbst um ihre Anmeldung: Öffnest du die App erneut, kommt das Menü von selbst zurück. |
| **„Popper: Datenbank nicht gefunden“** im Einstellungsfeld | Die Brücke findet Poppers Datenbank nicht: Prüfe Poppers Ordner mit `ponte.ps1 controllo` (liegt er in einem Nicht-Standardordner, starte die Brücke mit `--popper`). |
| **„Keine Tische in dieser Liste.“** | Normal bei **Zuletzt** oder **Meistgespielt**, wenn du noch nicht gespielt hast; probiere **Alle**. |
| **„Der PC hat die Tischliste nicht gesendet: Popper prüfen.“** | `ponte.ps1 controllo`: Wurden Poppers Ordner und Datenbank gefunden? |
| **Bilder fehlen (Wheel, Backglass)** | Beim ersten Mal kommen die Bilder einzeln vom PC; mit microSD lädt die Platine sie bei ruhendem Menü selbst, danach geht alles sofort. Die Datei in Popper muss wie das Spiel heißen (oder „Name(1).png“, „Name (2).png“). Ein in Popper ergänztes Bild erscheint innerhalb von 10 Minuten oder nach erneutem Öffnen der App. |
| **„Das Kabel zum PC ist belegt: gleich noch einmal versuchen.“** | Das Kabel übertrug gerade ein großes Bild: noch einmal versuchen. |
| **„Die Anleitung kam nicht vom PC: noch einmal versuchen.“** | Die Seite ging auf dem Kabel verloren, auch nach zwei weiteren Anfragen: gleich noch einmal versuchen. |
| **„Nach dem Spiel verfügbar: Während des Spiels gehört das Kabel dem DMD.“** | Normal während eines VPX-Tisches: Die Funktion ist nach dem Spiel wieder da. |
| **„… hat sich selbst beendet: Du kannst ihn hier neu starten.“** | Der Tisch ist abgestürzt oder wurde gewaltsam beendet: Tippe auf **Spielen**, um ihn neu zu starten. |
| **„Der PC hat nicht geantwortet: Die Backglass-Datei konnte nicht beschrieben werden.“** | Die Brücke hat für die Komposition nicht geantwortet: Prüfe, ob sie läuft (`ponte.ps1 stato`), und öffne die Komposition erneut. |
| **„Keine Anleitung für diesen Tisch.“** | Der PC hat für diesen Tisch keine Anleitungskarte zu schicken: Füge in Popper eine hinzu (Bildschirm GameHelp). |
| **Der Tisch startet nicht** („Der Tisch ist nicht gestartet: Prüfe Poppers Web Remote.“ / „Der PC hat den Tisch nicht gestartet: Popper prüfen.“) | Popper muss mit eingeschaltetem Web Remote laufen (`useWEB = true ;`, Popper neu gestartet, COM-Komponente registriert, Port 80). Starte denselben Tisch aus Popper: Startet er auch dort nicht, liegt das Problem am Tisch oder an Popper. |
| **Ich kann das Spiel nicht von der Platine beenden** („Der PC antwortet nicht über WLAN: Spiel am Automaten beenden.“ / „Der PC hat das Spiel nicht beendet: am Automaten beenden.“) | Bei VPX-Tischen läuft das Beenden über WLAN: Die Platine muss im Netz des Automaten sein und Poppers Web Remote (TCP 80) auf dem PC erreichen. Meist ist es so, wie installiert, erreichbar; falls nicht, erlaube in der Windows Defender Firewall eingehende Verbindungen auf TCP 80 für PinUP Popper im privaten Netzwerk. Bis dahin mit den Tasten des Automaten beenden. |
| **Welche URL trage ich für WLAN ein?** | Die auf der Warteseite der App oder oben im Einstellungsfeld: `ws://<IP der Platine>:9090/dmd`. Ändert sich die IP bei jedem Einschalten, reserviere im Router eine feste IP. „WLAN nicht verbunden“: Verbinde die Platine mit dem Netz des Automaten oder nutze USB. |
| **„Port: eine Zahl von 1 bis 65535.“ / „Port … kann nicht geöffnet werden.“** | Der eingegebene Port ist ungültig, oder die Platine kann ihn nicht öffnen: Wähle unter **Netzwerkport** einen anderen. Bis es klappt, bleibt der bisherige Port in Gebrauch. |
| **Alphanumerische oder Segment-Tische (80er-Jahre) ohne DMD auf der Platine** | dmdext wandelt Segmentanzeigen selbst um, aber nur, wenn VPinMAME ihm die Anzeige weitergibt: Für dieses ROM muss der Registry-Schlüssel `HKCU\Software\Freeware\Visual PinMame\<rom>\showpindmd` auf 1 stehen. Sichere die Registry, bevor du ihn änderst. |
| **„Dieses DMD (…) passt nicht auf den Bildschirm.“** | Über WLAN ist das DMD größer als 512x256: Nutze in dmdext eine kleinere Größe oder USB. |
| **Bild steht auf dem Kopf oder Farben stimmen nicht** | **Testbild**: Der volle Block muss oben links sein (sonst **Drehung**); für einfarbige DMDs die **Farbe** wählen. |
| **Kleine Punkte oder „zu viel Schwarz“ zwischen den Zeilen** | Nimm **DMD bildschirmfüllend** oder **Klassischer DMD** statt **Gestreckter DMD**; prüfe mit der Lupe **Echte Punkte, 1:1**. |
| **Das Backglass erscheint nicht, obwohl ich es gewählt habe** | Bei gedrehtem Bildschirm zeigen die Anordnungen mit Bildern den klassischen DMD: Stelle die **Drehung** wieder auf 0°. |
| **Das Einstellungsfeld öffnet sich im Spiel nicht** | Halte den Finger volle 2 Sekunden ruhig auf dem Bildschirm (kurzes Tippen öffnet die Schnellaktionen), oder drücke Zurück. |
| **Nach einem VPX-Absturz startet Popper nichts mehr** | Die Brücke schließt den übrig gebliebenen B2S-Server selbst („closed b2sbackglassserverexe“ im Protokoll). Passiert das nicht, prüfe, ob die Brücke läuft. |

Wenn du ein Problem meldest, füge die Ausgabe von `ponte.ps1 stato` und die letzten Zeilen von
`%LOCALAPPDATA%\nucleo_bridge\ponte.log` bei.

---

## 10. Firmware sicher aktualisieren

Die Einstellungen von DMD Flipper liegen im internen Speicher der Platine und überstehen Updates.

- **Über WLAN** (der normale Weg): **Einstellungen → Systemupdate**. Für das DMD ist nichts Besonderes nötig.
- **Über das USB-Kabel mit esptool**: Solange DMD Flipper offen ist, **ignoriert die Platine Resets über den
  USB-Port** (das schützt sie vor dmdext), daher könnte esptool sie nicht neu starten. Vor dem Schreiben:
  1. die App schließen: zum Home-Bildschirm gehen und „DMD schließen?“ mit **Schließen** beantworten, oder
     `GET /api/ui/home` aufrufen, das sie ohne Rückfrage schließt;
  2. auf dem PC die Brücke stoppen, die den COM-Port belegt: `ponte.ps1 ferma`;
  3. während des Schreibens keine Tische starten;
  4. danach: `ponte.ps1 avvia` und DMD Flipper wieder öffnen (oder **Beim Einschalten starten** das erledigen lassen).

---

## 11. Bekannte Grenzen

- Über USB ist die Platine ein 128x32-DMD: dmdext skaliert andere Größen. Über WLAN werden sie wie gesendet
  gezeigt, bis 512x256.
- Über WLAN immer nur ein PC.
- Mit **DMD bildschirmfüllend** wird das DMD mit etwa 30 Bildern pro Sekunde aktualisiert; mit den Anordnungen mit
  quadratischen Punkten etwa 52-60.
- Woher das DMD kommt: VPX-Tische schicken es mit dmdext über USB; Future-Pinball-Tische mit DMD spiegelt die
  Brücke mit dem Mirror-Modus von dmdext; Pinball FX, MAME und PC-Spiele zeigen die Schaufensterbilder des Spiels.
- Über das Kabel kommen große Nachrichten (Bilder) manchmal beschädigt an: Die Brücke sendet sie erneut, bis die
  Platine sie bestätigt; selten fehlt ein Bild bis zum nächsten Durchgang.
- Ohne microSD kommen die Bilder jedes Mal über das Kabel.
- Jede Liste fasst höchstens 2000 Tische.
- Bei gedrehtem Bildschirm zeigen die Anordnungen mit Bildern den klassischen DMD.
- Flyer und Anleitungen sind nicht im Schaufenster; in Popper ausgeblendete Tische erscheinen nicht im Menü.
- Änderungen am Monitor des PCs gelten ab dem nächsten Spiel (oder mit **Tisch jetzt neu starten**).
- Poppers Web Remote funktioniert nur, solange Popper läuft.

---

## 12. Datenschutz und Netzwerk

- Die Platine spricht **nur mit dem PC des Automaten, im lokalen Netz**: Sie empfängt das DMD (USB oder WLAN auf
  Port 9090) und ruft, um während eines VPX-Spiels einen Tisch zu beenden oder zu starten, Poppers Web Remote auf
  dem PC auf (Port 80). Die App schickt nichts ins Internet.
- **Der WLAN-DMD-Server (Port 9090) hat keine Authentifizierung**, und **Poppers Web Remote hat kein Passwort**:
  Nutze beide nur in einem vertrauenswürdigen Heimnetz, nie in einem öffentlichen Netz und nie aus dem Internet
  erreichbar.
- Die Brücke lauscht nur auf dem PC selbst (`127.0.0.1:58090`), liest Poppers Datenbank nur lesend und legt ihre
  umgewandelten Bilder in `%LOCALAPPDATA%\nucleo_bridge\cache` ab. Dateien von B2S und dmdext schreibt sie nur,
  wenn du eine Monitor-Komposition wählst (Kapitel 8), mit Sicherungskopie.
- Die Platine speichert die empfangenen Bilder und die letzten Listen auf der microSD, in `/sdcard/data/dmd`.
