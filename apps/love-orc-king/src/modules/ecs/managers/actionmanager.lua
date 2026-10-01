-- actionmanager is a module managing action definitions

local actionManager = {}

local definitions = {
	attack = require("modules.ecs.actions.attack"),
	drop = require("modules.ecs.actions.drop"),
	get = require("modules.ecs.actions.get"),
	move = require("modules.ecs.actions.move"),
	skip = require("modules.ecs.actions.skip"),
	takeoff = require("modules.ecs.actions.takeoff"),
	wield = require("modules.ecs.actions.wield")
}

function actionManager.createAction(id,data)
	return definitions[id].create(data)
end

return actionManager