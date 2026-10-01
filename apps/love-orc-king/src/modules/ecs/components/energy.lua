-- entity has energy: entity can take part in the turn system

local Component = require("modules.ecs.components.component")

local Energy = {}

function Energy.create(entityDefaults,entityData)
    local self = Component.create({
        level = 0,
        increment = 0,
        turn = false
    })

    self.setValues(entityDefaults,entityData)

    return self
end

return Energy