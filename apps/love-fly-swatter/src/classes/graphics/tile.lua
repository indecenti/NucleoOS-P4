------------------------------------------------
--                  TILE
-- a tile that can be drawn from a tileset
------------------------------------------------

-- imports
local Class = require('class');
local Vector2 = require('classes.math.vector2');

local Tile = Class:derive("Tile");

-- tile constructor
function Tile:new(tileset, index)
    self.tileset = tileset
    self.size = tileset.tile_size
    self.quad = love.graphics.newQuad(self.size.x * index, 
    -- NOTE: y position on tileset is always 0 because its a one dimensional tileset
                                    0, 
                                    self.size.x,
                                    self.size.y, 
                                    tileset.image:getDimensions()); 
end

-- tile draw event
function Tile:draw(x, y, scale_x, scale_y)
    love.graphics.draw(self.tileset.image, self.quad, x, y, 0, scale_x, scale_y);
end

return Tile;