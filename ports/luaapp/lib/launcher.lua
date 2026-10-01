-- launcher.lua - what the "Lua App" tile shows: the Lua apps copied to /sdcard/home/lua, either as
-- folders (lua/<name>/main.lua + modules + img/) or single scripts (lua/<name>.lua). Tap one to run
-- it; the back gesture returns here. Store apps built on this engine have their own tiles.
local T = ui.theme
local S = ui.S
local apps = {}
local home = _nv.info().home
local ROOT = "/lua"

local function title_of(dir, name)
  local f = io.open(dir .. "/manifest.json", "r")
  if f then
    local ok, m = pcall(json.decode, f:read("a"))
    f:close()
    if ok and type(m) == "table" and m.name then return m.name end
  end
  return name
end

local function scan()
  apps = {}
  local list = _nv.ls(ROOT) or {}
  table.sort(list, function(a, b) return a.name:lower() < b.name:lower() end)
  for _, e in ipairs(list) do
    local path = ROOT .. "/" .. e.name
    if e.dir then
      local f = io.open(path .. "/main.lua", "r")
      if f then
        f:close()
        apps[#apps + 1] = { title = title_of(path, e.name), sub = "lua/" .. e.name .. "/", path = path }
      end
    elseif e.name:match("%.lua$") then
      apps[#apps + 1] = { title = e.name:gsub("%.lua$", ""), sub = "lua/" .. e.name, path = path }
    end
  end
end

function nv.init()
  scan()
end

function nv.draw()
  ui.clear()
  local back, refresh = ui.header("Lua App", { action = "refresh" })
  if back then nv.exit() end
  if refresh then scan() end
  local top = S(64)
  if not home then
    ui.paragraph(S(32), top + S(32), ui.W - S(64), nv.tr(
      "This engine runs the Lua apps of the Store. Install one from the Store: it gets its own tile.",
      "Questo motore fa girare le app Lua dello Store. Installane una dallo Store: avrà la sua icona."), ui.font.body, T.fg)
    return
  end
  if #apps == 0 then
    local y = top + S(40)
    y = y + ui.paragraph(S(40), y, ui.W - S(80), nv.tr(
      "No Lua apps yet. Copy a script to /sdcard/home/lua/hello.lua, or a folder with a main.lua to /sdcard/home/lua/<name>/, then tap refresh.",
      "Ancora nessuna app Lua. Copia uno script in /sdcard/home/lua/ciao.lua, o una cartella con main.lua in /sdcard/home/lua/<nome>/, poi tocca aggiorna."),
      ui.font.body, T.fg) + S(24)
    ui.paragraph(S(40), y, ui.W - S(80), nv.tr(
      "Ready-made Lua apps are in the Store (Tools, Home, Games). The API is in docs/LUA_APPS.md.",
      "Le app Lua pronte sono nello Store (Strumenti, Casa, Giochi). L'API è in docs/LUA_APPS.md."), ui.font.small, T.dim)
    return
  end
  local i = ui.list("apps", 0, top, ui.W, ui.H - top, apps, { row_h = S(76) })
  if i then _nv.launch(apps[i].path) end
end

