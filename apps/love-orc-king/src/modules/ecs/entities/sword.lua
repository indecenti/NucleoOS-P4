local Entity = require("modules.ecs.entities.entity")
local Equipment = require("modules.ecs.components.equipment")

local Sword = {}

function Sword.create(data)
    return Entity.create({
        appearance = {
			imageId = "sword"
		},
		description = {
			text = "The sword is rusty and blunt but can still be used as a weapon."	
		},
		item = {
			wieldable = true,
			slotType = Equipment.SLOT_TYPE_MAIN_HAND
		},
		name = {
			genericName = "rusty short sword"
		},
		position = {},
		stats = {
			strength = 1,
			damage = 10
		}
	},data)
end

return Sword