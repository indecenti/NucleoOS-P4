return {
    colors = {
        red = { 222 / 255, 43 / 255, 43 / 255 },
        blue = { 43 / 255, 164 / 255, 227 / 255 },
        darkBlue = { 33 / 255, 50 / 255, 92 / 255 },
        black = { 33 / 255, 40 / 255, 48 / 255 },
        white = { 233 / 255, 236 / 255, 242 / 255 }
    },
    fonts = {
        squirk_xl = love.graphics.newFont('assets/fonts/Squirk-RMvV.ttf', 270),
        squirk_l = love.graphics.newFont('assets/fonts/Squirk-RMvV.ttf', 160),
        squirk_m = love.graphics.newFont('assets/fonts/Squirk-RMvV.ttf', 80),
        squirk_s = love.graphics.newFont('assets/fonts/Squirk-RMvV.ttf', 50),
        maldini_bold_l = love.graphics.newFont('assets/fonts/MaldiniBold-OVZO6.ttf', 80),
        maldini_bold_m = love.graphics.newFont('assets/fonts/MaldiniBold-OVZO6.ttf', 60),
        maldini_bold_s = love.graphics.newFont('assets/fonts/MaldiniBold-OVZO6.ttf', 36),
    },
    sounds = {
        play = love.audio.newSource('assets/sound/ball_tap.wav', 'static'),
        win = love.audio.newSource('assets/sound/winner.wav', 'static'),
        tie = love.audio.newSource('assets/sound/draw.wav', 'static'),
        click = love.audio.newSource('assets/sound/click.wav', 'static')
    },

    colorRGB = function(r, g, b)
        return r / 255, g / 255, b / 255
    end
}
