---
name: ui-screenshot
description: Drive the NucleoV2 ESP32-P4 UI remotely over Wi-Fi to capture screenshots of any app or state — open a native app by id, inject taps (rails/tabs/buttons), go home, then grab the panel as a JPEG. Use when the user wants to see/verify an app on the device screen, "fai uno screenshot dell'app X", "apri l'app e mostrami", "screenshot di ogni tab", or to visually verify a UI change without a serial/USB cable. Needs the board on Wi-Fi with the web companion (nv_web) running.
---

# UI screenshot / remote navigation (NucleoV2)

The board runs a web companion (`nv_web`) exposing a small automation surface that lets you drive
SystemUI headlessly and capture the panel — no COM/USB needed, just the board's Wi-Fi IP. This is
the way to *see* an app after a change (the board has no local screen-share).

One PowerShell script wraps everything: `scripts/shot.ps1`.

## The endpoints (nv_ui + nv_web)

All GET, JSON replies. Host = the board IP (default `192.168.1.50`; overridable). Every call needs
the paired session token: pair the PC once with `python tools/pair.py --serial COM5` (reads the code
from the serial console) or `python tools/pair.py` (type the code shown on the board); shot.ps1 then
reads `%USERPROFILE%\.nucleo\token` by itself. A 401 means this PC is not paired (or was revoked in
Settings > Security).

| Endpoint | Effect |
|---|---|
| `GET /api/ui/state` | `{"app":"<id>"}` — foreground app id, `""` at home |
| `GET /api/ui/open?id=<id>` | open a native app (solo-mode; tears down any current app). `{"ok":bool,"app":"<current>"}` |
| `GET /api/ui/home` | close the foreground app + any shade/recents/search → launcher |
| `GET /api/ui/tap?x=<>&y=<>` | inject a synthetic pointer tap at absolute coords (0..1023 , 0..599) — drives rails, tabs, chips, buttons |
| `GET /api/ui/type?text=<url-encoded>` | insert literal text into the IME's focused field, whole line in one call — no per-letter taps on the on-screen keyboard. `{"ok":bool}`, false if no field is focused |
| `GET /api/ui/key?code=<name>` | a special key on the focused field: `enter esc backspace delete tab left right up down`. `{"ok":bool}` |
| `GET /api/screen` | JPEG of the current panel (1024×600) |

Taps go through a dedicated synthetic LVGL input device, so they resolve to real press→click on
whatever widget is under the point. `type`/`key` reuse `nv_ime_inject_text`/`nv_ime_inject_key` —
the same path the USB-keyboard and KeyDeck remote-keyboard use — so they only work once some field
already has IME focus: tap the field first (a real tap or `/api/ui/tap` on it), then `type`/`key`.

**`key=enter` does not submit every field.** `nv_ime_inject_key(ENTER)` runs the keyboard's
`ready_action()`: GO/SEARCH/SEND fields fire the ONE global callback set by
`nv_ime_set_submit_cb`; `NV_IME_RET_ENTER` fields (the Terminal's command line) get their own
`LV_EVENT_READY`, so `-Key enter` runs a Terminal command and the keyboard stays up. A page that
listens only for a per-widget `LV_EVENT_READY` on a GO field still just closes the keyboard on a
remote Enter — if `key=enter` does nothing there, look for an explicit button.

## Capture workflow

```
powershell -ExecutionPolicy Bypass -File .claude/skills/ui-screenshot/scripts/shot.ps1 -Open sysmon -Out D:/tmp/perf.jpg
```

Then Read the JPEG to see it. Common recipes:

- **Screenshot an app**: `shot.ps1 -Open <id> -Out shot.jpg` → open + settle + capture.
- **Walk the tabs/buttons**: capture once, read the image to find a control's pixel position, then
  `shot.ps1 -Tap "x,y" -Out tabN.jpg`. Repeat per tab. (Coordinates are absolute panel pixels.)
- **Type into a search box** wired through `nv_ime_set_submit_cb` (e.g. Settings' update-URL
  field): tap it, type, `-Key enter` submits — no per-letter on-screen-keyboard taps:
  `shot.ps1 -Tap "x,y" -Type "some text" -Key enter -Out result.jpg`.
- **Type a Terminal command**: tap anywhere on the terminal (focuses the inline command line),
  then type and Enter: `shot.ps1 -Tap "500,300" -Type "ls /sdcard" -Key enter -Out term.jpg`.
  The keyboard stays up; later commands need no tap: `shot.ps1 -Type "apps" -Key enter ...`.
- **Back to launcher**: `shot.ps1 -GoHome -Out home.jpg`.
- **Just the current screen**: `shot.ps1 -Out now.jpg`.
- **Where am I**: `shot.ps1 -State` → `{"app":"sysmon"}`.

Flags: `-Ip <addr>` (or env `NV_BOARD_IP`), `-Wait <ms>` (settle delay before capture, default 800
— raise for slide-in animations), `-Out <path>` (JPEG; defaults to a temp file, and the script
prints `SHOT <path> | app=<id> | <bytes>`).

## Native app ids

`settings files anima diag apps terminal secondscreen gallery music video recorder camera calc
notes tasks sysmon`

(Installed WASM apps use their own manifest id. Unknown id → `{"ok":false}`.) When unsure of the
current state, call `-State` or open the launcher with `-GoHome` first.

## Notes / gotchas
- **Find the IP**: `GET /api/info` returns it (`{"ip":"192.168.1.50",...}`); or try
  `http://nucleov2.local`. Pass `-Ip` if it differs from the default.
- **Board must be on Wi-Fi + web up**. If calls time out, the board is off-network or `nv_web`
  isn't serving yet (it starts after Wi-Fi connects). A 404/timeout on `/api/ui/*` on an older
  firmware means the board predates this feature — ship an OTA build first (see the `ota-release`
  skill).
- **Solo-mode**: only one app is resident at a time; `open` always leaves the previous app. There
  is no app→app back stack (RAM strategy) — use `open` per app, `home` to reset.
- **Coordinates** are the full panel (status bar at y≈0..40, app header ~40..80, content below).
  Tap targets are ≥44 px, so exact center isn't required.
- **The on-screen keyboard shifts the layout when it slides up** (~42% of the screen height): a
  field and its side buttons (send arrow, EOF, stop, ...) move to new y-coordinates once a field is
  focused and typed into. Re-screenshot after focusing/typing before tapping a button near a text
  field — coordinates read from a shot taken before the keyboard appeared will land on a different
  widget (learned the hard way: a stale coordinate landed on the keyboard's own ")" key instead of
  Terminal's send button, typing a stray character into the field).
- Screenshots land wherever `-Out` points; keep them in a scratch/temp dir, not the repo.
