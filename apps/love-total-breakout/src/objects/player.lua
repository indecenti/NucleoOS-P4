Player = GameObject:extend()

local function playerFilter(item, other)
    -- La collision avec la balle est gérée manuellement
    if other.class == "Ball" then return "cross"
    elseif other.class == "Wall" then return "touch"
    end
end

function Player:new(area, x, y, opts)
    self.super.new(self, area, x, y, opts)

    self.w, self.h = 80, 16
    self.maxSpeed = 450
    self.speed = 0
    self.area.world:add(self, self.x - self.w/2, self.y - self.h/2 + 4, self.w, self.h - 8)
end

function Player:update(dt)
    self.super.update(self, dt)

    if Input:pressed("start") then
        Signal.emit("launchBall")
    end

    if Input:down("left") then self.speed = self.maxSpeed
    elseif Input:down("right") then self.speed = -self.maxSpeed
    else self.speed = 0 end

    local goalX, goalY = self.x - self.w/2 + self.speed * dt, self.y - self.h/2 + 4
    local resX, resY = self.area.world:move(self, goalX, goalY, playerFilter)
    self.x, self.y = resX + self.w/2, resY + self.h/2 - 4
end

function Player:draw()
    love.graphics.rectangle("fill", self.x - self.w/2, self.y - self.h/2 + 4, self.w, self.h - 8)
    love.graphics.setColor(1, 0.56, 0)
    love.graphics.rectangle("fill", self.x - self.w/2, self.y - self.h/2, 10, self.h)
    love.graphics.rectangle("fill", self.x + self.w/2 - 10, self.y - self.h/2, 10, self.h)
    love.graphics.setColor(Game.defaultColor)
end

function Player:destroy()
    self.super.destroy(self)
    self.area.world:remove(self)
end