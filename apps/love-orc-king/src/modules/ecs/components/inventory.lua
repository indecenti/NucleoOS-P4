-- entity has inventory: entity can store items

local Component = require("modules.ecs.components.component")

local Inventory = {}

function Inventory.create(entityDefaults,entityData)
    local self = Component.create({
        items = {}
    })

    self.setValues(entityDefaults,entityData)

    return self
end

return Inventory