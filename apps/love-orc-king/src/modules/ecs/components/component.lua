-- Component is a base class for other component classes

local Component = {}

function Component.create(systemDefaults)
    -- set system defaults
    local self = systemDefaults
    
    function self.setValues(entityDefaults,entityData)
        if entityDefaults ~= nil then
            -- set default values defining entity type
			for key,value in pairs(entityDefaults) do
				self[key] = value 
			end
        end

        if entityData ~= nil then
            -- set possibly modified values for entity instance
			for key,value in pairs(entityData) do
				self[key] = value 
			end
        end
    end

    return self
end

return Component