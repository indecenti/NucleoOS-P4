local entityManager = require("modules.ecs.managers.entitymanager")

local energySystem = {}

energySystem.ENERGY_FOR_ACTION = 1

function energySystem.update()
	local entities = entityManager.getEntitiesHaving({"energy"})
    
	for i = 1, #entities do
		if entities[i].energy.level < energySystem.ENERGY_FOR_ACTION then
			entities[i].energy.level = entities[i].energy.level + entities[i].energy.increment
			-- Note: level can be more than required for action and is not reset to 0;
			-- otherwise small differences in increment between entities would be removed
		end
	end
end

return energySystem