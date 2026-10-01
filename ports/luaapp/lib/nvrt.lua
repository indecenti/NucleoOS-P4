-- nvrt.lua - the Lua App runtime: frame loop, input events, timers, storage, network.
-- Compiled into the engine (ports/luaapp); apps see the global tables `nv`, `gfx`, `ui`, `json`.
-- API reference: docs/LUA_APPS.md
local _nv, gfx = _nv, gfx
local info = _nv.info()
json = require "json"

nv = {}
nv.app = info.id
nv.sideloaded = info.sideloaded
nv.engine = info.engine
nv.W, nv.H = gfx.width(), gfx.height()
local LANG = _nv.lang()
nv.it = LANG == "it"
function nv.lang() return LANG end
-- nv.tr{en="Hello", it="Ciao"} or nv.tr("Hello", "Ciao")
function nv.tr(en, it)
  if type(en) == "table" then return en[LANG] or en.en or next(en) and en[next(en)] end
  return (LANG == "it" and it) or en
end

-- ---- drawing policy --------------------------------------------------------------------------
local dirty, continuous = true, false
function nv.redraw() dirty = true end
function nv.continuous(on) continuous = (on ~= false) end   -- call nv.draw every frame (games)
nv.fps = 0

-- ---- time ------------------------------------------------------------------------------------
nv.millis = _nv.millis
function nv.clock() return _nv.millis() / 1000 end
function nv.time() return _nv.time() end
local TZFILE = "/engine/utc_offset"
local tz
function nv.utc_offset()
  if tz == nil then
    tz = 0
    local f = io.open(TZFILE, "r") or io.open(info.data .. "utc_offset", "r")
    if f then tz = tonumber(f:read("l") or "") or 0; f:close() end
  end
  return tz
end
-- shared by every Lua app (the engine's folder); weather apps set it from Open-Meteo's answer
function nv.set_utc_offset(sec)
  tz = math.floor(tonumber(sec) or 0)
  for _, p in ipairs({ TZFILE, info.data .. "utc_offset" }) do
    local f = io.open(p, "w")
    if f then f:write(tostring(tz)); f:close() end
  end
end
-- local time: nv.date("%H:%M"), nv.date("*t")
function nv.date(fmt, t)
  fmt = fmt or "%Y-%m-%d %H:%M"
  return os.date("!" .. fmt, (t or _nv.time()) + nv.utc_offset())
end

-- ---- timers ----------------------------------------------------------------------------------
local timers, tseq = {}, 0
local function add_timer(sec, fn, rep)
  tseq = tseq + 1
  timers[tseq] = { at = _nv.millis() + sec * 1000, every = rep and sec * 1000 or nil, fn = fn }
  return tseq
end
function nv.after(sec, fn) return add_timer(sec, fn, false) end
function nv.every(sec, fn) return add_timer(math.max(sec, 0.01), fn, true) end
function nv.cancel(id) if id then timers[id] = nil end end
local function run_timers(now)
  for id, t in pairs(timers) do
    if now >= t.at then
      if t.every then t.at = math.max(t.at + t.every, now) else timers[id] = nil end
      t.fn()
      dirty = true
    end
  end
end

-- ---- storage ---------------------------------------------------------------------------------
-- nv.save(key, value) / nv.load(key, default): a JSON file in the app's private folder.
local store_file
do
  local tag = nv.sideloaded and ("store-" .. (info.dir:gsub("[^%w]", "_")) .. ".json") or "store.json"
  store_file = info.data .. tag
end
local store
local function store_load()
  if store then return store end
  store = {}
  local f = io.open(store_file, "r")
  if f then
    local ok, t = pcall(json.decode, f:read("a"))
    f:close()
    if ok and type(t) == "table" then store = t end
  end
  return store
end
function nv.load(key, default)
  local v = store_load()[key]
  if v == nil then return default end
  return v
end
function nv.save(key, value)
  store_load()[key] = value
  local f = io.open(store_file .. ".tmp", "w")
  if not f then return false end
  f:write(json.encode(store))
  f:close()
  os.remove(store_file)
  return os.rename(store_file .. ".tmp", store_file)
end
-- a file path in the app's private folder (for io.open)
function nv.path(name) return info.data .. name end
-- a file shipped in the app's bundle (string or nil)
nv.read = _nv.res
nv.files = _nv.res_list

-- ---- feedback --------------------------------------------------------------------------------
function nv.toast(msg, kind) _nv.toast(tostring(msg), ({ info = 0, ok = 1, warn = 2, error = 3 })[kind] or 0) end
function nv.log(...)
  local t = {}
  for i = 1, select("#", ...) do t[i] = tostring((select(i, ...))) end
  _nv.log(table.concat(t, " "), 1)
end
print = nv.log
nv.tone = _nv.tone
function nv.beep() _nv.tone(1760, 40) end
nv.sound = _nv.sound
nv.speak = _nv.speak
nv.backlight = _nv.backlight
-- nv.melody{ {440,200}, {0,100}, {660,200} }  (Hz, ms; 0 = rest)
function nv.melody(notes)
  local t = 0
  for _, n in ipairs(notes) do
    local f, ms = n[1], n[2]
    if f and f > 0 then nv.after(t / 1000, function() _nv.tone(f, ms) end) end
    t = t + ms
  end
end
local quitting = false
function nv.exit() quitting = true; _nv.exit() end
os.exit = function() nv.exit() end
function nv.random(a, b)
  if a == nil then return (_nv.rand() % 1000000) / 1000000 end
  if b == nil then a, b = 1, a end
  return a + _nv.rand() % (b - a + 1)
end
math.randomseed(_nv.rand(), _nv.millis())

-- ---- input -----------------------------------------------------------------------------------
-- pointer state the ui library reads; touch events go to nv.touch(ev), keys to nv.key(ev)
local ptr = { down = false, x = 0, y = 0, sx = 0, sy = 0, pressed = false, released = false,
              moved = false, t0 = 0, wheel = 0 }
nv.pointer = ptr
local fingers = {}            -- index -> {x, y, sx, sy}
local held = {}               -- key name -> true
function nv.down(k) return held[k] == true end

local function emit(name, ev)
  dirty = true
  if name == "key" and ui._key(ev) then return end
  if name == "touch" and ui._modal then return end
  local h = nv[name]
  local used = h and h(ev)
  dirty = true
  -- Escape not taken by the app (its nv.key did not return true) works as the back gesture
  if name == "key" and ev.down and ev.key == "escape" and used ~= true then nv._back() end
end

local function poll_touch(now)
  local r = { _nv.touches() }
  local n = r[1] or 0
  local seen = {}
  local k = 0
  for i = 1, n do
    local x, y = r[2 * i], r[2 * i + 1]
    if x then
      k = k + 1
      seen[k] = true
      local f = fingers[k]
      if not f then
        f = { x = x, y = y, sx = x, sy = y }
        fingers[k] = f
        emit("touch", { type = "down", id = k, x = x, y = y, sx = x, sy = y, dx = 0, dy = 0 })
      elseif f.x ~= x or f.y ~= y then
        local dx, dy = x - f.x, y - f.y
        f.x, f.y = x, y
        emit("touch", { type = "move", id = k, x = x, y = y, sx = f.sx, sy = f.sy, dx = dx, dy = dy })
      end
    end
  end
  for id, f in pairs(fingers) do
    if not seen[id] then
      fingers[id] = nil
      emit("touch", { type = "up", id = id, x = f.x, y = f.y, sx = f.sx, sy = f.sy, dx = 0, dy = 0 })
      if id == 1 and math.abs(f.x - f.sx) < 24 and math.abs(f.y - f.sy) < 24 and nv.tap then
        nv.tap(f.x, f.y)
      end
    end
  end
  -- primary pointer for the ui library
  local f = fingers[1]
  ptr.pressed, ptr.released = false, false
  if f and not ptr.down then
    ptr.down, ptr.pressed, ptr.sx, ptr.sy, ptr.moved, ptr.t0 = true, true, f.x, f.y, false, now
  elseif not f and ptr.down then
    ptr.down, ptr.released = false, true
  end
  if f then
    ptr.x, ptr.y = f.x, f.y
    if math.abs(f.x - ptr.sx) > 16 or math.abs(f.y - ptr.sy) > 16 then ptr.moved = true end
  end
  if ptr.down or ptr.released then dirty = true end
end

-- HID usage -> key name (US layout) and the character it types
local KEYS = {}
do
  for i = 0, 25 do KEYS[4 + i] = string.char(97 + i) end
  for i = 1, 9 do KEYS[29 + i] = tostring(i) end
  KEYS[39] = "0"
  local named = { [40] = "enter", [41] = "escape", [42] = "backspace", [43] = "tab", [44] = "space",
    [45] = "-", [46] = "=", [47] = "[", [48] = "]", [49] = "\\", [51] = ";", [52] = "'", [53] = "`",
    [54] = ",", [55] = ".", [56] = "/", [57] = "capslock", [73] = "insert", [74] = "home",
    [75] = "pageup", [76] = "delete", [77] = "end", [78] = "pagedown", [79] = "right", [80] = "left",
    [81] = "down", [82] = "up", [84] = "kp/", [85] = "kp*", [86] = "kp-", [87] = "kp+",
    [88] = "kpenter", [99] = "kp." }
  for k, v in pairs(named) do KEYS[k] = v end
  for i = 1, 12 do KEYS[57 + i] = "f" .. i end
  for i = 1, 9 do KEYS[88 + i] = "kp" .. i end
  KEYS[98] = "kp0"
end
local SHIFTED = { ["1"] = "!", ["2"] = "@", ["3"] = "#", ["4"] = "$", ["5"] = "%", ["6"] = "^",
  ["7"] = "&", ["8"] = "*", ["9"] = "(", ["0"] = ")", ["-"] = "_", ["="] = "+", ["["] = "{",
  ["]"] = "}", ["\\"] = "|", [";"] = ":", ["'"] = "\"", ["`"] = "~", [","] = "<", ["."] = ">", ["/"] = "?" }
local function key_text(name, shift)
  if #name == 1 then
    if name:match("%a") then return shift and name:upper() or name end
    return shift and SHIFTED[name] or name
  end
  if name == "space" then return " " end
  if name:match("^kp[%d%.%+%-%*/]$") then return name:sub(3) end
  return nil
end
local kb_prev = {}
local function poll_keys()
  local r = { _nv.kbd() }
  if #r == 0 then return end
  local mod = r[1]
  local shift, ctrl, alt = (mod & 0x22) ~= 0, (mod & 0x11) ~= 0, (mod & 0x44) ~= 0
  local now = {}
  for i = 2, #r do now[r[i]] = true end
  for u in pairs(now) do
    if not kb_prev[u] and KEYS[u] then
      local name = KEYS[u]
      held[name] = true
      emit("key", { key = name, down = true, text = (not ctrl and not alt) and key_text(name, shift) or nil,
                    shift = shift, ctrl = ctrl, alt = alt })
    end
  end
  for u in pairs(kb_prev) do
    if not now[u] and KEYS[u] then
      held[KEYS[u]] = nil
      emit("key", { key = KEYS[u], down = false, shift = shift, ctrl = ctrl, alt = alt })
    end
  end
  kb_prev = now
end
-- game controllers (ABI 11): buttons -> key names, d-pad and left stick -> arrows
local PADB = { { 1, "pad_a" }, { 2, "pad_b" }, { 4, "pad_x" }, { 8, "pad_y" }, { 16, "pad_select" },
  { 64, "pad_start" }, { 512, "pad_l" }, { 1024, "pad_r" }, { 2048, "up" }, { 4096, "down" },
  { 8192, "left" }, { 16384, "right" } }
local pad_prev = 0
local function poll_pads()
  local n = _nv.pad_count()
  local bits = 0
  for i = 0, (n or 0) - 1 do
    local b, lx, ly = _nv.pad_state(i)
    if b then
      bits = bits | b
      if lx < -16000 then bits = bits | 8192 elseif lx > 16000 then bits = bits | 16384 end
      if ly < -16000 then bits = bits | 2048 elseif ly > 16000 then bits = bits | 4096 end
    end
  end
  if bits == pad_prev then return end
  for _, p in ipairs(PADB) do
    local was, is = (pad_prev & p[1]) ~= 0, (bits & p[1]) ~= 0
    if is ~= was then
      if is then held[p[2]] = true else held[p[2]] = nil end
      emit("key", { key = p[2], down = is, pad = true })
    end
  end
  pad_prev = bits
end

-- ---- network ---------------------------------------------------------------------------------
-- Every call is asynchronous: the callback runs from the frame loop when the answer is in.
local NETERR = { [-1] = "permission", [-2] = "bad request", [-3] = "busy", [-4] = "destination refused",
  [-5] = "connect failed", [-6] = "too big", [-7] = "timeout", [-8] = "closed", [-9] = "not configured" }
nv.net_error = NETERR
local pending, queue = {}, {}
local function finish(p, h)
  local st = _nv.http_state(h)
  local res
  if st == 1 then
    local body = _nv.http_body(h)
    local status = _nv.http_status(h)
    res = { ok = status >= 200 and status < 300, status = status, body = body }
  else
    res = { ok = false, status = 0, body = "", error = NETERR[st] or ("error " .. tostring(st)) }
  end
  _nv.http_close(h)
  function res.json() local ok, v = pcall(json.decode, res.body); if ok then return v end end
  if p.cb then p.cb(res) end
end
local function submit(p)
  local h = p.start()
  if h == -3 then table.insert(queue, p); return end        -- 4 handles busy: retry next frame
  if h < 0 then
    local res = { ok = false, status = 0, body = "", error = NETERR[h] or ("error " .. h), json = function() end }
    if p.cb then nv.after(0, function() p.cb(res) end) end
    return
  end
  pending[h] = p
end
local function poll_http()
  for h, p in pairs(pending) do
    if _nv.http_state(h) ~= 0 then pending[h] = nil; finish(p, h); dirty = true end
  end
  if #queue > 0 then local q = queue; queue = {}; for _, p in ipairs(q) do submit(p) end end
end
-- nv.http{url=, method="GET", headers={}, body=, timeout=ms, max=bytes}, function(res) ... end
-- res = {ok, status, body, error, json()}
function nv.http(opts, cb)
  if type(opts) == "string" then opts = { url = opts } end
  local body = opts.body
  if type(body) == "table" then
    body = json.encode(body)
    opts.headers = opts.headers or {}
    opts.headers["Content-Type"] = opts.headers["Content-Type"] or "application/json"
  end
  local spec = json.encode({ url = opts.url, method = opts.method, headers = opts.headers,
                             timeout = opts.timeout, max = opts.max })
  submit({ cb = cb, start = function() return _nv.http_req(spec, body) end })
end
function nv.get(url, cb) nv.http({ url = url }, cb) end
-- nv.get_json(url, function(data, err) ... end)
function nv.get_json(url, cb)
  nv.http({ url = url, max = 262144 }, function(r)
    if not r.ok then return cb(nil, r.error or ("HTTP " .. r.status)) end
    local v = r.json()
    if v == nil then return cb(nil, "bad JSON") end
    cb(v)
  end)
end

-- WebSocket: local s = nv.ws(url[, headers]); s.on_open, s.on_message(msg), s.on_close; s:send(text)
local sockets = {}
local WS = {}
WS.__index = WS
function WS:send(msg, binary) return _nv.ws_send(self.h, type(msg) == "table" and json.encode(msg) or msg, binary) end
function WS:close() if self.h >= 0 then _nv.ws_close(self.h); sockets[self.h] = nil end self.closed = true end
local function new_ws(h)
  local s = setmetatable({ h = h, open = false, closed = h < 0, error = h < 0 and NETERR[h] or nil }, WS)
  if h >= 0 then sockets[h] = s end
  return s
end
function nv.ws(url, headers) return new_ws(_nv.ws_open(url, headers and json.encode(headers) or nil)) end
local function poll_ws()
  for h, s in pairs(sockets) do
    local st = _nv.ws_state(h)
    if st == 1 and not s.open then s.open = true; dirty = true; if s.on_open then s.on_open(s) end end
    for _ = 1, 16 do
      local m = _nv.ws_recv(h)
      if not m then break end
      dirty = true
      if s.on_message then s.on_message(m, s) end
    end
    if st < 0 then
      sockets[h] = nil
      _nv.ws_close(h)
      s.closed, s.open = true, false
      dirty = true
      if s.on_close then s.on_close(s) end
    end
  end
end

-- MQTT through the OS connection
nv.mqtt = {}
local subs = {}
local function topic_match(filter, topic)
  if filter == topic then return true end
  local fp, tp = {}, {}
  for p in (filter .. "/"):gmatch("([^/]*)/") do fp[#fp + 1] = p end
  for p in (topic .. "/"):gmatch("([^/]*)/") do tp[#tp + 1] = p end
  for i, p in ipairs(fp) do
    if p == "#" then return true end
    if p ~= "+" and p ~= tp[i] then return false end
  end
  return #fp == #tp
end
function nv.mqtt.sub(filter, cb)
  local r = _nv.mqtt_sub(filter)
  if r < 0 then return false, NETERR[r] end
  subs[#subs + 1] = { f = filter, cb = cb }
  return true
end
function nv.mqtt.pub(topic, payload, retain)
  if type(payload) == "table" then payload = json.encode(payload) end
  local r = _nv.mqtt_pub(topic, tostring(payload), retain)
  if r < 0 then return false, NETERR[r] end
  return true
end
local function poll_mqtt()
  if #subs == 0 then return end
  for _ = 1, 32 do
    local t, p = _nv.mqtt_recv()
    if not t then break end
    dirty = true
    for _, s in ipairs(subs) do if topic_match(s.f, t) and s.cb then s.cb(t, p) end end
  end
end

-- Home Assistant (token kept by the OS, Settings > Home)
nv.ha = {}
nv.ha.available = _nv.ha_available
function nv.ha.req(method, path, body, cb)
  if type(body) == "table" then body = json.encode(body) end
  submit({ cb = cb, start = function() return _nv.ha_req(method, path, body) end })
end
-- nv.ha.states(function(list, err)) ; nv.ha.state("light.kitchen", cb) ; nv.ha.call("light", "toggle", {entity_id=...}, cb)
function nv.ha.states(cb)
  nv.ha.req("GET", "/api/states", nil, function(r) if r.ok then cb(r.json()) else cb(nil, r.error or r.status) end end)
end
function nv.ha.state(entity, cb)
  nv.ha.req("GET", "/api/states/" .. entity, nil, function(r) if r.ok then cb(r.json()) else cb(nil, r.error or r.status) end end)
end
function nv.ha.call(domain, service, data, cb)
  nv.ha.req("POST", "/api/services/" .. domain .. "/" .. service, data or {}, function(r) if cb then cb(r.ok, r) end end)
end
function nv.ha.ws() return new_ws(_nv.ha_ws()) end

-- mDNS: nv.mdns("_http", "_tcp", function(list)) ; list[i] = {name, host, ip, port, txt={}}
function nv.mdns(service, proto, cb)
  submit({ start = function() return _nv.mdns(service, proto or "_tcp") end, cb = function(r)
    local out = {}
    for line in (r.body or ""):gmatch("[^\n]+") do
      local name, host, ip, port, txt = line:match("^([^|]*)|([^|]*)|([^|]*)|([^|]*)|?(.*)$")
      if name then
        local t = {}
        for k, v in (txt or ""):gmatch("([^=;]+)=([^;]*)") do t[k] = v end
        out[#out + 1] = { name = name, host = host, ip = ip, port = tonumber(port), txt = t }
      end
    end
    cb(out, r.error)
  end })
end

-- ---- frame loop (called by the engine) --------------------------------------------------------
ui = require "ui"
local last, fcount, ftime = _nv.millis(), 0, 0

function nv._back()
  if not ui._back() and not (nv.back and nv.back()) then nv.exit() end
  dirty = true
end

function nv._start(lib_main)
  if lib_main ~= "" then
    require(lib_main)
  else
    -- LOVE games run through the compatibility layer (lib/love.lua)
    setmetatable(_G, { __index = function(t, k)
      if k == "love" then local l = require "love"; rawset(t, "love", l); return l end
    end })
    local src = _nv.res(info.main)
    if not src then error("the app has no " .. info.main) end
    local fn, err = load(src, "@" .. info.main, "t")
    if not fn then error(err, 0) end
    fn()
    setmetatable(_G, nil)
    local love = rawget(_G, "love")
    if love and love._bridge then love._bridge() end
  end
  if nv.init then nv.init() end
  dirty = true
end

function nv._frame()
  local now = _nv.millis()
  local dt = (now - last) / 1000
  if dt > 0.1 then dt = 0.1 end
  last = now
  fcount, ftime = fcount + 1, ftime + dt
  if ftime >= 1 then nv.fps, fcount, ftime = fcount, 0, 0 end
  poll_touch(now)
  poll_keys()
  poll_pads()
  for _ = 1, _nv.back() do nv._back() end
  if quitting then return end
  run_timers(now)
  poll_http()
  poll_ws()
  poll_mqtt()
  if nv.update then nv.update(dt) end
  if (dirty or continuous) and nv.draw then
    dirty = false
    ui._begin()
    nv.draw()
    ui._end()
  end
end

function nv._stop()
  if nv.quit then nv.quit() end
  for h in pairs(sockets) do _nv.ws_close(h) end
end
