-- Robo House - main.lua
-- 2020 Foppygames

local aspect = require("modules.aspect")
local entities = require("modules.entities")
local floors = require("modules.floors")
local images = require("modules.images")
local ladders = require("modules.ladders")
local utils = require("modules.utils")
local score = require("modules.score")
local sound = require("modules.sound")

local GAME_TITLE = "Robo House"
local BACKGROUND_COLOR = utils.getColorFromRgb(126,37,83)
local FULL_SCREEN = true
local MUSIC_VOLUME_TITLE = 0.1
local MUSIC_VOLUME_ACTION = 0.05
local MUSIC_PATH = "/music/POL-chubby-cat-long.wav"

local STATE_TITLE = 1
local STATE_ACTION = 2

local state
local titleAngle = 0
local music = nil
local musicEnabled = true

function love.load()
    math.randomseed(os.time())

    love.window.setTitle(GAME_TITLE)
	love.graphics.setDefaultFilter("nearest","nearest")
	love.graphics.setLineStyle("rough")
    love.graphics.setFont(love.graphics.newFont("Retroville_NC.ttf",10))
    love.graphics.setBackgroundColor(BACKGROUND_COLOR)
    
    -- check if music file exist
    local info = love.filesystem.getInfo(MUSIC_PATH)
    if info ~= nil then
        music = love.audio.newSource(MUSIC_PATH,"static")
        music:setLooping(true)
    else
        musicEnabled = false
    end

    aspect.init(FULL_SCREEN)
    images.init()
    ladders.init()
    sound.init()
    switchToState(STATE_TITLE)
end

function switchToState(new)
    if state == STATE_ACTION then
        entities.reset()
    end
    state = new
    if state == STATE_TITLE then
        titleAngle = 0

        if musicEnabled then
            music:setVolume(MUSIC_VOLUME_TITLE)
            love.audio.play(music)
        end
    elseif state == STATE_ACTION then
        if musicEnabled then
            music:setVolume(MUSIC_VOLUME_ACTION)
        end

        score.reset()
        
        entities.add(entities.TYPE_PLAYER,2,0.2)

        entities.add(entities.TYPE_ROBOT,3,0.3)
        entities.add(entities.TYPE_ROBOT,3,0.7)
        entities.add(entities.TYPE_ROBOT,2,0.3)
        entities.add(entities.TYPE_ROBOT,2,0.7)
        entities.add(entities.TYPE_ROBOT,1,0.3)
        entities.add(entities.TYPE_ROBOT,1,0.7)

        entities.add(entities.TYPE_KITTEN,3,0.1)
        entities.add(entities.TYPE_KITTEN,3,0.9)
        entities.add(entities.TYPE_KITTEN,2,0.1)
        entities.add(entities.TYPE_KITTEN,2,0.9)
        entities.add(entities.TYPE_KITTEN,1,0.1)
        entities.add(entities.TYPE_KITTEN,1,0.9)

        entities.add(entities.TYPE_COIN,2,0.5)
    end
end

function love.update(dt)
    if state == STATE_TITLE then
        titleAngle = titleAngle + 12 * dt
        if titleAngle >= 360 then
            titleAngle = titleAngle - 360
        end
    end
    if state == STATE_ACTION then
        if not entities.update(dt) then
            switchToState(STATE_TITLE)
        end
    end
end

function love.keypressed(key)
    if state == STATE_TITLE then
        if key == "escape" then
            love.event.quit()
        elseif key == "space" then
            switchToState(STATE_ACTION)
        end
    elseif state == STATE_ACTION then
        if key == "escape" then
            switchToState(STATE_TITLE)
        elseif key == "space" then
            entities.setInput("jump")
        end
    end
    if key == "m" then
        if musicEnabled then
            love.audio.stop(music)
            musicEnabled = false
        elseif music ~= nil then
            love.audio.play(music)
            musicEnabled = true
        end
    end
end

function drawTitleScreen()
    love.graphics.push()
    love.graphics.scale(2,2)
    love.graphics.setColor(utils.getColorFromRgb(255,163,0))
    for i = 1, string.len(GAME_TITLE) do
        local angle = titleAngle + i * 12
        if angle >= 360 then
            angle = angle - 360
        end
        love.graphics.print(string.sub(GAME_TITLE,i,i),30 + i * 10 - math.cos(math.rad(angle)*5),10-math.sin(math.rad(angle)*20))
	end
    love.graphics.pop()
    love.graphics.setColor(utils.getColorFromRgb(255,236,39))
    love.graphics.printf("For the #Redefine2021 gamejam!",0,60,aspect.GAME_WIDTH,"center")	
    
    love.graphics.setColor(utils.getColorFromRgb(41,173,255))
    love.graphics.printf("Controls: arrow keys and space",0,80,aspect.GAME_WIDTH,"center")	
    love.graphics.printf("PRESS SPACE TO START",0,100,aspect.GAME_WIDTH,"center")	
    love.graphics.setColor(utils.getColorFromRgb(131,118,156))
    love.graphics.printf("ESC to quit / M to toggle music",0,120,aspect.GAME_WIDTH,"center")	

    love.graphics.setColor(utils.getColorFromRgb(255,241,232))
    local writtenByY = aspect.GAME_HEIGHT-60
    if music == nil then
        writtenByY = aspect.GAME_HEIGHT-40
    end
    love.graphics.printf("Written by Robbert Prins",0,writtenByY,aspect.GAME_WIDTH,"center")	
    if music ~= nil then
        love.graphics.printf("Music from PlayOnLoop.com",0,aspect.GAME_HEIGHT-40,aspect.GAME_WIDTH,"center")	
    end
    love.graphics.setColor(utils.getColorFromRgb(255,236,39))
    love.graphics.printf("Foppygames 2020",0,aspect.GAME_HEIGHT-20,aspect.GAME_WIDTH,"center")	
end

function love.draw()
    aspect.apply()
    love.graphics.clear(BACKGROUND_COLOR)

    if state == STATE_TITLE then
        drawTitleScreen()
        score.draw()
    end

    if state == STATE_ACTION then
        score.draw()
        floors.draw()
        ladders.draw()
        entities.draw()
    end

    aspect.letterbox()
end