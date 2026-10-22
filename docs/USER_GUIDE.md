# NucleoOS P4 user guide

This guide is for people who have just put NucleoOS P4 on a Guition JC1060P470C (the ESP32-P4 7"
touchscreen) and want to use all of it. It tells you where to tap and which keys to press. How the
system is built is covered in other documents, linked where they help.

Some features ship before they have been proven on real hardware. They are marked
**(experimental)** here. The full list is in [STATUS.md](STATUS.md).

## Contents

1. [First start](#1-first-start)
2. [Getting around](#2-getting-around)
3. [The classic desktop](#3-the-classic-desktop)
4. [App Store](#4-app-store)
5. [Built-in apps](#5-built-in-apps)
6. [Reading: Books and Library](#6-reading-books-and-library)
7. [Writing: WordGrinder](#7-writing-wordgrinder)
8. [Games and emulators](#8-games-and-emulators)
9. [ANIMA, the assistant](#9-anima-the-assistant)
10. [DMD Pinball](#10-dmd-pinball)
11. [Second Screen](#11-second-screen)
12. [Smart home](#12-smart-home)
13. [The web desktop](#13-the-web-desktop)
14. [Updates and safety](#14-updates-and-safety)
15. [Troubleshooting](#15-troubleshooting)

---

## 1. First start

You need three things: the firmware on the board, a microSD card, and a short setup wizard. After
that the board updates itself and installs apps over Wi-Fi.

### Flashing

1. Open the web flasher in Chrome or Edge: <https://indecenti.github.io/nucleoos-p4-store/flash/>
2. Connect the board to the computer with a USB-C **data** cable. Use the board's programming port.
   The other USB-C port is the high-speed one, used for accessories and Second Screen.
3. Press **Connect & install** and wait for the flasher to finish.

If you prefer the command line, every [release](https://github.com/indecenti/NucleoOS-P4/releases/latest)
has a single factory image you can write at offset `0x0` with `esptool.py`.

The board needs an ESP32-P4 chip revision v0.x or v1.x, which covers every board sold so far. On a v3
chip the bootloader will not start, and you can flash the stock firmware back.

### The microSD card

Format the card as **FAT32** and insert it. Apps, photos, music, offline content, settings backups and
firmware updates all live on the card. Offline content needs at least 1 GB free, and updates need
about 20 MB. Without a card the system still runs, but the Store, updates and most apps will ask you
to insert one.

### The setup wizard

The wizard opens by itself the first time. It shows **Step N of M**, and **Back** (a swipe from the
left edge) goes to the previous step. The steps are:

1. **Welcome to NucleoOS**: pick your language (English, Italiano, Español, Français, Deutsch).
2. **Connect to Wi-Fi**: tap your network, type the password (tick **Show password** to check it),
   then tap **Connect**. You can **Skip** and do it later in **Settings > Network**. The board uses
   2.4 GHz Wi-Fi.
3. **Offline content**: one tap on **Download** gets what the board needs on the card: ANIMA's
   offline answers, dictionaries and the web companion (about 140 MB). It goes on in the background,
   so you can continue right away. **Customize** lets you pick packs one by one.
4. **Date, time and region**: the clock sets itself from the Internet. Pick your **Time zone**, the
   **24-hour format** if you want it, and your **Region**.
5. **Protect your device**: an optional 4-digit PIN that locks the screen when it goes to sleep.
6. **Help improve NucleoOS**: anonymous statistics, off unless you tap **Share statistics**. The
   screen lists what is sent and what is never sent (no device ID, IP address, files or location).
   You can change it later in **Settings > Security**.
7. **You're all set**: a reminder of the three edge gestures (see the next section).

Missed something? **Settings > About > Run the setup wizard again**. Skipped the content download?
**Settings > System content**.

### Language later

**Settings > Language & Region** changes the language at once, without a restart. The same page has
**Physical keyboard**, the layout of a USB or Bluetooth keyboard: **Like the language** (default),
**Italiano** or **English (US)**.

---

## 2. Getting around

The home screen is a tablet-style launcher. This section covers it, the system gestures, the
notification shade, and how to work with a keyboard, a mouse or the on-screen keyboard.

### The launcher

- **Pages**: icons sit in a grid (6 x 3 in landscape, 3 x 6 in portrait). Swipe left or right to change
  page. The dots show where you are.
- **Open an app**: tap its icon.
- **Rearrange**: press and hold an icon, then drag it. Hold it against the screen's left or right edge
  to move it to another page.
- **Folders**: drag an icon onto the centre of another icon to make a folder, or onto a folder to add it.
  Tap a folder to open it. Its name field at the top renames it. Inside a folder, press and hold an
  icon to move it back to the grid.
- **Dock**: the bar at the bottom fills itself with the apps you use most. There is nothing to pin.
- **Search**: swipe down anywhere on the launcher (not from the top edge) to search your apps. A swipe
  from the left edge closes the search.
- **Wallpaper**: in **Files** or **Gallery**, open a JPEG photo and choose **Set as wallpaper**.
  **Settings > Display > Wallpaper > Remove wallpaper** goes back to the theme gradient.

### Edge gestures

Apps have no back button. You move around with three swipes that start on the edge of the screen:

| Gesture | What it does |
|---|---|
| Swipe right from the left edge | Back (inside the app first, then out of it) |
| Swipe up from the bottom edge | Home. At Home, the same swipe opens **Recent apps** |
| Swipe down from the top edge | Quick settings and notifications |

A swipe that starts in the middle of the screen belongs to the app. Some full-screen games and
DMD Pinball turn the system gestures off while they play. Their own menus show how to leave.

### Quick settings and notifications

Swipe down from the top edge.

- **Top row**: the clock and date, then three buttons: **Screenshot**, **Lock now** and
  **Settings**.
- **Round toggles**: **Wi-Fi**, **Dark** theme, **Do Not Disturb**, **Mute** and **Rotate**. A tap
  switches one. Press and hold one to see its name.
- **Sliders**: brightness and volume.
- **Notifications**: swipe a card sideways to dismiss it. The trash button is **Clear all**.

To close the shade, swipe it up, tap the grip at the bottom, or tap the dim band below it.

### Portrait mode

Tap **Rotate** in the quick settings to switch between landscape (1024 x 600) and portrait
(600 x 1024). The whole interface rebuilds for the new orientation. Some apps, such as Books, have
their own portrait button.

### Screenshots

Tap **Screenshot** in the quick settings, or press PrtSc or Win+Shift+S on a keyboard. The screen
freezes. Drag a rectangle, or just tap to take the whole screen. A toolbar offers a pen, an arrow, a
rectangle, a highlighter, a colour and undo, then the actions: **Copy** (to the clipboard), save, send
to ANIMA, and close. Saved shots go to `~/shots` (`/sdcard/home/shots`). Alt+PrtSc copies the whole
screen to the clipboard.

### Keyboard and mouse

USB keyboards and mice work in every app, in both the launcher and the classic desktop. Bluetooth Low
Energy keyboards and mice can be paired in **Settings > Bluetooth** **(experimental)**. Classic
Bluetooth is not supported.

When a physical keyboard is connected the on-screen keyboard stays down. Press **F1** (or Ctrl+/) at any
time for the shortcut sheet: the system shortcuts plus those of the app in front.

| Keys | What they do |
|---|---|
| Win | Start (type to search) |
| Win+E / Win+I | Files / Settings |
| Win+A | Quick settings and notifications |
| Win+V | Clipboard history |
| Win+L | Lock |
| Win+D | Show the desktop |
| Win+1..9 | Switch to the n-th taskbar app |
| Alt+Tab / Win+Tab | Recent apps |
| Ctrl+W / Alt+F4 | Close the app |
| Ctrl+Alt+Del | System Monitor |
| Tab / Shift+Tab | Next / previous control |
| Arrows, Enter, Space | Move and activate |
| Menu / Shift+F10 | Context menu |
| Esc | Back / close the panel |
| Ctrl+C / X / V, Ctrl+A, Ctrl+Z / Ctrl+Y | Copy / cut / paste, select all, undo / redo |
| Ctrl+Backspace, Ctrl+arrows | Delete a word, jump by word |
| PrtSc / Win+Shift+S | Screenshot of an area |
| Media keys | Volume, mute, play/pause |

Esc goes back inside an app and never closes it by surprise. To leave, use the edge gestures, the
title bar's close button in the classic desktop, or Alt+F4.

**Settings > Mouse** has **Pointer speed**, **Wheel scroll (lines per notch)**, **Reverse wheel
direction** and **Left-handed (swap buttons)**. A right click opens context menus. On a touch screen,
a long press does the same.

A Cardputer can also act as a Wi-Fi keyboard (KeyDeck). It is off by default because it accepts keys
from the network without a password: **Settings > Security > Remote keyboard**.

### The on-screen keyboard

It slides up when you tap a text field and goes down when you tap elsewhere or press its check key.

- Number and PIN fields get a numeric keypad. Other fields get letters in your language's layout
  (QWERTY, QWERTZ or AZERTY), with a row of accented letters.
- The first letter of a sentence field is capitalised and the keyboard goes back to lowercase after it.
- Hold Backspace or the cursor keys to repeat them.
- The return key does what the field needs: next field, search, connect or send.
- Right-click a text field (or press the Menu key) for **Cut**, **Copy**, **Paste** and **Select all**.
- **Settings > Sound > Keyboard clicks** turns the key sound on or off.

---

## 3. The classic desktop

The classic desktop **(experimental)** looks like a Windows 95 desktop: icons, a taskbar, a Start menu
and windows with a title bar. Apps look and work the same in both interfaces. Only the shell around
them changes.

### Turning it on

**Settings > Display > Interface**:

- **Classic desktop**: on, it uses the classic desktop always.
- **Automatic with mouse and keyboard**: on, it switches to the classic desktop when both a mouse
  and a keyboard are connected, and back to the launcher when they are not.

The line below shows whether a mouse and a keyboard are detected. **Desktop colours** (**NucleoOS**,
**Cyberdeck**, **Amber**, **Teal**) change the classic desktop's palette. The classic desktop is
always in landscape: rotation comes back when you return to the tablet launcher. The edge gestures are
off in this mode. The taskbar and the title bars replace them.

### Desktop

- With a mouse: click an icon to select it (Ctrl, Shift or a drag for more), and double-click to open
  it. With a finger or the keyboard, one tap or Enter opens it.
- Right-click (or long press) an empty spot for **Arrange icons by name**, **Default icons** and
  **Display settings**.
- Right-click an icon for **Open**, **Pin to Start** / **Unpin from Start** and **Add to desktop** /
  **Remove from desktop**.

### Taskbar

From left to right:

- **Start** opens the Start menu.
- The apps you used recently, as buttons. The open one looks pressed. Click it to minimize or restore
  it. Right-click a button to close that app.
- The tray. The **^** arrow shows hidden icons: **Screenshot** and **Clipboard**. Hold an icon to pin
  it to the taskbar, and hold a pinned one to put it back. Next come the notifications bell, USB
  drives, SD card, Wi-Fi (with **Network settings**), volume, and the clock with the date.
- The thin button in the far corner is **Show desktop**.

### Start menu

- **Pinned** apps, with **All apps** to see everything.
- **Games**, with **Show all**, and **Recommended**.
- Start typing to search **apps and files** (the first search indexes the files, which takes a moment).
- The footer: **Touch interface** (back to the tablet launcher), **Lock now**, **Turn off screen**,
  **Restart device**.

Win opens and closes Start. Esc, Win or a click outside closes it.

### Windows

Apps open maximized, with a title bar showing **Back** (when the app has a page to go back to),
**Minimize** and **Close**. Only one app runs at a time, by design, so the board's memory goes to the
app in front. Full-screen apps (games, video, camera) cover the taskbar. Alt+F4 or Win gets you out.

---

## 4. App Store

The Store installs apps, games, console cartridges, terminal programs and offline knowledge packs over
Wi-Fi. Everything in it is signed: the board refuses packages that are unsigned or have been altered.

### Browsing

Open **Apps** from Home and pick the **Store** tab (the other tab, **Installed**, lists what you
have). Along the top runs a row of chips:

- **Discover**: **Featured**, **Most downloaded**, **New releases** and **Recently updated**, each with
  **See all**.
- **Categories**: browse by category.
- **Consoles**: the retro platforms (such as WASM-4, Game Boy, Arduboy, CHIP-8), with how many games
  each one has.
- **Apps**, **Featured**, **Most downloaded**, **New releases**, then one chip per category.

The search field (**Search games and apps**) finds apps by name. You need Wi-Fi and an SD card. The
error messages **No Wi-Fi connection** and **Insert an SD card to install apps.** say which is
missing.

### Installing

Tap an app to see its page: author, **Version**, **Size**, **Category**, **License**, **Web page**,
**What's new** and **Permissions**. Many apps also have a **Guide** with a QR code: scan it with your
phone to read how the app works.

Tap **Install**. If the app asks for sensitive permissions, a sheet titled **This app can:** lists them
(use the Internet, talk to devices on your home network, use MQTT or Home Assistant, read and write
files, use the camera or the microphone). Read it, then tap **Accept and install**. When an update asks
for new permissions they are highlighted.

If an app needs another package, the page lists it under **Requires**, and it is **installed with it**.
When the app is installed its icon appears on Home straight away. Terminal programs have no icon of
their own and run inside the Terminal.

You can withdraw a permission later in **Settings > Security > App permissions**.

### Updates and removal

- When an update is out, the app page shows **Update**. The board also checks the Store in the
  background and notifies you. You can turn this off in **Settings > Notifications > Notify app
  updates from the store**.
- **Uninstall** is on the app page. Tap it twice: the second tap removes the app with its saves.
- A component that other apps use says **Used by ... - uninstall that first**.

### System apps

Some packages are part of the system and install themselves: the terminal programs Lua, JavaScript,
Python, SQLite, qrencode, units, eigenmath, dateutils, html2text, lowdown, zstd and pdfio, plus the zim
reader, the spreadsheet engine sc-im and the word processor WordGrinder. They are marked **System app -
installed by NucleoOS** and cannot be uninstalled.

The Store's **Region** is in **Settings > System update**. From the Terminal, `store search WORDS`,
`store info ID`, `store install ID` and `store remove ID` do the same as the Store.

---

## 5. Built-in apps

These apps come with the firmware. Each has a short guide here. Press F1 inside an app to see its
keyboard shortcuts.

### Files

The file manager for the SD card and USB drives.

- Tap a folder to open it, and tap a file to open it in its default app. If several apps can open it,
  the **Open with** sheet appears, with **Remember my choice**.
- Press and hold a file for **Details**: type, size, modified date, location, **Open with**, the file's
  own actions (for example **Set as wallpaper**), **Copy**, **Move**, **Rename** and **Delete**.
  **Delete** asks for a second tap.
- After **Copy** or **Move**, go to the target folder and tap **Paste ... here**.
- Press and hold an empty part of a folder for **Paste**, **Select all**, **Refresh** and **New
  spreadsheet**.
- With a mouse: click selects, double-click opens, right-click shows the menu. With a keyboard: Ctrl+C,
  Ctrl+X, Ctrl+V, Ctrl+A, Del, F2 (rename), F5 (refresh) and Backspace (up one folder).
- Files' **Preview** shows text and images read-only, so a config file is never edited by mistake.
- USB drives appear under **Places** while one is attached **(experimental)**. Use **Eject** before
  unplugging one (also in **Settings > Storage**).

Your personal folder is `/sdcard/home` (`~` in the Terminal). **Settings > Default apps** chooses which
app opens each file type ("Open with" and default apps are **(experimental)**).

### Gallery

Photos and videos on the SD card, newest first. It looks in the card's root, in `/sdcard/Photos` and in
`/sdcard/DCIM`.

- Tap a picture to view it. The controls sit in the side margins.
- Videos show their first frame with a play button that opens the Video player.
- **Delete** asks for a second tap (**Tap again to confirm**).
- JPEG photos can be **Set as wallpaper**.

JPEG, PNG and BMP are shown. JPEG photos over 512 KB may not open.

### Camera

Works with an OV02C10 camera module on the MIPI-CSI connector. Without one it shows **No camera
detected**.

- **PHOTO** / **VIDEO** switch the mode. Tap the shutter button to capture.
- Tap the picture to meter the light on that spot. Hold to go back to **Auto exposure**.
- The camera also offers exposure compensation, a rule-of-thirds grid and a self-timer.
- **Save to folder** chooses or creates the folder. By default files go to `/sdcard/DCIM`.
- The thumbnail of the last shot opens the Gallery.

Photos are JPEG. Video recording **(experimental)** makes MJPEG `.avi` files at about 5 frames per
second.

### Music

Plays MP3 and WAV files from `/sdcard/Music`. AAC and FLAC decoders are present but not verified.

- The left panel shows what is playing: seek bar, previous / play / next, shuffle, repeat and volume.
  Swipe left or right on it to change track.
- The right side is the library.
- Music stops when you leave the app.

### Video

A video player for **MJPEG `.avi`** (with PCM audio) and **MPEG-1 `.mpg`**. H.264 / MP4 is **not
supported**: convert those files to one of the two formats first.

- Browse folders from `/sdcard` and tap a video.
- Below the picture: a scrub bar with 10-second skips, previous / play / next / stop, volume, the gear
  (player settings) and full screen.
- The gear panel has repeat (off, all, one), aspect (fit, stretch, zoom), keep the screen on, and show
  the frame rate. Its labels are currently shown in Italian only.
- Keys: Space play/pause, left/right 5 s, Ctrl+left/right previous/next video, up/down volume, F full
  screen.

MPEG-1 plays in real time up to about 640 x 360.

### Recorder

A voice recorder **(experimental)** that uses the board's microphone.

- Tap the microphone button to record and tap again to stop.
- Recordings are listed on the right. Each row can be renamed or deleted.
- Files are WAV, saved in `/sdcard/Recordings`.

### Notes

A searchable list of notes. Each note is saved as you type, so there is no Save button.

- Tap **+** to create a note, and type a **Title** and the text.
- **Delete** asks for a second tap.
- If saving fails (no SD card), **Back** shows **Retry save** and **Discard & exit**.
- Notes are stored in `/sdcard/notes`. Text and Markdown files opened from Files are edited in place.

### Calculator

An immediate-execution calculator: each operation applies as you press it, with no operator
precedence. The **Scientific** pill adds memory, square root, x² and 1/x. **Standard** goes back to the
simple keypad.

### Tasks

A checklist. Type a **New task** and tap **Add**. Tap a task's circle to mark it done, and the trash
icon to delete it. The list is saved in `/sdcard/tasks.txt`.

### Sheets

A spreadsheet that opens `.xlsx`, `.xls`, `.ods`, `.csv`, `.tsv` and `.sc` files and saves `.xlsx`,
`.csv` and `.tsv`. `.xls` and `.ods` open read-only, and **Save** then makes an `.xlsx` copy. More in
[FOGLI.md](FOGLI.md).

- **Start page**: **Blank sheet**, three templates (**Monthly budget**, **Shopping list**, **Timesheet**),
  **Open a file…**, **Recent**, and **Recover unsaved work** when the app closed with unsaved changes.
- **Formulas** use Excel's notation in your language: `=SUM(B2:B8)` in English, `=SOMMA` in Italian.
  Any of the five languages is accepted. While you write a formula, tap a cell to insert its reference.
- **Touch**: tap selects, a second tap edits, drag scrolls, the corner handles stretch the selection,
  and a long press opens the menu. A mouse and keyboard work as on a PC (arrows, F2, Enter, Ctrl+C/V/Z/S
  and so on).
- **Toolbar**: open, save, undo/redo, cut/copy/paste, bold/italic/underline, **Number format**, Σ
  (sum), sort, **Find**, **Ask ANIMA** and **More** (insert or delete rows and columns, **Fit column
  width**, **Keyboard shortcuts**). The tabs at the bottom add, rename and delete sheets. The status bar
  shows the sum, average and count of the selection.
- **ANIMA help**: select cells and tap **Ask ANIMA**, then choose **Summarize**, **Check for errors**,
  **Write a formula** or **Explain the formula**, or type a question. Proposed changes are highlighted
  on the grid. Nothing is written until you tap **Apply N change(s)**, and Undo puts it back. Without a
  language model the device gives a **Column summary** it computes itself. The selected cells are sent
  to the model you chose in ANIMA's settings.
- In **Files**, a spreadsheet's details offer **Analyze with ANIMA**, and an empty part of a folder
  offers **New spreadsheet**.

The engine (sc-im) installs itself as a system app. If Sheets says it is being installed, try again in
a minute.

### Terminal

A Unix-like shell with GNU-style commands, and the place where terminal programs run. The full command
list is in [SHELL.md](SHELL.md). `help` lists the commands, and `help CMD` shows how to use one.

- Type in the bottom line and press Enter. Tab completes, and the arrow keys bring back earlier
  commands. Drag the screen to scroll back.
- On touch, the key row above the keyboard has Esc, Tab, Ctrl, ^C (stop the program), ^D (end of
  input), the arrows, and `/ - | ~`.
- `~` is `/sdcard/home`. Programs see that folder as `/`.

A few useful commands:

| Command | What it does |
|---|---|
| `sysinfo` | the board in one call: time, app, Wi-Fi, SD, RAM, volume |
| `ls`, `cd`, `cp`, `mv`, `rm`, `mkdir` | files and folders |
| `df -h`, `free -h`, `top` | disk space, memory, live tasks |
| `edit FILE`, `less FILE` | a text editor (^S saves, ^X exits), a pager |
| `open FILE` | open a file in its app |
| `wifi status`, `wifi scan`, `ip` | the network |
| `store search WORDS`, `store install ID` | the App Store |
| `apps` | installed apps and terminal programs |
| `update status` | firmware version, safety copy, channel |
| `screenshot` | save the screen to `~/shots` |
| `content` | the offline content on the SD card |

Programs from the Store run here by name: `lua`, `js` (JavaScript ES2024), `python` (MicroPython 1.26),
`sqlite3`, `zim`, `wordgrinder`, `zork`, `glulxe` and others. Tap a program's icon on Home to open the
Terminal with it already running. Each program's guide in the Store explains it.

The shell has no `if`/`for` loops, command substitution or script files. Use `xargs` instead of loops.

### System Monitor

A live view of the board in three tabs. **Performance** shows CPU per core, RAM, PSRAM and system
facts. **Processes** is the task list with CPU bars. **Services** shows the system services. It is
read-only: nothing can be killed from here. Ctrl+Alt+Del opens it.

### Diagnostics

Memory figures and the **Last crash** record (task and address). **Clear** removes the record. If you
report a problem, include what it shows.

### Settings

Settings has a list of categories on the left and the page on the right:

| Group | Pages |
|---|---|
| Connectivity | **Network** (Wi-Fi: scan, connect, forget, IP address), **Bluetooth** (BLE devices, **Controllers**), **Home** (Home Assistant and MQTT) |
| Device | **Display** (brightness, **Screen sleep**, **Interface**, **Desktop colours**, **Icons**, **Theme**, wallpaper, **Accent color**, **Font size**), **Sound** (volume, **Mute**, **Keyboard clicks**, **Startup chime**, **Play test sound**, microphone test), **Mouse**, **Notifications** (**Do Not Disturb**, Store update notices, **Clear all**), **Date & time**, **Storage** (microSD, internal flash, USB drives), **Sensors**, **Memory** |
| Personalization | **Anima AI**, **Language & Region**, **Accessibility** (font size), **Default apps** |
| System | **System update**, **System content**, **Backup & restore**, **Security**, **About** (version, **Restart device**, **Run the setup wizard again**) |

---

## 6. Reading: Books and Library

Two Store apps turn the board into an e-reader and an offline encyclopedia. Install them from the
Store: **Books** and **Library**, both in the Knowledge category.

### Books

An ebook reader for EPUB 2 and 3, FB2 (also zipped), CBZ comics, TXT, HTML and Markdown. Full guide:
[apps/libri/GUIDE.en.md](../apps/libri/GUIDE.en.md).

- **Library**: **Continue reading** at the top opens your last book where you left it. Below are all
  your books with cover and progress. Tap a book to open it. Hold it for details, including
  **Delete** (two taps). The lines button sorts by recent, title, author or progress.
- **Catalog**: thousands of free, public-domain or freely licensed books (Project Gutenberg, Standard
  Ebooks, Wikisource and others) in many languages, such as Italian, English, Spanish, French, German,
  Portuguese and Latin. Search by title or author, and filter by category and language. Tap a book,
  then **Download**. Downloads continue with the app closed, are verified, and the book then appears
  in your library.
- **Reading**: tap the right side to go forward and the left side to go back, or drag the page. Tap the
  middle for the controls: contents, search, appearance, bookmark, details, and a slider to move
  through the book.
- **Bookmarks, notes and search**: the contents panel has a second tab with your bookmarks. Tap a note
  number to read it in a popup. **Back to page** returns after a link. `/` or Ctrl+F searches the book.
- **Appearance** (the two A's): **White**, **Sepia**, **Dark**, **Black**, **E-ink** (16 greys and a
  full refresh flash every few pages) and **1-bit** (pure black and white). You can also set the font,
  size, spacing, margins, justification, hyphenation, two pages side by side and the page-turn
  animation. Pinch with two fingers to change the text size.
- **Portrait**: the phone button in the library and in the reading controls turns the app upright.
- **Comics** (CBZ): double-tap a page to zoom, and drag to move.
- **Keys**: arrows, Space or Page Down to turn pages, `t` contents, `b` bookmark, `a` appearance, `+` and
  `-` text size, Ctrl+O landscape/portrait, Esc to close a panel.

To add your own books, copy them into `/sdcard/home/books` (subfolders are fine), or open them from
**Files** with **Open with > Books**. DRM-protected books cannot be read.

### Library

Reads Kiwix ZIM files offline: Wikipedia, Wiktionary, Wikivoyage, a medical encyclopedia, iFixit
guides, DevDocs, Gutenberg and more, in every language. Full guide:
[apps/biblioteca/GUIDE.en.md](../apps/biblioteca/GUIDE.en.md).

- **Download collections** (the down arrow in the library) shows the Kiwix catalog in your language,
  with **Recommended**, **All**, categories and search. The **EN** button changes the catalog
  language. Tap a collection to choose an edition (**with pictures**, **without pictures**,
  **introductions only**), then tap **Download**. Downloads pause, resume and survive a restart. Every
  piece is checked.
- Tap a collection in the library to open its main page. The **i** button shows its information and
  **Delete from the card**.
- **Search** (magnifier) matches titles as you type. **Find inside titles** finds words anywhere in a
  title. Full-text search of articles is not available.
- While reading, tap links, use the back and forward arrows, **Sections** (the list button),
  **Bookmark** (star), and the menu (random article, theme, history, text size). Tap a picture or a
  formula to enlarge it.

You can also copy `.zim` files from a PC into `/sdcard/home/zim`. Files over 2 GB must be split into
parts (`name.zimaa`, `name.zimab`…) on FAT32. A good start is a `nopic` or `mini` edition. The same
collections work in the Terminal with `zim` (see `zim ls`, `zim search`, `zim get`), and ANIMA uses
them to answer.

---

## 7. Writing: WordGrinder

WordGrinder is a distraction-free word processor that runs in the Terminal. It is a system app, so it
is already installed. Full guide: [apps/wordgrinder/GUIDE.en.md](../apps/wordgrinder/GUIDE.en.md).

- **Open it**: tap its icon (an empty document), or in **Files** tap an `.odt`, `.md`, `.html` or
  `.txt` file and choose **Open with › WordGrinder**, or type `wordgrinder letter.odt` in the Terminal.
- **Formats**: it opens and exports OpenDocument `.odt` (LibreOffice and Word open it), Markdown, HTML
  and plain text, and also exports LaTeX, troff and Org. An imported file is saved as a `.wg` file
  next to it. To write `.odt` again, use **ESC › File › Export**. `.docx` and `.doc` do not open.
- **Touch**: tap the text to move the cursor, and drag up and down to scroll. **ESC** (on the key row)
  opens the menu: File, Edit, Style, Documents, Navigation.

| Keys | What they do |
|---|---|
| ESC | menu |
| Ctrl+S / Ctrl+O | save / open |
| Ctrl+B / Ctrl+U | bold / underline (italic is in ESC › Style) |
| Ctrl+N | normal text |
| Ctrl+P | paragraph style: headings, lists, quotes |
| Ctrl+Z / Ctrl+Y | undo / redo |
| Ctrl+X / C / V | cut / copy / paste |
| Ctrl+F / Ctrl+K | find and replace / find next |
| Ctrl+G | go to a page or heading |
| Ctrl+Q | quit |

It has no pictures, tables or page layout, and no spelling dictionaries. Your documents live in
`/sdcard/home`.

---

## 8. Games and emulators

Games come from the Store. Most open full-screen with on-screen controls, and work with a USB keyboard
too. Gamepads over USB or Bluetooth Low Energy are **(experimental)**: no controller has been paired on
the board yet. Pair one in **Settings > Bluetooth**. Under **Controllers** you can tap a pad to test it
and set **Vibration**. Classic-Bluetooth pads (PS4/PS5/Switch and many generic ones) do not work over
the air, so plug them in by USB.

In most games the system Back gesture opens the game's own pause menu, and a second Back leaves.

| Platform | How to play |
|---|---|
| **WASM-4** | About 150 fantasy-console games by the WASM-4 community, under **Store > Consoles**. On-screen gamepad or USB keyboard. |
| **Game Boy** (experimental) | The Game Boy app plays original Game Boy `.gb` files copied to `/sdcard/home/roms` (homebrew, or backups of your own cartridges). Game Boy Color-only games are not supported. Touch D-pad, **A**, **B**, **SELECT**, **START**. **MENU** pauses (**Resume**, **Reset**, **Zoom**, **Colours**, **Other game**). Battery saves appear as `.sav` next to the game. Homebrew packs are also in the Store. |
| **Arduboy** | Each game is its own app. Touch D-pad, **A** and **B**. **MENU** pauses (**Resume**, **Zoom**, **Colours**, **Sound**, **Exit**). High scores are saved. |
| **CHIP-8** | One app with 83 homebrew games built in (CHIP-8, SUPER-CHIP, XO-CHIP). The on-screen hex keypad lights the keys each game uses. |
| **Doom engine** (experimental) | Freedoom, DOOM Shareware and community megawads install as apps of their own. The Doom Engine app plays any WAD in `/sdcard/home/doom`, including your own `doom.wad` or `doom2.wad`. You need a keyboard, mouse or gamepad to really play: WASD/arrows, Ctrl to fire, E or Space to use, Esc for the menu (save, load, options). |
| **ScummVM** (experimental) | Eight free adventure games (Beneath a Steel Sky, Flight of the Amazon Queen, Lure of the Temptress, Drascula, DreamWeb, Soltys, Sfinx, Nippon Safes). Install one from the Store. The first start downloads its files. Tap to walk or use, and hold (or two-finger tap) to examine. The Back gesture opens the ScummVM menu (save, load). |
| **Interactive fiction** | Zork I, II and III (`zork`, `zork 2` in the Terminal) and many text adventures (Glulxe and Frotz), each with its own icon that opens the Terminal. Type short commands (`LOOK`, `N`, `TAKE LAMP`, `INVENTORY`), and use `SAVE` / `RESTORE`. `glulxe story.gblorb` plays your own Glulx stories. |
| **Puzzles** | Simon Tatham's collection of 40 logic puzzles in one app. A long press is a right click. |
| **Lua / LÖVE games** | Games such as RetroLove (Pong, Breakout, Asteroids), 2048 and Checkers run on the **Lua App** engine, which installs itself. Taps and swipes act as the mouse and arrow keys. To run your own scripts, put them in `/sdcard/home/lua` and open Lua App. |
| **Vertice 3D games** | 3D games such as **Vertice GP** (karts). On-screen steering and pedals, or keyboard and gamepad. The on-screen controls hide when a pad or keyboard is connected. |

If a game starts silent, another app (Music, for example) is holding the speaker. Close it and the
sound comes back.

---

## 9. ANIMA, the assistant

ANIMA answers questions and runs commands in five languages. It works offline, and it can use a
language model on your own network or in the cloud if you add one. Open the **Anima** app from Home.

### Talking to ANIMA

- Type at the `>` prompt and press Enter. `/help` lists the commands and keys.
- The microphone key (or `/voice`) asks by voice. Turning your voice into text needs a home Whisper
  server or a cloud key, set in the web Settings > AI. **Settings > Anima AI > Voice transcription**
  shows which is in use.
- Answers can be read aloud with the offline voices (Italian and English) if a voice pack is installed.
- Esc or Ctrl+C interrupts a turn. Ctrl+T starts a new conversation, and Ctrl+B (or `/sessions`) lists
  your conversations. `/rename`, `/delete` and `/clear` manage the current one.
- The bottom bar shows the workspace (the folder where it works), the model, how full its context is,
  and the permission mode. On the right are **Stop**, attach (a paperclip: an image for the model to
  see, or a file to read), microphone and settings.

### What it does offline

With no network and no model, ANIMA still:

- opens and closes apps, and sets volume and brightness ("open the calculator", "turn it up");
- does calculations, with follow-ups ("6 times 6", then "plus 5");
- tells the time and date, and the system status;
- sets timers and alarms ("set a timer for 10 minutes", "wake me at 7"), which ring in Do Not Disturb
  too;
- writes notes and reminders;
- gives definitions, synonyms and translations from the offline dictionaries (the dict packs in the
  Store);
- answers facts and summaries from the knowledge packs (Wikipedia packs in the Store's **Knowledge**
  category) and from Library collections.

When it is not sure what you meant, it suggests a command and waits for a yes. It never acts on a
guess. Details: [ANIMA_L0.md](ANIMA_L0.md), [ANIMA_KB.md](ANIMA_KB.md).

### Modes

Choose the mode in ANIMA's settings (`/config`, or the gear on the bar) under **Network mode**, or type
`/mode`:

| Mode | What it uses |
|---|---|
| **Offline** | the device only, nothing leaves it |
| **Local** | the device plus a language model on your own network (Ollama, LM Studio...). Nothing goes to the Internet |
| **Hybrid** (default) | the device, then the web (Wikipedia, weather, news, exchange rates), then the model as a last resort |
| **LLM** | the model answers first. If it is missing or silent, the device answers and still runs your commands |

`/mode` without an argument shows the mode and what ANIMA can reach right now. An answer given without
a model is marked `(offline)` or `(no model)`. More in [ANIMA_MODES.md](ANIMA_MODES.md).

### Connecting a model

In ANIMA's settings, under **Cloud teacher (API key)**:

1. Pick a provider: **Groq**, **Claude (Anthropic)**, **Gemini**, **Local server (Ollama, LM
   Studio...)** or **OpenAI-compatible (custom)**.
2. For a cloud provider, paste your **API key**. **Model** can stay empty for the default.
3. For a server on your network, fill in **Base URL**: Ollama `http://<pc>:11434/v1`, LM Studio
   `http://<pc>:1234/v1`, llama.cpp `http://<pc>:8080/v1`. **Model** must be the exact name the server
   lists (for example `llama3.2`). The key is optional.
4. Tap **Save teacher**.

`/model` shows the model in use, and `/model NAME` switches it. `/caps` shows whether the model can see
images and use tools. The same settings are in the web desktop's Settings > AI.

### Tools and permissions

With a model, ANIMA works as an agent: it can run shell commands, read and write files in your home
folder, install apps from the Store, open files, look at screenshots and images, run Lua, JavaScript and
Python, control Home Assistant and LAN devices (`ha`, `dev`), and create automations.

- Commands that only read (list files, check disk space, Wi-Fi status) run straight away.
- Anything that changes something asks first. ANIMA proposes the action and waits for your "yes". The
  proposal expires after two minutes.
- The permission chip on the bar cycles **Ask**, **Auto** and **Plan**. **Auto** runs actions without
  asking (a red frame shows it is on, and explicit "deny" rules still hold). **Plan** only reads and
  proposes. The same switches are `/auto on|off` and `/plan on|off`.
- A few settings can never be changed by ANIMA: the update and store addresses, the lock screen and
  the PIN.

Rules per action live in `/sdcard/data/anima/permissions.json`. See
[ANIMA_WORKSPACE.md](ANIMA_WORKSPACE.md).

### Memory and proactive checks

- Tell it a fact ("remember that...") and it keeps it, up to 48 facts, for every chat on the screen,
  the web and Telegram. You can manage them from the Memory page of the web chat.
- **Settings > Anima AI > Proactive checks (heartbeat)**: every 15, 30 or 60 minutes (or **Off**)
  ANIMA reads your checklist (`HEARTBEAT.md`, written in the web Settings > AI > Workspace) with the
  model, and notifies you only if something needs you.
- **Telegram**: connect a bot in the web Settings > AI > Telegram, then send it the `/pair` code shown
  in **Settings > Anima AI**. After that you can write to ANIMA from anywhere.

### Hands-free voice

**Settings > Anima AI > Hands-free voice** has **Listen for the wake word**, the **Wake word**, and
**Sensitivity** (**Low**, **Normal**, **High**). Say the wake word, then your question. ANIMA stops
listening when you go quiet and answers aloud. Listening pauses while the board plays or records audio.
In the ANIMA app, `/wake on|off|low|normal|high` does the same.

The wake word is an optional part of the firmware build **(experimental)**. If your build does not
include it, the switch is greyed out and the status line says why. See [HANDSFREE.md](HANDSFREE.md).

### Privacy

- API keys are saved in `/sdcard/data/anima/teacher.json`, sealed to this board's chip. The file
  cannot be read on another device.
- In **Offline** mode nothing leaves the board. In **Local** mode nothing leaves your network.
- Wake-word listening runs on the board. Audio is sent for transcription only after the confirmation
  sound.
- Cells in Sheets, images and files you attach go only to the model you configured.

---

## 10. DMD Pinball

**DMD Pinball** turns the board into the dot-matrix display (DMD) of a virtual pinball cabinet. When
no table is running, it is a touch menu of your PinUP Popper tables. The complete guide, with the PC
bridge and every screen, is [user/dmd-flipper/GUIDE.en.md](user/dmd-flipper/GUIDE.en.md).

### Connecting the PC

The PC runs Visual Pinball X with VPinMAME and dmdext 2.x. In dmdext's `DmdDevice.ini`, turn on one of
the two outputs:

- **USB** (recommended): `[zedmd]` with `enabled = true`. Connect the board's programming port
  straight to the PC, with no unpowered hub. The board presents itself as a ZeDMD 128x32.
- **Wi-Fi**: `[networkstream]` with `enabled = true`, `url = ws://<board IP>:9090/dmd` and `retry =
  true`. The app's waiting page shows the exact address. Give the board a fixed IP in your router so
  the address never changes.

The app must already be open when a table starts, because that is when dmdext looks for it.

### On the board

- **Hold the screen for 2 seconds** (or press Enter or Esc) to open the settings panel. It shows the
  link status and the Wi-Fi address, with **Back to the DMD**, **Test screen** and **Close the DMD**.
- **Screen**: **Layout for every table** (Automatic, Full-screen DMD, Classic DMD, Tall DMD, Stretched
  DMD, Backglass + DMD, Backglass only, Table art, Art with window, Backglass with housing), with
  **Choose with previews**, and **Rotation**.
- **Dots**: **Colour**, **Brightness**, **Dot shape** and **Glow**.
- **Table menu**: **Menu style** (**Cabinet** or **DMD**).
- **System**: **Clock when the PC is silent**, **Start at power-on** and **Network port**.
- **Test screen** checks orientation and colours without a PC. The solid block must be top-left.

**Start at power-on** is off by default. Turn it on for a cabinet, so the app opens a second after
boot. It does not open by itself while the lock screen is up, so on a cabinet it is better not to use
a lock-screen PIN.

### The table menu

With the PinUP Popper bridge running on the PC, the board lists your tables (**Recent**, **New**,
**Most played**, **Favourites**, **All**). Drag the wheel bar to choose a table and tap **Play**. During
a game, a short tap on the DMD opens quick actions: **Back to the DMD**, **Tables**, **Quit the game**,
**DMD settings** and **Composition of this table**. While a game's DMD is shown, the system gestures are
locked. Every way out of the app asks **Close the DMD?** first.

The Wi-Fi DMD port has no password. Use it only on your home network.

---

## 11. Second Screen

The **Second Screen** app shows a computer or phone screen on the board, with touch passed back to the
device. It is a step-by-step guide: pick your device and it suggests the best method, checks each step
by itself, and offers the next method when one fails. Technical details:
[SECOND_SCREEN.md](SECOND_SCREEN.md).

On the first page, **Connect a device**, choose **Windows**, **Mac**, **Linux**, **Android** or
**Other**. The quickest way from any computer is to open `http://<board IP>:7070` in its browser.

| Device | Methods, best first |
|---|---|
| Windows | **USB cable** (extended desktop, touch, up to 60 fps) **(experimental)**, **Wi-Fi from the browser**, **VNC** |
| Mac | **Mac Screen Sharing**, **Wi-Fi from the browser** |
| Linux | **Wi-Fi from the browser** (Chrome or Firefox), **VNC** |
| Android | **droidVNC-NG** (a free app) |
| Other | any desktop browser that can share its screen |

- **USB cable (Windows)**: the app switches the high-speed USB-C port to PC mode (one restart). Use
  that port, not the programming one. Install the signed driver from the address shown, or from the SD
  card (`nucleos/drivers/windows`). Then press Win+P and choose **Extend**. The USB method has never
  been tested with the cable.
- **Wi-Fi from the browser**: the board and computer must be on the same network. Open the address
  shown (or scan the code), click **Share screen** and pick a screen or window. If the browser refuses,
  use "Download NucleoCast" on the page.
- **Mac Screen Sharing**: on the Mac, System Settings > General > Sharing > Screen Sharing. Allow VNC
  viewers with a password, or type the Mac's user name and login password instead.
- **VNC**: start a VNC server (TightVNC on Windows, Remote Desktop / Desktop Sharing on GNOME or KDE,
  or `x11vnc`), then tap a server found on the network or type its **IP address** and **Password**. One
  finger works as the left button, a two-finger tap as a right click, and a two-finger drag as the wheel.

The first time a new device connects over the network, the board asks: **Allow**, **Always allow** or
**Refuse**. To leave the shared screen, swipe in from the left edge. The **Paused** card offers
**Resume** and **Disconnect**. **Options** has **Open this app when a device connects**, **Ask before a
new device connects**, **Forget trusted devices** and the USB port mode. **Settings > Security > Second
Screen ready at power-on** lets a PC connect at any time without opening the app first.

Miracast, AirPlay and Google Cast are not possible on this hardware.

---

## 12. Smart home

The board can be a panel for Home Assistant, a device inside Home Assistant, and a controller for LAN
devices. MQTT and Home Assistant support is **(experimental)**: it is off by default and has not yet
been run against a real broker.

### The Home app (with Home Assistant)

Install **Home** from the Store (id `casa`). It shows your lights, plugs, covers, thermostats, scenes
and sensors by room.

1. In Home Assistant, open your profile > Security > **Long-lived access tokens** and create one.
2. On the board: **Settings > Home > Home Assistant for apps**. Fill in **Home Assistant URL** (for
   example `http://homeassistant.local:8123`) and **Long-lived access token**, then tap **Save**. A
   long token is easier to paste from a PC, in the web desktop's settings.
3. Open **Home**. Pick a room at the top. Tap a tile to switch it, and tap the bar of a light that is on
   to dim it. Use **-** and **+** on a thermostat. Scenes and scripts run with one tap. Locks are shown
   but never opened from here.

Apps never see the token: their requests go through the system. More in
[apps/casa/GUIDE.en.md](../apps/casa/GUIDE.en.md).

### The board inside Home Assistant (MQTT discovery)

1. In Home Assistant, install the **Mosquitto broker** add-on and the **MQTT** integration.
2. On the board, open **Settings > Home > Home Assistant (MQTT)**. Fill in the broker
   (`homeassistant.local` or an IP), the port (`1883`), the MQTT username and the password, then tap
   **Save and connect**. **Status** should turn to **Connected**.
3. The device **NucleoOS xxxxxx** appears in Home Assistant, with no YAML needed. Its entities cover
   the screen (on/off and brightness), volume, mute, Do Not Disturb, notifications, an offline voice,
   opening an app, go home, lock, restart, touch activity, and diagnostics such as temperature and Wi-Fi
   signal.

Turning off **Connect to Home Assistant** removes the device from Home Assistant. **Publish again to
Home Assistant** resends it. Only plain MQTT (no TLS) and one broker are supported. Automation examples
are in [HOME_ASSISTANT.md](HOME_ASSISTANT.md).

### Other smart-home apps

- **Devices**: finds Shelly, WLED and Tasmota devices on your Wi-Fi and switches them, with no Home
  Assistant or cloud.
- **Zigbee**: your Zigbee2MQTT network, through the broker set in Settings > Home.
- **MQTT Explorer**: watch topics and publish test messages.
- **openHASP**: a touch panel driven by Home Assistant's openHASP integration.

In the Terminal, `ha` (for example `ha ls kitchen`, `ha on light.kitchen`, `ha say turn off the
lights`) and `dev` (Shelly / Tasmota / WLED) do the same. ANIMA uses them too.

---

## 13. The web desktop

Open the board's address in any browser on your network and you get a desktop with windowed apps that
talk to the board. It needs the SD card and the web companion (part of **Settings > System content**).

### Opening and pairing

1. Find the board's address in **Settings > Network** (**Connected - IP ...**), or type `ip` in the
   Terminal. `http://nucleov2.local` may also work on your network.
2. Open `http://<address>` in the browser. It shows **Pair with this device**.
3. The board shows **Pair a web device** with a 6-digit code and a countdown. Type the code in the
   browser and press **Pair**.

Each browser pairs once. Wrong codes slow pairing down. **Settings > Security > Web access** lists the
paired devices, with **Revoke** and **Revoke all**. The web desktop is plain HTTP, protected by the
pairing, so use it on your home network.

### What it offers

About forty apps, including a file manager (File Commander), Settings (with the AI, Telegram, voice and
workspace pages for ANIMA), Terminal, System Monitor, Spreadsheet, Notepad, Tasks, Calendar, Clock,
Calculator, Unit Converter, Media Player, Photo Viewer, Video Player, Voice Recorder, an AI chat and
agents, a browser, mail, an SSH client, a QR code maker, Wi-Fi Scanner, Log Viewer, Updates, Weather and
games. Files you open there are the files on the board's SD card.

---

## 14. Updates and safety

The board updates itself with signed firmware, keeps a way back from every update, and backs up your
settings to the SD card.

### Automatic updates

At startup the board checks for a newer release over Wi-Fi. If there is one, it downloads it to the SD
card, checks the signature, installs it and restarts. New releases reach devices in stages, so yours
may update later than someone else's. A manual check always offers the newest release. When an update is available but not yet installed, you get
the notification **Version ... available: Settings > System update**.

**Settings > System update** shows:

- **Current version**, and whether the running version has been confirmed, a safety copy exists, and
  which recovery is installed.
- **Update channel**: **Stable** (for everyone) or **Beta** (early builds).
- **Check for updates**, then **Download & install**, then **Restart to apply**.
- **Install from SD**: for offline updates. Put a release's `nucleos-anima.bin` together with its signed
  `nucleos-anima.json` in the card's root.
- The Store's **Region**.

Updates need an SD card with about 20 MB free. In the Terminal, `update status`, `update check` and
`update channel beta` do the same. Technical details: [OTA.md](OTA.md).

### If an update goes wrong

- A new version must run correctly for 60 seconds, with a working screen and a reachable update server,
  before it is confirmed. If it crashes, freezes or loses the network, the recovery restores the
  previous version by itself, and a notification tells you what happened. A version rolled back this
  way is not installed again automatically.
- If the system stops three times in a row, it starts in **safe mode**: the screen, network, web
  console and updates still work, but audio, USB, Bluetooth, ANIMA services and MQTT are off. Fix it
  with **Settings > System update > Restart normally**, with `update normal` in the Terminal, or by
  unplugging the power and plugging it back in. After five stops the previous version is restored.
- If nothing can start at all, the board shows a repair screen. A signed `nucleos-anima.bin` and `.json`
  in the card's root are installed as soon as they appear. You can also reinstall from the web flasher:
  "keep data" keeps your settings.
- A board with the old flash layout shows a message asking for a one-time reinstall from the web
  flasher. Settings and files are kept.

### Backups

Settings are saved to the SD card automatically, so a firmware update or a reset never loses them.
**Settings > Backup & restore** has **Back up now** and **Restore & restart**. **Factory reset** erases
all settings, saved Wi-Fi networks and the SD backup, then restarts. It asks first (**Erase
everything?**).

### Lock screen

**Settings > Security**:

- **Screen lock** shows a lock screen when the screen goes to sleep (set the delay in **Settings >
  Display > Screen sleep**).
- **Set PIN** sets a 4-digit PIN, and **Remove PIN** removes it.
- **Lock on startup** asks for the PIN at every power-on.
- **Lock now** is in the quick settings. Win+L locks from a keyboard.

After 5 wrong PINs the pad pauses for 30 seconds, doubling up to 15 minutes, even across restarts.

The same page lists the **Active protections** (signed firmware, signed Store apps, network access for
paired devices only, encryption state), the **Security events** since startup, **Web access**,
**Remote keyboard**, the Second Screen and statistics switches, and **App permissions**. Firmware and
Store apps are always checked. The developer switch **Allow unsigned store apps (developer)** should
stay off. More in [SECURITY.md](../SECURITY.md).

---

## 15. Troubleshooting

| Problem | What to do |
|---|---|
| **Wi-Fi: "Connection failed - check the password"** | Retype the password with **Show password** ticked. The board uses 2.4 GHz networks only: a 5 GHz-only network will not appear. **Forget** the network in **Settings > Network** and join it again. |
| **No networks found** | Tap **Scan** again and move closer to the router. Check that the **Wi-Fi** toggle in the quick settings is on. |
| **"No SD card"** | Insert a FAT32 microSD and restart. Photos, the Store, updates, backups and the web desktop all need it. |
| **Offline content missing** ("Offline content is missing: tap to download it.") | Open **Settings > System content**. It says what is blocking: no Wi-Fi, no card, not enough space, or a server that is not answering (the board retries by itself). **Download the recommended** gets everything. **Check installed files** and **Repair** fix damaged files. |
| **An app will not start** | **"Not enough memory to open the app"**: only one app runs at a time, so go Home and try again, or restart the board (**Settings > About > Restart device**). **"Another app is running"**: close it first. **"Needs a newer OS"** or **"Needs ... - update the system"**: install the latest firmware. **"Needs ... - install it from the Store"**: install that package. **"Timeout - app stopped"**: open it again. |
| **Store: "Can't reach the store" / "Install failed"** | Check Wi-Fi and free space on the card, then **Retry**. |
| **"2D accelerator stuck: videos and games may not show. Restart to fix it."** | Restart the board. |
| **No sound in a game or video** | Another app holds the speaker: close Music or the other app. Check **Settings > Sound** (volume, **Mute**) and **Play test sound**. |
| **Video: "unsupported format"** | Only MJPEG `.avi` and MPEG-1 `.mpg` play. Convert H.264 / MP4 files first. |
| **Web desktop shows an error or a blank page** | The web desktop lives on the SD card. Check that the card is in and that the web companion is installed in **Settings > System content**. |
| **The web API answers 401** | That browser or tool is not paired, or its pairing was revoked. Open the board's address again and enter the new 6-digit code. Check **Settings > Security > Web access**. |
| **The board started in safe mode** | It stopped several times in a row. Tap **Restart normally** in **Settings > System update**. If it keeps happening, report it (below). |
| **"Last boot crashed" notification** | **Diagnostics** shows the crash record. Note the firmware version (**Settings > About**) and report it. |
| **Gamepad or Bluetooth keyboard not working** | Pairing is **(experimental)**. Only Bluetooth Low Energy devices are supported over the air: use USB for other pads. |
| **USB drive does not appear** | USB drives are **(experimental)**. Plug the drive straight into the port, not through a hub. If **Settings > Storage** says the port is in PC mode, tap **Switch to accessories and restart**. |

### Reporting a problem

Open an issue on GitHub: <https://github.com/indecenti/NucleoOS-P4/issues>. Include the firmware version
from **Settings > About**, what you did, what you expected and what happened, and the **Diagnostics**
crash record if there is one. Before attaching a screenshot, check that it shows no private data, such
as your Wi-Fi name in the status bar or notifications.

Security problems should be reported privately, as explained in [SECURITY.md](../SECURITY.md).
