------------------------------------------------
--                   CANVAS
-- used as parent for other canvas objects
------------------------------------------------

-- imports
local Class = require('class');
local Vector2 = require('classes.math.vector2');

local Canvas = Class:derive("Canvas");

-- canvas constructor
function Canvas:new(x, y, width, height, scale_x, scale_y)
    self.position = Vector2(x, y)
    self.size = Vector2(width, height);
    self.scale = Vector2(scale_x or 1, scale_y or 1);

    self.canvas = love.graphics.newCanvas(self.size.x, self.size.y);
end

-- canvas draw event
function Canvas:draw()
    love.graphics.draw(self.canvas, self.position.x, self.position.y, 0, self.scale_x, self.scale_y);
end

return Canvas