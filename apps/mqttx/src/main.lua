-- MQTT Explorer - watch the topics on your broker, read their payloads (JSON pretty-printed),
-- keep a short history per topic and publish test messages. Uses the OS MQTT connection
-- (Settings > Home), so no address or password lives in the app. For NucleoOS (Lua App engine).
local T, S = ui.theme, ui.S
local filters = nv.load("filters", { "zigbee2mqtt/#", "tele/#", "stat/#", "shellies/#", "esphome/#", "nucleo-demo/#" })
local topics, order = {}, {}       -- topic -> {payload, t, n, hist}
local sel = nil
local err = nil
local total = 0
local paused = false

local function on_msg(topic, payload)
  if paused then return end
  total = total + 1
  local e = topics[topic]
  if not e then
    e = { n = 0, hist = {} }
    topics[topic] = e
    order[#order + 1] = topic
    table.sort(order)
  end
  e.n = e.n + 1
  e.payload, e.t = payload, nv.time()
  table.insert(e.hist, 1, { t = e.t, p = payload })
  if #e.hist > 8 then e.hist[9] = nil end
end

local function subscribe_all()
  err = nil
  for _, f in ipairs(filters) do
    local ok, why = nv.mqtt.sub(f, on_msg)
    if not ok then err = (why == "not configured") and nv.tr("MQTT is not set up: Settings > Home", "MQTT non configurato: Impostazioni > Casa")
                         or (f .. ": " .. tostring(why)) end
  end
end

-- JSON payloads as indented lines; anything else as wrapped text
local function pretty(p, w)
  local ok, v = pcall(json.decode, p)
  if not ok or type(v) ~= "table" then return ui.wrap(p, ui.font.small, w) end
  local lines = {}
  local function walk(x, ind, key)
    local pre = string.rep("  ", ind) .. (key and (key .. ": ") or "")
    if type(x) == "table" then
      local keys = {}
      for k in pairs(x) do keys[#keys + 1] = k end
      table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
      if #keys == 0 then lines[#lines + 1] = pre .. "{}" return end
      lines[#lines + 1] = pre
      for _, k in ipairs(keys) do walk(x[k], ind + 1, tostring(k)) end
    else
      lines[#lines + 1] = pre .. tostring(x)
    end
  end
  walk(v, -1, nil)
  if lines[1] == "" then table.remove(lines, 1) end
  return lines
end

function nv.init() subscribe_all() end

local function ago(t)
  local d = nv.time() - t
  if d < 60 then return d .. " s" elseif d < 3600 then return (d // 60) .. " min" end
  return (d // 3600) .. " h"
end

local mode = "topics"   -- or "filters"
function nv.draw()
  ui.clear()
  local back, act = ui.header("MQTT Explorer", { action = mode == "topics" and nv.tr("Filters", "Filtri") or nv.tr("Done", "Fatto") })
  if back then nv.exit() end
  if act then mode = mode == "topics" and "filters" or "topics"; nv.redraw() end
  local top = S(64)
  if mode == "filters" then
    local items = {}
    for i, f in ipairs(filters) do items[i] = { title = f, right = nv.tr("remove", "togli") } end
    local i = ui.list("filters", 0, top, ui.W, ui.H - top - S(84), items, { row_h = S(60) })
    if i then table.remove(filters, i); nv.save("filters", filters) end
    if ui.button(S(16), ui.H - S(72), S(300), S(56), nv.tr("Add filter", "Aggiungi filtro"), { icon = "plus" }) then
      ui.prompt(nv.tr("Topic filter (+ and # wildcards)", "Filtro topic (jolly + e #)"), "", function(f)
        if f and f ~= "" then filters[#filters + 1] = f; nv.save("filters", filters); nv.mqtt.sub(f, on_msg) end
      end)
    end
    gfx.text(S(340), ui.H - S(58), nv.tr("\"#\" alone is not allowed by the OS.", "\"#\" da solo non è ammesso dal sistema."), ui.font.small, T.dim)
    return
  end
  -- topic list
  local lw = S(420)
  local items = {}
  for i, t in ipairs(order) do
    local e = topics[t]
    local p = e.payload:gsub("%s+", " ")
    if #p > 40 then p = p:sub(1, 40) .. "…" end
    items[i] = { title = t, sub = p, right = e.n > 1 and tostring(e.n) or nil }
  end
  if #items == 0 then
    ui.paragraph(S(24), top + S(24), lw - S(40), err or nv.tr("Waiting for messages on: ", "In attesa di messaggi su: ") ..
                 table.concat(filters, ", "), ui.font.body, err and T.err or T.dim)
  else
    local selected
    for i, t in ipairs(order) do if t == sel then selected = i end end
    local i = ui.list("topics", 0, top, lw, ui.H - top - S(60), items, { row_h = S(64), selected = selected, size = ui.font.small })
    if i then sel = order[i] end
  end
  gfx.rect(lw, top, 1, ui.H - top, T.line)
  gfx.text(S(16), ui.H - S(44), string.format(nv.tr("%d topics, %d messages", "%d topic, %d messaggi"), #order, total), ui.font.small, T.dim)
  if ui.button(lw - S(130), ui.H - S(54), S(118), S(44), paused and nv.tr("Resume", "Riprendi") or nv.tr("Pause", "Pausa"),
               { style = "outline", size = ui.font.small }) then paused = not paused end
  -- detail
  local dx, dw = lw + S(20), ui.W - lw - S(36)
  local e = sel and topics[sel]
  if not e then
    ui.paragraph(dx, top + S(24), dw, nv.tr("Tap a topic to see its payload and history. Publish sends a message to any topic you choose.",
      "Tocca un topic per vederne il contenuto e la cronologia. Pubblica invia un messaggio al topic che scegli."), ui.font.body, T.dim)
  else
    ui.paragraph(dx, top + S(14), dw, sel, ui.font.body, T.accent)
    gfx.text(dx, top + S(48), nv.tr("received ", "ricevuto ") .. ago(e.t) .. nv.tr(" ago", " fa") .. "  ·  " .. #e.payload .. " B", ui.font.small, T.dim)
    local y = top + S(80)
    gfx.clip(dx, y, dw, ui.H - y - S(140))
    for _, l in ipairs(pretty(e.payload, dw)) do
      gfx.text(dx, y, l, ui.font.small, T.fg)
      y = y + gfx.font_height(ui.font.small)
    end
    gfx.clip()
    local hy = ui.H - S(132)
    gfx.rect(dx, hy - S(8), dw, 1, T.line)
    gfx.text(dx, hy, nv.tr("History", "Cronologia"), ui.font.small, T.dim)
    for k = 2, math.min(4, #e.hist) do
      local h = e.hist[k]
      local p = h.p:gsub("%s+", " ")
      if #p > 60 then p = p:sub(1, 60) .. "…" end
      gfx.text(dx, hy + (k - 1) * S(24), nv.date("%H:%M:%S", h.t) .. "  " .. p, ui.font.small, T.fg)
    end
  end
  if ui.button(ui.W - S(196), ui.H - S(54), S(180), S(44), nv.tr("Publish", "Pubblica"), { size = ui.font.small }) then
    ui.prompt(nv.tr("Topic", "Topic"), sel or "nucleo-demo/test", function(t)
      if not t or t == "" then return end
      ui.prompt(nv.tr("Payload for ", "Contenuto per ") .. t, e and e.payload or "", function(p)
        if p == nil then return end
        local ok, why = nv.mqtt.pub(t, p, false)
        nv.toast(ok and nv.tr("Published", "Pubblicato") or (nv.tr("Not sent: ", "Non inviato: ") .. tostring(why)), ok and "ok" or "error")
      end)
    end)
  end
end

function nv.back()
  if mode ~= "topics" then mode = "topics"; return true end
  if sel then sel = nil; return true end
  return false
end
