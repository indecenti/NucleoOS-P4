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

All GET, LAN-open, JSON replies. Host = the board IP (default `192.168.0.128`; overridable).

| Endpoint | Effect |
|---|---|
| `GET /api/ui/state` | `{"app":"<id>"}` — foreground app id, `""` at home |
| `GET /api/ui/open?id=<id>` | open a native app (solo-mode; tears down any current app). `{"ok":bool,"app":"<current>"}` |
| `GET /api/ui/home` | close the foreground app + any shade/recents/search → launcher |
| `GET /api/ui/tap?x=<>&y=<>` | inject a synthetic pointer tap at absolute coords (0..1023 , 0..599) — drives rails, tabs, chips, buttons |
| `GET /api/screen` | JPEG of the current panel (1024×600) |

Taps go through a dedicated synthetic LVGL input device, so they resolve to real press→click on
whatever widget is under the point.

## Capture workflow

```
powershell -ExecutionPolicy Bypass -File .claude/skills/ui-screenshot/scripts/shot.ps1 -Open sysmon -Out D:/tmp/perf.jpg
```

Then Read the JPEG to see it. Common recipes:

- **Screenshot an app**: `shot.ps1 -Open <id> -Out shot.jpg` → open + settle + capture.
- **Walk the tabs/buttons**: capture once, read the image to find a control's pixel position, then
  `shot.ps1 -Tap "x,y" -Out tabN.jpg`. Repeat per tab. (Coordinates are absolute panel pixels.)
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
- **Find the IP**: `GET /api/info` returns it (`{"ip":"192.168.0.128",...}`); or try
  `http://nucleov2.local`. Pass `-Ip` if it differs from the default.
- **Board must be on Wi-Fi + web up**. If calls time out, the board is off-network or `nv_web`
  isn't serving yet (it starts after Wi-Fi connects). A 404/timeout on `/api/ui/*` on an older
  firmware means the board predates this feature — ship an OTA build first (see the `ota-release`
  skill).
- **Solo-mode**: only one app is resident at a time; `open` always leaves the previous app. There
  is no app→app back stack (RAM strategy) — use `open` per app, `home` to reset.
- **Coordinates** are the full panel (status bar at y≈0..40, app header ~40..80, content below).
  Tap targets are ≥44 px, so exact center isn't required.
- Screenshots land wherever `-Out` points; keep them in a scratch/temp dir, not the repo.
