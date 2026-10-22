# Plan: "Classic 95" interface, mouse and keyboard

Status: v3 (2026-10-01). Written, compiled and committed; NOT yet tested on HW.
- Phases 1-2 (input, keyboard navigation): done, app audit resolved.
- Phases 3-6 (Classic shell) first version: `nv_ui_classic.cpp` (desktop, taskbar, Start, context
  menus, title bar), integration in `nv_ui.cpp` (per-shell geometry, `shell_apply`,
  `shell_tick` with hysteresis), Settings > Display: "Classic desktop" + "Automatic with mouse and
  keyboard". NucleoOS logo (crystal, from the Cardputer edition, web/shell/icon.png) via
  `tools/gen_logo.py`. Windows: always maximized, can be minimized to an icon (app alive, hidden) and
  closed.
- Desktop wallpapers generated locally (`tools/gen_desk_wallpaper.py`, Qwen-Image 2.1).

## 1. Goal and decisions

- **Settings > Display > Interface**: `Tablet` / `Classic` / `Automatic`
  (`nv_config` key `ui_shell` = 0/1/2, default 0).
- **Automatic**: mouse **and** keyboard present (USB or BLE) → Classic, otherwise Tablet.
- **Classic** = desktop with icons, taskbar, Start menu, tray; apps always **maximized**
  with a title bar.
- Mouse and keyboard become first-class input **in both shells**.

Out of scope, decided:

| Excluded | Why |
|---|---|
| Overlapping / resizable windows / multiple live apps | solo-mode with `nv_mem_request`; bottleneck = PSRAM bandwidth; scaled WASM games write over the whole panel |
| Classic in portrait | taskbar + menus designed for 1024×600; rotation disabled in Classic (the saved one returns on exit) |
| Restyling apps one by one | apps use the theme tokens; camera/gallery/terminal (fixed colors) stay dark, they are full screen |

## 2. What the code says

### 2.1 Shell (`components/nv_ui/nv_ui.cpp`, 4575 lines)
- A single screen. Children: launcher → status bar → shade → gesture strips → IME keyboard;
  then app plane, search, folders. `lv_layer_top`: Recent apps, toast, lock, PIN, wake-catcher,
  pairing. `lv_layer_sys`: cursor only.
- Home = launcher shown/hidden (`open_app` :1485, `close_app` :1634), not a screen change.
- **Coupling surface**: ~30 functions outside the builders touch launcher/status/
  shade/dock/Recent apps; lines ~1640-3470 are launcher only. Natural seams: `open_app`,
  `close_app`, gesture strip callbacks, `ui_refresh_async` (:3374), `nv_ui_go_home` (:3663).
- Nothing is fixed at 1024/600 (all `LV_HOR_RES`), except header/status/inset (`kStatusH` 34,
  `kHeaderH` 44, `kHomeInset` 24, duplicated in `nv_gesture.cpp:19`).
- `NV_EV_APP_LIFECYCLE` exists but nobody publishes it → the taskbar can use it.
- Rotation: chip in quick settings (:950), full rebuild; 1024×600 RGB565 wallpaper
  in PSRAM (1.2 MB, +1.2 MB portrait copy).
- Icons: 80×80 ARGB8888 in PSRAM; `icon_scaled()` (:511) does quality resizing with a PSRAM cache
  of **96 slots** (dock 56, search 50, folders 30).

### 2.2 Gesture strips (`nv_gesture.cpp`)
24 px clickable objects on the top, left, bottom edges: they **steal clicks** on the edge.
In Classic they would cover the taskbar (bottom), Start button and icons (left), title bar and
`X` (top). → in Classic **all off**; navigation goes through taskbar/title/keyboard.

### 2.3 Tap guard and indev
`tap_guard_cb` (:2489) is attached to **every** pointer indev (the mouse too): it cancels the click
if the pointer moved > 20 px or there is a gesture. **Global** state (`s_press_pt`,
`s_touch_claimed`), shared among touch, mouse and automation indevs → possible
interference. To be made per-indev.

### 2.4 Mouse (`nv_hid_host.cpp:50-92`)
- POINTER indev + 14 px blue dot on `layer_sys`. Only the left button goes to LVGL.
- Right/middle **read but discarded** (`s_mbuttons`, :83); wheel accumulated in
  `s_acc_wheel` but given only to games.
- Cursor not hidden on detach.
- **LVGL 9.5 handles the wheel on the pointer**: `data->enc_diff` → vertical scroll
  of the object under the cursor (`lv_indev.c:767`, :1620-1660). Just pass it.
- **Hover already supported** (`LV_STATE_HOVERED`, `lv_indev.c:1455`) if the read_cb reports
  position with RELEASED state during movement.
- Right click: **does not exist in LVGL** → handled by us (`lv_indev_search_obj` + custom event).

### 2.5 Keyboard (`nv_hid_host.cpp:96-164`, `nv_bt.c:1235`)
- USB and BLE go through the same `keyboard_report`.
- Only Shift and Ctrl are read; **Alt and Win ignored** (but the byte is in `s_mods`).
- **No release events, no auto-repeat.**
- **US layout only**: no Italian, no AltGr (`@ # [ ] è à ù` wrong).
- Mapped: Enter, Esc, Backspace, Tab, Del, arrows, Home, End. Missing PgUp/PgDn, F1-F12, Ins.
- Runs in the `hid_host` task and takes `lvgl_port_lock(50)`: if it expires, **the key is lost**.
- Without an active text field **every key is discarded**. No `lv_group` in the project.

### 2.6 IME (`nv_ime.cpp:306`)
The on-screen keyboard comes up on every text field **even with a physical keyboard connected**
(no `keyboard_present` check). It takes 42% of the screen. To be fixed in any case.

### 2.7 Focus in LVGL 9.5
- Keypad: arrows/Tab/Enter/Esc; Tab = focus next, Enter = CLICKED, Esc = `LV_EVENT_CANCEL`.
- Added to the default group automatically: button, checkbox, dropdown, slider, switch,
  textarea, roller, table, msgbox, buttonmatrix, list buttons.
- **Not** added: generic clickable `lv_obj`, `lv_win`, `lv_menu`.
- The textarea takes arrows and Enter (multiline); exits only with Tab. OK.
- In the apps: `lv_button_create` used 12 times in total; most controls are
  `lv_obj` + `LV_EVENT_CLICKED`. Estimate outside the group: ~13 in Settings, 8 Second Screen,
  8 Camera, 6 Recorder, 6 Video, 4 Gallery, 3 Music, plus launcher/shade/Recent apps.

### 2.8 Lock screen
PIN on a custom keypad (`build_pin_pad` :3886) in `layer_top`: **does not accept digits from the
physical keyboard**. To be connected.

### 2.9 Drawing
- No shadows/transforms/opa-layers (SW renderer hang). Verified in `lv_obj_style.c:1084`:
  **outline, partial border_side and `bg_grad` do not create layers** → usable.
- Bevel 95 = light `border_side` TOP|LEFT on an object + dark BOTTOM|RIGHT lines in
  `LV_EVENT_DRAW_POST` with `lv_draw_line` (no layer, no extra object).
- Dashed outline does not exist → classic focus with 4 `dash_width/gap` lines in DRAW_POST.
- Cursor: `lv_image` ARGB8888 (RGB565A8 is disabled in the config).

### 2.10 Budget
| Resource | Today | CI limit | Margin |
|---|---|---|---|
| Firmware / 4.5 MB OTA slot | 4 009 KB (85%) | 90% | ~238 KB |
| Static internal SRAM | 192.5 KB | 210 KB | ~17 KB |
| Internal .bss | 57.8 KB | 72 KB | ~14 KB |
| Static PSRAM | ~490 KB | 640 KB | ~150 KB |

Rule: every new static goes in `NV_PSRAM_BSS`, UI objects in the LVGL pool (already in PSRAM).
Goal for the whole project: < 2 KB internal SRAM, < 60 KB flash.

## 3. Architecture

```
 nv_hid_host (task hid_host)                      nv_config "ui_shell"
   keyboard_report ── layout IT/US, mods, repeat         │
        │  key event queue (no LVGL lock)                 │
        ▼                                                 ▼
 nv_ui_input (new)  ◄── NV_EV_INPUT_DEVICES ──►  nv_shell (new)
   ├ KEYPAD indev + lv_group stack                    decides the effective shell
   ├ global shortcuts                                 applies only in home / in close_app
   ├ keys → IME if a field is active                       │
   └ mouse: wheel, right button, cursor              ┌─────┴──────┐
                                                 shell_tablet  shell_classic
                                                 (current code) (nv_ui_classic.cpp)
                         NvShellOps + ShellGeom used by open_app/close_app/fullscreen/refresh
```

```c
typedef struct { int16_t top_h, bottom_h, hdr_h, inset; bool edge_gestures; } ShellGeom;

typedef struct {
    void (*build_home)(lv_obj_t *scr);
    void (*destroy_home)(void);
    void (*show_home)(bool show);
    void (*frame_build)(lv_obj_t *plane, const NvApp *a);   /* header or title bar */
    void (*on_app_opened)(const NvApp *a);
    void (*on_app_closed)(const NvApp *a);
    void (*go_home)(void);
    ShellGeom geom;
} NvShellOps;
```

New files (no code added to `nv_ui.cpp` beyond the seams):
`nv_ui_input.cpp`, `nv_shell.cpp`, `nv_ui_classic.cpp`, `nv_ui_classic_theme.cpp`.

## 4. Phases

Each phase: clean build, CI budgets green, OTA, on-screen verification with `/api/screen`.

### Phase 1 — Input foundations (useful right away, even without Classic)

| # | Work | Where |
|---|---|---|
| 1.1 | Key event queue (FreeRTOS, ~32 elements, PSRAM): `{key, mods, pressed}`; the HID task no longer takes the LVGL lock → no lost keys | `nv_hid_host.cpp` |
| 1.2 | Release events (diff `s_prev_keys`), software auto-repeat (500 ms / 33 ms) | `nv_hid_host.cpp` |
| 1.3 | Alt, AltGr, Win; PgUp/PgDn, F1-F12, Ins | `nv_hid_host.cpp` |
| 1.4 | **Italian layout** (+ US), selected in Settings > Language & Region > Physical keyboard; default from the system language | `nv_hid_host.cpp`, `settings_app.cpp` |
| 1.5 | `NV_EV_INPUT_DEVICES` published on every presence change (USB + BLE `ext_*`) | `nv_event_bus.h`, `nv_hid_host.cpp` |
| 1.6 | IME: with a physical keyboard present the on-screen keyboard **does not come up**; a button in the bar brings it back | `nv_ime.cpp` |
| 1.7 | Wheel → `enc_diff`; right/middle buttons exposed | `nv_hid_host.cpp` |
| 1.8 | 12×19 ARGB8888 arrow cursor (~1 KB), hidden on detach and when touch is used, shown again on movement | `nv_hid_host.cpp` |
| 1.9 | Per-indev tap guard (state in `lv_indev_get_driver_data`) | `nv_ui.cpp` |
| 1.10 | Lock screen PIN from keyboard (digits, Backspace, Enter) | `nv_ui.cpp` |
| 1.11 | `/api/ui/key` and `/api/ui/type` go through the queue (real navigation tests) | `nv_web.cpp` |

Acceptance: in Notes I write `perché @ [x]` on an Italian keyboard without the on-screen keyboard;
the wheel scrolls Settings; 20 keys per second without losses; hotplug 20× without errors.

### Phase 2 — Keyboard navigation (both shells)

1. **KEYPAD** indev in `nv_ui_input.cpp`, reads the queue. If an IME field is active,
   characters go to the field (as today), the rest to the group.
2. **Group stack**: push/pop in home, app, Start, Recent apps, search, folders, shade,
   "Open with" dialog, lock. Always pop in the overlay's `LV_EVENT_DELETE`.
3. `nv_ui_focusable(obj)`: adds a clickable `lv_obj` to the current group and maps
   Enter → CLICKED. Used inside the `nv_kit_*` and in the ~50 custom controls listed in 2.7
   (Settings rail, launcher, dock, shade chip, Recent apps, Files, Music, Gallery, Camera,
   Video, Recorder, Second Screen).
4. Focus style: Tablet = 2 px accent ring (outline); Classic = dashed rectangle.
   Visible only after keyboard use (disappears on the first touch/click).
5. Global shortcuts (intercepted before the group):

| Keys | Tablet | Classic |
|---|---|---|
| Win | home | opens/closes Start |
| Esc | back (`back_clicked`) | back |
| Alt+Tab | Recent apps, Tab cycles, Alt release opens | same on the taskbar |
| Alt+F4 | closes app | closes app |
| Win+E / Win+I | Files / Settings | same |
| Win+L | locks | locks |
| Ctrl+Alt+Del | System Monitor | same |
| PrtSc | screenshot | screenshot |
| F5 | — | refresh desktop |

6. WASM apps: unchanged (they read `nv_kbd_state`/`nv_mouse_read`, ABI 14). With input captured
   by WASM the LVGL keypad receives no keys; only Alt+F4 and Win remain (guaranteed exit).

Acceptance: Settings, Notes, Files, Calculator and Store usable without mouse or touch;
no focus lost after opening/closing 10 overlays in a row.

### Phase 3 — Shell refactor (no visible change)

1. `ShellGeom` replaces the constants in `open_app`, `nv_ui_app_fullscreen`,
   `ui_refresh_async`, `nv_gesture.cpp`.
2. Current launcher behind `NvShellOps` = `shell_tablet`.
3. `NV_EV_APP_LIFECYCLE` published in `open_app`/`close_app`.
4. Audit of apps with fixed heights: the app area goes from 1024×498 (Tablet) to 1024×548 (Classic);
   apps must use `lv_pct`/flex. Grep `lv_obj_set_height|set_size` with constants.

**Coordination**: `nv_ui.cpp` is clean now, but other sessions touch it. Phase 3 in one go,
immediate commit, notice to peers (`SendMessage`) before and after.

Acceptance: home/app/Recent apps/shade/rotation screenshots identical to before.

### Phase 4 — Classic style (shell only, NucleoOS identity)

Rule: Classic **recalls** Windows 95, but remains NucleoOS. Only the shell changes
(desktop, taskbar, Start, tray, window frame). **Apps are not touched**: no new palette
pushed into the tokens, no restyling, no redrawn icons.

1. Shell style: raised 95 borders (light/shadow, radius 0) on taskbar, taskbar buttons,
   Start menu, tray popups, window frame. Colors taken from the active NucleoOS theme (surfaces,
   text, **accent** for the active title bar and selection): the user's light/dark and accent
   remain valid in Classic too.
2. Bevel without shadows: `border_side` + lines in `LV_EVENT_DRAW_POST` (no layer).
3. Font: current Montserrat/nv_font. No bitmap font.
4. Icons: the existing ones (80 px, reduced with `icon_scaled`), no new set.
5. NucleoOS logo/brand in the Start button and in the menu's side band.

Acceptance: no hang/WDT; apps opened in Classic are identical to Tablet (only the frame
changes); launcher FPS does not get worse (`/api/display`).

### Phase 5 — Classic shell

**Desktop** (1024×570)
- Background: `wallpaper.jpg` or the current NucleoOS gradient (same PSRAM buffer).
- **48 px** icons (`icon_scaled`) in columns from top left, label below,
  blue selection. Tap = opens (touch); with the mouse: click selects, double click opens.
- The desktop shows: Resources (Files), Settings, Store, no Recycle Bin (it does not exist), plus the
  apps chosen by the user ("Add to desktop" from the Start menu). Key `cdesk` (id list).
- Context menu with the right button: Open, Remove from desktop, Uninstall (store apps), Properties
  (app card); on empty space: Arrange icons, Wallpaper, Display settings.
- Icon cache: take `icon_scaled` from 96 slots to LRU (or 160 slots) because desktop 48 +
  Start 24 double the sizes.

**Taskbar** (30 px, at the bottom)
- `Start` | open-app button (pressed) | up to 5 recent apps | tray.
- Tray: Wi-Fi (without SSID in the text: screenshot privacy), volume, USB/SD, notifications
  (counter), clock `HH:MM`.
- Tray popup = existing quick settings chips and notification list (reuse of
  `build_shade_content`, mounted in a 95 panel instead of the pull-down shade).

**Start menu** (in `lv_layer_top`)
- "NucleoOS" side band, items: Programs ▸ by category, Games ▸, Documents (Files),
  Settings, Find… (existing search, typing filters immediately), Guide (Store guide),
  Lock now, Restart device, Turn off screen.
- Cascading submenus: opened on hover (mouse), on right arrow (keyboard), on tap (touch).
- Closes with Esc, Win, click outside.

**App frame**
- 22 px title bar: 16 icon, name, `←` (if the app has a back handler), `_`, `X`.
- `_` = back to desktop; in solo-mode the app is closed and stays in the recent apps on the taskbar
  (honest: no live "minimized" apps).
- App area 1024×548. Fullscreen (games, video, camera) covers the taskbar as today.
- Exit from fullscreen: Alt+F4, Win, Esc held 1 s.

**Lock screen, "Open with" dialog, toast**: stay the current ones, with the classic theme.

Acceptance: all native apps opened and closed from Classic; a WASM game from Start
and exit with Alt+F4; language and theme change live; no gesture strip active.

### Phase 6 — Selection and automatic mode

1. Settings > Display > "Interface": 3 pills (pattern `sleep_pick_cb` :396) +
   status row "Mouse ✓ Keyboard ✗".
2. `nv_shell` listens to `NV_EV_SETTINGS_CHANGED` (`ui_shell`) and `NV_EV_INPUT_DEVICES`
   (via `lv_async_call`: dispatch is synchronous in the HID task).
3. Hysteresis: Classic after 3 s with both present; Tablet after 5 s without.
4. With an app open: toast "Mouse and keyboard detected: classic interface on exit" and
   switch in `close_app`. In fullscreen no toast.
5. At boot: wait for the end of USB/BLE enumeration (max 3 s) before choosing, to avoid
   building two shells.
6. Shell change = `destroy_home` + `build_home` on the `ui_refresh_async` path; in
   Classic rotation forced to landscape, restored on exit.
7. `/api/ui/shell?mode=` for automated tests.

Acceptance: I plug and unplug USB and BLE mouse/keyboard 20× → no double switch,
no crash, stable PSRAM heap (`/api/heap/map`).

## 5. Releases

| OTA | Phases | The user sees | Risk |
|---|---|---|---|
| A | 1 | Italian keyboard, no on-screen keyboard with a physical keyboard, wheel, arrow, PIN from keyboard | low |
| B | 2 | whole system usable from keyboard, shortcuts | medium (touches many apps) |
| C | 3 + 4 + 5 + 6 | manual and automatic Classic interface | medium-high (shell) |

Estimate: A 1 session, B 1-2, C 2-3. A and B have value even if C is not done.

## 6. Tests

| Case | How |
|---|---|
| Lost keys / repeat | `/api/ui/type` 500 characters + real keyboard held down |
| IT layout | all keys of the number row and AltGr in Notes |
| Hotplug | USB 20×, BLE 20×, both together |
| Focus | 10 overlays opened/closed from keyboard, focus always visible |
| Classic | every native app, 3 WASM games, terminal, Store |
| Tablet regressions | swipe, pager, folders, Recent apps, shade, rotation |
| Budget | `tools/ci/check_budgets.py`, `/api/heap/map` before/after |
| Privacy | screenshots with SSID/city obscured before publishing them |

## 7. Risks

| Risk | Mitigation |
|---|---|
| `nv_ui.cpp` touched by parallel sessions | Phase 3 in one go, commit right away, notice to peers |
| Focus lost after overlay | group pop always in `LV_EVENT_DELETE` |
| Global tap guard among touch/mouse/automation | per-indev state (1.9) |
| Custom controls without focus | `nv_ui_focusable` + audit list (2.7) |
| App area height differs between shells | fixed-height audit (Phase 3.4) |
| Flash at 85% | < 60 KB total; bitmap font optional |
| Internal SRAM at the limit | everything in PSRAM / LVGL pool; measure before and after |
| Icon cache full | LRU in `icon_scaled` |
| Startup sound → brownout | no startup sound; at most the existing `nv_audio_chime` |

## 8. Open questions

2. Desktop icons: fixed set (Files, Settings, Store) + user choices, or all apps?
3. Keyboard layout: only IT and US, or also ES/FR/DE like the system languages?
4. In Classic without a mouse (enabled by hand, touch only): gesture strips off anyway?
   (proposed: yes, taskbar and title bar are enough)
