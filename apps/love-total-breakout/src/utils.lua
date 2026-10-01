function UUID()
    local fn = function(x)
        local r = love.math.random(16) - 1
        r = (x == "x") and (r + 1) or (r % 4) + 9
        return ("0123456789abcdef"):sub(r, r)
    end
    return (("xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx"):gsub("[xy]", fn))
end

function Distance(x1, x2, y1, y2)
    return math.sqrt((x1 - x2)*(x1 - x2) + (y1 - y2)*(y1 - y2))
end

function Distance2(x1, x2, y1, y2)
    return (x1 - x2)*(x1 - x2) + (y1 - y2)*(y1 - y2)
end

function Length(x, y)
    return math.sqrt(x*x + y*y)
end

function Length2(x, y)
    return x*x + y*y
end

function Normalize(x, y)
    local l = Length(x, y)
    return x/l, y/l
end

function PrintAll(...)
    local args = { ... }
    for _, v in ipairs(args) do
        print(v)
    end
end

function PrintText(...)
    local args = { ... }
    print(table.concat(args, " "))
end

function Random(min, max)
    if not max then
        return love.math.random(min)
    end

    if min > max then min, max = max, min end
    return love.math.random(min, max)
end

function ResizeWindow(s)
    love.window.setMode(s*Game.w, s*Game.h)
    Game.sX, Game.sY = s, s
end

function SetTimeScale(amount, duration)
    Game.timeScale = amount
    Game.globalTimer:tween("timeScale", duration, Game, { timeScale = 1 }, 'in-out-cubic')
end

function FlashScreen(frames)
    Game.flashFrames = frames
end