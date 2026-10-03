# Writing apps for NucleoOS

The developer entry point: which runtime to pick, how a native app is put together, the keyboard
conventions every app follows, and a checklist before shipping. Headers are the source of truth;
every API named here is declared in the file cited next to it.

- [Choose your runtime](#choose-your-runtime)
- [Native app, step by step](#native-app-step-by-step)
- [Keyboard conventions](#keyboard-conventions)
- [Checklist before shipping](#checklist-before-shipping)
- [Other guides](#other-guides)

## Choose your runtime

| Runtime | Where | What you get | Ships via | Use it when |
|---|---|---|---|---|
| **Native C++** | `components/nv_apps/*.cpp`, in the firmware | Full LVGL 9, every OS service, no sandbox | Firmware build / OTA | System apps, anything that needs LVGL widgets, hardware (camera, JPEG/PPA engines, audio), or tight integration (file associations, clipboard, notifications) |
| **WASM (C)** | `sdk/` (`nucleo_sdk.h`, `build_app.ps1`), app on the SD under `/sdcard/apps/<id>/` | Sandboxed (WAMR, AOT or interpreter), immediate-mode canvas, touch, keyboard, audio, network, files (WASI), manifest permissions. Host ABI **14** (`NV_WASM_ABI`, `components/nv_wasm/include/nv_wasm.h`; `NUCLEO_SDK_ABI` in the SDK) | SD card, Wi-Fi push, App Store | Third-party apps and games, ports of existing C code, anything that must not be able to crash the OS. Guide: [WASM_APPS.md](WASM_APPS.md), [`sdk/README.md`](../sdk/README.md) |
| **Lua** | `apps/<id>/src/main.lua`, package with `"engine": "luaapp"` | Lua 5.4 with `gfx`, `ui`, `nv` modules, LÖVE-style games, PC test runner | App Store, or `tools/lua_pack.py push` for development | Quick tools, dashboards, Home Assistant / MQTT panels, small games, no C toolchain. Guide: [LUA_APPS.md](LUA_APPS.md) |
| **Web app** | `sd/web/apps/<id>/` (`index.html`, `manifest.json`, `i18n.*.json`, each with its `.gz` twin), listed in `sd/web/apps.json` | HTML/JS in the browser desktop served by the board, talking to the device REST API | SD card (`/sdcard/web/...`) | Companion UI used from a PC or phone: editors, spreadsheets, admin pages. Never runs on the panel |

Rule of thumb: start with Lua or WASM; go native only when you need something they cannot reach.
A native app costs firmware size and internal SRAM for every user, and a bug in it can take the
whole OS down.

## Native app, step by step

A complete, idiomatic example lives in [`sdk/native/template_app.cpp`](../sdk/native/template_app.cpp)
(not built). Real apps to read: `calculator_app.cpp` (key handler, state save/load) and
`diagnostics_app.cpp` (nv_kit, timers, teardown).

### 1. The descriptor (`components/nv_ui/include/nv_app.h`)

```cpp
const NvApp kMyApp = {
    .id = "myapp",              // stable: persisted (Home layout, task state, default apps)
    .name = "My App",           // English label, used when name_id < 0
    .icon = &nv_icon_apps,      // launcher icon (nv_icons.h)
    .ram_budget = 256u << 10,   // the Memory Broker frees this much before launch
    .build = myapp_build,       // void (*)(lv_obj_t *content)
    .name_id = NV_STR_APP_MYAPP,// nv_str_id_t, or -1 for "use .name"
    .user = nullptr,            // opaque per-app context (nv_ui_current_app()->user)
    .flags = 0,                 // NV_APP_FLAG_GAME lists it in Start > Games
};
void myapp_app_register(void) { nv_app_register(&kMyApp); }
```

Designated initializers must follow the field order above (C++20). Set `.name_id` explicitly:
the launcher translates any `name_id >= 0`, and `0` is `NV_STR_APP_SETTINGS`, so a forgotten
field shows your app as "Settings". The descriptor must have static lifetime.

### 2. Registration (3 edits)

1. `components/nv_apps/<name>_app.cpp`: the app, with one public `<name>_app_register()`.
2. `components/nv_apps/apps_internal.h`: declare it; `components/nv_apps/nv_apps.cpp`: call it in
   `nv_apps_register_all()` (call order is the default launcher order; the user's arrangement is
   persisted by id, so renaming an id later moves the tile, see ENGINEERING_RULES §8).
   File handlers (`nv_open_register`) are registered in the same function, next to the `NvApp`.
3. `components/nv_apps/CMakeLists.txt`: add the file to `SRCS` (and new components to `REQUIRES`).

### 3. Lifecycle: `build(content)`

- The shell creates the app window and calls `build(content)` on the LVGL thread. Populate
  `content`; do not keep anything outside it.
- `build()` runs again **in place** after a theme or language change and when a new file intent
  arrives (`nv_ui_rebuild_app()`): the content is cleaned, then rebuilt. Write it to be re-entrant.
- Before every build the shell clears the per-open hooks: back handler, key handler, shortcuts
  table. Set them again in `build()` (or on the sub-page that needs them).
- Hang teardown on your root object's `LV_EVENT_DELETE` (it fires on close, suspend and rebuild):
  delete `lv_timer`s, `lv_async_call_cancel()` pending calls, bump generation counters of
  in-flight background jobs, free per-open buffers, null your static widget pointers
  (ENGINEERING_RULES §3).
- `nv_ui_close_app()` closes the app from inside; `nv_ui_set_title()` (`nv_ui_host.h`) retitles
  the header for sub-pages; `nv_ui_app_fullscreen(true)` hides the chrome (games).

### 4. Theme (`components/nv_theme/include/nv_theme.h`)

Read `const NvTheme *th = nv_theme_get();` at build time and use roles, never hex colours:
`bg` (canvas), `surface` / `surface2` / `surface3` (cards, raised and tertiary controls), `header`,
`shade_bg`, `control_alt`, `text`, `text_strong`, `text_dim`, `text_disabled`, `accent`, `primary`,
`on_primary`, `success`, `success_solid`, `danger`, `divider`, `scrim` + `scrim_opa`,
`font_default`. Theme setters publish `NV_EV_THEME_CHANGED`; the shell answers by re-running your
`build()`, so an app that reads the tokens in `build()` needs no subscription of its own.
Renderer traps: no shadows, transforms, opacity layers or `clip_corner` (ENGINEERING_RULES §3).

### 5. UI kit and spacing (`nv_ui_kit.h`, `nv_ui_scale.h`)

| Helper | What |
|---|---|
| `nv_kit_scroll_column(parent)` | Full-height scrolling column: the usual app root |
| `nv_kit_button(parent, label, primary)` | Themed button, >= 44 px, primary = brand fill |
| `nv_kit_round_btn(parent, sym, cb, primary, size)` | Round transport button (media) |
| `nv_kit_info(parent)` | Muted full-width label |
| `nv_kit_row(parent, label)` | Settings card "label ....... (control)"; add the control to it |
| `nv_kit_slider_row(col, name, value, lo, hi, cb)` | Labelled slider row, returns the slider |
| `nv_kit_switch_row(col, name, on, cb)` | Labelled switch row, returns the switch |
| `nv_kit_textarea(parent, placeholder, one_line)` | Textarea bound to the system IME |
| `nv_kit_textarea_ex(..., nv_ime_type_t, nv_ime_return_t)` | Same with input class (email, URL, number, PIN, password) and return-key action |
| `nv_kit_label_set`, `nv_kit_text_color`, `nv_kit_bg_color`, `nv_kit_border_color` | Change-only setters for widgets refreshed by a timer |
| `nv_kit_eq_create` / `nv_kit_eq_run` | "Now playing" mini equalizer |
| `nv_kit_fmt_ms(buf, n, ms)` | `m:ss` formatting |
| `nv_kit_find_ci` / `nv_kit_contains_ci` | Case-insensitive search (ASCII folding) |

Spacing: radii `NV_RAD_SM` 12 / `NV_RAD_MD` 16 / `NV_RAD_LG` 20; spacing ladder `NV_SP_1..5` =
4, 8, 12, 16, 24 px; touch target `NV_TOUCH_MIN` 44 px. No magic numbers.

### 6. Strings (`components/nv_i18n`)

Every user-visible string is `nv_tr(NV_STR_*)`. To add one:

1. `nv_i18n/include/nv_i18n.h`: add `NV_STR_MY_THING,` to `nv_str_id_t`, just before
   `NV_STR_COUNT` (the order is the table index).
2. `nv_i18n/nv_i18n.c`: add `[NV_STR_MY_THING] = "...",` to **each** of the five blocks
   (`[NV_LANG_EN]`, `[NV_LANG_IT]`, `[NV_LANG_ES]`, `[NV_LANG_FR]`, `[NV_LANG_DE]`). A missing cell
   falls back to English, so it builds, but ship all five. Keep printf placeholders identical
   across languages.

Language changes publish `NV_EV_LANG_CHANGED` and rebuild the app like a theme change.

### 7. Keyboard (`components/nv_ui/include/nv_ui_focus.h`)

Keyboard navigation is automatic: the focus engine collects every visible, enabled, clickable
object with its own `CLICKED` / `VALUE_CHANGED` handler (plus LVGL's focusable widgets) and walks
them with Tab / arrows; Enter / Space click. Create children in reading order. Refine with:

- `nv_focus_prefer(obj)`: first focus of the page (primary action / first field). Set it again
  after a rebuild.
- `nv_focus_skip(obj)`: keep decorations, scrims and drag surfaces out of navigation.
- `nv_focus_include(obj)`: force an object in (large clickable areas, handler on an ancestor).
- `nv_focus_current()`: the focused object, e.g. to act on the focused row.

App shortcuts go through **one** hook:

```cpp
bool my_key(uint32_t key, uint8_t usage, uint8_t mods) {
    const bool ctrl = mods & 0x11;                              // HID boot modifier byte
    if (ctrl && usage == 0x11) { new_item(); return true; }     // Ctrl+N (HID usage)
    if (key == LV_KEY_DEL)     { delete_focused(); return true; }
    return false;                                               // let the system handle it
}
nv_ui_set_key_handler(my_key);
```

`key` is an `LV_KEY_*` for Enter / Esc / arrows / Tab / Backspace / Delete / Home / End, else the
typed code point; `usage` / `mods` are the raw HID usage and modifier byte (Ctrl `0x11`, Shift
`0x22`, Alt `0x44`, Win `0x88`). It runs only while no text field has the focus, before
navigation, auto-repeat included. Match Ctrl chords on `usage`, not on `key`.

Declare the app's shortcuts so F1 / Ctrl+/ lists them above the system ones:

```cpp
static const nv_shortcut_t kKeys[] = {   // {keys, it, en}; static: must outlive the page
    {"Ctrl+N", "Nuovo elemento", "New item"},
    {"Canc",   "Elimina",        "Delete"},
};
nv_ui_set_shortcuts(kKeys, 2);
```

Write the key names the way the system sheet does (`components/nv_ui/nv_ui_shortcuts.cpp`).
Text fields created with `nv_kit_textarea*` get typing, selection and clipboard keys for free.
Rules: ENGINEERING_RULES §11.

### 8. Back, state, pages

- **Back** (`nv_ui.h`): `nv_ui_set_back_handler(fn)` routes the header arrow, the Back gesture and
  Esc to `fn` (sub-page -> parent); `nullptr` restores "Back closes the app". Set it when a
  sub-page opens, clear it on the root page. `nv_ui_set_back()` in `nv_ui_host.h` is a deprecated
  alias.
- **Task state** (`nv_ui.h`): `nv_ui_state_save(data, len)` from the root's `LV_EVENT_DELETE`,
  `nv_ui_state_load(out, cap)` in `build()`. Up to 1 KB per app, PSRAM, kept while the task sits
  on the taskbar or across a rebuild; dropped when the user really closes it (X, Alt+F4, Home).
  Use `nv_config_*` for settings that must survive a reboot.
- **Pages** (`nv_ui.h`): `nv_ui_open_app_page("settings", "network")` opens an app on a page;
  the app reads it once in `build()` with `nv_ui_take_page(id)` (NULL = start page).
  `nv_ui_open_app_id()` / `nv_ui_open_app()` open by id / descriptor; from a non-LVGL task use
  `nv_ui_open_app_id_async()` / `nv_ui_go_home_async()`.

### 9. Notifications (`components/nv_ui/include/nv_notify.h`)

- `nv_toast(kind, msg)`: transient snackbar, not stored. Kinds: `NV_NOTE_INFO`, `NV_NOTE_OK`,
  `NV_NOTE_WARN`, `NV_NOTE_ERROR`.
- `nv_notify_post(kind, title, msg)`: popup + stored in the notification center.
- `nv_notify_post_ex(kind, title, msg, &opts)` with `nv_note_opts_t {tag, app, page, quiet}`: a
  `tag` keeps one live note per subject, `app` + `page` make a tap open that app page, `quiet`
  stores it with the badge only. `nv_notify_remove_tag(tag)` when the subject is over.

The system decides when a post pops up (DND, fullscreen apps, lock screen, folding repeats).
Never draw your own toasts or banners. LVGL thread only.

### 10. Files (`components/nv_ui/include/nv_open.h`)

Open files with `nv_open_file(path)` (or `nv_open_with`, `nv_open_reveal`), never by hard-coding
an app. An app that opens files registers an `NvOpenHandler` (opener or action) next to its
`NvApp` and checks `nv_open_intent()` at the top of `build()`; `nv_open_finish()` returns to the
caller. Full model, priorities and examples: [FILE_ASSOCIATIONS.md](FILE_ASSOCIATIONS.md).

### 11. Clipboard (`components/nv_kernel/include/nv_clipboard.h`)

One system clipboard, thread-safe, no LVGL. Text: `nv_clip_set_text(utf8, source)`,
`nv_clip_has_text()`, `nv_clip_get_text()` (malloc'd, caller frees). Image (RGB565, optional
encoded file): `nv_clip_set_image(...)`, `nv_clip_image_get()` + `nv_clip_image_release()`,
`nv_clip_image_file()`. Files: `nv_clip_set_files(paths, n, cut, source)`, `nv_clip_get_files()`.
`nv_clip_kind()` and `nv_clip_seq()` tell what is held and whether it changed; every change
publishes `NV_EV_CLIPBOARD` (payload `nv_clip_change_t`, may arrive off the LVGL thread: set a
flag and act in a UI tick). Design notes: [SCREENSHOT_CLIPBOARD.md](SCREENSHOT_CLIPBOARD.md).

### 12. Background work and threads

- LVGL objects and timers are touched only on the LVGL task. From any other task wrap the calls
  in `lvgl_port_lock(timeout_ms)` / `lvgl_port_unlock()`, or post with `lv_async_call()`; for
  app switches use the `_async` variants (ENGINEERING_RULES §3).
- `nv_bgwork_submit(fn, arg)` (`nv_kernel/include/nv_bgwork.h`) runs best-effort jobs (SD writes,
  thumbnails) on a low-priority worker. It never blocks: `false` means dropped, you still own
  `arg`. Jobs never touch LVGL (post results with `lv_async_call` + a generation check) and never
  touch flash: the worker stack is in PSRAM (`nv_config_*` is the one exception, ENGINEERING_RULES
  §2).
- Your own tasks: PSRAM stack (`xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)`) only if they never
  touch flash; end them with `vTaskDeleteWithCaps` (§2). No SD or blocking work in esp_timer
  callbacks (§3).

### 13. Memory (ENGINEERING_RULES §1)

- Internal SRAM is the scarce tier (~200-240 KB free at runtime); PSRAM has 32 MB.
- Allocate per open in `build()`, free on `LV_EVENT_DELETE`; big buffers with
  `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` (a plain `malloc` under 16 KB lands in internal SRAM).
- Bulky statics get `NV_PSRAM_BSS` (`nv_mem_attr.h`), never for ISR / DMA data.
- `ram_budget` is what the Memory Broker reclaims before launch; set it to your real peak.
- Check with `python tools/ci/check_budgets.py build` (CI enforces it).

## Keyboard conventions

Every app, native or not, uses the same keys for the same things. Implement the ones that apply.

| Key | Action |
|---|---|
| Esc | Back: close the sub-page / panel; at the root the system closes the app. Comes from the back handler, never consumed by a key handler |
| Enter / Space | Activate the focused control (system) |
| Arrows | Move in lists and grids (system); in viewers: previous / next item, pan |
| Tab / Shift+Tab | Next / previous control (system) |
| Ctrl+N | New (document, note, item) |
| Ctrl+S | Save |
| Ctrl+F | Find / filter |
| Delete | Delete the focused / selected item (confirm if not undoable) |
| F2 | Rename |
| F5 | Refresh |
| Space | Play / pause in media apps (when the focus is not on a button) |
| Ctrl+A / C / X / V | Select all / copy / cut / paste in lists that have a selection |
| Menu, Shift+F10 | Context menu of the focused control (system: long press) |
| PgUp / PgDn / Home / End | Scroll / jump (system) |

**Reserved by the system — apps must not take them:** Win and every Win chord, Ctrl+Esc,
Alt+Tab, Alt+F4, Ctrl+W (closes the app outside text fields), Ctrl+Alt+Del, PrtSc / Alt+PrtSc /
Win+Shift+S, F1 and Ctrl+/ (shortcuts sheet), media keys (volume, mute, play/pause, stop), Tab.
Most are handled before the app key handler runs; Esc and Tab reach it but must be returned
`false`.

## Checklist before shipping

- [ ] `NvApp` with a final, stable `id`, a real icon, `.name_id` set (translated name or -1),
      an honest `ram_budget`.
- [ ] Registered in `apps_internal.h`, `nv_apps.cpp`, `CMakeLists.txt`.
- [ ] Colours only from `nv_theme_get()`; checked in light and dark, normal and large font.
- [ ] Every string through `nv_tr()`, present in all five locales.
- [ ] Works with the keyboard alone: everything reachable by Tab / arrows, `nv_focus_prefer` on
      the main action, Esc goes back from every sub-page, shortcuts in `nv_ui_set_shortcuts`.
- [ ] Works with touch alone: 44 px targets, nothing reachable only by a key.
- [ ] Teardown on `LV_EVENT_DELETE`: timers deleted, async calls cancelled, pointers cleared,
      buffers freed. Open / close it 20 times and watch the heap in System Monitor.
- [ ] Survives a theme / language change and a suspend / resume while on a sub-page.
- [ ] No LVGL calls off the LVGL thread without `lvgl_port_lock`; no flash access from PSRAM
      stacks; no blocking work on the LVGL thread (use `nv_bgwork_submit`).
- [ ] Feedback via `nv_toast` / `nv_notify_post*`, files via `nv_open_*`, copy / paste via
      `nv_clip_*`.

## Other guides

- [WASM_APPS.md](WASM_APPS.md) — WASM apps: canvas, manifests, build and push, store
- [`sdk/README.md`](../sdk/README.md) — WASM SDK reference: host imports, WASI, WASM-4 carts
- [LUA_APPS.md](LUA_APPS.md) — Lua apps and LÖVE games
- [GAME_DEV.md](GAME_DEV.md) — games and the Vertice 3D engine
- [FILE_ASSOCIATIONS.md](FILE_ASSOCIATIONS.md) — `nv_open`: types, handlers, intents
- [SCREENSHOT_CLIPBOARD.md](SCREENSHOT_CLIPBOARD.md) — capture and the system clipboard
- [ENGINEERING_RULES.md](ENGINEERING_RULES.md) — invariants every change respects
