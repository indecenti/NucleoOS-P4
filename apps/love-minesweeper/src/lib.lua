LEFT_CLICK = 1
RIGHT_CLICK = 2

BLOCK_SIZE = 16

EMPTY_ZERO = 0
EMPTY_ONE = 1
EMPTY_TWO = 2
EMPTY_THREE = 3
EMPTY_FOUR = 4
EMPTY_FIVE = 5
EMPTY_SIX = 6
EMPTY_SEVEN = 7
EMPTY_EIGHT = 8
MINE = 9
HIDDEN = 10
FLAG = 11

-- Generate mines on board
function setMines()
    local remainingMines = config.mines

    math.randomseed(os.time())
    math.random(); math.random(); math.random()
    while remainingMines > 0 do
        local positionX, positionY = math.random(1, 10), math.random(1, 10)
        
        if minefield[positionY][positionX][1] == HIDDEN then
            minefield[positionY][positionX][1] = MINE

            remainingMines = remainingMines - 1
        end
    end

end

-- Realocate mine to a valid position (left -> right, top -> bottom)
function realocateMine(x, y)
    minefield[y][x][1] = HIDDEN

    x = 1
    y = 1

    while minefield[y][x][1] == MINE do
        x = x + 1

        if x > config.boardWidth then
            x = 1
            y = y + 1
        end

        if y > config.boardHeight then
            print("Could not realocate mine...")

            return
        end
    end

    minefield[y][x][1] = MINE

end

-- Load a non-mine block (and its neighbouring blocks in case there are no mines surrounding it)
function loadBlock(x, y)
    local mineCount = 0

    remainingBlocks = remainingBlocks - 1

    for i = -1, 1 do
        for j = -1, 1 do
            local positionX, positionY = x + j, y + i
            local block = minefield[positionY] and minefield[positionY][positionX] or nil

            if block and block[1] == MINE then
                mineCount = mineCount + 1
            end
        end
    end

    minefield[y][x][1] = mineCount

    if mineCount == 0 then
        floodFill(x, y)
    end

end

-- Load neighbouring blocks
function floodFill(x, y)
    for i = -1, 1 do
        for j = -1, 1 do
            local positionX, positionY = x + j, y + i
            local block = minefield[positionY] and minefield[positionY][positionX] or nil

            if block and block[1] == HIDDEN and not block[2] then
                loadBlock(positionX, positionY)
            end
        end
    end

end

-- Get x, y coordinates in hypothetical board based on window coordinates
function getEquivalentPosition(x, y)
    return math.ceil(x / blockScaledSize), math.ceil((y - config.boardY) / blockScaledSize)
end