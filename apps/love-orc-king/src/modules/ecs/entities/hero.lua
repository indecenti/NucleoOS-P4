local Entity = require("modules.ecs.entities.entity")
local Equipment = require("modules.ecs.components.equipment")

local Hero = {}

function Hero.create(data)
    return Entity.create({
        action = {},
		appearance = {
			imageId = "human"
		},
		attack = {},
		camera = {},
		energy = {
			increment = 1
		},
		equipment = {
			items = {
				Equipment.NULL,Equipment.NULL,Equipment.NULL,Equipment.NULL,Equipment.NULL,
				Equipment.NULL,Equipment.NULL,Equipment.NULL,Equipment.NULL,Equipment.NULL
			},
			slots = {
				{
					name = "main hand",
					type = Equipment.SLOT_TYPE_MAIN_HAND
				},
				{
					name = "off hand",
					type = Equipment.SLOT_TYPE_OFF_HAND
				},
				{
					name = "head",
					type = Equipment.SLOT_TYPE_HEAD
				},
				{
					name = "neck",
					type = Equipment.SLOT_TYPE_NECK
				},
				{
					name = "body",
					type = Equipment.SLOT_TYPE_BODY
				},
				{
					name = "back",
					type = Equipment.SLOT_TYPE_BACK
				},
				{
					name = "hands",
					type = Equipment.SLOT_TYPE_HANDS
				},
				{
					name = "ring 1",
					type = Equipment.SLOT_TYPE_FINGER
				},
				{
					name = "ring 2",
					type = Equipment.SLOT_TYPE_FINGER
				},
				{
					name = "feet",
					type = Equipment.SLOT_TYPE_FEET
				}
			}
		},
		input = {},
		inventory = {},
		movement = {},
		position = {},
		stats = {
			health = 50,
			healthMax = 50,
			strength = 4,
			damage = 4,
			dexterity = 5
		},
		vision = {}
    },data)
end

return Hero