local Entity = require("modules.ecs.entities.entity")

local Bat = {}

function Bat.create(data)
    return Entity.create({
        action = {},
		ai = {},
		appearance = {
			imageId = "bat"
		},
		attack = {},
		energy = {
			increment = 2
		},
		movement = {},
		name = {
			genericName = "bat"
		},
		position = {},
		stats = {
			health = 5,
			healthMax = 5,
			strength = 1,
			damage = 1,
			dexterity = 4
		},
		vision = {}
    },data)
end

return Bat