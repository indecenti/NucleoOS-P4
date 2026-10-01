local Action = require("modules.ecs.actions.action")
local entityManager = require("modules.ecs.managers.entitymanager")
local grammar = require("modules.grammar")
local log = require("modules.log")
local world = require("modules.world")

local Get = {}

function Get.create(data)
    local self = Action.create({})

    self.setValues(data)

    function self.perform(entity)
        local success = false

        if entityManager.entityHas(entity,{"inventory","position"}) then
            local item = entityManager.getEntityAtLocationHaving(entity.position.x,entity.position.y,entity.position.z,{"item"})

            if item ~= nil then
                -- remove item position component
                item.position = nil

                -- add to inventory
                table.insert(entity.inventory.items,item)

                log.addEntry(grammar.interpolate(grammar.STRUCT_E1_GET_E2,{entity,item}))

                success = true
            end

            if not success then
                -- skip turn if not controlled by player
                if not entityManager.entityHas(entity,{"input"}) then
                    success = true
                -- show message to player
                else
                    log.addEntry("There is nothing on the ground.")
                end
            end
        end
        
        return self.getPerformResult(success)
    end

    return self
end

return Get