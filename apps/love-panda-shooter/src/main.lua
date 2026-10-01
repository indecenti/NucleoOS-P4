---@class love
local love = require("love")

Game = {
	state = "playing",
}

---@module "player"
local player
---@module "enemy"
local enemy
---@type number
local screenWidth
---@type number
local screenHeight
---@type love.Font
local font

function Game:reset()
	self.state = "playing"
end

function Game.load()
	Object = require("classic")
	Player = require("player")
	Enemy = require("enemy")
	Bullet = require("bullet")
	font = love.graphics.newFont("assets/FantasqueSansMono-Regular.ttf", 24, "light")

	player = Player()
	enemy = Enemy()
	ListOfBullets = {}
	screenWidth = love.graphics.getWidth()
	screenHeight = love.graphics.getHeight()
end

---Key pressed event handler
---@param key love.KeyConstant
---
---@see love.keypressed
function Game.keypressed(key)
	player:keypressed(key)
end

---Game update function
---@param dt number
function Game.update(dt)
	if Game.state ~= "playing" then
		return
	end
	player:update(dt)
	enemy:update(dt)

	for i, v in ipairs(ListOfBullets) do
		v:update(dt)
		v:check_collision(enemy)

		if v.dead then
			table.remove(ListOfBullets, i)
		end
	end

	if enemy.health <= 0 then
		Game.state = "victory"
	end
end

--- Game draw function
function Game.draw()
	if Game.state == "playing" then
		player:draw()
		enemy:draw()

		for _, v in ipairs(ListOfBullets) do
			v:draw()
		end
		local healthText = love.graphics.newText(font, "Boss Health: " .. enemy.health)
		love.graphics.draw(healthText, 0, 0)
	elseif Game.state == "victory" then
		love.graphics.clear(0, 0, 0, 0.8, true, true)
		love.graphics.setColor(1, 0.85, 0.1) -- gold
		love.graphics.setFont(font)
		-- local victoryText = love.graphics.newText(font, "Victory!!!")
		love.graphics.printf("Victory!!!", 0, screenHeight / 2 - 20, screenWidth, "center")
		love.graphics.setColor(1, 1, 1)
	end
end

--- Checks collosion between two rectangles
---@param a table
---@param b table
function Game.check_collision(a, b)
	local a_left = a.x
	local a_right = a.x + a.width
	local a_top = a.y
	local a_bottom = a.y + a.height

	local b_left = b.x
	local b_right = b.x + b.width
	local b_top = b.y
	local b_bottom = b.y + b.height

	return a_right > b_left and a_left < b_right and a_bottom > b_top and a_top < b_bottom
end

--- Bind game functions to love bindings
love.load = Game.load
love.keypressed = Game.keypressed
love.update = Game.update
love.draw = Game.draw
