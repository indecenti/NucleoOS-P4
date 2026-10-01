if arg[#arg] == "vsc_debug" then require("lldebugger").start() end

require("utils")
Physics = require("libs.bump")
Object  = require("libs.classic")
Input   = require("libs.boypushy")()
Timer   = require("libs.humpTimerExtension")
Camera  = require("libs.hump.camera")
Signal  = require("libs.hump.signal")
Audio   = require("libs.ripple")
M = require("libs.moses")

function love.load(args, uArgs)
    local classFiles = {}
    RecursiveEnumerate("objects", classFiles)
    RecursiveEnumerate("rooms", classFiles)
    RequireFiles(classFiles)

    GameCamera = Camera()
    Game.globalTimer = Timer()
    love.math.setRandomSeed(os.time())
    love.graphics.setDefaultFilter("nearest", "nearest")
    love.graphics.setLineStyle("rough")
    love.graphics.setBackgroundColor(Game.backgroundColor)

    Input:bind("q", "right")
    Input:bind("d", "left")
    Input:bind("a", "left")
    Input:bind("space", "start")
    Input:bind("escape", function() love.event.quit() end)

    --ResizeWindow(2)

    CurrRoom = nil
    GoToRoom("Stage")
end

function love.update(dt)
    Game.globalTimer:update(dt*Game.timeScale)
    GameCamera:update(dt*Game.timeScale)
    if CurrRoom then CurrRoom:update(dt*Game.timeScale) end
    Input:update(dt)

    if Game.flashFrames then
        Game.flashFrames = Game.flashFrames - 1
        if Game.flashFrames <= 0 then Game.flashFrames = nil end
    end
end

function love.draw()
    if CurrRoom then CurrRoom:draw() end

    if Game.flashFrames then
        love.graphics.setColor(Game.backgroundColor)
        love.graphics.rectangle('fill', 0, 0, Game.sX*Game.w, Game.sY*Game.h)
        love.graphics.setColor(1, 1, 1)
    end
end

-- HELPER FUNCTIONS

function GoToRoom(room, ...)
    if CurrRoom and CurrRoom.destroy then CurrRoom:destroy() end
    CurrRoom = _G[room](...)
end

function RecursiveEnumerate(folder, fileList)
    local items = love.filesystem.getDirectoryItems(folder)
    for _, item in pairs(items) do
        local file = folder .. "/" .. item
        local fileType = love.filesystem.getInfo(file).type
        if fileType == "file" then
            table.insert(fileList, file)
        elseif fileType == "directory" then
            RecursiveEnumerate(file, fileList)
        end
    end
end

function RequireFiles(files)
    for _, file in pairs(files) do
        local file = file:sub(1, -5)
        require(file)
    end
end

-- MAIN LOOP
function love.run()
    if love.load then love.load(love.arg.parseGameArguments(arg), arg) end
    if not love.timer then error("Timer module required") end

    local lag = 0
    local fixedDt = 1/60 -- DT fix (60 FPS)

    -- Main loop time.
    return function()
        -- Process events.
        if love.event then
            love.event.pump()
            for name, a,b,c,d,e,f in love.event.poll() do
                if name == "quit" then
                    if not love.quit or not love.quit() then
                        return a or 0
                    end
                end
                love.handlers[name](a,b,c,d,e,f)
            end
        end

        local elapsed = love.timer.step()
        lag = lag + elapsed

        -- Call update and draw
        while lag >= fixedDt do
            if love.update then love.update(fixedDt) end
            lag = lag - fixedDt
        end

        if love.graphics and love.graphics.isActive() then
            love.graphics.origin()
            love.graphics.clear(love.graphics.getBackgroundColor())

            if love.draw then love.draw() end

            love.graphics.present()
        end

        if love.timer then love.timer.sleep(0.001) end
    end
end