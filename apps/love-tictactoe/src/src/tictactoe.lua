local utils = require('src.utils')
local colors = utils.colors

tictactoe = {}

tictactoe.players = {
    MAX = 'X',
    MIN = 'O',
}

-- draw X
function tictactoe.X(x, y)
    local offset = 160

    love.graphics.setColor(colors.red[1], colors.red[2], colors.red[3])
    love.graphics.setLineWidth(37)

    love.graphics.line(x, y, x + offset, y + offset)
    love.graphics.line(x + offset, y, x, y + offset)
    love.graphics.setColor(1, 1, 1)
end

-- draw O
function tictactoe.O(x, y)
    love.graphics.setColor(colors.blue[1], colors.blue[2], colors.blue[3])
    love.graphics.setLineWidth(37)

    love.graphics.circle('line', x, y, 80)
    love.graphics.setColor(1, 1, 1)
end

function tictactoe.crossWin(start, finish)
    love.graphics.setLineWidth(20)
    love.graphics.setColor(utils.colors.black[1], utils.colors.black[2], utils.colors.black[3])

    local currentX = start.x + (finish.x - start.x) * crossLine.progress
    local currentY = start.y + (finish.y - start.y) * crossLine.progress

    love.graphics.line(start.x, start.y, currentX, currentY)
end

return tictactoe
