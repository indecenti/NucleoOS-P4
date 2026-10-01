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

local function aiPaddleUpdate(self, paddle, ball)
	if (paddle.y + (paddle.height / 2) > ball.y and paddle.y > 0) then
		local var = math.random(self.difficulty)
		if(var < self.difficulty) then
			paddle.y = paddle.y - paddle.speed
		end
	end

	if (paddle.y + (paddle.height / 2) < ball.y and paddle.y < (love.graphics.getHeight() - 80)) then
		paddle.y = paddle.y + paddle.speed
	end
end

-- module

local aiPaddleLogicModule = {}

function aiPaddleLogicModule:new(difficulty)
	local aiPaddleLogic =
	{
		difficulty = difficulty,

		update = aiPaddleUpdate,
	}

	return aiPaddleLogic
end

return aiPaddleLogicModule