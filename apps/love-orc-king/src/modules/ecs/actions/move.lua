local Action = require("modules.ecs.actions.action")
local entityManager = require("modules.ecs.managers.entitymanager")
local log = require("modules.log")
local world = require("modules.world")

local Move = {}

function Move.create(data)
    local self = Action.create({
        dX = 0,
        dY = 0
    })

    self.setValues(data)

    function self.perform(entity)
        local success = false
        local newActionId = nil
        local newActionData = nil

        if entityManager.entityHas(entity,{"movement","position"}) then
            -- target location is not entity location
            if (self.dX ~= 0) or (self.dY ~= 0) then
				-- check new location
				local newX = entity.position.x + self.dX
				local newY = entity.position.y + self.dY

                -- todo: for diagonals, also check two tiles entity partly traverses
                -- ...

                local noWall = world.locationIsPassable(newX,newY,entity.position.z)
                local target = entityManager.getEntityAtLocationNotHaving(newX,newY,entity.position.z,{"item"})

                if noWall and (target == nil) then
                    entity.position.x = newX
					entity.position.y = newY
                    success = true
                end

                if not success then
                    -- skip turn if not controlled by player
                    if not entityManager.entityHas(entity,{"input"}) then
                        success = true
                    -- show message to player
                    else
                        if not noWall then
                            log.addEntry("Your path is blocked by a wall.")
                        elseif target ~= nil then
                            if entityManager.entityHas(entity,{"attack"}) then
                                newActionId = "attack"
                                newActionData = {
                                    x = newX,
                                    y = newY,
                                    z = entity.position.z
                                }
                            else
                                log.addEntry("Your path is blocked by [...]")
                            end
                        end
                    end
                end
            -- target location is entity location
            else
                -- skip turn
                success = true
			end
        end
        
        return self.getPerformResult(success,newActionId,newActionData)
    end

    return self
end

return Move