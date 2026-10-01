-- pong\pong.lua
-- Copyright (c) 2017-2018 Jon Thysell

local game = require "game"

local Pong = game.Game:new({
    id = "pong",
    title = "Pong",
    scoreToWin = 10,
    paddleSpeed = 2,
    ballSpeed = 0.5,
    paddleBounce = 1.1,
})

function Pong:initGame()
    self:resetPaddles()
    self:resetBall()

    -- Init sounds
    self.bounceSFX = love.audio.newSource("pong/bounce.ogg", "static")
    self.hitSFX = love.audio.newSource("pong/hit.ogg", "static")
    self.scoreSFX = love.audio.newSource("pong/score.ogg", "static")
end

function Pong:resetBall()
    self.ball = {
        x = (self.resWidth - self.ballSize) / 2,
        y = (self.resHeight - self.ballSize) / 2,
        dx = 0.5 + 0.1 * love.math.random(),
        dy = 0.5 + 0.1 * love.math.random()
    }

    if love.math.random() > 0.5 then self.ball.dx = -self.ball.dx end
    if love.math.random() > 0.5 then self.ball.dy = -self.ball.dy end
end

function Pong:resetPaddles()
    self.paddleHeight = self.resHeight * 0.2
    self.paddleWidth = self.resWidth * 0.0125
    self.ballSize = self.paddleWidth

    self.left = {
        x = self.paddleWidth + self.margin,
        y = (self.resHeight - self.paddleHeight) / 2,
        moving = false,
        score = 0,
    }

    self.right = {
        x = self.resWidth - (2 * self.paddleWidth + self.margin),
        y = (self.resHeight - self.paddleHeight) / 2,
        moving = false,
        score = 0,
    }

    self.pauseState = "GAME OVER"
    self.newGame = true
end

function Pong:getInput()
    if love.keyboard.isDown("down") then
        return "down"
    elseif love.keyboard.isDown("up") then
        return "up"
    end

    if love.touch then
        local touches = love.touch.getTouches()
        for i, id in ipairs(touches) do
            local x, y = love.touch.getPosition(id)
            if x <= self.screenWidth * .4 or x >= self.screenWidth * .6 then
                if y < self.screenHeight / 2 then
                    return "up"
                else
                    return "down"
                end
            end
        end
    end

    return nil
end

function Pong:keyReleasedGame(key)
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

function Pong:touchReleasedGame(id, x, y, dx, dy, pressure)
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

function Pong:updateGame(dt, input)
    if not self.pauseState then
        if self.newGame then
            self.newGame = false
        end

        -- Process input for left
        if input == "down" then
            self.left.y = self.left.y + (self.paddleHeight * self.paddleSpeed * dt)
            self.left.moving = true
        elseif input == "up" then
            self.left.y = self.left.y - (self.paddleHeight * self.paddleSpeed * dt)
            self.left.moving = true
        else
            self.left.moving = false
        end

        -- Process AI for right
        if self.ball.dx > 0 then
            if self.ball.y + (self.ballSize / 2) > self.right.y + self.paddleHeight then
                self.right.y = self.right.y + (self.paddleHeight * self.paddleSpeed * dt)
                self.right.moving = true
            elseif self.ball.y + (self.ballSize / 2) < self.right.y then
                self.right.y = self.right.y - (self.paddleHeight * self.paddleSpeed * dt)
                self.right.moving = true
            else
                self.right.moving = false
            end
        end

        -- Process paddle bounds
        self.left.y = math.max(self.left.y, self.margin)
        self.left.y = math.min(self.left.y, self.resHeight - self.paddleHeight - self.margin)
        self.right.y = math.max(self.right.y, self.margin)
        self.right.y = math.min(self.right.y, self.resHeight - self.paddleHeight - self.margin)

        -- Process ball movement
        self.ball.dx = math.min(1, math.max(self.ball.dx, -1))
        self.ball.dy = math.min(1, math.max(self.ball.dy, -1))

        self.ball.x = self.ball.x + (self.ball.dx * self.ballSpeed * math.min(self.resHeight, self.resWidth) * dt)
        self.ball.y = self.ball.y + (self.ball.dy * self.ballSpeed * math.min(self.resHeight, self.resWidth) * dt)

        -- Process wall/ball collisions
        if self.ball.y < self.margin then
            love.audio.play(self.bounceSFX:clone())
            self.ball.y = self.margin
            self.ball.dy = -self.ball.dy
        elseif self.ball.y > self.resHeight - self.ballSize - self.margin then
            love.audio.play(self.bounceSFX:clone())
            self.ball.y = self.resHeight - self.ballSize - self.margin
            self.ball.dy = -self.ball.dy
        end

        if self.ball.x < self.left.x + self.paddleWidth then
            -- ball is crossing into left
            if self.ball.x > self.left.x and self.ball.y > self.left.y - self.ballSize and self.ball.y < self.left.y + self.paddleHeight then
                -- left hits ball
                love.audio.play(self.hitSFX:clone())
                self.ball.x = self.left.x + self.paddleWidth
                self.ball.dx = -self.ball.dx
                if self.left.moving then
                    self.ball.dx = self.ball.dx * self.paddleBounce
                    self.ball.dy = self.ball.dy * self.paddleBounce
                end
            elseif self.ball.x + self.ballSize <= self.margin then
                -- left misses ball
                love.audio.play(self.scoreSFX)
                self.right.score = self.right.score + 1
                self:resetBall()
            end
        elseif self.ball.x + self.ballSize > self.right.x then
            -- ball is crossing into right
            if self.ball.x + self.ballSize < self.right.x + self.paddleWidth and self.ball.y > self.right.y - self.ballSize and self.ball.y < self.right.y + self.paddleHeight then
                -- right hits ball
                love.audio.play(self.hitSFX:clone())
                self.ball.x = self.right.x - self.ballSize
                self.ball.dx = -self.ball.dx
                if self.right.moving then
                    self.ball.dx = self.ball.dx * self.paddleBounce
                    self.ball.dy = self.ball.dy * self.paddleBounce
                end
            elseif self.ball.x >= self.resWidth - self.margin then
                -- right misses ball
                love.audio.play(self.scoreSFX)
                self.left.score = self.left.score + 1
                self:resetBall()
            end
        end

        if self.left.score == self.scoreToWin or self.right.score == self.scoreToWin then
            self:resetPaddles()
        end
    end
end

function Pong:drawGame()
    -- Draw to canvas
    local font = love.graphics.getFont()

    love.graphics.setColor({255, 255, 255, 255})

    if self.pauseState then
        local centerText = tostring(self.pauseState)
        love.graphics.print(centerText, (self.resWidth - font:getWidth(centerText)) / 2, (self.resHeight - font:getHeight(centerText)) / 2)
    else
        -- Draw center line
        love.graphics.line(self.resWidth / 2, self.margin, self.resWidth / 2, self.resHeight - self.margin)

        -- Draw paddles
        love.graphics.rectangle("fill", self.left.x, self.left.y, self.paddleWidth, self.paddleHeight)
        love.graphics.rectangle("fill", self.right.x, self.right.y, self.paddleWidth, self.paddleHeight)

        -- Draw ball
        love.graphics.rectangle("fill", self.ball.x, self.ball.y, self.ballSize, self.ballSize)
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
    local leftScoreText = tostring(self.left.score)
    love.graphics.print(leftScoreText, (self.resWidth * .25) - (font:getWidth(leftScoreText)/ 2), self.ballSize / 2)
    local rightScoreText = tostring(self.right.score)
    love.graphics.print(rightScoreText, (self.resWidth * .75) - (font:getWidth(rightScoreText)/ 2), self.ballSize / 2)

    -- Draw Debug Info
    if self.debugMode then
        local fpsText = "FPS: "..tostring(love.timer.getFPS())
        love.graphics.print(fpsText, self.resWidth - (font:getWidth(fpsText) + self.margin / 4), self.resHeight - self.margin + ((self.margin - font:getHeight(fpsText)) / 2))
    end
end

return {
    Pong = Pong
}