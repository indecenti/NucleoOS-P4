-- Action is a base class for other action classes

local Action = {}

function Action.create(defaults)
    local self = defaults
    
    function self.setValues(data)
        if data ~= nil then
            for key,value in pairs(data) do
				self[key] = value 
			end
        end
    end

    function self.getPerformResult(success,newActionId,newActionData)
        local result = {
            success = success,
            newAction = nil
        }  

        if newActionId ~= nil then
            result.newAction = {
                id = newActionId
            }

            if newActionData ~= nil then
                result.newAction.data = newActionData
            end
        end

        return result
    end

    function self.perform(entity)
        print "Error: action.perform() not implemented for derived class"
        
        return self.getPerformResult(false)
    end

    return self
end

return Action