# ports/luaapp — the Lua App engine

Lua 5.4.9 as a WASI reactor (`run`) that runs graphical Lua apps: the Store packages with
`"engine": "luaapp"` and the scripts in `/sdcard/home/lua`. User documentation: `docs/LUA_APPS.md`.

| file | |
|---|---|
| `luaapp.c` | entry, bundle (`app.lpk`: read-only `/package/app.lpk` from firmware "wasi" 1.3, else cache or download) + SHA-256 check against the manifest `args`, `require` from the bundle, Lua bindings (`gfx`, raw `_nv`), frame loop, error screen |
| `gfx.c`, `gfx.h` | software renderer into a guest RGB565 framebuffer (anti-aliased text, circles, rounded rects, Wu lines, polygons, arcs, surfaces with alpha, transform stack); only the changed rows are blitted (ABI 6 persist mode) |
| `sha256.h` | SHA-256 for the bundle check |
| `lib/nvrt.lua` | runtime: callbacks, input events (touch, HID keyboard, ABI 11 pads), timers, storage, HTTP/WS/MQTT/HA/mDNS wrappers |
| `lib/ui.lua` | immediate-mode widgets and the on-screen keyboard |
| `lib/love.lua` | LÖVE compatibility layer |
| `lib/json.lua` | rxi/json.lua 0.1.2 (MIT), unchanged |
| `lib/launcher.lua` | the engine's own screen: the scripts in `/sdcard/home/lua` |
| `gen_font.py`, `font/` | Montserrat Medium (SIL OFL 1.1) baked to 4-bit glyphs, `gen/font_data.h` |
| `host/luahost.c` | PC test host (WAMR in WSL, stubbed `nv` imports, scripted input, canned network) |

```bash
bash ports/luaapp/build.sh engine test    # apps/luaapp/app.wasm + app.aot, then every Lua app on the PC
```

Firmware side: only the read-only `/package` preopen of engine packages and the store installing
`app.lpk` with the signed package ("wasi" 1.3, `components/nv_wasm/nv_wasi_ro.h`). Otherwise the
engine uses the existing ABI (6 persist blit, 8 try_call/throw for Lua
errors, 11 pads, 12/13 network, 14 engine packages + kbd). Lua errors unwind through
`nv.try_call`/`nv.throw` exactly like the terminal Lua (`ports/lua/nv_lua_port.h`).
