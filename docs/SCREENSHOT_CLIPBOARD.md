# Screenshot tool + system clipboard — integration plan

Goal: a Lightshot / Win+Shift+S style capture on the device — freeze the screen, drag a region,
the image lands on a **system-wide clipboard** — plus a Windows-like expandable tray that hosts it.

## What exists today (analysis)

| Area | Today | Where |
|---|---|---|
| Full-screen capture | `nv_hal_screenshot(path)`: front framebuffer (always physical 1024x600 RGB565) -> HW JPEG q85 -> SD. No region, no RAM result. | `nv_hal/nv_hal.cpp:454` |
| PPA crop/scale of the front buffer | `nv_hal_thumbnail_grab` (Recents cards): the template for a direct crop | `nv_hal/nv_hal.cpp:548` |
| Screenshot entry points | PrtSc (global hotkey), shade button, shell `screenshot`, `/api/screen` | `nv_ui.cpp:4640`, `:1127`, `term_sh.cpp:3741`, `nv_web.cpp:2576` |
| Device clipboard | **text only**, one string inside the IME (`s_clip`), Ctrl+C/X/V/A, right-click edit menu | `nv_ui/nv_ime.cpp:482-606`, `nv_ui_classic.cpp:1723` |
| Web shell clipboard | history of 20 items, kinds `text` / `files`, persisted `/system/config/clipboard.json`, Ctrl+Shift+V history | `sd/web/shell.js:171-351` |
| Files app | its own private copy/move "clipboard" | `files_app.cpp:86-95, 466` |
| ANIMA image input | path-based `nucleo_anima_attach_image` (jpg/png, 2 MB) | `nucleo_anima_online.c:1680` |
| Tray | flat row of icons on `lv_layer_top`, one popup slot `S.menu` + scrim, `tray_popup`/`tray_popup_place` | `nv_ui_classic.cpp:1303-1594` |
| Global hotkeys | one block in `ui_kbd_nav` (Win+E/I/L/D, Alt+F4, PrtSc...) | `nv_ui.cpp:4616-4660` |
| Event bus | fixed enum, add `NV_EV_*` before `NV_EV__COUNT` | `nv_kernel/include/nv_event_bus.h` |

Gaps: the two clipboards (device / web) are separate and neither holds images; captures cannot be
cropped; the tray cannot grow.

## Design

### 1. `nv_clipboard` — one system clipboard (nv_kernel)

Lives in **nv_kernel** (no LVGL): nv_ui, nv_apps, nv_web, nv_wasm all already depend on it, and it is
safe from any task (httpd, workers). Mutex-guarded, PSRAM.

```c
typedef enum { NV_CLIP_NONE, NV_CLIP_TEXT, NV_CLIP_IMAGE, NV_CLIP_FILES } nv_clip_kind_t;
bool nv_clip_set_text(const char *utf8, const char *source_app);
bool nv_clip_set_image_rgb565(const uint16_t *px, int w, int h, const char *source_app); // copied
bool nv_clip_set_files(const char *const *paths, int n, bool cut);
nv_clip_kind_t nv_clip_kind(void);
char *nv_clip_get_text(void);                 // malloc'd copy, caller frees
bool nv_clip_get_image(nv_clip_image_t *out); // ref-counted read: px, w, h; nv_clip_release()
bool nv_clip_image_file(char *path, size_t n);// lazily encoded PNG/JPG on SD for path-based consumers
uint32_t nv_clip_seq(void);                   // bumps on every change
int  nv_clip_history(nv_clip_entry_t *out, int max);  // last 20, text + image thumbnails
```

- Every change publishes **`NV_EV_CLIPBOARD`** (new event) -> UIs refresh paste state.
- History: 20 entries like the web shell (text <= 8 KB each, images kept as thumbnails + file).
- **Persistence and web sync**: same schema as the shell's `/system/config/clipboard.json`
  (add kind `image` with a file path), plus `GET/POST /api/clipboard` in nv_web, so copying on the
  device and pasting in the browser (and the reverse) just works.
- The IME keeps its API (`nv_ime_edit`), reimplemented over `nv_clip_*` (text kind); paste is
  offered only when the clipboard holds text, so an image never pastes into a text field as garbage.

### 2. Capture service (nv_hal + nv_ui)

- `nv_hal_screen_freeze(nv_frame_t *)`: front buffer -> 64-byte-aligned PSRAM copy (1.2 MB) via
  `nv_2d_copy`, lock released at once; rotation undone (PPA SRM) so the copy matches what the user sees.
- `nv_hal_crop(frame, x, y, w, h, dst)`: PPA SRM block offset/size (scale 1.0), CPU memcpy fallback.
- Encoding off the LVGL thread (worker task): JPEG via `nv_2d_jpeg_encode` (height padded to 16),
  PNG via lodepng (RGB565 -> RGB888) for lossless text/UI shots. Default PNG for regions, JPEG for
  full-screen.
- One place for file names: `~/shots/shot-YYYYmmdd-HHMMSS.png` (today two folders are used:
  `/sdcard/Screenshots` and `~/shots`; unify on `~/shots`, keep the old folder readable).

### 3. Region selection overlay (nv_ui, `nv_capture.cpp`)

A shell overlay on `lv_layer_top` (not an NvApp: opening an app would tear down the screen being
captured), like the lock screen / Recents:

1. Freeze -> show the frozen frame as an `lv_image` (RGB565 dsc), dimmed 50%.
2. Drag (touch or mouse) -> the selection shows undimmed, with a 1 px accent border, 8 handles and a
   `W x H` label; handles move/resize it; tap without drag = whole screen; Enter = confirm,
   Esc = cancel (via the existing `kbd_escape` chain: a full-size CLICKABLE barrier).
3. A small toolbar next to the selection (Lightshot): **Copy** (default, Ctrl+C), **Save**
   (Ctrl+S), **Ask ANIMA**, **Cancel**. Phase 2: pen / arrow / rectangle / highlighter / text.
4. Result: image on the clipboard + notification with a thumbnail ("Copied to clipboard"),
   tapping it opens Save / Ask ANIMA.

Entry points (all the same `nv_capture_start(mode)`): tray icon, **Win+Shift+S** (new hotkey),
**PrtSc** (region mode; Alt+PrtSc = full screen to clipboard), shade button, shell
`screenshot -r` / `-c`.

### 4. Expandable tray (nv_ui_classic)

- A `^` chevron at the left of the tray opens a flyout grid (Windows 11 style) with the
  "overflow" icons; it reuses `tray_popup` / `tray_popup_place` / the `S.menu` scrim, so dismissal,
  Esc and focus already work.
- Registry instead of hard-coded icons: `nv_tray_item_t {id, symbol, tooltip, on_click, pinned}`;
  pinned items stay on the bar, the rest go into the flyout; pin/unpin by right-click, saved in
  nv_config (`tray.pinned`).
- First items: Screenshot (pinned by default), Clipboard history, USB/SD (already dynamic), Bluetooth.

### 5. Paste where it makes sense

| Target | Paste behaviour |
|---|---|
| Text fields (IME) | text kind only |
| ANIMA | Ctrl+V / paste button: image -> `nucleo_anima_attach_image(nv_clip_image_file())`, thumbnail in chat |
| Files | Ctrl+V with an image -> `Pasted image YYYYmmdd-HHMMSS.png` in the open folder; text -> `.txt`; files kind -> its existing copy/move (moved onto `nv_clip_set_files`) |
| Gallery | "Copy image" in the viewer; paste saves into the gallery |
| Web shell / web apps | through `/api/clipboard` + the shell's existing postMessage protocol (`clipboard-write/read`), new kind `image` |
| Clipboard history | Win+V flyout (tray item too): last 20, tap to re-copy |

## Status

| Phase | State |
|---|---|
| 1 Clipboard (`nv_kernel/nv_clipboard.*`, `NV_EV_CLIPBOARD`, IME on top) | done, verified on device |
| 2 Capture (`nv_hal_screen_freeze`, `nv_hal_jpeg_save_rgb565`, `nv_ui/nv_capture.*`; PrtSc, Win+Shift+S, Alt+PrtSc, shade) | done, verified on device |
| 3 Tray tools (`^` flyout, pinnable Screenshot / Clipboard, `tray.pinned`), Win+V history | built |
| 4 Paste: ANIMA (Ctrl+V, "Ask ANIMA"), Files (image -> .jpg, text -> .txt) | ANIMA verified; Files built |
| 5 Web sync (`/api/clipboard`, kind `image`) | todo |
| 6 Annotation tools, resize handles, rotated screens | todo |

Notes from the implementation: regions are saved as JPEG (HW encoder, YUV444, q92, width rounded
down to 8 px) — LVGL's bundled lodepng allocates from LVGL's 320 KB pool and cannot encode a
screen-sized PNG. A rotated display falls back to a full-screen capture.

## Phases (each one shippable and testable on its own)

1. `nv_clipboard` in nv_kernel + `NV_EV_CLIPBOARD`; IME text moved onto it (no visible change).
2. Capture service (freeze, crop, encode) + region overlay + Copy/Save/Cancel; Win+Shift+S, PrtSc.
3. Expandable tray with the item registry + Screenshot and Clipboard-history items.
4. Paste targets: ANIMA, Files, Gallery; Win+V history flyout.
5. `/api/clipboard` + web shell sync (kind `image`).
6. Annotation tools in the overlay.

## Risks / rules to respect

- PPA and JPEG share 2D-DMA channel 0: only through `nv_2d_*` wrappers (ENGINEERING_RULES.md:128).
- No `esp_lcd_dpi_panel_get_frame_buffer()` outside nv_disp (ENGINEERING_RULES.md:141): use
  `nv_disp_front_begin/end`.
- Event-bus callbacks may run off the LVGL thread: set a flag, let the UI tick act.
- `kMaxSubs = 8` per event: few, shell-level subscribers for `NV_EV_CLIPBOARD`.
- Memory: frozen frame 1.2 MB + crop + encoder scratch ~2.5 MB transient, all PSRAM (16+ MB free).
- Rotation: the framebuffer is always physical landscape; the overlay works in logical
  coordinates and maps them back.
