# DMD Flipper + PinUP Popper: the board as a touch menu when no table runs

Status: **version 2, running on the real cabinet** (DESKTOP-NVR308C: Windows 10, VPX 10.8.1, VPinMAME +
dmdext 2.5, PinUP Popper, 10 emulators, ~1000 tables). Tested there end to end: lists and images from the
real database, wheel bar, themes, Popper's selection followed (showcase), a VPX table with DMD (Attack from
Mars) shown over USB as ZeDMD, a table without DMD (Firepower), a Pinball FX2 table with the game's
showcase, quitting a game from the board, the port handed over and back at every start / end.

## How it works

### The pieces

- **The board**: the DMD app (`components/nv_apps/dmd_app.cpp`) with the menu (`dmd_menu.cpp`), the DMD
  core (`components/nv_dmd`).
- **The bridge** on the PC (`tools/popper_bridge/nucleo_bridge.py`, Python 3.12 + pyserial + Pillow): it
  starts with Windows (a scheduled task, `ponte.ps1 installa`), runs hidden at below-normal priority, owns
  the board's USB port while no table runs. `ponte.ps1 stato` shows its CPU / memory (a session: ~1 s of CPU
  in minutes, ~27 MB).
- **Popper**: its database read-only; its emulator Launch / Close scripts call the bridge
  (`curl ... :58090/game/start|end`, first line of every visible emulator); its **Web Remote** (`useWEB =
  true`, port 80, also on the LAN) to launch (`/function/launchgame/<id>`), quit (`/pupkey/15`, fEmuExit,
  tested) and read the selection (`/function/getcuritem`).
- **dmdext** (VPinMAME's `DmdDevice.dll`, `[zedmd] enabled = true`): the DMD of VPX tables over the same
  USB port, as a ZeDMD.

### Who has the USB port

| Moment | Port | Board shows |
|---|---|---|
| No table | the bridge | the menu, or the showcase when the player moves in Popper |
| A VPX table (DMD through dmdext, `DMD_EMUS = {1}` in the bridge) | dmdext | the DMD (or "in game" for a table without DMD frames, e.g. 1980 alphanumerics) |
| Pinball FX2, MAME, PC games, the Godot games (HAKKEN, Downwell, Upwhell) | the bridge keeps it | the game's showcase (its horizontal pictures) + quick actions over the cable |

While dmdext has the port the board sends nothing on it (`nv_dmd_bridge_send` refuses as soon as ZeDMD frames
arrive, until the ZeDMD goodbye, 10 s of silence or the bridge talking again): a request written there
would reach dmdext, get lost, and count as asked (that is how the instructions button once stayed hidden
after a game). The menu asks again once the bridge is back.

**The bridge restarted during a game** (an update, a crash): at start it lists the processes once and,
if a table's emulator runs (`GAME_EXES`), takes Popper's selected table as the running game. A VPX table:
it leaves the port to dmdext and waits for Popper's end hook. A game without DMD: it opens the port and
sends the board the game and its showcase. On the board, a DMD driven by dmdext with no game announced
still gets "quit the game" in the quick actions (over Wi-Fi: the board keeps the PC's address in its
settings, `dmd_pc_ip` / `dmd_pc_web`, so this works after a board restart too).

**Popper's end hook missing** (seen: a crashed or killed emulator, an exit pressed while the table was
loading): at every game start the bridge finds the emulator's process once and waits on it with a
kernel wait (`WaitForSingleObject`: no polling, no CPU). It exits and no hook came within 6 s: the
bridge ends the game itself (log: "the emulator exited without Popper's end hook") and the menu comes
back. Checked on the cabinet with VPX killed: menu again in 14 s.

**What a crashed table leaves open**: VPX's B2S backglass server (`B2SBackglassServerEXE`) stays open on
its own when VPX crashes or is killed, and Popper then refuses every launch (seen on this cabinet). After a
table's emulator exits, the bridge looks once at the processes: no table running and that server still
there, it closes it (log: "closed b2sbackglassserverexe..."). With a table running nothing is touched.

dmdext's mirror mode can read Pinball FX2's DMD (`dmdext mirror --source=pinballfx2`), but it is one more
process reading the game's memory on an old PC and not certain on this FX2 build: not used; the showcase
is shown instead.

### On the board

- **Two looks** (panel → "Stile del menu"): **Cabinato**, Popper's own default theme as on this cabinet
  ("Cyan Dusk"): the wheel bar between chrome rails with a cyan light tube, the selected wheel in a chrome
  and glass orb (rendered once with Qwen Image 2.1, `tools/icons/make_dmd_orb.py`), a lit cyan PLAY; and
  **DMD**: titles, facts, lists and buttons drawn as ColorDMD-coloured dots, the backglass seen through an
  LED grid, square bezels.
- **The stage**: the selected table — its backglass behind everything (sent at 512x300, enlarged and
  darkened on the board), its wheel, its name without "(Maker Year)" tags, maker and year, plays and play
  time, last played, high scores, PLAY / instructions.
- **The wheel bar**: drag it, tap a wheel, or swipe the stage (`nv_gesture_bind`: the system edge
  gestures keep working). A slide remembers the table it goes to: a list that arrives meanwhile (a game
  ended, the stats changed, the order of "Recenti" too) does not move the finger's choice to another table. The middle wheel is large and lit, the others pre-shrunk and dimmed once on
  the board (no image transforms: scaled images came out as noise on the board; the cause was not found — LVGL's PPA unit is off in this build).
- **Showcase**: when the player uses the cabinet's controls in Popper (Popper leaves its attract mode, or
  the selection moves outside it — read once a second from `getcuritem`), the board shows that table's
  pictures one after the other every 7 s (backglass, DMD, topper), each framed in a glow of its own colours.
  Flyers and instructions are not in it: on this cabinet all of them (1358, checked) are portrait cards
  saved on their side for a rotated screen, unreadable in a 1024x600 slide. A touch: the menu, on that table.
- **Popper on something that is not a table's showcase**: on an entry ("Go Back", "Exit", a playlist —
  `getcuritem` gives GameID 0, mtType 2, its own label in GameDisplay and an internal name in GameName,
  "menuback" for Go Back) the board shows a card: the entry's sign, the action in big dots in the board's
  language ("INDIETRO", "ESCI", or the playlist's name) and Popper's own label under it. A table without
  any horizontal picture: its wheel and name. Before, the last table's pictures stayed on screen. Popper
  back in its attract mode: the board's menu again. A touch: the menu (on that table, for a table).
  `getcuritem` sometimes answers `""` for a moment while Popper changes list: that reading is skipped
  (it used to pause the reading for 4 s).
- **Instructions** (the button on the stage): the bridge turns the card upright (90 degrees clockwise, once,
  cached) and fits it to the screen's width (narrower when needed to fit the USB link's ~1.2 MB message);
  the board shows it as a tall page scrolled with a finger; a tap or Back closes it.
- **In game**: a tap on the DMD (or on the showcase of a game without DMD) opens the quick actions —
  back, the tables (browse during the game, a "back to the DMD" button in the header), quit the game,
  DMD settings. Starting another table while one runs asks "X is running: quit it and start Y?": the board
  quits the running one and starts the new one when the bridge reports the end.
- **Quit / launch during a VPX game** go over Wi-Fi straight to Popper's Web Remote (dmdext has the
  cable; the bridge tells the board the PC's address in its hello). For the other emulators they go
  through the bridge on the cable.
- **NucleoOS integration** (behaviour, not looks): system gestures (shade, Home / Recents, Back from the
  left edge) work on the menu, the showcase and the panel; only the DMD of a running game locks them.
  Back closes the modals / help / browsing first. The app keeps its list and table when it is suspended
  and comes back (`nv_ui_state_save`). Toasts are the system's. Keyboard / remote: left / right move the
  wheel bar, Enter plays.
- **Attract**: after 2 minutes without a touch the wheel bar turns by itself (not during a game; the
  2 minutes start again when a game ends, so the table just played stays on screen; its card is asked
  again then, with the new scores).

### Finding a table among hundreds

- **"Go to" (A-Z)**: the categories of the list as chips, each with how many of the open list's tables it has
  (in "Recent" a system counts only the ones played: "Visual Pinball X 41", in "All" 72), and the letters as
  keys: a category filters the list (on the board, nothing asked to the PC), a letter jumps to its first
  table; letters nothing starts with are off; digits and the rest are "0-9".
- **Categories** come from the bridge (`CATEGORIES` in `nucleo_bridge.py`, or `categories.json` next to it):
  Popper's emulators gathered in a few — Visual Pinball X, Future Pinball, Pinball FX, MAME split by
  Popper's genre (Shoot'em-ups, Mazes, Platforms, Sports and driving, Other arcade), and "PC games" for the
  launchers with one to three games each (Downwell, HAKKEN, DOWNWHELL, UPWHELL, GRAVEFLIP). An item's `emu`
  is its category's id; the hello's `emus` lists them with their sizes (`n`).
- **Lists are confirmed**: the board acks a whole 'L' as it does an 'I', and the bridge sends it again if not
  (a lost "All", 138 KB, left the menu on the card's copy); a list for another tab goes to the card only.
- **Fling**: a quick swipe on the wheel bar keeps it going — the faster, the further (at most a third of
  the list); a slow drag moves by what it covered.
- The count says which category is shown ("Sparatutto 12 / 324"), its name cut to the room the header
  leaves ("Visual Pin. 28 / 72"), never the buttons pushed off; in a mixed list the facts say it too
  ("Capcom · 1990 · Sparatutto").
- A table without a wheel shows its name in the orb and on the bar instead of an empty ring.

### Reactivity: the microSD cache

The cable carries ~0.8 MB/s: a wheel takes 30-50 ms, a backglass or a showcase page 0.5-1.5 s. So:

- opaque pictures (backglass, showcase, game backglass, instructions) travel as baseline JPEG: the board
  asks with `"jpeg":1` and decodes them in its hardware JPEG unit (~10 ms for 1024x600). A showcase page is
  ~60-120 KB instead of ~250 KB zlib (1.2 MB of pixels); the PC makes it in tens of ms (Pillow) and keeps
  it (`.nvj` in its cache). Wheels keep RGB565 + alpha, zlib (they need transparency, and are small).
  FullDMD art and the grill backglass stay RGB565 zlib for now;
- every picture is kept on the board's microSD as it came (JPEG, or compressed RGB565), in
  `/sdcard/data/dmd/img`, and read from there next time (a few ms) instead of crossing the cable;
- in memory the pictures stay under 8 MB together (least recently used first out; never the ones on
  screen or the running game's art); an allocation that fails makes room the same way. Before, only their
  number was limited, and a few dozen showcase pages left 4 MB of PSRAM (screenshots failed);
- a game starts: the menu's pictures leave the memory (they are on the card), only the game's art and the
  wheels in view stay. A game holds the board for long, and ~8 MB of 1.2 MB pictures left the PSRAM in
  pieces: the screenshot's encoder (two ~1.2 MB buffers) then failed, silently (it logs it now);
- a copy read from the card is checked later, when nothing else waits: the board sends the bridge's key
  of those bytes (`"have"`), the bridge answers 'K' (a few bytes: no conversion, no file read) or sends
  the new picture when Popper's file changed; a picture on screen is swapped for the fresh one safely;
- with the menu untouched for 15 s (and no joystick in Popper meanwhile) the board fetches the wheels (bar
  and stage size) and backglasses of the list one at a time onto the card: after the first time the whole list is instant (the PC converts each picture once, at
  below-normal priority, only while no game runs);
- a joystick move in Popper goes first: the showcase's request does not wait behind a background fetch;
- JPEG quality by use: instructions 90 (text), showcase 85, backglass and game backglass 80 (shown enlarged
  and darkened); the quality is part of the PC cache's key;
- Popper's selection is read as often as the player needs: 5 times a second for 20 s after a move, twice
  a second on a table left alone, once in Popper's attract mode, with `http.client` (~2.6 ms of CPU per
  reading on this PC's AMD A4-5050, urllib took 5-8 ms): faster where it shows, less CPU on the whole;
- a showcase picture the board asked for and the player already left behind in Popper is not converted
  (the CPU goes to the table on screen); the board's prefetch requests carry `"pf":1` and are never dropped;
- in idle time (no game, nobody at Popper for 30 s) the bridge converts ahead the showcase pictures and
  backglasses of every table (784 tables, ~2,200 pictures, ~140 MB in its cache, 78 GB free here), one
  at a time, resting 6x as long as it worked (2x measured ~18% of a core here), recently played first: then a joystick
  move only reads a file (~5 ms instead of ~300). Log: "pictures ahead: N converted";
- the board's prefetch also puts each table's first showcase picture on the card; a picture that is not
  wanted on screen when it arrives goes onto the card as it came, not decoded (no 1.2 MB of pixels made,
  nothing pushed out of the memory). A prefetch asked twice in a row without landing on the card is left
  for the next pass (never asked in a loop);
- before (measured): Popper's selection was read twice a second: measured, from the joystick to the
  first picture ready on the PC ~1.4 s with the old 1 s reading and RGB565; the reading alone was half;
- the lists are kept on the card too: the menu shows the last one at once and applies the bridge's only
  when it differs; at rest the list is asked again every 5 minutes (favourites, new tables);
- in the simulator (below) a second run with the card filled sends the board 0 pictures out of ~60.

### The right pictures for each table

- the board remembers what the bridge has no picture for by table, kind and showcase screen. Before, any
  error but instructions and backglass was taken for "no wheel": a table without a game backglass (Twilight
  Zone here) lost its wheel once it left the memory, and a missing showcase screen was asked again and again;
- the bridge's errors always say table, kind and screen, also for a picture it cannot convert (a damaged
  file): marked missing on the board, not retried in a loop;
- a table's file is its game name, or one of Popper's alternatives ("Name(1).png", "Name (2).png"); never a
  file that only starts with the same words ("Blade" does not take "Blade Runner.png"; checked on this
  cabinet: none of its 817 tables was affected so far);
- changing table, the previous table's backglass goes at once; this table's comes up (a short fade) as the
  selection rests. The stage's wheel shows nothing until its own is there;
- a copy on the card that cannot be decoded for lack of memory stays on the card (only a broken one goes).

### In game

- A VPX table that has sent no DMD frame yet is "Loading..." (up to a minute: then it is a table without a
  DMD, "Playing for 12 min").
- The in-game page is a page of its own: the game's backglass darkened behind its wheel, its name, the time
  played, two tiles (tables, quit).

### The simulator (no board needed)

`tests/host/sim_dmd` builds the real `dmd_menu.cpp` on the PC with the real LVGL 9.5, NucleoOS's fonts and
strings, the dot renderer, and answers the bridge recorded on the cabinet
(`tools/popper_bridge/sim_data.py`, data not in git). It runs scenes (menu, drag, fling, "go to", filter,
showcase, VPX and FX2 games, quick actions, empty list, attract) in both looks and writes PNGs:

```
python tests/host/sim_dmd/run.py          # both looks; "cab" or "dmd" for one; "sel": taps while lists come and go ([sel] lines)
```

It counts the bridge requests by kind (pictures sent, card copies confirmed) and keeps its "microSD" in
`tests/host/sim_dmd/sdcache`.

### The PC's CPU

The bridge does the minimum on the PC and leaves the rest to the board:

- images converted once (Pillow's C code only, JPEG decoded at reduced scale) and cached on disk; media
  folders listed once and again only when they change; picture orientation read from the header once;
- everything visual (backglass enlarging, darkening, LED grid, ambient glow, side wheels, dot text,
  the DMD itself) is computed on the board;
- the serial port is read blocking (one wake-up a second when idle), Popper's selection read once a
  second only while no game runs; during a game the bridge sleeps;
- below-normal priority, no window, ~27 MB.

### Wire format

`"NVDM"` type(1) len(u32 LE) payload sum(u16 LE). Board → bridge: 'Q' requests (JSON: `hello`, `list`,
`table`, `random`, `image` — kinds `wheel` (with `alpha`: RGB565A8), `backglass`, `help`, `gamebg`, `media`
+ `screen` — `launch`, `exit`). Bridge → board: 'H' hello (with `ip`, `web`), 'P' ping (2 s), 'L' list,
'T' table, 'I' image (u16 json_len, JSON, zlib pixels: `rgb565z` or `rgb565a8z`), 'G' game start / end
(`dmd`, `showcase`), 'S' Popper's selection (table + its horizontal screens), 'R' result, 'E' error.
Board side: `dmd_link_*` in `dmd_core.c` (unit-tested, fuzzed), `nv_dmd_bridge_*` in `nv_dmd.c`.

**Losses on the cable** (measured on the cabinet with 400 KB messages back to back at ~880 KB/s): none with the
DMD app freshly open (0 of 150), about one in six with the menu loaded. Each time up to 64 bytes (one USB
packet) go missing, sometimes bytes are changed instead, while the receiver is never behind (longest wait
~15 ms, no full read). The same with the driver's buffer in internal RAM and with its interrupt on CPU 0, so
neither is the cause; the cause is still open. A missing piece makes the damaged message eat the start of
the next one. The board confirms every whole 'I' (`{"q":"ack","n","s"}`)
and the bridge sends a game's pictures again until they are confirmed. When a message's checksum fails, the
board looks inside it for the next message's start and feeds it in again, so only one message is lost, not
two. The log line for a damaged message gives the longest wait between two USB reads, the slowest pass,
how many reads filled the buffer, and how many bytes were missing.

### Install and operate

`tools/popper_bridge/README.md` (Italian). The cabinet's own Claude has a skill for it
(`tools/popper_bridge/skill/SKILL.md`, installed in `C:\Users\pinball\.claude\skills\nucleo-dmd`).

# The study (before version 1, kept for the reasons)

## Goal

When no table is running, the 7" touch board (today: the DMD's waiting page or idle clock) becomes an
appendix of PinUP Popper's menu: recently played tables, new ones, most played, favourites, a random pick,
the table's card (wheel image, plays, time played, high scores, instruction card) and a **Play** button that
launches the table on the cabinet. When a table starts, the board goes back to being the DMD; when it ends,
back to the menu.

## What Popper offers

### Its database: `PUPDatabase.db` (SQLite)

In the PinUPSystem folder (usually `C:\vPinball\PinUPSystem\PUPDatabase.db`; VPin Studio opens it there).
The tables that matter, as VPin Studio queries them:

| Table | Fields used |
|---|---|
| `Games` | GameID, EMUID, GameName, GameFileName, GameDisplay, Visible (0 hidden, 1 visible, 2 adult / hidden), DateAdded, DateUpdated, GameYear, Manufact, Category, GameRating, TAGS, ROM, Author, IPDBNum |
| `GamesStats` | GameID, LastPlayed, NumberPlays, TimePlayedSecs, HighScores |
| `Playlists`, `PlayListDetails` | playlists; `isFav` = favourite inside a playlist |
| `Emulators` | LaunchScript, PostScript (the close script) |

Queries the community already uses: recently played = `GamesStats` by `LastPlayed DESC`; most played = by
`TimePlayedSecs` (or `NumberPlays`) `DESC`; new = `Games.DateAdded >= datetime('now','-30 day')`.
Not verified: the column of Popper's *global* favourites. Reading while Popper runs works in practice
(VPin Studio does it all the time, short read-only connections); a reader can still get `SQLITE_BUSY` while
Popper writes the stats at the end of a game, so reads must be short, read-only and retried.

### Its Web Remote (official, off by default)

`useWEB=true` in the script options + `PUPServerRegisterCOM.bat` as administrator + a restart. It runs
**only while Popper runs**, has **no authentication** (NailBuster: never on a public network), and offers:
`/function/launchgame/<GameID>`, `/function/getcuritem` (JSON of the selected item, schema not documented),
`/function/getdisplay/<n>` (png or mp4 of a screen), `/pupkey/<id>` (menu keys: 1/2 previous/next game,
7 launch, 15 exit the emulator, 50/51 add/remove favourite, ...). **There is no endpoint that lists the
games.** Port: 80 by default as far as the wiki shows (`PUPServer.exe -wwwport <p> -sockport 8888`).

### Media

`PinUPSystem\POPMedia\<Emulator>\<Screen>\<GameName>.<ext>`, screens: Wheel, Backglass, DMD, Topper,
PlayField, Loading, GameInfo, GameHelp, Menu, Audio... Wheel images (png with transparency) are ideal for a
1024x600 grid / carousel; GameInfo / GameHelp are instruction cards (images); DMD videos are mp4 (would need
converting on the PC); playfield videos are too heavy (and rotated) for the board.

### Game started / ended events

Each emulator in Popper has a Launch script and a Close script with variables such as `[GAMEFULLNAME]` and
`[DIREMU]`. VPin Studio adds one line to each (`curl -X POST ... http://localhost:8089/service/gameLaunch`
and `.../gameExit`). The same mechanism can notify the board (or our PC agent). The board already has a
fallback signal: DMD frames arriving = a table is running (not enough alone: some tables have no DMD).

### VPin Studio (if installed)

A Java server (Spring Boot, port 8089, very active project) that already exposes over HTTP: the game list
(`/api/v1/games`, with dateAdded, numberPlayed, rating), a game, its scores, launching
(`/api/v1/frontend/launch/{id}`), the running game (`/api/v1/gamestatus`) and media
(`/api/v1/media/{id}/{screen}`). No authentication found. It is an internal API (not documented as public:
it can change between versions). A "recently played" endpoint was not found (LastPlayed is in its model).

## Architectures

| | On the cabinet PC | For | Against |
|---|---|---|---|
| **A. Popper's Web Remote only** | `useWEB=true`, COM registration, port 80 | official; launch and keys | no game list, only while Popper runs, undocumented JSON: not enough for a menu |
| **B. VPin Studio's REST API** | VPin Studio Server (Java service) | list, media, scores, launch, game status ready | heavy dependency; internal API that may change |
| **C. Our small PC agent** (recommended) | one small exe (tray / service) + one line in the emulator's launch/close scripts | a stable contract made for the board: lists from `PUPDatabase.db` (read-only), wheel images already shrunk to the board's size, launch through Popper (Web Remote or its launcher), game start/end pushed to the board, found by the board on the LAN by itself | we write and maintain it |
| D. Reading the DB over an SMB share | a share | nothing to install | SQLite over the network is unsafe, no launching: rejected |

**Proposal: C**, with VPin Studio as an optional source when it is installed (the agent can ask it instead of
the DB). The board never depends on Popper's or VPin Studio's versions; the agent does.

Sketch of the contract (agent → board, JSON over HTTP on the LAN, events over a WebSocket):

```
GET  /tables?list=recent|new|top|fav|all&limit=30   -> [{id, name, year, maker, plays, last, rating}]
GET  /table/{id}                                     -> details + high scores
GET  /media/{id}/wheel?w=240                         -> PNG/JPEG sized for the board
POST /launch/{id}                                    -> starts the table in Popper
WS   /events                                         -> {"game":"start","id":..} / {"game":"end"} / {"popper":"up"}
```

USB: the menu needs data both ways; over USB the ZeDMD link carries only frames. Options: Wi-Fi for the
menu while the DMD runs over USB (simplest), or a second USB channel (more work). To decide.

## What the board would show

Proposal for the 1024x600 touch screen when no table runs (the menu is an "appendix" of the DMD app):

1. **Home**: big rows of wheel images, swipe sideways: *Recently played*, *New*, *Most played*,
   *Favourites*; a *Surprise me* button (random table).
2. **Table card**: wheel / backglass image, year and maker, times played, time played, last played,
   high scores, instruction card (GameHelp) if present, a big **Play** button.
3. While a table runs: the DMD (as today); a long press gives *Exit the table* (Popper key 15), volume.
4. Idle for a while: attract mode (wheel slideshow or the clock), touch wakes the menu.

## Questions

1. Popper version, and is it installed in `C:\vPinball\PinUPSystem`?
2. Is VPin Studio installed (server running)?
3. Can we install a small program on the cabinet PC, and add one line to the emulator's launch / close
   scripts in Popper?
4. Link for the menu: Wi-Fi (simplest) or must everything go over USB?
5. Which features matter most (order): recent, new, most played, favourites, random, launch, high scores,
   instruction cards, exit table, volume?
6. Is Popper's Web Remote already on (port 80 free)?
7. Hidden / adult tables (`Visible` = 2): never show them?

## Sources

- PinUP Web Remote: https://www.nailbuster.com/wikipinup/doku.php?id=web_remote_control
- PinUP FAQ: https://www.nailbuster.com/wikipinup/doku.php?id=faq
- SQL playlists (Games / GamesStats queries): https://vpinball.com/forums/topic/sql-playlists/page/2/
- VPin Studio source: https://github.com/syd711/vpin-studio (`PinUPConnector.java`, `PupServer.java`,
  `FrontendResource.java`, `GamesResource.java`, `GameStatusResource.java`, `VPinScreen.java`,
  `application.properties`)
- VPin Studio status updates (launch / exit scripts):
  https://github-wiki-see.page/m/syd711/vpin-studio/wiki/VPin-Studio-Server-Status-Updates
