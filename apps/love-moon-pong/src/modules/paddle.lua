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

local function drawPaddle(self)
	love.graphics.rectangle('fill', self.x, self.y, self.width, self.height)
end

-- module

local paddleModule = {defaultWidth = 6, defaultHeight = 80}

function paddleModule:new(x)
	local paddle =
	{
		x = x,
		y = 0,

		width = self.defaultWidth,
		height = self.defaultHeight,

		speed = 2.5,

		draw = drawPaddle,
	}

	paddle.y = ((love.graphics.getHeight() - paddle.height) / 2)

	return paddle
end

return paddleModule