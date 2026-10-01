local Action = require("modules.ecs.actions.action")
local componentManager = require("modules.ecs.managers.componentmanager")
local entityManager = require("modules.ecs.managers.entitymanager")
local Equipment = require("modules.ecs.components.Equipment")
local grammar = require("modules.grammar")
local log = require("modules.log")
local world = require("modules.world")

local Drop = {}

function Drop.create(data)
    local self = Action.create({
        item = nil
    })

    self.setValues(data)

    function self.perform(entity)
        local success = false

        if entityManager.entityHas(entity,{"inventory","position"}) then
            -- intended drop location is entity location
            local x = entity.position.x
            local y = entity.position.y
            local z = entity.position.z

            local emptyFloor = true
            local otherItem = entityManager.getEntityAtLocationHaving(x,y,z,{"item"})

            -- other item already at location
            if otherItem ~= nil then
                emptyFloor = false
            end

            if emptyFloor then
                -- todo: refactor entity definition so that entity component defaults can be 
                -- retrieved separately and applied to component re-creation? (currently set to nil)
                -- ...

                -- create item position component
                self.item.position = componentManager.createComponent("position",nil,{
                    x = entity.position.x,
                    y = entity.position.y,
                    z = entity.position.z
                })

                -- remove from inventory...
                local removed = false

                for i = 1, #entity.inventory.items do
                    if entity.inventory.items[i] == self.item then
                        table.remove(entity.inventory.items,i)

                        removed = true

                        break
                    end
                end

                -- or remove from equipment
                if not removed then
                    for i = 1, #entity.equipment.items do
                        if entity.equipment.items[i] == self.item then
                            entity.equipment.items[i] = Equipment.NULL

                            if entityManager.entityHas(self.item,{"stats"}) and entityManager.entityHas(entity,{"stats"}) then
                                -- update stats: strength
                                if entity.stats.strength ~= nil and self.item.stats.strength ~= nil then
                                    entity.stats.strength = entity.stats.strength - self.item.stats.strength
                                end

                                -- update stats: attack
                                if entity.stats.attack ~= nil and self.item.stats.attack ~= nil then
                                    entity.stats.attack = entity.stats.attack - self.item.stats.attack
                                end
                            end
        
                            break
                        end
                    end
                end
            
                log.addEntry(grammar.interpolate(grammar.STRUCT_E1_DROP_E2,{entity,self.item}))

                success = true
            end

            if not success then
                -- skip turn if not controlled by player
                if not entityManager.entityHas(entity,{"input"}) then
                    success = true
                -- show message to player
                elseif not emptyFloor then
                    log.addEntry(grammar.interpolate(grammar.STRUCT_E1_CANNOT_DROP_E2_HERE,{entity,self.item}))
                end
            end
        end
        
        return self.getPerformResult(success)
    end

    return self
end

return Drop