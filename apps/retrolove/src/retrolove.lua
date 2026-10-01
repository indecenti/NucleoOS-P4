-- retrolove.lua
-- Copyright (c) 2018 Jon Thysell

local retrolove = {}

function retrolove.load()
    local menu = require "menu"
    retrolove.mainMenu = menu.Menu:new({})

    -- load games
    local pong = require "pong.pong"
    retrolove.mainMenu:addGame(pong.Pong:new({}))

    local breakout = require "breakout.breakout"
    retrolove.mainMenu:addGame(breakout.Breakout:new({}))

    local asteroids = require "asteroids.asteroids"
    retrolove.mainMenu:addGame(asteroids.Asteroids:new({}))

    local splash = require "splash"
    retrolove.currentGame = splash.Splash:new({nextGame = retrolove.mainMenu})

    retrolove.currentGame:init()
end

function retrolove.update(dt)
    retrolove.currentGame:update(dt)
end

function retrolove.draw()
    retrolove.currentGame:draw()
end

function retrolove.keyreleased(key)
    retrolove.currentGame:keyReleased(key)
end

function retrolove.touchreleased(id, x, y, dx, dy, pressure)
    retrolove.currentGame:touchReleased(id, x, y, dx, dy, pressure)
end

function retrolove.resize()
    retrolove.currentGame:resize()
end

function retrolove.activate(isActive)
    if not isActive then
        if retrolove.currentGame.saveState then retrolove.currentGame:saveState() end
    end
end

function retrolove.switchGame(game)
    if game then
        print("Switching to "..game.id)
        local debugMode = retrolove.currentGame.debugMode
        game.debugMode = debugMode

        retrolove.currentGame = game
        retrolove.currentGame:init()
    else
        print("Exiting...")
        love.event.quit()
    end
end

return retrolove