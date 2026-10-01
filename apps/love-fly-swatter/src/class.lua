------------------------------------------------
--                    CLASS
-- is a parent of all classes
-- provides basic class functionality
------------------------------------------------

local Class = {}
Class.__index = Class;

-- default implementation
function Class:new() end

-- create a new Class type from our base class
function Class:derive(type)
    local _class = {} 
    _class["__call"] = Class.__call;
    _class.type = type;
    _class.__index = _class
    _class.parent = self;
    setmetatable(_class, self); -- sets the parent to this class
    return _class
end

-- call meta method
function Class:__call(...)
    local instance = setmetatable({}, self);
    instance:new(...);
    return instance;
end

-- get class type
function Class:get_type()
    return self.type;
end

return Class;