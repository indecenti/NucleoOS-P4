-- entity has ai: ai routines determine entity actions

local Component = require("modules.ecs.components.component")

local Ai = {}

function Ai.create(entityDefaults,entityData)
    local self = Component.create({})

    self.setValues(entityDefaults,entityData)

    return self
end

return Ai