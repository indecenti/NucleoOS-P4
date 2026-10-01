local Action = require("modules.ecs.actions.action")
local componentManager = require("modules.ecs.managers.componentmanager")
local entityManager = require("modules.ecs.managers.entitymanager")
local Equipment = require("modules.ecs.components.Equipment")
local grammar = require("modules.grammar")
local log = require("modules.log")

local Wield = {}

function Wield.create(data)
    local self = Action.create({
        item = nil
    })

    self.setValues(data)

    function self.perform(entity)
        local success = false

        if entityManager.entityHas(entity,{"equipment"}) then
            if entityManager.entityHas(self.item,{"item"}) and self.item.item.wieldable then
                -- look for first slot of correct type that is empty
                local emptySlotIndex = nil

                for i = 1, #entity.equipment.slots do
                    if entity.equipment.slots[i].type == self.item.item.slotType then
                        if entity.equipment.items[i] == Equipment.NULL then
                            emptySlotIndex = i

                            break
                        end
                    end
                end

                if emptySlotIndex ~= nil then
                    -- add item to equipment
                    entity.equipment.items[emptySlotIndex] = self.item

                    -- remove item from inventory
                    for i = 1, #entity.inventory.items do
                        if entity.inventory.items[i] == self.item then
                            table.remove(entity.inventory.items,i)

                            break
                        end
                    end

                    if entityManager.entityHas(self.item,{"stats"}) and entityManager.entityHas(entity,{"stats"}) then
                        -- update stats: strength
                        if entity.stats.strength ~= nil and self.item.stats.strength ~= nil then
                            entity.stats.strength = entity.stats.strength + self.item.stats.strength
                        end

                        -- update stats: damage
                        if entity.stats.damage ~= nil and self.item.stats.damage ~= nil then
                            entity.stats.damage = entity.stats.damage + self.item.stats.damage
                        end
                    end

                    log.addEntry(grammar.interpolate(grammar.STRUCT_E1_WIELD_E2,{entity,self.item}))

                    success = true
                else
                    -- consider swapping with item currently in wield slot
                    -- ...
                end
            
                if not success then
                    -- skip turn if not controlled by player
                    if not entityManager.entityHas(entity,{"input"}) then
                        success = true
                    else
                        log.addEntry("[No free slots available]")
                    end
                end
            end
        end
        
        return self.getPerformResult(success)
    end

    return self
end

return Wield