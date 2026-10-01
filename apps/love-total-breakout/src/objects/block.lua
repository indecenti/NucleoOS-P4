Block = GameObject:extend()

function Block:new(area, x, y, opts)
    self.super.new(self, area, x, y, opts)

    self.w, self.h = self.w or 40, self.h or 20
    self.color = self.color or Game.defaultColor
    self.health = self.health or 1
    self.area.world:add(self, self.x - self.w/2, self.y - self.h/2, self.w, self.h)
end

function Block:update(dt)
    self.super.update(self, dt)
end

function Block:draw()
    love.graphics.setColor(self.color)
    love.graphics.rectangle("fill", self.x - self.w/2, self.y - self.h/2, self.w, self.h)
    love.graphics.setColor(Game.defaultColor)
end

function Block:destroy()
    self.super.destroy(self)
end

function Block:kill()
    self.area.world:remove(self)
    self.timer:tween(0.5, self, { w = 0, h = 0 }, 'in-out-back', function()
        self.dead = true
    end)
end

function Block:hit()
    self.health = self.health - 1
    if self.health <= 0 then
        self:kill()
    else
        self.color[1] = self.color[1] - 0.4
        self.color[2] = self.color[2] - 0.4
        self.color[3] = self.color[3] - 0.4
    end
end