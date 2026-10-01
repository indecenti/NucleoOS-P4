-- images is a module that manages the set of available entity graphics

local images = {}

local items = {
	bat = {
		path = "images/entities/bat.png"
	},
	human = {
		path = "images/entities/human.png"
	},
	orc = {
		path = "images/entities/orc_green.png"
	},
	sword = {
		path = "images/entities/sword.png"
	}
}

function images.init()
	for id,data in pairs(items) do
		items[id].image = nil
	end
end

function images.get(id)
	if items[id].image == nil then
		items[id].image = love.graphics.newImage(items[id].path)
	end
    
	return items[id].image
end

return images