-- log is a module that manages and displays the most recent game messages

local colors = require("modules.colors")
local entityManager = require("modules.ecs.managers.entitymanager")
local layout = require("modules.layout")

local log = {}

log.TEXT_COLOR_DEFAULT = colors.get("BLACK_SOULS_2")
log.TEXT_COLOR_DANGER = colors.get("BLACK_SOULS_13")

local BACKGROUND_COLOR = colors.get("BLACK_SOULS_15")
local LINE_HEIGHT = 16
local PADDING_LEFT = 4

local entries
local rect

function log.init(drawingAreaIndex)
	rect = layout.getRect(drawingAreaIndex)
end

local function scroll()
	-- scrolling currently removes old log entries - this is a simple implementation,
	-- a more complex approach could involve retaining all entries but displaying a
	-- selected range, modified by scrolling - also allowing player to scroll up/down
	local maxLineCount = math.floor(rect.height / LINE_HEIGHT)

	while #entries > maxLineCount do
		table.remove(entries,1)
	end
end

function log.addEntry(text,color)
	if color == nil then
		color = log.TEXT_COLOR_DEFAULT
	end

	width, lines = love.graphics.getFont():getWrap(text,rect.width-PADDING_LEFT)
	
    for i = 1, #lines do
		table.insert(entries,{
			text = lines[i],
			color = color
		})
	end

	scroll()
end

function log.draw()
	layout.drawBackground(rect,BACKGROUND_COLOR)
	layout.enableClipping(rect)

	for i = 1, #entries do
		love.graphics.setColor(entries[i].color)
		love.graphics.print(entries[i].text,rect.x+PADDING_LEFT,rect.y+LINE_HEIGHT*(i-1))
	end
    
	layout.disableClipping()
end

function log.clear()
	entries = {}
end

return log