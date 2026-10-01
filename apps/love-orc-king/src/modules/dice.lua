-- dice is a module that rolls dice

local dice = {}

-- rolls dice constructed out of two ranges and returns result range
function dice.roll(range1,range2)
	-- negative ranges not allowed
	if range1 < 0 then
		range1 = 0
	end
	if range2 < 0 then
		range2 = 0
	end

	-- special case: both ranges zero
	if range1 < 1 and range2 < 1 then
		range1 = 1
		range2 = 1
	end

	-- compute number of sides
	local total = range1 + range2

	-- roll dice
	local result = math.random(1,total)

	-- result is in first range
	if result <= range1 then
		return 1
	end
    
	-- result is in second range
	return 2
end

return dice