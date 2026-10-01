------------------------------------------------
--                   ENTITY
-- used as parent for other entity objects
------------------------------------------------

-- imports
local Class = require('class');
local Vector2 = require('classes.math.vector2');
local Sprite = require('classes.graphics.sprite');

local Entity = Class:derive("Entity");

-- entity constructor
function Entity:new(sprite_sheet, width, height, x, y, origin_x, origin_y)
    self.position = Vector2(x, y);
    self.size = Vector2(width, height);
    self.origin = Vector2(origin_x or 0, origin_y or 0);
    self.state = 1; -- 0: deactive, 1: active
    
    self.sprite = Sprite(sprite_sheet, self.size.x, self.size.y, self.position.x, self.position.y, 1, 1, 0, self.origin.x, self.origin.y);
end

-- entity draw event
function Entity:draw()
    -- set sprite position to position of entity
    self.sprite.position.x = self.position.x;
    self.sprite.position.y = self.position.y;

    -- draw sprite
    self.sprite:draw();
end

-- set entity position
function Entity:set_position(x, y)
    self.position.x = x;
    self.position.y = y;
end

return Entity;