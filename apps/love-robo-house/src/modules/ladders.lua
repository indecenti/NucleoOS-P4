-- Robo House - modules/ladders.lua
-- 2020 Foppygames

local ladders = {}

local floors = require("modules.floors")
local utils = require("modules.utils")

local LADDER_WIDTH = 16
local LADDER_COLOR = utils.getColorFromRgb(255,236,39)
local LADDER_STEP_SIZE = 10
local LADDER_LINE_WIDTH = 3

local list = {}

function ladders.draw()
    love.graphics.setColor(LADDER_COLOR)
    for i = 1, #list do
        local ladder = list[i]
        love.graphics.rectangle("fill",ladder.x-ladder.w/2,ladder.y-ladder.h,LADDER_LINE_WIDTH,ladder.h)
        love.graphics.rectangle("fill",ladder.x+ladder.w/2-LADDER_LINE_WIDTH,ladder.y-ladder.h,LADDER_LINE_WIDTH,ladder.h)
        for i = 1, ladder.steps do
            love.graphics.rectangle("fill",ladder.x-ladder.w/2,ladder.y-LADDER_STEP_SIZE*i,LADDER_WIDTH,LADDER_LINE_WIDTH)
        end
    end
end

function ladders.init()
    local floorCount = floors.getCount()
    for i = 1, floorCount-1 do
        for j = 0, 1 do
            local floorXFraction = 0.1 + (i-1) * 0.2
            if j == 1 then
                floorXFraction = 1 - floorXFraction
            end
            local x, y = floors.getLocation(i,floorXFraction)
            local h = floors.getYStep() + floors.getHeight() * 2
            local steps = h / LADDER_STEP_SIZE
            if steps < 0 then
                steps = 0
            end
            table.insert(list,{
                x = x,
                y = y,
                w = LADDER_WIDTH,
                h = h,
                steps = steps,
                floor = i
            })
        end
    end 
end

-- returns floor of ladder colliding with provided area and true, or nil and false
function ladders.grab(x,y,w,h)
    for i = 1, #list do
        local ladder = list[i]
        if math.abs(ladder.x - x) < (w + ladder.w) / 2 then
            local diffY = math.abs(ladder.y - y)
            if y > (ladder.y - ladder.h) and y < (ladder.y + h) then
                return ladder.floor, true
            end
        end
    end
    return nil, false
end

return ladders