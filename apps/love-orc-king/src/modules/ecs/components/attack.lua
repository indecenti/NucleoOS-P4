-- entity has attack: entity can attack other entities

local Component = require("modules.ecs.components.component")

local Attack = {}

function Attack.create(entityDefaults,entityData)
    local self = Component.create({})

    self.setValues(entityDefaults,entityData)

    return self
end

return Attack