-- entity has equipment: entity can wear/wield items

local Component = require("modules.ecs.components.component")

local Equipment = {}

-- value representing empty slot
Equipment.NULL = "null"

-- constants for slot types
Equipment.SLOT_TYPE_MAIN_HAND = "main_hand"
Equipment.SLOT_TYPE_OFF_HAND = "off_hand"
Equipment.SLOT_TYPE_BODY = "body"
Equipment.SLOT_TYPE_HEAD = "head"
Equipment.SLOT_TYPE_BACK = "back"
Equipment.SLOT_TYPE_FEET = "feet"
Equipment.SLOT_TYPE_HANDS = "hands"
Equipment.SLOT_TYPE_FINGER = "finger"
Equipment.SLOT_TYPE_NECK = "neck"

function Equipment.create(entityDefaults,entityData)
    local self = Component.create({
        -- Note: Slots defines the available slots. Each slot is defined by a table containing name and 
        -- type. The location of an item in the items table corresponds to the slot at the same index in 
        -- the slots table.
        items = {},
        slots = {}
    })

    self.setValues(entityDefaults,entityData)

    return self
end

return Equipment