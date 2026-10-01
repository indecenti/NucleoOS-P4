-- entity has appearance: entity can be rendered

local Component = require("modules.ecs.components.component")

local Appearance = {}

function Appearance.create(entityDefaults,entityData)
    local self = Component.create({
        imageId = nil,
        rendered = false
    })

    self.setValues(entityDefaults,entityData)

    return self
end

return Appearance