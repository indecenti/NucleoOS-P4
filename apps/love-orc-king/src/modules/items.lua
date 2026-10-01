-- items is a module that displays information about the camera entity's items in the items area

local actionManager = require("modules.ecs.managers.actionmanager")
local colors = require("modules.colors")
local entityManager = require("modules.ecs.managers.entitymanager")
local Equipment = require("modules.ecs.components.equipment")
local grammar = require("modules.grammar")
local layout = require("modules.layout")

local items = {}

items.STATE_DEFAULT = 0
items.STATE_INVENTORY = 1
items.STATE_EQUIPMENT = 2
items.STATE_INSPECTION = 3

local BACKGROUND_COLOR = colors.get("BLACK_SOULS_5")
local TEXT_COLOR_DEFAULT = colors.get("BLACK_SOULS_15")
local TEXT_COLOR_KEY = colors.get("BLACK_SOULS_10")
local TEXT_COLOR_ITEM = colors.get("BLACK_SOULS_1")
local TEXT_COLOR_ITEM_COUNT = colors.get("BLACK_SOULS_1")
local SELECTED_COLOR = colors.get("BLACK_SOULS_7")

local LINE_HEIGHT = 16
local PADDING_LEFT = 4

local EQUIPMENT_COUNT_LABEL = "Equipped: "
local EQUIPMENT_COUNT_TEXT_NONE = "nothing"
local EQUIPMENT_EMPTY_SLOT_TEXT = "nothing"

local GROUND_ITEM_LABEL = "On ground: "
local GROUND_ITEM_TEXT_NONE = "nothing"

local INVENTORY_COUNT_LABEL = "Inventory: "
local INVENTORY_COUNT_TEXT_NONE = "empty"

local entity
local equipmentCount
local equipmentIndex
local groundItemName
local inspectedItem
local inventoryCount
local inventoryIndex
local rect
local state
local previousState

function items.init(drawingAreaIndex)
	rect = layout.getRect(drawingAreaIndex)
end

local function displayEquipment()
	-- display header
	local headerPrintTable = {
		TEXT_COLOR_DEFAULT, "Equipment (",
		TEXT_COLOR_KEY, "up",
		TEXT_COLOR_DEFAULT, "/",
		TEXT_COLOR_KEY, "down",
		TEXT_COLOR_DEFAULT, ", ",
		TEXT_COLOR_KEY, "esc",
		TEXT_COLOR_DEFAULT, " to exit)"
	}

	love.graphics.setColor(1,1,1)
	love.graphics.print(headerPrintTable,rect.x+PADDING_LEFT,rect.y)
	
	if entityManager.entityHas(entity,{"equipment"}) then
		if #entity.equipment.items > 0 then
			local selectedItem = nil

			-- display items
			for i = 1, #entity.equipment.items do
				-- current item is selected
				if equipmentIndex == i then
					selectedItem = entity.equipment.items[i]
					love.graphics.setColor(SELECTED_COLOR)
					love.graphics.rectangle("fill",rect.x+PADDING_LEFT/2,rect.y+LINE_HEIGHT*i-1,rect.width-PADDING_LEFT,LINE_HEIGHT)
					love.graphics.setColor(TEXT_COLOR_ITEM)
				end

				local equipmentPrintTable = {
					TEXT_COLOR_DEFAULT, entity.equipment.slots[i].name..": "
				}

				if entity.equipment.items[i] ~= Equipment.NULL then
					local _, text = grammar.resolveEntity(entity.equipment.items[i],true)

					table.insert(equipmentPrintTable,TEXT_COLOR_ITEM)
					table.insert(equipmentPrintTable,text)
				else
					table.insert(equipmentPrintTable,TEXT_COLOR_DEFAULT)
					table.insert(equipmentPrintTable,EQUIPMENT_EMPTY_SLOT_TEXT)
				end	

				love.graphics.setColor(1,1,1)
				love.graphics.print(equipmentPrintTable,rect.x+PADDING_LEFT,rect.y+LINE_HEIGHT*i-1)
			end

			-- display available actions for selected item
			if selectedItem ~= nil then
				local actions = {"drop","inspect"}

				if entityManager.entityHas(selectedItem,{"item"}) then
					table.insert(actions,"take off")
				end

				if #actions > 0 then
					-- create print table out of available actions
					local actionsPrintTable = {
						TEXT_COLOR_DEFAULT, "("
					}

					for i = 1, #actions do
						if i < #actions then
							actions[i] = actions[i]..", "
						else
							actions[i] = actions[i]..")"
						end

						table.insert(actionsPrintTable,TEXT_COLOR_KEY)
						table.insert(actionsPrintTable,string.sub(actions[i],1,1))
						table.insert(actionsPrintTable,TEXT_COLOR_DEFAULT)
						table.insert(actionsPrintTable,string.sub(actions[i],2))
					end

					love.graphics.setColor(1,1,1)
					love.graphics.print(actionsPrintTable,rect.x+PADDING_LEFT,rect.y+rect.height-LINE_HEIGHT)
				end
			end
		end
	end
end

local function displayEquipmentCount()
	if equipmentCount == 0 then
		love.graphics.setColor(TEXT_COLOR_DEFAULT)
		love.graphics.print(EQUIPMENT_COUNT_LABEL..EQUIPMENT_COUNT_TEXT_NONE,rect.x+PADDING_LEFT,rect.y+rect.height-LINE_HEIGHT*3)
	else
		local itemWord
		if equipmentCount ~= 1 then
			itemWord = "items"
		else
			itemWord = "item"
		end

		local equipmentCountPrintTable = {
			TEXT_COLOR_DEFAULT, EQUIPMENT_COUNT_LABEL,
			TEXT_COLOR_ITEM_COUNT, equipmentCount.." "..itemWord,
			TEXT_COLOR_DEFAULT, " (",
			TEXT_COLOR_KEY, "e",
			TEXT_COLOR_DEFAULT, "quipment)"
		}

		love.graphics.setColor(1,1,1)
		love.graphics.print(equipmentCountPrintTable,rect.x+PADDING_LEFT,rect.y+rect.height-LINE_HEIGHT*3)
	end
end

local function displayGroundItem()
	if groundItemName == nil then
		love.graphics.setColor(TEXT_COLOR_DEFAULT)
		love.graphics.print(GROUND_ITEM_LABEL..GROUND_ITEM_TEXT_NONE,rect.x+PADDING_LEFT,rect.y+rect.height-LINE_HEIGHT)
	else
		local groundItemPrintTable = {
			TEXT_COLOR_DEFAULT, GROUND_ITEM_LABEL,
			TEXT_COLOR_ITEM, groundItemName,
			TEXT_COLOR_DEFAULT, " (",
			TEXT_COLOR_KEY, "g",
			TEXT_COLOR_DEFAULT, "et)"
		}

		love.graphics.setColor(1,1,1)
		love.graphics.print(groundItemPrintTable,rect.x+PADDING_LEFT,rect.y+rect.height-LINE_HEIGHT)
	end
end

local function displayInspection()
	-- display header
	local headerPrintTable = {
		TEXT_COLOR_DEFAULT, "Inspection (",
		TEXT_COLOR_KEY, "esc",
		TEXT_COLOR_DEFAULT, " to exit)"
	}

	love.graphics.setColor(1,1,1)
	love.graphics.print(headerPrintTable,rect.x+PADDING_LEFT,rect.y)
		
	if inspectedItem ~= nil then
		local line = 2
	
		-- display item name
		local _, text = grammar.resolveEntity(inspectedItem,true)

		love.graphics.setColor(TEXT_COLOR_ITEM)
		love.graphics.print(text,rect.x+PADDING_LEFT,rect.y+(line-1)*LINE_HEIGHT)

		line = line + 1

		-- display item stats
		local statsPrintTable = {}

		if entityManager.entityHas(inspectedItem,{"stats"}) then
			-- strength
			if inspectedItem.stats.strength ~= nil then
				local sign = ""

				if inspectedItem.stats.strength > 0 then
					sign = "+"
				end

				table.insert(statsPrintTable,TEXT_COLOR_ITEM)
				table.insert(statsPrintTable,sign..inspectedItem.stats.strength)
				table.insert(statsPrintTable,TEXT_COLOR_DEFAULT)
				table.insert(statsPrintTable," str")
			end

			-- damage
			if inspectedItem.stats.damage ~= nil then
				local sign = ""

				if inspectedItem.stats.damage > 0 then
					sign = "+"
				end

				if #statsPrintTable > 0 then
					table.insert(statsPrintTable,TEXT_COLOR_DEFAULT)
					table.insert(statsPrintTable,", ")
				end

				table.insert(statsPrintTable,TEXT_COLOR_ITEM)
				table.insert(statsPrintTable,sign..inspectedItem.stats.damage)
				table.insert(statsPrintTable,TEXT_COLOR_DEFAULT)
				table.insert(statsPrintTable," dmg")
			end
		end

		if #statsPrintTable > 0 then
			love.graphics.setColor(1,1,1)
			love.graphics.print(statsPrintTable,rect.x+PADDING_LEFT,rect.y+(line-1)*LINE_HEIGHT)

			line = line + 1
		end

		-- display description
		if entityManager.entityHas(inspectedItem,{"description"}) then
			width, lines = love.graphics.getFont():getWrap(inspectedItem.description.text,rect.width-PADDING_LEFT)

			love.graphics.setColor(TEXT_COLOR_DEFAULT)

			for i = 1, #lines do
				love.graphics.print(lines[i],rect.x+PADDING_LEFT,rect.y+(line-1)*LINE_HEIGHT)

				line = line + 1
			end
		end
	end
end

local function displayInventory()
	-- display header
	local headerPrintTable = {
		TEXT_COLOR_DEFAULT, "Inventory (",
		TEXT_COLOR_KEY, "up",
		TEXT_COLOR_DEFAULT, "/",
		TEXT_COLOR_KEY, "down",
		TEXT_COLOR_DEFAULT, ", ",
		TEXT_COLOR_KEY, "esc",
		TEXT_COLOR_DEFAULT, " to exit)"
	}

	love.graphics.setColor(1,1,1)
	love.graphics.print(headerPrintTable,rect.x+PADDING_LEFT,rect.y)
	
	if entityManager.entityHas(entity,{"inventory"}) then
		if #entity.inventory.items > 0 then
			local selectedItem = nil

			-- display items
			love.graphics.setColor(TEXT_COLOR_ITEM)

			for i = 1, #entity.inventory.items do
				-- current item is selected
				if inventoryIndex == i then
					selectedItem = entity.inventory.items[i]

					love.graphics.setColor(SELECTED_COLOR)
					love.graphics.rectangle("fill",rect.x+PADDING_LEFT/2,rect.y+LINE_HEIGHT*i-1,rect.width-PADDING_LEFT,LINE_HEIGHT)
					love.graphics.setColor(TEXT_COLOR_ITEM)
				end

				local _, text = grammar.resolveEntity(entity.inventory.items[i],true)

				love.graphics.print(text,rect.x+PADDING_LEFT,rect.y+LINE_HEIGHT*i-1)
			end

			-- display available actions for selected item
			if selectedItem ~= nil then
				local actions = {"drop","inspect"}

				if entityManager.entityHas(selectedItem,{"item"}) then
					if selectedItem.item.wieldable then
						table.insert(actions,"wield")
					end

					if selectedItem.item.wearable then
						table.insert(actions,"wear")
					end	
				end

				if #actions > 0 then
					-- create print table out of available actions
					local actionsPrintTable = {
						TEXT_COLOR_DEFAULT, "("
					}

					for i = 1, #actions do
						if i < #actions then
							actions[i] = actions[i]..", "
						else
							actions[i] = actions[i]..")"
						end

						table.insert(actionsPrintTable,TEXT_COLOR_KEY)
						table.insert(actionsPrintTable,string.sub(actions[i],1,1))
						table.insert(actionsPrintTable,TEXT_COLOR_DEFAULT)
						table.insert(actionsPrintTable,string.sub(actions[i],2))
					end

					love.graphics.setColor(1,1,1)
					love.graphics.print(actionsPrintTable,rect.x+PADDING_LEFT,rect.y+rect.height-LINE_HEIGHT)
				end
			end
		end
	end
end

local function displayInventoryCount()
	if inventoryCount == 0 then
		love.graphics.setColor(TEXT_COLOR_DEFAULT)
		love.graphics.print(INVENTORY_COUNT_LABEL..INVENTORY_COUNT_TEXT_NONE,rect.x+PADDING_LEFT,rect.y+rect.height-LINE_HEIGHT*2)
	else
		local itemWord

		if inventoryCount ~= 1 then
			itemWord = "items"
		else
			itemWord = "item"
		end

		local inventoryCountPrintTable = {
			TEXT_COLOR_DEFAULT, INVENTORY_COUNT_LABEL,
			TEXT_COLOR_ITEM_COUNT, inventoryCount.." "..itemWord,
			TEXT_COLOR_DEFAULT, " (",
			TEXT_COLOR_KEY, "i",
			TEXT_COLOR_DEFAULT, "nventory)"
		}

		love.graphics.setColor(1,1,1)
		love.graphics.print(inventoryCountPrintTable,rect.x+PADDING_LEFT,rect.y+rect.height-LINE_HEIGHT*2)
	end
end

function items.draw()
	layout.drawBackground(rect,BACKGROUND_COLOR)
	layout.enableClipping(rect)

	if state == items.STATE_DEFAULT then
		displayEquipmentCount()
		displayGroundItem()
		displayInventoryCount()
	elseif state == items.STATE_EQUIPMENT then
		displayEquipment()
	elseif state == items.STATE_INSPECTION then
		displayInspection()
	elseif state == items.STATE_INVENTORY then
		displayInventory()
	end
	
	layout.disableClipping()
end

function items.reset()
	entity = nil
	equipmentCount = 0
	equipmentIndex = 0
	groundItemName = nil
	inspectedItem = nil
	inventoryCount = 0
	inventoryIndex = 1
	state = nil
	previousState = nil
	items.switchToState(items.STATE_DEFAULT)
end

-- returns whether key was processed, resulting action, and new key listener
function items.keyListenerEquipment(key)
	local processed = false
	local action = nil
	local listener = items.keyListenerEquipment

	if key == "escape" then
		listener = items.switchToState(items.STATE_DEFAULT)
		processed = true
	elseif entityManager.entityHas(entity,{"equipment"}) then
		if ((key == "up") or (key == "kp8")) and (equipmentIndex > 1) then
			equipmentIndex = equipmentIndex - 1
			processed = true
		elseif ((key == "down") or (key == "kp2")) and (equipmentIndex < #entity.equipment.items) then
			equipmentIndex = equipmentIndex + 1
			processed = true
		elseif key == "d" then
			action = actionManager.createAction("drop",{
				item = entity.equipment.items[equipmentIndex]
			})

			-- exit equipment if dropping single remaining item
			if (equipmentCount == 1) then
				listener = items.switchToState(items.STATE_DEFAULT)
			end

			processed = true
		elseif key == "i" then
			inspectedItem = entity.equipment.items[equipmentIndex]
			listener = items.switchToState(items.STATE_INSPECTION)
			processed = true
		elseif key == "t" then
			action = actionManager.createAction("takeoff",{
				item = entity.equipment.items[equipmentIndex]
			})

			-- exit inventory if taking off single remaining item
			if equipmentCount == 1 then
				listener = items.switchToState(items.STATE_DEFAULT)
			end

			processed = true
		end
	end

	return processed, action, listener
end

-- returns whether key was processed, resulting action, and new key listener
function items.keyListenerInspection(key)
	local processed = false
	local action = nil
	local listener = items.keyListenerInspection

	if key == "escape" then
		listener = items.switchToState(previousState)
		processed = true
	end

	return processed, action, listener
end

-- returns whether key was processed, resulting action, and new key listener
function items.keyListenerInventory(key)
	local processed = false
	local action = nil
	local listener = items.keyListenerInventory

	if key == "escape" then
		listener = items.switchToState(items.STATE_DEFAULT)
		processed = true
	elseif entityManager.entityHas(entity,{"inventory"}) then
		if ((key == "up") or (key == "kp8")) and (inventoryIndex > 1) then
			inventoryIndex = inventoryIndex - 1
			processed = true
		elseif ((key == "down") or (key == "kp2")) and (inventoryIndex < #entity.inventory.items) then
			inventoryIndex = inventoryIndex + 1
			processed = true
		elseif key == "d" then
			action = actionManager.createAction("drop",{
				item = entity.inventory.items[inventoryIndex]
			})

			-- move pointer up if removing bottom item in list
			if inventoryIndex == #entity.inventory.items then
				inventoryIndex = inventoryIndex - 1
			end

			-- exit inventory if dropping single remaining item
			if #entity.inventory.items == 1 then
				listener = items.switchToState(items.STATE_DEFAULT)
			end

			processed = true
		elseif key == "i" then
			inspectedItem = entity.inventory.items[inventoryIndex]
			listener = items.switchToState(items.STATE_INSPECTION)
			processed = true
		elseif key == "w" then
			action = actionManager.createAction("wield",{
				item = entity.inventory.items[inventoryIndex]
			})

			-- exit inventory if wielding single remaining item
			if #entity.inventory.items == 1 then
				listener = items.switchToState(items.STATE_DEFAULT)
			end

			processed = true
		end
	end

	return processed, action, listener
end

-- returns new key listener to be used in input module
function items.switchToState(newState)
	previousState = state
	state = newState

	if state == items.STATE_EQUIPMENT then
		equipmentIndex = 1

		return items.keyListenerEquipment
    end

	if state == items.STATE_INSPECTION then
		return items.keyListenerInspection
    end

	if state == items.STATE_INVENTORY then
		inventoryIndex = 1

		return items.keyListenerInventory
    end

	return nil
end

function items.update(targetEntity)
	entity = targetEntity

	if entity ~= nil then
		-- update ground item name
		local groundItem = entityManager.getEntityAtLocationHaving(entity.position.x,entity.position.y,entity.position.z,{"item"})

		if groundItem == nil then
			groundItemName = nil
		else
			if entityManager.entityHas(groundItem,{"name"}) then
				if groundItem.name.specificName ~= nil then
					groundItemName = groundItem.name.specificName
				else
					groundItemName = groundItem.name.genericName
				end
			else
				groundItemName = "unnamed item"
			end
		end

		-- update inventory count
		if entityManager.entityHas(entity,{"inventory"}) then
			inventoryCount = #entity.inventory.items
		else
			inventoryCount = 0
		end

		if inventoryIndex > inventoryCount then
			inventoryIndex = inventoryCount
		end

		-- update equipment count
		equipmentCount = 0
        
		if entityManager.entityHas(entity,{"equipment"}) then
			for i = 1, #entity.equipment.items do
				if entity.equipment.items[i] ~= Equipment.NULL then
					equipmentCount = equipmentCount + 1
				end
			end
		end
	else
		items.reset()
	end
end

return items