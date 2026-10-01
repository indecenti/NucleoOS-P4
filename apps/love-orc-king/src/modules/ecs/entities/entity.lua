-- Entity is a base class for other entity classes

local componentManager = require("modules.ecs.managers.componentmanager")

local Entity = {}

function Entity.create(defaults,data)
    local self = {}
    
    for componentId,entityDefaults in pairs(defaults) do
		self[componentId] = componentManager.createComponent(componentId,entityDefaults,data[componentId])
    end

    return self
end

return Entity