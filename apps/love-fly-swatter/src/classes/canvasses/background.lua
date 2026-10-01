------------------------------------------------
--             BACKGROUND (CANVAS)
-- this is where the background gets rendered onto
------------------------------------------------

-- imports
require('globals');
local Canvas = require('classes.canvas');
local Vector2 = require('classes.math.vector2');

local Background = Canvas:derive("Background");

-- background constructor
function Background:new()
    self.position = Vector2(0, 0)
    self.size = Vector2(WIDTH, HEIGHT);

    self.canvas = love.graphics.newCanvas(self.size.x, self.size.y);
end

-- background draw event
function Background:draw()
    -- render background
    self.canvas:renderTo(function ()
        -- draw background color
        love.graphics.setColor(0, 0, 0);
        love.graphics.rectangle("fill", 0, 0, WIDTH, HEIGHT);
        -- reset drawing color
        love.graphics.setColor(1, 1, 1);

        tile_width = WIDTH/16 - 1;
        tile_height = HEIGHT/16 - 1;

        -- draw tiles
        for _x = 0, tile_width, 1 do
            for _y = 0, tile_height, 1 do
                -- leave a border around screen without tiles
                if not((_x == 0) or (_y == 0) or (_x == tile_width) or (_y == tile_height)) then
                    love.graphics.rectangle("fill", _x*16, _y*16, 16, 16);
                end
            end
        end
    end);

    -- draw background
    love.graphics.draw(self.canvas, self.position.x, self.position.y);
end


return Background