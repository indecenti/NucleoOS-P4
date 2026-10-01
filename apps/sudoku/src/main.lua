-- Sudoku for NucleoOS (Lua App engine). The puzzle generator and solver are sudoku.lua by
-- Azdren Ymeri (MIT, https://github.com/azdrenymeri/sudoku), unchanged; this file is the touch UI.
require "sudoku"
local T, S = ui.theme, ui.S

local game                -- { grid = 9x9, given = 9x9 bool, sol = 9x9, level, t = seconds, done }
local sel_r, sel_c = 5, 5
local level = nv.load("level", 0)
local LEVELS = { [0] = nv.tr("Easy", "Facile"), [1] = nv.tr("Medium", "Medio"), [2] = nv.tr("Hard", "Difficile") }

local function copy(b)
  local o = {}
  for r = 1, 9 do o[r] = {} for c = 1, 9 do o[r][c] = b[r][c] end end
  return o
end

local function new_game()
  local full = Sudoku.generateFullSolutionBoard(Sudoku.createEmptyBoard())
  local puzzle = Sudoku.createPuzzle(copy(full), level)
  game = { grid = copy(puzzle), given = {}, sol = full, level = level, t = 0, done = false }
  for r = 1, 9 do
    game.given[r] = {}
    for c = 1, 9 do game.given[r][c] = puzzle[r][c] ~= 0 end
  end
  nv.save("game", game)
end

-- a digit clashes when the same one sits in its row, column or box
local function clash(r, c)
  local v = game.grid[r][c]
  if v == 0 then return false end
  for i = 1, 9 do
    if i ~= c and game.grid[r][i] == v then return true end
    if i ~= r and game.grid[i][c] == v then return true end
  end
  local br, bc = (r - 1) // 3 * 3, (c - 1) // 3 * 3
  for rr = br + 1, br + 3 do
    for cc = bc + 1, bc + 3 do
      if (rr ~= r or cc ~= c) and game.grid[rr][cc] == v then return true end
    end
  end
  return false
end

local function solved()
  for r = 1, 9 do for c = 1, 9 do
    if game.grid[r][c] == 0 or clash(r, c) then return false end
  end end
  return true
end

local function put(v)
  if game.done or game.given[sel_r][sel_c] then return end
  game.grid[sel_r][sel_c] = v
  if solved() then
    game.done = true
    nv.melody({ { 523, 120 }, { 659, 120 }, { 784, 120 }, { 1047, 300 } })
    local best = nv.load("best" .. game.level)
    if not best or game.t < best then nv.save("best" .. game.level, math.floor(game.t)) end
  else
    nv.tone(v == 0 and 330 or 880, 25)
  end
  nv.save("game", game)
end

function nv.init()
  game = nv.load("game")
  if type(game) ~= "table" or not game.grid then new_game() end
  nv.every(1, function() if not game.done then game.t = game.t + 1 end end)
end

local function fmt_time(t) return string.format("%d:%02d", t // 60, t % 60) end

function nv.draw()
  ui.clear()
  local B = S(564)
  local bx, by = S(18), (ui.H - B) // 2
  local cs = B / 9
  gfx.rect(bx, by, B, B, 0xF4F1EA, S(8))
  local sv = game.grid[sel_r][sel_c]
  for r = 1, 9 do
    for c = 1, 9 do
      local x, y = bx + (c - 1) * cs, by + (r - 1) * cs
      local same_box = (r - 1) // 3 == (sel_r - 1) // 3 and (c - 1) // 3 == (sel_c - 1) // 3
      if r == sel_r and c == sel_c then gfx.rect(x, y, cs, cs, 0x9CC3FF)
      elseif r == sel_r or c == sel_c or same_box then gfx.rect(x, y, cs, cs, 0xE2E8F2)
      elseif sv ~= 0 and game.grid[r][c] == sv then gfx.rect(x, y, cs, cs, 0xC9DAF5) end
      local v = game.grid[r][c]
      if v ~= 0 then
        local col = game.given[r][c] and 0x1B1F27 or (clash(r, c) and 0xD9363E or 0x2D63C8)
        gfx.text(x + cs / 2, y + (cs - gfx.font_height(ui.font.huge)) / 2, tostring(v), ui.font.huge, col, "center")
      end
      if ui.hit(x, y, cs, cs) then sel_r, sel_c = r, c end
    end
  end
  for i = 0, 9 do
    local w = (i % 3 == 0) and S(3) or 1
    gfx.rect(bx + i * cs - w / 2, by, w, B, 0x1B1F27)
    gfx.rect(bx, by + i * cs - w / 2, B, w, 0x1B1F27)
  end

  -- right panel
  local px = bx + B + S(28)
  local pw = ui.W - px - S(18)
  ui.label(px, by, "Sudoku", ui.font.title)
  ui.label(px + pw, by + S(6), LEVELS[game.level] .. "  " .. fmt_time(game.t), ui.font.body, T.dim, "right")
  local best = nv.load("best" .. game.level)
  if best then ui.label(px + pw, by + S(34), nv.tr("Best ", "Record ") .. fmt_time(best), ui.font.small, T.dim, "right") end
  local k = ui.keys(px, by + S(70), pw, S(300), { { "1", "2", "3" }, { "4", "5", "6" }, { "7", "8", "9" } },
    { size = ui.font.title, style = function() return { style = "flat", color = T.panel2 } end })
  if k then put(tonumber(k)) end
  if ui.button(px, by + S(384), pw, S(64), nv.tr("Erase", "Cancella"), { style = "outline" }) then put(0) end
  if game.done then
    ui.label(px + pw / 2, by + S(462), nv.tr("Solved!", "Risolto!"), ui.font.big, T.ok, "center")
  end
  local lw = (pw - S(16)) / 3
  for i = 0, 2 do
    if ui.button(px + i * (lw + S(8)), by + B - S(56), lw, S(56), LEVELS[i],
                 { style = (i == level) and "primary" or "flat", color = (i ~= level) and T.panel2 or nil, size = ui.font.small }) then
      level = i
      nv.save("level", level)
      new_game()
    end
  end
  ui.label(px, by + B - S(84), nv.tr("New game:", "Nuova partita:"), ui.font.small, T.dim)
end

function nv.key(ev)
  if not ev.down then return end
  local k = ev.key
  if k == "up" then sel_r = math.max(1, sel_r - 1)
  elseif k == "down" then sel_r = math.min(9, sel_r + 1)
  elseif k == "left" then sel_c = math.max(1, sel_c - 1)
  elseif k == "right" then sel_c = math.min(9, sel_c + 1)
  elseif k:match("^kp?%d$") then put(tonumber(k:sub(-1)))
  elseif k == "backspace" or k == "delete" then put(0) end
end
