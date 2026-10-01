local utils = {}

function utils.round(num) 
	if num >= 0 then 
		return math.floor(num + 0.5) 
    end
	
    return math.ceil(num - 0.5)
end

return utils