local Bullet = Object:extend()

function Bullet:new(x, y)
	self.image = love.graphics.newImage("assets/bullet.png")
	self.x = x
	self.y = y
	self.speed = 700
	self.width = self.image:getWidth()
	self.height = self.image:getHeight()
end

function Bullet:update(dt)
	self.y = self.y + self.speed * dt
end

function Bullet:draw()
	love.graphics.draw(self.image, self.x, self.y)
end

function Bullet:check_collision(obj)
	if Game.check_collision(self, obj) then
		self.dead = true
		obj.health = obj.health - 50
		if obj.speed > 0 then
			obj.speed = obj.speed + 50
		else
			obj.speed = obj.speed - 50
		end
	end
end

return Bullet
