-- grammar is a module that constructs messages

local entityManager = require("modules.ecs.managers.entitymanager")

local grammar = {}

grammar.STRUCT_E1_ATTACK_E2 = {{"e",1}, {"v",{"attack","attacks"}}, {"e",2}}
grammar.STRUCT_E1_CANNOT_DROP_E2_HERE = {{"e",1},{"v",{"cannot drop","cannot drop"}},{"e",2},{"f","here"}}
grammar.STRUCT_E1_DIE = {{"e",1}, {"v",{"die","dies"}}}
grammar.STRUCT_E1_DROP_E2 = {{"e",1}, {"v",{"drop","drops"}}, {"e",2}}
grammar.STRUCT_E1_GET_E2 = {{"e",1}, {"v",{"get","gets"}}, {"e",2}}
grammar.STRUCT_E1_LOSE_P2_HEALTH = {{"e",1},{"v",{"lose","loses"}},{"p",2},{"f","health"}}
grammar.STRUCT_E1_MISS = {{"e",1}, {"v",{"miss","misses"}}}
grammar.STRUCT_E1_TAKE_OFF_E2 = {{"e",1}, {"v",{"take off","takes off"}}, {"e",2}}
grammar.STRUCT_E1_WIELD_E2 = {{"e",1}, {"v",{"wield","wields"}}, {"e",2}}

local PERSON_INDEX_SECOND_SINGULAR = 1
local PERSON_INDEX_THIRD_SINGULAR = 2

-- given an entity, returns person and text
function grammar.resolveEntity(entity,leaveOutArticle)
	if entity == nil then
		return PERSON_INDEX_THIRD_SINGULAR,"[Grammar error: missing entity]"
	end

	if entityManager.entityHas(entity,{"input"}) then
		return PERSON_INDEX_SECOND_SINGULAR,"you"
    end

	if entityManager.entityHas(entity,{"name"}) then
		if entity.name.specificName ~= nil then
			return PERSON_INDEX_THIRD_SINGULAR,entity.name.specificName
		end

		if leaveOutArticle ~= true then
			return PERSON_INDEX_THIRD_SINGULAR,"the "..entity.name.genericName
        end

		return PERSON_INDEX_THIRD_SINGULAR,entity.name.genericName
	end

	return PERSON_INDEX_THIRD_SINGULAR,"unnamed entity" 
end

-- returns verb conjugation for provided person
local function resolveVerb(conjugations,person)
	if person == nil then
		return "[Grammar error: missing person]"
	end

	if conjugations[person] == nil then
		return "[Grammar error: missing conjugation]"
	end

	return conjugations[person]
end

-- returns sentence made by inserting parameters into struct
function grammar.interpolate(struct,parameters)
	if struct == nil then
		return "[Grammar error: unknown struct]"
	end

	local lastPerson = nil
	local words = {}

	for i = 1, #struct do
		-- entity expected on this position
		if struct[i][1] == "e" then
			local person, text = grammar.resolveEntity(parameters[struct[i][2]])
			table.insert(words,text)
			lastPerson = person
		-- verb expected on this position
		elseif struct[i][1] == "v" then
			table.insert(words,resolveVerb(struct[i][2],lastPerson))
		-- fixed phrase expected on this position
		elseif struct[i][1] == "f" then
			table.insert(words,struct[i][2])
		-- provided phrase expected on this position
		elseif struct[i][1] == "p" then
			table.insert(words,parameters[struct[i][2]])
		end
	end

	local sentence = table.concat(words," ")

	if sentence ~= "" then
		-- capitalize first character and add period
		sentence = sentence:gsub("^%l", string.upper).."."
	end
    
	return sentence
end

return grammar