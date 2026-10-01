-- RPN Calc - a reverse Polish notation calculator in the HP style for NucleoOS (Lua App engine).
-- Type a number, ENTER pushes it, an operation takes its operands from the stack. The stack and
-- the memory survive restarts. Keyboard: digits, Enter, + - * / ^, Backspace, Esc = clear all.
-- Tap the stack display to switch sin/cos/tan to their inverses.
local T, S = ui.theme, ui.S
local stack = nv.load("stack", {})
local mem = nv.load("mem", 0)
local entry = ""            -- the number being typed
local undo = nil
local deg = nv.load("deg", true)
local inv = false
local err = nil

local function save() nv.save("stack", stack); nv.save("mem", mem) end
local function snapshot() local c = {} for i, v in ipairs(stack) do c[i] = v end undo = c end
local function commit()
  if entry ~= "" then
    local v = tonumber(entry)
    if v then stack[#stack + 1] = v end
    entry = ""
  end
end
local function pop() return table.remove(stack) end
local function need(n)
  commit()
  if #stack < n then err = nv.tr("Too few values", "Valori insufficienti"); nv.tone(220, 80); return false end
  snapshot()
  return true
end
local function un(f) if need(1) then stack[#stack] = f(stack[#stack]) end end
local function bin(f) if need(2) then local x = pop(); local y = pop(); stack[#stack + 1] = f(y, x) end end
local function rad(a) return deg and math.rad(a) or a end
local function arc(a) return deg and math.deg(a) or a end

local OPS = {
  ["+"] = function() bin(function(y, x) return y + x end) end,
  ["-"] = function() bin(function(y, x) return y - x end) end,
  ["×"] = function() bin(function(y, x) return y * x end) end,
  ["÷"] = function() bin(function(y, x) return y / x end) end,
  ["y^x"] = function() bin(function(y, x) return y ^ x end) end,
  ["%"] = function() bin(function(y, x) return y * x / 100 end) end,
  ["√x"] = function() un(math.sqrt) end,
  ["x²"] = function() un(function(x) return x * x end) end,
  ["1/x"] = function() un(function(x) return 1 / x end) end,
  ["ln"] = function() un(math.log) end,
  ["log"] = function() un(function(x) return math.log(x, 10) end) end,
  ["e^x"] = function() un(math.exp) end,
  ["sin"] = function() un(function(x) return math.sin(rad(x)) end) end,
  ["cos"] = function() un(function(x) return math.cos(rad(x)) end) end,
  ["tan"] = function() un(function(x) return math.tan(rad(x)) end) end,
  ["asin"] = function() un(function(x) return arc(math.asin(x)) end) end,
  ["acos"] = function() un(function(x) return arc(math.acos(x)) end) end,
  ["atan"] = function() un(function(x) return arc(math.atan(x)) end) end,
  ["n!"] = function() un(function(x) local r = 1 for i = 2, math.floor(x) do r = r * i end return r end) end,
  ["π"] = function() commit(); snapshot(); stack[#stack + 1] = math.pi end,
  ["±"] = function()
    if entry ~= "" then entry = entry:sub(1, 1) == "-" and entry:sub(2) or "-" .. entry
    else un(function(x) return -x end) end
  end,
  ["ENTER"] = function()
    if entry ~= "" then snapshot(); commit()
    elseif #stack > 0 then snapshot(); stack[#stack + 1] = stack[#stack] end
  end,
  ["DROP"] = function() commit(); if #stack > 0 then snapshot(); pop() end end,
  ["SWAP"] = function() if need(2) then stack[#stack], stack[#stack - 1] = stack[#stack - 1], stack[#stack] end end,
  ["CLR"] = function() snapshot(); stack = {}; entry = "" end,
  ["UNDO"] = function() if undo then stack = undo; undo = nil; entry = "" end end,
  ["STO"] = function() commit(); if #stack > 0 then mem = stack[#stack] end end,
  ["RCL"] = function() commit(); snapshot(); stack[#stack + 1] = mem end,
  ["DEL"] = function() if entry ~= "" then entry = entry:sub(1, -2) else commit(); if #stack > 0 then snapshot(); pop() end end end,
  ["DEG"] = function() deg = not deg; nv.save("deg", deg) end,
}

local function press(k)
  err = nil
  if k:match("^%d$") then
    if #entry < 16 then entry = entry .. k end
  elseif k == "." then
    if not entry:find("%.") then entry = (entry == "" and "0" or entry) .. "." end
  elseif OPS[k] then
    OPS[k]()
  end
  save()
  nv.redraw()
end

local function fmt(v)
  if v ~= v then return nv.tr("not a number", "non è un numero") end
  if v == math.huge or v == -math.huge then return (v < 0 and "-" or "") .. "inf" end
  if v == math.floor(v) and math.abs(v) < 1e15 then return string.format("%d", v) end
  return string.format("%.12g", v)
end

local KEYS = {
  { "sin", "cos", "tan", "π", "DEG" },
  { "√x", "x²", "y^x", "1/x", "%" },
  { "ln", "log", "e^x", "n!", "SWAP" },
  { "STO", "RCL", "DEL", "÷", "DROP" },
  { "7", "8", "9", "×", "UNDO" },
  { "4", "5", "6", "-", "CLR" },
  { "1", "2", "3", "+", "ENTER" },
  { "0:2", ".", "±" },
}

local function key_style(l)
  if l:match("^[%d%.]$") or l == "±" then return { style = "flat", color = 0x2A3240, size = ui.font.big } end
  if l == "ENTER" then return { style = "primary" } end
  if l == "+" or l == "-" or l == "×" or l == "÷" then return { style = "flat", color = 0x3A4A66, size = ui.font.big } end
  if l == "CLR" then return { style = "flat", color = 0x5A2A30 } end
  return { style = "flat", color = T.panel2 }
end

function nv.draw()
  ui.clear()
  local dw = S(430)
  ui.panel(S(16), S(16), dw, ui.H - S(32), T.panel)
  local step = S(52)
  local lines = math.floor((ui.H - S(140)) / step)
  local x1 = S(16) + dw - S(24)
  local y = ui.H - S(108)
  if entry ~= "" or #stack == 0 then
    gfx.text(x1, y, entry ~= "" and entry .. "_" or "0", ui.font.title, T.fg, "right")
    y = y - step
  end
  for i = #stack, math.max(1, #stack - lines + 1), -1 do
    local level = #stack - i + 1 + (entry ~= "" and 1 or 0)
    local name = ({ "x", "y", "z", "t" })[level] or tostring(level)
    gfx.text(S(36), y + S(8), name, ui.font.small, T.dim)
    gfx.text(x1, y, fmt(stack[i]), ui.font.title, (entry == "" and i == #stack) and T.fg or 0xB9C2CF, "right")
    y = y - step
  end
  gfx.text(S(36), ui.H - S(52), (deg and "DEG" or "RAD") .. (inv and "  INV" or "") .. "   M = " .. fmt(mem), ui.font.small, T.dim)
  if err then gfx.text(x1, ui.H - S(52), err, ui.font.small, T.err, "right") end
  -- keypad
  local kx = S(16) + dw + S(16)
  local rows = {}
  for r, row in ipairs(KEYS) do
    rows[r] = {}
    for c, k in ipairs(row) do
      if inv and (k == "sin" or k == "cos" or k == "tan") then k = "a" .. k end
      if k == "DEG" and not deg then k = "RAD" end
      rows[r][c] = k
    end
  end
  local k = ui.keys(kx, S(16), ui.W - kx - S(16), ui.H - S(32), rows, { gap = S(8), size = ui.font.body, style = key_style })
  if k == "RAD" then k = "DEG" end
  if k then press(k) end
end

function nv.tap(x, y)
  if x < S(446) then inv = not inv; nv.redraw() end
end

local KMAP = { ["kp+"] = "+", ["-"] = "-", ["kp-"] = "-", ["kp*"] = "×", ["kp/"] = "÷", ["/"] = "÷",
               enter = "ENTER", kpenter = "ENTER", backspace = "DEL", escape = "CLR", ["."] = ".", ["kp."] = "." }
function nv.key(ev)
  if not ev.down then return end
  local t = ev.text
  if t == "*" then return press("×") end
  if t == "^" then return press("y^x") end
  if t == "+" then return press("+") end
  if t == "%" then return press("%") end
  if t and t:match("^%d$") then return press(t) end
  if KMAP[ev.key] then press(KMAP[ev.key]); return true end
end
