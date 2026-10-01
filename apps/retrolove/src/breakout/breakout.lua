-- breakout\breakout.lua
-- Copyright (c) 2017-2018 Jon Thysell

local game = require "game"

local Breakout = game.Game:new({
    id = "breakout",
    title = "Breakout",
    highScore = 0,
    paddleSpeed = 2,
    ballSpeed = 0.5,
    minPaddleBounce = 1.0,
    maxPaddleBounce = 1.1,
    numBlocksPerRow = 10,
    blockColors = {
        {148, 0, 211}, -- violet
        {75, 0, 130}, -- indigo
        {0, 0, 255}, -- blue
        {0, 255, 0}, -- green
        {255, 255, 0}, -- yellow
        {255, 127, 0}, -- orange
        {255, 0, 0}, -- red
    },
    minNumRowsOfBlocks = 1,
    maxNumRowsOfBlocks = 7,
    highScoreFile = "breakout.highscore.txt",
})

function Breakout:loadState()
    local contents = love.filesystem.read(self.highScoreFile)
    if contents then self.highScore = tonumber(contents) end
end

function Breakout:saveState()
    love.filesystem.write(self.highScoreFile, tostring(self.highScore))
end

function Breakout:resetPaddle()
    self.paddle.x = (self.resWidth - self.paddleWidth) / 2
    self.paddle.y = self.resHeight - self.paddleHeight - self.ballSize - self.margin

    self.ball = {
        x = (self.resWidth - self.ballSize) / 2,
        y = self.paddle.y - 3 * self.ballSize,
        dx = 0.5 + 0.1 * love.math.random(),
        dy = -1 * (0.5 + 0.1 * love.math.random())
    }

    if love.math.random() > 0.5 then self.ball.dx = -self.ball.dx end

    self.started = false
end

function Breakout:resetStage()
    local stage = self.paddle.stage

    self.numRowsOfBlocks = math.min(self.minNumRowsOfBlocks + stage, self.maxNumRowsOfBlocks)

    self.blocks = {}
    for row = 1, self.numRowsOfBlocks do
        self.blocks[row] = {}
        for col = 1, self.numBlocksPerRow do
            self.blocks[row][col] = true
        end
    end
end

function Breakout:resetGame()
    self.paddleHeight = self.resHeight * 0.015
    self.paddleWidth = self.resWidth * 0.15

    self.ballSize = self.paddleHeight

    self.blockHeight = self.paddleHeight * 2
    self.blockWidth = (self.resWidth - 2 * self.margin) / self.numBlocksPerRow

    self.paddle = {
        moving = false,
        lives = 2,
        score = 0,
        stage = 0,
    }

    self:resetPaddle()
    self:resetStage()

    self.pauseState = "GAME OVER"
    self.newGame = true
end

function Breakout:comma_value(n) -- credit http://richard.warburton.it
	local left,num,right = string.match(n,'^([^%d]*%d)(%d*)(.-)$')
	return left..(num:reverse():gsub('(%d%d%d)','%1,'):reverse())..right
end

function Breakout:ballHitsBlock(row, col)
    local x, y = self.margin + (col - 1) * self.blockWidth, self.margin + (self.numRowsOfBlocks - row + 1) * self.blockHeight

    -- Find intersection
    local ix1 = math.max(self.ball.x, x)
    local iy1 = math.max(self.ball.y, y)
    local ix2 = math.min(self.ball.x + self.ballSize, x + self.blockWidth)
    local iy2 = math.min(self.ball.y + self.ballSize, y + self.blockHeight)

    local hit = false

    if ix1 < ix2 and iy1 < iy2 then
        -- Valid intersection
        if ix1 > self.ball.x then
            -- ball hit left
            self.ball.x = x - self.ballSize
            self.ball.dx = -self.ball.dx
            hit = true
        elseif ix2 < self.ball.x + self.ballSize then
            -- ball hit right
            self.ball.x = x + self.blockWidth
            self.ball.dx = -self.ball.dx
            hit = true
        end

        if iy1 > self.ball.y then
            -- ball hit top
            self.ball.y = y - self.ballSize
            self.ball.dy = -self.ball.dy
            hit = true
        elseif iy2 < self.ball.y + self.ballSize then
            -- ball hit bottom
            self.ball.y = y + self.blockHeight
            self.ball.dy = -self.ball.dy
            hit = true
        end
    end

    return hit
end

function Breakout:remainingBlocks()
    local sum = 0
    for row = 1, self.numRowsOfBlocks do
        for col = 1, self.numBlocksPerRow do
            if self.blocks[row][col] then sum = sum + 1 end
        end
    end

    return sum
end

function Breakout:getInput()
    if love.keyboard.isDown("left") then
        return "left"
    elseif love.keyboard.isDown("right") then
        return "right"
    end

    if love.touch then
        local touches = love.touch.getTouches()
        for i, id in ipairs(touches) do
            local x, y = love.touch.getPosition(id)
            if x <= self.screenWidth * .4 then
                return "left"
            elseif x >= self.screenWidth * .6 then
                return "right"
            end
        end
    end

    return nil
end

function Breakout:keyReleasedGame(key)
    if key == "q" or key == "escape" then
        self:exit()
    elseif key == "return" then
        if self.pauseState then
            self.pauseState = nil
        else
            self.pauseState = "PAUSED"
        end
    elseif key == "d" then
        self.debugMode = not self.debugMode
    end
end

function Breakout:touchReleasedGame(id, x, y, dx, dy, pressure)
    if x > self.screenWidth * .4 and x < self.screenWidth * .6 then
        if y < self.screenHeight / 2 then
            if self.pauseState then
                self.pauseState = nil
            else
                self.pauseState = "PAUSED"
            end
        else
            self.debugMode = not self.debugMode
        end
    end
end

function Breakout:initGame()
    -- Setup game
    self:resetGame()

    -- Init sounds
    self.startSFX = love.audio.newSource("breakout/start.ogg", "static")
    self.bounceSFX = love.audio.newSource("breakout/bounce.ogg", "static")
    self.hitSFX = love.audio.newSource("breakout/hit.ogg", "static")
end

function Breakout:updateGame(dt, input)
    if not self.pauseState then
        if self.newGame then
            love.audio.play(self.startSFX)
            self.newGame = false
        end

        if not self.started then
            if input then
                self.started = true
            end
        else
            -- Process input for paddle
            if input == "left" then
                self.paddle.x = self.paddle.x - (self.paddleWidth * self.paddleSpeed * dt)
                self.paddle.moving = true
            elseif input == "right" then
                self.paddle.x = self.paddle.x + (self.paddleWidth * self.paddleSpeed * dt)
                self.paddle.moving = true
            else
                self.paddle.moving = false
            end

            -- Process paddle bounds
            self.paddle.x = math.max(self.paddle.x, self.margin)
            self.paddle.x = math.min(self.paddle.x, self.resWidth - self.paddleWidth - self.margin)

            -- Process ball movement
            self.ball.dx = math.min(1, math.max(self.ball.dx, -1))
            self.ball.dy = math.min(1, math.max(self.ball.dy, -1))

            self.ball.x = self.ball.x + (self.ball.dx * self.ballSpeed * math.min(self.resHeight, self.resWidth) * dt)
            self.ball.y = self.ball.y + (self.ball.dy * self.ballSpeed * math.min(self.resHeight, self.resWidth) * dt)

            -- Process wall/ball collisions
            if self.ball.x < self.margin then
                love.audio.play(self.bounceSFX:clone())
                self.ball.x = self.margin
                self.ball.dx = -self.ball.dx
            elseif self.ball.x > self.resWidth - self.ballSize - self.margin then
                love.audio.play(self.bounceSFX:clone())
                self.ball.x = self.resWidth - self.ballSize - self.margin
                self.ball.dx = -self.ball.dx
            end

            -- Process block collisions
            local blockHit = 0
            for row = 1, self.numRowsOfBlocks do
                for col = 1, self.numBlocksPerRow do
                    if self.blocks[row][col] then
                        local hit = self:ballHitsBlock(row, col)
                        if hit then
                            self.blocks[row][col] = false
                            blockHit = row
                            break
                        end
                    end
                end
                if blockHit > 0 then break end
            end

            if blockHit > 0 then
                love.audio.play(self.hitSFX:clone())
                self.paddle.score = self.paddle.score + 10 * blockHit
                self.highScore = math.max(self.highScore, self.paddle.score)
            end

            -- Process paddle collisions
            if self.ball.y < self.margin then
                love.audio.play(self.bounceSFX:clone())
                self.ball.y = self.margin
                self.ball.dy = -self.ball.dy
            elseif self.ball.y + self.ballSize > self.paddle.y then
                -- ball is crossing into bottom
                if self.ball.x + self.ballSize > self.paddle.x and self.ball.x < self.paddle.x + self.paddleWidth then
                    -- paddle hits ball
                    love.audio.play(self.bounceSFX:clone())
                    self.ball.y = self.paddle.y - self.ballSize
                    self.ball.dy = -self.ball.dy
                    if self.paddle.moving then
                        self. ball.dx = self.ball.dx * (self.minPaddleBounce + ((self.maxPaddleBounce - self.minPaddleBounce) * love.math.random()))
                        self.ball.dy = self.ball.dy * (self.minPaddleBounce + ((self.maxPaddleBounce - self.minPaddleBounce) * love.math.random()))
                    end
                elseif self.ball.y >= self.resHeight - self.margin then
                    -- paddle misses ball
                    self.paddle.lives = self.paddle.lives - 1
                    self:resetPaddle()
                end
            end

            -- Gameover check
            if self.paddle.lives < 0 then
                self:saveState()
                self:resetGame()
            elseif self:remainingBlocks() == 0 then
                -- Load next stage
                love.audio.play(self.startSFX)
                self.paddle.stage = self.paddle.stage + 1
                self:saveState()
                self:resetPaddle()
                self:resetStage()
            end
        end
    end
end

function Breakout:drawGame()
    -- Draw to canvas
    local font = love.graphics.getFont()

    love.graphics.setColor({255, 255, 255, 255})

    if self.pauseState then
        local centerText = tostring(self.pauseState)
        love.graphics.print(centerText, (self.resWidth - font:getWidth(centerText)) / 2, (self.resHeight - font:getHeight(centerText)) / 2)
    else
        -- Draw paddle
        love.graphics.rectangle("fill", self.paddle.x, self.paddle.y, self.paddleWidth, self.paddleHeight)

        -- Draw ball
        love.graphics.circle("fill", self.ball.x + (self.ballSize / 2), self.ball.y + (self.ballSize / 2), self.ballSize / 2)

        -- Draw blocks
        for row = 1, self.numRowsOfBlocks do
            for col = 1, self.numBlocksPerRow do
                love.graphics.setColor(self.blockColors[row])
                if self.blocks[row][col] then
                    local x, y = self.margin + (col - 1) * self.blockWidth, self.margin + (self.numRowsOfBlocks - row + 1) * self.blockHeight
                    love.graphics.rectangle("fill", x, y, self.blockWidth, self.blockHeight)
                end
            end
        end
    end

    -- Draw margins
    love.graphics.setColor({0, 0, 0, 255})
    love.graphics.rectangle("fill", 0, 0, self.margin, self.resHeight - self.margin)
    love.graphics.rectangle("fill", 0, self.resHeight - self.margin, self.resWidth - self.margin, self.resHeight)
    love.graphics.rectangle("fill", self.margin, 0, self.resWidth, self.margin)
    love.graphics.rectangle("fill", self.resWidth - self.margin, self.margin, self.resWidth, self.resHeight)
    love.graphics.setColor({255, 255, 255, 255})
    love.graphics.rectangle("line", self.margin - 1 , self.margin - 1, self.resWidth - 2 * (self.margin - 1), self.resHeight - (2 * self.margin - 1))

    -- Draw score
    local scoreText = self:comma_value(self.paddle.score)
    love.graphics.print(scoreText, (self.resWidth * 0.5 - font:getWidth(scoreText)), (self.margin - font:getHeight(scoreText)) / 2)
    local highScoreText = "HI: "..self:comma_value(self.highScore)
    love.graphics.print(highScoreText, (self.resWidth - self.margin - font:getWidth(highScoreText)), (self.margin - font:getHeight(highScoreText)) / 2)

    -- Draw lives
    local livesText = "x"..tostring(math.max(0, self.paddle.lives))
    love.graphics.print(livesText, self.margin / 4, self.resHeight - self.margin + ((self.margin - font:getHeight(livesText)) / 2))

    -- Draw Debug Info
    if self.debugMode then
        local fpsText = "FPS: "..tostring(love.timer.getFPS())
        love.graphics.print(fpsText, self.resWidth - (font:getWidth(fpsText) + self.margin / 4), self.resHeight - self.margin + ((self.margin - font:getHeight(fpsText)) / 2))
    end
end

return {
    Breakout = Breakout
}