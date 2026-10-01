-- My App - a NucleoOS Lua app. API: docs/LUA_APPS.md
-- nv.draw() repaints the screen; the ui widgets return what the finger did to them.
local count = nv.load("count", 0)       -- kept across restarts

local function set(v)
  count = v
  nv.save("count", count)
end

function nv.draw()
  ui.clear()
  local back = ui.header(nv.tr("My App", "La mia app"))
  if back then nv.exit() end
  ui.label(ui.W / 2, ui.S(170), tostring(count), ui.font.giant, ui.theme.fg, "center")
  local cell = ui.grid(ui.W / 2 - ui.S(250), ui.S(330), ui.S(500), ui.S(80), 3, 1, ui.S(16))
  local x, y, w, h = cell(1, 1)
  if ui.button(x, y, w, h, nil, { style = "outline", icon = "minus" }) then set(count - 1) end
  x, y, w, h = cell(2, 1)
  if ui.button(x, y, w, h, nv.tr("Reset", "Azzera"), { style = "flat", color = ui.theme.panel2 }) then set(0) end
  x, y, w, h = cell(3, 1)
  if ui.button(x, y, w, h, nil, { icon = "plus" }) then set(count + 1); nv.beep() end
end

function nv.key(ev)
  if not ev.down then return end
  if ev.key == "up" or ev.text == "+" then set(count + 1)
  elseif ev.key == "down" or ev.text == "-" then set(count - 1) end
  nv.redraw()
end
