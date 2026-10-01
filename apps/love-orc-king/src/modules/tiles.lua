-- tiles is a module that manages the set of available tiles

local tiles = {}

local items = {
	floor = {
		path = "images/tiles/floor.png"
	},
	floorCorridor = {
		path = "images/tiles/floor_corridor.png"
	},
	wallFront = {
		path = "images/tiles/wall_front.png"
	},
	wallTop = {
		path = "images/tiles/wall_top.png"
	}
}

function tiles.init()
	for id,data in pairs(items) do
		items[id].image = nil
	end
end

function tiles.draw(id,x,y,visible)
	if items[id].image == nil then
		items[id].image = love.graphics.newImage(items[id].path)
	end

	if visible then
		love.graphics.setColor(1,1,1)
	else
		love.graphics.setColor(0.3,0.3,0.3)
	end
    
	love.graphics.draw(items[id].image,x,y)
end

return tiles