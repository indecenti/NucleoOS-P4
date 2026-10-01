BallTrailEffect = GameObject:extend()

function BallTrailEffect:new(area, x, y, opts)
    self.super.new(self, area, x, y, opts)
    self.w = self.w or 5
    self.color = self.color or M.clone(Game.defaultColor)
    self.timer:tween(0.2, self,
    { w = 0, color = Game.backgroundColor },
    'out-linear', 
    function ()
        self.dead = true
    end)
end

function BallTrailEffect:update(dt)
    self.super.update(self, dt)
end

function BallTrailEffect:draw()
    love.graphics.setColor(self.color)
    love.graphics.circle("fill", self.x, self.y, self.w)
    love.graphics.setColor(Game.defaultColor)
end

function BallTrailEffect:destroy()
    self.super.destroy(self)
end