local FPS = 15
local TIME_PER_FRAME = 1 / FPS
local TITLE = "Wormy"
local WINDOWWIDTH  = 1024
local WINDOWHEIGHT = 768
local CELLSIZE     = 32

assert(WINDOWWIDTH % CELLSIZE == 0, "Window width must be a multiple of cell size.")
assert(WINDOWHEIGHT % CELLSIZE == 0, "Window height must be a multiple of cell size.")

CELLWIDTH  = math.floor(WINDOWWIDTH / CELLSIZE)
CELLHEIGHT = math.floor(WINDOWHEIGHT / CELLSIZE)
 
--                                     R    G    B
local GREEN0 = { .60,  .73,  .05} --( 155, 188, 15)
local GREEN1 = { .54,  .67,  .05} --( 139, 172, 15)
local GREEN2 = { .18,  .38,  .18} --(  48,  98, 48)
local GREEN3 = { .05,  .21,  .05} --(  15,  56, 15)

local BGCOLOR = GREEN0
local LINECOLOR = GREEN1

local UP    = "up"
local DOWN  = "down"
local LEFT  = "left"
local RIGHT = "right"

local STATES = {
    MENU = "menu",
    PAUSED = "paused",
    GAME = "game",
    GAME_OVER = "game_over"
}

local HEAD = 1
local state = STATES.MENU

local BIGFONT   = love.graphics.newFont('/assets/fonts/early_gameboy.ttf', 100)
local BASICFONT   = love.graphics.newFont('/assets/fonts/early_gameboy.ttf', 25)

local degrees1 = 0 -- degrees of start text
local degrees2 = 0 -- degrees of start text
local direction = RIGHT
local apple = {}
local accumulatedTime = 0
local fruitImage = love.graphics.newImage('assets/sprites/fruit.png')
local fruit1 = love.graphics.newQuad(0, 0, 32, 32, fruitImage)
local fruit2 = love.graphics.newQuad(32, 0, 32, 32, fruitImage)
local fruit3 = love.graphics.newQuad(0, 32, 32, 32, fruitImage)
local fruit4 = love.graphics.newQuad(32, 32, 32, 32, fruitImage)
local snakeBodyImg = love.graphics.newImage('assets/sprites/body.png')

local snakeHeadImg = love.graphics.newImage('assets/sprites/head.png')
local upHead       = love.graphics.newQuad( 0,  0, 32, 32, snakeHeadImg)
local rightHead    = love.graphics.newQuad(32,  0, 32, 32, snakeHeadImg)
local downHead     = love.graphics.newQuad( 0, 32, 32, 32, snakeHeadImg)
local leftHead     = love.graphics.newQuad(32, 32, 32, 32, snakeHeadImg)

apple.index  = 1
local sounds = {}

function love.load()
    love.window.setTitle(TITLE)
    love.window.setMode(WINDOWWIDTH, WINDOWHEIGHT)
    love.graphics.setBackgroundColor(BGCOLOR)
    sounds.music = love.audio.newSource("assets/music/alex_gameboy.wav", "stream")
    sounds.music:setLooping(true)
    sounds.music:setVolume(0.05)
    sounds.music:play()

    sounds.pickUpSFX = love.audio.newSource("assets/sounds/pick_up.ogg", "static")
    sounds.pickUpSFX:setVolume(0.5)

    sounds.hitSFX = love.audio.newSource("assets/sounds/hit.ogg", "static")
    sounds.hitSFX:setVolume(0.5)
end

function love.keypressed(key)
    if state == STATES.GAME then
        if (key == "up" or key == "w") and direction ~= DOWN then
            direction = UP
        elseif (key == "right" or key == "d") and direction ~= LEFT then
            direction = RIGHT
        elseif (key == "down" or key == "s") and direction ~= UP then
            direction = DOWN
        elseif (key == "left" or key == "a") and direction ~= RIGHT then
            direction = LEFT
        end
    end
end

function love.keyreleased(key)
    if state == STATES.MENU then
        state = STATES.GAME
        -- Set a random start point.
        local startx = love.math.random(5, CELLWIDTH - 6)
        local starty = love.math.random(5, CELLHEIGHT - 6)
        wormCoords = { {x = startx,     y = starty},
                       {x = startx - 1, y = starty},
                       {x = startx - 2, y = starty} }
        direction = RIGHT
        --Start the apple in a random place.
        apple = getRandomLocation()
        accumulatedTime = 0
    elseif state == STATES.GAME_OVER then
        state = STATES.MENU
    end

    if key == "escape" then
        love.event.quit()
    end
end

function love.update(dt)
    if state == STATES.GAME then
        accumulatedTime = accumulatedTime + dt
        if accumulatedTime >= TIME_PER_FRAME then
            accumulatedTime = accumulatedTime - TIME_PER_FRAME
            -- check if the worm has hit itself or the edge
            if wormCoords[HEAD].x == -1 or wormCoords[HEAD].x == CELLWIDTH or wormCoords[HEAD].y == -1 or wormCoords[HEAD].y == CELLHEIGHT then
                sounds.hitSFX:play()
                state = STATES.GAME_OVER
                return -- game over
            end
            for i = 2, #wormCoords do
                if wormCoords[i].x == wormCoords[HEAD].x and wormCoords[i].y == wormCoords[HEAD].y then
                    sounds.hitSFX:play()
                    state = STATES.GAME_OVER
                    return -- game over
                end
            end

            -- check if worm has eaten an apply
            if wormCoords[HEAD].x == apple.x and wormCoords[HEAD].y == apple.y then
                sounds.pickUpSFX:stop()
                sounds.pickUpSFX:play()
                -- don't remove worm's tail segment
                apple = getRandomLocation() -- set a new apple somewhere
            else
                table.remove(wormCoords, #wormCoords)
            end

            -- move the worm by adding a segment in the direction it is moving
            local newHead = {}
            if direction == UP then
                newHead = {x = wormCoords[HEAD].x, y = wormCoords[HEAD].y - 1}
            elseif direction == DOWN then
                newHead = {x = wormCoords[HEAD].x, y = wormCoords[HEAD].y + 1}
            elseif direction == LEFT then
                newHead = {x = wormCoords[HEAD].x - 1, y = wormCoords[HEAD].y}
            elseif direction == RIGHT then
                newHead = {x = wormCoords[HEAD].x + 1, y = wormCoords[HEAD].y}
            end
            table.insert(wormCoords, 1, newHead)
        end
    end
end

function love.draw()
    if state == STATES.MENU then
        showStartScreen()
    elseif state == STATES.GAME then
        drawGrid()
        drawApple()
        drawScore()
        drawWorm()
    elseif state == STATES.GAME_OVER then
        showGameOverScreen()
    end
end

function showStartScreen() 
    love.graphics.setFont(BIGFONT)
    local titleText = "Wormy !"
    local font = love.graphics.getFont()
    local textWidth = font:getWidth(titleText)
    local textHeight = font:getHeight()
    love.graphics.setColor(GREEN2)
    love.graphics.print(titleText, WINDOWWIDTH/2, WINDOWHEIGHT/2, degrees1, 1, 1, textWidth/2, textHeight/2)
    
    love.graphics.setColor(GREEN3)
    love.graphics.print(titleText, WINDOWWIDTH/2, WINDOWHEIGHT/2, degrees2, 1, 1, textWidth/2, textHeight/2)
    degrees1 = degrees1 + 0.01
    degrees2 = degrees2 + 0.015

    love.graphics.setColor(GREEN3)
    love.graphics.setFont(BASICFONT)
    local hintText = "Press a key to play."
    local font = love.graphics.getFont()
    local textWidth = font:getWidth(hintText)
    local textHeight = font:getHeight()
    love.graphics.print( hintText, WINDOWWIDTH/2, WINDOWHEIGHT - 30, 0, 1, 1, textWidth/2, textHeight/2)

    love.graphics.setColor(1,1,1)
end

function showGameOverScreen() 
    love.graphics.setFont(BIGFONT)
    local titleText = "Game Over"
    local font = love.graphics.getFont()
    local textWidth = font:getWidth(titleText)
    local textHeight = font:getHeight()
    love.graphics.setColor(GREEN3)
    love.graphics.print(titleText, WINDOWWIDTH/2, WINDOWHEIGHT/2, 0, 1, 1, textWidth/2, textHeight/2)

    love.graphics.setColor(1,1,1)
end

function getRandomLocation()
    return {x = love.math.random(0, CELLWIDTH - 1), y = love.math.random(0, CELLHEIGHT - 1), index = love.math.random(1, 4)}
end

function drawScore()
    love.graphics.setFont(BASICFONT)
    love.graphics.setColor(GREEN3)
    love.graphics.print("Score: "..tostring((#wormCoords-3)), WINDOWWIDTH - 210, 10)
    love.graphics.setColor(1,1,1) 
end

function drawApple()
    if apple.index == 1 then
        love.graphics.draw(fruitImage, fruit1, apple.x * CELLSIZE, apple.y * CELLSIZE)
    elseif apple.index == 2 then
        love.graphics.draw(fruitImage, fruit2, apple.x * CELLSIZE, apple.y * CELLSIZE)
    elseif apple.index == 3 then 
        love.graphics.draw(fruitImage, fruit3, apple.x * CELLSIZE, apple.y * CELLSIZE)
    elseif apple.index == 4 then
        love.graphics.draw(fruitImage, fruit4, apple.x * CELLSIZE, apple.y * CELLSIZE)
    end
    love.graphics.setColor(1,1,1)
end

function drawWorm()
    local x = wormCoords[1].x * CELLSIZE
    local y = wormCoords[1].y * CELLSIZE
    if direction == UP then
        love.graphics.draw(snakeHeadImg, upHead, x , y)
    elseif direction == RIGHT then
        love.graphics.draw(snakeHeadImg, rightHead, x, y)
    elseif direction == LEFT then
        love.graphics.draw(snakeHeadImg, leftHead, x , y)
    elseif direction == DOWN then
        love.graphics.draw(snakeHeadImg, downHead, x, y)
    end
    love.graphics.origin()
    for i = 2, #wormCoords do
        local x = wormCoords[i].x * CELLSIZE
        local y = wormCoords[i].y * CELLSIZE
        love.graphics.draw(snakeBodyImg, x, y)
    end
    love.graphics.setColor(1,1,1)
end

function drawGrid()
    for x = 0, WINDOWWIDTH, CELLSIZE do -- draw vertical lines
        love.graphics.setColor(LINECOLOR)
        love.graphics.line(x, 0, x, WINDOWHEIGHT)
    end
    for y = 0, WINDOWHEIGHT, CELLSIZE  do -- draw horizontal lines
        love.graphics.setColor(LINECOLOR)
        love.graphics.line(0, y, WINDOWWIDTH, y)
    end
    love.graphics.setColor(1,1,1)
end