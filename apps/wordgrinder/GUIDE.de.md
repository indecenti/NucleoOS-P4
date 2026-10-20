# WordGrinder, Textverarbeitung

## Wofür es da ist

WordGrinder ist ein Programm zum **Schreiben**: Briefe, Berichte, Notizen, Kapitel. Es hält dich beim Text, ohne Ablenkung, hat aber alles Nötige: Überschriften, Absätze, Aufzählungen und nummerierte Listen, **fett**, *kursiv*, unterstrichen, Suchen und Ersetzen, Wortzählung, Rückgängig und Wiederholen, mehrere Dokumente in einer Datei. Es öffnet und exportiert **OpenDocument `.odt`** (LibreOffice; auch Word öffnet es), **Markdown**, **HTML** und Text; außerdem exportiert es LaTeX, troff und Org.

## So öffnest du es

- **Tippe auf das Symbol** auf dem Startbildschirm: Ein leeres Dokument öffnet sich.
- Tippe in **Dateien** auf eine `.odt`-, `.md`-, `.html`- oder `.txt`-Datei und wähle **Öffnen mit › WordGrinder**.
- Im Terminal: `wordgrinder brief.odt`.

Eine `.odt`-, `.md`-, `.html`- oder `.txt`-Datei wird **importiert**: Beim Speichern schreibt WordGrinder daneben eine `.wg`-Datei (sein Format) und das Original bleibt unverändert. Um es wieder als `.odt` zu schreiben, nimm **ESC › File › Export**.

## Touch und Tastatur

- **Tippe in den Text**, um den Cursor dorthin zu setzen. **Wische nach oben und unten**, um zu scrollen.
- **ESC** (in der Tastenleiste über der Tastatur) öffnet das **Menü**: File, Edit, Style, Documents, Navigation. Jeder Eintrag hat einen Buchstaben, die häufigsten haben eine Strg-Kurztaste.
- Mit einer USB- oder KeyDeck-Tastatur funktionieren auch Umschalt+Pfeile (Auswahl) und Strg+Pfeile (Wort für Wort).

| Tasten | Was sie tun |
|---|---|
| ESC | Menü |
| Strg+S | speichern |
| Strg+O | ein Dokument öffnen |
| Strg+B / Strg+U | fett / unterstrichen (kursiv: ESC › Style › Set italic) |
| Strg+N | normaler Text |
| Strg+P | Absatzstil: Überschriften H1–H4, Listen, Zitate |
| Strg+Z / Strg+Y | rückgängig / wiederholen |
| Strg+X / Strg+C / Strg+V | ausschneiden / kopieren / einfügen |
| Strg+F / Strg+K | suchen und ersetzen / weitersuchen |
| Strg+G | zu einer Seite oder Überschrift springen |
| Strg+Q | beenden |

Die Menüs von WordGrinder sind auf Englisch.

## Mit Anima

WordGrinder ist ein Systemprogramm: Anima liest damit eine `.odt`-Datei oder macht aus Notizen ein Dokument. Frag zum Beispiel „Lies mir die Datei bericht.odt vor" oder „Mach aus notizen.md ein odt-Dokument".

## Grenzen

- Word-Dateien `.docx` und `.doc` lassen sich nicht öffnen: speichere sie als `.odt`.
- Keine Bilder, Tabellen, Spalten oder Seitenlayout: Es ist ein Programm für Text. Bilder einer importierten `.odt` gehen verloren.
- Die Rechtschreibprüfung hat keine Wörterbücher installiert.
- Die Zwischenablage gibt es nur im Programm.

## Im Terminal

- **Tippe auf das Symbol**, um das Terminal mit dem bereits gestarteten Programm zu öffnen, oder öffne **Terminal** und gib den Befehl ein.
- Deine Dateien liegen im Ordner **home** der SD-Karte (`/sdcard/home`): Für das Programm ist dieser Ordner `/`. Dateien vom PC kopierst du, indem du die SD-Karte in einen Kartenleser steckst.
- Umwandeln, ohne zu öffnen: `wordgrinder --convert brief.odt brief.txt` (die Endung bestimmt das Format: `.odt` `.md` `.html` `.txt` `.wg`, als Ausgabe auch `.tex`).
- `apps` listet die installierten Programme, `help wordgrinder` die Syntax.
