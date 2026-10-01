-- input is a module managing aspects of player input leading to actions

local actionManager = require("modules.ecs.managers.actionmanager")
local items = require("modules.items")

local input = {}

local DIRECTIONS = {
	up = {
		dY = -1
	},
	down = {
		dY = 1
	},
	left = {
		dX = -1
	},
	right = {
		dX = 1
	},
	kp8 = {
		dY = -1
	},
	kp9 = {
		dX = 1,
		dY = -1
	},
	kp6 = {
		dX = 1
	},
	kp3 = {
		dX = 1,
		dY = 1
	},
	kp2 = {
		dY = 1
	},
	kp1 = {
		dX = -1,
		dY = 1
	},
	kp4 = {
		dX = -1
	},
	kp7 = {
		dX = -1,
		dY = -1
	}
}

local action = nil
local listener = nil

-- returns whether key was processed, resulting action, and new key listener
function input.defaultListener(key)
	local processed = false
	local action = nil
	local newListener = nil
	
	if DIRECTIONS[key] ~= nil then
		action = actionManager.createAction("move",DIRECTIONS[key])
		processed = true
	elseif key == "e" then
		newListener = items.switchToState(items.STATE_EQUIPMENT)
		processed = true
	elseif key == "g" then
		action = actionManager.createAction("get")
		processed = true
	elseif key == "i" then
		newListener = items.switchToState(items.STATE_INVENTORY)
		processed = true
	elseif key == "space" then
		action = actionManager.createAction("skip")
		processed = true
	end
	
	return processed, action, newListener
end

function input.getAction()
	return action
end

function input.resetAction()
	action = nil
end

function input.resetListener()
	listener = input.defaultListener
end

-- returns whether key was processed
function input.registerKeyPressed(key)
	local keyProcessed, newAction, newListener = listener(key)

	if newAction ~= nil then
		action = newAction
	end

	if newListener ~= nil then
		listener = newListener
	else
		listener = input.defaultListener
	end
    
	return keyProcessed
end

return input