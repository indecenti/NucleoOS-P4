Area = Object:extend()

function Area:new(room)
    self.room = room
    self.gameObjects = {}
end

function Area:update(dt)
    for i = #self.gameObjects, 1, -1 do
        local go = self.gameObjects[i]
        go:update(dt)
        if go.dead then
            table.remove(self.gameObjects, i)
            go:destroy()
        end
    end
end

function Area:draw()
    for _, go in ipairs(self.gameObjects) do go:draw() end
end

function Area:addGameObject(gameObject, x, y, opts)
    opts = opts or {}
    local go = _G[gameObject](self, x or 0, y or 0, opts)
    go.class = gameObject
    table.insert(self.gameObjects, go)
    return go
end

function Area:addPhysicsWorld(cellSize)
    self.world = Physics.newWorld(cellSize)
end

function Area:getGameObject(filterFn)
    local result = {}
    for _, go in ipairs(self.gameObjects) do
        if filterFn(go) then
            table.insert(result, go)
        end
    end
    return result
end

function Area:queryCircleArea(x, y, radius, objectTypes)
    local result = {}

    for _, go in ipairs(self.gameObject) do
        if M.any(objectTypes, go.class) then
            local d = Distance(x, y, go.x, go.y)
            if d <= radius then
                table.insert(result, go)
            end
        end
    end

    return result
end

function Area:getClosestObject(x, y, radius, objectTypes)
    local objects = self:queryCircleArea(x, y, radius, objectTypes)
    table.sort(objects, function(a, b)
        local dA = Distance2(x, y, a.x, a.y)
        local dB = Distance2(x, y, b.x, b.y)

        return dA < dB
    end)

    return objects[1]
end

function Area:destroy()
    for i = #self.gameObjects, 1, -1 do
        local go = self.gameObjects[i]
        go:destroy()
        table.remove(self.gameObjects, i)
    end
    self.gameObjects = {}
    self.world = nil
end