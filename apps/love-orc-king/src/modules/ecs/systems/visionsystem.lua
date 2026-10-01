local entityManager = require("modules.ecs.managers.entitymanager")
local world = require("modules.world")

local visionSystem = {}

-- returns first entity having camera and position
function visionSystem.update()
	-- Note: visible locations are registered for every matching entity; perhaps
	-- for ai entities this is not always necessary and this can be simplified
	local entities = entityManager.getEntitiesHaving({"vision","position"})

	for i = 1, #entities do
		entities[i].vision.visible = visionSystem.getVisibleLocations(entities[i])
	end

	-- return first entity having camera and position, typically used in displaying world
	entities = entityManager.getEntitiesHaving({"camera","position"})

	return entities[1]
end

-- source: https://doomwiki.org/wiki/approximate_distance
-- Note: this is not used at the moment
local function approxDistance(dX,dY)
    dX = math.abs(dX)
	dY = math.abs(dY)

	if dX < dY then
		return dX + dY - bit.rshift(dX,1)
	end

	return dX + dY - bit.rshift(dY,1)
end

-- returns pythagoras distance
local function distance(dX,dY)
	return math.sqrt(dX*dX + dY*dY)
end

-- returns set of visible locations by level, row, column
function visionSystem.getVisibleLocations(entity)
	local visible = {}
	local z = entity.position.z
	
	-- Todo: implement line-of-sight
	-- ...

	visible[z] = {}
    
	for y = entity.position.y - entity.vision.range, entity.position.y + entity.vision.range do
		visible[z][y] = {}

		for x = entity.position.x - entity.vision.range, entity.position.x + entity.vision.range do
			-- location is within world boundaries
			if world.locationExists(x,y,z) then
				-- distance to location is within range (using < instead of <= for nicer circle)
				if distance(x-entity.position.x,y-entity.position.y) < entity.vision.range then
					visible[z][y][x] = 1
				end
			end
		end
	end
	
	return visible
end

return visionSystem