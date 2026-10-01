-- entity has position: entity is located somewhere in the world

local Component = require("modules.ecs.components.component")

local Position = {}

function Position.create(entityDefaults,entityData)
    local self = Component.create({
        x = 0,
        y = 0,
        z = 0
    })

    self.setValues(entityDefaults,entityData)

    return self
end

return Position