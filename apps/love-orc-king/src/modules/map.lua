-- map is a module for creating dungeon layouts with initial entity locations

local map = {}

-- Note: room dimensions do not include walls
-- Note: margin is to allow for corridors parallel to room walls with one tile inbetween
local ROOM_GRID_WIDTH = 4
local ROOM_GRID_HEIGHT = 4
local ROOM_GRID_MARGIN = 5
local MIN_ROOM_WIDTH = 8
local MIN_ROOM_HEIGHT = 8
local MAX_ROOM_WIDTH = 20
local MAX_ROOM_HEIGHT = 20

-- clockwise directions, starting with up
local DIRECTIONS = {
	{dX = 0, dY = -1},
	{dX = 1, dY = -1},
	{dX = 1, dY = 0},
	{dX = 1, dY = 1},
	{dX = 0, dY = 1},
	{dX = -1, dY = 1},
	{dX = -1, dY = 0},
	{dX = -1, dY = -1}
}

-- connect each room to only its east and south neighbour to avoid double corridors
local CONNECTION_DIRECTION_INDEXES = {3,5}

-- number of rooms * (max room width + 2 walls) + margins between rooms
map.WORLD_WIDTH = ROOM_GRID_WIDTH * (MAX_ROOM_WIDTH + 2) + (ROOM_GRID_WIDTH - 1) * ROOM_GRID_MARGIN
map.WORLD_HEIGHT = ROOM_GRID_HEIGHT * (MAX_ROOM_HEIGHT + 2) + (ROOM_GRID_HEIGHT - 1) * ROOM_GRID_MARGIN

-- returns table containing id and random location within room
local function getEntityData(id,x1,y1,width,height)
	return {
		id = id,
		x = math.random(x1,x1 + width - 1),
		y = math.random(y1,y1 + height - 1)
	}
end

-- returns location and dimensions of rooms and the entities in them
local function createRooms()
	local rooms = {}

	for gridY = 1, ROOM_GRID_HEIGHT do
		rooms[gridY] = {}

		for gridX = 1, ROOM_GRID_WIDTH do
			local width = math.random(MIN_ROOM_WIDTH,MAX_ROOM_WIDTH)
			local height = math.random(MIN_ROOM_HEIGHT,MAX_ROOM_HEIGHT)
			local x1 = (gridX - 1) * (MAX_ROOM_WIDTH + 2 + ROOM_GRID_MARGIN) + math.random(2, 2 + MAX_ROOM_WIDTH - width)
			local y1 = (gridY - 1) * (MAX_ROOM_HEIGHT + 2 + ROOM_GRID_MARGIN) + math.random(2, 2 + MAX_ROOM_HEIGHT - height)

			rooms[gridY][gridX] = {
				x1 = x1,
				y1 = y1,
				width = width,
				height = height,
				entities = {}
			}

			-- each room contains 0-3 bats
			for i = 1, 3 do
				if math.random(3) == 1 then
					table.insert(rooms[gridY][gridX].entities,getEntityData("bat",x1,y1,width,height))
				end
			end

			-- each room contains 0-2 orcs
			for i = 1, 2 do
				if math.random(5) == 1 then
					table.insert(rooms[gridY][gridX].entities,getEntityData("orc",x1,y1,width,height))
				end
			end

			-- upper left room contains two swords
			if (gridY == 1) and (gridX == 1) then
				table.insert(rooms[gridY][gridX].entities,getEntityData("sword",x1,y1,width,height))
				table.insert(rooms[gridY][gridX].entities,getEntityData("sword",x1,y1,width,height))
			end

			-- upper left room contains hero
			-- Note: later entity planned at same location will overwrite previous entity, therefore hero planned last
			if (gridY == 1) and (gridX == 1) then
				table.insert(rooms[gridY][gridX].entities,getEntityData("hero",x1,y1,width,height))
			end
		end
	end
	return rooms
end

-- returns definitions of corridors connecting all rooms
local function createConnections(rooms)
	local connections = {}

	for gridY = 1, ROOM_GRID_HEIGHT do
		connections[gridY] = {}

		for gridX = 1, ROOM_GRID_WIDTH do
			connections[gridY][gridX] = {}

			for i = 1, #CONNECTION_DIRECTION_INDEXES do
				local dir = DIRECTIONS[CONNECTION_DIRECTION_INDEXES[i]]
				local targetGridX = gridX + dir.dX
				local targetGridY = gridY + dir.dY

				if (targetGridX >= 1) and (targetGridX <= ROOM_GRID_WIDTH) then
					if (targetGridY >= 1) and (targetGridY <= ROOM_GRID_HEIGHT) then
						local source = {
							x = nil,
							y = nil
						}

						local dest = {
							x = nil,
							y = nil
						}

						local bend = nil

						-- vertical
						if dir.dX == 0 then
							source.x = math.random(rooms[gridY][gridX].x1, rooms[gridY][gridX].x1 + rooms[gridY][gridX].width - 1)
							dest.x = math.random(rooms[targetGridY][targetGridX].x1, rooms[targetGridY][targetGridX].x1 + rooms[targetGridY][targetGridX].width - 1)

							-- up
							if dir.dY == -1 then
								source.y = rooms[gridY][gridX].y1 - 1
								dest.y = rooms[targetGridY][targetGridX].y1 + rooms[targetGridY][targetGridX].height
							-- down
							else
								source.y = rooms[gridY][gridX].y1 + rooms[gridY][gridX].height
								dest.y = rooms[targetGridY][targetGridX].y1 - 1
							end

							-- bend needed
							if source.x ~= dest.x then
								-- bend is understood to be defined as its vertical location along corridor
								-- Note: always have at least 3 tiles of margin to avoid double layers of wall
								bend = math.random(source.y + dir.dY * 3, dest.y - dir.dY * 3)
							end
						-- horizontal
						else
							source.y = math.random(rooms[gridY][gridX].y1, rooms[gridY][gridX].y1 + rooms[gridY][gridX].height - 1)
							dest.y = math.random(rooms[targetGridY][targetGridX].y1, rooms[targetGridY][targetGridX].y1 + rooms[targetGridY][targetGridX].height - 1)

							-- left
							if dir.dX == -1 then
								source.x = rooms[gridY][gridX].x1 - 1
								dest.x = rooms[targetGridY][targetGridX].x1 + rooms[targetGridY][targetGridX].width
							-- right
							else
								source.x = rooms[gridY][gridX].x1 + rooms[gridY][gridX].width
								dest.x = rooms[targetGridY][targetGridX].x1 - 1
							end

							-- bend needed
							if source.y ~= dest.y then
								-- bend is understood to be defined as its horizontal location along corridor
								-- Note: always have at least 3 tiles of margin to avoid double layers of wall
								bend = math.random(source.x + dir.dX * 3, dest.x - dir.dX * 3)
							end
						end

						table.insert(connections[gridY][gridX],{
							source = source,
							dest = dest,
							bend = bend,
							dirIndex = CONNECTION_DIRECTION_INDEXES[i]
						})
					end
				end
			end
		end
	end
    
	return connections
end

-- returns tile layout containing only empty values
local function createEmptyLayout()
	local layout = {}

	for x = 1, map.WORLD_WIDTH do
		for y = 1, map.WORLD_HEIGHT do
			local posIndex = (y - 1) * map.WORLD_WIDTH + x

			layout[posIndex] = {
				wall = 0,
				floor = 0,
				tile = nil,
				entities = {}
			}
		end
	end

	return layout
end

-- returns tile layout of dungeon level
function map.createLayout(level)
	local rooms = createRooms()
	local connections = createConnections(rooms)
	local layout = createEmptyLayout()

	-- add rooms
	for gridX = 1, ROOM_GRID_WIDTH do
		for gridY = 1, ROOM_GRID_HEIGHT do
			local room = rooms[gridY][gridX]

			for x = room.x1, room.x1 + room.width - 1 do
				for y = room.y1, room.y1 + room.height - 1 do
					local posIndex = (y - 1) * map.WORLD_WIDTH + x				
					layout[posIndex].floor = 1
					layout[posIndex].tile = "floor"
				end
			end

			-- add entities
			-- Note: later entity planned at same location will overwrite previous entity
			for i = 1, #room.entities do
				local entity = room.entities[i]
				local posIndex = (entity.y - 1) * map.WORLD_WIDTH + entity.x

				table.insert(layout[posIndex].entities,{
					id = entity.id,
					data = {
						position = {
							x = entity.x,
							y = entity.y,
							z = level
						}
					}
				})
			end
		end
	end

	-- add corridors
	for gridX = 1, ROOM_GRID_WIDTH do
		for gridY = 1, ROOM_GRID_HEIGHT do
			local roomConnections = connections[gridY][gridX]

			for i = 1, #roomConnections do
				local connection = roomConnections[i]
				local dX = DIRECTIONS[connection.dirIndex].dX
				local dY = DIRECTIONS[connection.dirIndex].dY
				local x = connection.source.x
				local y = connection.source.y
				local posIndex = (y - 1) * map.WORLD_WIDTH + x				
				layout[posIndex].floor = 1
				layout[posIndex].tile = "floorCorridor"

				repeat
					-- corridor is straight
					if connection.bend == nil then
						x = x + dX
						y = y + dY
					-- corridor has bend
					else
						-- corridor is vertical
						if dX == 0 then
							-- currently in bend
							if y == connection.bend then
								-- end of bend reached
								if x == connection.dest.x then
									y = y + dY
								-- end of bend not reached
								else
									if x < connection.dest.x then
										x = x + 1
									else
										x = x - 1
									end
								end
							-- not currently in bend
							else
								x = x + dX
								y = y + dY
							end
						-- corridor is horizontal
						else
							-- currently in bend
							if x == connection.bend then
								-- end of bend reached
								if y == connection.dest.y then
									x = x + dX
								-- end of bend not reached
								else
									if y < connection.dest.y then
										y = y + 1
									else
										y = y - 1
									end
								end
							-- not currently in bend
							else
								x = x + dX
								y = y + dY
							end
						end
					end

					posIndex = (y - 1) * map.WORLD_WIDTH + x				
					layout[posIndex].floor = 1
					layout[posIndex].tile = "floorCorridor"
				until (x == connection.dest.x) and (y == connection.dest.y)
			end
		end
	end

	-- add walls
	for y = 1, map.WORLD_HEIGHT do
		for x = 1, map.WORLD_WIDTH do
			local posIndex = (y - 1) * map.WORLD_WIDTH + x

			-- location is empty
			if (layout[posIndex].wall == 0) and (layout[posIndex].floor == 0) then
				local foundFloor = false

				for i = 1, #DIRECTIONS do
					local checkX = x + DIRECTIONS[i].dX
					local checkY = y + DIRECTIONS[i].dY

					-- check location is within layout
					if (checkX >= 1) and (checkX <= map.WORLD_WIDTH) then
						if (checkY >= 1) and (checkY <= map.WORLD_HEIGHT) then
							-- check location contains floor
							if layout[(checkY - 1) * map.WORLD_WIDTH + checkX].floor == 1 then
								foundFloor = true

								break
							end
						end
					end
				end

				if foundFloor then
					layout[posIndex].wall = 1
				end
			end
		end
	end

	-- set correct wall tiles
	for y = 1, map.WORLD_HEIGHT do
		for x = 1, map.WORLD_WIDTH do
			local posIndex = (y - 1) * map.WORLD_WIDTH + x
            
			-- location is wall
			if layout[posIndex].wall == 1 then
				local checkX = x
				local checkY = y + 1

				-- check location is not within layout
				if checkY > map.WORLD_HEIGHT then
					layout[posIndex].tile = "wallTop"
				else
					-- check location contains wall
					if layout[(checkY - 1) * map.WORLD_WIDTH + checkX].wall == 1 then
						layout[posIndex].tile = "wallTop"
					else
						layout[posIndex].tile = "wallFront"
					end
				end				
			end
		end
	end

	return layout
end

return map