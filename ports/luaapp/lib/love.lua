-- love.lua - a LÖVE (love2d.org) compatibility layer for the Lua App engine: enough of
-- love.graphics / keyboard / mouse / touch / timer / audio / filesystem for small 2D games to run
-- unchanged or nearly so. Shapes, text, images (packed by tools/lua_pack.py), canvases, transforms.
-- Not there: shaders, meshes, quads, sprite batches, physics, rotation of images, TTF files (any
-- newFont() maps to the engine's Montserrat at that size), streamed music. Colours may be 0..1
-- (LÖVE 11) or 0..255 (LÖVE 0.10): both are accepted.
local love = {}
love._version = "11.4"
love.getVersion = function() return 11, 4, 0, "NucleoOS" end

local g = {}
love.graphics = g
local SW, SH = gfx.width(), gfx.height()     -- the canvas
local W, H = SW, SH                          -- the game's window (love.window.setMode)
local K, OX, OY = 1, 0, 0                    -- window -> canvas: scale to fit, centred
local function set_window(w, h)
  W, H = w or SW, h or SH
  K = math.min(SW / W, SH / H)
  if math.abs(K - 1) < 0.01 then K = 1 end
  OX, OY = math.floor((SW - W * K) / 2), math.floor((SH - H * K) / 2)
end
local function base() gfx.origin(); if K ~= 1 or OX ~= 0 or OY ~= 0 then gfx.translate(OX, OY); gfx.scale(K) end end
local function to_game(x, y) return (x - OX) / K, (y - OY) / K end
love._to_game = to_game

-- ---- colour state ------------------------------------------------------------------------------
local cur, cur_a = 0xFFFFFF, 255
local bg = 0x000000
local line_w, point_size = 1, 1
local function to_rgb(r, gg, b, a)
  if type(r) == "table" then r, gg, b, a = r[1], r[2], r[3], r[4] end
  r, gg, b = r or 0, gg or 0, b or 0
  local scale = (r <= 1 and gg <= 1 and b <= 1 and (a == nil or a <= 1)) and 255 or 1
  a = a and a * scale or 255
  return gfx.rgb(r * scale, gg * scale, b * scale), math.floor(a + 0.5)
end
function g.setColor(...) cur, cur_a = to_rgb(...) end
function g.getColor()
  return ((cur >> 16) & 255) / 255, ((cur >> 8) & 255) / 255, (cur & 255) / 255, cur_a / 255
end
function g.setBackgroundColor(...) bg = to_rgb(...) end
function g.getBackgroundColor() return ((bg >> 16) & 255) / 255, ((bg >> 8) & 255) / 255, (bg & 255) / 255, 1 end
function g.setLineWidth(w) line_w = w end
function g.getLineWidth() return line_w end
function g.setPointSize(s) point_size = s end
local target = nil
function g.clear(...)
  gfx.alpha(255)
  if select("#", ...) == 0 then
    if target then gfx.clear() else gfx.clear(bg) end
    return
  end
  local c, a = to_rgb(...)
  if target and a == 0 then gfx.clear() else gfx.clear(c) end
end
local function A() gfx.alpha(cur_a) end

-- ---- shapes --------------------------------------------------------------------------------------
function g.rectangle(mode, x, y, w, h, rx)
  A()
  if mode == "fill" then gfx.rect(x, y, w, h, cur, rx or 0)
  else gfx.frame(x - line_w / 2, y - line_w / 2, w + line_w, h + line_w, cur, line_w, rx or 0) end
end
function g.circle(mode, x, y, r)
  A()
  if mode == "fill" then gfx.circle(x, y, r, cur) else gfx.ring(x, y, r + line_w / 2, cur, line_w) end
end
function g.ellipse(mode, x, y, rx, ry)
  if not ry or math.abs(rx - ry) < 0.5 then return g.circle(mode, x, y, rx) end
  local pts, n = {}, 48
  for i = 0, n - 1 do
    local t = i / n * 2 * math.pi
    pts[#pts + 1] = x + math.cos(t) * rx
    pts[#pts + 1] = y + math.sin(t) * ry
  end
  g.polygon(mode, pts)
end
function g.arc(mode, a, ...)
  local x, y, r, a1, a2
  if type(a) == "string" then x, y, r, a1, a2 = ... else x, y, r, a1, a2 = a, ... end
  A()
  local d1, d2 = math.deg(a1), math.deg(a2)
  if mode == "fill" then gfx.arc(x, y, r, d1, d2, cur) else gfx.arc(x, y, r + line_w / 2, d1, d2, cur, line_w) end
end
local function flat(...)
  local t = ...
  if type(t) == "table" then return t end
  return { ... }
end
function g.line(...)
  local p = flat(...)
  A()
  for i = 1, #p - 3, 2 do gfx.line(p[i], p[i + 1], p[i + 2], p[i + 3], cur, line_w) end
end
function g.polygon(mode, ...)
  local p = flat(...)
  A()
  if mode == "fill" then gfx.poly(p, cur)
  else
    for i = 1, #p - 1, 2 do
      local j = (i + 2 > #p) and 1 or i + 2
      gfx.line(p[i], p[i + 1], p[j], p[j + 1], cur, line_w)
    end
  end
end
function g.points(...)
  local p = flat(...)
  A()
  for i = 1, #p - 1, 2 do
    if type(p[i]) == "table" then gfx.rect(p[i][1], p[i][2], point_size, point_size, cur)
    else gfx.rect(p[i] - point_size / 2, p[i + 1] - point_size / 2, point_size, point_size, cur) end
  end
end

-- ---- text ---------------------------------------------------------------------------------------
local Font = {}
Font.__index = Font
function Font:getWidth(s) return gfx.text_width(tostring(s), self.size) end
function Font:getHeight() return gfx.font_height(self.size) end
function Font:getLineHeight() return 1 end
function Font:getAscent() local _, a = gfx.font_height(self.size); return a end
function Font:getBaseline() return self:getAscent() end
function Font:setFilter() end
function Font:getWrap(text, limit)
  local lines = ui.wrap(text, self.size, limit)
  local w = 0
  for _, l in ipairs(lines) do w = math.max(w, gfx.text_width(l, self.size)) end
  return w, lines
end
function g.newFont(a, b)
  local size = type(a) == "number" and a or (b or 12)
  return setmetatable({ size = size }, Font)
end
g.setNewFont = function(...) local f = g.newFont(...); g.setFont(f); return f end
local font = g.newFont(12)
function g.setFont(f) font = f end
function g.getFont() return font end
function g.print(text, x, y, r, sx, sy, ox, oy)
  if type(text) == "table" then
    local t = {}
    for i = 2, #text, 2 do t[#t + 1] = tostring(text[i]) end
    text = table.concat(t)
  end
  x, y, sx = x or 0, y or 0, sx or 1
  sy = sy or sx
  A()
  if sx ~= 1 or sy ~= 1 or ox or oy then
    gfx.push(); gfx.translate(x, y); gfx.scale(sx, sy)
    gfx.text(-(ox or 0), -(oy or 0), tostring(text), font.size, cur)
    gfx.pop()
  else
    gfx.text(x, y, tostring(text), font.size, cur)
  end
end
function g.printf(text, x, y, limit, align, r, sx, sy)
  A()
  sx = sx or 1
  sy = sy or sx
  local lines = ui.wrap(tostring(text), font.size, limit)
  local lh = gfx.font_height(font.size)
  gfx.push(); gfx.translate(x, y); gfx.scale(sx, sy)
  for i, l in ipairs(lines) do
    local lx = 0
    if align == "center" then lx = limit / 2 elseif align == "right" then lx = limit end
    gfx.text(lx, (i - 1) * lh, l, font.size, cur, align == "center" and "center" or align == "right" and "right" or "left")
  end
  gfx.pop()
end

-- ---- images and canvases ----------------------------------------------------------------------
local Drawable = {}
Drawable.__index = Drawable
function Drawable:getWidth() return self.w end
function Drawable:getHeight() return self.h end
function Drawable:getDimensions() return self.w, self.h end
function Drawable:setFilter() end
function Drawable:setWrap() end
function Drawable:renderTo(fn) local prev = target; g.setCanvas(self); fn(); g.setCanvas(prev) end
local function wrap(surf, kind)
  local w, h = surf:size()
  return setmetatable({ surf = surf, w = w, h = h, kind = kind }, Drawable)
end
function g.newImage(path)
  local s, err = gfx.image(path)
  if not s then error(err, 2) end
  return wrap(s, "image")
end
function g.newCanvas(w, h) return wrap(gfx.surface(w or W, h or H), "canvas") end
function g.setCanvas(c)
  if type(c) == "table" and c.surf == nil then c = c[1] end
  if c and not target then gfx.push(); gfx.origin()        -- canvases are drawn unscaled
  elseif not c and target then gfx.pop() end
  target = c
  gfx.target(c and c.surf or nil)
end
function g.getCanvas() return target end
function g.draw(d, x, y, r, sx, sy, ox, oy)
  if type(d) ~= "table" or not d.surf then return end
  x, y, sx = x or 0, y or 0, sx or 1
  sy = sy or sx
  ox, oy = ox or 0, oy or 0
  gfx.alpha(cur_a)
  gfx.draw(d.surf, x - ox * sx, y - oy * sy, d.w * sx, d.h * sy)
end
function g.newQuad() error("love.graphics.newQuad is not supported by the Lua App engine", 2) end
function g.newShader() error("shaders are not supported by the Lua App engine", 2) end

-- ---- transform, state ---------------------------------------------------------------------------
g.push = function() gfx.push() end
g.pop = function() gfx.pop() end
g.translate = gfx.translate
g.scale = function(sx, sy) gfx.scale(sx, sy or sx) end
g.origin = function() if target then gfx.origin() else base() end end
g.rotate = function() end
function g.setScissor(x, y, w, h) if x then gfx.clip(x, y, w, h) else gfx.clip() end end
function g.getWidth() return W end
function g.getHeight() return H end
function g.getDimensions() return W, H end
for _, k in ipairs({ "setBlendMode", "setDefaultFilter", "setLineStyle", "setLineJoin", "present",
                     "captureScreenshot", "setShader", "setWireframe", "reset" }) do g[k] = function() end end

-- ---- window, system, event ----------------------------------------------------------------------
love.window = {}
for _, k in ipairs({ "setTitle", "setIcon", "setFullscreen", "setVSync", "maximize" }) do
  love.window[k] = function() return true end
end
-- a window bigger (or smaller) than the canvas is scaled to fit it, letterboxed
love.window.setMode = function(w, h) set_window(w, h); return true end
love.window.updateMode = love.window.setMode
love.window.getMode = function() return W, H, {} end
love.window.getDimensions = function() return W, H end
love.window.getDesktopDimensions = function() return W, H end
love.window.getDPIScale = function() return 1 end
love.window.isOpen = function() return true end
love.system = {}
-- "Android": games then show their touch controls
love.system.getOS = function() return "Android" end
love.system.getProcessorCount = function() return 2 end
love.event = { quit = function() nv.exit() end, push = function(e) if e == "quit" then nv.exit() end end }

-- ---- time, math -------------------------------------------------------------------------------
local last_dt = 0
love.timer = {
  getTime = function() return nv.clock() end,
  getDelta = function() return last_dt end,
  getFPS = function() return nv.fps end,
  sleep = function() end,
  step = function() return last_dt end,
}
love.math = {
  random = function(a, b) if a == nil then return math.random() elseif b == nil then return math.random(a) end return math.random(a, b) end,
  setRandomSeed = function(s) math.randomseed(s) end,
  noise = function(x, y) return (math.sin((x or 0) * 12.9898 + (y or 0) * 78.233) * 43758.5453) % 1 end,
}

-- ---- input --------------------------------------------------------------------------------------
local held = {}
local PADKEY = { pad_a = "space", pad_b = "lctrl", pad_x = "z", pad_y = "x", pad_start = "return",
                 pad_select = "escape", pad_l = "q", pad_r = "e" }
local function lkey(k) if k == "enter" then return "return" end return PADKEY[k] or k end
love.keyboard = {
  isDown = function(...)
    for i = 1, select("#", ...) do if held[(select(i, ...))] then return true end end
    return false
  end,
  setKeyRepeat = function() end,
  hasTextInput = function() return false end,
  setTextInput = function() end,
}
love.keyboard.isScancodeDown = love.keyboard.isDown
local touches = {}
love.touch = {
  getTouches = function() local t = {} for id in pairs(touches) do t[#t + 1] = id end return t end,
  getPosition = function(id) local t = touches[id]; if t then return t.x, t.y end end,
  getPressure = function() return 1 end,
}
love.mouse = {
  getPosition = function() return to_game(nv.pointer.x, nv.pointer.y) end,
  getX = function() return (to_game(nv.pointer.x, nv.pointer.y)) end,
  getY = function() local _, y = to_game(nv.pointer.x, nv.pointer.y); return y end,
  isDown = function(b) return (b == nil or b == 1) and nv.pointer.down end,
  setVisible = function() end, setCursor = function() end, getSystemCursor = function() end,
  setGrabbed = function() end, setRelativeMode = function() end, isVisible = function() return false end,
}

-- ---- audio: packed sounds play through the OS (snd/<name>.wav, one at a time) -----------------
local Source = {}
Source.__index = Source
function Source:play() if self.snd then nv.sound(self.snd) end self.playing = true return true end
function Source:stop() self.playing = false end
function Source:pause() self.playing = false end
function Source:isPlaying() return false end
function Source:clone() return setmetatable({ snd = self.snd }, Source) end
for _, k in ipairs({ "setVolume", "setLooping", "setPitch", "seek", "rewind", "setPosition" }) do Source[k] = function() end end
function Source:getVolume() return 1 end
-- "sounds/pop1.ogg" -> "sounds_pop1": the name tools/lua_pack.py gives the converted WAV
function love._sound_name(path) return (path:gsub("%.%w+$", ""):gsub("[^%w]", "_"):lower():sub(-24)) end
love.audio = {
  newSource = function(path) return setmetatable({ snd = type(path) == "string" and love._sound_name(path) or nil }, Source) end,
  play = function(s) if s and s.play then s:play() end end,
  stop = function() end, setVolume = function() end, pause = function() end,
}
love.sound = { newSoundData = function() return {} end }

-- ---- filesystem: reads from the bundle (or the save folder), writes to the save folder --------
local function save_path(n) return nv.path("love_" .. tostring(n):gsub("[^%w%._-]", "_")) end
love.filesystem = {
  read = function(name)
    local f = io.open(save_path(name), "rb")
    if f then local d = f:read("a"); f:close(); return d, #d end
    local d = nv.read(name)
    if d then return d, #d end
    return nil, "file not found: " .. tostring(name)
  end,
  write = function(name, data)
    local f = io.open(save_path(name), "wb")
    if not f then return false, "cannot write" end
    f:write(data); f:close()
    return true
  end,
  append = function(name, data)
    local f = io.open(save_path(name), "ab")
    if not f then return false end
    f:write(data); f:close()
    return true
  end,
  remove = function(name) return os.remove(save_path(name)) ~= nil end,
  getInfo = function(name)
    local f = io.open(save_path(name), "rb")
    if f then local n = f:seek("end"); f:close(); return { type = "file", size = n } end
    local d = nv.read(name)
    if d then return { type = "file", size = #d } end
  end,
  load = function(name)
    local d = nv.read(name)
    if not d then return nil, "no file " .. name end
    return load(d, "@" .. name)
  end,
  createDirectory = function() return true end,
  setIdentity = function() end,
  getSaveDirectory = function() return nv.path("") end,
  getDirectoryItems = function() return {} end,
}
love.filesystem.exists = function(n) return love.filesystem.getInfo(n) ~= nil end
love.filesystem.isFile = love.filesystem.exists

-- ---- the bridge to the nv callbacks (the engine calls it after main.lua) -----------------------
function love._bridge()
  local conf = nv.read("conf.lua")
  if conf then
    local fn = load(conf, "@conf.lua")
    if fn then pcall(fn) end
    if love.conf then
      local t = { window = {}, modules = {}, audio = {} }
      pcall(love.conf, t)
    end
  end
  nv.continuous(true)
  nv.init = function() if love.load then love.load({}) end end
  nv.update = function(dt) last_dt = dt; if love.update then love.update(dt) end end
  nv.draw = function()
    gfx.target(nil); target = nil
    gfx.origin()
    gfx.alpha(255)
    if K ~= 1 or OX ~= 0 or OY ~= 0 then
      gfx.clear(0x000000)
      gfx.clip(OX, OY, math.floor(W * K), math.floor(H * K))
      gfx.rect(OX, OY, W * K, H * K, bg)
    else
      gfx.clear(bg)
    end
    base()
    if love.draw then love.draw() end
    if target then g.setCanvas() end
    gfx.clip()
    gfx.target(nil); target = nil
    gfx.origin()
    gfx.alpha(255)
  end
  nv.key = function(ev)
    local k = lkey(ev.key)
    if ev.down then
      held[k] = true
      if love.keypressed then love.keypressed(k, k, false) end
      if ev.text and love.textinput then love.textinput(ev.text) end
    else
      held[k] = nil
      if love.keyreleased then love.keyreleased(k, k) end
    end
    return true
  end
  -- keyboard-only games: a tap / swipe becomes a key (love.touch_keys overrides the mapping)
  local wants_touch = love.touchpressed or love.mousepressed or love.touchreleased or love.mousereleased
  local tk = love.touch_keys or { tap = "space", left = "left", right = "right", up = "up", down = "down" }
  local function tap_key(k)
    if not k then return end
    held[k] = true
    if love.keypressed then love.keypressed(k, k, false) end
    held[k] = nil
    if love.keyreleased then love.keyreleased(k, k) end
  end
  nv.touch = function(ev)
    local id = ev.id
    local x, y = to_game(ev.x, ev.y)
    ev = { type = ev.type, x = x, y = y, dx = ev.dx / K, dy = ev.dy / K, sx = ev.sx, sy = ev.sy, rx = ev.x, ry = ev.y }
    if not wants_touch or love.touch_keys then
      if ev.type == "up" and id == 1 then
        local dx, dy = ev.rx - ev.sx, ev.ry - ev.sy
        if math.abs(dx) < 30 and math.abs(dy) < 30 then tap_key(tk.tap)
        elseif math.abs(dx) > math.abs(dy) then tap_key(dx < 0 and tk.left or tk.right)
        else tap_key(dy < 0 and tk.up or tk.down) end
      end
      if not wants_touch then return end
    end
    if ev.type == "down" then
      touches[id] = { x = x, y = y }
      if love.touchpressed then love.touchpressed(id, x, y, 0, 0, 1) end
      if id == 1 and love.mousepressed then love.mousepressed(ev.x, ev.y, 1, true, 1) end
    elseif ev.type == "move" then
      touches[id] = { x = x, y = y }
      if love.touchmoved then love.touchmoved(id, ev.x, ev.y, ev.dx, ev.dy, 1) end
      if id == 1 and love.mousemoved then love.mousemoved(ev.x, ev.y, ev.dx, ev.dy, true) end
    else
      touches[id] = nil
      if love.touchreleased then love.touchreleased(id, ev.x, ev.y, 0, 0, 1) end
      if id == 1 and love.mousereleased then love.mousereleased(ev.x, ev.y, 1, true, 1) end
    end
  end
  -- the OS back gesture is Escape; a second one within a second leaves the app
  local last_back = -10
  nv.back = function()
    local now = nv.clock()
    if now - last_back < 1 then return false end
    last_back = now
    held.escape = true
    if love.keypressed then love.keypressed("escape", "escape", false) end
    held.escape = nil
    if love.keyreleased then love.keyreleased("escape", "escape") end
    return true
  end
  nv.quit = function() if love.quit then love.quit() end end
end

return love
