-- loading.lua
-- Copyright (c) 2018 Jon Thysell

local game = require "game"

local Splash = game.Game:new({
    id = "splash",
    title = "RetroLove 1.0.0",
    caption = "Made with LÖVE",
    timeRemaining = 2.0,
})

function Splash:initGame()
    self.logo = love.graphics.newImage("logo.png")
end

function Splash:updateGame(dt, input)
    self.timeRemaining = self.timeRemaining - dt
    if self.timeRemaining <= 0 then
        self:exit()
    end
end

function Splash:drawGame(dt, input)
    local font = love.graphics.getFont()

    love.graphics.setColor({255, 255, 255, 255})

    local logoImage = self.logo
    love.graphics.draw(logoImage, (self.resWidth - logoImage:getWidth()) / 2, (self.resHeight - logoImage:getHeight()) * 0.5)

    local titleText = tostring(self.title)
    love.graphics.print(titleText, (self.resWidth - font:getWidth(titleText)) / 2, (self.resHeight - font:getHeight(titleText)) * 0.1)

    local captionText = tostring(self.caption)
    love.graphics.print(captionText, (self.resWidth - font:getWidth(captionText)) / 2, (self.resHeight - font:getHeight(captionText)) * 0.9)

    -- Draw Debug Info
    if self.debugMode then
        local fpsText = "FPS: "..tostring(love.timer.getFPS())
        love.graphics.print(fpsText, self.resWidth - (font:getWidth(fpsText) + self.margin / 4), self.resHeight - self.margin + ((self.margin - font:getHeight(fpsText)) / 2))
    end
end

function Splash:exitGame()
    return self.nextGame
end

return {
    Splash = Splash
}