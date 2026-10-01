-- entity has description: entity can be described to player

local Component = require("modules.ecs.components.component")

local Description = {}

function Description.create(entityDefaults,entityData)
    local self = Component.create({
        text = ""
    })

    self.setValues(entityDefaults,entityData)

    return self
end

return Description