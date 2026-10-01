require "globals"

function love.conf(t)
    t.window.title = "Total Breaköut v" .. Game.version
    t.window.width = Game.w
    t.window.height = Game.h
    t.window.msaa = 0
end