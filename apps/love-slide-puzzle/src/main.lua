Timer = require 'libraries.hump.timer'
Input = require 'libraries.boipushy.Input'

local BOARDWIDTH = 4  -- number of columns in the board
local BOARDHEIGHT = 4 -- number of rows in the board
local TILESIZE = 80
local WINDOWWIDTH = 640
local WINDOWHEIGHT = 480
local BLANK = nil

--COLORS
local GREEN3     = {.05, .21, .05}--(15 ,  56, 15);
local GREEN2     = {.18, .38, .18}--(48 ,  98, 48);
local GREEN1     = {.54, .67, .05}--(139, 172, 15);
local GREEN0     = {.6,  .73, .05}--(155, 188, 15);

local BGCOLOR = GREEN0
local TILECOLOR = GREEN2
local TEXTCOLOR = GREEN1
local BORDERCOLOR = GREEN3
local MESSAGECOLOR = GREEN2

local BASICFONTSIZE = 20

local XMARGIN = math.floor((WINDOWWIDTH - (TILESIZE * BOARDWIDTH + (BOARDWIDTH - 1))) / 2)
local YMARGIN = math.floor((WINDOWHEIGHT - (TILESIZE * BOARDHEIGHT + (BOARDHEIGHT - 1))) / 2)

local UP = 'up'
local DOWN = 'down'
local LEFT = 'left'
local RIGHT = 'right'

local TIME_TILE_MOVE_ANIM = 0.2
local EASING_TYPE = 'in-out-quad'

local message = ''
local activeTile = {x = -1, y = -1, isActive = false, posX = 0, posY = 0}

function love.load()
    love.window.setTitle("LOVE Slide Puzzle")
    love.window.setMode( WINDOWWIDTH, WINDOWHEIGHT )
    love.graphics.setBackgroundColor( BGCOLOR )
    font = love.graphics.newFont('assets/fonts/Early GameBoy.ttf', BASICFONTSIZE)
    timer = Timer()
    input = Input()
    input:bind('mouse1', 'leftButtonPressed')
    input:bind('left',   'left')
    input:bind('a',   'left')
    input:bind('right',   'right')
    input:bind('d',   'right')
    input:bind('up',   'up')
    input:bind('w',   'up')
    input:bind('down',   'down')
    input:bind('s',   'down')
    generateNewPuzzle(5)
    SOLVEDBOARD = getStartingBoard()
    allMoves = {}
    slideTo = nil
end

function love.update(dt)
    timer:update(dt)
    if activeTile.isActive then
        return
    end

    slideTo = nil
    if checkWinner() then
        message = "Solved !"
    end

    if input:released('leftButtonPressed') then
        local x, y = love.mouse.getPosition()
        local spotX, spotY = getSpotClicked(x, y)
        if spotX == nil and spotY == nil then
            --reset
            local resetButton = {x = WINDOWWIDTH - 160, y = 15, w = 150, h = 25}
            if (x > resetButton.x) and (x < resetButton.x + resetButton.w) and (y > resetButton.y) and (y < resetButton.y + resetButton.h) then
                resetAnimation('Resetting')
            end
            --new game
            local newButton = {x = WINDOWWIDTH - 160, y = WINDOWHEIGHT - 60, w = 150, h = 25}
            if (x > newButton.x) and (x < newButton.x + newButton.w) and (y > newButton.y) and (y < newButton.y + newButton.h) then
                generateNewPuzzle(80)
                allMoves = {}
            end
            --solved
            local solvedButton = {x = WINDOWWIDTH - 160, y = WINDOWHEIGHT - 30, w = 150, h = 25}
            if (x > solvedButton.x) and (x < solvedButton.x + solvedButton.w) and (y > solvedButton.y) and (y < solvedButton.y + solvedButton.h) then
                for i = 1, #allMoves do
                    table.insert(sequence, allMoves[i])
                end
                allMoves = sequence
                sequence = {}
                resetAnimation('Solving')
            end
        else
            local blankx, blanky = getBlankPosition()
            if spotX == blankx + 1 and spotY == blanky then
                slideTo = LEFT
            elseif spotX == blankx - 1 and spotY == blanky then
                slideTo = RIGHT
            elseif spotX == blankx and spotY == blanky + 1 then
                slideTo = UP
            elseif spotX == blankx and spotY == blanky - 1 then
                slideTo = DOWN
            end

        end
    elseif input:released('left') or input:released('down') or input:released('right') or input:released('up') then
        if input:released('left') and isValidMove(LEFT) then
            slideTo = LEFT
        elseif input:released('right') and isValidMove(RIGHT) then
            slideTo = RIGHT
        elseif input:released('down') and isValidMove(DOWN) then
            slideTo = DOWN
        elseif input:released('up') and isValidMove(UP) then
            slideTo = UP
        end
    end

    if slideTo then 
        local direction = slideTo
        slideTile(direction)
        timer:after(0.25 * TIME_TILE_MOVE_ANIM, function() 
            activeTile.isActive = false
            activeTile.y = -1
            activeTile.x = -1
        end)
        makeMove(direction)
        table.insert(allMoves, direction)
    end
end

function love.draw()
    drawButtons()
    drawBoard()
    if activeTile.isActive then
        drawTile(activeTile.posX, activeTile.posY, board[activeTile.x][activeTile.y], 0, 0)
    end
end

function  drawButtons()
    love.graphics.setColor(TILECOLOR)
    local rect_width = 150
    local rect_height = 25

    love.graphics.rectangle("fill", WINDOWWIDTH - 160, 15, rect_width, rect_height)
    love.graphics.rectangle("fill", WINDOWWIDTH - 160, WINDOWHEIGHT - 60, rect_width, rect_height)
    love.graphics.rectangle("fill", WINDOWWIDTH - 160, WINDOWHEIGHT - 30, rect_width, rect_height)

    love.graphics.setFont(font)
    love.graphics.setColor(TEXTCOLOR)
    local text = 'Reset'
    local font = love.graphics.getFont()
    local textWidth = font:getWidth(text)
    local textHeight = font:getHeight()

    love.graphics.print(text, WINDOWWIDTH - 160 + rect_width/2, 15 + rect_height/2, 0, 1, 1, textWidth/2, textHeight/2)
    text = 'New Game'
    textWidth = font:getWidth(text)
    love.graphics.print(text, WINDOWWIDTH - 160 + rect_width/2, WINDOWHEIGHT - 60 + rect_height/2, 0, 1, 1, textWidth/2, textHeight/2)
    text = 'Solved'
    textWidth = font:getWidth(text)
    love.graphics.print(text, WINDOWWIDTH - 160 + rect_width/2, WINDOWHEIGHT - 30 + rect_height/2, 0, 1, 1, textWidth/2, textHeight/2)
    
    love.graphics.setColor(1,1,1)
end

function generateNewPuzzle(numSlides)
    sequence = {}
    board = getStartingBoard()
    timer:after(0.5, function()
        for i = 0, numSlides do
            timer:after(TIME_TILE_MOVE_ANIM*i, function()
                message = "Generating new puzzle...\n"..tostring(i).."-"..tostring(numSlides).." moves"
                local direction = getRandomMove()
                slideTile(direction)
                makeMove(direction)
                table.insert(sequence, direction)
                if(i == numSlides) then
                    activeTile.isActive = false
                    activeTile.y = -1
                    activeTile.x = -1
                    message = ''
                end
            end)
        end
    end)
end

function getStartingBoard()
    local board = {}

    for x = 1, BOARDWIDTH do
        local column = {}

        for y = 1, BOARDHEIGHT do
            if x == BOARDWIDTH and y == BOARDHEIGHT then
                table.insert(column, BLANK)
            else
                table.insert(column, (x - 1) * BOARDWIDTH + y)
            end
        end

        table.insert(board, column)
    end

    return board
end

function drawBoard()
    if message then
        local font = love.graphics.getFont()
        local textWidth = font:getWidth(message)
        local textHeight = font:getHeight()
        local x = 5
        local y = 5
        love.graphics.setColor(BGCOLOR)
        love.graphics.rectangle("fill", x, y, textWidth, textHeight)
        love.graphics.setColor(MESSAGECOLOR)
        love.graphics.print(message, x + textWidth/2, y + textHeight/2, 0, 1, 1, textWidth/2, textHeight/2)
    end

    for x = 1, BOARDWIDTH do
        for y = 1, BOARDHEIGHT do
            if x == activeTile.x and y == activeTile.y then

            elseif board[x][y] then
                drawTile(x, y, board[x][y], 0, 0)
            end
        end
    end

    local left, top = getLeftTopOfTile(1, 1)
    local width = BOARDWIDTH * TILESIZE
    local height = BOARDHEIGHT * TILESIZE
    love.graphics.setColor(BORDERCOLOR)
    love.graphics.rectangle("line", left - 5, top - 5, width + 11, height + 11, 4, 4)

    love.graphics.setColor(1,1,1)
end

function getLeftTopOfTile(tileX, tileY)
    tileX = tileX - 1
    tileY = tileY - 1
    local left = XMARGIN + (tileX * TILESIZE) + (tileX - 1)
    local top  = YMARGIN + (tileY * TILESIZE) + (tileY - 1)
    return left, top
end

function drawTile(tilex, tiley, number, adjx, adjy)
   local left, top = getLeftTopOfTile(tilex, tiley)
   love.graphics.setColor(TILECOLOR)
   love.graphics.rectangle("fill", left + adjx, top + adjy, TILESIZE, TILESIZE) 

   love.graphics.setColor(TEXTCOLOR)
   local font = love.graphics.getFont()
   local textWidth = font:getWidth(tostring(number))
   local textHeight = font:getHeight()
   love.graphics.print(tostring(number), left + math.floor(TILESIZE/2) + adjx, top + math.floor(TILESIZE/2) + adjy, 0, 1, 1, textWidth/2, textHeight/2)

   love.graphics.setColor(1,1,1)
end

function getBlankPosition()
    for x = 1, BOARDWIDTH do
        for y = 1, BOARDHEIGHT do
            if board[x][y] == BLANK then
                return x, y
            end
        end
    end
end

function isValidMove(move)
    local blankx, blanky = getBlankPosition()
    return (move == UP and blanky ~= BOARDHEIGHT) or (move == DOWN and blanky ~= 1) or (move == LEFT and blankx ~= BOARDWIDTH) or (move == RIGHT and blankx ~= 1)
end

function getRandomMove()
    local validMoves = {'up', 'down', 'left', 'right'}
    if lastMove == UP or not isValidMove(DOWN) then
        table.remove(validMoves, table.indexof(validMoves, DOWN))
    end
    if lastMove == DOWN or not isValidMove(UP) then
        table.remove(validMoves, table.indexof(validMoves, UP))
    end
    if lastMove == LEFT or not isValidMove(RIGHT) then
        table.remove(validMoves, table.indexof(validMoves, RIGHT))
    end
    if lastMove == RIGHT or not isValidMove(LEFT) then
        table.remove(validMoves, table.indexof(validMoves, LEFT))
    end
    return validMoves[love.math.random(1, #validMoves)]
end

function makeMove(move)
    local blankx, blanky = getBlankPosition()
    local tempVar
    if move == UP then
        tempVar = board[blankx][blanky] 
        board[blankx][blanky]     = board[blankx][blanky + 1]
        board[blankx][blanky + 1] = tempVar
    elseif move == DOWN then
        tempVar = board[blankx][blanky] 
        board[blankx][blanky]     = board[blankx][blanky - 1]
        board[blankx][blanky - 1] = tempVar
    elseif move == LEFT then
        tempVar = board[blankx][blanky]
        board[blankx][blanky]      = board[blankx + 1][blanky]
        board[blankx + 1][blanky]  = tempVar
    elseif move == RIGHT then
        tempVar = board[blankx][blanky]
        board[blankx][blanky]     = board[blankx - 1][blanky]
        board[blankx - 1][blanky] =  tempVar
    end
end

function table.indexof(tab, val)
    for i, v in ipairs(tab) do
        if v == val then
            return i
        end
    end
    return nil
end

function printBoard()
    print(board[1][1], ' ', board[2][1],' ', board[3][1],' ',board[4][1])
    print(board[1][2], ' ', board[2][2],' ', board[3][2],' ',board[4][2])
    print(board[1][3], ' ', board[2][3],' ', board[3][3],' ',board[4][3])
    print(board[1][4], ' ', board[2][4],' ', board[3][4],' ',board[4][4])
end

function getSpotClicked(x, y)
    for tileX = 1, BOARDWIDTH do
        for tileY = 1, BOARDHEIGHT do
            local left, top = getLeftTopOfTile(tileX, tileY)
            local rectangle = {x = left, y = top, w = TILESIZE, h = TILESIZE}
            if (x > rectangle.x) and (x < rectangle.x + rectangle.w) and (y > rectangle.y) and (y < rectangle.y + rectangle.h) then
                return tileX, tileY
            end
        end
    end
    return nil, nil
end

function checkWinner()
    for x = 1, BOARDWIDTH do
        for y = 1, BOARDHEIGHT do
            if board[x][y] ~= SOLVEDBOARD[x][y] then
                return false
            end
        end
    end
    return true
end

function resetAnimation(actionName)
    revAllMoves = {}
    for i = #allMoves, 1, -1 do
        table.insert(revAllMoves, allMoves[i])
    end

    timer:after(0.25, function()
        for i = 1, #revAllMoves do
            timer:after(TIME_TILE_MOVE_ANIM*i, function()
                message = actionName.."\n"..tostring(i).."-"..tostring(#revAllMoves).." moves"
                local oppositeMove
                if revAllMoves[i] == UP then
                    oppositeMove = DOWN
                elseif revAllMoves[i] == LEFT then 
                    oppositeMove = RIGHT
                elseif revAllMoves[i] == RIGHT then
                    oppositeMove = LEFT
                elseif revAllMoves[i] == DOWN then
                    oppositeMove = UP
                end
                
                local direction = oppositeMove
                slideTile(direction)

                makeMove(direction)
                table.insert(sequence, direction)
                if(i == #revAllMoves) then
                    activeTile.isActive = false
                    activeTile.y = -1
                    activeTile.x = -1
                    message = ''
                    allMoves = {}
                end
            end)
        end
    end)
end

function slideTile(direction)
    activeTile.isActive = true
    local blankx, blanky = getBlankPosition()

    local movex, movey
    if direction == UP then
        movex = blankx 
        movey = blanky + 1
    elseif direction == DOWN then
        movex = blankx
        movey =  blanky - 1
    elseif direction == LEFT then
        movex = blankx + 1
        movey = blanky
    elseif direction == RIGHT then
        movex = blankx - 1
        movey = blanky
    end

    activeTile.x = blankx
    activeTile.y = blanky

    activeTile.posX = activeTile.x
    activeTile.posY = activeTile.y
    timer:tween(TIME_TILE_MOVE_ANIM, activeTile, { posX = movex, posY = movey}, EASING_TYPE)
end