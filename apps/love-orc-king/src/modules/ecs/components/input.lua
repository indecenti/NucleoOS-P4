-- entity has input: input devices determine entity actions

local Component = require("modules.ecs.components.component")

local Input = {}

function Input.create(entityDefaults,entityData)
    local self = Component.create({})

    self.setValues(entityDefaults,entityData)

    return self
end

return Input