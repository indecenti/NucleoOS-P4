---@diagnostic disable: cast-local-type
local players = require('src.tictactoe').players

local ai = {}

local MAX = players.MAX
local MIN = players.MIN

function getMoves(board)
    local moves = {}
    for i = 1, 3 do
        for j = 1, 3 do
            if board[i][j] == '' then
                if #moves > 1 then
                    math.randomseed(os.time())
                    local pos = math.random(1, #moves)
                    table.insert(moves, pos, { row = i, col = j })
                else
                    table.insert(moves, { row = i, col = j })
                end
            end
        end
    end
    return moves
end

local function boardState(board)
    local function getValue(row, col)
        if board[row][col] == MAX then
            return 10
        elseif board[row][col] == MIN then
            return -10
        end
    end

    local state = {
        isTerminal = false
    }

    -- Check if there's a winner
    if board[1][1] ~= '' and board[1][1] == board[2][2] and board[2][2] == board[3][3] then
        state.value = getValue(2, 2)
        state.isTerminal = true
        return state
    elseif board[1][3] ~= '' and board[1][3] == board[2][2] and board[2][2] == board[3][1] then
        state.value = getValue(2, 2)
        state.isTerminal = true
        return state
    else
        for i = 1, 3 do
            if board[i][1] ~= '' and board[i][1] == board[i][2] and board[i][2] == board[i][3] then
                state.value = getValue(i, 1)
                state.isTerminal = true
                return state
            elseif board[1][i] ~= '' and board[1][i] == board[2][i] and board[2][i] == board[3][i] then
                state.value = getValue(1, i)
                state.isTerminal = true
                return state
            end
        end
    end

    -- check if tie
    local filledCells = 0
    for _, row in ipairs(board) do
        for _, cell in ipairs(row) do
            if cell ~= '' then
                filledCells = filledCells + 1
            end
        end
    end

    if filledCells == 9 then
        state.value = 0
        state.isTerminal = true
    end
    return state
end

function ai.minimax(board, player, depth)
    local state = boardState(board)
    local bestMove

    if state.isTerminal then
        return state.value
    end

    if player == MAX then
        local value = -math.huge

        for _, move in ipairs(getMoves(board)) do
            board[move.row][move.col] = MAX
            local nextVal = ai.minimax(board, MIN, depth + 1)

            board[move.row][move.col] = ''
            if nextVal > value then
                value = nextVal - depth
                bestMove = move
            end
        end

        if depth > 0 then
            return value
        else
            return bestMove
        end
    elseif player == MIN then
        local value = math.huge

        for _, move in ipairs(getMoves(board)) do
            board[move.row][move.col] = MIN
            local nextVal = ai.minimax(board, MAX, depth + 1)

            board[move.row][move.col] = ''
            if nextVal < value then
                value = nextVal + depth
                bestMove = move
            end
        end

        if depth > 0 then
            return value
        else
            return bestMove
        end
    end
end

function ai.playRandom(board)
    math.randomseed(os.time())
    local moves = getMoves(board)
    local selectMove = math.random(1, #moves)

    return moves[selectMove]
end

return ai
