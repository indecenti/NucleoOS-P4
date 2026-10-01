-- asteroids\queue.lua
-- Copyright (c) 2017-2018 Jon Thysell

-- Queue
local Queue = {}

function Queue:new()
    local o = {first = 0, last = -1}
    setmetatable(o, self)
    self.__index = self
    return o
end

function Queue:count()
    return (self.last - self.first) + 1
end

function Queue:enqueue(value)
    local last = self.last + 1
    self.last = last
    self[last] = value
end

function Queue:dequeue(value)
    if self:count() == 0 then error("queue is empty") end
    local first = self.first
    local value = self[first]
    self[first] = nil
    self.first = first + 1
    return value
end

return {
    Queue = Queue
}