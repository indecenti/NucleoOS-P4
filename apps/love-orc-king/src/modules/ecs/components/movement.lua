-- entity has movement: entity can move

local Component = require("modules.ecs.components.component")

local Movement = {}

function Movement.create(entityDefaults,entityData)
    local self = Component.create({})

    self.setValues(entityDefaults,entityData)

    return self
end

return Movement