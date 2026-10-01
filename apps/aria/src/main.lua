-- Aria - air quality, UV and pollen where you are, from Open-Meteo's free Air Quality API
-- (CAMS data, no key; https://open-meteo.com, data CC BY 4.0). For NucleoOS (Lua App engine).
-- First start: the position comes from ip-api.com; "Place" searches any city by name.
local T, S = ui.theme, ui.S
local place = nv.load("place")          -- {name=, lat=, lon=}
local data = nv.load("data")            -- last answer, shown while offline
local status = nil
local busy = false
local choices = nil                     -- city search results

local LEVELS = {   -- European AQI bands
  { 20, 0x50F0E6, nv.tr("Good", "Buona") }, { 40, 0x50CCAA, nv.tr("Fair", "Discreta") },
  { 60, 0xF0E641, nv.tr("Moderate", "Moderata") }, { 80, 0xFF5050, nv.tr("Poor", "Scadente") },
  { 100, 0x960032, nv.tr("Very poor", "Pessima") }, { 1e9, 0x7D2181, nv.tr("Extremely poor", "Molto pessima") },
}
local function level(aqi)
  for _, l in ipairs(LEVELS) do if aqi <= l[1] then return l end end
  return LEVELS[#LEVELS]
end
local POLLEN = { alder_pollen = nv.tr("Alder", "Ontano"), birch_pollen = nv.tr("Birch", "Betulla"),
  grass_pollen = nv.tr("Grass", "Graminacee"), mugwort_pollen = nv.tr("Mugwort", "Artemisia"),
  olive_pollen = nv.tr("Olive", "Olivo"), ragweed_pollen = nv.tr("Ragweed", "Ambrosia") }

local function fetch()
  if not place then return end
  busy = true
  status = nv.tr("Updating...", "Aggiorno...")
  local url = string.format("https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.4f&longitude=%.4f" ..
    "&current=european_aqi,pm10,pm2_5,nitrogen_dioxide,ozone,uv_index,alder_pollen,birch_pollen,grass_pollen," ..
    "mugwort_pollen,olive_pollen,ragweed_pollen&hourly=european_aqi,uv_index&forecast_days=2&timezone=auto",
    place.lat, place.lon)
  nv.get_json(url, function(d, err)
    busy = false
    if not d or not d.current then status = nv.tr("No data: ", "Nessun dato: ") .. tostring(err); return end
    data = d
    data.fetched = nv.time()
    status = nil
    if d.utc_offset_seconds then nv.set_utc_offset(d.utc_offset_seconds) end
    nv.save("data", data)
  end)
end

local function locate()
  status = nv.tr("Finding your position...", "Cerco la tua posizione...")
  nv.get_json("http://ip-api.com/json/?fields=status,lat,lon,city", function(d, err)
    if d and d.status == "success" then
      place = { name = d.city, lat = d.lat, lon = d.lon }
      nv.save("place", place)
      fetch()
    else
      status = nv.tr("Position unknown: tap Place", "Posizione sconosciuta: tocca Luogo")
    end
  end)
end

local function search(name)
  if not name or name == "" then return end
  status = nv.tr("Searching...", "Cerco...")
  local q = name:gsub("[^%w%-%s]", ""):gsub("%s+", "+")
  nv.get_json("https://geocoding-api.open-meteo.com/v1/search?count=8&language=" .. nv.lang() .. "&name=" .. q,
    function(d, err)
      status = nil
      choices = {}
      for _, r in ipairs(d and d.results or {}) do
        choices[#choices + 1] = { title = r.name, sub = (r.admin1 or "") .. (r.country and (", " .. r.country) or ""),
                                  lat = r.latitude, lon = r.longitude }
      end
      if #choices == 0 then choices = nil; status = nv.tr("Place not found", "Luogo non trovato") end
    end)
end

function nv.init()
  if not place then locate() else fetch() end
  nv.every(1800, fetch)
end

local function card(r, title, value, unit, color)
  local x, y, w, h = r[1], r[2], r[3], r[4]
  ui.panel(x, y, w, h)
  gfx.text(x + S(16), y + S(12), title, ui.font.small, T.dim)
  local vw = gfx.text(x + S(16), y + h - S(56), value, ui.font.title, color or T.fg)
  if unit then gfx.text(x + S(22) + vw, y + h - S(44), unit, ui.font.small, T.dim) end
end

function nv.draw()
  ui.clear()
  local back, act = ui.header("Aria" .. (place and ("  ·  " .. place.name) or ""), { action = nv.tr("Place", "Luogo") })
  if back then nv.exit() end
  if act then ui.prompt(nv.tr("City", "Città"), "", search) end
  local top = S(64)
  if choices then
    gfx.text(S(24), top + S(14), nv.tr("Choose the place", "Scegli il luogo"), ui.font.body, T.dim)
    local i = ui.list("places", 0, top + S(52), ui.W, ui.H - top - S(52), choices, { row_h = S(70) })
    if i then
      local c = choices[i]
      place = { name = c.title, lat = c.lat, lon = c.lon }
      nv.save("place", place)
      choices = nil
      data = nil
      fetch()
    end
    return
  end
  if not data then
    ui.label(ui.W / 2, ui.H / 2 - S(20), status or nv.tr("Waiting for data", "In attesa dei dati"), ui.font.big, T.dim, "center")
    return
  end
  local c = data.current
  local aqi = c.european_aqi or 0
  local lv = level(aqi)
  -- big gauge
  local gx, gy, gr = S(190), top + S(200), S(140)
  ui.gauge(gx, gy, gr, math.min(1, aqi / 100), lv[2], S(22))
  gfx.text(gx, gy - S(50), tostring(math.floor(aqi + 0.5)), ui.font.giant, T.fg, "center")
  gfx.text(gx, gy + S(34), lv[3], ui.font.big, lv[2], "center")
  gfx.text(gx, gy + S(70), nv.tr("European AQI", "Indice europeo (AQI)"), ui.font.small, T.dim, "center")
  -- cards
  local cell = ui.grid(S(380), top + S(16), ui.W - S(396), S(250), 3, 2, S(12))
  card({ cell(1, 1) }, "PM2.5", string.format("%.0f", c.pm2_5 or 0), "µg/m³")
  card({ cell(2, 1) }, "PM10", string.format("%.0f", c.pm10 or 0), "µg/m³")
  card({ cell(3, 1) }, "NO2", string.format("%.0f", c.nitrogen_dioxide or 0), "µg/m³")
  card({ cell(1, 2) }, "O3", string.format("%.0f", c.ozone or 0), "µg/m³")
  local uv = c.uv_index or 0
  card({ cell(2, 2) }, nv.tr("UV index", "Indice UV"), string.format("%.1f", uv), nil,
       uv < 3 and T.ok or uv < 6 and T.warn or uv < 8 and 0xFF8A3D or T.err)
  local best, bestv = nil, 0
  for k, name in pairs(POLLEN) do if (c[k] or 0) > bestv then best, bestv = name, c[k] end end
  card({ cell(3, 2) }, nv.tr("Pollen", "Pollini"), best and string.format("%.0f", bestv) or "0",
       best and (best .. " gr/m³") or nil)
  -- next 24 hours of AQI
  local hx, hy, hw, hh = S(380), top + S(290), ui.W - S(396), ui.H - top - S(330)
  ui.panel(hx, hy, hw, hh)
  gfx.text(hx + S(16), hy + S(10), nv.tr("Next 24 hours", "Prossime 24 ore"), ui.font.small, T.dim)
  local h = data.hourly
  if h and h.time then
    local now = nv.date("%Y-%m-%dT%H:00")
    local start = 1
    for i, t in ipairs(h.time) do if t >= now then start = i; break end end
    local n = 24
    local bw = (hw - S(32)) / n
    local base = hy + hh - S(30)
    local maxh = hh - S(70)
    for i = 0, n - 1 do
      local v = h.european_aqi[start + i]
      if v then
        local bh = math.max(S(3), maxh * math.min(1, v / 100))
        gfx.rect(hx + S(16) + i * bw + 1, base - bh, bw - 2, bh, level(v)[2], S(3))
      end
      if i % 6 == 0 and h.time[start + i] then
        gfx.text(hx + S(16) + i * bw, base + S(4), h.time[start + i]:sub(12, 13) .. ":00", ui.font.small, T.dim)
      end
    end
  end
  local foot = status or (nv.tr("Updated ", "Aggiornato ") .. nv.date("%H:%M", data.fetched) ..
               "  ·  Open-Meteo / CAMS (CC BY 4.0)")
  gfx.text(S(24), ui.H - S(34), foot, ui.font.small, T.dim)
  if busy then ui.icon("refresh", ui.W - S(36), ui.H - S(24), S(22), T.dim) end
end

function nv.back()
  if choices then choices = nil; return true end
  return false
end
