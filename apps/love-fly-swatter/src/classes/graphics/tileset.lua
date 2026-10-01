------------------------------------------------
--                  TILESET
-- lets you draw single tiles from a tileset
-- WARNING: only use 1d tilesets (indexed from left to right, starts with 0)
------------------------------------------------

-- imports
local Class = require('class');
local Vector2 = require('classes.math.vector2');

local Tileset = Class:derive("Tileset");

-- tileset constructor
function Tileset:new(image, width, height, tile_width, tile_height)
    self.size = Vector2(width, height);
    self.tile_size = Vector2(tile_width, tile_height);
    self.image = image;
end

return Tileset;