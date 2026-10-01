------------------------------------------------
--                SCREEN (CANVAS)
-- the main screen where everything seen on the window
-- is drawn onto
------------------------------------------------

-- imports
require('globals');
local Canvas = require('classes.canvas');
local Vector2 = require('classes.math.vector2');
-- rooms
local Level = require('classes.canvasses.rooms.level');
local Game_Over = require('classes.canvasses.rooms.game_over');
local Title_Screen = require('classes.canvasses.rooms.title_screen');

local Screen = Canvas:derive("Screen");

-- screen constructor
function Screen:new()
    self.position = Vector2(0, 0)
    self.size = Vector2(WIDTH, HEIGHT);

    self.canvas = love.graphics.newCanvas(self.size.x, self.size.y);

    -- rooms
    self.level = Level();
    self.game_over = Game_Over();
    self.title_screen = Title_Screen();
end

-- screen update event
function Screen:update(delta_time)
    -- update rooms
    if CURRENT_ROOM == 3 then
        -- title screen
    elseif CURRENT_ROOM == 4 then
        -- level
        self.level:update(delta_time);
    elseif CURRENT_ROOM == 5 then
        -- game_over
    end
end

-- screen draw event
function Screen:draw()
    -- render to screen canvas
        self.canvas:renderTo(function()
           -- draw rooms
            if CURRENT_ROOM == 3 then
                -- title screen
                self.title_screen:draw();
            elseif CURRENT_ROOM == 4 then
                -- level
                self.level:draw();
            elseif CURRENT_ROOM == 5 then
                -- gameover
                self.game_over:draw();
            end
        end);

    -- draw screen scaled to window size
    love.graphics.draw(self.canvas, self.position.x, self.position.y, 0, love.graphics.getWidth()/WIDTH, love.graphics.getHeight()/HEIGHT);
end

-- mouse pressed event
function Screen:mousepressed(x, y, button, istouch, presses)
    -- pass function to rooms
    if CURRENT_ROOM == 3 then
        -- title screen
        self.title_screen:mousepressed(x, y, button, istouch, presses, self);
    elseif CURRENT_ROOM == 4 then
        -- level
        self.level:mousepressed(x, y, button, istouch, presses);
    elseif CURRENT_ROOM == 5 then
        -- game_over
        self.game_over:mousepressed(x, y, button, istouch, presses, self);
    end
end

return Screen