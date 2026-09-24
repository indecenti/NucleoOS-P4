# Second Screen — the board as a display for another device

The Second Screen app turns the 7" board into a screen for a computer or a phone. Several
transports feed one display engine; the app is a connection wizard that checks every step by
itself and, when a method can't work, proposes the next best one.

## What works with what

| Device | 1st choice | 2nd | 3rd |
|---|---|---|---|
| Windows | USB cable + signed IDD driver — **extended** desktop, touch, up to 60 fps | Wi-Fi from the browser page (mirror, no install) | VNC (TightVNC) / NucleoCast helper |
| macOS | Screen Sharing (VNC) — built in, touch drives the Mac; VNC password **or** Mac account (Apple auth, type 30) | Wi-Fi from the browser page (Safari/Chrome) | NucleoCast helper |
| Linux | Wi-Fi from the browser page (Chrome/Firefox) | VNC (GNOME/KDE/x11vnc/wayvnc) — resizable servers get exactly 1024x600 = extended | NucleoCast helper |
| Android | droidVNC-NG (free app), incl. reverse connection to board:5500 | — | — |
| Other | any desktop browser with screen sharing | — | — |

Not possible on this hardware: Miracast (needs Wi-Fi Direct discovery + H.264), AirPlay/Google
Cast (proprietary crypto/certificates), screen sharing from Android/iOS browsers (no
`getDisplayMedia` there).

## Architecture (`components/nv_secondscreen`)

- **Engine** (`ss_engine.cpp`, `include/nv_ss.h`): one owner at a time (USB / Cast / VNC).
  LIVE = LVGL stopped (`lvgl_port_stop`), the source owns the DSI framebuffer. JPEGs go through
  the P4 hardware decoder + PPA into the panel; VNC writes rows directly. Touch comes from the
  shared `nv_hal_touch_points()` cache (never read the GT911 directly: nv_hal's 60 Hz task owns
  it), fingers get stable ids by nearest-neighbour tracking. A swipe from the left edge PAUSES
  (panel snapshot kept, app UI back); Resume restores the snapshot. Lock order: LVGL port lock ->
  engine lock, never the reverse.
- **USB** (`ss_usb.cpp` + `components/nv_usb`): Espressif `usb_extend_screen` protocol for the
  xfz1986 IDD driver (VID 303A PID 2986, interface 0 = vendor, MUST stay 0: the driver binds
  `MI_00`). `_Bl` in the interface string is in **KB** (the driver multiplies by 1024) — it was
  advertised in bytes before, so the driver never lowered JPEG quality and big frames were dropped.
  Interface 2 = a read-only **setup drive** (virtual FAT16, `nv_usb_msc.c`): README, the sender
  page with the board address baked in, the driver installer (from SD), `nucleocast.py`.
- **NucleoCast** (`ss_cast.cpp`, `web/cast.html`): http :7070 + https :7443 (per-device
  self-signed ECDSA cert on SD, `ss_tls.cpp` — only for the browser's secure-context rule),
  WebSocket `/cast`. The page diffs 32x32 cells and sends changed rectangles as baseline JPEG
  tiles with a 16-byte header; one frame in flight (ack) keeps latency bounded; quality adapts to
  the ack round-trip. New senders need approval on the board ("Allow / Always allow" → token).
  The page also works as a local file (download or USB drive): `file://` is a secure context,
  so screen capture works with no certificate warning.
- **VNC viewer** (`ss_vnc.cpp`, `ss_des.c`): RFB 3.3/3.7/3.8 (+ Apple 3.889), security None /
  VNC auth (DES, validated against a reference + KAT) / Apple ARD (DH + MD5 + AES-128). Encodings
  Tight (fill/basic/palette/gradient/JPEG via HW decoder), ZRLE, Zlib, Hextile, CopyRect, Raw,
  DesktopSize, ExtendedDesktopSize (asks resizable servers for 1024x600), LastRect. The remote
  framebuffer is never stored: each decoded row is mapped (nearest) into the panel. Touch: one
  finger = left button/drag, two-finger tap = right click, two-finger drag = wheel. mDNS
  discovery of `_rfb._tcp`, reverse connections on 5500 while the app is open.

## Tests (`tools/nucleocast`)

- `mock_board.py` — the board's protocol on the PC (page + WebSocket, composites tiles to PNG):
  test the page and the helper without hardware.
- `nucleocast.py` — the desktop helper (Windows/macOS/Linux): `--test` / `--bench N` / real screen
  with touch -> mouse.
- `rfb_testserver.py` — RFB server exercising every encoding, VNC/Apple auth, resizable desktop,
  reverse connections; writes the expected picture.
- `board_test.py` — end-to-end on the real board with pixel comparison against `/api/screen`:
  `python board_test.py cast`, `python board_test.py vnc`, `python board_test.py bench`.
- `des_ref_vnc.py` — reference DES used by the test server (and to validate `ss_des.c`).
