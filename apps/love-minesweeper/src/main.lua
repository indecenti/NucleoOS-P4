require("lib")

minefield = {}

blockScaledSize = config.blockScale * BLOCK_SIZE
remainingBlocks = config.boardWidth * config.boardHeight

local firstClick = true
local remainingFlags = config.flags

local timer = 0
local tHelper = 0

local mineFile = nil
local mineIcons = {}

-- Load icons and setup starting board
function love.load()
    mineFile = love.graphics.newImage("minesweeper.png")

    love.graphics.setBackgroundColor(0, 180 / 255, 131 / 255)

    mineIcons[HIDDEN] = love.graphics.newQuad(2, 53, BLOCK_SIZE, BLOCK_SIZE, mineFile:getDimensions())
    mineIcons[MINE] = mineIcons[HIDDEN]

    mineIcons[FLAG] = love.graphics.newQuad(37, 54, BLOCK_SIZE, BLOCK_SIZE, mineFile:getDimensions())

    mineIcons[EMPTY_ZERO] = love.graphics.newQuad(20, 54, BLOCK_SIZE, BLOCK_SIZE, mineFile:getDimensions())
    mineIcons[EMPTY_ONE] = love.graphics.newQuad(3, 71, BLOCK_SIZE, BLOCK_SIZE, mineFile:getDimensions())
    mineIcons[EMPTY_TWO] = love.graphics.newQuad(20, 71, BLOCK_SIZE, BLOCK_SIZE, mineFile:getDimensions())
    mineIcons[EMPTY_THREE] = love.graphics.newQuad(37, 71, BLOCK_SIZE, BLOCK_SIZE, mineFile:getDimensions())
    mineIcons[EMPTY_FOUR] = love.graphics.newQuad(54, 71, BLOCK_SIZE, BLOCK_SIZE, mineFile:getDimensions())
    mineIcons[EMPTY_FIVE] = love.graphics.newQuad(71, 71, BLOCK_SIZE, BLOCK_SIZE, mineFile:getDimensions())
    mineIcons[EMPTY_SIX] = love.graphics.newQuad(88, 71, BLOCK_SIZE, BLOCK_SIZE, mineFile:getDimensions())
    mineIcons[EMPTY_SEVEN] = love.graphics.newQuad(105, 71, BLOCK_SIZE, BLOCK_SIZE, mineFile:getDimensions())
    mineIcons[EMPTY_EIGHT] = love.graphics.newQuad(122, 71, BLOCK_SIZE, BLOCK_SIZE, mineFile:getDimensions())

    for i = 1, config.boardHeight do
        minefield[i] = {}

        for j = 1, config.boardWidth do
            minefield[i][j] = {HIDDEN, false}
        end
    end

    setMines()
end

-- Draw timer, flags remaining and icons
function love.draw()

    love.graphics.setNewFont(72)
    love.graphics.print(timer, config.timerX, config.timerY)
    love.graphics.print(remainingFlags, config.flagX, config.flagY)

    for i = 0, config.boardHeight - 1 do
        for j = 0, config.boardWidth - 1 do
            local block = minefield[i + 1][j + 1]
            local blockIcon = block[2] and mineIcons[FLAG] or mineIcons[block[1]]

            love.graphics.draw(mineFile, blockIcon, config.boardX + blockScaledSize * j, config.boardY + blockScaledSize * i, 0, config.blockScale, config.blockScale)
        end
    end

end

-- Update timer and check for victory
function love.update(dt)
    if remainingBlocks == config.mines then
        print("You win!")
        love.event.quit()
    end

    if not firstClick then
        tHelper = tHelper + dt

        if tHelper < 1 then return end

        timer = timer + 1
        tHelper = tHelper - 1
    end
end

-- Handle block selection
function love.mousereleased(x, y, button, istouch)
    if y < config.boardY then return end

    x, y = getEquivalentPosition(x, y)
    local block = minefield[y][x]

    if button == LEFT_CLICK then
        -- Can't select a flagged block
        if block[2] then return end
        
        if block[1] == MINE then
            -- In case the first block chosen is a mine, realocate it
            if firstClick then
                realocateMine(x, y)
                loadBlock(x, y)
            else
                print("Game over!") 
                love.event.quit()
            end

        elseif block[1] == HIDDEN then
            loadBlock(x, y)
        end

        if firstClick then firstClick = false end

    elseif button == RIGHT_CLICK then
        if (not block[2] and remainingFlags == 0) or (block[1] ~= HIDDEN and block[1] ~= MINE) then return end

        remainingFlags = remainingFlags + (block[2] and 1 or -1)

        block[2] = not block[2]
    end

end