local Action = require("modules.ecs.actions.action")
local dice = require("modules.dice")
local entityManager = require("modules.ecs.managers.entitymanager")
local grammar = require("modules.grammar")
local log = require("modules.log")
local world = require("modules.world")

local Attack = {}

function Attack.create(data)
    local self = Action.create({
        x = 0,
        y = 0,
        z = 0
    })

    self.setValues(data)

    function self.perform(entity)
        local success = false

        if entityManager.entityHas(entity,{"attack","position"}) then
            local target = entityManager.getEntityAtLocationNotHaving(self.x,self.y,self.z,{"item"})

            if target ~= nil then
                local logEntry = grammar.interpolate(grammar.STRUCT_E1_ATTACK_E2,{entity,target})
    
                -- hit or miss is decided by dexterity
                local hit = false
                local dexterity = 0
                local targetDexterity = 0

                if entityManager.entityHas(entity,{"stats"}) then
                    if entity.stats.dexterity ~= nil then
                        dexterity = entity.stats.dexterity
                    end
                end

                if entityManager.entityHas(target,{"stats"}) then
                    if target.stats.dexterity ~= nil then
                        targetDexterity = target.stats.dexterity
                    end
                end

                local result = dice.roll(dexterity,targetDexterity)

                if result == 1 then
                    hit = true
                else
                    logEntry = logEntry.." "..grammar.interpolate(grammar.STRUCT_E1_MISS,{entity})
                end

                if hit then
                    -- damage is strength plus damage
                    local damage = 0

                    if entityManager.entityHas(entity,{"stats"}) then
                        if entity.stats.strength ~= nil then
                            damage = damage + entity.stats.strength
                        end

                        if entity.stats.damage ~= nil then
                            damage = damage + entity.stats.damage
                        end
                    end

                    if entityManager.entityHas(target,{"stats"}) then
                        if target.stats.health ~= nil then
                            target.stats.health = target.stats.health - damage
                            
                            -- target is killed
                            if target.stats.health <= 0 then
                                logEntry = logEntry.." "..grammar.interpolate(grammar.STRUCT_E1_DIE,{target})
                                entityManager.deleteEntity(target)
                            -- target has health remaining
                            else
                                logEntry = logEntry.." "..grammar.interpolate(grammar.STRUCT_E1_LOSE_P2_HEALTH,{target,damage})
                            end
                        end
                    end
                end

                log.addEntry(logEntry)
            end

            success = true
        end
        
        return self.getPerformResult(success)
    end

    return self
end

return Attack