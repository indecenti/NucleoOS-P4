-- love.lua - a LÖVE (love2d.org) compatibility layer for the Lua App engine: enough of
-- love.graphics / keyboard / mouse / touch / timer / audio / filesystem for small 2D games to run
-- unchanged or nearly so. Shapes, text, images (packed by tools/lua_pack.py), quads, sprite
-- batches, canvases, transforms (rotation too: images, lines and polygons rotate; rectangles become
-- polygons; text keeps upright). Not there: shaders, meshes, physics, TTF files (any newFont() maps
-- to the engine's Montserrat at that size), streamed music. Colours may be 0..1
-- (LÖVE 11) or 0..255 (LÖVE 0.10): both are accepted.
local ui = ui or require "ui"    -- the engine's, whatever the game calls ui
local love = {}
love._version = "11.4"
-- LÖVE runs LuaJIT, whose ipairs ignores __index (Lua 5.4's does not): libraries such as moonshine
-- rely on that, so LÖVE games get the raw one
do
  local ipairs54 = ipairs
  local function step(t, i)
    i = i + 1
    local v = rawget(t, i)
    if v ~= nil then return i, v end
  end
  ipairs = function(t)
    if type(t) == "table" and getmetatable(t) ~= nil then return step, t, 0 end
    return ipairs54(t)
  end
end
-- LuaJIT writes the float 10.0 as "10" (scores, file names built from numbers)
_nv.lua51_numbers(true)
-- LuaJIT's math.random floors float bounds; Lua 5.4's refuses them
do
  local random54, floor = math.random, math.floor
  math.random = function(a, b)
    if a == nil then return random54() end
    if b == nil then return random54(floor(a)) end
    return random54(floor(a), floor(b))
  end
end
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
-- Fonts are Montserrat up to 72 px; a bigger LÖVE font is drawn at 72 px into a cached surface and
-- scaled up.
local MAXF = 72
local Font = {}
Font.__index = Font
local function fs(f) return math.min(f.size, MAXF), f.size > MAXF and f.size / MAXF or 1 end
local function twidth(f, s)
  local z, k = fs(f)
  if k == 1 then return gfx.text_width(s, z) end
  gfx.push(); gfx.identity(); local w = gfx.text_width(s, z); gfx.pop()
  return w * k
end
local function theight(f)
  local z, k = fs(f)
  if k == 1 then return gfx.font_height(z) end
  gfx.push(); gfx.identity(); local h, a = gfx.font_height(z); gfx.pop()
  return h * k, a * k
end
function Font:getWidth(s) local w = 0 for l in (tostring(s) .. "\n"):gmatch("([^\n]*)\n") do w = math.max(w, twidth(self, l)) end return w end
function Font:getHeight() return (theight(self)) end
function Font:getLineHeight() return self.lh or 1 end
function Font:setLineHeight(v) self.lh = v end
function Font:hasGlyphs() return true end
function Font:setFallbacks() end
function Font:getDPIScale() return 1 end
function Font:getDescent() local h, a = theight(self); return a - h end
function Font:type() return "Font" end
function Font:typeOf(t) return t == "Font" or t == "Object" end
function Font:release() end
function Font:getAscent() local _, a = theight(self); return a end
function Font:getBaseline() return self:getAscent() end
function Font:setFilter() end
function Font:getKerning() return 0 end
local function wrap_lines(f, text, limit)
  local z, k = fs(f)
  return ui.wrap(text, z, limit / k)
end
function Font:getWrap(text, limit)
  local _, str = (function(t) if type(t) == "table" then local a = {} for i = 2, #t, 2 do a[#a + 1] = tostring(t[i]) end return nil, table.concat(a) end return nil, tostring(t) end)(text)
  local lines = wrap_lines(self, str, limit)
  local w = 0
  for _, l in ipairs(lines) do w = math.max(w, twidth(self, l)) end
  return w, lines
end
function g.newFont(a, b)
  local size = type(a) == "number" and a or (b or 12)
  if size < 1 then size = 12 end
  return setmetatable({ size = size, cache = {}, ncache = 0 }, Font)
end
g.setNewFont = function(...) local f = g.newFont(...); g.setFont(f); return f end
local font = g.newFont(12)
function g.setFont(f) if f then font = f end end
function g.getFont() return font end
-- one line of text at (x, y) (current transform), returns its width
local function ftext(f, x, y, s, color, align)
  local z, k = fs(f)
  if k == 1 then return gfx.text(x, y, s, z, color, align) end
  local c = f.cache[s]
  if not c then
    if f.ncache > 48 then
      for _, v in pairs(f.cache) do v.surf:free() end
      f.cache, f.ncache = {}, 0
    end
    gfx.push(); gfx.identity()
    local w, h = gfx.text_width(s, z) + 2, gfx.font_height(z) + 2
    local surf = gfx.surface(w, h)
    gfx.target(surf); gfx.alpha(255)
    gfx.text(0, 0, s, z, 0xFFFFFF)
    gfx.target(target and target.surf or nil); gfx.pop()
    love._restore_clip()
    c = { surf = surf, w = w, h = h }
    f.cache[s], f.ncache = c, f.ncache + 1
  end
  local w = (c.w - 2) * k
  if align == "center" then x = x - w / 2 elseif align == "right" then x = x - w end
  gfx.alpha(cur_a)
  gfx.tint(color); gfx.draw(c.surf, x, y, c.w * k, c.h * k); gfx.tint()
  return w
end
-- coloredtext {color1, "text1", color2, "text2", ...} -> segments
local function segments(text)
  if type(text) ~= "table" then return nil, tostring(text) end
  local segs, all = {}, {}
  for i = 1, #text, 2 do
    local c, s = text[i], tostring(text[i + 1] or "")
    segs[#segs + 1] = { c, s }
    all[#all + 1] = s
  end
  return segs, table.concat(all)
end
local function draw_segments(f, segs, x, y)
  local lh = theight(f)
  for _, sg in ipairs(segs) do
    local c, a = cur, cur_a
    if type(sg[1]) == "table" then
      c, a = to_rgb(sg[1])
      a = math.floor(a * cur_a / 255)
    end
    local first = true
    for line in (sg[2] .. "\n"):gmatch("([^\n]*)\n") do
      if not first then y = y + lh; x = 0 end
      first = false
      gfx.alpha(a)
      x = x + ftext(f, x, y, line, c)
    end
  end
  gfx.alpha(cur_a)
end
function g.print(text, x, ...)
  local f = font
  local y, r, sx, sy, ox, oy, kx, ky = ...
  if type(x) == "table" and x.size then f = x; x, y, r, sx, sy, ox, oy, kx, ky = ... end
  local segs, str = segments(text)
  x, y, sx, r = x or 0, y or 0, sx or 1, r or 0
  sy = sy or sx
  A()
  local fancy = sx ~= 1 or sy ~= 1 or ox or oy or r ~= 0 or kx or ky
  gfx.push(); gfx.translate(x, y)
  if fancy then gfx.rotate(r); gfx.scale(sx, sy); gfx.shear(kx or 0, ky or 0); gfx.translate(-(ox or 0), -(oy or 0)) end
  draw_segments(f, segs or { { false, str } }, 0, 0)
  gfx.pop()
end
function g.printf(text, x, ...)
  local f = font
  local y, limit, align, r, sx, sy, ox, oy = ...
  if type(x) == "table" and x.size then f = x; x, y, limit, align, r, sx, sy, ox, oy = ... end
  A()
  x, y, limit, sx, r = x or 0, y or 0, limit or 1e6, sx or 1, r or 0
  sy = sy or sx
  local _, str = segments(text)
  local lh = theight(f)
  gfx.push(); gfx.translate(x, y); gfx.rotate(r); gfx.scale(sx, sy); gfx.translate(-(ox or 0), -(oy or 0))
  local i = 0
  for para in (str .. "\n"):gmatch("([^\n]*)\n") do
    local lines = wrap_lines(f, para, limit)
    if #lines == 0 then lines = { "" } end
    for _, l in ipairs(lines) do
      local lx = 0
      if align == "center" then lx = limit / 2 elseif align == "right" then lx = limit end
      ftext(f, lx, i * lh, l, cur, align == "center" and "center" or align == "right" and "right" or "left")
      i = i + 1
    end
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
  if not c and (K ~= 1 or OX ~= 0 or OY ~= 0) then
    gfx.push(); gfx.origin(); gfx.clip(OX, OY, math.floor(W * K), math.floor(H * K)); gfx.pop()
  end
end
function g.getCanvas() return target end
local Quad = {}
Quad.__index = Quad
function Quad:getViewport() return self.x, self.y, self.w, self.h end
function Quad:setViewport(x, y, w, h) self.x, self.y, self.w, self.h = x, y, w or self.w, h or self.h end
function Quad:getTextureDimensions() return self.sw, self.sh end
function g.newQuad(x, y, w, h, sw, sh)
  if type(sw) == "table" then sw, sh = sw.w, sw.h end
  return setmetatable({ x = x, y = y, w = w, h = h, sw = sw, sh = sh, quad = true }, Quad)
end
local function tinted(fn, ...)
  gfx.alpha(cur_a)
  if cur ~= 0xFFFFFF then gfx.tint(cur); fn(...); gfx.tint() else fn(...) end
end
local function draw_one(d, q, x, y, r, sx, sy, ox, oy, kx, ky)
  x, y, sx = x or 0, y or 0, sx or 1
  sy = sy or sx
  ox, oy, r = ox or 0, oy or 0, r or 0
  if kx and kx ~= 0 or ky and ky ~= 0 then
    gfx.push(); gfx.translate(x, y); gfx.rotate(r); gfx.scale(sx, sy); gfx.shear(kx or 0, ky or 0)
    if q then gfx.draw(d.surf, -ox, -oy, q.w, q.h, q.x, q.y, q.w, q.h)
    else gfx.draw(d.surf, -ox, -oy, d.w, d.h) end
    gfx.pop()
  elseif q then
    gfx.draw(d.surf, x, y, q.w * sx, q.h * sy, q.x, q.y, q.w, q.h, r, ox * sx, oy * sy)
  elseif r ~= 0 or sx < 0 or sy < 0 then
    gfx.draw(d.surf, x, y, d.w * sx, d.h * sy, 0, 0, d.w, d.h, r, ox * sx, oy * sy)
  else
    gfx.draw(d.surf, x - ox * sx, y - oy * sy, d.w * sx, d.h * sy)
  end
end
function g.draw(d, ...)
  if type(d) ~= "table" then return end
  if d.batch then return d:_draw(...) end
  if d.text_obj then return d:_draw(...) end
  if not d.surf then return end
  local q = ...
  if type(q) == "table" and q.quad then tinted(draw_one, d, q, select(2, ...))
  else tinted(draw_one, d, nil, ...) end
end
-- ---- image data: pixels kept in Lua (0xRRGGBBAA per pixel) ----------------------------------
local ImageData = {}
ImageData.__index = ImageData
function ImageData:getWidth() return self.w end
function ImageData:getHeight() return self.h end
function ImageData:getDimensions() return self.w, self.h end
function ImageData:getFormat() return "rgba8" end
function ImageData:getPixel(x, y)
  x, y = math.floor(x), math.floor(y)
  if x < 0 or y < 0 or x >= self.w or y >= self.h then error("pixel out of range", 2) end
  local v = self.px[y * self.w + x + 1] or 0
  return ((v >> 24) & 255) / 255, ((v >> 16) & 255) / 255, ((v >> 8) & 255) / 255, (v & 255) / 255
end
local function c8(v) v = v or 0; if v <= 1 then v = v * 255 end; v = math.floor(v + 0.5); return v < 0 and 0 or v > 255 and 255 or v end
function ImageData:setPixel(x, y, r, gg, b, a)
  if type(r) == "table" then r, gg, b, a = r[1], r[2], r[3], r[4] end
  x, y = math.floor(x), math.floor(y)
  if x < 0 or y < 0 or x >= self.w or y >= self.h then return end
  self.px[y * self.w + x + 1] = (c8(r) << 24) | (c8(gg) << 16) | (c8(b) << 8) | c8(a == nil and 1 or a)
end
function ImageData:mapPixel(fn, x0, y0, w, h)
  x0, y0 = x0 or 0, y0 or 0
  w, h = w or self.w - x0, h or self.h - y0
  for y = y0, y0 + h - 1 do
    for x = x0, x0 + w - 1 do self:setPixel(x, y, fn(x, y, self:getPixel(x, y))) end
  end
end
function ImageData:paste(src, dx, dy, sx, sy, sw, sh)
  sx, sy = sx or 0, sy or 0
  sw, sh = sw or src.w, sh or src.h
  for y = 0, sh - 1 do
    for x = 0, sw - 1 do
      local X, Y = dx + x, dy + y
      if X >= 0 and Y >= 0 and X < self.w and Y < self.h and sx + x < src.w and sy + y < src.h then
        self.px[Y * self.w + X + 1] = src.px[(sy + y) * src.w + sx + x + 1]
      end
    end
  end
end
function ImageData:clone() local t = {} for i, v in pairs(self.px) do t[i] = v end return setmetatable({ w = self.w, h = self.h, px = t }, ImageData) end
function ImageData:encode() return nil end
function ImageData:release() end
function ImageData:type() return "ImageData" end
function ImageData:typeOf(t) return t == "ImageData" or t == "Data" or t == "Object" end
local function imagedata_from_surface(s)
  local w, h = s:size()
  local px = {}
  local prev = target
  gfx.target(s)
  for y = 0, h - 1 do
    for x = 0, w - 1 do
      local c, a = gfx.get_pixel(x, y)
      px[y * w + x + 1] = (c << 8) | (a or 255)
    end
  end
  gfx.target(prev and prev.surf or nil)
  return setmetatable({ w = w, h = h, px = px }, ImageData)
end
local function surface_from_imagedata(d, s)
  s = s or gfx.surface(d.w, d.h)
  local prev = target
  gfx.push(); gfx.target(s); gfx.identity(); gfx.clear()
  for y = 0, d.h - 1 do
    for x = 0, d.w - 1 do
      local v = d.px[y * d.w + x + 1] or 0
      local a = v & 255
      if a > 0 then gfx.alpha(a); gfx.pixel(x, y, v >> 8) end
    end
  end
  gfx.alpha(255)
  gfx.target(prev and prev.surf or nil)
  gfx.pop()
  return s
end
love.image = {
  newImageData = function(a, b)
    if type(a) == "number" then
      return setmetatable({ w = math.floor(a), h = math.floor(b or a), px = {} }, ImageData)
    end
    if type(a) == "table" and a.surf then return imagedata_from_surface(a.surf) end
    local s, err = gfx.image(tostring(a))
    if not s then error(err, 2) end
    local d = imagedata_from_surface(s)
    s:free()
    return d
  end,
  isCompressed = function() return false end,
}
love.image.newImageData_ = love.image.newImageData
function Drawable:replacePixels(d) if d and d.px then surface_from_imagedata(d, self.surf) end end
function Drawable:getData() return imagedata_from_surface(self.surf) end
function Drawable:newImageData() return imagedata_from_surface(self.surf) end
function Drawable:type() return self.kind == "canvas" and "Canvas" or "Image" end
function Drawable:typeOf(t) return t == self:type() or t == "Drawable" or t == "Texture" or t == "Object" end
function Drawable:release() if self.surf then self.surf:free() end self.surf = nil end
function Drawable:getPixelDimensions() return self.w, self.h end
function Drawable:getFilter() return "nearest", "nearest" end
function Drawable:getWrap() return "clamp", "clamp" end
function Drawable:setMipmapFilter() end
local newImage0 = g.newImage
function g.newImage(src, ...)
  if type(src) == "table" and src.px then return wrap(surface_from_imagedata(src), "image") end
  return newImage0(src, ...)
end
function g.newImageFont(src)
  local h = 16
  if type(src) == "string" then
    local s = gfx.image(src)
    if s then local _, hh = s:size(); h = math.max(12, hh); s:free() end
  elseif type(src) == "table" and src.h then h = math.max(12, src.h) end
  return g.newFont(h)
end
function g.newArrayImage() error("array images are not supported by the Lua App engine", 2) end

-- sprite batches: the sprites are kept and drawn one by one (no GPU here)
local Batch = {}
Batch.__index = Batch
function Batch:add(q, ...)
  local it
  if type(q) == "table" and q.quad then it = { q, ... } else it = { false, q, ... } end
  it.c, it.a = self.col, self.col_a
  self.items[#self.items + 1] = it
  return #self.items
end
function Batch:set(id, q, ...)
  local it
  if type(q) == "table" and q.quad then it = { q, ... } else it = { false, q, ... } end
  it.c, it.a = self.col, self.col_a
  self.items[id] = it
end
function Batch:clear() self.items = {}; self.col = nil end
function Batch:setColor(...) if select("#", ...) == 0 then self.col = nil else self.col, self.col_a = to_rgb(...) end end
function Batch:getCount() return #self.items end
function Batch:getBufferSize() return self.size end
function Batch:setTexture(img) self.img = img end
function Batch:getTexture() return self.img end
for _, k in ipairs({ "flush", "bind", "unbind", "setBufferSize", "attachAttribute", "setDrawRange" }) do Batch[k] = function() end end
function Batch:_draw(x, y, r, sx, sy, ox, oy)
  local c0, a0 = cur, cur_a
  gfx.push()
  gfx.translate(x or 0, y or 0); gfx.rotate(r or 0); gfx.scale(sx or 1, sy or sx or 1); gfx.translate(-(ox or 0), -(oy or 0))
  for i = 1, #self.items do
    local it = self.items[i]
    if it then
      if it.c then cur, cur_a = it.c, math.floor(it.a * a0 / 255) end
      tinted(draw_one, self.img, it[1] or nil, it[2], it[3], it[4], it[5], it[6], it[7], it[8], it[9], it[10])
      cur, cur_a = c0, a0
    end
  end
  gfx.pop()
end
function g.newSpriteBatch(img, size) return setmetatable({ img = img, items = {}, size = size or 1000, batch = true }, Batch) end
-- text objects
local Text = {}
Text.__index = Text
function Text:set(t) self.t = t end
function Text:setf(t, limit, align) self.t, self.limit, self.align = t, limit, align end
function Text:add(t, x, y) self.t = (self.t or "") .. (type(t) == "table" and "" or tostring(t)) end
function Text:clear() self.t = "" end
function Text:getWidth() return self.font:getWidth(self.t or "") end
function Text:getHeight() return self.font:getHeight() end
function Text:getDimensions() return self:getWidth(), self:getHeight() end
function Text:setFont(f) self.font = f end
function Text:_draw(x, y, r, sx, sy, ox, oy)
  local f = font; font = self.font
  if self.limit then g.printf(self.t or "", x, y, self.limit, self.align, r, sx, sy)
  else g.print(self.t or "", x, y, r, sx, sy, ox, oy) end
  font = f
end
function g.newText(f, t) return setmetatable({ font = f or font, t = type(t) == "string" and t or "", text_obj = true }, Text) end
-- shaders cannot run here: a shader object accepts everything and changes nothing
local Shader = {}
Shader.__index = Shader
function Shader:send() end
function Shader:sendColor() end
function Shader:hasUniform() return false end
function Shader:getWarnings() return "" end
function Shader:release() end
function g.newShader() return setmetatable({}, Shader) end
function g.getShader() return nil end

-- ---- transform, state ---------------------------------------------------------------------------
g.push = function() gfx.push() end
g.pop = function() gfx.pop() end
g.translate = function(x, y) gfx.translate(x or 0, y or 0) end
g.scale = function(sx, sy) gfx.scale(sx, sy or sx) end
g.origin = function() if target then gfx.origin() else base() end end
g.rotate = function(r) gfx.rotate(r or 0) end
g.shear = function(kx, ky) gfx.shear(kx or 0, ky or 0) end
-- the scissor is in window (or canvas) pixels, whatever the transform
local scissor
function g.setScissor(x, y, w, h)
  gfx.push()
  if target then gfx.origin() else base() end
  if x then scissor = { x, y, w, h }; gfx.clip(x, y, w, h)
  else scissor = nil; if target then gfx.clip() else gfx.clip(0, 0, W, H) end end
  gfx.pop()
end
function g.intersectScissor(x, y, w, h)
  if not scissor then return g.setScissor(x, y, w, h) end
  local x0, y0 = math.max(x, scissor[1]), math.max(y, scissor[2])
  local x1, y1 = math.min(x + w, scissor[1] + scissor[3]), math.min(y + h, scissor[2] + scissor[4])
  g.setScissor(x0, y0, math.max(0, x1 - x0), math.max(0, y1 - y0))
end
-- after a target switch: the scissor, or the letterbox, again
function love._restore_clip()
  if scissor then g.setScissor(scissor[1], scissor[2], scissor[3], scissor[4])
  elseif not target and (K ~= 1 or OX ~= 0 or OY ~= 0) then
    gfx.push(); gfx.origin(); gfx.clip(OX, OY, math.floor(W * K), math.floor(H * K)); gfx.pop()
  end
end
function g.getScissor() if scissor then return scissor[1], scissor[2], scissor[3], scissor[4] end end
local blend_mode, blend_alpha = "alpha", "alphamultiply"
local BLENDS = { alpha = "alpha", add = "add", additive = "add", subtract = "subtract", subtractive = "subtract",
                 multiply = "multiply", multiplicative = "multiply", screen = "screen", replace = "replace",
                 lighten = "lighten", darken = "darken", premultiplied = "alpha" }
g.setBlendMode = function(m, am) blend_mode, blend_alpha = m or "alpha", am or "alphamultiply"; gfx.blend(BLENDS[blend_mode] or "alpha") end
g.getBlendMode = function() return blend_mode, blend_alpha end
g.isCreated = function() return true end
g.isActive = function() return true end
g.getLineStyle = function() return "smooth" end
g.getLineJoin = function() return "miter" end
g.getDefaultFilter = function() return "nearest", "nearest", 1 end
g.getPointSize = function() return point_size end
g.getStackDepth = function() return 0 end
g.isWireframe = function() return false end
g.getColorMask = function() return true, true, true, true end
g.setColorMask = function() end
g.stencil = function() end
g.setStencilTest = function() end
g.getStencilTest = function() return "always", 0 end
g.getRendererInfo = function() return "NucleoOS", "1.1", "software", "Lua App engine" end
g.getSystemLimits = function() return { texturesize = 2048, canvasmsaa = 0 } end
g.isGammaCorrect = function() return false end
g.getPixelWidth = function() return W end
g.getPixelHeight = function() return H end
g.getPixelDimensions = function() return W, H end
g.getDPIScale = function() return 1 end
function g.getWidth() return W end
function g.getHeight() return H end
function g.getDimensions() return W, H end
for _, k in ipairs({ "setDefaultFilter", "setLineStyle", "setLineJoin", "present",
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
love.window.isCreated = function() return true end
love.window.getFullscreen = function() return true end
love.window.getFullscreenModes = function() return { { width = SW, height = SH } } end
love.window.getTitle = function() return "" end
love.window.hasFocus = function() return true end
love.window.hasMouseFocus = function() return true end
love.window.isVisible = function() return true end
love.window.getPosition = function() return 0, 0, 1 end
love.window.setPosition = function() end
love.window.showMessageBox = function() return 1 end
love.window.requestAttention = function() end
love.window.minimize = function() end
love.window.restore = function() end
love.window.close = function() end
love.window.getDisplayCount = function() return 1 end
love.window.getDisplayName = function() return "NucleoOS" end
love.window.toPixels = function(...) return ... end
love.window.fromPixels = function(...) return ... end
love.window.getSafeArea = function() return 0, 0, W, H end
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
  sleep = function() if coroutine.isyieldable() then coroutine.yield() end end,
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
-- love.key_alias (nucleo.lua) renames keys for old games, e.g. { space = " " } for LÖVE 0.9
local function lkey(k)
  if k == "enter" then k = "return" else k = PADKEY[k] or k end
  return love.key_alias and love.key_alias[k] or k
end
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
-- a SoundData made from a file is just its path here (a string, like LÖVE's userdata is no table)
love.sound = { newSoundData = function(a) if type(a) == "string" then return a end return {} end,
               newDecoder = function(a) return a end }
-- gamepads arrive as keys (see PADKEY); the joystick module reports none
love.joystick = { getJoysticks = function() return {} end, getJoystickCount = function() return 0 end,
                  setGamepadMapping = function() return false end, loadGamepadMappings = function() end }

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
    local pre = tostring(name):gsub("^/+", ""):gsub("/+$", "") .. "/"
    for _, f in ipairs(nv.files()) do
      if f:sub(1, #pre) == pre then return { type = "directory", size = 0 } end
    end
  end,
  load = function(name)
    local d = love.filesystem.read(name)
    if not d then return nil, "no file " .. name end
    return load(d, "@" .. name)
  end,
  createDirectory = function() return true end,
  setIdentity = function() end,
  getSaveDirectory = function() return nv.path("") end,
  getDirectoryItems = function(dir)
    dir = tostring(dir or ""):gsub("^/+", ""):gsub("/+$", "")
    local pre = dir == "" and "" or dir .. "/"
    local seen, out = {}, {}
    for _, f in ipairs(nv.files()) do
      if f:sub(1, #pre) == pre then
        local rest = f:sub(#pre + 1)
        local name = rest:match("^[^/]+")
        if name and not seen[name] then seen[name] = true; out[#out + 1] = name end
      end
    end
    table.sort(out)
    return out
  end,
}
love.filesystem.exists = function(n) return love.filesystem.getInfo(n) ~= nil end
love.filesystem.isFile = function(n) local i = love.filesystem.getInfo(n); return i ~= nil and i.type == "file" end
love.filesystem.isDirectory = function(n) local i = love.filesystem.getInfo(n); return i ~= nil and i.type == "directory" end
love.filesystem.getLastModified = function() return 0 end
love.filesystem.lines = function(name)
  local d = love.filesystem.read(name) or ""
  return d:gmatch("([^\n]*)\n?")
end


-- ---- odds and ends LÖVE games call ----------------------------------------------------------------
love.keyboard.getKeyFromScancode = function(s) return s end
love.keyboard.getScancodeFromKey = function(k) return k end
love.keyboard.isScancodeDown = love.keyboard.isDown
love.keyboard.hasKeyRepeat = function() return false end
love.keyboard.hasScreenKeyboard = function() return false end
love.mouse.isCursorSupported = function() return false end
love.mouse.newCursor = function() return {} end
love.mouse.getCursor = function() return nil end
love.mouse.setPosition = function() end
love.mouse.setX = function() end
love.mouse.setY = function() end
love.mouse.getRelativeMode = function() return false end
love.mouse.isGrabbed = function() return false end
love.math.colorFromBytes = function(r, gg, b, a)
  if type(r) == "table" then r, gg, b, a = r[1], r[2], r[3], r[4] end
  return r / 255, gg / 255, b / 255, a and a / 255 or nil
end
love.math.colorToBytes = function(r, gg, b, a)
  if type(r) == "table" then r, gg, b, a = r[1], r[2], r[3], r[4] end
  local f = function(v) return math.floor(v * 255 + 0.5) end
  return f(r), f(gg), f(b), a and f(a) or nil
end
love.math.gammaToLinear = function(...) return ... end
love.math.linearToGamma = function(...) return ... end
love.math.getRandomSeed = function() return 0, 0 end
love.math.randomNormal = function(sd, mean)
  local u1, u2 = math.random(), math.random()
  return (mean or 0) + (sd or 1) * math.sqrt(-2 * math.log(1 - u1)) * math.cos(2 * math.pi * u2)
end
local RNG = {}
RNG.__index = RNG
local function rng_next(self)       -- xorshift32
  local x = self.s
  x = x ~ ((x << 13) & 0xFFFFFFFF); x = x ~ (x >> 17); x = x ~ ((x << 5) & 0xFFFFFFFF)
  self.s = x & 0xFFFFFFFF
  return self.s / 4294967296
end
function RNG:random(a, b)
  local r = rng_next(self)
  if a == nil then return r end
  if b == nil then a, b = 1, a end
  return math.floor(a + r * (b - a + 1))
end
function RNG:randomNormal(sd, mean)
  local u1, u2 = rng_next(self), rng_next(self)
  return (mean or 0) + (sd or 1) * math.sqrt(-2 * math.log(1 - u1)) * math.cos(2 * math.pi * u2)
end
function RNG:setSeed(s) self.s = (math.floor(s or 1) & 0xFFFFFFFF); if self.s == 0 then self.s = 0x9E3779B9 end end
function RNG:getSeed() return self.s, 0 end
function RNG:getState() return tostring(self.s) end
function RNG:setState(v) self.s = tonumber(v) or 1 end
love.math.newRandomGenerator = function(seed)
  local r = setmetatable({}, RNG)
  r:setSeed(seed or nv.random(1, 0x7FFFFFFF))
  return r
end
love.math.isConvex = function() return true end
love.math.triangulate = function(...)
  local p = flat(...)
  local t = {}
  for i = 3, #p / 2 - 1 do
    t[#t + 1] = { p[1], p[2], p[2 * i - 1], p[2 * i], p[2 * i + 1], p[2 * i + 2] }
  end
  return t
end
-- love.filesystem.newFile: a File object over the save folder (reads fall back to the bundle)
local File = {}
File.__index = File
function File:open(mode)
  self.mode = mode or "r"
  if self.mode == "r" then
    self.data = love.filesystem.read(self.name)
    self.pos = 1
    return self.data ~= nil
  end
  self.f = io.open(save_path(self.name), self.mode == "a" and "ab" or "wb")
  return self.f ~= nil
end
function File:write(d) if self.f then self.f:write(d) return true end return false end
function File:read(n)
  if not self.data then return nil, 0 end
  local s = n and self.data:sub(self.pos, self.pos + n - 1) or self.data:sub(self.pos)
  self.pos = self.pos + #s
  return s, #s
end
function File:lines() return (self.data or ""):gmatch("([^\n]*)\n?") end
function File:close() if self.f then self.f:close() end self.f = nil return true end
function File:flush() if self.f then self.f:flush() end return true end
function File:isOpen() return self.f ~= nil or self.data ~= nil end
function File:getSize() local d = love.filesystem.read(self.name); return d and #d or 0 end
function File:getFilename() return self.name end
function File:isEOF() return not self.data or self.pos > #self.data end
love.filesystem.newFile = function(name, mode)
  local f = setmetatable({ name = name }, File)
  if mode then f:open(mode) end
  return f
end
love.filesystem.newFileData = function(data, name) return { getString = function() return data end, getFilename = function() return name end, getSize = function() return #data end } end
love.filesystem.getIdentity = function() return nv.app end
love.filesystem.getUserDirectory = function() return nv.path("") end
love.filesystem.getAppdataDirectory = function() return nv.path("") end
love.filesystem.getWorkingDirectory = function() return "/" end
love.filesystem.getSource = function() return "/" end
love.filesystem.getSourceBaseDirectory = function() return "/" end
love.filesystem.isFused = function() return true end
love.filesystem.mount = function() return false end
love.filesystem.unmount = function() return false end
love.filesystem.setRequirePath = function() end
love.filesystem.getRequirePath = function() return "?.lua;?/init.lua" end
-- LÖVE passes the command line in the global arg
arg = arg or {}
-- LuaJIT's require returns one value; Lua 5.4's two would sneak into table.insert(t, require(m))
do
  local require54 = require
  require = function(m) return (require54(m)) end
end

-- ---- love.thread: threads run as coroutines, resumed once a frame (and whenever the main code
-- waits on a channel); a thread yields when it waits for a channel or calls love.timer.sleep.
-- Threads share the globals of the game (LÖVE gives each its own Lua state).
local threads = {}
local Channel = {}
Channel.__index = Channel
local channels = {}
local function run_threads()
  for i = #threads, 1, -1 do
    local t = threads[i]
    if coroutine.status(t.co) == "dead" then table.remove(threads, i)
    else
      local ok, err = coroutine.resume(t.co, table.unpack(t.args or {}))
      t.args = nil
      if not ok then
        t.err = tostring(err)
        print("thread error: " .. t.err)
        table.remove(threads, i)
        if love.threaderror then love.threaderror(t, t.err) end
      elseif coroutine.status(t.co) == "dead" then table.remove(threads, i) end
    end
  end
end
love._run_threads = run_threads
local function wait_until(ready, timeout)
  local t0 = nv.clock()
  while not ready() do
    if coroutine.isyieldable() then coroutine.yield()
    else
      if #threads == 0 then return false end
      run_threads()
    end
    if timeout and nv.clock() - t0 >= timeout then return ready() end
  end
  return true
end
function Channel:push(v) self.n = self.n + 1; self.q[#self.q + 1] = v; return self.n end
function Channel:supply(v, timeout)
  local id = self:push(v)
  wait_until(function() return self.read >= id end, timeout)
  return true
end
function Channel:pop()
  if #self.q == 0 then return nil end
  self.read = self.read + 1
  return table.remove(self.q, 1)
end
function Channel:peek() return self.q[1] end
function Channel:demand(timeout)
  if wait_until(function() return #self.q > 0 end, timeout) then return self:pop() end
end
function Channel:getCount() return #self.q end
function Channel:hasRead(id) return self.read >= id end
function Channel:clear() self.read = self.read + #self.q; self.q = {} end
function Channel:performAtomic(fn, ...) return fn(self, ...) end
function Channel:release() end
local function new_channel() return setmetatable({ q = {}, n = 0, read = 0 }, Channel) end
local Thread = {}
Thread.__index = Thread
function Thread:start(...)
  if self.co and coroutine.status(self.co) ~= "dead" then return end
  local fn = self.fn
  self.err = nil
  self.co = coroutine.create(function(...) return fn(...) end)
  self.args = table.pack(...)
  threads[#threads + 1] = self
end
function Thread:isRunning() return self.co ~= nil and coroutine.status(self.co) ~= "dead" end
function Thread:getError() return self.err end
function Thread:wait() wait_until(function() return not self:isRunning() end) end
function Thread:release() end
love.thread = {
  newThread = function(src)
    local code = src
    local name = "thread"
    if type(src) == "string" and not src:find("\n") and nv.read(src) then
      code, name = nv.read(src), src
    end
    local fn, err = load(code, "@" .. name, "t")
    if not fn then error(err, 2) end
    return setmetatable({ fn = fn }, Thread)
  end,
  getChannel = function(n)
    channels[n] = channels[n] or new_channel()
    return channels[n]
  end,
  newChannel = new_channel,
}
-- require "love.timer" and friends inside threads
for _, m in ipairs({ "graphics", "timer", "math", "filesystem", "audio", "sound", "image", "keyboard",
                     "mouse", "event", "system", "window", "thread", "data", "touch", "joystick", "physics" }) do
  package.preload["love." .. m] = function() return love[m] or {} end
end
package.preload["love"] = function() return love end
love.data = love.data or {}

-- ---- the bridge to the nv callbacks (the engine calls it after main.lua) -----------------------
-- conf.lua runs before main.lua, like in LÖVE; a window size it sets becomes the game's window
-- (scaled to fit the canvas)
function love._conf()
  if love._conf_done then return end
  love._conf_done = true
  local conf = nv.read("conf.lua")
  if not conf then return end
  local fn = load(conf, "@conf.lua")
  if fn then fn() end
  if love.conf then
    local t = { window = { width = SW, height = SH, title = "", resizable = false, fullscreen = false, vsync = 1 },
                modules = {}, audio = {}, identity = nil, version = "11.4", console = false }
    t.screen = t.window          -- LÖVE 0.8
    love.conf(t)
    local tw = t.window or t.screen or {}
    local w, h = tonumber(tw.width), tonumber(tw.height)
    if w and h and w > 0 and h > 0 and (w ~= SW or h ~= SH) then set_window(w, h) end
  end
end

-- moonshine (vrld) post-processing: shaders cannot run here, so a chain just draws the scene.
-- love.stub_moonshine("libraries.moonshine") in nucleo.lua, before main.lua requires it.
function love.stub_moonshine(name)
  local chain_mt = { __call = function(_, fn, ...) return fn(...) end,
                     __index = function(t, k)
                       if k == "draw" then return function(fn, ...) return fn(...) end end
                       if k == "next" or k == "chain" then return function() return t end end
                       if k == "disable" or k == "enable" or k == "resize" then return function() end end
                       return setmetatable({}, { __newindex = function() end })
                     end,
                     __newindex = function() end }
  local m = { effects = setmetatable({}, { __index = function() return function() return {} end end }),
              Effect = function(e) return e end, draw_shader = function() end }
  m.chain = function() return setmetatable({}, chain_mt) end
  package.loaded[name] = setmetatable(m, { __call = function() return setmetatable({}, chain_mt) end })
end

-- ---- on-screen controls for keyboard games: love.touch_pad = { dpad = true | {up=, down=, left=,
-- right=}, buttons = { {key = "space", label = "A"}, ... } } (set in the bundle's nucleo.lua). The
-- d-pad sits bottom-left, the buttons bottom-right; fingers on them hold the keys, others reach
-- the game as usual.
local pad_ctl, pad_fingers, pad_count = nil, {}, {}
local function pad_layout(p)
  local ctl = {}
  local u = math.min(SW / 1024, SH / 600)     -- small canvases (scaled up by the OS) get a small pad
  local m = math.floor(24 * u + 0.5)
  local r = math.floor(102 * u)
  if p.dpad then
    local d = type(p.dpad) == "table" and p.dpad or {}
    ctl[#ctl + 1] = { kind = "dpad", x = r + m, y = SH - r - m, r = r,
      up = d.up or "up", down = d.down or "down", left = d.left or "left", right = d.right or "right" }
  end
  local br = math.floor(r * 0.48)
  local gap = math.floor(br * 0.35)
  local c1, c2 = SW - br - m, SW - 3 * br - m - gap
  local r1, r2 = SH - br - m, SH - 3 * br - m - gap
  local spots = { { c1, r1 }, { c2, r1 }, { c1, r2 }, { c2, r2 } }
  for i, b in ipairs(p.buttons or {}) do
    local sp = spots[i]
    if sp then ctl[#ctl + 1] = { kind = "button", x = sp[1], y = sp[2], r = br, key = b.key, label = b.label or b.key, fs = math.max(12, math.floor(20 * u)) } end
  end
  return ctl
end
local function pad_press(k, on)
  if not k then return end
  local n = (pad_count[k] or 0) + (on and 1 or -1)
  if n < 0 then n = 0 end
  local was = (pad_count[k] or 0) > 0
  pad_count[k] = n
  if n > 0 and not was then
    held[k] = true
    if love.keypressed then love.keypressed(k, k, false) end
  elseif n == 0 and was then
    held[k] = nil
    if love.keyreleased then love.keyreleased(k, k) end
  end
end
local function pad_keys_at(c, x, y)
  local dx, dy = x - c.x, y - c.y
  if c.kind == "button" then return { c.key } end
  local d = math.sqrt(dx * dx + dy * dy)
  if d < c.r * 0.18 then return {} end
  local t = {}
  if dx > d * 0.38 then t[#t + 1] = c.right elseif dx < -d * 0.38 then t[#t + 1] = c.left end
  if dy > d * 0.38 then t[#t + 1] = c.down elseif dy < -d * 0.38 then t[#t + 1] = c.up end
  return t
end
local function pad_hit(x, y)
  for _, c in ipairs(pad_ctl or {}) do
    local dx, dy = x - c.x, y - c.y
    local reach = c.kind == "dpad" and c.r * 1.25 or c.r * 1.3
    if dx * dx + dy * dy <= reach * reach then return c end
  end
end
local function pad_set(f, keys)
  local old = f.keys or {}
  local new = {}
  for _, k in ipairs(keys) do new[k] = true end
  for _, k in ipairs(old) do if not new[k] then pad_press(k, false) end end
  local oldset = {}
  for _, k in ipairs(old) do oldset[k] = true end
  for _, k in ipairs(keys) do if not oldset[k] then pad_press(k, true) end end
  f.keys = keys
end
-- true when the event belonged to the pad
local function pad_touch(ev)
  if not pad_ctl then return false end
  local f = pad_fingers[ev.id]
  if ev.type == "down" then
    local c = pad_hit(ev.x, ev.y)
    if not c then return false end
    f = { c = c, keys = {} }
    pad_fingers[ev.id] = f
    pad_set(f, pad_keys_at(c, ev.x, ev.y))
    return true
  end
  if not f then return false end
  if ev.type == "move" then pad_set(f, pad_keys_at(f.c, ev.x, ev.y))
  else pad_set(f, {}); pad_fingers[ev.id] = nil end
  return true
end
local function pad_draw()
  if not pad_ctl then return end
  gfx.origin(); gfx.clip(); gfx.tint()
  for _, c in ipairs(pad_ctl) do
    local on = false
    for _, f in pairs(pad_fingers) do if f.c == c and #f.keys > 0 then on = true end end
    if c.kind == "dpad" then
      gfx.alpha(on and 90 or 60); gfx.circle(c.x, c.y, c.r, 0x808890)
      gfx.alpha(150)
      local a, r = c.r * 0.22, c.r * 0.72
      gfx.tri(c.x, c.y - r - a, c.x - a, c.y - r + a, c.x + a, c.y - r + a, 0xFFFFFF)
      gfx.tri(c.x, c.y + r + a, c.x - a, c.y + r - a, c.x + a, c.y + r - a, 0xFFFFFF)
      gfx.tri(c.x - r - a, c.y, c.x - r + a, c.y - a, c.x - r + a, c.y + a, 0xFFFFFF)
      gfx.tri(c.x + r + a, c.y, c.x + r - a, c.y - a, c.x + r - a, c.y + a, 0xFFFFFF)
    else
      gfx.alpha(on and 120 or 70); gfx.circle(c.x, c.y, c.r, 0x808890)
      local l = c.label
      if gfx.text_width(l, c.fs) > c.r * 1.7 then l = l:sub(1, 1) end    -- small pads: the initial
      gfx.alpha(180); gfx.text(c.x, c.y - c.fs * 0.6, l, c.fs, 0xFFFFFF, "center")
    end
  end
  gfx.alpha(255)
end

function love._bridge()
  love._conf()
  if love.touch_pad then pad_ctl = pad_layout(love.touch_pad) end
  nv.continuous(true)
  nv.init = function() if love.load then love.load({}) end end
  nv.update = function(dt) last_dt = dt; love._run_threads(); if love.update then love.update(dt) end end
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
    gfx.tint()
    pad_draw()
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
    -- held for a moment, so games that poll isDown() once a frame see it too
    nv.after(0.1, function()
      if (pad_count[k] or 0) > 0 then return end
      held[k] = nil
      if love.keyreleased then love.keyreleased(k, k) end
    end)
  end
  nv.touch = function(ev)
    if pad_touch(ev) then return end
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
