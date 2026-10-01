--[[
Copyright 2023 Om Rawaley

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
--]]

-- functions

local function bounce(self, direction)
	-- if direction is 0 then bounce horizontally
	if(direction == 0) then
		self.xVelocity = -self.xVelocity
		
	-- if direction is 1 then bounce vertically
	elseif(direction == 1) then
		self.yVelocity = -self.yVelocity
	end
end

local function reset(self, direction)
	self.x = ((love.graphics.getWidth() - self.radius) / 2)
	self.y = ((love.graphics.getHeight() - self.radius) / 2)

	self.xVelocity = self.speed * direction
	self.yVelocity = self.speed

	self.friction = 1
end

local function update(self)
	self.xVelocity = self.xVelocity / self.friction
	self.yVelocity = self.yVelocity / self.friction

	self.x = self.x + self.xVelocity
	self.y = self.y + self.yVelocity
end

local function draw(self, red, green, blue)
	love.graphics.setColor(red, green, blue)
	love.graphics.rectangle("fill", self.x, self.y, self.radius, self.radius)
	love.graphics.setColor(255, 255, 255)
end

-- module

local ballModule = {}

function ballModule:new()
	local ball =
	{
		x = 0,
		y = 0,

		xVelocity = 0,
		yVelocity = 0,

		-- as this is inverse friction, higher values mean less friction
		friction = 1,

		speed = 2.5,

		radius = 10,

		reset = reset,
		update = update,
		draw = draw,
		bounce = bounce,
	}

	ball.x = ((love.graphics.getWidth() - ball.radius) / 2)
	ball.y = ((love.graphics.getHeight() - ball.radius) / 2)

	ball.xVelocity = ball.speed
	ball.yVelocity = ball.speed

	return ball
end

return ballModule