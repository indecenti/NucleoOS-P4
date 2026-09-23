# File associations (`nv_open`)

How NucleoOS decides which app opens a file, and how to plug a new file type or a new app
into that. API: `components/nv_ui/include/nv_open.h`. Implementation: `components/nv_ui/nv_open.cpp`.

## The rule

No app hard-codes which app handles a file type. To open a file, an app calls
`nv_open_file(path)`. To offer the user a choice, it calls `nv_open_with(path)`. To list the verbs
available for a file, it asks `nv_open_handlers(path, NV_OPEN_ACTION, ...)`. The OS decides the rest.

## How a file is resolved

1. **Type.** The extension maps to a MIME type and a kind (`nv_open_mime`, `nv_open_kind`), using
   one table in `nv_open.cpp`. Unknown extensions are `application/octet-stream`.
2. **Handlers.** Apps register `NvOpenHandler` descriptors next to their `NvApp`, in their
   `*_register()` function. Each descriptor lists the MIME patterns it accepts
   (`"text/plain text/markdown"`, `"audio/*"`). There are two roles:
   - **OPENER** launches its app with an *intent* for the file.
   - **ACTION** runs `run(path, ctx)` in place. For example, `sys.wallpaper` is "Set as wallpaper".
3. **Choice.** The user's default for that MIME wins. Failing that, the app opens directly when it
   is the only opener, or when its `priority` beats every other opener. A tie shows the system
   "Open with" sheet, which has a "Remember my choice" switch.

Defaults are stored in the nv_config key `open_defs` (`mime=handler;...`). Users manage them in
**Settings > Default apps**, which offers "Ask every time" and "Reset default apps".

## Built-in handlers

| id               | role   | app     | types                                      | prio |
|------------------|--------|---------|--------------------------------------------|------|
| `notes.edit`     | opener | notes   | text/plain text/markdown                   | 50   |
| `music.play`     | opener | music   | audio/mpeg audio/wav audio/flac audio/aac audio/mp4 | 50 |
| `gallery.view`   | opener | gallery | image/jpeg image/png image/bmp             | 50   |
| `video.play`     | opener | video   | video/x-msvideo video/mpeg (+ mp4/h264 only with `CONFIG_NV_VPLAYER_H264`) | 50 |
| `files.preview`  | opener | files   | text/* application/json application/xml image/jpeg image/png image/bmp | 10 |
| `sys.wallpaper`  | action | —       | image/jpeg                                 | 0    |
| `<wasm id>.open` | opener | a store app | manifest `"opens"`                     | 0    |

Priorities follow a convention. The real editor or viewer of a type uses 50. A secondary view,
such as Files' read-only Preview, uses 10. Store apps use 0, so installing an app never silently
takes over a type. The user makes it the default from "Open with" or from Settings.

Types list only what the decoder really supports. `music.play` has no `audio/ogg` because
`nv_media` cannot decode it. An `.ogg` therefore shows "No app can open this file"; it is never
opened in an app that then fails.

## What the launched app sees (intents)

```c
const NvIntent *in = nv_open_intent();   // NULL when opened normally
if (in && in->verb == NV_INTENT_OPEN) { /* open in->path */ }
```

- The intent is bound to the foreground app until that app closes. It survives `build()` re-runs
  (a theme or language refresh), so check it at the top of `build()`.
- **Back** at the app's root returns to the app that asked. Files, for example, comes back on the
  same folder with the file highlighted, through an `NV_INTENT_RESUME` intent. An app whose
  intent view has its own in-app Back calls `nv_open_finish()` when that view is done. Gallery's
  viewer and Notes' editor do this.
- `nv_open_intent_drop()` means the user moved on inside the app, so Back is normal again.
- **Home** and **Recents** never return: they close as usual.
- Opening a file with the app already in the foreground delivers the intent in place: `build()`
  re-runs through `nv_ui_rebuild_app()`. WASM apps are relaunched instead.
- `NV_INTENT_REVEAL` (`nv_open_reveal(path)`) opens Files on a folder, or on a file inside its folder.

## Adding a file type

Add a row to `kTypes[]` in `nv_open.cpp` (`{"ext", "mime/type", NV_FILE_KIND}`). A store app can
also teach the OS an extension at boot through its manifest `"file_types"`
(`nv_open_register_type`). A built-in extension always keeps its built-in type.

## Adding a native app that opens files

```c
const NvOpenHandler kMyView = {
    "myapp.view", "myapp", "application/pdf", -1 /* label = app name */, nullptr,
    LV_SYMBOL_FILE, NV_OPEN_OPENER, 50, nullptr, nullptr,
};
void myapp_register(void) {
    nv_app_register(&kMyApp);
    nv_open_register(&kMyView);
}
void myapp_build(lv_obj_t *content) {
    const NvIntent *in = nv_open_intent();
    if (in && in->verb == NV_INTENT_OPEN) show_file(in->path);
    else                                  show_home();
}
```

## Adding a system action

```c
bool send_to_pc(const char *path, void *ctx);   // LVGL thread; toast the outcome
const NvOpenHandler kSend = {
    "sys.send_pc", nullptr, "*/*", NV_STR_SEND_TO_PC, nullptr, LV_SYMBOL_UPLOAD,
    NV_OPEN_ACTION, 0, send_to_pc, nullptr,
};
```

Files' Details page and the Gallery viewer list every action that matches the file, so a new
action appears in both without touching them.

## Store apps (WASM, ABI v7)

A manifest declares `"opens": ["text/markdown"]`, plus optionally `"file_types"`. The app then
appears under "Open with" and in Settings > Default apps. When the user opens a file with it, the
app may read **only that file**, read-only, through `nv.open_path` / `nv.open_size` /
`nv.open_read`. No filesystem permission is involved: the user's choice is the grant. See
`docs/WASM_APPS.md`.

## Other entry points

- Web: `GET /api/open?path=/sdcard/...[&with=<handler id>]` opens the file on the device.
  `GET /api/open/handlers?path=...` returns its MIME, the preferred handler, the default, the
  openers and the actions.
- ANIMA: the `open_file` tool calls `nv_open_file_async()`. Back from the opened app returns to
  ANIMA.

## Rules

- Launches are always deferred (lv_async), so calling `nv_open_file()` from a row's own click
  handler is safe.
- `nv_open_file_async()` is the only entry for other tasks (httpd, ANIMA workers).
- A handler's types must match what the app really decodes. Test with a real file.
- Handler ids are persisted in user defaults. Never rename one; add a new id instead.
