# sc-im, Tabellenkalkulation

## Wofür sie da ist

sc-im ist eine echte Tabellenkalkulation im Vollbild: Zellen mit Zahlen, Texten und Formeln, die sich selbst neu berechnen, Funktionen (`@sum`, `@avg`, `@max`, `@if`, Datum und Uhrzeit), Zahlenformate, Spaltenbreiten, Sortieren, Filter, Rückgängig und Wiederholen. Sie öffnet **Excel-`.xlsx`**- und **`.xls`**-, **LibreOffice-`.ods`**-, **`.csv`**- und **`.tsv`**-Dateien und speichert als `.xlsx`, `.csv` und `.tsv`: eine Ausgabenliste, eine Inventur, einen Ratenplan, eine Tabelle, die dir jemand gemailt hat.

## So öffnest du sie

- **Tippe auf das Symbol** auf dem Startbildschirm: Eine leere Tabelle öffnet sich.
- Tippe in **Dateien** auf eine `.xlsx`-, `.xls`-, `.ods`- oder `.csv`-Datei und wähle **Öffnen mit › sc-im**.
- Im Terminal: `sc-im ausgaben.xlsx`.

## Touch und Tastatur

- **Tippe auf eine Zelle**, um sie auszuwählen. **Wische nach oben und unten**, um durch die Tabelle zu scrollen.
- Die Tastenleiste über der Tastatur hat **Esc**, **Tab**, **Strg** und die **Pfeile**.
- Mit einer USB- oder KeyDeck-Tastatur funktionieren auch Strg, Umschalt und die Funktionstasten.

## Die wichtigsten Befehle

sc-im bedient man wie den Editor vim: Ein Buchstabe ist ein Befehl, lange Befehle beginnen mit einem Doppelpunkt.

| Tasten | Was sie tun |
|---|---|
| Pfeile | bewegen die Auswahl |
| `=` | eine Zahl oder Formel eingeben, z. B. `=@sum(B0:B9)`, dann Enter |
| `<` `>` `\` | Text eingeben, links, rechts, zentriert ausgerichtet |
| `e` / `E` | die Zahl / den Text der Zelle bearbeiten |
| `x` | die Zelle löschen |
| `u` / Strg+R | rückgängig / wiederholen |
| `yy`, dann `p` | die Zelle kopieren und einfügen |
| `ir` `ic` | eine Zeile / Spalte einfügen |
| `dr` `dc` | die Zeile / Spalte löschen |
| `f>` `f<` | Spalte breiter / schmaler |
| `f+` `f-` | mehr / weniger Nachkommastellen |
| `goB12` | zur Zelle B12 springen |
| `:w` | speichern; `:w konten.xlsx` speichert als Excel |
| `:e csv konten.csv` | als CSV exportieren (auch `tab`, `txt`, `mkd`) |
| `:load datei.ods` | eine andere Datei öffnen |
| `:q` | beenden (`:q!` ohne zu speichern) |
| `:h` | das ganze Handbuch (`q` schließt es) |

Zellen heißen nach Spalte und Zeile, **die Zeilen ab 0 gezählt**: `A0` ist die erste Zelle oben links. In Formeln schreibt man Bereiche als `A0:A9`.

## Mit Anima

sc-im ist ein Systemprogramm: Anima liest damit deine Tabellen. Frag zum Beispiel „Wie hoch ist die Summe in ausgaben.xlsx?" oder „Fasse die Tabelle bestellungen.ods zusammen".

## Grenzen

- Diagramme, Makros, Bilder und Kommentare in Excel-Dateien werden nicht gelesen; aus einer `.xlsx` liest man Werte, Texte und (mit der Option `--xlsx_readformulas`) Formeln.
- `.ods`-Dateien lassen sich öffnen, werden aber als `.xlsx`, `.csv` oder im Format von sc-im (`.sc`) gespeichert.
- Aus einer alten `.xls`-Datei wird das erste Blatt gelesen; gespeichert wird als `.xlsx`.
- Die System-Zwischenablage und `!`-Befehle (externe Programme) sind nicht verfügbar.
- Sehr große Tabellen (zehntausende Zeilen) passen eventuell nicht in den Speicher des Geräts: sc-im meldet das und die Originaldatei bleibt unverändert, weil zuerst in eine temporäre Datei gespeichert wird.

## Im Terminal

- **Tippe auf das Symbol**, um das Terminal mit dem bereits gestarteten Programm zu öffnen, oder öffne **Terminal** und gib den Befehl ein.
- Deine Dateien liegen im Ordner **home** der SD-Karte (`/sdcard/home`): Für das Programm ist dieser Ordner `/`. Dateien vom PC kopierst du, indem du die SD-Karte in einen Kartenleser steckst.
- Eine Tabelle lesen, ohne sie zu öffnen: `sc-im --nocurses --quit_afterload --export_csv ausgaben.xlsx` (mit `--export_mkd` kommt eine Tabelle heraus).
- `apps` listet die installierten Programme, `help sc-im` die Syntax.

---

*This product includes software developed by Andrés Martinelli <andmarti@gmail.com>.*
