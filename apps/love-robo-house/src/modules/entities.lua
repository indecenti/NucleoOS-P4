-- Robo House - modules/entities.lua
-- 2020 Foppygames

local entities = {}

local aspect = require("modules.aspect")
local floors = require("modules.floors")
local images = require("modules.images")
local ladders = require("modules.ladders")
local score = require("modules.score")
local sound = require("modules.sound")
local utils = require("modules.utils")

entities.TYPE_PLAYER = 1
entities.TYPE_ROBOT = 2
entities.TYPE_KITTEN = 3
entities.TYPE_COIN = 4

local ATTACK_TIME = 20
local ATTACK_INTERVAL_MAX = 12
local ATTACK_INTERVAL_MIN = 6
local ATTACK_INTERVAL_MARGIN = 2
local ATTACK_INTERVAL_CHANGE = 1
local MINIMUM_ATTACK_DISTANCE = 35
local LINE_MIN_LENGTH = 2
local LINE_MAX_LENGTH = 10
local LINE_MAX_OFFSET_Y = 7
local LINE_SPEED = 160
local LINE_TIME = 0.1
local LINE_COLOR = utils.getColorFromRgb(255,163,0)
local LINE_MAX_DISTANCE = 80
local MAX_BUTTON_BOUNCE_DY = 100
local CLIMB_SOUND_TIME = 0.16
local COIN_FLOAT_HEIGHT = 40
local COIN_POSITIONS = {
    {1,0.2},{1,0.5},{1,0.2},
    {2,0.2},{2,0.5},{2,0.2},
    {3,0.5}
}

local list = {}
local attackIntervalClock = ATTACK_INTERVAL_MAX - ATTACK_INTERVAL_MARGIN
local attackIntervalTime = ATTACK_INTERVAL_MAX
local lastCoinPosition = 5

local input = {
    jump = false,
    resetKeyDownNeeded = false
}

function entities.reset()
    list = {}
    attackIntervalClock = ATTACK_INTERVAL_MAX - ATTACK_INTERVAL_MARGIN
    attackIntervalTime = ATTACK_INTERVAL_MAX
end

function entities.add(type,floor,floorXFraction)
    local x, y = floors.getLocation(floor,floorXFraction)
    if type == entities.TYPE_PLAYER then
        entity = {
            x = x,
            y = y,
            w = 16,
            h = 16,
            action = {
                left = false,
                right = false,
                jump = false,
                up = false,
                down = false
            },
            move = {
                dx = 0,
                dy = 0,
                oldY = 0,
                maxDx = 90,
                accX = 1000,
                landed = true,
                floor = floor
            },
            jump = {
                power = 140
            },
            fall = {
                gravity = 5
            },
            climb = {
                climbing = false,
                floor = nil,
                maxDx = 90,
                accX = 1000, 
                maxDy = 95,
                accY = 1000,
                soundClock = 0
            },
            draw = {
                dir = "right",
                newDir = "right",
                current = nil,
                frame = 1,
                clock = 0,
                default = "landed",
                landed = {
                    left = {
                        images.get(images.IMAGE_PLAYER_WALK_LEFT_1),
                        images.get(images.IMAGE_PLAYER_WALK_LEFT_2)
                    },
                    right = {
                        images.get(images.IMAGE_PLAYER_WALK_RIGHT_1),
                        images.get(images.IMAGE_PLAYER_WALK_RIGHT_2)
                    },
                    time = 0.2
                },
                jump = {
                    left = {
                        images.get(images.IMAGE_PLAYER_JUMP_LEFT)
                    },
                    right = {
                        images.get(images.IMAGE_PLAYER_JUMP_RIGHT)
                    },
                    time = 0.2
                },
                explosion = {
                    both = {
                        images.get(images.IMAGE_PLAYER_EXPLOSION_1),
                        images.get(images.IMAGE_PLAYER_EXPLOSION_2),
                        images.get(images.IMAGE_PLAYER_EXPLOSION_3)
                    },
                    time = 0.06
                }
            },
            collide = {
                type = entities.TYPE_PLAYER,
                w = 12,
                h = 16
            },
            die = {
                dying = false,
                shake = true,
                draw = "explosion",
                clock = 0,
                time = 2
            },
            delete = {
                delete = false,
                alsoDeletePlayer = false
            }
        }
    elseif type == entities.TYPE_ROBOT then
        entity = {
            x = x,
            y = y,
            w = 18,
            h = 24,
            action = {
                left = false,
                right = false,
                jump = false,
                up = false,
                down = false
            },
            ai = {
                dir = "right",
                move = true,
                turnFloorFraction = 0.95
            },
            move = {
                dx = 0,
                dy = 0,
                oldY = 0,
                appliedDy = 0,
                maxDx = 20,
                accX = 40,
                landed = true,
                floor = floor
            },
            draw = {
                dir = "right",
                newDir = "right",
                current = nil,
                frame = 1,
                clock = 0,
                default = "landed",
                landed = {
                    left = {
                        images.get(images.IMAGE_ROBOT_WALK_LEFT_1)
                    },
                    right = {
                        images.get(images.IMAGE_ROBOT_WALK_RIGHT_1)
                    },
                    time = 0.2
                },
                attack = {
                    left = {
                        images.get(images.IMAGE_ROBOT_ATTACK_LEFT_1),
                        images.get(images.IMAGE_ROBOT_ATTACK_LEFT_2)
                    },
                    right = {
                        images.get(images.IMAGE_ROBOT_ATTACK_RIGHT_1),
                        images.get(images.IMAGE_ROBOT_ATTACK_RIGHT_2)
                    },
                    time = 0.2
                }
            },
            collide = {
                type = entities.TYPE_ROBOT,
                w = 18,
                h = 20
            },
            button = {
                h = 12
            },
            attack = {
                attacking = false,
                clock = 0,
                time = 20,
                originalDir = "right"
            }
        }
    elseif type == entities.TYPE_KITTEN then
        entity = {
            x = x,
            y = y,
            w = 10,
            h = 9,
            action = {
                left = false,
                right = false,
                jump = false,
                up = false,
                down = false
            },
            ai = {
                dir = "right",
                move = true,
                clock = 0,
                time = 0,
                minTime = 3,
                maxTime = 10
            },
            move = {
                dx = 0,
                dy = 0,
                oldY = 0,
                appliedDy = 0,
                maxDx = 5,
                accX = 10,
                landed = true,
                floor = floor
            },
            draw = {
                dir = "right",
                newDir = "right",
                current = nil,
                frame = 1,
                clock = 0,
                default = "landed",
                landed = {
                    left = {
                        images.get(images.IMAGE_KITTEN_WALK_LEFT_1),
                        images.get(images.IMAGE_KITTEN_WALK_LEFT_2)
                    },
                    right = {
                        images.get(images.IMAGE_KITTEN_WALK_RIGHT_1),
                        images.get(images.IMAGE_KITTEN_WALK_RIGHT_2)
                    },
                    time = 0.2
                },
                sit = {
                    left = {
                        images.get(images.IMAGE_KITTEN_SIT_LEFT)
                    },
                    right = {
                        images.get(images.IMAGE_KITTEN_SIT_RIGHT)
                    },
                    time = 0.2
                },
                attacked = {
                    left = {
                        images.get(images.IMAGE_KITTEN_ATTACKED_LEFT)
                    },
                    right = {
                        images.get(images.IMAGE_KITTEN_ATTACKED_RIGHT)
                    },
                    time = 0.2
                }
            },
            victim = {
                attacked = false,
                distance = nil,
                dir = nil,
                maxDx = 6,
                originalMaxDx = nil,
                lines = {},
                lineClock = 0
            },
            collide = {
                type = entities.TYPE_KITTEN,
                w = 2,
                h = 9
            },
            die = {
                dying = false,
                shake = true,
                draw = nil,
                clock = 0,
                time = 3
            },
            delete = {
                delete = false,
                alsoDeletePlayer = true
            }
        }
    elseif type == entities.TYPE_COIN then
        entity = {
            x = x,
            y = y - COIN_FLOAT_HEIGHT,
            w = 8,
            h = 7,
            ai = {},
            draw = {
                dir = "both",
                newDir = "both",
                current = nil,
                frame = 1,
                clock = 0,
                default = "coin",
                coin = {
                    both = {
                        images.get(images.IMAGE_COIN_1)
                    },
                    time = 0.2
                }
            },
            collide = {
                type = entities.TYPE_COIN,
                w = 8,
                h = 7
            },
            die = {
                dying = false,
                shake = false,
                draw = nil,
                clock = 0,
                time = 0
            },
            delete = {
                delete = false,
                alsoDeletePlayer = false
            }
        }
    end

    -- select initial image
    if entity.draw ~= nil then
        entity.draw.current = entity.draw[entity.draw.default]
    end

    -- change robot direction per floor
    if type == entities.TYPE_ROBOT and floor % 2 ~= 0 then
        entity.ai.dir = "left"
        entity.draw.dir = "left"
        entity.draw.newDir = "left"
    end

    table.insert(list,entity)
end

function entities.setInput(key)
    input[key] = true
end

local function action(entity)
    if entity.ai ~= nil then  
        entity.action.left = (entity.ai.dir == "left" and entity.ai.move)
        entity.action.right = (entity.ai.dir == "right" and entity.ai.move)
    else
        if input.resetKeyDownNeeded then
            if not love.keyboard.isDown("down") then
                input.resetKeyDownNeeded = false
            end
        end
        entity.action.left = love.keyboard.isDown("left")
        entity.action.right = love.keyboard.isDown("right")
        entity.action.up = love.keyboard.isDown("up")
        entity.action.down = love.keyboard.isDown("down") and not input.resetKeyDownNeeded
        entity.action.jump = input.jump
    end
end

local function ai(entity,dt)
    -- currently attacking
    if entity.attack ~= nil and entity.attack.attacking then
        return
    end 
    if entity.ai.turnFloorFraction ~= nil then
        local fraction = floors.getFraction(entity.x)
        if entity.ai.dir == "left" then
            if fraction <= (1 - entity.ai.turnFloorFraction) then
                entity.ai.dir = "right"
            end
        elseif entity.ai.dir == "right" then
            if fraction >= entity.ai.turnFloorFraction then
                entity.ai.dir = "left"
            end
        end
    elseif entity.victim ~= nil and entity.victim.attacked then
        if entity.ai.dir ~= entity.victim.dir then
            entity.ai.dir = entity.victim.dir
        end
        entity.ai.move = true
    elseif entity.ai.clock ~= nil then
        entity.ai.clock = entity.ai.clock + dt
        if entity.ai.clock >= entity.ai.time then
            local pick = math.random(1,2)
            if entity.ai.move then
                if entity.ai.dir == "left" then
                    if pick == 1 then
                        entity.ai.dir = "right"
                    else
                        entity.ai.move = false
                    end
                elseif entity.ai.dir == "right" then
                    if pick == 1 then
                        entity.ai.dir = "left"
                    else
                        entity.ai.move = false
                    end
                end
            else
                entity.ai.move = true
                if pick == 1 then
                    entity.ai.dir = "left"
                else
                    entity.ai.dir = "right"
                end
            end
            entity.ai.clock = 0
            entity.ai.time = math.random(entity.ai.minTime,entity.ai.maxTime)
            if not entity.ai.move then
                if entity.draw.sit ~= nil then
                    entity.draw.current = entity.draw.sit
                    entity.draw.frame = 1
                    entity.draw.clock = 0
                end
            else
                if entity.draw.landed ~= nil then
                    entity.draw.current = entity.draw.landed
                    entity.draw.frame = 1
                    entity.draw.clock = 0
                end
            end
        end
    end
end

local function getAction(entity,type)
    if entity.action ~= nil then
        return entity.action[type]
    end
    return false
end

local function loseHorMomentum(entity,dt)
    if entity.move.dx > 0 then
        entity.move.dx = entity.move.dx - entity.move.accX*dt
        if entity.move.dx < 0 then
            entity.move.dx = 0
        end
    elseif entity.move.dx < 0 then
        entity.move.dx = entity.move.dx + entity.move.accX*dt
        if entity.move.dx > 0 then
            entity.move.dx = 0
        end
    end
end

local function move(entity,dt)
    entity.move.oldY = entity.y
    local left = getAction(entity,"left")
    local right = getAction(entity,"right")
    if left then
        entity.move.dx = entity.move.dx - entity.move.accX*dt
        if entity.move.dx < -entity.move.maxDx then
            entity.move.dx = -entity.move.maxDx
        end
        if entity.draw ~= nil then
            entity.draw.newDir = "left"
        end
    elseif right then
        entity.move.dx = entity.move.dx + entity.move.accX*dt
        if entity.move.dx > entity.move.maxDx then
            entity.move.dx = entity.move.maxDx
        end
        if entity.draw ~= nil then
            entity.draw.newDir = "right"
        end
    else
        -- during climbing there is no momentum
        if entity.climb ~= nil and entity.climb.climbing then
            entity.move.dx = 0
        else
            loseHorMomentum(entity,dt)
        end
    end
    if entity.climb ~= nil and entity.climb.climbing then
        local up = getAction(entity,"up")
        local down = getAction(entity,"down")
        if up then
            entity.move.dy = entity.move.dy - entity.climb.accX*dt
            if entity.move.dy < -entity.climb.maxDy then
                entity.move.dy = -entity.climb.maxDy
            end
        elseif down then
            entity.move.dy = entity.move.dy + entity.climb.accX*dt
            if entity.move.dy > entity.climb.maxDy then
                entity.move.dy = entity.climb.maxDy
            end
        else
            -- during climbing there is no momentum
            entity.move.dy = 0
        end
    end
    entity.x = entity.x + entity.move.dx*dt
    entity.y = entity.y + entity.move.dy*dt
    if entity.x <= entity.w/2 then
        entity.x = entity.w/2

        if entity.ai ~= nil and entity.ai.dir == "left" then
            entity.ai.dir = "right"
        end
    end
    if entity.x >= aspect.GAME_WIDTH-entity.w/2 then
        entity.x = aspect.GAME_WIDTH-entity.w/2

        if entity.ai ~= nil and entity.ai.dir == "right" then
            entity.ai.dir = "left"
        end
    end
end

local function jump(entity)
    if getAction(entity,"jump") then
        if entity.move ~= nil and entity.move.landed then
            entity.move.dy = -entity.jump.power
            entity.move.landed = false

            -- switch to animation used for jumping
            if entity.draw.jump ~= nil then
                if entity.draw.current ~= entity.draw.jump then
                    entity.draw.current = entity.draw.jump
                    entity.draw.frame = 1
                    entity.draw.clock = 0
                end
            end

            -- play sound
            sound.play(sound.SOUND_JUMP)
        end
    end
end

local function fall(entity,dt)
    if entity.climb ~= nil and entity.climb.climbing then
        return
    end
    if entity.move ~= nil then
        entity.move.dy = entity.move.dy + entity.fall.gravity
    end
end

local function land(entity)
    if entity.move ~= nil then
        -- entity is moving down
        if entity.move.dy > 0 then
            local landed, floor, y = floors.land(entity.x,entity.w,entity.move.oldY,entity.y)
            if landed then
                -- entity is climbing... 
                if entity.climb ~= nil and entity.climb.climbing then
                    -- ...through floor that is not the floor the ladder is standing on
                    if entity.climb.floor ~= floor then
                        landed = false
                    -- ...on to the floor the ladder is standing on
                    else
                        entity.climb.climbing = false

                        -- entity uses keyboard for input
                        if entity.ai == nil then
                            -- key down has to be released to be registered again
                            input.resetKeyDownNeeded = true
                        end
                    end
                end
            end
            entity.move.landed = landed
            if landed then
                entity.y = y
                entity.move.dy = 0

                -- switch to animation used for walking
                if entity.draw.landed ~= nil then
                    if entity.draw.current ~= entity.draw.landed then
                        entity.draw.current = entity.draw.landed
                        entity.draw.frame = 1
                        entity.draw.clock = 0
                    end
                end
            end
        end
    end
end

local function climb(entity,dt)
    -- entity is climbing
    if entity.climb.climbing then
        local floor, grab = ladders.grab(entity.x,entity.y,entity.w,entity.h)
        entity.climb.climbing = grab
        entity.climb.floor = floor

        if entity.move ~= nil and entity.move.dy ~= 0 then
            entity.climb.soundClock = entity.climb.soundClock + dt
            if entity.climb.soundClock >= CLIMB_SOUND_TIME then
                local fromMid = (entity.y - aspect.GAME_HEIGHT/2) / (aspect.GAME_HEIGHT/2)
                local pitch = 2 - fromMid
                sound.play(sound.SOUND_CLIMB,pitch)
                entity.climb.soundClock = entity.climb.soundClock - CLIMB_SOUND_TIME
            end
        end
    -- entity is not climbing
    else
        if getAction(entity,"up") or getAction(entity,"down") then
            local floor, grab = ladders.grab(entity.x,entity.y,entity.w,entity.h)
            if grab then
                entity.move.dy = 0
                entity.climb.climbing = true
                entity.climb.floor = floor
                entity.climb.soundClock = CLIMB_SOUND_TIME
                entity.move.landed = false

                -- switch to animation used for climbing
                if entity.draw.landed ~= nil then
                    if entity.draw.current ~= entity.draw.landed then
                        entity.draw.current = entity.draw.landed
                        entity.draw.frame = 1
                        entity.draw.clock = 0
                    end
                end
            end
        end
    end
end

local function die(entity,dt)
    if entity.die.dying then
        entity.die.clock = entity.die.clock + dt
        if entity.die.clock >= entity.die.time then
            if entity.delete ~= nil then
                entity.delete.delete = true
                if entity.delete.alsoDeletePlayer then
                    for i = 1, #list do
                        local e = list[i]
                        if e.ai == nil and e.delete ~= nil then
                            e.delete.delete = true
                        end
                    end
                end
            end
        end
    end    
end

local function draw(entity,dt)
    if entity.draw.current ~= nil then
        -- animate
        entity.draw.clock = entity.draw.clock + dt
        if entity.draw.clock >= entity.draw.current.time then
            entity.draw.frame = entity.draw.frame + 1
            if entity.draw.frame > #entity.draw.current[entity.draw.dir] then
                entity.draw.frame = 1
            end
            entity.draw.clock = entity.draw.clock - entity.draw.current.time
        end

        -- change direction
        if entity.draw.newDir ~= entity.draw.dir then
            entity.draw.dir = entity.draw.newDir
        end
    end    
end

local function collidePlayerRobot(player,robot)
    local diffX = math.abs(robot.x-player.x)
    if diffX > (player.collide.w/2+robot.collide.w/2) then
        return
    end
    if player.y > robot.y+player.collide.h then
        return
    end
    if player.y < robot.y-robot.collide.h then
        return
    end
    local playerHit = false
    if robot.button ~= nil then
        if player.y <= robot.y-robot.h+robot.button.h then
            if player.move ~= nil then
                -- robot button is hit from above
                if player.move.dy > 0 then
                    player.y = robot.y-robot.h-1
                    player.move.dy = -(math.min(player.move.dy,MAX_BUTTON_BOUNCE_DY))

                    -- robot is attacking                
                    if robot.attack ~= nil and robot.attack.attacking then
                        -- stop attacking
                        robot.attack.attacking = false
                        
                        -- continue moving into original direction
                        if robot.ai ~= nil then
                            robot.ai.move = true
                            robot.ai.dir = robot.attack.originalDir
                        end

                        -- set graphics
                        if robot.draw ~= nil and robot.draw.landed ~= nil then
                            robot.draw.current = robot.draw.landed
                            robot.draw.frame = 1
                            robot.draw.clock = 0
                        end

                        -- score points
                        score.add(score.POINTS_FOR_BUTTON)

                        -- play sound
                        sound.play(sound.SOUND_BUTTON_SUCCESS)
                    else
                        -- play sound
                        sound.play(sound.SOUND_BUTTON_NEUTRAL)
                    end
                elseif player.move.dx > 0 then
                    player.x = robot.x-robot.w/2-player.w/2
                    player.move.dx = -player.move.dx*1.1
                    player.move.dy = player.move.dy*1.2
                else
                    player.x = robot.x+robot.w/2+player.w/2
                    player.move.dx = -player.move.dx*1.1
                    player.move.dy = player.move.dy*1.2
                end
            end
        else
            playerHit = true
        end
    else
        playerHit = true
    end
    if playerHit then
        if player.die ~= nil then
            player.die.dying = true

            -- remove player components
            player.move = nil
            player.jump = nil
            player.fall = nil
            player.climb = nil
            player.collide = nil

            -- remove components for non-player entities
            for i = 1, #list do
                local entity = list[i]
                if entity.ai ~= nil then
                    entity.move = nil
                    entity.collide = nil
                end
            end

            if player.draw[player.die.draw] ~= nil then
                player.draw.current = player.draw[player.die.draw]
                player.draw.dir = "both"
                player.draw.newDir = "both"
                player.draw.frame = 1
            end

            -- play sounds
            sound.play(sound.SOUND_PLAYER_HURT)
            sound.play(sound.SOUND_PLAYER_HIT)
        end
    end
end

local function collidePlayerCoin(player,coin)
    local diffX = math.abs(coin.x-player.x)
    if diffX > (player.collide.w/2+coin.collide.w/2) then
        return
    end
    if player.y > coin.y+player.collide.h then
        return
    end
    if player.y < coin.y-coin.collide.h then
        return
    end
    
    -- score points
    score.add(score.POINTS_FOR_COIN)

    -- play sound
    sound.play(sound.SOUND_COIN)

    -- remove coin components
    coin.collide = nil

    -- delete coin
    if coin.die ~= nil then
        coin.die.dying = true
    end

    -- create new coin
    local next = lastCoinPosition + math.random(1,1+#COIN_POSITIONS-1)
    if next > #COIN_POSITIONS then
        next = next - #COIN_POSITIONS
    end
    entities.add(entities.TYPE_COIN,COIN_POSITIONS[next][1],COIN_POSITIONS[next][2])
    lastCoinPosition = next
end

local function collideKittenRobot(kitten,robot)
    -- robot is not attacking
    if robot.attack == nil or not robot.attack.attacking then
        return
    end
    local diffX = math.abs(robot.x-kitten.x)
    if diffX > (kitten.collide.w/2+robot.collide.w/2) then
        return
    end
    if kitten.y > robot.y+kitten.collide.h then
        return
    end
    if kitten.y < robot.y-robot.collide.h then
        return
    end
    if kitten.die ~= nil then
        kitten.die.dying = true

        -- remove entity components
        for i = 1, #list do
            local entity = list[i]
            entity.move = nil
            entity.collide = nil
            entity.jump = nil
            entity.fall = nil
            entity.climb = nil
        end

        -- play sound
        sound.play(sound.SOUND_KITTEN_HURT)

        --[[if player.draw[player.die.draw] ~= nil then
            player.draw.current = player.draw[player.die.draw]
            player.draw.dir = "both"
            player.draw.newDir = "both"
            player.draw.frame = 1
        end]]--
    end
end

local function collide(entity,other)
    if entity.collide.type == entities.TYPE_KITTEN then
        if other.collide.type == entities.TYPE_ROBOT then
            collideKittenRobot(entity,other)
        end
    elseif entity.collide.type == entities.TYPE_PLAYER then
        if other.collide.type == entities.TYPE_ROBOT then
            collidePlayerRobot(entity,other)
        elseif other.collide.type == entities.TYPE_COIN then
            collidePlayerCoin(entity,other)
        end
    elseif entity.collide.type == entities.TYPE_ROBOT then
        if other.collide.type == entities.TYPE_PLAYER then
            collidePlayerRobot(other,entity)
        elseif other.collide.type == entities.TYPE_KITTEN then
            collideKittenRobot(other,entity)
        end
    elseif entity.collide.type == entities.TYPE_COIN then
        if other.collide.type == entities.TYPE_PLAYER then
            collidePlayerCoin(other,entity)
        end
    end
end

local function collisions()
    for i = 1, #list do
        local entity = list[i]
        -- entity can collide
        if entity.collide ~= nil then
            -- check all entities below this one in list
            for j = i+1, #list do
                local other = list[j]
                -- other can collide
                if other.collide ~= nil then
                    -- same type cannot collide
                    if entity.collide.type ~=  other.collide.type then
                        collide(entity,other)

                        -- entity no longer colliding
                        if entity.collide == nil then
                            -- skip rest of others
                            break
                        end
                    end
                end
            end
        end
    end
end

-- returns true if player deleted
local function deletion()
    local playerDeleted = false
    for i = #list, 1, -1 do
        if list[i].delete ~= nil and list[i].delete.delete then
            if list[i].ai == nil then
                playerDeleted = true
            end
            table.remove(list,i)
        end
    end
    return playerDeleted
end

local function attack(entity,dt)
    if entity.attack.attacking then
        entity.attack.clock = entity.attack.clock + dt
        if entity.attack.clock >= entity.attack.time then
            entity.attack.attacking = false

            -- continue moving into original direction
            if entity.ai ~= nil then
                entity.ai.move = true
                entity.ai.dir = entity.attack.originalDir
            end

            -- set graphics
            if entity.draw ~= nil and entity.draw.landed ~= nil then
                entity.draw.current = entity.draw.landed
                entity.draw.frame = 1
                entity.draw.clock = 0
            end
        end
    end
end

local function victim(entity,dt)
    local attacked = false
    local distance = nil
    local dir = nil

    -- look for attackers on same floor
    local floor = nil
    if entity.move ~= nil then
        floor = entity.move.floor
    end
    for i = 1, #list do
        local e = list[i]
        if e.attack ~= nil and e.attack.attacking then
            if e.move ~= nil and e.move.floor == floor then
                if e.ai ~= nil then
                    if entity.x < e.x then
                        if e.ai.dir == "left" then
                            local d = math.abs(e.x - entity.x)
                            if distance == nil or d < distance then
                                attacked = true
                                distance = d
                                dir = "right"
                            end
                        end
                    else
                        if e.ai.dir == "right" then
                            local d = math.abs(e.x - entity.x)
                            if distance == nil or d < distance then
                                attacked = true
                                distance = d
                                dir = "left"
                            end
                        end
                    end
                end
            end
        end
    end

    -- already attacked
    if entity.victim.attacked then
        if attacked then
            -- update direction and distance
            entity.victim.dir = dir
            entity.victim.distance = distance
        else
            -- return to sitting
            entity.victim.attacked = false
            entity.victim.lines = {}
            if entity.move ~= nil then
                entity.move.maxDx = entity.victim.originalMaxDx
            end
            if entity.ai ~= nil then
                if entity.victim.dir == "left" then
                    entity.ai.dir = "right"
                else
                    entity.ai.dir = "left"
                end
                entity.ai.move = false
            end

            -- set graphics
            if entity.draw ~= nil and entity.draw.sit ~= nil then
                entity.draw.current = entity.draw.sit
                if entity.ai ~= nil then
                    entity.draw.newDir = entity.ai.dir
                end
                entity.draw.frame = 1
                entity.draw.clock = 0
            end
        end
    -- not under attack
    else
        if attacked then
            entity.victim.attacked = true
            entity.victim.dir = dir
            entity.victim.distance = distance

            if entity.move ~= nil then
                entity.victim.originalMaxDx = entity.move.maxDx
                entity.move.maxDx = entity.victim.maxDx
            end

            -- set graphics
            if entity.draw ~= nil and entity.draw.attacked ~= nil then
                entity.draw.current = entity.draw.attacked
                entity.draw.frame = 1
                entity.draw.clock = 0
            end
        end
    end

    if entity.victim.attacked then
        entity.victim.lineClock = entity.victim.lineClock + dt
        if entity.victim.lineClock >= LINE_TIME then
            local dx = 1
            if entity.victim.dir == "left" then
                dx = -1
            end
            table.insert(entity.victim.lines,{
                x = entity.x + dx * entity.w/2,
                y = entity.y - math.random(0,LINE_MAX_OFFSET_Y),
                dx = dx,
                targetX = entity.x + dx * (math.min(entity.victim.distance*0.8,LINE_MAX_DISTANCE)),
                length = math.random(LINE_MIN_LENGTH,LINE_MAX_LENGTH)
            })
            entity.victim.lineClock = 0
        end

        for i = #entity.victim.lines, 1, -1 do
            local line = entity.victim.lines[i]
            line.x = line.x + line.dx * dt * LINE_SPEED
            if line.dx < 0 then
                if line.x <= line.targetX then
                    table.remove(entity.victim.lines,i)
                end
            else
                if line.x >= line.targetX then
                    table.remove(entity.victim.lines,i)
                end
            end
        end
    end
end

local function getDistanceToNearestKitten(entity)
    local distance = nil
    local dir = nil
    local floor = nil
    if entity.move ~= nil then
        floor = entity.move.floor
    end
    for i = 1, #list do
        local e = list[i]
        if e.victim ~= nil then
            if e.move ~= nil and e.move.landed and e.move.floor == floor then
                local d = math.abs(entity.x - e.x)
                if distance == nil or d < distance then
                    distance = d
                    if e.x < entity.x then
                        dir = "left"
                    else
                        dir = "right"
                    end
                end
            end
        end
    end
    return distance, dir
end

local function updateAttackInterval(dt)
    attackIntervalClock = attackIntervalClock + dt

    if attackIntervalClock >= attackIntervalTime then
        -- collect indexes to robots that could attack
        local army = {}
        for i = 1, #list do
            local entity = list[i]
            if entity.ai ~= nil and entity.attack ~= nil and not entity.attack.attacking then
                local distance, dir = getDistanceToNearestKitten(entity)
                -- robot is not too close
                if distance ~= nil and distance >= MINIMUM_ATTACK_DISTANCE then
                    -- insert sorted from big to small distance
                    local index = 1
                    while index <= #army and distance < army[index].distance do
                        index = index + 1
                    end
                    table.insert(army,index,{
                        index = i,
                        distance = distance,
                        direction = dir
                    })
                end
            end
        end

        -- pick random attacker from the 3 that are furthest away from a kitten
        local data = nil
        local entity = nil
        if #army > 0 then
            local pickFrom = math.min(3,#army)
            data = army[math.random(1,pickFrom)]
            entity = list[data.index]
        end

        if entity ~= nil then
            entity.attack.attacking = true
            entity.attack.clock = 0

            -- stop moving
            entity.ai.move = false

            if data.direction ~= nil then
                entity.attack.originalDir = entity.ai.dir
                entity.ai.dir = data.direction
            end

            -- set graphics
            if entity.draw ~= nil and entity.draw.attack ~= nil then
                entity.draw.current = entity.draw.attack
                entity.draw.newDir = entity.ai.dir
                entity.draw.frame = 1
                entity.draw.clock = 0
            end

            -- play sound
            sound.play(sound.SOUND_ROBOT_ATTACK)
        end

        attackIntervalTime = attackIntervalTime - ATTACK_INTERVAL_CHANGE
        if attackIntervalTime < ATTACK_INTERVAL_MIN then
            attackIntervalTime = ATTACK_INTERVAL_MIN
        end

        attackIntervalClock = math.random(0,ATTACK_INTERVAL_MARGIN)
    end
end

-- returns whether game continues
function entities.update(dt)
    for i = 1, #list do
        local entity = list[i]
        
        if entity.ai ~= nil then
            ai(entity,dt)
        end
        if entity.action ~= nil then
            action(entity)
        end
        if entity.jump ~= nil then
            jump(entity)
        end
        if entity.fall ~= nil then
            fall(entity,dt)
        end
        if entity.climb ~= nil then
            climb(entity,dt)
        end
        if entity.move ~= nil then
            move(entity,dt)
            land(entity)
        end
        if entity.attack ~= nil then
            attack(entity,dt)
        end
        if entity.victim ~= nil then
            victim(entity,dt)
        end
        if entity.die ~= nil then
            die(entity,dt)
        end
        if entity.draw ~= nil then
            draw(entity,dt)
        end   
    end

    collisions()
    local continue = not deletion()

    updateAttackInterval(dt)

    -- reset input
    input = {
        jump = false
    }

    return continue
end

function entities.draw()
    love.graphics.setColor(1,1,1)
    for i = 1, #list do
        local entity = list[i]
        if entity.draw ~= nil then
            if entity.draw.current ~= nil then
                local sprite = entity.draw.current[entity.draw.dir][entity.draw.frame]
                local x = entity.x-sprite.w/2
                local y = entity.y-sprite.h
                if entity.die ~= nil and entity.die.dying and entity.die.shake then
                    x = x + math.random(-1,1)
                    y = y + math.random(-1,1)
                end
                love.graphics.draw(sprite.image,x,y)
            end
            if entity.victim ~= nil and entity.victim.attacked then
                love.graphics.setColor(LINE_COLOR)
                for i = 1, #entity.victim.lines do
                    local line = entity.victim.lines[i]
                    love.graphics.line(line.x-line.length/2,line.y,line.x+line.length/2,line.y)
                end
                love.graphics.setColor(1,1,1)
            end
        end
    end
end

return entities