-- layout is a module that manages rectangular drawing areas and some drawing operations on them

local aspect = require("modules.aspect")

local layout = {}

layout.DRAWING_AREA_WORLD = 1
layout.DRAWING_AREA_STATS = 2
layout.DRAWING_AREA_ITEMS = 3
layout.DRAWING_AREA_LOG = 4

local AREA_PADDING = 0 --4

local rects = {}

function layout.init(gameWidth,gameHeight)
	-- column 1
	rects[layout.DRAWING_AREA_WORLD] = {
		x = AREA_PADDING,
		y = AREA_PADDING,
		width = gameHeight - AREA_PADDING * 2,
		height = gameHeight - AREA_PADDING * 2
	}

	-- column 2, row 1
	rects[layout.DRAWING_AREA_STATS] = {
		x = rects[layout.DRAWING_AREA_WORLD].width+AREA_PADDING*2,
		y = AREA_PADDING,
		width = gameWidth - rects[layout.DRAWING_AREA_WORLD].width - AREA_PADDING * 3,
		height = (gameHeight - AREA_PADDING * 3) * 0.2
	}

	-- column 2, row 2
	rects[layout.DRAWING_AREA_ITEMS] = {
		x = rects[layout.DRAWING_AREA_STATS].x,
		y = rects[layout.DRAWING_AREA_STATS].y + rects[layout.DRAWING_AREA_STATS].height + AREA_PADDING,
		width = gameWidth - rects[layout.DRAWING_AREA_WORLD].width - AREA_PADDING * 3,
		height = (gameHeight - AREA_PADDING * 3) * 0.6
	}
    
	-- column 2, row 3
	rects[layout.DRAWING_AREA_LOG] = {
		x = rects[layout.DRAWING_AREA_STATS].x,
		y = rects[layout.DRAWING_AREA_ITEMS].y + rects[layout.DRAWING_AREA_ITEMS].height + AREA_PADDING,
		width = rects[layout.DRAWING_AREA_STATS].width,
		height = gameHeight - (rects[layout.DRAWING_AREA_ITEMS].y + rects[layout.DRAWING_AREA_ITEMS].height) - AREA_PADDING * 2
	}
end

function layout.getRect(index)
	return rects[index]
end

function layout.enableClipping(rect)
	love.graphics.setScissor(rect.x*aspect.getScale(),rect.y*aspect.getScale(),rect.width*aspect.getScale(),rect.height*aspect.getScale())
end

function layout.disableClipping()
	love.graphics.setScissor()
end

function layout.drawBackground(rect,color)
    love.graphics.setColor(color)
	love.graphics.rectangle("fill",rect.x,rect.y,rect.width,rect.height)
end

return layout