# Developing WASM apps for NucleoOS (ESP32-P4)

Condensed guide. The source of truth for the imports is `sdk/include/nucleo_sdk.h`; the reference
app is `apps/abc123`. Apps run in the WAMR interpreter with a full-screen 1024×600 canvas
(ABI v2 "gfx"). They are built **on the PC** (clang wasm32) and loaded over Wi-Fi: the device runs
only the `.wasm`.

## Layout

```
apps/<id>/manifest.json   { "id":"myapp", "name":"My App", "version":"1.0", "entry":"run",
                            "abi":2, "ram_budget":65536, "stack_kb":16, "timeout_ms":120000,
                            "permissions":["gfx"], "canvas_w":1024, "canvas_h":600 }
apps/<id>/main.c          immediate-mode loop (this shape is REQUIRED, see below)
apps/<id>/img/*.565       optional RGB565 assets (tools/build_abc_assets.py)
apps/<id>/snd/*.wav       optional SFX, 48 kHz mono 16-bit
apps/<id>/icon.argb       optional launcher icon, 80×80 ARGB8888 (tools/make_app_icon.py)
```

## Skeleton main.c

```c
#include "nucleo_sdk.h"
NV_EXPORT("run")
void run(void) {
    int W = nv_gfx_width(), H = nv_gfx_height();
    int redraw = 2, prev_down = 0;
    while (nv_gfx_present()) {                 // frame pacing; 0 = the OS is closing the app
        int x, y, down = nv_touch(&x, &y);
        int tap = (!down && prev_down); prev_down = down;
        if (nv_gfx_back()) break;              // OS back gesture, at your root screen
        if (tap) { /* handle the tap */ redraw = 2; }
        if (redraw > 0) { /* redraw EVERYTHING */ redraw--; }  // 2 frames: double buffer
    }
}
```

## ABI (module "nv", permission "gfx")

- Frame: `nv_gfx_present` `nv_gfx_width/height` `nv_gfx_clear(col)` `nv_gfx_back`
- Draw: `nv_gfx_rect` `nv_gfx_circle` `nv_gfx_line` `nv_gfx_tri` `nv_gfx_text(x,y,s,col,scale)` `nv_gfx_image(name,x,y,w,h)` `nv_gfx_blit`
- Input: `nv_touch(&x,&y)` → pressed 1/0 (the TAP is the release edge); multi-touch since ABI v3: `nv_gfx_touch_count/_point`
- Audio: `nv_gfx_tone(hz,ms)` `nv_sound(name)` (WAV in snd/) `nv_speak(text,lang)` (offline voice)
- ABI v4 (`"abi":4`): `nv_gfx_text_width(s,scale)` + helper `nv_gfx_text_center(y,s,col,scale)`; `nv_backlight(0..100)` (flashlight; the OS restores the user's brightness on exit)
- ABI v5 (`"abi":5`, permission `net`): UDP LAN — `nv_net_open/close/send/bcast/recv/from_ip/from_port/ip` (IPs are opaque tokens, you just pass them back; one socket per app, closed on exit). Foundation for multiplayer.
- **ABI v6 (`"abi":6`) — dirty-rect engine** (the key to high FPS on the P4, where PSRAM bandwidth is the bottleneck): call `nv_gfx_persist(1)` once → the OS keeps ONE persistent buffer (no swap/clear) and **re-blits only the pixels you draw** (auto-tracked, PPA flush). Pattern: draw the static scene once → `nv_gfx_bg_save()`; each frame **erase** the moving objects with `nv_gfx_bg_restore(x,y,w,h)` (copies the saved background back) and redraw them. A full-screen repaint (bandwidth-bound, ~2 fps) becomes a few small blits. Reference: `apps/tanks` (static scene + barrel/trajectory/projectile overlay). Why it is needed: the 1024×600 framebuffer lives in PSRAM, so redrawing everything every frame does not keep up; serious games use dirty-rect/sprites + 2D hardware (PPA).
- ABI v7 (`"abi":7`, no permission): opening files — `nv_open_path/size/read`, see below.
- **ABI v9 (`"abi":9`) — Vertice 3D engine** (`nv.vx_*`): a 3D scene rendered on both cores into the canvas, 2D on top. Manifest `"requires": {"vertice": "1.0"}`, `canvas_scale` `fit`/`stretch`/`zoom`. Input `nv_gfx_pad()` (keyboard + gamepad, SNES mask). Dependencies between packages (`requires`, `"kind": "library"`, assets `"<id>:<name>"`). All in [VERTICE.md](VERTICE.md); example `apps/vxgp`.
- ABI v10 (`"abi":10`, permission `gfx`) — raw audio stream: the app generates 16-bit signed PCM itself and feeds the speaker (synths, trackers, emulators). `nv_audio_open(rate, channels)` (rate 8000..48000, 1 or 2 channels; 1 = ok, 0 = speaker busy or unavailable), `nv_audio_write(pcm, bytes)`, `nv_audio_backlog()` (write only while the queued bytes are under your latency target), `nv_audio_close()`. One stream per run, closed by the OS when the app exits.
- **ABI v11 (`"abi":11`) — controllers** (`nv.pad_*`, permission `gfx`): every controller is a separate player, already in the standard Xbox layout (A B X Y, LB RB, analog LT RT, two sticks, Back/Start/Guide) whatever the model. Generic USB HID mapped with SDL_GameControllerDB (~1000 models + heuristic + user lines in `/sdcard/data/pads.txt`, same format as `gamecontrollerdb.txt`), USB XInput (Xbox 360/One/Series and clones), Switch Pro and NSO over USB, DualShock 4 / DualSense over USB (rumble + light bar), Bluetooth LE HID (Xbox Series/One S, 8BitDo, Stadia...). `nv_pad_count()`, `nv_pad_state(i, &st, sizeof st)`, `nv_pad_name(i, buf, len)`, `nv_pad_rumble(i, low, high, ms)`; the `nv_pad_state_t` struct is in `nucleo_sdk.h`. `nv_gfx_pad()` keeps working and merges all controllers. Bluetooth Classic pads (DualShock 4, DualSense, Switch Pro, Joy-Con) work only over a cable: the C6 is BLE-only.
- ABI v12 (`"abi":12`) — network (`http_*`, `ws_*`, `mqtt_*`, `ha_*`): see "Networking for apps (ABI v12)" below. ABI v13 adds `nv_mdns_browse(service, proto)`: mDNS discovery on the home network (permission `lan`), an HTTP-style handle whose body has one line per device, `instance|hostname|ipv4|port|key=value;key=value`, ~2.5 s.
- ABI v14 (`"abi":14`, permission `gfx`) — raw keyboard + mouse, for games, emulators and ports that need real keys (Ctrl, Alt, digits, F-keys) or mouse motion rather than the SNES-style `nv_gfx_pad`; USB and Bluetooth devices are merged. `nv_kbd_state(buf, len)` (`buf[0]` = HID modifier bits, `buf[1..6]` = HID usages of the keys held now; returns how many usages were written, -1 = no keyboard), `nv_mouse_read(&m, len)` (`nv_mouse_t`: motion since the previous call plus the buttons held; 1 = mouse present, 0 = none; the first call hides the OS pointer and stops it clicking the UI for the rest of the run, touch keeps working). Manifest: `"engine": "<id>"` runs package `<id>`'s module for this package (list it in `requires` too); `"args": ["..."]` = argv[1..] of a WASI app.
- **ABI v15 (`"abi":15`) — 2D UI and sound** (permission `gfx`): `nv_gfx_sprite(name, sx, sy, sw, sh, x, y, w, h, tint)` draws one cell of an img/ image (sprite sheet, font atlas, icons: one asset, one call per cell; `tint` is an RGB565 that multiplies each channel, 0xFFFF = as painted) — real fonts with outlines instead of the 5x7; `nv_gfx_panel(x, y, w, h, radius, color_top, color_bottom, alpha)` = rounded rectangle with a vertical gradient and transparency (panels, buttons, bars, translucent HUD) in one call. **Audio mixer**: `nv_snd_play(name, vol, pitch, NV_SND_LOOP|NV_SND_STREAM)` → voice (up to 12 at once: effects, loops, music read from the SD while it plays), `nv_snd_set(voice, vol, pitch)` to follow it from the game (engine, reel), `nv_snd_stop(voice, fade_ms)`, `nv_snd_preload`, `nv_snd_master`; the mixer runs in its own task, detached from the game loop (a slow frame never makes the audio crackle). From ABI 15 `nv_sound` also goes through the mixer (sounds overlap instead of cutting each other off). Full example: `apps/bass`.
- ABI v16 (`"abi":16`) — Vertice 1.5 (`"requires": {"vertice": "1.5"}`): `vx_project` (world point → canvas pixel with the camera of the last `vx_render`, for a HUD over the 3D frame), `vx_texture_size`, `vx_obj_get_pos`, `vx_obj_fade`/`vx_obj_appear` (distance fades), `vx_emitter_clear`, `vx_config`, `vx_obj_material`, `vx_obj_alpha`, `vx_mat_set`, `vx_obj_parent`, `vx_obj_shadow`. Details in [VERTICE.md](VERTICE.md).
- ABI v17 (`"abi":17`) — full-screen terminal programs (`"tty": true`): see "Full-screen programs (curses, ABI 17)" below.
- ABI v18 (`"abi":18`, permission `net`) — system downloader: big files in the background, checked piece by piece and resumed after a dropped connection or a restart; a download goes on after the app closes (the OS posts a notification at the end, a tap opens the app). `nv_dl_start(spec_json)` (a `meta4` Metalink 4, or a plain `url` with the exact `size` and `sha256`; returns an id > 0), `nv_dl_info(id, buf, len)`, `nv_dl_list(buf, len)`, `nv_dl_pause(id, on)`, `nv_dl_cancel(id)`, `nv_dl_free()` (MiB a new download may use). The file lands where the app sees it (`home`: the shared workspace, else `fs`: its data folder) as `<name>.part` until it is complete and checked, and never over an existing file. An app only sees its own downloads.
- **ABI v19 (`"abi":19`) — rotated canvas** (permission `gfx`, only with `canvas_scale` `fit`/`stretch`/`zoom`): `nv_gfx_set_rotation(q)` (q = quarter turns 0..3) makes the PPA rotate the canvas while it scales it to the panel, at no cost to the app (rotating pixels in WASM costs ~95 ms per full frame, a PSRAM bandwidth limit). With an odd q the sides swap: a 1024×600 canvas becomes 600×1024 (`nv_gfx_width/height` report it), the app draws upright, and the device held with its left edge up shows it upright, like the system's portrait mode. Canvas pixel (x,y) of a W×H canvas lands on the panel at q=1 (y, W-1-x), q=2 (W-1-x, H-1-y), q=3 (H-1-y, x). Touch, mouse and remote taps arrive in upright canvas pixels. The canvas is black after a change: redraw everything. Every run starts at 0; the manifest always declares the landscape canvas (≤ 1024×600). Returns q, or -1 if refused (no `gfx`, a 1:1 canvas, WASM-4 cartridges, Vertice already bound to the canvas). Board test: `tests/apps/rotest` + `tools/hil/rotation.py`.
- **ABI v19 — hardware JPEG decoder** (permission `gfx`): `nv_jpeg_decode(jpg, len, out, cap, w, h)` decodes a baseline JPEG (8-bit, grey or YCbCr 4:4:4/4:2:2/4:2:0, up to 4096 per side) with the P4 decoder and scales it down (area average, CPU) to exactly w×h in `out` (RGB565 in canvas order, rows of w pixels, `cap` ≥ w·h·2, `out` 2-byte aligned). Returns 0, or -1 no `gfx`, -2 bad arguments, -3 not decodable in hardware (progressive, 12-bit, CMYK...), -4 memory, -5 corrupt file: in those cases the app uses its own software decoder. `nv_jpeg_info(jpg, len, &w, &h)` tells you whether the hardware takes it, without decoding. Board test: `tests/apps/jpgtest` + `tools/hil/jpeg.py`.
- State: `nv_save/nv_load(name,buf,len)` ≤8 KB; `nv_millis` `nv_rand` `nv_lang`
- `NV_RGB(r,g,b)` → RGB565. 5×7 font: ` 0-9 A-Z - . : % / < > ! + x ' , = ? ( ) # *`, advance 6*scale.

## Opening files (ABI v7)

An app can have files opened with it: it appears in **Files → Open with** and among the **default apps**
(Settings → Default apps), next to the system viewers. Two optional manifest fields:

- `"opens"`: the MIME types the app opens — `type/subtype` or `type/*` (lowercase, `[a-z0-9.+-]`, max 8;
  `*/*` is forbidden). Malformed patterns are discarded with a log line, not truncated.
- `"file_types"`: up to 4 new extensions the app teaches the OS — `ext` `[a-z0-9]{1,11}` without the
  dot, `mime` without `*`, `kind` one of `text image audio video app archive other` (default `other`).
  An already known extension keeps its system type.

```json
{ "id":"mdview", "name":"MD Viewer", "version":"1.0", "entry":"run", "abi":7,
  "ram_budget":131072, "permissions":["gfx","log"], "canvas_w":1024, "canvas_h":600,
  "opens":["text/plain", "text/markdown", "application/x-mdnote"],
  "file_types":[ { "ext":"mdn", "mime":"application/x-mdnote", "kind":"text" } ] }
```

**Grant model.** There is no `fs` permission: the user's choice ("open THIS file with THIS app") is
the permission. The app reads **only that file, read-only** — no other path is reachable. When it is
launched from Home, `nv_open_path` returns 0 and the others return -1. The grant ends when the run
ends (and lapses if the SD is remounted).

- `nv_open_path(buf,len)` → absolute path (truncated to `len`, always NUL-terminated); returns the full length, 0 = no file
- `nv_open_size()` → bytes; -1 if no file / unreadable
- `nv_open_read(off,buf,len)` → bytes read (max 64 KB per call), 0 at EOF, -1 on error. Read in
  chunks: the file does not have to fit in the 64 KB of linear memory.

```c
#include "nucleo_sdk.h"
static char chunk[4096];
NV_EXPORT("run")
void run(void) {
    char path[256];                                 // granted paths are < 256 bytes
    if (nv_open_path(path, sizeof path) == 0) { /* launched from Home: normal UI */ return; }
    int32_t size = nv_open_size(), off = 0, n;
    while ((n = nv_open_read(off, chunk, sizeof chunk)) > 0) {
        /* process chunk[0..n) */
        off += n;
    }
    if (n < 0) nv_print("read error");
    nv_printf("%s: %d/%d bytes", path, off, size);
}
```

gfx apps start on the file right away; console apps show their card and the file is granted when Run
is pressed. Associations are registered at boot together with the tiles (an app just installed
from the store appears in *Open with* after a restart); uninstalling it removes it immediately. Every
call reopens the file (safe with hot-swapping the SD): prefer 4-64 KB chunks over a thousand reads of
a few bytes.

## Golden rules (lag = number of draw calls per frame)

1. **Frame-skip**: a still screen = ~0 host calls. Redraw for 2 frames only when something changes.
2. **No floats in hot paths** (the interpreter is slow on floats): fixed-point + sine LUT (abc123 `SINQ`).
3. **Few primitives**: a `.565` image = 1 call; a gradient = N rects = N calls. Flat is better.
4. **Sequenced audio**: voice and SFX must not overlap — an app-side timeline clock
   (`g_speak_until`, `schedule_sfx`; see abc123). Numbers for nv_speak = WORDS ("TRE", not "3").
5. **Big buffers inside the 64 KB of linear memory**, not huge static arrays.

## Build + push (PC)

```powershell
.\.claude\skills\wasm-app\scripts\build_push.ps1 -AppDir apps\myapp          # build+push wasm+manifest
.\.claude\skills\wasm-app\scripts\build_push.ps1 -AppDir apps\myapp -Assets  # also img/ and snd/
```
(clang `--target=wasm32 -mcpu=mvp -O2 -ffreestanding -nostdlib`, 64 KB linear memory, 8 KB stack;
upload `curl --data-binary` to `POST /api/fs/write?path=/sdcard/apps/<id>/...`.)
A running instance keeps the OLD wasm: reopen the app. Debugging without a cable: `GET /api/screen`,
`GET /api/logs`, `POST /api/app/run?id=<id>`.

## Distribution: remote store (Wi-Fi)

Besides the dev push, apps can be installed from a **remote server** with no cable and no reflash.

- **Server** (`server/appstore/`, Python stdlib only): `python appstore_server.py` serves the repo's
  `apps/` folder on `:8090`. It exposes `GET /store.json?lang=&region=` (a **categorized,
  multilingual, geolocated** catalog) and `GET /apps/<id>/{manifest.json,app.wasm,icon.argb}`. Curated
  store metadata lives in `server/appstore/catalog.json` (category, per-language names/descriptions,
  featured, rating, `regions[]`) and is merged on top of the manifests. See `server/appstore/README.md`.
- **Device**: Settings → System update → *App store* = `http://<PC>:8090` → SAVE STORE URL, and pick the
  *Region*. Then Apps → **Store** tab: category chips (All/Featured/Games/Kids…), cards with
  rating + localized description, Install/Update. The UI language (`nv_lang`) and the region go in
  the query string → the store answers localized and filtered. The module goes to `/sdcard/apps/<id>/`
  (Home tile after reboot/scan). Firmware: `components/nv_appstore` + strings in `nv_i18n`.
- The device validates the wasm magic + a 2 MB cap before writing; WAMR isolates the guest and limits
  its permissions. Optional manifest fields for the store: `author`, `description`.
- **Signature (firmware ≥ 1.1.132)**: every store app has `apps/<id>/package.sig` (sha256 + size of
  each file, signed with the STORE key — not the OTA one — in `%USERPROFILE%\.nucleo\store-signing-key.pem`).
  `export_static.py` and `appstore_server.py` generate it on their own; `python tools/store_sign.py
  verify <dir>` checks it. The device downloads everything into `.tmp`, verifies, and only at the end
  renames (manifest last). Unsigned apps are rejected, unless Settings → Security →
  *Allow unsigned store apps (developer)* is on. The dev push (`push_app.ps1`) does not go through
  the store.
- **Sensitive permissions** (`net`, `lan`, `ws`, `mqtt`, `ha`, `fs`, `camera`, `mic`): the catalog
  carries them in `perms`, the app's card lists them and installation asks for a second tap
  ("Accept and install"); an update that adds some highlights them. Revoke per app in
  Settings → Security → *App permissions* (`/sdcard/nucleos/perms.json`).

## Networking for apps (ABI v12)

`"abi": 12`. Nothing blocks: every request returns a handle, and the app polls it from its loop.
Permissions: `net` = public Internet, `lan` = home network (private IPs, `*.local`), `ws` = WebSocket,
`mqtt` = system broker (Settings → Home), `ha` = Home Assistant with the system token
(the app never sees it). The host resolves the name and classifies the IP before connecting, and
redirects are not followed. Details and error codes in `nucleo_sdk.h` (ABI v12 section); full
example `apps/net12` + `tools/net12_server.py`.

```c
char spec[160];
nv_snprintf(spec, sizeof spec, "{\"url\":\"http://%s/rpc/Switch.Set?id=0&on=true\"}", shelly_ip);
int h = nv_http_req(spec, 0, 0);                 // returns a handle at once
... in the loop: if (nv_http_state(h) == 1) { n = nv_http_read(h, buf, sizeof buf); nv_http_close(h); }
int ha = nv_ha_req("POST", "/api/services/light/toggle", "{\"entity_id\":\"light.cucina\"}", 29);
nv_mqtt_sub("zigbee2mqtt/+/state");  ...  n = nv_mqtt_recv(topic, sizeof topic, buf, sizeof buf);
```

## Terminal programs (WASI, ABI 8)

WASI apps with `"console": true` in the manifest: launched by the **Terminal** (`lua`, `js file.js`,
`sqlite3 note.db`: first word = app id, the rest = argv); the Home tile opens a Terminal that
runs them. stdin = typed lines (EOF button = end of input, STOP = kill), stdout/stderr go to the
scrollback (plain text, ANSI escapes discarded). No timeout and no opcode cap. The `home` permission =
`/sdcard/home` as `/`. Ready-made ports in `ports/` (Lua 5.4, QuickJS-ng, SQLite): `bash ports/build.sh`,
PC tests with `bash ports/test.sh`; in the Store they are in the **Terminal** category. Lua uses
`nv.try_call`/`nv.throw` in place of setjmp/longjmp (`ports/common/nv_sjlj.h`).

### Full-screen programs (curses, ABI 17)

With `"tty": true` as well, the program owns the screen (sc-im, WordGrinder): it gets
`TERM=xterm-256color`, `LANG=C.UTF-8`, and the grid's `COLUMNS`/`LINES`; on the alternate screen the
Terminal is raw (keys as xterm sequences) and does not translate the output; `poll()`/`select()` on
stdin wait with a timeout (ESC vs arrow keys); a grid change (on-screen keyboard, rotation) arrives
in-band on stdin as in xterm mode 2048 (`CSI 48;rows;cols;h;w t`); if the program asks for the
mouse, a touch is a click and a vertical drag is the wheel. The Home tile stays for system apps too;
when opened from Files with "Open with", the program receives the path and sees the file's folder.
Program side: ncurses 6.5 and the `ports/tui/compat` shim (termios, resize → `SIGWINCH`,
`TIOCGWINSZ`, local time): see `ports/README.md`, section *Full-screen programs*.

## "Done" checklist

1. Compiles clean with `-Wall -Wextra`. 2. Idle ≈ 0 draw calls. 3. Animations with a time window.
4. Sequenced audio. 5. Pushed with curl and sizes verified. 6. Reopen the app on the device.
