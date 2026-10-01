-- entity has name: entity name can be used in messages involving entity

local Component = require("modules.ecs.components.component")

local Name = {}

function Name.create(entityDefaults,entityData)
    local self = Component.create({
        genericName = "unknown",
        specificName = nil
    })

    self.setValues(entityDefaults,entityData)

    return self
end

return Name