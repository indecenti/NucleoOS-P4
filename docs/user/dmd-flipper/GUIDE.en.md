# DMD Flipper — User guide

**DMD Flipper** turns your NucleoOS board (ESP32-P4 with a 1024x600 touch screen) into the DMD of your
virtual pinball cabinet and, whenever no table is running, into a touch menu of your PinUP Popper tables.
In the English interface the app is called **DMD Pinball**.

This guide is for cabinet owners: it explains how to connect the board, how to install the small "bridge"
program on the PC, and how to use every screen. No programming knowledge needed.

![The dot clock while the PC is silent](../../screenshots/dmd_clock.jpg)

---

## Contents

1. [What it is and what you need](#1-what-it-is-and-what-you-need)
2. [Quick start in 5 steps](#2-quick-start-in-5-steps)
3. [Connecting the PC](#3-connecting-the-pc)
4. [Installing the PinUP Popper bridge](#4-installing-the-pinup-popper-bridge)
5. [The table menu](#5-the-table-menu)
6. [During a game](#6-during-a-game)
7. [The settings panel](#7-the-settings-panel)
8. [The Composition screen](#8-the-composition-screen)
9. [Troubleshooting](#9-troubleshooting)
10. [Updating the firmware safely](#10-updating-the-firmware-safely)
11. [Known limits](#11-known-limits)
12. [Privacy and network](#12-privacy-and-network)

---

## 1. What it is and what you need

The app does two jobs:

- **The cabinet's DMD.** During a game the PC (Visual Pinball X + VPinMAME + dmdext) sends the DMD to the
  board, which draws it as LED/plasma dots. Frames can arrive over **USB** or **Wi-Fi**, or both at once.
- **A touch menu for PinUP Popper.** When no table is running the board shows your tables (recent, new, most
  played, favourites, all) with wheels, backglasses, stats and high scores: tap **Play** and the table starts
  on the cabinet. This needs the **bridge**, a small program running on the PC (chapter 4).

### What you need

| Item | Details |
|---|---|
| NucleoOS board | With the **DMD Pinball** app. A microSD card is optional: with one, the board keeps the pictures and the last lists there and the menu becomes instant; without one the menu still works, but pictures come over the cable every time and nothing is kept between sessions. |
| USB cable | From the board's USB programming port (Windows sees it as USB device 303A:1001) **straight to the PC**. No unpowered hub: the board would run short of power and restart. |
| Cabinet PC | Windows 10 (version 1803 or later) or 11, Visual Pinball X, VPinMAME with **dmdext 2.x** (its `DmdDevice.dll`). |
| PinUP Popper | For the table menu, with its **Web Remote** turned on (chapter 4). |
| Python | 3.8 or later, for the bridge. |
| Wi-Fi | Optional for the DMD (USB is enough), **recommended** with Popper: it is how the board quits a VPX table, or starts another one, while a game is running. |

---

## 2. Quick start in 5 steps

1. **Connect** the board to the PC with the USB cable, directly (no unpowered hub).
2. **Open DMD Pinball** on the board. Hold the screen for 2 seconds to open the panel and turn on
   **Start at power-on**: the app must already be open when a table starts.
3. **On the PC**, in dmdext's `DmdDevice.ini`, turn on the USB output:
   ```ini
   [zedmd]
   enabled = true
   ```
4. **Install the bridge** for Popper (chapter 4): copy the folder, `pip install pyserial pillow`, add the two
   lines to the emulators' scripts in Popper, then
   `powershell -ExecutionPolicy Bypass -File ponte.ps1 installa`.
5. **Try it**: with no table running the board shows the menu. Pick a table, tap **Play**: the table starts and
   the board switches to the DMD. Quit the table: the menu comes back.

---

## 3. Connecting the PC

The board takes the DMD in two ways, both active at the same time. You choose in dmdext's `DmdDevice.ini` on
the PC (the file next to VPinMAME's `DmdDevice.dll`). The app's waiting page shows both setups, with your
board's exact address.

### USB (recommended)

```ini
[zedmd]
enabled = true
```

- The board presents itself to dmdext as a **ZeDMD 128x32**: dmdext scans the serial ports and finds it by
  itself. To pin the port, add `port = COMx` under `[zedmd]` (the board's COM port number from Device
  Manager; `ponte.ps1 stato` shows it on the "Scheda" line).
- **The app must be open when the table starts**: that is when dmdext looks for the device. This is why
  **Start at power-on** is worth turning on (below).
- DMDs of other sizes (for example 128x16 or 192x64) are scaled by dmdext to 128x32.
- With the Popper bridge, USB is also the menu's channel: the bridge and dmdext hand the port to each other
  at every game start and end, by themselves.

### Wi-Fi

```ini
[networkstream]
enabled = true
url = ws://<board IP>:9090/dmd
retry = true
```

- The exact address (`ws://…:9090/dmd`) is written on the app's waiting page and at the top of the settings
  panel, under the status line.
- Over Wi-Fi the board shows the DMD at its real size, up to 512x256.
- One PC at a time: a new connection replaces the previous one.
- Port 9090 can be changed in the panel (**Network port**); with `retry = true` the PC reconnects by itself.

> **Tip — fixed IP.** In your router, give the board a fixed address (DHCP reservation): the URL in
> `DmdDevice.ini` then never changes.

### Start at power-on

In the panel (**System** section), turn on **Start at power-on** (off by default): a second after power-on,
NucleoOS opens DMD Pinball directly. If the lock screen is on, the app does not open by itself until you
unlock: on a cabinet it is better not to use the lock screen.

---

## 4. Installing the PinUP Popper bridge

### What it does

The bridge (`nucleo_bridge.py`) runs on the cabinet PC, starts with Windows and is very light (low priority,
about 27 MB, no window). All the graphics work is done by the board.

- **While no table runs** it sends the board the table lists, the table cards and the pictures (converted
  once and kept in a cache).
- **When a VPX table starts** it hands the USB port to dmdext: the board becomes the DMD. At the end of the
  game it takes the port back.
- **Future Pinball tables with a DMD**: the bridge mirrors it to the board with dmdext's mirror mode.
- **Pinball FX, MAME, PC games**: the bridge keeps the port and the board shows the running game's showcase,
  with the quick actions.
- **Starting and quitting** tables go through Popper's Web Remote.
- It reads Popper's database (`PUPDatabase.db`) **read-only**: it never writes anything there. Tables hidden
  in Popper do not appear.
- If Popper does not report the end of a game (an emulator that crashed or was killed), the bridge notices
  by itself when the emulator's process ends, and the menu comes back.
- It closes the B2S backglass server that VPX sometimes leaves open after a crash (otherwise Popper would not
  start anything any more).
- It can be restarted even during a game: it recognises the running table.

### Requirements

- Python 3.8 or later, with `pip install pyserial pillow` (numpy is optional: it makes pictures faster).
- PinUP Popper, with the **Web Remote** turned on:
  - `useWEB = true ;` in `PUPMenuScriptSysOptions.txt` and in `PuPMenuScript.pup` (Popper's "script
    options"), then restart Popper;
  - the COM component `PuPServer.WebServer` registered (if it is not: run `PUPServerRegisterCOM.bat` as
    administrator);
  - port 80 (if Popper uses another port, start the bridge with `--web-port`).
  The Web Remote is needed for **Play**, for quitting a game and for the showcase.
- dmdext with `[zedmd] enabled = true` in `DmdDevice.ini`.
- On the board: the **DMD Pinball** app open (best with **Start at power-on**).

### Installation

1. Copy the `popper_bridge` folder to the PC, for example to `C:\vPinball\nucleo_bridge`.
2. Install the libraries:
   ```
   pip install pyserial pillow
   ```
3. Check that it finds everything:
   ```
   python nucleo_bridge.py --check --report report.txt
   ```
   The report says what it finds: Popper's folder, the database columns, lists, media, Web Remote, the
   board's port, `DmdDevice.ini`.
4. In **Popper Setup → Emulators**, for **every** visible emulator, add as the **first** line:
   - in the **Launch Script**:
     ```
     curl -s -m 2 -G --data-urlencode "name=[GAMEFULLNAME]" http://127.0.0.1:58090/game/start >nul 2>&1
     ```
   - in the **Close Script**:
     ```
     curl -s -m 2 http://127.0.0.1:58090/game/end >nul 2>&1
     ```
   `curl` ships with Windows 10 since version 1803. If the bridge is not running these lines block nothing:
   they fail within 2 seconds.
5. Make the bridge start with Windows (from the bridge's folder):
   ```
   powershell -ExecutionPolicy Bypass -File ponte.ps1 installa
   ```
   This creates a scheduled task at Windows logon, with no window, at low priority, restarted if it ever
   closes; and starts the bridge right away.
6. Try it: with no table running, the board shows the menu. Tap a table, then **Play**.

### `ponte.ps1` commands

The commands are Italian words. Run them like this, from the bridge's folder:
`powershell -ExecutionPolicy Bypass -File ponte.ps1 <command>`

| Command | What it does |
|---|---|
| `installa` (install) | Start with Windows (scheduled task) and start now |
| `rimuovi` (remove) | Removes the start with Windows and stops the bridge |
| `avvia` (start) | Starts the bridge |
| `ferma` (stop) | Stops the bridge (and any dmdext it started): the board's port is free |
| `riavvia` (restart) | Stops and starts again |
| `stato` (status) | On/off, CPU and memory, start with Windows yes/no, the board's COM port, the last lines of the log |
| `controllo` (check) | Runs the check (`--check`) and saves the report to `%LOCALAPPDATA%\nucleo_bridge\controllo.txt` |

The script's messages are in Italian ("Ponte ACCESO" = bridge on, "SPENTO" = off, "Scheda" = board).

- Log: `%LOCALAPPDATA%\nucleo_bridge\ponte.log` (over 5 MB it is moved to `ponte.log.1`).
- Status from the PC's browser: `http://127.0.0.1:58090/status` tells whether the bridge has the USB port and
  whether a game is running.
- Manual options: `python nucleo_bridge.py [--popper FOLDER] [--port COMx] [--web-port 80]`.
  `--popper` is needed if Popper is not in a standard folder (`C:\vPinball\PinUPSystem`, `C:\PinUPSystem`,
  `D:\vPinball\PinUPSystem`).

### Updating the bridge

1. `ponte.ps1 ferma`
2. Copy the new files into the bridge's folder (the caches stay in `%LOCALAPPDATA%\nucleo_bridge`).
3. `ponte.ps1 avvia`

`installa` is only needed the first time, or if the scheduled task was removed.

### Stopping or removing it

- For a while: `ponte.ps1 ferma` (it starts again at the next Windows logon, or with `avvia`).
- For good: `ponte.ps1 rimuovi`, then remove the two `curl` lines from the emulators' scripts in Popper.

---

## 5. The table menu

With no table running and the bridge on, the board is a Popper menu. At the top, the lists and three
buttons; in the middle, the selected table; at the bottom, the wheel bar.

### The two looks

Chosen in the panel, **Menu style**:

| Look | Appearance |
|---|---|
| **Cabinet** (default) | Like Popper's default theme: the wheel bar between chrome rails with a cyan light tube, the selected wheel inside a chrome-and-glass orb, a lit cyan **Play**. |
| **DMD** | Titles, facts, lists and buttons drawn as coloured dots like a ColorDMD, the backglass seen through an LED grid, square bezels. Dot text is in capitals. |

### The lists

| List | What it holds |
|---|---|
| **Recent** | The tables you have played, latest first |
| **New** | Every table, the ones most recently added to Popper first |
| **Most played** | Tables played at least once, by number of games |
| **Favourites** | The favourites marked in Popper (if empty: "No favourites yet: mark them in Popper.") |
| **All** | Every visible table, alphabetically |

At the top right, the count (for example "Visual Pinball X 28 / 72") and three buttons:

- **A-Z** — opens **Go to** (below);
- **shuffle** (crossed arrows icon) — **Surprise me**: a random table;
- **gear** — the DMD settings panel (chapter 7).

### The wheel bar

- **Drag** the bar: the wheel in the middle, larger and lit, is the selected table. The bar goes round.
- **Tap** a wheel: it comes to the middle.
- **Fling** it with a quick swipe: the bar keeps going, the faster the further (at most a third of the list);
  a slow drag moves by what you covered.
- **Swipe** left or right on the middle area: one table forward or back.
- **With a keyboard or remote**: left/right arrows move the bar, Enter activates the focused button
  (**Play** first), Esc / Back closes any open overlay first (Go to, instructions, confirmations).

A table with no wheel shows its name instead of the picture.

### Go to (A-Z)

- At the top, the **categories** of the open list, in the board's language (Visual Pinball X, Future Pinball,
  Pinball FX, Shoot'em-ups, Mazes, Platformers, Sports and racing, Other arcade, PC games), each with how many
  tables of **this** list it holds; the first one, **All games**, removes the filter. A category filters the
  list.
- Below, the **letters**: a letter jumps to the first table starting with it. Letters no table starts with are
  greyed out; digits and symbols are **0-9**.
- To close: the **✕**, a tap outside the box, a swipe down, or Back.

### Surprise me

The shuffle button shows a random table. Its card then gets an **Another one** button to draw another one
straight away.

### The table card

The selected table fills the screen: its backglass behind (darkened), its large wheel, name, maker and year,
games and time played ("10 games, 7 h 16 min played"), the last game (dates as dd/mm/yyyy), **Favourite** if it
is one, the top high scores. The buttons:

| Button | What it does |
|---|---|
| **Play** | Starts the table on the cabinet (through Popper's Web Remote) |
| **Instructions** | Only if the table has an instruction card in Popper: shows it upright, full width; scroll with a finger, tap (or Back) to close. Until the page arrives it shows "Loading..."; it also closes with the **✕**. If the page gets lost the board asks twice more, then closes it with "The instructions did not arrive from the PC: try again."; if the PC has no instructions for that table: "No instructions for this table." |
| **Composition** | How the table uses the PC's monitor and this screen (chapter 8), without playing it |
| **Another one** | After Surprise me: another random table |

After **Play** you see "Starting..." with the table's name; **Back** only hides this page. If Popper has not
started anything after about 2 minutes (the board waits that long: the bridge retries while Popper refuses launches, for example right after a crash): "The table did not start: check Popper's Web Remote."

### Showcase: when you use the cabinet's buttons in Popper

When someone uses the cabinet's joystick/buttons in Popper, the board follows its selection and shows that
table's **showcase**: backglass, DMD and topper one after the other, every 7 seconds, each framed in a glow of
its own colours, with "Touch to choose a table". A touch opens the menu on that table.

- Flyers and instructions are not in the showcase (in Popper they are portrait pages saved on their side):
  open the instructions with the **Instructions** button.
- When Popper selects an entry that is not a table ("Go Back", "Exit", a playlist), the board shows its action
  in large letters (for example "BACK", "EXIT", or the playlist's name) with "On Popper: …" below.
- When Popper goes back to its attract mode, the board's menu returns.
- If a table is started from Popper (not from the board), the board still switches to the DMD.

### Attract mode

After 2 minutes without a touch the bar turns by itself (one table every 8 seconds) and "Touch to choose"
blinks. This never happens during a game; when a game ends the 2 minutes start again, so the table you just
played stays on screen with its updated scores.

---

## 6. During a game

### What the board shows

| Situation | On screen |
|---|---|
| VPX table with a DMD | The DMD in dots, in the chosen layout (chapter 7) |
| VPX table that has not sent its DMD yet | "Loading..." for up to a minute, then the game's page ("Playing for 12 min"): it is a table without a DMD, for example an alphanumeric one (see chapter 9) |
| Future Pinball table with a DMD | The DMD, mirrored by the bridge |
| Pinball FX, MAME, PC games | The running game's showcase |
| PC silent for 5 s (no Popper) | The dot clock, 12 or 24 hours as the system clock, if **Clock when the PC is silent** is on |

While a game's DMD is on screen, NucleoOS's system gestures (shade, Home, Recents) are locked: a stray swipe
cannot hide the score.

### Gestures and keys

- **Short tap** on the DMD (or on the showcase of a game without a DMD, or on the "Playing" page of a table that has not sent its DMD yet): the **quick actions**.
- **Hold for 2 seconds** (or Enter / Space on a keyboard, also on the clock and the waiting page): the
  **settings panel**. While the panel is open the system gestures work again.
- **Back** (or Esc) on the DMD opens and closes the panel: it never closes the app.

### Quick actions

| Item | What it does |
|---|---|
| **Back to the DMD** | Closes the quick actions |
| **Tables** | Browse the tables during the game; at the top, a **Back to the DMD** button |
| **Quit the game** | Closes the running table ("Closing the game...") and the menu returns |
| **DMD settings** | Opens the settings panel |
| **Composition of this table** | The running table's composition (chapter 8), opened on the **This screen** tab |

To close the quick actions without choosing: tap outside the box or swipe down.

For VPX tables, quitting goes over **Wi-Fi**, straight to Popper's Web Remote (at that moment the cable
belongs to dmdext): the board must be on the same network as the PC. If the PC does not answer: "The PC does
not answer over Wi-Fi: quit the game on the cabinet." For the other emulators quitting goes over the cable,
through the bridge. If the game does not close after the retries (also when switching tables): "The PC could not
close the game: quit it on the cabinet."

### Starting another table

From **Tables**, pick a table and tap **Play**. The board asks "*X* is running. Quit it and start *Y*?":
**Quit and start** closes the running table and starts the new one as soon as the bridge reports the end;
**Cancel** leaves everything as it is.

### When the cable belongs to the DMD

During a game whose DMD goes through dmdext (VPX tables) the cable is busy with the DMD: **Surprise me**, the cards of other tables, **Instructions** and the backglass version choice say "Available when the game ends: the cable belongs to the DMD during the game."

### A table that closes by itself

If the table crashes or is killed (without Popper's end hook), the board says so — "*Name* closed by itself: you can start it again from here." — and keeps it selected, ready for **Play**.

---

## 7. The settings panel

Open it by holding the screen for 2 seconds (or with Enter / Space / Esc), from the menu's gear, or from
**DMD settings** in the quick actions. At the top:

- **the status line**: USB / Wi-Fi, the format in plain words ("16 shades", "16 colours", "RGB", "clock",
  "Test screen", "score displays"), size, frames per second, game;
- **under it, the Wi-Fi address** to put in `DmdDevice.ini` (`ws://…:9090/dmd`; "no address yet" if the board
  is not on a network); once the Popper bridge has said hello to the board, the same line also shows "Bridge <version> · Popper: ready" (or "database not found");
- three buttons: **Back to the DMD** (it reads **Back** when no PC is driving the DMD) closes the panel;
  **Test screen** (below); **Close the DMD** closes the app, after the question described below.

Every change applies at once and is saved on the board.

### Closing the app

Every way out of DMD Pinball — **Close the DMD**, the Home / Recents gestures, the system keys, the window's close button in the classic desktop mode — first asks
**"Close the DMD?"**: during a game "A table is running: the cabinet will have no DMD until the app is opened
again.", otherwise "The cabinet will have no DMD and no table menu until the app is opened again." **Stay**
(the button already selected) cancels; **Close** leaves. Back never closes the app.

### Screen

| Setting | What it does | Default |
|---|---|---|
| **Layout for every table** | How the DMD uses the screen (table below). The card shows the layout's name and description; if the running table has its own, it says so ("This table has its own: …"). Tap the card or **Choose with previews** to choose (chapter 8). | Classic DMD |
| **Rotation** | 0°, 90°, 180°, 270° clockwise, to match how the board is mounted | 0° |

The layouts, in the order the previews show them:

| Layout | What it does |
|---|---|
| **Automatic** | Each table its best layout, from the pictures it has: Art with window with its FullDMD art, Backglass with housing with its grill, else Classic DMD. A 128x16 DMD gets Full-screen DMD (rows rebuilt). |
| **Full-screen DMD** | The whole screen with no gaps: the rows are rebuilt (128x64) and the dots stay round and large. |
| **Classic DMD** | Square dots and the true proportions, as large as they fit; bands above and below. |
| **Tall DMD** | The DMD across the whole width and a little taller; bands above and below. |
| **Stretched DMD** | The whole screen with the original dots: the picture is stretched, the dots stay small and the rows far apart. |
| **Backglass + DMD** | The table's backglass on top and the DMD below it, as in a real backbox. Without a backglass the DMD is centred. |
| **Backglass only** | Only the table's backglass, no DMD: the board is the backglass screen. |
| **Table art** | The table's FullDMD art on top and the DMD in a wide band below it. |
| **Art with window** | The table's FullDMD art over the whole screen and the DMD inside its window. |
| **Backglass with housing** | The backglass with its grill over the whole screen and the DMD in its housing. |

- The layouts with a backglass or art use the pictures the bridge sends as the game starts: without the
  bridge, or for a table that does not have them, they are not available.
- **Rotated screen** (90°, 180°, 270°): the layouts with pictures show the classic DMD; at 90° and 270° also
  Tall DMD and Full-screen DMD. The panel and the Composition screen say so: "Screen rotated: layouts with
  pictures show the classic DMD."
- The clock always fills the screen with round dots, whatever the layout.

### Table menu

| Setting | Values | Default |
|---|---|---|
| **Menu style** | **Cabinet** / **DMD** (chapter 5) | Cabinet |

### Dots

| Setting | What it does | Default |
|---|---|---|
| **Colour** | The colour of monochrome DMDs, chosen from a row of coloured circles; the caption names the chosen one ("Colour · Orange"). The empty ring is **From the PC** (the colour the PC sends); then Orange, Red, Yellow, Green, Cyan, White. This works over USB too: monochrome frames are recoloured, while coloured DMDs (ColorDMD, Serum, palettes) keep their own colours; **From the PC** keeps dmdext's colour. | Orange |
| **Brightness** | From 10 to 100%, with a finger or the keys; saved either way | 100% |
| **Dot shape** | **Round** or **Square** | Round |
| **Glow** | A soft halo of light in the gaps between the dots | Off |

### System

| Setting | What it does | Default |
|---|---|---|
| **Clock when the PC is silent** | If the PC sends nothing for 5 seconds (after it has shown something), the board shows the time in dots (12 or 24 hours, as the system clock) with the date below. It never appears over the Popper menu or during a game. | On |
| **Start at power-on** | NucleoOS opens DMD Pinball at power-on | Off |
| **Network port** | The Wi-Fi port. It applies when you confirm or close the panel, and is saved only once the board really listens on it. Errors appear under the field ("Port: a number from 1 to 65535.", "Port … cannot be opened."): the port in use then stays. | 9090 |

### Test screen

For 10 seconds the board shows "TEST" with a countdown of the seconds, the chosen colour in 16 steps, colour
bars (red, green, blue, yellow, cyan, magenta, white, grey), a border and a solid block in the top-left corner.
Frames from the PC are ignored meanwhile. A tap, Enter, Space or Esc ends it early. It checks the cabinet's
orientation, colours and brightness **without a PC**: if the block is not in the top-left corner, change
**Rotation**.

---

## 8. The Composition screen

Here you choose, with previews made from the table's real pictures, how a table uses the cabinet's screens.
It opens in three ways:

| From | What it sets |
|---|---|
| Panel → **Choose with previews** | The layout of **this screen** for **Every table**: the ten layouts in a grid, all in view. The previews use the pictures of the running table, or of the one selected in the menu. |
| Table card → **Composition** | The PC's monitor and this screen for **that** table, without playing it |
| Quick actions → **Composition of this table** | The same, for the running table; it opens on the **This screen** tab |

The PC's monitor is chosen table by table: from the panel you only choose this screen. Close with the **✕** at
the top right or with Back. At the top, the line "Pictures of this table: …" says what the table has
(backglass and its size, with grill, FullDMD art, dual backglass); while they arrive: "The table's pictures
are coming from the PC...".

### "This screen" tab

- **Real dots, 1:1**: a lens showing the middle of the DMD drawn at the screen's real size, so you can judge
  the dot size and the gaps between rows before leaving (a thumbnail cannot show them).
- The chosen layout's name and what it does. For **Automatic** on a table that is not running: "decided when
  the table runs".
- **Dot size**: **Fitted** (to the layout) or **Original** (square, whole pixels, never stretched).
- **Margin**: 0, 1, 2 or 3 rows and columns of unlit dots around the DMD.
- Only during a game with a 128x16 DMD: **128x16 DMD: rebuilt** (brought to full height, the default) or
  **128x16 DMD: as it is**.
- The layouts as cards: for a table the first is **As every table** (follows the panel's choice), then only
  the ones possible with the pictures the table has.

### "Second screen (PC)" tab

Decides what the **PC's backglass monitor** shows for this table. The bridge writes the choice where B2S and
dmdext read it when the table starts (the table's `.res` file, `B2STableSettings.xml`, the game's section of
`DmdDevice.ini`), after making **a backup** of each file with the suffix `.prima-di-s2`.

| Choice | On the PC's monitor |
|---|---|
| **As it came** | As the table's backglass file (B2S) places it, nothing changed |
| **Backglass** | Only the backglass, over the whole monitor |
| **Backglass + DMD** | The backglass above and the DMD below it |
| **Backglass with grill** | The whole backglass with its speaker grill |
| **FullDMD art** | The table's FullDMD art (the B2S file's second, smaller picture) |
| **Backglass + art** | The backglass above and the FullDMD art below it |

Only the choices possible for that table appear. Also:

- **DMD on the monitor too**: **No** (the DMD stays on this board only) or **Yes** (dmdext's virtual DMD on
  the monitor too). With **As it came** these buttons are disabled: "Choose one of the compositions below
  first."
- **Backglass version**: **Authentic** or **Fantasy**, for dual backglasses: B2S lights the other set of
  pictures from the next game.
- Changes apply **from the next game** ("Second screen ready for the next game"). During a game,
  **Restart the table now** appears, to close it and open it again straight away with the new composition.
- "No second screen on the PC": the bridge found no second monitor. "This table has no backglass file (B2S)":
  the monitor shows Popper's backglass and there is nothing to choose. "The PC did not answer: the backglass file could not be described.": the PC did not describe the backglass file after 3 tries; open the Composition again.

---

## 9. Troubleshooting

| Problem | What to do |
|---|---|
| **No DMD on the board** | Check that the app is open **before** the table starts; that `DmdDevice.ini` has `[zedmd] enabled = true` (USB) or `[networkstream]` with the right URL (Wi-Fi); open the panel and look at the status line (USB / Wi-Fi, frames per second). |
| **dmdext says "ZeDMD is not available"** | The app was not open when the table started, or another program holds the COM port. Turn on **Start at power-on**, close programs using the port and, if needed, pin the port with `port = COMx` under `[zedmd]`. |
| **The board restarts when a table starts** | When it opens the port, dmdext sends the board a reset signal: the app blocks it **only while it is open**. Keep it open (Start at power-on). If it still restarts, remove the hub: an unpowered hub leaves the board short of power. |
| **The table menu does not appear** | Open the panel: if the address line has no "Bridge …" part, the bridge is not talking to the board. The bridge must be on: `ponte.ps1 stato` ("Ponte ACCESO" and "Scheda: COMx"). If the board shows as "non collegata" (not connected), check the cable. Run `ponte.ps1 controllo` and read the report. During a game there is no menu: it appears when the game ends. While the bridge is on the cable the board asks it for its hello every 3 seconds, so reopening the app brings the menu back by itself. |
| **"Popper: database not found"** in the panel | The bridge cannot find Popper's database: check Popper's folder with `ponte.ps1 controllo` (if it is in a non-standard folder, start the bridge with `--popper`). |
| **"No tables in this list."** | Normal for **Recent** or **Most played** if you have not played yet; try **All**. |
| **"The PC could not send the table list: check Popper."** | `ponte.ps1 controllo`: were Popper's folder and database found? |
| **Pictures missing (wheel, backglass)** | The first time, pictures come from the PC one at a time; with a microSD, while the menu is left alone, the board fetches them by itself and then it is instant. The file in Popper must be named like the game (or "Name(1).png", "Name (2).png"). A picture added in Popper shows up within 10 minutes, or when the app is reopened. |
| **"The cable to the PC is busy: try again in a moment."** | The cable was carrying a large picture: try again. |
| **"The instructions did not arrive from the PC: try again."** | The page was lost on the cable even after two more requests: try again in a moment. |
| **"Available when the game ends: the cable belongs to the DMD during the game."** | Normal during a VPX table: the feature is back when the game ends. |
| **"… closed by itself: you can start it again from here."** | The table crashed or was killed: tap **Play** to start it again. |
| **"The PC did not answer: the backglass file could not be described."** | The bridge did not answer for the Composition: check that it is on (`ponte.ps1 stato`) and open the Composition again. |
| **"No instructions for this table."** | The PC has no instruction card to send for that table: add one in Popper (GameHelp screen). |
| **The table does not start** ("The table did not start: check Popper's Web Remote." / "The PC could not start the table: check Popper.") | Popper must be running with its Web Remote on (`useWEB = true ;`, Popper restarted, COM component registered, port 80). Try starting the same table from Popper: if it does not start there either, the problem is the table or Popper. |
| **I cannot quit the game from the board** ("The PC does not answer over Wi-Fi: quit the game on the cabinet." / "The PC could not close the game: quit it on the cabinet.") | For VPX tables quitting goes over Wi-Fi: the board must be on the cabinet's network and reach Popper's Web Remote (TCP 80) on the PC. Usually it is reachable as installed; if not, in Windows Defender Firewall allow inbound TCP 80 for PinUP Popper on the private network. Meanwhile, quit with the cabinet's buttons. |
| **Which URL do I use for Wi-Fi?** | The one on the app's waiting page or at the top of the panel: `ws://<board IP>:9090/dmd`. If the IP changes at every power-on, reserve a fixed IP in the router. "Wi-Fi is not connected": join the board to the cabinet's network, or use USB. |
| **"Port: a number from 1 to 65535." / "Port … cannot be opened."** | The port typed is not valid, or the board cannot open it: choose another one in **Network port**. Until it works, the previous port stays in use. |
| **Alphanumeric or segment tables (1980s) with no DMD on the board** | dmdext converts segment displays by itself, but only if VPinMAME hands it the display: for that ROM the registry key `HKCU\Software\Freeware\Visual PinMame\<rom>\showpindmd` must be 1. Back up the registry before changing it. |
| **"This DMD (…) does not fit the screen."** | Over Wi-Fi the DMD is larger than 512x256: use a smaller size in dmdext, or USB. |
| **Picture upside down or odd colours** | **Test screen**: the solid block must be top-left (otherwise **Rotation**); for monochrome DMDs choose the **Colour**. |
| **Small dots, or "too much black" between rows** | Use **Full-screen DMD** or **Classic DMD** instead of **Stretched DMD**; check with the **Real dots, 1:1** lens. |
| **The backglass does not appear although I chose it** | With the screen rotated, the layouts with pictures show the classic DMD: set **Rotation** back to 0°. |
| **The panel does not open during a game** | Keep your finger still on the screen for a full 2 seconds (a short tap opens the quick actions), or press Back. |
| **After a VPX crash Popper starts nothing** | The bridge closes the leftover B2S server by itself ("closed b2sbackglassserverexe" in the log). If it does not, check that the bridge is on. |

When reporting a problem, attach the output of `ponte.ps1 stato` and the last lines of
`%LOCALAPPDATA%\nucleo_bridge\ponte.log`.

---

## 10. Updating the firmware safely

DMD Pinball's settings live in the board's internal memory and survive updates.

- **Over Wi-Fi** (the normal way): **Settings → System update**. Nothing special is needed for the DMD.
- **Over the USB cable with esptool**: while DMD Pinball is open, the board **ignores resets from the USB port**
  (that is what protects it from dmdext), so esptool could not restart it. Before flashing:
  1. close the app: go Home and answer **Close** to "Close the DMD?", or call `GET /api/ui/home`, which closes
     it without asking;
  2. on the PC, stop the bridge, which holds the COM port: `ponte.ps1 ferma`;
  3. do not start tables while flashing;
  4. when done: `ponte.ps1 avvia` and reopen DMD Pinball (or let **Start at power-on** do it).

---

## 11. Known limits

- Over USB the board is a 128x32 DMD: dmdext scales other sizes. Over Wi-Fi they are shown as sent, up to
  512x256.
- Over Wi-Fi, one PC at a time.
- With **Full-screen DMD** the DMD updates at about 30 frames per second; with the square-dot layouts about
  52-60.
- Where the DMD comes from: VPX tables send it through dmdext over USB; Future Pinball tables that have a DMD
  are mirrored by the bridge with dmdext's mirror mode; Pinball FX, MAME and PC games show the game's showcase
  pictures.
- On the cable, large messages (pictures) sometimes arrive damaged: the bridge sends them again until the board
  confirms them; rarely a picture may be missing until the next pass.
- Without a microSD, pictures come over the cable every time.
- Each list holds up to 2000 tables.
- With the screen rotated, the layouts with pictures show the classic DMD.
- Flyers and instructions are not in the showcase; tables hidden in Popper do not appear in the menu.
- Changes to the PC's monitor apply from the next game (or with **Restart the table now**).
- Popper's Web Remote works only while Popper is running.

---

## 12. Privacy and network

- The board talks **only to the cabinet PC, on the local network**: it receives the DMD (USB, or Wi-Fi on port
  9090) and, to quit or start a table during a VPX game, it calls Popper's Web Remote on the PC (port 80). The
  app sends nothing to the Internet.
- **The Wi-Fi DMD server (port 9090) has no authentication** and **Popper's Web Remote has no password**: use
  them on a trusted home network only, never on a public network and never exposed to the Internet.
- The bridge listens only on the PC itself (`127.0.0.1:58090`), reads Popper's database read-only and keeps its
  converted pictures in `%LOCALAPPDATA%\nucleo_bridge\cache`. It writes B2S and dmdext files only when you
  choose a monitor composition (chapter 8), with a backup.
- The board keeps the pictures it has received and the last lists on the microSD, in `/sdcard/data/dmd`.
