-- entity has stats: stats influence actions performed by and on entity

local Component = require("modules.ecs.components.component")

local Stats = {}

function Stats.create(entityDefaults,entityData)
    local self = Component.create({
        health = nil,
        healthMax = 0,
        rest = false,
        healthIncrement = 0,
        strength = nil,
        damage = nil,
        dexterity = nil
    })

    self.setValues(entityDefaults,entityData)

    return self
end

return Stats