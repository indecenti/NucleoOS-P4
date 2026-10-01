Wall = GameObject:extend()

function Wall:new(area, x, y, opts)
    self.super.new(self, area, x, y, opts)

    self.w, self.h = self.w or 40, self.h or 20
    self.color = self.color or { 0.1, 0.1, 0.1 }
    self.area.world:add(self, self.x, self.y, self.w, self.h)
end

function Wall:update(dt)

end

function Wall:draw()
    love.graphics.setColor(self.color)
    love.graphics.rectangle("fill", self.x, self.y, self.w, self.h)
    love.graphics.setColor(Game.defaultColor)
end

function Wall:destroy()
    self.super.destroy(self)
    self.area.world:remove(self)
end