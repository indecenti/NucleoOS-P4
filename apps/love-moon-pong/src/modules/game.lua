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

-- imported modules

local playerModule = require('modules.player')
local paddleModule = require('modules.paddle')
local keyboardPaddleLogicModule = require('modules.keyboardPaddleLogic')
local aiPaddleLogicModule = require('modules.aiPaddleLogic')
local ballModule = require('modules.ball')

local ball = ballModule:new()

local player1 = playerModule:new()
player1.paddle = paddleModule:new(15)
player1.paddleLogic = keyboardPaddleLogicModule:new("up", "down")

local playerComputer = playerModule:new()
playerComputer.paddle = paddleModule:new(love.graphics.getWidth() - paddleModule.defaultWidth - 15)
playerComputer.paddleLogic = aiPaddleLogicModule:new(7)

local players = {player1, playerComputer}

local ballLoseFriction = 1.05

local player1Score, player2Score = 0, 0

local ballResetTimer = 0

local ballResetDirection = 0

-- functions

-- function to return collisions/intersections using AABB
local function intersect(x1, y1, w1, h1, x2, y2, w2, h2)
	return (x1 < x2 + w2) and (x2 < x1 + w1) and (y1 < y2 + h2) and (y2 < y1 + h1)
end

local function update(self, dt)
	-- loop through all players
	for i, player in ipairs(players) do
		-- check for collision between the current player and the ball and make the ball bounce if there is a collision
		if(intersect(player.paddle.x, player.paddle.y, player.paddle.width, player.paddle.height, ball.x, ball.y, ball.radius, ball.radius)) then
			ball:bounce(0)
		end

		-- update the current player
		player.paddleLogic:update(player.paddle, ball)
	end

	-- update the ball
	ball:update()

	-- if the ball is at the top or bottom of the screen
	if(ball.y <= 0) or (ball.y >= (love.graphics.getHeight() - ball.radius)) then
		-- bounce
		ball:bounce(1)
	end

	-- if the ball passes the left side of the screen
	if(ball.x <= 0) then
		-- add a point to player 2
		player2Score = player2Score + 1

		-- make the ball move in the right direction
		ballResetDirection = 1

		-- bounce the ball and increase the friction
		ball:bounce(0)
		ball.friction = ballLoseFriction
	end

	-- if the ball passes the right side of the screen
	if((ball.x >= (love.graphics.getWidth() - ball.radius))) then
		-- add a point to player 1
		player1Score = player1Score + 1

		-- make the ball move in the left direction
		ballResetDirection = -1

		-- bounce the ball and increase the friction
		ball:bounce(0)
		ball.friction = ballLoseFriction
	end

	-- if the friction is bigger than 1 i.e. one of the players lost
	if(ball.friction > 1) then
		-- start the timer
		ballResetTimer = ballResetTimer + dt

		-- if it has been one second
		if(ballResetTimer > 1) then
			-- reset the ball
			ball:reset(ballResetDirection)
			-- reset the timer
			ballResetTimer = 0
		end
	end
end

local function draw(self)
	-- loop through all players
	for _, player in ipairs(players) do
		-- draw the current player
		player.paddle:draw()
	end

	-- draw the ball
	ball:draw(255, 255, 255)
end

local function drawScores(self)
	-- set the font size to 40
	love.graphics.setNewFont(40)

	-- print the scores with a space in between
	love.graphics.print(player1Score .. "        " .. player2Score, 240, 10)
end

-- module

local gameModule = {}

function gameModule.new()
	local game =
	{
		update = update,
		draw = draw,
		drawScores = drawScores,
	}

	return game
end

return gameModule