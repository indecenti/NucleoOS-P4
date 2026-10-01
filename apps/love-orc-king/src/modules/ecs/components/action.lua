-- entity has action: entity has the potential to perform actions

local Component = require("modules.ecs.components.component")

local Action = {}

function Action.create(entityDefaults,entityData)
    local self = Component.create({})

    self.setValues(entityDefaults,entityData)

    return self
end

return Action