-- TODO: uhm no portrait mode in android or ios. that shit looks weeird somehow manage to get that out pls  

------------------------------------------------
--                   MAIN
--
------------------------------------------------

-- imports
require('globals');
require('filesystem');
local Screen = require('classes.canvasses.screen');

-- executes when game starts
function love.load()
    -- set title
    love.window.setTitle(GAME_TITLE.." "..GAME_VERSION);

    -- set window mode
    love.window.setMode(640, 400, {
        fullscreen = false,
        resizable = true,
        minwidth = WIDTH,
        minheight = HEIGHT,
    });

    -- get os
    OS = love.system.getOS();

    -- hide default cursor (desktop)
    if(love.mouse.isCursorSupported() == true) then
        love.mouse.setVisible(false);
    end

    -- set default scaling filter
    love.graphics.setDefaultFilter('nearest', 'nearest', 0);

    -- create screen
    screen = Screen();

    -- file load
    file_highscore_load();
end

-- called on every tick for calculations
function love.update(delta_time)
    -- accomidate for lag
    if (delta_time > 0.035) then return; end

    -- update
    screen:update(delta_time);
end

-- called on every tick for graphics
function love.draw()
    -- render screen
    screen:draw();
end

-- keyboard press event
function love.keypressed(key)
    -- fullscreen changing
    if key == "f11" then
        love.window.setFullscreen(not love.window.getFullscreen());
    end
    -- close game
    if key == "escape" then
        love.event.quit();
    end
end

-- mouse press event
function love.mousepressed(x, y, button, istouch, presses)
    -- pass to screen canvas
    screen:mousepressed(x, y, button, istouch, presses);
end
