-- game.lua
-- Copyright (c) 2018 Jon Thysell

local Game = {
    id = "",
    title = "",
    active = false,
    debugMode = false,
    resWidth = 320,
    resHeight = 240,
    margin = 20,
}

function Game:new(o)
    local o = o or {}
    setmetatable(o, self)
    self.__index = self
    return o
end

function Game:init()
    self.canvas = love.graphics.newCanvas(self.resWidth, self.resHeight)

    self.canvas:setFilter("nearest", "nearest", 0)
    love.graphics.setFont(love.graphics.newFont(self.margin * .7))

    self:resize()

    if self.initGame then self:initGame() end
    if self.loadState then self:loadState() end

    self.active = true
end

function Game:getInput()
    return nil
end

function Game:keyReleased(key)
    if self.active and self.keyReleasedGame then
        self:keyReleasedGame(key)
    end
end

function Game:touchReleased(id, x, y, dx, dy, pressure)
    if self.active and self.touchReleasedGame then
        self:touchReleasedGame(id, x, y, dx, dy, pressure)
    end
end

function Game:update(dt)
    if self.active then
        self:updateGame(dt, self:getInput())
    end
end

function Game:updateGame(dt, input)

end

function Game:draw()
    if self.active then
        love.graphics.setCanvas(self.canvas)
        love.graphics.clear({0, 0, 0, 255})
        
        self:drawGame()

        love.graphics.setCanvas()

        if self.debugMode then
            love.graphics.clear({255, 0, 0, 255})
        else
            love.graphics.clear({0, 0, 0, 255})
        end

        love.graphics.draw(self.canvas, self.canvasOriginX, self.canvasOriginY, 0, self.scale, self.scale)

        if self.drawOverlay then self:drawOverlay() end
    end
end

function Game:drawGame(dt, input)
    local font = love.graphics.getFont()

    love.graphics.setColor({255, 255, 255, 255})
    local centerText = tostring(self.title)
    love.graphics.print(centerText, (self.resWidth - font:getWidth(centerText)) / 2, (self.resHeight - font:getHeight(centerText)) / 2)

    -- Draw margins
    love.graphics.setColor({0, 0, 0, 255})
    love.graphics.rectangle("fill", 0, 0, self.margin, self.resHeight - self.margin)
    love.graphics.rectangle("fill", 0, self.resHeight - self.margin, self.resWidth - self.margin, self.resHeight)
    love.graphics.rectangle("fill", self.margin, 0, self.resWidth, self.margin)
    love.graphics.rectangle("fill", self.resWidth - self.margin, self.margin, self.resWidth, self.resHeight)
    love.graphics.setColor({255, 255, 255, 255})
    love.graphics.rectangle("line", self.margin - 1 , self.margin - 1, self.resWidth - 2 * (self.margin - 1), self.resHeight - (2 * self.margin - 1))

    -- Draw Debug Info
    if self.debugMode then
        local fpsText = "FPS: "..tostring(love.timer.getFPS())
        love.graphics.print(fpsText, self.resWidth - (font:getWidth(fpsText) + self.margin / 4), self.resHeight - self.margin + ((self.margin - font:getHeight(fpsText)) / 2))
    end
end

function Game:resize()
    self.screenWidth = love.graphics.getWidth()
    self.screenHeight = love.graphics.getHeight()

    self.scale = math.floor(math.min(self.screenWidth / self.resWidth, self.screenHeight / self.resHeight))
    self.canvasOriginX = math.floor(0.5 + ((self.screenWidth - self.resWidth * self.scale) / 2))
    self.canvasOriginY = math.floor(0.5 + ((self.screenHeight - self.resHeight * self.scale) / 2))
end

function Game:exit()
    if self.saveState then self:saveState() end

    local switchToGame = self:exitGame()

    self.active = false

    local retrolove = require "retrolove"
    retrolove.switchGame(switchToGame)
end

function Game:exitGame()
    local retrolove = require "retrolove"
    return retrolove.mainMenu
end

return {
    Game = Game
}