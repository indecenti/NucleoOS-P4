local entityManager = require("modules.ecs.managers.entitymanager")

local statsSystem = {}

function statsSystem.update()
	local entities = entityManager.getEntitiesHaving({"stats"})

	for i = 1, #entities do
		-- entity has health stat
		if entities[i].stats.health ~= nil then
			if entities[i].stats.health < entities[i].stats.healthMax then
				if entities[i].stats.rest then
					entities[i].stats.health = entities[i].stats.health + entities[i].stats.healthIncrement
                    
					if entities[i].stats.health > entities[i].stats.healthMax then
						entities[i].stats.health = entities[i].stats.healthMax
					end	
				end
			end
		end

		-- reset rest
		if entities[i].stats.rest then
			entities[i].stats.rest = false
		end
	end
end

return statsSystem