# Lua App

## What it is

The engine of the Store's Lua apps: Lua 5.4 with drawing, crisp text in several sizes, touch and multitouch, keyboard and gamepads, timers, storage, HTTP, WebSocket, MQTT and Home Assistant. Every Lua app in the Store (Sudoku, RPN Calc, Aria, MQTT Explorer, RetroLove...) has its own tile and runs on this engine, which installs itself when needed.

## Your own Lua apps

Opened from its tile, Lua App lists the scripts you copy to the SD card:

- a single file: `/sdcard/home/lua/hello.lua`
- a folder with a `main.lua` (plus modules and images): `/sdcard/home/lua/weather/main.lua`

Tap one to run it; the Back gesture returns to the list. Scripts run with Lua App's permissions (screen, files, network); MQTT and Home Assistant need a real Store package.

## For developers

A complete app:

```lua
function nv.draw()
  ui.clear()
  ui.label(ui.W / 2, 200, "Hello!", ui.font.huge, ui.theme.accent, "center")
  if ui.button(412, 360, 200, 70, "Beep") then nv.beep() end
end
```

The full API reference, examples and how to publish in the Store are in `docs/LUA_APPS.md` in the repository. Many games written for LÖVE (love2d.org) run unchanged too.

## Credits and license

Lua 5.4 (Lua.org, PUC-Rio, MIT), json.lua (rxi, MIT), Montserrat font (SIL OFL 1.1). NucleoOS engine.
