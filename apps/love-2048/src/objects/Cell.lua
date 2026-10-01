Cell = Object:extend()

function Cell:new()
    self.shape = {x = 0, y = 0, w = SCREEN_WIDTH / GRID_SIZE, h = SCREEN_HEIGHT / GRID_SIZE}
    self.target_value = 0
    self.value = EMPTY_CELL_VALUE
    self.text = ''
    self.start_position = {x = 0, y = 0}
    self.end_position = {x = 0, y = 0}
    self.animating = false
end

function Cell:draw()
    love.graphics.setColor(self:get_color())
    love.graphics.rectangle('fill', self.shape.x, self.shape.y, self.shape.w, self.shape.h)
    love.graphics.setColor(0, 0, 0)
    love.graphics.rectangle('line', self.shape.x, self.shape.y, self.shape.w, self.shape.h)
    love.graphics.setColor(self:get_text_color())
    love.graphics.setFont(DEFAULT_FONT)
    local font = love.graphics.getFont()
    local font_width = font:getWidth(self.value)
    local font_height = font:getHeight()
    love.graphics.print(self.value, self.shape.x + self.shape.w / 2, self.shape.y + self.shape.h / 2, 0, 1, 1, font_width / 2, font_height / 2)
end

function Cell:get_color()
    local color_index = math.floor(math.log(self.value) / math.log(2))
    return CELLS_COLORS[color_index]
end

function Cell:get_text_color()
    return (self.value > 4) and CELL_TEXT_COLORS[1] or CELL_TEXT_COLORS[2]
end