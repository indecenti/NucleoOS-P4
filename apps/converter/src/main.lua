-- Converter - units for everyday and the workshop, for NucleoOS (Lua App engine).
-- Pick a quantity on the left, type a value, tap a unit to make it the one you type in:
-- every other unit of that quantity updates at once.
local T, S = ui.theme, ui.S
local it = nv.it

-- factor = how many base units one unit is; temperatures use functions
local Q = {
  { id = "length", en = "Length", it = "Lunghezza", units = {
    { "mm", 0.001 }, { "cm", 0.01 }, { "m", 1 }, { "km", 1000 }, { "in", 0.0254 }, { "ft", 0.3048 },
    { "yd", 0.9144 }, { "mi", 1609.344 }, { "NM", 1852 } } },
  { id = "mass", en = "Mass", it = "Massa", units = {
    { "mg", 1e-6 }, { "g", 0.001 }, { "kg", 1 }, { "t", 1000 }, { "oz", 0.028349523125 }, { "lb", 0.45359237 },
    { "st", 6.35029318 } } },
  { id = "temp", en = "Temperature", it = "Temperatura", units = {
    { "°C", function(v) return v end, function(v) return v end },
    { "°F", function(v) return (v - 32) * 5 / 9 end, function(c) return c * 9 / 5 + 32 end },
    { "K", function(v) return v - 273.15 end, function(c) return c + 273.15 end } } },
  { id = "volume", en = "Volume", it = "Volume", units = {
    { "ml", 0.001 }, { "cl", 0.01 }, { "l", 1 }, { "m³", 1000 }, { "tsp", 0.00492892 }, { "tbsp", 0.0147868 },
    { "cup", 0.24 }, { "fl oz", 0.0295735 }, { "gal US", 3.785411784 }, { "gal UK", 4.54609 } } },
  { id = "area", en = "Area", it = "Superficie", units = {
    { "cm²", 1e-4 }, { "m²", 1 }, { "ha", 1e4 }, { "km²", 1e6 }, { "ft²", 0.09290304 }, { "acre", 4046.8564224 } } },
  { id = "speed", en = "Speed", it = "Velocità", units = {
    { "m/s", 1 }, { "km/h", 1 / 3.6 }, { "mph", 0.44704 }, { "kn", 0.514444 }, { "ft/s", 0.3048 } } },
  { id = "pressure", en = "Pressure", it = "Pressione", units = {
    { "Pa", 1 }, { "hPa", 100 }, { "kPa", 1000 }, { "bar", 1e5 }, { "atm", 101325 }, { "psi", 6894.757 },
    { "mmHg", 133.322 } } },
  { id = "energy", en = "Energy", it = "Energia", units = {
    { "J", 1 }, { "kJ", 1000 }, { "cal", 4.184 }, { "kcal", 4184 }, { "Wh", 3600 }, { "kWh", 3.6e6 }, { "BTU", 1055.06 } } },
  { id = "data", en = "Data", it = "Dati", units = {
    { "bit", 0.125 }, { "B", 1 }, { "KB", 1e3 }, { "MB", 1e6 }, { "GB", 1e9 }, { "KiB", 1024 }, { "MiB", 1048576 },
    { "GiB", 1073741824 } } },
  { id = "time", en = "Time", it = "Tempo", units = {
    { "ms", 0.001 }, { "s", 1 }, { "min", 60 }, { "h", 3600 }, { "day", 86400 }, { "week", 604800 },
    { "year", 31557600 } } },
  { id = "angle", en = "Angle", it = "Angolo", units = {
    { "°", math.pi / 180 }, { "rad", 1 }, { "grad", math.pi / 200 }, { "turn", 2 * math.pi } } },
  { id = "fuel", en = "Fuel", it = "Consumi", units = {     -- base: km per litre
    { "km/l", function(v) return v end, function(b) return b end },
    { "l/100km", function(v) return 100 / v end, function(b) return 100 / b end },
    { "mpg US", function(v) return v * 0.425144 end, function(b) return b / 0.425144 end },
    { "mpg UK", function(v) return v * 0.354006 end, function(b) return b / 0.354006 end } } },
}

local qi = nv.load("q", 1)
if not Q[qi] then qi = 1 end
local from = nv.load("from_" .. Q[qi].id, 1)
local entry = nv.load("entry", "1")

local function to_base(u, v) if type(u[2]) == "function" then return u[2](v) end return v * u[2] end
local function from_base(u, b) if type(u[3]) == "function" then return u[3](b) end return b / u[2] end
local function fmt(v)
  if v ~= v or v == math.huge or v == -math.huge then return "-" end
  local a = math.abs(v)
  if a ~= 0 and (a >= 1e12 or a < 1e-6) then return string.format("%.6e", v) end
  -- about 9 significant digits, no exponent in the everyday range
  local int_digits = a >= 1 and math.floor(math.log(a, 10)) + 1 or 1
  local dec = math.max(0, math.min(10, 9 - int_digits + (a < 1 and a > 0 and -math.floor(math.log(a, 10)) - 1 or 0)))
  local s = string.format("%." .. dec .. "f", v)
  if s:find("%.") then s = s:gsub("0+$", ""):gsub("%.$", "") end
  if it then s = s:gsub("%.", ",") end
  return s
end

local function select_q(i)
  qi = i
  from = nv.load("from_" .. Q[qi].id, 1)
  nv.save("q", qi)
end

local function press(k)
  if k == "C" then entry = ""
  elseif k == "DEL" then entry = entry:sub(1, -2)
  elseif k == "±" then entry = entry:sub(1, 1) == "-" and entry:sub(2) or "-" .. entry
  elseif k == "." or k == "," then if not entry:find("%.") then entry = (entry == "" and "0" or entry) .. "." end
  elseif #entry < 14 then entry = entry .. k end
  nv.save("entry", entry)
end

function nv.draw()
  ui.clear()
  local back = ui.header(nv.tr("Converter", "Convertitore"))
  local top = S(64)
  -- quantities
  local items = {}
  for i, q in ipairs(Q) do items[i] = { title = it and q.it or q.en } end
  local i = ui.list("q", 0, top, S(230), ui.H - top, items, { row_h = S(52), selected = qi })
  if i then select_q(i) end
  gfx.rect(S(230), top, 1, ui.H - top, T.line)
  -- input + keypad
  local q = Q[qi]
  local u = q.units[from]
  local cx, cw = S(246), S(360)
  ui.panel(cx, top + S(14), cw, S(78), T.panel)
  local shown = entry == "" and "0" or (it and entry:gsub("%.", ",") or entry)
  gfx.text(cx + cw - S(90), top + S(30), shown, ui.font.title, T.fg, "right")
  gfx.text(cx + cw - S(18), top + S(36), u[1], ui.font.big, T.accent, "right")
  local k = ui.keys(cx, top + S(106), cw, ui.H - top - S(122),
    { { "7", "8", "9" }, { "4", "5", "6" }, { "1", "2", "3" }, { "±", "0", it and "," or "." }, { "C", "DEL:2" } },
    { gap = S(8), size = ui.font.big,
      style = function(l) if l == "C" then return { style = "flat", color = 0x5A2A30 } end
                          return { style = "flat", color = 0x2A3240 } end })
  if k then press(k) end
  -- results: every unit; tap one to type in it
  local v = tonumber(entry) or 0
  local b = to_base(u, v)
  local rx = cx + cw + S(16)
  local res = {}
  for j, uu in ipairs(q.units) do
    res[j] = { title = fmt(j == from and v or from_base(uu, b)), right = uu[1], color = j == from and T.accent or T.fg }
  end
  local j = ui.list("res_" .. q.id, rx, top + S(14), ui.W - rx - S(12), ui.H - top - S(28), res,
                    { row_h = S(56), selected = from, size = ui.font.big })
  if j and j ~= from then
    entry = fmt(from_base(q.units[j], b)):gsub(",", "."):gsub("e%+?", "e")
    if not tonumber(entry) then entry = "" end
    from = j
    nv.save("from_" .. q.id, from)
    nv.save("entry", entry)
  end
  if back then nv.exit() end
end

function nv.key(ev)
  if not ev.down then return end
  local t = ev.text
  if t and t:match("^[%d%.,]$") then press(t == "," and "." or t)
  elseif ev.key == "backspace" then press("DEL")
  elseif ev.key == "escape" or ev.key == "delete" then press("C"); return true
  elseif ev.key == "up" then select_q(qi > 1 and qi - 1 or #Q)
  elseif ev.key == "down" then select_q(qi < #Q and qi + 1 or 1)
  elseif t == "-" then press("±") end
end
