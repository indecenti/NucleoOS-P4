-- ui.lua - immediate-mode widgets for Lua apps. Call them from nv.draw(): each one draws itself and
-- reports what the finger did to it this frame (ui.button returns true when tapped, ui.slider the
-- new value...). Sizes are in canvas pixels; ui.s scales the defaults to the canvas (1 at 1024x600).
local ui = {}
local W, H = gfx.width(), gfx.height()
local s = math.min(W / 1024, H / 600)
ui.s = s
ui.W, ui.H = W, H
local function S(v) return math.floor(v * s + 0.5) end
ui.S = S

ui.theme = {
  bg = 0x0F1217, panel = 0x1A1F27, panel2 = 0x232A35, line = 0x2E3744, press = 0x33415A,
  fg = 0xE8ECF2, dim = 0x8A93A3, accent = 0x4C8DFF, accent_fg = 0xFFFFFF,
  ok = 0x3DDC84, warn = 0xFFB020, err = 0xFF5C5C,
}
local T = ui.theme
-- font sizes the engine has: 12 14 16 20 24 32 48 72 (others round down)
ui.font = { small = S(16) < 12 and 12 or S(16), body = S(20) < 12 and 12 or S(20), big = S(24), title = S(32),
            huge = S(48), giant = S(72) }

local blocked = false     -- a modal (keyboard) owns the input
local consumed = false    -- a widget already took this frame's tap
function ui._begin() consumed = false; blocked = ui._modal ~= nil end

local function inside(x, y, w, h, px, py) return px >= x and px < x + w and py >= y and py < y + h end

-- ui.hit(x, y, w, h) -> clicked, pressing   (the building block of every widget)
function ui.hit(x, y, w, h)
  if blocked then return false, false end
  local p = nv.pointer
  if not inside(x, y, w, h, p.sx, p.sy) then return false, false end
  local over = inside(x, y, w, h, p.x, p.y)
  local pressing = p.down and over and not p.moved
  local clicked = p.released and over and not p.moved and not consumed
  if clicked then consumed = true; nv.redraw() end
  return clicked, pressing
end

-- ---- drawing helpers ------------------------------------------------------------------------
function ui.clear(c) gfx.clear(c or T.bg) end
function ui.panel(x, y, w, h, color, r) gfx.rect(x, y, w, h, color or T.panel, r or S(14)) end
-- ui.label(x, y, text, size, color, align)
function ui.label(x, y, text, size, color, align)
  return gfx.text(x, y, text, size or ui.font.body, color or T.fg, align)
end
-- word-wrap: lines that fit in w pixels
function ui.wrap(text, size, w)
  local lines = {}
  for para in (tostring(text) .. "\n"):gmatch("(.-)\n") do
    local cur = ""
    for word in para:gmatch("%S+") do
      local try = cur == "" and word or (cur .. " " .. word)
      if gfx.text_width(try, size) > w and cur ~= "" then
        lines[#lines + 1] = cur
        cur = word
      else
        cur = try
      end
    end
    lines[#lines + 1] = cur
  end
  return lines
end
-- ui.paragraph(x, y, w, text, size, color) -> height drawn
function ui.paragraph(x, y, w, text, size, color)
  size = size or ui.font.body
  local lh = gfx.font_height(size)
  local lines = ui.wrap(text, size, w)
  for i, l in ipairs(lines) do gfx.text(x, y + (i - 1) * lh, l, size, color or T.fg) end
  return #lines * lh
end

-- ---- icons (vector, centred on cx,cy, `size` px box) -------------------------------------------
function ui.icon(name, cx, cy, size, color)
  local r = size / 2
  local c = color or T.fg
  local t = math.max(2, size / 10)
  if name == "back" then
    gfx.line(cx + r * 0.3, cy - r * 0.6, cx - r * 0.3, cy, c, t); gfx.line(cx - r * 0.3, cy, cx + r * 0.3, cy + r * 0.6, c, t)
  elseif name == "next" then
    gfx.line(cx - r * 0.3, cy - r * 0.6, cx + r * 0.3, cy, c, t); gfx.line(cx + r * 0.3, cy, cx - r * 0.3, cy + r * 0.6, c, t)
  elseif name == "close" then
    gfx.line(cx - r * .5, cy - r * .5, cx + r * .5, cy + r * .5, c, t); gfx.line(cx + r * .5, cy - r * .5, cx - r * .5, cy + r * .5, c, t)
  elseif name == "plus" then
    gfx.rect(cx - r * .6, cy - t / 2, r * 1.2, t, c); gfx.rect(cx - t / 2, cy - r * .6, t, r * 1.2, c)
  elseif name == "minus" then
    gfx.rect(cx - r * .6, cy - t / 2, r * 1.2, t, c)
  elseif name == "play" then
    gfx.tri(cx - r * .4, cy - r * .55, cx - r * .4, cy + r * .55, cx + r * .55, cy, c)
  elseif name == "pause" then
    gfx.rect(cx - r * .45, cy - r * .5, r * .3, r, c); gfx.rect(cx + r * .15, cy - r * .5, r * .3, r, c)
  elseif name == "stop" then
    gfx.rect(cx - r * .45, cy - r * .45, r * .9, r * .9, c, r * .1)
  elseif name == "check" then
    gfx.line(cx - r * .55, cy, cx - r * .15, cy + r * .4, c, t); gfx.line(cx - r * .15, cy + r * .4, cx + r * .6, cy - r * .45, c, t)
  elseif name == "refresh" then
    gfx.arc(cx, cy, r * .6, 40, 330, c, t); gfx.tri(cx + r * .62, cy - r * .55, cx + r * .62, cy - r * .05, cx + r * .2, cy - r * .3, c)
  elseif name == "power" then
    gfx.arc(cx, cy, r * .6, -50, 230, c, t); gfx.rect(cx - t / 2, cy - r * .75, t, r * .7, c)
  elseif name == "bulb" then
    gfx.circle(cx, cy - r * .15, r * .5, c); gfx.rect(cx - r * .25, cy + r * .35, r * .5, r * .35, c, r * .08)
  elseif name == "gear" then
    for i = 0, 7 do
      local a = i * math.pi / 4
      gfx.line(cx + math.cos(a) * r * .45, cy + math.sin(a) * r * .45, cx + math.cos(a) * r * .8, cy + math.sin(a) * r * .8, c, t * 1.6)
    end
    gfx.ring(cx, cy, r * .55, c, t * 1.4)
  elseif name == "home" then
    gfx.tri(cx - r * .7, cy, cx + r * .7, cy, cx, cy - r * .65, c); gfx.rect(cx - r * .45, cy, r * .9, r * .6, c)
  elseif name == "menu" then
    for i = -1, 1 do gfx.rect(cx - r * .6, cy + i * r * .4 - t / 2, r * 1.2, t, c) end
  elseif name == "dot" then
    gfx.circle(cx, cy, r * .35, c)
  end
end

-- ---- widgets ----------------------------------------------------------------------------------
-- ui.button(x, y, w, h, label, opts) -> clicked
-- opts: style = "primary" | "flat" | "outline" | "danger", color, fg, size, radius, icon, disabled
function ui.button(x, y, w, h, label, opts)
  opts = opts or {}
  local clicked, pressing = false, false
  if not opts.disabled then clicked, pressing = ui.hit(x, y, w, h) end
  local style = opts.style or "primary"
  local bg = opts.color or (style == "primary" and T.accent) or (style == "danger" and T.err) or T.panel2
  local fg = opts.fg or ((style == "primary" or style == "danger") and T.accent_fg) or T.fg
  local r = opts.radius or math.min(S(14), h / 2)
  if opts.disabled then gfx.alpha(110) end
  if style == "outline" then
    gfx.frame(x, y, w, h, opts.color or T.line, math.max(1, S(2)), r)
    if pressing then gfx.rect(x, y, w, h, T.press, r) end
  elseif style ~= "flat" or pressing or opts.color then
    gfx.rect(x, y, w, h, pressing and (style == "flat" and T.press or ui.shade(bg, 0.75)) or bg, r)
  end
  local size = opts.size or ui.font.body
  if opts.icon and (label == nil or label == "") then
    ui.icon(opts.icon, x + w / 2, y + h / 2, math.min(w, h) * 0.55, fg)
  elseif opts.icon then
    local isz = size * 1.1
    local tw = gfx.text_width(label, size)
    local x0 = x + (w - tw - isz - S(8)) / 2
    ui.icon(opts.icon, x0 + isz / 2, y + h / 2, isz, fg)
    gfx.text(x0 + isz + S(8), y + (h - gfx.font_height(size)) / 2, label, size, fg)
  elseif label then
    gfx.text(x + w / 2, y + (h - gfx.font_height(size)) / 2, label, size, fg, "center")
  end
  if opts.disabled then gfx.alpha(255) end
  return clicked
end
-- darker / lighter colour: ui.shade(0x4C8DFF, 0.8)
function ui.shade(c, k)
  local r, g, b = (c >> 16) & 255, (c >> 8) & 255, c & 255
  return gfx.rgb(r * k, g * k, b * k)
end
-- mix two colours, t = 0..1
function ui.mix(a, b, t)
  local function ch(c, sh) return (c >> sh) & 255 end
  return gfx.rgb(ch(a, 16) + (ch(b, 16) - ch(a, 16)) * t, ch(a, 8) + (ch(b, 8) - ch(a, 8)) * t, ch(a, 0) + (ch(b, 0) - ch(a, 0)) * t)
end

-- ui.toggle(x, y, on, label) -> on   (a switch; the label sits to its right)
function ui.toggle(x, y, on, label, opts)
  opts = opts or {}
  local w, h = S(64), S(36)
  local lw = label and gfx.text_width(label, ui.font.body) + S(16) or 0
  if ui.hit(x, y, w + lw, h) then on = not on end
  gfx.rect(x, y, w, h, on and (opts.color or T.accent) or T.line, h / 2)
  gfx.circle(on and x + w - h / 2 or x + h / 2, y + h / 2, h / 2 - S(4), 0xFFFFFF)
  if label then gfx.text(x + w + S(16), y + (h - gfx.font_height(ui.font.body)) / 2, label, ui.font.body, T.fg) end
  return on
end

-- ui.checkbox(x, y, on, label) -> on
function ui.checkbox(x, y, on, label)
  local b = S(32)
  local lw = label and gfx.text_width(label, ui.font.body) + S(14) or 0
  if ui.hit(x, y, b + lw, b) then on = not on end
  if on then gfx.rect(x, y, b, b, T.accent, S(8)); ui.icon("check", x + b / 2, y + b / 2, b * .8, 0xFFFFFF)
  else gfx.frame(x, y, b, b, T.dim, S(2), S(8)) end
  if label then gfx.text(x + b + S(14), y + (b - gfx.font_height(ui.font.body)) / 2, label, ui.font.body, T.fg) end
  return on
end

-- ui.slider(x, y, w, value, min, max, opts) -> value, changed   (opts.step, opts.color)
local drag = nil
function ui.slider(x, y, w, value, lo, hi, opts)
  opts = opts or {}
  lo, hi = lo or 0, hi or 1
  local h, kr = S(40), S(14)
  local p = nv.pointer
  local id = opts.id or (x .. ":" .. y)
  local changed = false
  if not blocked then
    if p.pressed and inside(x - kr, y, w + 2 * kr, h, p.x, p.y) then drag = id end
    if drag == id then
      if p.down then
        local t = math.max(0, math.min(1, (p.x - x) / w))
        local v = lo + t * (hi - lo)
        if opts.step then v = math.floor((v - lo) / opts.step + 0.5) * opts.step + lo end
        if v ~= value then value, changed = v, true end
        p.moved = true   -- a drag is not a tap for the widgets underneath
      else
        drag = nil
        changed = true
        if opts.on_release then opts.on_release(value) end
      end
    end
  end
  local t = (hi > lo) and (value - lo) / (hi - lo) or 0
  t = math.max(0, math.min(1, t))
  local cy = y + h / 2
  gfx.rect(x, cy - S(3), w, S(6), T.line, S(3))
  gfx.rect(x, cy - S(3), w * t, S(6), opts.color or T.accent, S(3))
  gfx.circle(x + w * t, cy, drag == id and kr + S(3) or kr, 0xFFFFFF)
  return value, changed
end

-- ui.progress(x, y, w, h, frac, color)
function ui.progress(x, y, w, h, frac, color)
  frac = math.max(0, math.min(1, frac or 0))
  gfx.rect(x, y, w, h, T.line, h / 2)
  if frac > 0 then gfx.rect(x, y, math.max(h, w * frac), h, color or T.accent, h / 2) end
end

-- ui.gauge(cx, cy, r, frac, color, thick) -- a 270 degree dial
function ui.gauge(cx, cy, r, frac, color, thick)
  thick = thick or S(14)
  gfx.arc(cx, cy, r, 135, 405, T.line, thick)
  frac = math.max(0, math.min(1, frac or 0))
  if frac > 0 then gfx.arc(cx, cy, r, 135, 135 + 270 * frac, color or T.accent, thick) end
end

-- ui.header(title, opts) -> back_clicked, action_clicked
-- opts: back = true (an arrow on the left), action = icon name or label on the right, h
function ui.header(title, opts)
  opts = opts or {}
  local h = opts.h or S(64)
  gfx.rect(0, 0, W, h, opts.color or T.panel)
  local x = S(20)
  local back = false
  if opts.back then
    back = ui.button(S(8), S(8), h - S(16), h - S(16), nil, { style = "flat", icon = "back" })
    x = h + S(4)
  end
  gfx.text(x, (h - gfx.font_height(ui.font.big)) / 2, title or "", ui.font.big, T.fg)
  local act = false
  if opts.action then
    local isicon = ({ back = 1, next = 1, close = 1, plus = 1, minus = 1, play = 1, pause = 1, stop = 1, check = 1,
                      refresh = 1, power = 1, bulb = 1, gear = 1, home = 1, menu = 1 })[opts.action]
    local bw = isicon and h - S(16) or gfx.text_width(opts.action, ui.font.body) + S(32)
    local label, icon = opts.action, nil
    if isicon then label, icon = nil, opts.action end
    act = ui.button(W - bw - S(8), S(8), bw, h - S(16), label, { style = "flat", icon = icon })
  end
  return back, act
end

-- ui.tabs(x, y, w, h, labels, selected) -> selected
function ui.tabs(x, y, w, h, labels, sel)
  local n = #labels
  local tw = w / n
  gfx.rect(x, y, w, h, T.panel2, h / 2)
  for i, l in ipairs(labels) do
    local tx = x + (i - 1) * tw
    if ui.hit(tx, y, tw, h) then sel = i end
    if i == sel then gfx.rect(tx + S(3), y + S(3), tw - S(6), h - S(6), T.accent, (h - S(6)) / 2) end
    gfx.text(tx + tw / 2, y + (h - gfx.font_height(ui.font.body)) / 2, l, ui.font.body, i == sel and T.accent_fg or T.fg, "center")
  end
  return sel
end

-- ui.grid(x, y, w, h, cols, rows, gap) -> cell(c, r, cspan, rspan) -> x, y, w, h   (1-based)
function ui.grid(x, y, w, h, cols, rows, gap)
  gap = gap or S(10)
  local cw, rh = (w - gap * (cols - 1)) / cols, (h - gap * (rows - 1)) / rows
  return function(c, r, cs, rs)
    cs, rs = cs or 1, rs or 1
    return x + (c - 1) * (cw + gap), y + (r - 1) * (rh + gap), cw * cs + gap * (cs - 1), rh * rs + gap * (rs - 1)
  end
end

-- ui.keys(x, y, w, h, rows, opts) -> label of the key tapped (nil)
-- rows = { {"7","8","9","/"}, {"4","5","6","*"}, ... }; a key "0:2" spans two columns
-- opts.style(label) -> button opts for that key (colour per key)
function ui.keys(x, y, w, h, rows, opts)
  opts = opts or {}
  local cols = 0
  for _, r in ipairs(rows) do
    local n = 0
    for _, k in ipairs(r) do n = n + (tonumber(k:match(":(%d+)$")) or 1) end
    cols = math.max(cols, n)
  end
  local cell = ui.grid(x, y, w, h, cols, #rows, opts.gap)
  local hit
  for ri, r in ipairs(rows) do
    local c = 1
    for _, k in ipairs(r) do
      local label, span = k:match("^(.-):(%d+)$")
      label, span = label or k, tonumber(span) or 1
      local bx, by, bw, bh = cell(c, ri, span, 1)
      local bo = opts.style and opts.style(label) or { style = "flat", color = T.panel2 }
      if bo.style == nil then bo.style = "primary" end
      bo.size = bo.size or opts.size or ui.font.big
      if label ~= "" and ui.button(bx, by, bw, bh, label, bo) then hit = label end
      c = c + span
    end
  end
  return hit
end

-- ui.list(id, x, y, w, h, items, opts) -> index tapped (nil)
-- items[i] = "text" or {title=, sub=, right=, color=}; opts.row_h, opts.selected, opts.size
local scrolls = {}
function ui.list(id, x, y, w, h, items, opts)
  opts = opts or {}
  local rh = opts.row_h or S(64)
  local st = scrolls[id] or { y = 0 }
  scrolls[id] = st
  local p = nv.pointer
  local total = #items * rh
  local maxs = math.max(0, total - h)
  if not blocked and p.down and inside(x, y, w, h, p.sx, p.sy) then
    if st.last then st.y = st.y - (p.y - st.last) end
    st.last = p.y
    nv.redraw()
  else
    st.last = nil
  end
  st.y = math.max(0, math.min(maxs, st.y))
  gfx.clip(x, y, w, h)
  local tapped
  local first = math.max(1, math.floor(st.y / rh) + 1)
  for i = first, math.min(#items, first + math.ceil(h / rh) + 1) do
    local it = items[i]
    if type(it) ~= "table" then it = { title = tostring(it) } end
    local ry = y + (i - 1) * rh - st.y
    local clicked, pressing = ui.hit(x, math.max(y, ry), w, math.min(rh, y + h - math.max(y, ry)))
    if clicked then tapped = i end
    if pressing or opts.selected == i then gfx.rect(x, ry, w, rh, pressing and T.press or T.panel2) end
    local size = opts.size or ui.font.body
    if it.sub then
      gfx.text(x + S(16), ry + rh / 2 - gfx.font_height(size) + S(2), it.title or "", size, it.color or T.fg)
      gfx.text(x + S(16), ry + rh / 2 + S(2), it.sub, ui.font.small, T.dim)
    else
      gfx.text(x + S(16), ry + (rh - gfx.font_height(size)) / 2, it.title or "", size, it.color or T.fg)
    end
    if it.right then gfx.text(x + w - S(16), ry + (rh - gfx.font_height(size)) / 2, it.right, size, T.dim, "right") end
    gfx.rect(x + S(16), ry + rh - 1, w - S(32), 1, T.line)
  end
  gfx.clip()
  if total > h then   -- scroll bar
    local bh = math.max(S(30), h * h / total)
    gfx.rect(x + w - S(5), y + (h - bh) * st.y / maxs, S(4), bh, T.dim, S(2))
  end
  return tapped
end
function ui.list_scroll(id, v) if v then scrolls[id] = { y = v } end return scrolls[id] and scrolls[id].y or 0 end

-- ---- text input: ui.prompt(title, initial, function(text) ... end) ----------------------------
-- An on-screen keyboard over the app (a physical keyboard types into it too). cb(nil) = cancelled.
local KROWS = { { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
                { "q", "w", "e", "r", "t", "y", "u", "i", "o", "p" },
                { "a", "s", "d", "f", "g", "h", "j", "k", "l", "'" },
                { "shift", "z", "x", "c", "v", "b", "n", "m", ".", "del" },
                { "sym", "-", "/", "space", ":", "_", "ok" } }
local KSYM = { ["1"] = "!", ["2"] = "@", ["3"] = "#", ["4"] = "$", ["5"] = "%", ["6"] = "&", ["7"] = "*",
               ["8"] = "(", ["9"] = ")", ["0"] = "=", ["'"] = "\"", ["."] = ",", ["-"] = "+", ["/"] = "?",
               [":"] = ";", ["_"] = "~" }
function ui.prompt(title, initial, cb)
  ui._modal = { title = title, text = tostring(initial or ""), cb = cb, shift = false, sym = false }
  nv.redraw()
end
local function modal_done(text)
  local m = ui._modal
  ui._modal = nil
  nv.redraw()
  if m and m.cb then m.cb(text) end
end
function ui._key(ev)        -- physical keys while the keyboard is up; true = consumed
  local m = ui._modal
  if not m or not ev.down then return m ~= nil end
  if ev.key == "enter" or ev.key == "kpenter" then modal_done(m.text)
  elseif ev.key == "escape" then modal_done(nil)
  elseif ev.key == "backspace" then m.text = m.text:sub(1, -2)
  elseif ev.text then m.text = m.text .. ev.text end
  nv.redraw()
  return true
end
local function draw_modal()
  local m = ui._modal
  blocked = false
  gfx.alpha(170); gfx.rect(0, 0, W, H, 0x000000); gfx.alpha(255)
  local pad = S(16)
  local kh = H * 0.58
  local ky = H - kh
  gfx.rect(0, ky - S(120), W, kh + S(120), T.panel)
  gfx.text(pad, ky - S(110), m.title or "", ui.font.body, T.dim)
  gfx.rect(pad, ky - S(76), W - 2 * pad, S(60), T.bg, S(10))
  local shown = m.text
  local fs = ui.font.big
  while gfx.text_width(shown .. "|", fs) > W - 4 * pad and #shown > 0 do shown = shown:sub(2) end
  gfx.text(pad * 2, ky - S(76) + (S(60) - gfx.font_height(fs)) / 2, shown .. ((nv.millis() // 500) % 2 == 0 and "|" or " "), fs, T.fg)
  nv.redraw()   -- the cursor blinks
  local cell = ui.grid(pad, ky + pad, W - 2 * pad, kh - 2 * pad, 10, 5, S(8))
  for ri, row in ipairs(KROWS) do
    local c = 1
    for _, k in ipairs(row) do
      local span = (k == "space") and 4 or 1
      local bx, by, bw, bh = cell(c, ri, span, 1)
      local label = k
      if k == "space" then label = " "
      elseif k == "del" then label = "<-"
      elseif k == "ok" then label = "OK"
      elseif k == "shift" then label = "^"
      elseif k == "sym" then label = m.sym and "abc" or "#+="
      elseif m.sym and KSYM[k] then label = KSYM[k]
      elseif m.shift then label = k:upper() end
      local st = (k == "ok") and "primary" or "flat"
      if ui.button(bx, by, bw, bh, label, { style = st, color = st == "flat" and (k == "shift" and m.shift and T.press or T.panel2) or nil, size = ui.font.big }) then
        if k == "ok" then modal_done(m.text); return
        elseif k == "del" then m.text = m.text:sub(1, -2)
        elseif k == "shift" then m.shift = not m.shift
        elseif k == "sym" then m.sym = not m.sym
        elseif k == "space" then m.text = m.text .. " "
        else m.text = m.text .. label; m.shift = false end
      end
      c = c + span
    end
  end
end
function ui._end()
  if ui._modal then draw_modal() end
end
function ui._back()
  if ui._modal then modal_done(nil); return true end
  return false
end

return ui
