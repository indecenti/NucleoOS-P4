GameObject = Object:extend()

function GameObject:new(area, x, y, opts)
    local opts = opts or {}
    for k,v in pairs(opts) do self[k] = v end

    self.area = area
    self.x, self.y = x, y
    self.id = UUID()
    self.dead = false
    self.timer = Timer()
end

function GameObject:update(dt)
    if self.timer then self.timer:update(dt) end
end

function GameObject:draw()

end

function GameObject:destroy()
    self.timer:destroy()
end

function GameObject:debugDraw()
    if self.area.world then
        love.graphics.setColor(1, 0, 0)
        local x, y, w, h = self.area.world:getRect(self)
        love.graphics.rectangle("line", x, y, w, h)
        love.graphics.setColor(Game.defaultColor)
    end
end