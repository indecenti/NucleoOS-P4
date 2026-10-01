-- entity has vision: entity can see surroundings within range

local Component = require("modules.ecs.components.component")

local Vision = {}

function Vision.create(entityDefaults,entityData)
    local self = Component.create({
        range = 9,
        visible = {}
    })

    self.setValues(entityDefaults,entityData)

    return self
end

return Vision