local Enemy = Object:extend()

function Enemy:new()
	self.image = love.graphics.newImage("assets/snake.png")
	self.x = 325
	self.y = 450
	self.speed = 100
	self.width = self.image:getWidth()
	self.height = self.image:getHeight()
	self.health = 1000
end

function Enemy:update(dt)
	self.x = self.x + self.speed * dt
	local window_width = love.graphics.getWidth()
	if self.x < 0 then
		self.x = 0
		self.speed = -self.speed
	elseif self.x + self.width > window_width then
		self.x = window_width - self.width
		self.speed = -self.speed
	end
	-- toggle x speed direction
end

function Enemy:draw()
	if self.health > 0 then
		love.graphics.draw(self.image, self.x, self.y)
	end
end

return Enemy
