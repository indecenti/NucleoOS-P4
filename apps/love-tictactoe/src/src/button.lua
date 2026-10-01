local utils = require('src.utils')
function Button(text, color, font, width, height, radius, bg)
    return {
        text = text or 'Button',
        font = font,
        width = width or 100,
        radius = radius or 0,
        height = height or 100,
        bg = bg or { 0, 0, 0 },
        color = color or { 0, 0, 0 },

        bx = 0,
        by = 0,
        tx = 0,
        ty = 0,

        onclick = function(self, callbackFunc, mouseX, mouseY)
            if (mouseX > self.bx and mouseX < self.bx + self.width) and
                (mouseY > self.by and mouseY < self.by + self.height) then
                love.audio.play(utils.sounds.click)
                callbackFunc()
            end
        end,

        draw = function(self, bx, by, tx, ty)
            self.bx = bx or self.bx
            self.by = by or self.by

            if tx then
                self.tx = tx + bx
            end

            if ty then
                self.ty = ty + by
            end

            love.graphics.setColor(bg[1], bg[2], bg[3])
            love.graphics.rectangle('fill', self.bx, self.by, self.width, self.height, self.radius)

            love.graphics.setColor(1, 1, 1)
            love.graphics.print({ color, self.text }, self.font, self.tx, self.ty)

            -- Reset colors
            love.graphics.setColor(1, 1, 1)
        end
    }
end

return Button
