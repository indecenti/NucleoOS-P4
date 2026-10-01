# Lua apps

Graphical apps for NucleoOS written in Lua 5.4, distributed through the Store like any other app.
A Lua app is a store package with `"engine": "luaapp"`: it ships no WASM module of its own, only
its Lua code (and images, sounds). The **Lua App** engine (`apps/luaapp`, built from
`ports/luaapp`) runs it.

- [Three steps to a new app](#three-steps-to-a-new-app)
- [How it works](#how-it-works)
- [The app's life](#the-apps-life)
- [`gfx` — drawing](#gfx--drawing)
- [`ui` — widgets](#ui--widgets)
- [`nv` — input, time, storage, sound](#nv--input-time-storage-sound)
- [`nv` — network, MQTT, Home Assistant](#nv--network-mqtt-home-assistant)
- [LÖVE games](#löve-games)
- [Manifest and permissions](#manifest-and-permissions)
- [Testing on the PC](#testing-on-the-pc)
- [Limits](#limits)

## Three steps to a new app

```bash
python tools/lua_pack.py new myapp "My App"        # 1. apps/myapp/ from sdk/lua/template
#    write apps/myapp/src/main.lua, fill in manifest.json, GUIDE.md, GUIDE.en.md
python tools/make_mdi_icon.py apps/myapp gauge "#2563eb"   #    icon from a Material Design glyph
python tools/lua_pack.py pack apps/myapp           # 2. app.lpk + sounds + the bundle hash in the manifest
python tools/lua_pack.py run apps/myapp 120 "60:shot"     #    optional: run it on the PC (screenshot)
#                                                     3. a catalog line in server/appstore/catalog.json,
#                                                        then the usual store export (signs it)
```

During development, skip the store: `python tools/lua_pack.py push apps/myapp` copies `src/` to the
board's `/sdcard/home/lua/myapp/`, and the **Lua App** tile lists and runs it. A single script
works too: drop `hello.lua` into `/sdcard/home/lua/`.

## How it works

```
apps/<id>/
  manifest.json      "engine": "luaapp", "requires": {"luaapp": "1.0"}, "args": ["<sha256>"]
  src/main.lua       the app (plus modules, img/*.png, sounds, data files)
  app.lpk            the bundle tools/lua_pack.py builds from src/ (committed, exported)
  snd/*.wav          sounds converted from src/ (store assets, installed with the package)
  icon.z  GUIDE.md  GUIDE.en.md  shots/1.jpg ...
```

- `tools/lua_pack.py pack` turns `src/` into `app.lpk` — Lua and data files as they are, images
  (`.png`, `.jpg`) converted to the engine's RGB565 + alpha format with their path kept — and
  writes the bundle's SHA-256 into the manifest's `"args"`. Sounds (`.ogg`, `.wav`, `.mp3`) become
  48 kHz mono WAVs in `snd/` (`sfx/pop.ogg` → `snd/sfx_pop.wav`, played with `nv.sound("sfx_pop")`).
- The store export publishes `app.lpk` next to the manifest and signs the whole package. The
  device installs the manifest, icon and sounds; on the first start the engine downloads
  `app.lpk` from the store site, checks it against the hash in the (signed) manifest, keeps it in
  the app's private folder and checks it again at every start. A new version = a new hash = a
  fresh download. Nothing else can be run: a bundle with the wrong hash is refused.
- So every Lua store app declares `"net"` (the first download) and `"fs"` (the cached bundle and
  its saves), and the first start needs Wi-Fi.
- `require("foo.bar")` loads `foo/bar.lua` from the bundle. The engine's own libraries (`ui`,
  `json`, `love`) come first.
- The app runs in its own WASM sandbox with the manifest's permissions, like every app.

## The app's life

`main.lua` runs once at start. Then the engine calls the functions you define on `nv`:

| callback | when |
|---|---|
| `nv.init()` | once, after `main.lua` |
| `nv.update(dt)` | every frame (about 30-60 per second), `dt` in seconds |
| `nv.draw()` | when the screen needs redrawing: after an input event, a timer, a network answer, `nv.redraw()` — or every frame after `nv.continuous(true)` (games) |
| `nv.touch(ev)` | finger events: `ev.type` = `"down"`, `"move"`, `"up"`; `ev.id` (1 = first finger), `ev.x`, `ev.y`, `ev.sx`, `ev.sy` (where it started), `ev.dx`, `ev.dy` |
| `nv.tap(x, y)` | a short tap of the first finger |
| `nv.key(ev)` | keyboard and gamepads: `ev.key` (`"a"`, `"enter"`, `"left"`, `"f1"`, `"kp5"`, `"pad_a"`...), `ev.down`, `ev.text` (the character typed, with Shift), `ev.shift/ctrl/alt`. Return `true` when you used the key: an Escape you did not use works as the back gesture |
| `nv.back()` | the OS back gesture. Return `true` to stay (e.g. close a sub-page); otherwise the app closes |
| `nv.quit()` | the app is closing |

`nv.draw()` draws the whole screen each time (immediate mode); the engine sends only the rows
that changed to the display. Errors stop the app with the message and the stack traceback on
screen (and in the system log).

```lua
-- a complete app: a counter that survives restarts
local n = nv.load("n", 0)
function nv.draw()
  ui.clear()
  ui.header("Counter")
  ui.label(ui.W / 2, 200, tostring(n), ui.font.giant, ui.theme.fg, "center")
  if ui.button(412, 400, 200, 80, "+1") then n = n + 1; nv.save("n", n) end
end
```

## `gfx` — drawing

Coordinates are canvas pixels (1024×600 unless the manifest says otherwise), floats allowed.
Colours are `0xRRGGBB` integers (`gfx.rgb(r, g, b)` builds one). Shapes are anti-aliased where it
is cheap (text, circle and rounded edges, thin lines).

| function | |
|---|---|
| `gfx.clear(color)` | fill the screen (or the clip rectangle) |
| `gfx.rect(x, y, w, h, color [, radius])` | filled rectangle, rounded corners |
| `gfx.frame(x, y, w, h, color [, thick=1, radius])` | outline |
| `gfx.circle(x, y, r, color)` / `gfx.ring(x, y, r, color [, thick])` | disc / circle outline |
| `gfx.arc(x, y, r, a0, a1, color [, thick])` | arc in degrees (0 = 3 o'clock, clockwise); no `thick` = pie slice |
| `gfx.line(x0, y0, x1, y1, color [, thick=1])` | line |
| `gfx.tri(x0, y0, x1, y1, x2, y2, color)` / `gfx.poly({x1, y1, x2, y2, ...}, color)` | filled triangle / polygon |
| `gfx.pixel(x, y, color)` / `gfx.get_pixel(x, y)` | one pixel |
| `gfx.text(x, y, s [, size=16, color=white, align])` → width | text, `y` = top; `align` = `"left"`, `"center"`, `"right"` (relative to `x`); `\n` breaks lines |
| `gfx.text_width(s, size)`, `gfx.font_height(size)` → line height, ascent | measure |
| `gfx.alpha(a)` | opacity 0..255 for everything drawn after it |
| `gfx.clip(x, y, w, h)` / `gfx.clip()` | restrict drawing to a rectangle / reset |
| `gfx.push()`, `gfx.pop()`, `gfx.translate(x, y)`, `gfx.scale(sx [, sy])`, `gfx.origin()` | transform stack |
| `gfx.image(path)` → surface | an image of the bundle (`"img/logo.png"`) |
| `gfx.surface(w, h)` → surface | an off-screen canvas with transparency |
| `gfx.target(surface)` / `gfx.target()` | draw into a surface / back to the screen |
| `gfx.draw(surface, x, y [, w, h])` | draw a surface, scaled, with its transparency |
| `surface:size()` → w, h; `surface:free()` | |
| `gfx.width()`, `gfx.height()` | the current target's size |

Text uses Montserrat Medium at 12, 14, 16, 20, 24, 32, 48 and 72 px (other sizes round down).
Up to 32 px it covers Latin-1 (accented letters of the Western European languages) plus
`π Ω ∆ √ ∞ ≈ ≠ ≤ ≥ − – — … ‘ ’ “ ” • € ← → ↑ ↓ ▲ ▼`; 48 and 72 px carry ASCII and those symbols
(accents fall back to the plain letter).

## `ui` — widgets

Immediate-mode widgets: call them from `nv.draw()`; each draws itself and returns what the
finger did to it in this frame. `ui.S(v)` scales a 1024×600 size to the canvas, `ui.W`/`ui.H` are
the canvas size, `ui.theme` the colours (`bg panel panel2 line press fg dim accent accent_fg ok
warn err`), `ui.font` the sizes (`small body big title huge giant`).

| widget | returns |
|---|---|
| `ui.clear([color])` | |
| `ui.header(title, {back=true, action="refresh"\|"Label"})` | `back_tapped, action_tapped` |
| `ui.button(x, y, w, h, label, {style="primary"\|"flat"\|"outline"\|"danger", color, fg, size, icon, disabled})` | `true` when tapped |
| `ui.toggle(x, y, on, label)` / `ui.checkbox(x, y, on, label)` | the new state |
| `ui.slider(x, y, w, value, min, max, {step, color, id, on_release})` | `value, changed` |
| `ui.list(id, x, y, w, h, items, {row_h, selected, size})` | index tapped. `items[i]` = `"text"` or `{title=, sub=, right=, color=}`; drag to scroll |
| `ui.tabs(x, y, w, h, labels, selected)` | the selected index |
| `ui.keys(x, y, w, h, rows, {gap, size, style=function(label)})` | the label of the key tapped; `rows` like `{{"7","8","9"},{"0:2","."}}` (`:2` spans two columns) |
| `ui.progress(x, y, w, h, frac [, color])`, `ui.gauge(cx, cy, r, frac [, color, thick])` | |
| `ui.label(x, y, text [, size, color, align])`, `ui.paragraph(x, y, w, text [, size, color])` → height | |
| `ui.wrap(text, size, w)` → lines | |
| `ui.panel(x, y, w, h [, color, radius])` | |
| `ui.icon(name, cx, cy, size [, color])` | vector icons: `back next close plus minus play pause stop check refresh power bulb gear home menu dot` |
| `ui.grid(x, y, w, h, cols, rows [, gap])` → `cell(c, r [, cspan, rspan])` → `x, y, w, h` | layout |
| `ui.prompt(title, initial, function(text) end)` | on-screen keyboard (a physical keyboard types too); `text` is `nil` when cancelled |
| `ui.hit(x, y, w, h)` → `tapped, pressing` | build your own widget |
| `ui.shade(color, k)`, `ui.mix(a, b, t)` | colour helpers |

Note: a function returning several values passed in the middle of an argument list keeps only
the first (`f(ui.grid(...)(1, 1), "x")` passes just `x`); take the values first:
`local x, y, w, h = cell(1, 1)`.

## `nv` — input, time, storage, sound

| function | |
|---|---|
| `nv.redraw()`, `nv.continuous(on)`, `nv.exit()` | drawing policy, close the app |
| `nv.pointer` | `{down, x, y, sx, sy, pressed, released, moved}` of the first finger |
| `nv.down(key)` | is that key (or pad button) held |
| `nv.millis()`, `nv.clock()` | time since boot (ms / s) |
| `nv.time()` | Unix time (UTC, seconds) |
| `nv.date(fmt [, t])` | local time, `os.date` formats (`"%H:%M"`, `"*t"`) |
| `nv.utc_offset()`, `nv.set_utc_offset(sec)` | WASM apps cannot read the system time zone: the offset is kept by the engine, shared by all Lua apps (Aria sets it from Open-Meteo) |
| `nv.after(sec, fn)`, `nv.every(sec, fn)` → id, `nv.cancel(id)` | timers |
| `nv.save(key, value)`, `nv.load(key [, default])` | persistent values (tables, strings, numbers, booleans) |
| `nv.path(name)` | a file path in the app's private folder, for `io.open` |
| `nv.read(name)`, `nv.files()` | a file of the bundle / the list |
| `nv.lang()`, `nv.it`, `nv.tr(en, it)` / `nv.tr{en=..., it=..., de=...}` | the UI language |
| `nv.toast(msg [, "info"\|"ok"\|"warn"\|"error"])`, `nv.log(...)` (`print` too) | |
| `nv.tone(hz, ms)`, `nv.beep()`, `nv.melody{{440, 200}, {0, 100}, {660, 200}}` | tones |
| `nv.sound(name)` | a WAV of the package (`snd/<name>.wav`) |
| `nv.speak(text [, lang])` | the offline voice |
| `nv.backlight(percent)` | restored when the app closes |
| `nv.random([a [, b]])` | hardware random numbers (`math.random` is seeded from it) |
| `nv.fps`, `nv.W`, `nv.H`, `nv.app` | |

The standard Lua libraries are there (`string`, `table`, `math`, `utf8`, `os`, `io`, `coroutine`).
`io` sees the app's private folder as `/`; `os.execute` and `io.popen` are not available.

## `nv` — network, MQTT, Home Assistant

Nothing blocks: each call takes a callback that runs from the frame loop when the answer is in.
The manifest's permissions apply (`net` Internet, `lan` home network, `ws`, `mqtt`, `ha`).

```lua
nv.get_json("https://api.open-meteo.com/v1/forecast?latitude=52.52&longitude=13.41&current=temperature_2m",
  function(data, err)
    temp = data and data.current.temperature_2m
  end)

nv.http({ url = "http://192.168.1.20/rpc/Switch.Toggle?id=0", method = "POST", body = { on = true } },
  function(res) nv.toast(res.ok and "done" or res.error) end)   -- res = {ok, status, body, error, json()}

nv.mqtt.sub("zigbee2mqtt/+", function(topic, payload) last[topic] = payload end)
nv.mqtt.pub("lights/desk/set", { state = "ON" })                 -- tables are sent as JSON

if nv.ha.available() then
  nv.ha.states(function(list) entities = list end)
  nv.ha.call("light", "toggle", { entity_id = "light.kitchen" })
end

nv.mdns("_shelly", "_tcp", function(devices) found = devices end) -- {name, host, ip, port, txt}

local ws = nv.ws("wss://example.org/socket")
ws.on_message = function(msg) log[#log + 1] = msg end
ws.on_open = function() ws:send("hello") end
```

| function | |
|---|---|
| `nv.http(opts, cb)` | `opts = {url, method, headers, body (string or table → JSON), timeout (ms), max (bytes)}` |
| `nv.get(url, cb)`, `nv.get_json(url, cb(data, err))` | |
| `nv.ws(url [, headers])` → socket | `s.on_open, s.on_message(msg), s.on_close`, `s:send(text \| table)`, `s:close()`, `s.open`, `s.closed` |
| `nv.mqtt.sub(filter, cb(topic, payload))` → ok, err; `nv.mqtt.pub(topic, payload [, retain])` | through the OS connection (Settings > Home). No `#` alone, no publishing under `homeassistant/`, `nucleo/`, `$` |
| `nv.ha.available()`, `nv.ha.req(method, path, body, cb)`, `nv.ha.states(cb)`, `nv.ha.state(id, cb)`, `nv.ha.call(domain, service, data [, cb])`, `nv.ha.ws()` → socket (already authenticated) | the token stays with the OS |
| `nv.mdns(service, proto, cb(list))` | ~2.5 s browse of the home network |

At most four requests run at once; more wait in a queue. Answers are capped at 256 KB for
`get_json` (pass `max` to `nv.http` for up to 1 MB). Errors: `permission`, `timeout`,
`connect failed`, `destination refused`, `not configured`, `too big`.

## LÖVE games

`love` is available without `require`: a game written for [LÖVE](https://love2d.org) 0.10 / 11
runs from its own `main.lua` (and `conf.lua`). Supported: `love.load/update/draw`, keyboard,
mouse, touch callbacks; `love.graphics` shapes, `print`/`printf`, fonts (`newFont(size)` or any
TTF path → Montserrat at that size), images, canvases, `push/pop/translate/scale`, scissor;
`love.timer`, `love.math.random`, `love.keyboard.isDown`, `love.touch`, `love.mouse`,
`love.filesystem` (reads the bundle, writes the private folder), `love.audio` sources (play the
converted WAVs, one at a time), `love.event.quit`. Not there: shaders, meshes, quads, sprite
batches, physics, image rotation, streamed music.

- `love.window.setMode(w, h)` larger or smaller than the canvas: the game is scaled to fit and
  centred, touch coordinates mapped back. For retro games, set a small canvas in the manifest
  (`"canvas_w": 320, "canvas_h": 240, "canvas_scale": "fit"`): the OS scales it in hardware.
- Keyboard-only games get touch for free: a tap is Space, a swipe is an arrow key. Change the map
  in the game's `main.lua`: `love.touch_keys = { tap = "up", left = "left", right = "right", down = "space" }`.
- The back gesture sends Escape; two in a row close the app.
- Gamepads: d-pad = arrows, A = space, B = lctrl, X = z, Y = x, Start = return, Select = escape.

RetroLove and Tetronimo in the Store are LÖVE games running this way.

## Manifest and permissions

```json
{
  "id": "myapp", "name": "My App", "version": "1.0.0",
  "entry": "run", "abi": 14, "engine": "luaapp",
  "requires": { "luaapp": "1.0", "wasi": "1.1" },
  "ram_budget": 8388608, "stack_kb": 128, "timeout_ms": 120000,
  "permissions": ["gfx", "fs", "net", "log"],
  "canvas_w": 1024, "canvas_h": 600,
  "category": "tools", "author": "...", "license": "MIT", "source": "https://...",
  "description": "...", "descriptions": { "en": "...", "it": "..." },
  "args": ["<sha256 of app.lpk, written by lua_pack.py>"]
}
```

`gfx`, `fs` and `net` are always needed (screen, the cached bundle and saves, the first
download). Add `lan`, `ws`, `mqtt`, `ha` only if the app uses them: the Store shows them before
install. `ram_budget` caps the app's memory: the engine's own data and the screen buffer (1.2 MB
at 1024×600) come out of it, 8 MB is plenty for most apps. An optional `"args"` second entry overrides the bundle URL (a local store
during development).

## Testing on the PC

```bash
bash ports/luaapp/build.sh engine test        # build the engine, run every Lua app headless
python tools/lua_pack.py run apps/myapp 300 "30:t=500/300,60:shot,90:key=40,120:back"
```

`run` executes the real `apps/luaapp/app.wasm` under WAMR in WSL (the firmware's feature set)
with a simulated screen, clock and input, and saves `ports/_src/luaapp/test/<id>.png` (+ one PNG per
`shot`). Script items: `F:t=X/Y` tap at frame F, `F-G:t=X/Y` hold, `F:key=USAGE` (HID usage),
`F:back`, `F:shot`. Network answers come from `apps/<id>/test/net/<url with non-alphanumerics
as _>`, MQTT messages from `apps/<id>/test/mqtt.txt` (`topic|payload` per line), and
`apps/<id>/test/test.json` (`{"frames": N, "script": "...", "lang": "it"}`) is what `test-all` runs.

## Limits

- The bundle is at most 1 MB (images are stored uncompressed: prefer vector drawing and small
  images). Each sound up to 4 MB, all assets 24 MB.
- One sound effect at a time (`nv.sound`); tones are simple beeps.
- No threads; `coroutine` works for cooperative tasks.
- A full-screen redraw costs a few tens of milliseconds on the board: draw on change (the
  default), keep `nv.continuous(true)` for games, and prefer a smaller canvas for action games.
- Sideloaded scripts (`/sdcard/home/lua`) run with the Lua App engine's permissions (`gfx fs home
  net lan log`), not with MQTT or Home Assistant access.
