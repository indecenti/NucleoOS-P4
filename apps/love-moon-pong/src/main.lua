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

local gameModule = require('modules.game')

local game = gameModule:new()

-- function to draw the dotted line in the center
local function draw_dotted_line(start_height, dot_width, dot_amount, gap)
	local y = start_height

	-- repeat for the amount of dots
	for i = 1, dot_amount do
		-- create a small rectangle in the horizontal center of the screen
		love.graphics.rectangle("fill", ((love.graphics.getWidth() - dot_width) / 2), y, dot_width, dot_width)

		-- increase the starting point for the next dot
		y = y + gap
	end
end

function love.load()
	math.randomseed(os.time())
end

function love.update(dt)
	-- update the game
	game:update(dt)
end

function love.keypressed(key)
	-- if esc is pressed then quit the program
	if(key == 'escape') then
		love.event.quit()
	end
end

function love.draw()
	-- draw the dotted line
	draw_dotted_line(8, 4, 25, 20)

	-- draw the game
	game:draw()

	game:drawScores()
end