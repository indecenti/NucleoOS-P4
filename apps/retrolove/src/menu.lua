-- menu.lua
-- Copyright (c) 2018 Jon Thysell

local game = require "game"

local Menu = game.Game:new({
    id = "menu",
    title = "RetroLove Menu",
    caption = "Made with LÖVE",
    games = {},
    selectedGame = 0,
    boxSize = 80,
    boxLeftMargin = 10,
})

function Menu:keyReleasedGame(key)
    if key == "q" or key == "escape" then
        self:exit()
    elseif key == "d" then
        self.debugMode = not self.debugMode
    elseif key == "left" or key == "right" then
        self:moveCursor(key)
    elseif key == "space" or key == "return" then
        self:clickCursor()
    end
end

function Menu:touchReleasedGame(id, x, y, dx, dy, pressure)
    local gameClick = false
    if x > self.canvasOriginX and x < self.canvasOriginX + self.resWidth * self.scale then
        -- on the canvas
        local boxSize = self.boxSize
        local boxTop = (self.resHeight - boxSize) / 2
        local boxLeftMargin = self.boxLeftMargin

        for i = 1, #self.games do
            local boxX = self.margin + (i * boxLeftMargin) + ((i - 1) * boxSize)

            local scaledRect = {
                x = self.canvasOriginX + boxX * self.scale,
                y = self.canvasOriginY + boxTop * self.scale,
                width = boxSize * self.scale,
                height = boxSize * self.scale,
            }

            if x > scaledRect.x and x < scaledRect.x + scaledRect.width and y > scaledRect.y and y < scaledRect.y + scaledRect.height then
                gameClick = true
                if self.selectedGame == i then
                    self:clickCursor()
                else
                    self.selectedGame = i
                end
                break;
            end
        end
    end

    if not gameClick then
        if x > self.screenWidth * .4 and x < self.screenWidth * .6 then
            if y > self.screenHeight / 2 then
                self.debugMode = not self.debugMode
            end
        end
    end
end

function Menu:drawGame()
    -- Draw to canvas
    local font = love.graphics.getFont()

    love.graphics.setColor({255, 255, 255, 255})

    local titleText = tostring(self.title)
    love.graphics.print(titleText, (self.resWidth - font:getWidth(titleText)) / 2, (self.resHeight - font:getHeight(titleText)) * 0.1)

    local captionText = tostring(self.caption)
    love.graphics.print(captionText, (self.resWidth - font:getWidth(captionText)) / 2, (self.resHeight - font:getHeight(captionText)) * 0.9)

    if #self.games > 0 then
        local boxSize = self.boxSize
        local boxTop = (self.resHeight - boxSize) / 2
        local boxLeftMargin = self.boxLeftMargin

        for i = 1, #self.games do
            local boxX = self.margin + (i * boxLeftMargin) + ((i - 1) * boxSize)
                
            love.graphics.setColor({255, 255, 255, 255})
            love.graphics.rectangle("line", boxX, boxTop, boxSize, boxSize)

            local title = tostring(self.games[i].title)
            love.graphics.print(title, boxX + (boxSize - font:getWidth(title)) / 2, (self.resHeight - font:getHeight(title)) / 2)

            if self.selectedGame == i then
                love.graphics.setColor({0, 255, 255, 255})
                love.graphics.rectangle("line", boxX, boxTop, boxSize, boxSize)
                love.graphics.print(title, boxX + (boxSize - font:getWidth(title)) / 2, (self.resHeight - font:getHeight(title)) / 2)
            end
        end
    end

    love.graphics.setColor({255, 255, 255, 255})

    -- Draw Debug Info
    if self.debugMode then
        local fpsText = "FPS: "..tostring(love.timer.getFPS())
        love.graphics.print(fpsText, self.resWidth - (font:getWidth(fpsText) + self.margin / 4), self.resHeight - self.margin + ((self.margin - font:getHeight(fpsText)) / 2))
    end
end

function Menu:exitGame()
    return nil
end

function Menu:addGame(newGame)
    self.games[#self.games + 1] = newGame
end

function Menu:moveCursor(direction)
    if #self.games > 0 then
        if direction == "left" then
            if self.selectedGame == 0 then
                self.selectedGame = #self.games
            else
                self.selectedGame = math.max(1, self.selectedGame - 1)
            end
        elseif direction == "right" then
            if self.selectedGame == 0 then
                self.selectedGame = 1
            else
                self.selectedGame = math.min(#self.games, self.selectedGame + 1)
            end
        end
        print("Cursor on game #"..tostring(self.selectedGame)..": "..tostring(self.games[self.selectedGame].id))
    end
end

function Menu:clickCursor()
    if self.selectedGame > 0 then
        local retrolove = require "retrolove"
        local splash = require "splash"

        local nextGame = self.games[self.selectedGame]

        retrolove.switchGame(splash.Splash:new({
            title = "Loading "..nextGame.title.."...",
            nextGame = nextGame,
        }))
    end
end

return {
    Menu = Menu
}