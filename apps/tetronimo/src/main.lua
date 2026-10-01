-- NucleoOS port (Lua App engine): the only change to upstream is this block - touch gestures
-- become the game's keys (tap = rotate, swipe left/right = move, swipe down = drop). The TTF and
-- MIDI files are not shipped: text uses the engine font, the music is left out.
love.touch_keys = { tap = "up", left = "left", right = "right", down = "space", up = "up" }

-- constants
local SCALE = 1.5
local FPS = 25
local WINDOWWIDTH = SCALE * 640
local WINDOWHEIGHT = SCALE * 480
local BOXSIZE = SCALE * 20
local BOARDWIDTH = 10
local BOARDHEIGHT = 20
local BLANK = '.'
local TITLE = 'Tetronimo'
local MOVESIDEWAYSFREQ = 0.15
local MOVEDOWNFREQ = 0.1
local XMARGIN = math.floor((WINDOWWIDTH - BOARDWIDTH * BOXSIZE) / 2)
local TOPMARGIN = WINDOWHEIGHT - (BOARDHEIGHT * BOXSIZE) - 5
--               R    G    B
local WHITE       = {1.0, 1.0, 1.0} -- (255, 255, 255)
local GRAY        = {.72, .72, .72} -- (185, 185, 185)
local BLACK       = { .0,  .0,  .0} -- (  0,   0,   0)
local RED         = {.61,  .0,  .0} -- (155,   0,   0)
local LIGHTRED    = {.69,  .1,  .1} -- (175,  20,  20)
local GREEN       = { .0, .61,  .0} -- (  0, 155,   0)
local LIGHTGREEN  = { .1, .69,  .1} -- ( 20, 175,  20)
local BLUE        = { .0,  .0, .61} -- (  0,   0, 155)
local LIGHTBLUE   = { .1,  .1, .69} -- ( 20,  20, 175)
local YELLOW      = {.61, .61,  .0} -- (155, 155,   0)
local LIGHTYELLOW = {.69, .69,  .1} -- (175, 175,  20)

local BORDERCOLOR = BLUE
local BGCOLOR = BLACK
local TEXTCOLOR = WHITE
local TEXTSHADOWCOLOR = GRAY
local COLORS      = {     BLUE,      GREEN,      RED,      YELLOW}
local LIGHTCOLORS = {LIGHTBLUE, LIGHTGREEN, LIGHTRED, LIGHTYELLOW}
assert(#COLORS == #LIGHTCOLORS, "each color must have light color")

local TEMPLATEWIDTH = 5
local TEMPLATEHEIGHT = 5

local S_SHAPE_TEMPLATE = {{'.....',
                           '.....',
                           '..OO.',
                           '.OO..',
                           '.....'},
                          {'.....',
                           '..O..',
                           '..OO.',
                           '...O.',
                           '.....'}}

local Z_SHAPE_TEMPLATE = {{'.....',
                           '.....',
                           '.OO..',
                           '..OO.',
                           '.....'},
                          {'.....',
                           '..O..',
                           '.OO..',
                           '.O...',
                           '.....'}}

local I_SHAPE_TEMPLATE = {{'..O..',
                           '..O..',
                           '..O..',
                           '..O..',
                           '.....'},
                          {'.....',
                           '.....',
                           'OOOO.',
                           '.....',
                           '.....'}}

local O_SHAPE_TEMPLATE = {{'.....',
                           '.....',
                           '.OO..',
                           '.OO..',
                           '.....'}}

local J_SHAPE_TEMPLATE = {{'.....',
                           '.O...',
                           '.OOO.',
                           '.....',
                           '.....'},
                          {'.....',
                           '..OO.',
                           '..O..',
                           '..O..',
                           '.....'},
                          {'.....',
                           '.....',
                           '.OOO.',
                           '...O.',
                           '.....'},
                          {'.....',
                           '..O..',
                           '..O..',
                           '.OO..',
                           '.....'}}

local L_SHAPE_TEMPLATE = {{'.....',
                           '...O.',
                           '.OOO.',
                           '.....',
                           '.....'},
                          {'.....',
                           '..O..',
                           '..O..',
                           '..OO.',
                           '.....'},
                          {'.....',
                           '.....',
                           '.OOO.',
                           '.O...',
                           '.....'},
                          {'.....',
                           '.OO..',
                           '..O..',
                           '..O..',
                           '.....'}}

local T_SHAPE_TEMPLATE = {{'.....',
                           '..O..',
                           '.OOO.',
                           '.....',
                           '.....'},
                          {'.....',
                           '..O..',
                           '..OO.',
                           '..O..',
                           '.....'},
                          {'.....',
                           '.....',
                           '.OOO.',
                           '..O..',
                           '.....'},
                          {'.....',
                           '..O..',
                           '.OO..',
                           '..O..',
                           '.....'}}

local PIECES = {}
PIECES["S"] = S_SHAPE_TEMPLATE
PIECES["Z"] = Z_SHAPE_TEMPLATE
PIECES["J"] = J_SHAPE_TEMPLATE         
PIECES["L"] = L_SHAPE_TEMPLATE         
PIECES["I"] = I_SHAPE_TEMPLATE         
PIECES["O"] = O_SHAPE_TEMPLATE         
PIECES["T"] = T_SHAPE_TEMPLATE         
local BASICFONT = love.graphics.newFont('/assets/fonts/juniory.ttf', 22)
local BIGFONT   = love.graphics.newFont('/assets/fonts/juniory.ttf', 100)
local STATES = {
    MENU = "menu",
    PAUSED = "paused",
    GAME = "game",
    GAME_OVER = "game_over"
}
local TIME_PER_FRAME = 1 / FPS
local accumulatedTime = 0

local state = STATES.MENU
local sounds = {}
local score = 0
local level = 0
local fallFreq = 0
local lastMoveDownTime = love.timer.getTime()
local lastMoveSidewaysTime = love.timer.getTime()
local lastFallTime = love.timer.getTime()
local movingDown = false -- note: there is no movingUp variable
local movingLeft = false
local movingRight = false
local board = {}
local fallingPiece = {}
local nextPiece = {}
-- initialization
function love.load()
    love.window.setTitle(TITLE)
    love.window.setMode(WINDOWWIDTH, WINDOWHEIGHT)
    -- Set a seed for reproducibility (optional)
    love.math.setRandomSeed(os.time())
    love.graphics.setBackgroundColor(BLACK)

    sounds.destroyRowSFX = love.audio.newSource("assets/sounds/pop1.ogg", "static")
    sounds.destroyRowSFX:setVolume(1)
end

function love.keypressed(key)
    if state == STATES.GAME then
        if fallingPiece == nil then
            return
        end

        if (key == "left" or key == "a") and isValidPosition(-1, 0) then
            fallingPiece.x = fallingPiece.x - 1
            movingLeft = true
            movingRight = false
            lastMoveSidewaysTime = love.timer.getTime()
        elseif (key == "right" or key == "d") and isValidPosition(1, 0) then
            fallingPiece.x = fallingPiece.x + 1
            movingLeft = false
            movingRight = true
            lastMoveSidewaysTime = love.timer.getTime()

        elseif key == "up" or key == "w" then
            local currentRotation = fallingPiece.rotation
            fallingPiece.rotation = fallingPiece.rotation + 1
            if fallingPiece.rotation > #PIECES[fallingPiece.shape] then
                fallingPiece.rotation = 1
            end
            if not isValidPosition(0,0) then
                fallingPiece.rotation = currentRotation
            end
        elseif key == "q" then
            local currentRotation = fallingPiece.rotation
            fallingPiece.rotation = fallingPiece.rotation - 1
            if fallingPiece.rotation < 1 then
                fallingPiece.rotation = #PIECES[fallingPiece.shape]
            end
            if not isValidPosition(0, 0) then
                fallingPiece.rotation = currentRotation
            end
        elseif key == "down" or key == "s" then
            movingDown = True
            if isValidPosition(0, 1) then
                fallingPiece.y = fallingPiece.y + 1
                lastMoveDownTime = love.timer.getTime()
            end
        elseif key == "space" then
            movingDown = false
            movingLeft = false
            movingRight = false
            for i = 1, BOARDHEIGHT do
                if not isValidPosition(0, i) then
                    break
                end
                fallingPiece.y = fallingPiece.y + (i - 1)
            end

        end
    end
end

function love.keyreleased(key)
    if state == STATES.MENU then
        -- setup variables for the start of the game
        state = STATES.GAME
        -- Generate a random value between 0 and 1
        local randomValue = love.math.random()
        if randomValue > 0.5 then
            sounds.music = love.audio.newSource("assets/sounds/tetrisb.mid", "stream")
        else 
            sounds.music = love.audio.newSource("assets/sounds/tetrisc.mid", "stream")
        end
        sounds.music:setLooping(true)
        sounds.music:setVolume(0.05)
        sounds.music:play()
        
        setBlankBoard()

        lastMoveDownTime = love.timer.getTime()
        lastMoveSidewaysTime = love.timer.getTime()
        lastFallTime = love.timer.getTime()
        movingDown = false -- note: there is no movingUp variable
        movingLeft = false
        movingRight = false
        score = 0
        level, fallFreq = calculateLevelAndFallFreq()

        fallingPiece = getNewPiece()
        nextPiece = getNewPiece()

        accumulatedTime = 0
    elseif state == STATES.GAME then
        if key == "p" then
            state = STATES.PAUSED
            sounds.music:stop();
        elseif key == "left" or key == "a" then
            movingLeft = false
        elseif key == "right" or key == "d" then
            movingRight = false
        elseif key == "down" or key == "s" then
            movingDown = false  
        end
    elseif state == STATES.PAUSED then
        if key == "p" then
            state = STATES.GAME
            sounds.music:play();
            lastFallTime =         love.timer.getTime()
            lastMoveDownTime =     love.timer.getTime()
            lastMoveSidewaysTime = love.timer.getTime()
            accumulatedTime = 0
        end
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
        
            if fallingPiece == nil then
                -- No falling piece in play, so start a new piece at the top
                fallingPiece = nextPiece
                nextPiece = getNewPiece()
                lastFallTime = love.timer.getTime() -- reset lastFallTime
            end

            if not isValidPosition(0, 0) then
                state = STATES.GAME_OVER
                return -- can't fit a new piece on the board, so game over
            end
            -- handle moving the piece because of user input
            if (movingLeft or movingRight) and love.timer.getTime() - lastMoveSidewaysTime > MOVESIDEWAYSFREQ then
                if movingLeft and isValidPosition(-1, 0) then
                    fallingPiece.x = fallingPiece.x - 1
                elseif movingRight and isValidPosition(1, 0) then
                    fallingPiece.x = fallingPiece.x + 1
                end
                lastMoveSidewaysTime = love.timer.getTime()
            end

            -- handle moving the piece because of user input
            if movingDown and love.timer.getTime() - lastMoveDownTime > MOVEDOWNFREQ and isValidPosition(0, 1) then
                fallingPiece.y = fallingPiece.y + 1
                lastMoveDownTime = love.timer.getTime()
            end

            -- let the piece fall if it is time to fall
            if love.timer.getTime() - lastFallTime > fallFreq then
                -- see if the piece has landed
                if not isValidPosition(0, 1) then
                    -- falling piece has landed, set it on the board
                    addToBoard()
                    score = score + removeCompleteLines()
                    level, fallFreq = calculateLevelAndFallFreq()
                    fallingPiece = nil
                else
                    -- piece did not land, just move the piece down
                    fallingPiece.y = fallingPiece.y + 1
                    lastFallTime = love.timer.getTime()
                end
            end
        end
    end
end

function love.draw()
    if state == STATES.MENU then
        showTextScreen("Tetromino", "Press a key to play.")
    elseif state == STATES.GAME then
        drawBoard()
        drawStatus()
        drawNextPiece()
        if fallingPiece ~= nil then
            drawPiece(fallingPiece)
        end
    elseif state == STATES.PAUSED then
        showTextScreen("PAUSED", "Press \'p\' key to play.")
    elseif state == STATES.GAME_OVER then
        showTextScreen("GAME OVER", "     Your score: "..tostring(score)..
                                   "\nPress a key to play.")
    end
end

function showTextScreen(titleText, hintText)
    love.graphics.setFont(BIGFONT)
    love.graphics.setColor(TEXTSHADOWCOLOR)
    local font = love.graphics.getFont()
    local textWidth = font:getWidth(titleText)
    local textHeight = font:getHeight()
    love.graphics.print(titleText, WINDOWWIDTH/2, WINDOWHEIGHT/2, 0, 1, 1, textWidth/2, textHeight/2)

    love.graphics.setColor(TEXTCOLOR)
    love.graphics.print(titleText, WINDOWWIDTH/2-3, WINDOWHEIGHT/2-3, 0, 1, 1, textWidth/2, textHeight/2)

    love.graphics.setFont(BASICFONT)
    local font = love.graphics.getFont()

    textWidth = font:getWidth(hintText)
    love.graphics.print(hintText, WINDOWWIDTH/2, WINDOWHEIGHT/2+100, 0, 1, 1, textWidth/2, textHeight/2)
end

function setBlankBoard()
    -- return a random new piece in a random rotation and color
    for i = 1, BOARDWIDTH do
        board[i] = {}
        for j = 1, BOARDHEIGHT do
            board[i][j] = BLANK
        end
    end
    return board
end

function calculateLevelAndFallFreq()
    -- Based on the score, return the level the player is on and
    -- how many seconds pass until a falling piece falls one space.
    local level = math.floor(score / 10) + 1
    local fallFreq = 0.27 - (level * 0.02)
    return level, fallFreq
end

function getNewPiece()
    -- return a random new piece in a random rotation and color
    local keys = {"S", "Z", "J", "L", "I", "O", "T"}
    local randomValue = love.math.random(1, #keys)
    local randomKey = keys[randomValue]
    local newPiece = {}
    newPiece.shape = randomKey
    newPiece.rotation = love.math.random(1, #PIECES[newPiece.shape])
    newPiece.x = math.floor(BOARDWIDTH/2) - math.floor(TEMPLATEWIDTH/2)
    newPiece.y = -2 -- start it above the board (i.e. less than 0)
    newPiece.color = love.math.random(1, #COLORS)

    return newPiece
end

function isOnBoard(x, y)
    return x >= 1 and x <= BOARDWIDTH and y <= BOARDHEIGHT
end

function isValidPosition(adjX, adjY)
    -- Return true if the piece is within the board and not colliding
    if fallingPiece == nil then
        return
    end
    local shape = PIECES[fallingPiece.shape][fallingPiece.rotation]
    for x = 1, TEMPLATEWIDTH do
        for y = 1, TEMPLATEHEIGHT do
            local isAboveBoard = y + fallingPiece.y + adjY < 1
            if isAboveBoard or string.sub(shape[y], x, x) == BLANK then
                goto continue
            end
            if not isOnBoard(x + fallingPiece.x + adjX, y + fallingPiece.y + adjY) then
                return false
            end
            if board[x + fallingPiece.x + adjX][y + fallingPiece.y + adjY] ~= BLANK then
                return false

            end
            ::continue::
        end
    end

    return true
end

function printTable(table, name)
    print(name)
    for key, value in pairs(table) do
        print(key, value)
    end
end

function convertToPixelCoords(boxx, boxy)
    -- Convert the given xy coordinates of the board to xy
    -- coordinates of the location on the screen. 
    return (XMARGIN + (boxx * BOXSIZE)), (TOPMARGIN + (boxy * BOXSIZE))
end

function addToBoard() 
    local shape = PIECES[fallingPiece.shape][fallingPiece.rotation]
   -- fill in the board based on piece's location, shape, and rotation
   for x = 1, TEMPLATEWIDTH do
    for y = 1, TEMPLATEHEIGHT do
        if string.sub(shape[y],x,x) ~= BLANK then
            board[x + fallingPiece.x][y + fallingPiece.y] = fallingPiece.color
        end
    end
   end
end

function removeCompleteLines() 
    -- Remove any completed lines on the board, move everything above them down, and return the number of complete lines.
    numLinesRemoved = 0
    y = BOARDHEIGHT -- start y at the bottom of the board
    while y >= 1 do
        if isCompleteLine(board, y) then
            -- Remove the line and pull boxes down by one line.
            for pullDownY = y, 1, -1 do
                for x = 1, BOARDWIDTH do
                    board[x][pullDownY] = board[x][pullDownY-1]
                end
            end
            -- Set very top line to blank.
            for x = 1, BOARDWIDTH do
                board[x][1] = BLANK
            end
            numLinesRemoved = numLinesRemoved + 1
            -- Note on the next iteration of the loop, y is the same.
            -- This is so that if the line that was pulled down is also
            -- complete, it will be removed.
        else
            y = y - 1 -- move on to check next row up
        end
    end

    if numLinesRemoved > 0 then
        sounds.destroyRowSFX:play()
    end
    return numLinesRemoved
end

function isCompleteLine(board, y)
    -- Return true if the line filled with boxes with no gaps.
    for x = 1, BOARDWIDTH do
        if board[x][y] == BLANK then
            return false
        end
    end
    return true
end

function drawBoard()
    -- draw the border around the board
    love.graphics.setColor(BORDERCOLOR)
    love.graphics.rectangle("fill", XMARGIN - 13, TOPMARGIN - 7, 10, (BOARDHEIGHT * BOXSIZE) + 8)
    love.graphics.rectangle("fill", XMARGIN - 3 + (BOARDWIDTH * BOXSIZE) + 8, TOPMARGIN - 7, 10, (BOARDHEIGHT * BOXSIZE) + 8)
    love.graphics.rectangle("fill", XMARGIN - 13, TOPMARGIN - 17, (BOARDWIDTH * BOXSIZE) + 28, 10)
    love.graphics.rectangle("fill", XMARGIN - 13, TOPMARGIN - 5 + (BOARDHEIGHT * BOXSIZE), (BOARDWIDTH * BOXSIZE) + 28, 10)
    
    -- fill the background of the board
    love.graphics.setColor(BGCOLOR)
    love.graphics.rectangle("fill", XMARGIN, TOPMARGIN, BOXSIZE * BOARDWIDTH, BOXSIZE * BOARDHEIGHT)
    -- draw the individual boxes on the board
    for x = 1, BOARDWIDTH do
        for y = 1, BOARDHEIGHT do
            drawBox(x-1, y-1, board[x][y], nil, nil)
        end
    end
    love.graphics.setColor(1, 1, 1)
end

function drawBox(boxx, boxy, color, pixelx, pixely)
--[[ draw a single box (each tetromino piece has four boxes)
     at xy coordinates on the board. Or, if pixelx & pixely
     are specified, draw to the pixel coordinates stored in
     pixelx & pixely (this is used for the "Next" piece).]]
    if color == BLANK then
        return
    end

    if pixelx == nil and pixely == nil then
        pixelx, pixely = convertToPixelCoords(boxx, boxy)
    end

    love.graphics.setColor(COLORS[color])
    love.graphics.rectangle("fill", pixelx + 1, pixely + 1, BOXSIZE - 1 * SCALE, BOXSIZE - 1 * SCALE)

    love.graphics.setColor(LIGHTCOLORS[color])
    love.graphics.rectangle("fill", pixelx + 1, pixely + 1, BOXSIZE - 4 * SCALE, BOXSIZE - 4 * SCALE)
    love.graphics.setColor(1, 1, 1)
end

function drawStatus()
    love.graphics.setFont(BASICFONT)
    love.graphics.setColor(TEXTCOLOR)
    local font = love.graphics.getFont()
    local textWidth = font:getWidth("Score: " .. score)
    local textHeight = font:getHeight()
    love.graphics.print("Score: " .. score, WINDOWWIDTH - 150, 20)

    local textWidth = font:getWidth("Level: " .. level)
    love.graphics.print("Level: " .. level, WINDOWWIDTH - 150, 50)
    love.graphics.setColor(1, 1, 1)
end

function drawNextPiece()
    love.graphics.setColor(1, 1, 1, 0.5)
    love.graphics.rectangle("fill", WINDOWWIDTH - 178, 80, BOXSIZE*TEMPLATEWIDTH, BOXSIZE*TEMPLATEHEIGHT )

    love.graphics.setColor(TEXTCOLOR)
    love.graphics.setFont(BASICFONT)
    
    love.graphics.print("Next: ", WINDOWWIDTH - 120, 80)
    -- draw the "next" piece
    drawPiece(nextPiece, WINDOWWIDTH-120*SCALE, 100)
    love.graphics.setColor(1, 1, 1)
end

function drawPiece(piece, pixelx, pixely)
    local shapeToDraw = PIECES[piece.shape][piece.rotation]
    if pixelx == nil and pixely == nil then
        -- if pixelx & pixely hasn't been specified, use the location stored in the piece data structure
        pixelx, pixely = convertToPixelCoords(piece.x, piece.y)
    end
    -- draw each of the boxes that make up the piece
    for x = 1, TEMPLATEWIDTH do
       for y = 1, TEMPLATEHEIGHT do
        if string.sub(shapeToDraw[y],x,x) ~= BLANK then
            drawBox(nil, nil, piece.color, pixelx + ((x-1) * BOXSIZE), pixely + ((y-1) * BOXSIZE))
        end
       end
    end
end