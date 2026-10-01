-- asteroids\sprite.lua
-- Copyright (c) 2017 Jon Thysell

-- Sprite
local Sprite = {
    x = 0,
    y = 0,
    dx = 0,
    dy = 0,
    r = 0,
}

function Sprite:new(o)
    local o = o or {}
    setmetatable(o, self)
    self.__index = self
    return o
end

function Sprite:getMirrors(xmin, xmax, ymin, ymax)
    local mirrors = {}

    if self.x - self.r < xmin then table.insert(mirrors, { x = self.x + (xmax - xmin), y = self.y }) end
    if self.x + self.r > xmax then table.insert(mirrors, { x = self.x - (xmax - xmin), y = self.y }) end
    if self.y - self.r < ymin then table.insert(mirrors, { x = self.x, y = self.y + (ymax - ymin) }) end
    if self.y + self.r > ymax then table.insert(mirrors, { x = self.x, y = self.y - (ymax - ymin) }) end

    return mirrors
end

function Sprite:move(dt, xmin, xmax, ymin, ymax)
    self.x = self.x + self.dx * dt
    self.y = self.y + self.dy * dt

    -- Process wrap around
    if xmin and self.x < xmin then self.x = xmax end
    if xmax and self.x > xmax then self.x = xmin end
    if ymin and self.y < ymin then self.y = ymax end
    if ymax and self.y > ymax then self.y = ymin end

end

function Sprite:draw(ox, oy)
    local ox = ox or self.x
    local oy = oy or self.y

    love.graphics.circle("fill", ox, oy, self.r)
end

-- Ship
local Ship = Sprite:new({
    r = 6,
    heading = 0,
    shieldTimeRemaining = 0,
    deathTimeRemaining = 0,
    isAlive = true,
    isThrusting = false,
})

function Ship:draw(ox, oy)
    local ox = ox or self.x
    local oy = oy or self.y

    if self.deathTimeRemaining > 0 then
        love.graphics.circle("fill", ox, oy, self.r * (1 - self.deathTimeRemaining))
    else
        local p = {
            6, 0,
            -6, -4,
            -4, 0,
            -6, 4,
        }

        for i = 1, #p, 2 do
            local x, y = p[i], p[i+1]
            p[i] = ox + (x * math.cos(self.heading)) - (y * math.sin(self.heading))
            p[i+1] = oy - ((x * math.sin(self.heading)) + (y * math.cos(self.heading)))
        end

        love.graphics.polygon("fill", p)

        if self.isThrusting then
            local p = {
                -6, -2,
                -8, 0,
                -6, 2,
                -4, 0,
            }

            for i = 1, #p, 2 do
                local x, y = p[i], p[i+1]
                p[i] = ox + (x * math.cos(self.heading)) - (y * math.sin(self.heading))
                p[i+1] = oy - ((x * math.sin(self.heading)) + (y * math.cos(self.heading)))
            end

            love.graphics.polygon("line", p)
        end

        if self.shieldTimeRemaining > 0 then
            love.graphics.circle("line", ox, oy, self.r * 1.5)
        end
    end
end

-- Shot
local Shot = Sprite:new({
    r = 1,
    timeRemaining = 0,
})

-- Asteroid
local Asteroid = Sprite:new({
    r = 16,
    orientation = 0,
})

function Asteroid:split(speedMultiplier)
    local speedMultiplier = speedMultiplier or 1

    local m = math.sqrt(self.dx^2 + self.dy^2)
    local d = math.atan(self.dy / self.dx)

    local a1 = Asteroid:new({
        x = self.x,
        y = self.y,
        orientation = math.rad(love.math.random(0, 360)),
        dx = speedMultiplier * m * math.cos(d + math.rad(90)),
        dy = speedMultiplier * m * math.sin(d + math.rad(90)),
        r = self.r / 2
    })

    local a2 = Asteroid:new({
        x = self.x,
        y = self.y,
        orientation = math.rad(love.math.random(0, 360)),
        dx = speedMultiplier * m * math.cos(d - math.rad(90)),
        dy = speedMultiplier * m * math.sin(d - math.rad(90)),
        r = self.r / 2
    })

    return a1, a2
end

function Asteroid:draw(ox, oy)
    local ox = ox or self.x
    local oy = oy or self.y

    local p = {
        1, 0,
        0.3, 0.4,
        0.5, 0.75,
        0, 1,
        -0.8, 0.5,
        -1, 0,
        -0.6, -0.6,
        0, -1,
        0.3, -0.5,
        0.8, -0.4,
    }

    for i = 1, #p, 2 do
        local x, y = self.r * p[i], self.r * p[i+1]
        p[i] = ox + (x * math.cos(self.orientation)) - (y * math.sin(self.orientation))
        p[i+1] = oy - ((x * math.sin(self.orientation)) + (y * math.cos(self.orientation)))
    end

    love.graphics.polygon("line", p)
end

--TouchButton
local TouchButton = Sprite:new({
    isPressed = false,
    action = ""
})

function TouchButton:draw(ox, oy)
    local ox = ox or self.x
    local oy = oy or self.y

    if self.isPressed then
        love.graphics.circle("fill", ox, oy, self.r)
    else
        love.graphics.circle("line", ox, oy, self.r)
    end
end

return {
    Sprite = Sprite,
    Ship = Ship,
    Shot = Shot,
    Asteroid = Asteroid,
    TouchButton = TouchButton,
}