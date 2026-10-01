-- Robo House - modules/utils.lua
-- 2020 Foppygames

local utils = {}

function utils.round(num) 
	if num >= 0 then 
		return math.floor(num+.5) 
	else 
		return math.ceil(num-.5)
	end
end

function utils.getColorFromRgb(r,g,b) 
	return {r/255,g/255,b/255}
end

return utils