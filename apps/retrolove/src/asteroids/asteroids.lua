-- asteroids\asteroids.lua
-- Copyright (c) 2017-2018 Jon Thysell

local utils = require "asteroids/utils"
local sprite = require "asteroids/sprite"
local queue = require "asteroids/queue"

local game = require "game"

local Asteroids = game.Game:new({
    id = "asteroids",
    title = "Asteroids",
    highScore = 0,
    highScoreFile = "asteroids.highscore.txt",
    startingLives = 2,
    startingShieldTime = 3,
    startingStage = 0,
    extraLivesScoreThreshold = 5000,
    shipThrustSpeed = 1.5,
    maxShipSpeed = 200,
    shipRotateSpeed = math.rad(180),
    shotSpeed = 200,
    maxShots = 5,
    shotCooldownTime = 1/5,
    startingAsteroidsMin = 2,
    startingAsteroidsMax = 16,
    startingAsteroidMaxSpeed = 25,
    explodeSpeedMultiplier = 10/9,
    asteroidValue = 160,
    enableTouchControls = false,
})

function Asteroids:loadState()
    local contents = love.filesystem.read(self.highScoreFile)
    if contents then self.highScore = tonumber(contents) end
end

function Asteroids:saveState()
    love.filesystem.write(self.highScoreFile, tostring(self.highScore))
end

function Asteroids:resetShip()
    self.ship = sprite.Ship:new({
        x = self.resWidth / 2,
        y = self.resHeight / 2,
        heading = math.rad(90),
        shieldTimeRemaining = self.startingShieldTime,
    })

    self.shots = queue.Queue:new()
    self.timeSinceLastShot = 0
end

function Asteroids:resetStage()
    local stage = self.player.stage

    local numAsteroids = math.min(self.startingAsteroidsMin + stage, self.startingAsteroidsMax)

    self.asteroids = queue.Queue:new()
    for i = 1, numAsteroids do
        local angle = math.rad(love.math.random(0, 360))
        local ax = self.ship.x + sprite.Asteroid.r * love.math.random(4, 10) * math.cos(angle)
        local ay = self.ship.y - sprite.Asteroid.r * love.math.random(4, 10) * math.sin(angle)
        local asteroid = sprite.Asteroid:new({
            x = utils.bound(ax, self.margin, self.resWidth - self.margin),
            y = utils.bound(ay, self.margin, self.resHeight - self.margin),
            orientation = angle,
            dx = -self.startingAsteroidMaxSpeed + (2 * self.startingAsteroidMaxSpeed) * love.math.random(),
            dy = -self.startingAsteroidMaxSpeed + (2 * self.startingAsteroidMaxSpeed) * love.math.random(),
        })
        self.asteroids:enqueue(asteroid)
    end
end

function Asteroids:resetGame()
    self.player = {
        score = 0,
        lives = self.startingLives,
        stage = self.startingStage,
    }

    self:resetShip()
    self:resetStage()

    self.pauseState = "GAME OVER"
    self.newGame = true
end

function Asteroids:createTouchButtons()
    local touchButtonRadius = math.min(self.screenHeight, self.screenWidth) / 10

    self.touchButtons = queue.Queue:new()

    self.touchButtons:enqueue(sprite.TouchButton:new({
        x = touchButtonRadius,
        y = self.screenHeight - 2.5 * touchButtonRadius,
        r = touchButtonRadius,
        action = "left",
    }))

    self.touchButtons:enqueue(sprite.TouchButton:new({
        x = 2.5 * touchButtonRadius,
        y = self.screenHeight - touchButtonRadius,
        r = touchButtonRadius,
        action = "right",
    }))

    self.touchButtons:enqueue(sprite.TouchButton:new({
        x = self.screenWidth - 2.5 * touchButtonRadius,
        y = self.screenHeight - touchButtonRadius,
        r = touchButtonRadius,
        action = "thrust",
    }))

    self.touchButtons:enqueue(sprite.TouchButton:new({
        x = self.screenWidth - touchButtonRadius,
        y = self.screenHeight - 2.5 * touchButtonRadius,
        r = touchButtonRadius,
        action = "fire",
    }))
end

function Asteroids:getInput()
    local input = {
        left = false,
        right = false,
        thrust = false,
        fire = false,
        start = false,
    }

    if self.ship.isAlive then
        -- Process Keyboard
        if love.keyboard.isDown("left") then input.left = true end
        if love.keyboard.isDown("right") then input.right = true end
        if love.keyboard.isDown("up") then input.thrust = true end
        if love.keyboard.isDown("space") then input.fire = true end
    end

    if self.enableTouchControls then
        local touches = love.touch.getTouches()

        -- Process TouchButtons
        for tB = 1, self.touchButtons:count() do
            local touchButton = self.touchButtons:dequeue()
            touchButton.isPressed = false

            for i, id in ipairs(touches) do
                local x, y = love.touch.getPosition(id)
                if utils.within(x, y, touchButton) then
                    touchButton.isPressed = true
                    break
                end
            end

            if self.ship.isAlive and touchButton.isPressed then
                if touchButton.action == "left" then input.left = true end
                if touchButton.action == "right" then input.right = true end
                if touchButton.action == "thrust" then input.thrust = true end
                if touchButton.action == "fire" then input.fire = true end
            end

            self.touchButtons:enqueue(touchButton)
        end
    end

    return input
end

function Asteroids:initGame()
    self.lifeShip = sprite.Ship:new({
        heading = math.rad(90),
    })

    -- Load sounds
    self.sfx = {}
    self.sfx.thrust = love.audio.newSource("asteroids/thrust.ogg", "static")
    self.sfx.shot = love.audio.newSource("asteroids/shot.ogg", "static")
    self.sfx.hit = love.audio.newSource("asteroids/hit.ogg", "static")
    self.sfx.death = love.audio.newSource("asteroids/death.ogg", "static")
    self.sfx.extralife = love.audio.newSource("asteroids/extralife.ogg", "static")
    self.sfx.start = love.audio.newSource("asteroids/start.ogg", "static")

    self.enableTouchControls = love.touch and (love.system.getOS() == "Android" or love.system.getOS() == "iOS")
    if self.enableTouchControls then self:createTouchButtons() end

    self:resetGame()
end

function Asteroids:keyReleasedGame(key)
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

function Asteroids:touchReleasedGame(id, x, y, dx, dy, pressure)
    if self.enableTouchControls then
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
end

function Asteroids:mirroredCollision(s1, s2)
    -- Check for regular collision
    if utils.collision(s1, s2) then return true end

    local m1 = s1:getMirrors(self.margin, self.resWidth - self.margin, self.margin, self.resHeight - self.margin)
    table.insert(m1, { x = s1.x, y = s1.y })
    local r1 = s1.r

    local m2 = s2:getMirrors(self.margin, self.resWidth - self.margin, self.margin, self.resHeight - self.margin)
    table.insert(m2, { x = s2.x, y = s2.y })
    local r2 = s2.r

    for i = 1, #m1 do
        for j = 1, #m2 do
            if utils.collision({ x = m1[i].x, y = m1[i].y, r = r1 }, { x = m2[j].x, y = m2[j].y, r = r2}) then return true end
        end
    end

    return false
end

function Asteroids:updateGame(dt, input)
    if not self.pauseState then
        if self.newGame then
            love.audio.play(self.sfx.start)
            self.newGame = false
        end

        -- Process input
        if input.left then
            self.ship.heading = self.ship.heading + (self.shipRotateSpeed * dt)
        elseif input.right then
            self.ship.heading = self.ship.heading - (self.shipRotateSpeed * dt)
        end

        while self.ship.heading < 0 do self.ship.heading = self.ship.heading + 2 * math.pi end
        while self.ship.heading >= 2 * math.pi do self.ship.heading = self.ship.heading - 2 * math.pi end

        self.ship.isThrusting = input.thrust

        if self.ship.isThrusting then
            love.audio.play(self.sfx.thrust)
            local newThrust = {
                dx = self.shipThrustSpeed * math.cos(self.ship.heading),
                dy = -1 * self.shipThrustSpeed * math.sin(self.ship.heading),
            }
            newThrust = utils.vectorAdd(newThrust, {dx = self.ship.dx, dy = self.ship.dy})
            self.ship.dx, self.ship.dy = newThrust.dx, newThrust.dy
        end

        self.timeSinceLastShot = self.timeSinceLastShot + dt

        if input.fire then
            if self.shots:count() < self.maxShots and self.timeSinceLastShot >= self.shotCooldownTime then
                love.audio.play(self.sfx.shot:clone())
                local newShot = sprite.Shot:new({
                    x = self.ship.x + self.ship.r * math.cos(self.ship.heading),
                    y = self.ship.y - self.ship.r * math.sin(self.ship.heading),
                    dx = self.ship.dx + self.shotSpeed * math.cos(self.ship.heading),
                    dy = self.ship.dy + (-1 * self.shotSpeed * math.sin(self.ship.heading)),
                    timeRemaining = 2 * self.shotCooldownTime,
                })
                newShot.timeRemaining = newShot.timeRemaining + dt
                self.shots:enqueue(newShot)
                self.timeSinceLastShot = 0
            end
        end

        -- Bound and process ship movement
        self.ship.dx = utils.bound(self.ship.dx, -self.maxShipSpeed, self.maxShipSpeed)
        self.ship.dy = utils.bound(self.ship.dy, -self.maxShipSpeed, self.maxShipSpeed)

        if self.ship.isAlive then
            self.ship:move(dt, self.margin, self.resWidth - self.margin, self.margin, self.resHeight - self.margin)
        end

        -- Process shot movements
        for i = 1, self.shots:count() do
            local shot = self.shots:dequeue()
            shot.timeRemaining = shot.timeRemaining - dt
            if shot.timeRemaining > 0 then
                shot:move(dt, self.margin, self.resWidth - self.margin, self.margin, self.resHeight - self.margin)
                self.shots:enqueue(shot)
            end
        end

        -- Process asteroid movements
        for i = 1, self.asteroids:count() do
            local asteroid = self.asteroids:dequeue()
            asteroid:move(dt, self.margin, self.resWidth - self.margin, self.margin, self.resHeight - self.margin)
            self.asteroids:enqueue(asteroid)
        end

        -- Process shot/asteroid collisions
        for i = 1, self.shots:count() do
            local shot = self.shots:dequeue()
            local hit = false

            for j = 1, self.asteroids:count() do
                local asteroid = self.asteroids:dequeue()
                hit = hit or self:mirroredCollision(shot, asteroid)
                if hit then
                    love.audio.play(self.sfx.hit:clone())
                    local beforeScore = self.player.score
                    self.player.score = self.player.score + self.asteroidValue / asteroid.r
                    self.highScore = math.max(self.highScore, self.player.score)
                    local afterScore = self.player.score

                    if utils.passThreshold(beforeScore, afterScore, self.extraLivesScoreThreshold) then
                        -- Extra life
                        love.audio.play(self.sfx.extralife)
                        self.player.lives = self.player.lives + 1
                    end

                    local a1, a2 = asteroid:split(self.explodeSpeedMultiplier)
                    if a1.r > 2 then self.asteroids:enqueue(a1) end
                    if a2.r > 2 then self.asteroids:enqueue(a2) end

                    break
                end
                self.asteroids:enqueue(asteroid)
            end

            if not hit then self.shots:enqueue(shot) end
        end

        -- Process ship/asteroid collisions
        if self.ship.isAlive and self.ship.shieldTimeRemaining <= 0 then
            -- Ship can be hit
            for i = 1, self.asteroids:count() do
                local asteroid = self.asteroids:dequeue()
                local shipHit = self:mirroredCollision(self.ship, asteroid)
                self.asteroids:enqueue(asteroid)
                if shipHit then
                    love.audio.play(self.sfx.death)
                    self.ship.deathTimeRemaining = 1.0
                    self.ship.isAlive = false
                    self.player.lives = self.player.lives - 1
                    break
                end
            end
        elseif self.ship.isAlive and self.ship.shieldTimeRemaining > 0 then
            self.ship.shieldTimeRemaining = self.ship.shieldTimeRemaining - dt
        elseif not self.ship.isAlive then
            self.ship.deathTimeRemaining = self.ship.deathTimeRemaining - dt
        end

        -- Gameover check
        if self.ship.isAlive and self.asteroids:count() == 0 then
            -- Load next stage
            love.audio.play(self.sfx.start)
            self.player.stage = self.player.stage + 1
            self:saveState()
            self:resetShip()
            self:resetStage()
        elseif not self.ship.isAlive and self.ship.deathTimeRemaining <= 0 then
            -- Ship has finished dying
            if self.player.lives >= 0 then
                self:resetShip()
            elseif self.player.lives < 0 then
                self:saveState()
                self:resetGame()
            end
        end
    end
end

function Asteroids:drawMirroredSprite(s)
    -- Draw main sprite
    s:draw()

    -- Draw mirrors
    local mirrors = s:getMirrors(self.margin, self.resWidth - self.margin, self.margin, self.resHeight - self.margin)
    for i = 1, #mirrors do
        s:draw(mirrors[i].x, mirrors[i].y)
    end
end

function Asteroids:drawGame()
    -- Draw to canvas
    local font = love.graphics.getFont()

    love.graphics.setColor({255, 255, 255, 255})

    if self.pauseState then
        local centerText = tostring(self.pauseState)
        love.graphics.print(centerText, (self.resWidth - font:getWidth(centerText)) / 2, (self.resHeight - font:getHeight(centerText)) / 2)
    else
        -- Draw ship
        self:drawMirroredSprite(self.ship)

        -- Draw shots
        for i = 1, self.shots:count() do
            local shot = self.shots:dequeue()
            self:drawMirroredSprite(shot)
            self.shots:enqueue(shot)
        end

        -- Draw asteroids
        for i = 1, self.asteroids:count() do
            local asteroid = self.asteroids:dequeue()
            self:drawMirroredSprite(asteroid)
            self.asteroids:enqueue(asteroid)
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
    local scoreText = utils.comma_value(self.player.score)
    love.graphics.print(scoreText, (self.resWidth * 0.5 - font:getWidth(scoreText)), (self.margin - font:getHeight(scoreText)) / 2)
    local highScoreText = "HI: "..utils.comma_value(self.highScore)
    love.graphics.print(highScoreText, (self.resWidth - self.margin - font:getWidth(highScoreText)), (self.margin - font:getHeight(highScoreText)) / 2)

    -- Draw lives
    local livesText = "x "..tostring(math.max(0, self.player.lives))
    self.lifeShip.r = font:getHeight(livesText) / 2
    self.lifeShip.x = self.margin + self.lifeShip.r * 0.5
    self.lifeShip.y = ((self.margin - font:getHeight(livesText)) / 2) + self.lifeShip.r
    self.lifeShip:draw()
    love.graphics.print(livesText, self.lifeShip.x + self.lifeShip.r, self.lifeShip.y - font:getHeight(livesText)/ 2)

    -- Draw Debug Info
    if self.debugMode then
       local fpsText = "FPS: "..tostring(love.timer.getFPS())
       love.graphics.print(fpsText, self.resWidth - (font:getWidth(fpsText) + self.margin / 4), self.resHeight - self.margin + ((self.margin - font:getHeight(fpsText)) / 2))

       local thrustText = tostring(math.floor(math.deg(self.ship.heading)))..", "..tostring(math.floor(self.ship.dx))..", "..tostring(math.floor(self.ship.dy))
       love.graphics.print(thrustText, (self.resWidth - font:getWidth(thrustText)) / 2, self.resHeight - self.margin + ((self.margin - font:getHeight(thrustText)) / 2))
    end
end

function Asteroids:drawOverlay()
    -- Draw touch controls
    if self.enableTouchControls then
        love.graphics.setColor({255, 255, 255, 127})
        for i = 1, self.touchButtons:count() do
            local touchButton = self.touchButtons:dequeue()
            touchButton:draw()
            self.touchButtons:enqueue(touchButton)
        end
    end
end

return {
    Asteroids = Asteroids
}