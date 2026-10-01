-- asteroids\utils.lua
-- Copyright (c) 2017-2018 Jon Thysell

local utils = {}

function utils.vectorAdd(v1, v2)
    return {
        dx = v1.dx + v2.dx,
        dy = v1.dy + v2.dy,
    }
end

function utils.bound(value, min, max)
    if value < min then return min end
    if value > max then return max end
    return value
end

function utils.map(value, amin, amax, bmin, bmax)
    return bmin + (value - amin) * ((bmax - bmin) / (amax - amin))
end

function utils.distance(x1, y1, x2, y2)
    return math.sqrt((x2 - x1)^2 + (y2 - y1)^2)
end

function utils.within(x, y, c)
    return utils.distance(x, y, c.x, c.y) <= c.r
end

function utils.collision(c1, c2)
    return utils.distance(c1.x, c1.y, c2.x, c2.y) < (c1.r + c2.r)
end

function utils.passThreshold(before, after, threshold)
    return math.floor(before / threshold) < math.floor(after / threshold)
end

function utils.comma_value(n) -- credit http://richard.warburton.it
	local left,num,right = string.match(n,'^([^%d]*%d)(%d*)(.-)$')
	return left..(num:reverse():gsub('(%d%d%d)','%1,'):reverse())..right
end

return utils;