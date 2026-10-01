-- Robo House - modules/floors.lua
-- 2020 Foppygames

local floors = {}

local aspect = require("modules.aspect")
local utils = require("modules.utils")

local FLOOR_COUNT = 3
local FLOOR_Y_START = utils.round(aspect.GAME_HEIGHT*0.9)
local FLOOR_Y_STEP = utils.round(FLOOR_Y_START / FLOOR_COUNT)
local FLOOR_HEIGHT = 3
local FLOOR_WIDTH = utils.round(aspect.GAME_WIDTH * 1)
local FLOOR_X = utils.round((aspect.GAME_WIDTH - FLOOR_WIDTH) / 2)
local FLOOR_COLOR = utils.getColorFromRgb(255,163,0)

function floors.draw()
    love.graphics.setColor(FLOOR_COLOR)
    for i = 1, FLOOR_COUNT do
        love.graphics.rectangle("fill",FLOOR_X,FLOOR_Y_START-(i-1)*FLOOR_Y_STEP,FLOOR_WIDTH,FLOOR_HEIGHT)
    end
end

-- returns x and y of location on provided floor at provided fraction of floor width
function floors.getLocation(floor,floorXFraction)
    if floor > FLOOR_COUNT then
        return 0, 0
    end
    local y = FLOOR_Y_START - (floor-1) * FLOOR_Y_STEP
    local x = utils.round(FLOOR_X + floorXFraction * FLOOR_WIDTH)
    return x, y
end

-- returns fraction of floor width corresponding to provided x
function floors.getFraction(x)
    return (x - FLOOR_X) / FLOOR_WIDTH
end

-- returns number of floor and true and new y, or false and nil and old y
function floors.land(x,w,oldY,newY)
    local diff = math.abs(x - (FLOOR_X+FLOOR_WIDTH/2))
    if diff > (FLOOR_WIDTH/2 + w/2) then
        return false, nil, newY
    end
    for i = 1, FLOOR_COUNT do
        local floorY = FLOOR_Y_START-(i-1)*FLOOR_Y_STEP
        if oldY <= floorY and newY > floorY then
            return true, i, floorY
        end
    end
    return false, nil, newY 
end

function floors.getCount()
    return FLOOR_COUNT
end

function floors.getYStep()
    return FLOOR_Y_STEP
end

function floors.getHeight()
    return FLOOR_HEIGHT
end

return floors